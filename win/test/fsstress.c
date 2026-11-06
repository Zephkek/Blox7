#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define THREADS 16

static long g_total;
static unsigned g_mix;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

__attribute__((noinline)) static unsigned work(unsigned seed) {
  volatile char buf[64];
  unsigned h = 0;
  for (int i = 0; i < 64; i++) buf[i] = (char)(seed + i);
  for (int i = 0; i < 64; i++) h = h * 31 + (unsigned char)buf[i];
  return h;
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void* run(void* arg) {
  (void)arg;
  long calls = 0;
  unsigned h = 0;
  const double end = now() + 3.0;
  while (now() < end) {
    for (int i = 0; i < 1000; i++) h += work(h + (unsigned)i);
    calls += 1000;
    if ((calls & 0x3fff) == 0) usleep(100);
    if ((calls & 0xffff) == 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 200000;
      if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
      }
      pthread_mutex_lock(&g_lock);
      pthread_cond_timedwait(&g_cond, &g_lock, &until);
      pthread_mutex_unlock(&g_lock);
    }
  }
  __atomic_add_fetch(&g_total, calls, __ATOMIC_RELAXED);
  __atomic_xor_fetch(&g_mix, h, __ATOMIC_RELAXED);
  return NULL;
}

int main(void) {
  pthread_t threads[THREADS];
  const double start = now();
  for (int i = 0; i < THREADS; i++) pthread_create(&threads[i], NULL, run, NULL);
  for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
  const double took = now() - start;
  printf("fsstress: %.1f M canary-checked calls/s on %d threads (%.1f s, mix %08x)\n", (double)g_total / took / 1e6,
         THREADS, took, g_mix);
  return 0;
}
