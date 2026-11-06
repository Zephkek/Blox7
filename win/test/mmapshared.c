// MAP_SHARED like linux: stores reach the file, msync flushes, maps stay coherent. mmapshared [dir]
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char dir[256];

static void check(const char* name, int ok, const char* what) {
  printf("check %-44s %s", name, ok ? "ok" : "FAIL");
  if (!ok) printf("  (%s, errno %d %s)", what, errno, strerror(errno));
  printf("\n");
  if (!ok) failures++;
}
#define CHECK(name, cond) check(name, (cond), #cond)

static const char* at(const char* name) {
  static __thread char p[4][512];
  static __thread int i;
  i = (i + 1) % 4;
  snprintf(p[i], sizeof p[i], "%s/%s", dir, name);
  return p[i];
}

static uint8_t pat(uint64_t off, int seed) { return (uint8_t)(((off * 2654435761u) >> 13) ^ (uint64_t)(seed * 77)); }

static void fill(uint8_t* p, uint64_t off, size_t n, int seed) {
  for (size_t i = 0; i < n; i++) p[i] = pat(off + i, seed);
}

static int same(const uint8_t* p, uint64_t off, size_t n, int seed) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != pat(off + i, seed)) return 0;
  return 1;
}

static int file_has(const char* path, uint64_t off, size_t n, int seed) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  uint8_t* buf = malloc(n);
  ssize_t got = pread(fd, buf, n, (off_t)off);
  int ok = got == (ssize_t)n && same(buf, off, n, seed);
  free(buf);
  close(fd);
  return ok;
}

static int pread_is(int fd, off_t off, const char* text) {
  char buf[64] = {0};
  size_t n = strlen(text);
  return pread(fd, buf, n, off) == (ssize_t)n && memcmp(buf, text, n) == 0;
}

static int make(const char* path, size_t size, int seed) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return -1;
  uint8_t* buf = malloc(size ? size : 1);
  fill(buf, 0, size, seed);
  if (write(fd, buf, size) != (ssize_t)size) {
    free(buf);
    close(fd);
    return -1;
  }
  free(buf);
  return fd;
}

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int write_cache(const char* path, size_t size, int seed) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (fd < 0 || posix_fallocate(fd, 0, (off_t)size) != 0) return 0;
  uint8_t* m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return 0;
  fill(m, 0, size, seed);
  int ok = msync(m, size, MS_SYNC) == 0;
  return munmap(m, size) == 0 && ok;
}

static int writer_index;

