// statfs like linux, ext4 and all
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>
#include <unistd.h>

static void show(const char* path) {
  struct statfs s;
  if (statfs(path, &s) != 0) {
    printf("statfs %-22s -> %s\n", path, strerror(errno));
    return;
  }
  printf("statfs %-22s -> type %lx bsize %ld blocks %lu namelen %ld flags %lx\n", path, (unsigned long)s.f_type,
         (long)s.f_bsize, (unsigned long)s.f_blocks, (long)s.f_namelen, (unsigned long)s.f_flags);
}

int main(void) {
  const char* paths[] = {"/proc", "/proc/self", "/proc/self/maps", "/proc/self/exe", "/proc/cpuinfo", "/proc/nope",
                         "/sys", "/sys/devices/system/cpu/online", "/dev", "/dev/null", "/data", "/"};
  for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) show(paths[i]);
  int fd = open("/proc/self/maps", O_RDONLY);
  struct statfs s;
  if (fd >= 0 && fstatfs(fd, &s) == 0) printf("fstatfs /proc/self/maps fd -> type %lx\n", (unsigned long)s.f_type);
  else printf("fstatfs /proc/self/maps fd -> %s\n", strerror(errno));
  return 0;
}
