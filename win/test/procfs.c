// our fake /proc, diff it against a real linux box
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

static void show_file(const char* path, int binary) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    printf("== %s: open: %s\n", path, strerror(errno));
    return;
  }
  static char buf[1 << 20];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof buf && (r = read(fd, buf + n, sizeof buf - n)) > 0) n += (size_t)r;
  close(fd);
  printf("== %s (%zu bytes)\n", path, n);
  if (binary) {
    for (size_t i = 0; i + 16 <= n; i += 16) {
      unsigned long long key, value;
      memcpy(&key, buf + i, 8);
      memcpy(&value, buf + i + 8, 8);
      printf("  %llu = 0x%llx\n", key, value);
    }
  } else {
    fwrite(buf, 1, n, stdout);
    if (n && buf[n - 1] != '\n') printf("\n");
  }
}

static void show_dir(const char* path) {
  DIR* d = opendir(path);
  if (!d) {
    printf("== %s/: opendir: %s\n", path, strerror(errno));
    return;
  }
  printf("== %s/:", path);
  struct dirent* e;
  while ((e = readdir(d)) != NULL) printf(" %s(%d)", e->d_name, e->d_type);
  printf("\n");
  closedir(d);
}

static void show_stat(const char* path) {
  struct stat st, lst;
  struct statfs fs;
  char link[512];
  ssize_t n = readlink(path, link, sizeof link - 1);
  if (n >= 0) link[n] = 0; else snprintf(link, sizeof link, "(%s)", strerror(errno));
  int s = stat(path, &st), l = lstat(path, &lst), f = statfs(path, &fs);
  printf("%-28s stat %s mode %o lstat mode %o  statfs %lx  readlink %s\n", path, s ? strerror(errno) : "ok",
         s ? 0 : st.st_mode, l ? 0 : lst.st_mode, f ? 0 : (unsigned long)fs.f_type, link);
}

int main(void) {
  int fd = open("/proc/self/exe", O_RDONLY);
  void* view = fd >= 0 ? mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
  printf("exe fd %d view %p\n", fd, view);
  const char* files[] = {"/proc/self/stat", "/proc/self/status", "/proc/self/statm", "/proc/self/cmdline",
                         "/proc/self/comm", "/proc/self/mounts", "/proc/mounts", "/proc/self/mountinfo",
                         "/proc/self/limits", "/proc/self/cgroup", "/proc/self/oom_score", "/proc/sys/vm/overcommit_memory",
                         "/proc/net/unix", "/proc/meminfo", "/proc/self/maps"};
  for (unsigned i = 0; i < sizeof files / sizeof *files; i++) show_file(files[i], 0);
  show_file("/proc/self/auxv", 1);
  char pid_path[64];
  snprintf(pid_path, sizeof pid_path, "/proc/%d/cmdline", getpid());
  show_file(pid_path, 0);
  show_file("/proc/thread-self/comm", 0);
  show_file("/proc/cpuinfo", 0);
  show_dir("/proc");
  show_dir("/proc/self");
  show_dir("/proc/self/fd");
  show_dir("/sys/devices/system/cpu");
  const char* nodes[] = {"/proc", "/proc/self", "/proc/self/exe", "/proc/self/fd/0", "/proc/self/maps",
                         "/proc/self/cwd", "/proc/nope", "/sys", "/dev/null", "/"};
  for (unsigned i = 0; i < sizeof nodes / sizeof *nodes; i++) show_stat(nodes[i]);
  return 0;
}
