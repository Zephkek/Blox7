// what short sleeps really cost, roblox's luau profiler loop included
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t mono(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int64_t cpu(void) {
  struct timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void nap(int64_t ns) {
  struct timespec t = {ns / 1000000000, ns % 1000000000};
  while (nanosleep(&t, &t) != 0) {
  }
}
static void spin(int64_t ns) {
  int64_t end = mono() + ns;
  while (mono() < end) {
  }
}
static int cmp(const void* a, const void* b) {
  int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
  return x < y ? -1 : x > y;
}

#define N 2000
static int64_t late[N];

static void batch(int64_t ns, int random_phase, int n) {
  int64_t spun = 0, total = 0;
  int64_t c0 = cpu(), start = mono();
  for (int i = 0; i < n; i++) {
    if (random_phase) {
      int64_t s = (int64_t)(rand() % 1000) * 1000, a = mono();
      spin(s);
      spun += mono() - a;
    }
    int64_t t0 = mono();
    nap(ns);
    int64_t d = mono() - t0;
    total += d;
    late[i] = d - ns;
  }
  int64_t end = mono(), used = cpu() - c0 - spun;
  qsort(late, n, sizeof *late, cmp);
  printf("phase sleep-%lld-%s %lld %lld %lld\n", (long long)(ns / 1000), random_phase ? "random" : "aligned",
         (long long)start, (long long)end, (long long)spun);
  printf("sleep %6.3f ms %s: real avg %6.3f ms, late p10 %6.3f p50 %6.3f p90 %6.3f p99 %6.3f max %6.3f ms, tcpu %5.1f%%\n",
         ns / 1e6, random_phase ? "random " : "aligned", total / 1e6 / n, late[n / 10] / 1e6, late[n / 2] / 1e6,
         late[n * 9 / 10] / 1e6, late[n * 99 / 100] / 1e6, late[n - 1] / 1e6, 100.0 * used / (double)total);
}

static void luau_profiler(int seconds) {
  const double period = 1.0 / 1000;
  int64_t start = mono(), c0 = cpu();
  double last = start / 1e9;
  int64_t samples = 0, sleeps = 0;
  while (mono() - start < (int64_t)seconds * 1000000000) {
    double now = mono() / 1e9;
    if (period - (now - last) <= 0) {
      int64_t us = (int64_t)((now - last) * 1e6);
      last += us * 1e-6;
      samples++;
      continue;
    }
    nap(1000000);
    sleeps++;
  }
  int64_t end = mono(), wall = end - start;
  printf("phase luau-profiler %lld %lld 0\n", (long long)start, (long long)end);
  printf("luau profiler loop at 1000 Hz: %.0f samples/s, %.0f sleeps/s, tcpu %.1f%%\n", samples * 1e9 / wall,
         sleeps * 1e9 / wall, 100.0 * (cpu() - c0) / wall);
}

static void frame_loop(int hz, int frames) {
  const int64_t period = 1000000000 / hz;
  int64_t start = mono(), next = start + period, c0 = cpu();
  int n = frames < N ? frames : N;
  for (int i = 0; i < n; i++) {
    struct timespec t = {next / 1000000000, next % 1000000000};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    late[i] = mono() - next;
    next += period;
  }
  int64_t end = mono(), wall = end - start;
  qsort(late, n, sizeof *late, cmp);
  printf("phase frames-%d %lld %lld 0\n", hz, (long long)start, (long long)end);
  printf("frame loop %d Hz (absolute deadlines): late p50 %6.3f p90 %6.3f p99 %6.3f max %6.3f ms, tcpu %.1f%%\n", hz,
         late[n / 2] / 1e6, late[n * 9 / 10] / 1e6, late[n * 99 / 100] / 1e6, late[n - 1] / 1e6,
         100.0 * (cpu() - c0) / wall);
}

int main(int argc, char** argv) {
  srand(1234);
  int quick = argc > 1 && strcmp(argv[1], "quick") == 0;
  static const int64_t lengths_us[] = {10, 50, 100, 200, 300, 500, 700, 900, 1000, 1200, 1500, 1800, 2000, 3000, 5000};
  for (size_t i = 0; i < sizeof lengths_us / sizeof *lengths_us; i++) {
    int64_t ns = lengths_us[i] * 1000;
    int n = ns >= 2000000 ? 300 : (quick ? 600 : 1500);
    batch(ns, 0, n);
    batch(ns, 1, n / 2);
  }
  luau_profiler(4);
  frame_loop(240, 1200);
  frame_loop(144, 720);
  frame_loop(60, 300);
  printf("done\n");
  return 0;
}
