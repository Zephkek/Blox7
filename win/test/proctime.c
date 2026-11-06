// how slow are the /proc files programs poll
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void bench(const char* path, int times) {
  static char buf[1 << 20];
  double start = now_ms();
  size_t bytes = 0;
  for (int i = 0; i < times; i++) {
    int fd = open(path, O_RDONLY);
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) bytes += (size_t)n;
    close(fd);
  }
  double ms = now_ms() - start;
  printf("%-22s %4d reads, %.3f ms each (%zu bytes each)\n", path, times, ms / times, bytes / times);
}

int main(void) {
  bench("/proc/self/statm", 200);
  bench("/proc/self/status", 200);
  bench("/proc/self/stat", 200);
  bench("/proc/self/maps", 20);
  bench("/proc/meminfo", 200);
  return 0;
}
