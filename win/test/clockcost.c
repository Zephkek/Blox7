// how much does clock_gettime cost us vs rdtsc
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void) {
  unsigned a, b, c, d;
  __cpuid(0x80000000, a, b, c, d);
  unsigned invariant = 0;
  if (a >= 0x80000007) {
    __cpuid(0x80000007, a, b, c, d);
    invariant = (d >> 8) & 1;
  }
  printf("invariant TSC: %u\n", invariant);
  __cpuid(0x40000000, a, b, c, d);
  char vendor[13];
  memcpy(vendor, &b, 4);
  memcpy(vendor + 4, &c, 4);
  memcpy(vendor + 8, &d, 4);
  vendor[12] = 0;
  printf("hypervisor leaf 0x40000000: max 0x%x vendor '%s'\n", a, vendor);
  if (a >= 0x40000010) {
    unsigned max = a;
    __cpuid(0x40000010, a, b, c, d);
    printf("hypervisor leaf 0x40000010 (max 0x%x): TSC %u kHz, bus %u kHz\n", max, a, b);
  }
  const int n = 200000;
  int64_t t0 = now_ns();
  int64_t sink = 0;
  for (int i = 0; i < n; i++) sink += now_ns();
  int64_t t1 = now_ns();
  printf("clock_gettime: %.1f ns per call\n", (double)(t1 - t0) / n);
  uint64_t r0 = __rdtsc();
  int64_t s0 = now_ns();
  for (int i = 0; i < n; i++) sink += (int64_t)__rdtsc();
  int64_t s1 = now_ns();
  uint64_t r1 = __rdtsc();
  printf("rdtsc: %.1f ns per read\n", (double)(s1 - s0) / n);
  const int64_t c0 = now_ns();
  const uint64_t k0 = __rdtsc();
  struct timespec two = {2, 0};
  nanosleep(&two, NULL);
  const int64_t c1 = now_ns();
  const uint64_t k1 = __rdtsc();
  printf("TSC rate over %.3f s: %.3f MHz\n", (double)(c1 - c0) / 1e9, (double)(k1 - k0) * 1e3 / (double)(c1 - c0));
  printf("(%lld %llu)\n", (long long)(sink & 1), (unsigned long long)((r1 - r0) & 1));
  return 0;
}
