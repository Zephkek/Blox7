// anon vma names and thread names, diff it against linux
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

static long name_vma(void* p, size_t n, const char* name) {
  long r = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)p, n, (unsigned long)name);
  return r == 0 ? 0 : -errno;
}

static void maps_name(void* p, char* out, size_t cap) {
  out[0] = 0;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long s, e;
    if (sscanf(line, "%lx-%lx", &s, &e) != 2 || (unsigned long)p < s || (unsigned long)p >= e) continue;
    char* name = strchr(line, '/') ? strchr(line, '/') : strchr(line, '[');
    if (name) {
      name[strcspn(name, "\n")] = 0;
      snprintf(out, cap, "%s", name);
    }
    break;
  }
  fclose(f);
}

static void* thread_main(void* arg) {
  char name[16];
  prctl(PR_GET_NAME, name);
  printf("thread inherits name: %s\n", name);
  prctl(PR_SET_NAME, "worker-thread-long-name");
  prctl(PR_GET_NAME, name);
  printf("thread sets name: %s\n", name);
  (void)arg;
  return NULL;
}

int main(void) {
  const size_t page = 4096;
  char seen[256];
  char* p = mmap(NULL, 8 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  printf("name whole: %ld\n", name_vma(p, 8 * page, "mimalloc"));
  maps_name(p, seen, sizeof seen);
  printf("maps first page: %s\n", seen);
  printf("name middle: %ld\n", name_vma(p + 2 * page, 2 * page, "other"));
  maps_name(p + 2 * page, seen, sizeof seen);
  printf("maps renamed page: %s\n", seen);
  maps_name(p + 5 * page, seen, sizeof seen);
  printf("maps later page: %s\n", seen);
  mprotect(p, 8 * page, PROT_READ);
  maps_name(p, seen, sizeof seen);
  printf("after mprotect: %s\n", seen);
  printf("clear: %ld\n", name_vma(p, page, NULL));
  maps_name(p, seen, sizeof seen);
  printf("after clear: '%s'\n", seen);
  printf("unaligned: %ld\n", name_vma(p + 1, page, "x"));
  printf("bad char: %ld\n", name_vma(p, page, "a[b"));
  char longname[100];
  memset(longname, 'a', sizeof longname - 1);
  longname[sizeof longname - 1] = 0;
  printf("too long: %ld\n", name_vma(p, page, longname));
  longname[79] = 0;
  printf("79 bytes: %ld\n", name_vma(p, page, longname));
  munmap(p + 4 * page, 4 * page);
  printf("partly unmapped: %ld\n", name_vma(p, 8 * page, "again"));
  munmap(p, 4 * page);
  char* q = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  maps_name(q, seen, sizeof seen);
  printf("fresh mapping: '%s'\n", seen);
  munmap(q, 2 * page);

  char name[16];
  prctl(PR_SET_NAME, "main-renamed");
  prctl(PR_GET_NAME, name);
  printf("main sets name: %s\n", name);
  pthread_t t;
  pthread_create(&t, NULL, thread_main, NULL);
  pthread_join(t, NULL);
  prctl(PR_GET_NAME, name);
  printf("main keeps name: %s\n", name);
  FILE* f = fopen("/proc/self/comm", "r");
  if (f && fgets(seen, sizeof seen, f)) printf("/proc/self/comm: %s", seen);
  if (f) fclose(f);
  return 0;
}