static void* writer_thread(void* arg) {
  const int me = __atomic_fetch_add(&writer_index, 1, __ATOMIC_RELAXED);
  char name[64], tmp[64];
  snprintf(name, sizeof name, "thread%d", me);
  snprintf(tmp, sizeof tmp, "thread%d.tmp", me);
  int ok = 1;
  for (int i = 0; i < 100 && ok; i++) {
    const size_t n = 65536 + (size_t)((i * 37 + me * 11) % 50) * 4096 + 123;
    ok = write_cache(at(tmp), n, me * 1000 + i);
    if (!ok) printf("      writer %d: write %d: %s\n", me, i, strerror(errno));
    ok = ok && rename(at(tmp), at(name)) == 0;
    int fd = open(at(name), O_RDONLY);
    uint8_t* m = fd >= 0 ? mmap(NULL, n, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (fd >= 0) close(fd);
    if (ok && m == MAP_FAILED) printf("      writer %d: map %d: %s\n", me, i, strerror(errno));
    ok = ok && m != MAP_FAILED && same(m, 0, n, me * 1000 + i);
    if (ok && msync(m, n, MS_ASYNC) != 0) {
      printf("      writer %d: msync %d of %p+%zu: %s\n", me, i, (void*)m, n, strerror(errno));
      ok = 0;
    }
    if (m != MAP_FAILED) munmap(m, n);
  }
  unlink(at(name));
  *(int*)arg = ok;
  return NULL;
}

static void* churn_thread(void* arg) {
  int ok = 1;
  for (int i = 0; i < 3000 && ok; i++) {
    const size_t n = (size_t)(1 + i % 17) * 4096;
    uint8_t* p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = p != MAP_FAILED;
    if (!ok) printf("      churn: mmap %d: %s\n", i, strerror(errno));
    if (ok) {
      memset(p, i, n);
      ok = madvise(p, n, MADV_DONTNEED) == 0 && p[n - 1] == 0 && munmap(p, n) == 0;
      if (!ok) printf("      churn: madvise or munmap %d: %s\n", i, strerror(errno));
    }
  }
  *(int*)arg = ok;
  return NULL;
}

static void* piecewise_thread(void* arg) {
  const int me = __atomic_fetch_add(&writer_index, 1, __ATOMIC_RELAXED);
  char name[64];
  snprintf(name, sizeof name, "pieces%d", me);
  int ok = 1;
  for (int i = 0; i < 100 && ok; i++) {
    int fd = make(at(name), 16 * 4096, me * 1000 + i);
    uint8_t* m = fd >= 0 ? mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    ok = m != MAP_FAILED;
    for (int k = 0; k < 16 && ok; k++) {
      const int page = (k * 7 + i) % 16;
      ok = same(m + page * 4096, (uint64_t)page * 4096, 4096, me * 1000 + i) && munmap(m + page * 4096, 4096) == 0;
    }
    if (!ok) printf("      pieces %d: round %d: %s\n", me, i, strerror(errno));
    if (fd >= 0) close(fd);
  }
  unlink(at(name));
  *(int*)arg = ok;
  return NULL;
}

static int trash_left(void) {
  DIR* d = opendir("/.deleted");
  if (!d) return -1;
  char prefix[32];
  snprintf(prefix, sizeof prefix, "%d-", getpid());
  int n = 0;
  for (struct dirent* e; (e = readdir(d));)
    if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
  closedir(d);
  return n;
}

static int child(const char* path) {
  int fd = open(path, O_RDWR);
  uint8_t* m = fd >= 0 ? mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
  if (m == MAP_FAILED || ftruncate(fd, 65536) != 0) return 1;
  close(fd);
  memcpy(m + 4096, "at exit", 7);
  exit(0);
}

int main(int argc, char** argv) {
  if (argc > 2 && strcmp(argv[1], "child") == 0) return child(argv[2]);
  setvbuf(stdout, NULL, _IONBF, 0);
  snprintf(dir, sizeof dir, "%s/mmapshared-%d", argc > 1 ? argv[1] : "/tmp", getpid());
  mkdir(dir, 0700);
  printf("mmapshared in %s\n", dir);
  const long page = sysconf(_SC_PAGESIZE);

  {
    const size_t n = 3 * 65536 + 5000, n2 = 200000;
    CHECK("writer: write, msync, munmap", write_cache(at("cache.tmp"), n, 1));
    struct stat st;
    CHECK("writer: file size", stat(at("cache.tmp"), &st) == 0 && st.st_size == (off_t)n);
    CHECK("writer: read() gets the stores", file_has(at("cache.tmp"), 0, n, 1));
    CHECK("writer: resized with O_TRUNC", write_cache(at("cache.tmp"), n2, 2));
    CHECK("writer: read() after the resize", file_has(at("cache.tmp"), 0, n2, 2));
    CHECK("writer: rename into place", rename(at("cache.tmp"), at("cache")) == 0);
    CHECK("writer: renamed file holds the data", file_has(at("cache"), 0, n2, 2));
  }

  {
    const size_t n = 200000;
    int fd = open(at("cache"), O_RDONLY);
    uint8_t* old = mmap(NULL, n, PROT_READ, MAP_SHARED | MAP_NORESERVE, fd, 0);
    close(fd);
    CHECK("reader: read-only shared map", old != MAP_FAILED && same(old, 0, n, 2));
    CHECK("reader: new cache written", write_cache(at("cache2.tmp"), n, 4));
    CHECK("reader: rename over the mapped file", rename(at("cache2.tmp"), at("cache")) == 0);
    CHECK("reader: old map keeps the old file", old != MAP_FAILED && same(old, 0, n, 2));
    fd = open(at("cache"), O_RDONLY);
    uint8_t* fresh = mmap(NULL, n, PROT_READ, MAP_SHARED | MAP_NORESERVE, fd, 0);
    close(fd);
    CHECK("reader: a new map gets the new file", fresh != MAP_FAILED && same(fresh, 0, n, 4));
    if (old != MAP_FAILED) munmap(old, n);
    if (fresh != MAP_FAILED) munmap(fresh, n);
  }

  {
    const size_t n = 256 * 1024;
    int fd = open(at("coh"), O_RDWR | O_CREAT | O_TRUNC, 0666);
    ftruncate(fd, (off_t)n);
    char* a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK("coherence: read-write map", a != MAP_FAILED);
    if (a != MAP_FAILED) {
      memcpy(a + 1000, "through the map", 15);
      CHECK("coherence: pread sees a store at once", pread_is(fd, 1000, "through the map"));
      pwrite(fd, "through pwrite", 14, 70000);
      CHECK("coherence: the map sees pwrite at once", memcmp(a + 70000, "through pwrite", 14) == 0);
      char* b = mmap(NULL, n, PROT_READ, MAP_SHARED, fd, 0);
      CHECK("coherence: second read-only map", b != MAP_FAILED && b != a);
      if (b != MAP_FAILED) {
        CHECK("coherence: second map has both", memcmp(b + 1000, "through the map", 15) == 0 && memcmp(b + 70000, "through pwrite", 14) == 0);
        memcpy(a + 200000, "later", 5);
        CHECK("coherence: second map sees later stores", memcmp(b + 200000, "later", 5) == 0);
        munmap(b, n);
      }
      int ro = open(at("coh"), O_RDONLY);
      char* c = mmap(NULL, 65536, PROT_READ, MAP_SHARED, ro, 65536);
      close(ro);
      CHECK("coherence: map of another fd at 64 KB", c != MAP_FAILED && memcmp(c + (70000 - 65536), "through pwrite", 14) == 0);
      if (c != MAP_FAILED) munmap(c, 65536);
      int fd2 = open(at("coh"), O_RDONLY);
      char buf[16] = {0};
      lseek(fd2, 1000, SEEK_SET);
      CHECK("coherence: read() on a new fd", read(fd2, buf, 15) == 15 && memcmp(buf, "through the map", 15) == 0);
      close(fd2);
      CHECK("coherence: munmap", munmap(a, n) == 0);
    }
    close(fd);
  }

  {
    const size_t n = 256 * 1024;
    int fd = make(at("off"), n, 3);
    uint8_t* p = mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, 4096);
    CHECK("offsets: map at 4096", p != MAP_FAILED && same(p, 4096, 8192, 3));
    uint8_t* q = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 65536 + 4096);
    CHECK("offsets: map at 69632", q != MAP_FAILED && same(q, 65536 + 4096, 12288, 3));
    if (q != MAP_FAILED) {
      memcpy(q + 100, "offset store", 12);
      CHECK("offsets: pread sees the store", pread_is(fd, 65536 + 4096 + 100, "offset store"));
      munmap(q, 12288);
    }
    if (p != MAP_FAILED) munmap(p, 8192);
    close(fd);
  }

  {
    int fd = make(at("unl"), 65536, 5);
    char* m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    struct stat st;
    CHECK("unlink: while mapped", m != MAP_FAILED && unlink(at("unl")) == 0);
    CHECK("unlink: name gone", stat(at("unl"), &st) != 0 && errno == ENOENT);
    if (m != MAP_FAILED) {
      m[10] = 'x';
      CHECK("unlink: map still works", m[10] == 'x' && msync(m, 65536, MS_SYNC) == 0);
      CHECK("unlink: munmap", munmap(m, 65536) == 0);
    }
    const int left = trash_left();
    if (left >= 0) CHECK("unlink: nothing left in /.deleted", left == 0);
  }

  {
    int fd = make(at("acc"), 65536, 6);
    close(fd);
    int ro = open(at("acc"), O_RDONLY), wo = open(at("acc"), O_WRONLY);
    void* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
    CHECK("access: PROT_WRITE on O_RDONLY is EACCES", p == MAP_FAILED && errno == EACCES);
    p = mmap(NULL, 65536, PROT_READ, MAP_SHARED, wo, 0);
    CHECK("access: O_WRONLY is EACCES", p == MAP_FAILED && errno == EACCES);
    p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE, ro, 0);
    CHECK("access: MAP_PRIVATE write on O_RDONLY", p != MAP_FAILED);
    if (p != MAP_FAILED) munmap(p, 65536);
    p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
    CHECK("access: mprotect(PROT_WRITE) on O_RDONLY", p != MAP_FAILED && mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0 && errno == EACCES);
    if (p != MAP_FAILED) munmap(p, 4096);
    close(ro);
    close(wo);
  }

  {
    int fd = make(at("err"), 65536, 7);
    char* m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    CHECK("msync: unaligned is EINVAL", msync(m + 1, 4096, MS_SYNC) != 0 && errno == EINVAL);
    CHECK("msync: MS_SYNC|MS_ASYNC is EINVAL", msync(m, 4096, MS_SYNC | MS_ASYNC) != 0 && errno == EINVAL);
    CHECK("msync: MS_ASYNC", msync(m, 65536, MS_ASYNC) == 0);
    munmap(m, 65536);
    CHECK("msync: unmapped is ENOMEM", msync(m, 4096, MS_SYNC) != 0 && errno == ENOMEM);
  }

  {
    int fd = open(at("grow"), O_RDWR | O_CREAT | O_TRUNC, 0666);
    uint8_t* m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK("grow: map of an empty file", m != MAP_FAILED);
    if (m != MAP_FAILED) {
      ftruncate(fd, 65536);
      fill(m, 0, 65536, 8);
      CHECK("grow: msync writes it", msync(m, 65536, MS_SYNC) == 0 && file_has(at("grow"), 0, 65536, 8));
      pwrite(fd, "pwrite!", 7, 40000);
      memcpy(m + 100, "after msync", 11);
      CHECK("grow: munmap writes it", munmap(m, 65536) == 0 && pread_is(fd, 100, "after msync"));
      CHECK("grow: untouched pages keep pwrite", pread_is(fd, 40000, "pwrite!"));
    }
    close(fd);
  }

  {
    uint8_t* res = mmap(NULL, 256 * 1024, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int fd = make(at("fixed"), 128 * 1024, 9);
    uint8_t* m = mmap(res + 65536, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    CHECK("fixed: map in a reservation", m == res + 65536 && same(m, 0, 128 * 1024, 9));
    if (m == res + 65536) {
      memcpy(m + 5, "fixed", 5);
      CHECK("fixed: msync writes it", msync(m, 128 * 1024, MS_SYNC) == 0 && pread_is(fd, 5, "fixed"));
      pwrite(fd, "pwrite!", 7, 100000);
      memcpy(m + 70000, "unmap", 5);
      CHECK("fixed: munmap writes it", munmap(m, 128 * 1024) == 0 && pread_is(fd, 70000, "unmap"));
      CHECK("fixed: untouched pages keep pwrite", pread_is(fd, 100000, "pwrite!"));
    }
    munmap(res, 256 * 1024);
    close(fd);
  }

  {
    int fd = make(at("part"), 16384, 10);
    uint8_t* m = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK("part: munmap the second page", m != MAP_FAILED && munmap(m + 4096, 4096) == 0);
    if (m != MAP_FAILED) {
      CHECK("part: the rest is there", same(m, 0, 4096, 10) && same(m + 8192, 8192, 8192, 10));
      memcpy(m + 8192 + 7, "part", 4);
      CHECK("part: stores still reach the file", pread_is(fd, 8192 + 7, "part"));
      CHECK("part: msync of the hole is ENOMEM", msync(m + 4096, 4096, MS_SYNC) != 0 && errno == ENOMEM);
      CHECK("part: munmap the rest", munmap(m, 16384) == 0);
    }
    close(fd);
  }

  {
    int fd = make(at("over"), 16384, 11);
    uint8_t* m = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t* a = m == MAP_FAILED ? MAP_FAILED : mmap(m + 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK("over: anonymous MAP_FIXED on page 2", a != MAP_FAILED && a == m + 4096);
    if (a != MAP_FAILED && a == m + 4096) {
      int zero = 1;
      for (int i = 0; i < 4096; i++) zero &= a[i] == 0;
      CHECK("over: page 2 is fresh zeros", zero);
      CHECK("over: the rest keeps the file", same(m, 0, 4096, 11) && same(m + 8192, 8192, 8192, 11));
      memcpy(m + 8192 + 10, "after", 5);
      a[0] = 'Z';
      CHECK("over: msync of the rest", msync(m + 8192, 8192, MS_SYNC) == 0 && pread_is(fd, 8192 + 10, "after"));
      uint8_t b0;
      CHECK("over: page 2 stays out of the file", pread(fd, &b0, 1, 4096) == 1 && b0 == pat(4096, 11));
      CHECK("over: munmap", munmap(m, 16384) == 0);
    }
    close(fd);
  }

  {
    int fd = make(at("remap"), 16384, 12);
    uint8_t* m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t* n = m == MAP_FAILED ? MAP_FAILED : mremap(m, 8192, 16384, MREMAP_MAYMOVE);
    CHECK("mremap: grow", n != MAP_FAILED && same(n, 0, 16384, 12));
    if (n != MAP_FAILED) {
      memcpy(n + 12000, "remapped", 8);
      CHECK("mremap: stores reach the file", msync(n, 16384, MS_SYNC) == 0 && pread_is(fd, 12000, "remapped"));
      CHECK("mremap: shrink", mremap(n, 16384, 4096, 0) == n);
      munmap(n, 4096);
    }
    close(fd);
  }

  {
    int fd = make(at("prot"), 4096, 13);
    char* m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    CHECK("mprotect: read-only map to read-write", m != MAP_FAILED && mprotect(m, 4096, PROT_READ | PROT_WRITE) == 0);
    if (m != MAP_FAILED) {
      m[0] = 'P';
      CHECK("mprotect: the store reaches the file", pread_is(fd, 0, "P"));
      munmap(m, 4096);
    }
    close(fd);
  }

  {
    int fd = make(at("named"), 65536, 14);
    void* m = mmap(NULL, 65536, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    FILE* f = fopen("/proc/self/maps", "r");
    char line[1024], found[1024] = "";
    while (f && fgets(line, sizeof line, f))
      if (strstr(line, "/named")) snprintf(found, sizeof found, "%s", line);
    if (f) fclose(f);
    CHECK("maps: the file is listed", found[0] != 0);
    if (found[0]) printf("      %s", found);
    if (m != MAP_FAILED) munmap(m, 65536);
  }

  {
    int ok = 1;
    for (int i = 0; i < 300 && ok; i++) {
      ok = write_cache(at("many.tmp"), 65536 + (size_t)i * page, i) && rename(at("many.tmp"), at("many")) == 0;
      if (i % 50 == 0) ok = ok && file_has(at("many"), 0, 65536 + (size_t)i * page, i);
    }
    CHECK("many: 300 write-rename cycles", ok);
  }

  {
    pthread_t t[8];
    int ok[8] = {0};
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, i < 4 ? writer_thread : i < 6 ? piecewise_thread : churn_thread, &ok[i]);
    int all = 1;
    for (int i = 0; i < 8; i++) {
      pthread_join(t[i], NULL);
      all &= ok[i];
    }
    CHECK("threads: writers, piecewise munmaps, churn", all);
  }

  {
    char self[512];
    const ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
    self[k > 0 ? k : 0] = 0;
    int fd = open(at("atexit"), O_RDWR | O_CREAT | O_TRUNC, 0666);
    close(fd);
    char* args[] = {"mmapshared", "child", (char*)at("atexit"), NULL};
    pid_t pid = 0;
    int status = -1;
    int ok = k > 0 && posix_spawn(&pid, self, NULL, NULL, args, environ) == 0 && waitpid(pid, &status, 0) == pid;
    CHECK("exit: child exits cleanly", ok && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fd = open(at("atexit"), O_RDONLY);
    CHECK("exit: its stores are in the file", fd >= 0 && pread_is(fd, 4096, "at exit"));
    close(fd);
  }

  {
    const size_t n = 100u << 20;
    double t0 = now_ms();
    int fd = open(at("big"), O_RDWR | O_CREAT | O_TRUNC, 0666);
    int ok = fd >= 0 && posix_fallocate(fd, 0, (off_t)n) == 0;
    uint8_t* m = ok ? mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    double t1 = now_ms();
    if (m != MAP_FAILED) {
      for (size_t i = 0; i < n; i += 4096) memcpy(m + i, &i, sizeof i);
      double t2 = now_ms();
      ok = msync(m, n, MS_SYNC) == 0;
      double t3 = now_ms();
      ok = munmap(m, n) == 0 && ok;
      double t4 = now_ms();
      printf("      100 MB: open+fallocate+mmap %.1f ms, stores %.1f ms, msync %.1f ms, munmap %.1f ms\n", t1 - t0, t2 - t1,
             t3 - t2, t4 - t3);
      int rfd = open(at("big"), O_RDONLY);
      for (size_t i = 0; rfd >= 0 && i < n && ok; i += 4096 * 997) {
        size_t v = 0;
        ok = pread(rfd, &v, sizeof v, (off_t)i) == sizeof v && v == i;
      }
      if (rfd >= 0) close(rfd);
    }
    CHECK("big: 100 MB written through the map", m != MAP_FAILED && ok);
    unlink(at("big"));
  }

  {  // linux allows this, windows refuses it while a view is there: printed, not counted
    int fd = make(at("trunc"), 65536, 15);
    void* m = mmap(NULL, 65536, PROT_READ, MAP_SHARED, fd, 0);
    int r = ftruncate(fd, 4096);
    printf("note  ftruncate below a mapped size: %s\n", r == 0 ? "ok" : strerror(errno));
    if (m != MAP_FAILED) munmap(m, 65536);
    close(fd);
  }

  const char* names[] = {"cache", "coh", "off", "acc", "err", "grow", "fixed", "part", "over", "remap", "prot", "named", "many", "trunc", "atexit"};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) unlink(at(names[i]));
  rmdir(dir);
  printf("mmapshared: %d failure%s\n", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
