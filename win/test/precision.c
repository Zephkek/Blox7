// timed waits and clocks vs linux. run it on a phone for the reference numbers
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int64_t clock_ns(clockid_t c) {
  struct timespec t;
  clock_gettime(c, &t);
  return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int64_t mono(void) { return clock_ns(CLOCK_MONOTONIC); }
static int64_t real(void) { return clock_ns(CLOCK_REALTIME); }
static struct timespec ts_of(int64_t ns) {
  struct timespec t = {ns / 1000000000, ns % 1000000000};
  return t;
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
static void pct(const char* what, int64_t* v, int n) {
  qsort(v, n, sizeof *v, cmp);
  printf("%-40s p10 %7.3f p50 %7.3f p90 %7.3f p99 %7.3f max %7.3f ms\n", what, v[n / 10] / 1e6, v[n / 2] / 1e6,
         v[n * 9 / 10] / 1e6, v[n * 99 / 100] / 1e6, v[n - 1] / 1e6);
}
static long futex(int* addr, int op, int val, const struct timespec* t, int val3) {
  return syscall(SYS_futex, addr, op, val, t, NULL, val3);
}

#define N 2000
static int64_t late[N], late_wall[N];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_mono, cv_real;
static int word;

enum { COND_MONO, COND_REAL, FUTEX_REL, FUTEX_ABS_MONO, FUTEX_ABS_REAL };
static const char* const kKind[] = {"cond_timedwait monotonic", "cond_timedwait realtime", "FUTEX_WAIT relative",
                                    "FUTEX_WAIT_BITSET monotonic", "FUTEX_WAIT_BITSET realtime"};

static void timed_wait(int kind, int64_t timeout, int64_t* own, int64_t* wall) {
  const int64_t m0 = mono(), r0 = real();
  const int rt = kind == COND_REAL || kind == FUTEX_ABS_REAL;
  const int64_t deadline = (rt ? r0 : m0) + timeout;
  struct timespec d = ts_of(deadline);
  switch (kind) {
    case COND_MONO:
    case COND_REAL:
      pthread_mutex_lock(&mu);
      while (pthread_cond_timedwait(kind == COND_MONO ? &cv_mono : &cv_real, &mu, &d) != ETIMEDOUT) {
      }
      pthread_mutex_unlock(&mu);
      break;
    case FUTEX_REL:
      for (int64_t left = timeout; left > 0; left = m0 + timeout - mono()) {
        struct timespec t = ts_of(left);
        futex(&word, FUTEX_WAIT_PRIVATE, 0, &t, 0);
      }
      break;
    default:
      while (futex(&word, FUTEX_WAIT_BITSET_PRIVATE | (rt ? FUTEX_CLOCK_REALTIME : 0), 0, &d, FUTEX_BITSET_MATCH_ANY) != -1 ||
             errno != ETIMEDOUT) {
      }
  }
  const int64_t m1 = mono();
  *wall = m1 - m0 - timeout;
  *own = rt ? real() - deadline : *wall;
}

static void lateness(int kind, int64_t timeout, int n) {
  int64_t spun = 0, start = mono();
  for (int i = 0; i < n; i++) {
    int64_t s = (int64_t)(rand() % 1000) * 1000, a = mono();
    spin(s);
    spun += mono() - a;
    timed_wait(kind, timeout, &late[i], &late_wall[i]);
  }
  printf("phase wait-%d-%lld %lld %lld %lld\n", kind, (long long)(timeout / 1000), (long long)start, (long long)mono(),
         (long long)spun);
  char what[96];
  snprintf(what, sizeof what, "%s %.1f ms late", kKind[kind], timeout / 1e6);
  pct(what, late, n);
  if (kind == COND_REAL || kind == FUTEX_ABS_REAL) {
    snprintf(what, sizeof what, "  ... in monotonic time");
    pct(what, late_wall, n);
  }
}

static struct {
  int64_t timeout, after, signalled;
  int flag, timed, done;
  pthread_cond_t cv;
} g_w;

static void* waiter(void* arg) {
  int n = (int)(intptr_t)arg;
  for (int i = 0; i < n; i++) {
    pthread_mutex_lock(&mu);
    const int64_t deadline = mono() + (g_w.timeout ? g_w.timeout : 50000000);
    struct timespec d = ts_of(deadline);
    g_w.flag = 1;
    int timed_out = 0;
    while (g_w.flag != 2) {
      if (!g_w.timed) {
        pthread_cond_wait(&g_w.cv, &mu);
      } else if (pthread_cond_timedwait(&g_w.cv, &mu, &d) == ETIMEDOUT) {
        timed_out = 1;
        break;
      }
    }
    const int64_t now = mono();
    late[i] = timed_out ? -1 : now - g_w.signalled;
    g_w.flag = 0;
    pthread_mutex_unlock(&mu);
    while (__atomic_load_n(&g_w.flag, __ATOMIC_ACQUIRE) != 3) {
    }
    __atomic_store_n(&g_w.flag, 0, __ATOMIC_RELEASE);
  }
  return NULL;
}

static void wake_latency(const char* what, int timed, int64_t timeout, int64_t after, int n) {
  pthread_t t;
  g_w.timeout = timeout;
  g_w.timed = timed;
  pthread_create(&t, NULL, waiter, (void*)(intptr_t)n);
  int64_t start = mono(), spun = 0;
  for (int i = 0; i < n; i++) {
    int64_t a = mono();
    while (__atomic_load_n(&g_w.flag, __ATOMIC_ACQUIRE) != 1) {
    }
    spin(after + (int64_t)(rand() % 300) * 1000);
    spun += mono() - a;
    pthread_mutex_lock(&mu);
    if (g_w.flag == 1) {
      g_w.flag = 2;
      g_w.signalled = mono();
      pthread_cond_signal(&g_w.cv);
    }
    pthread_mutex_unlock(&mu);
    a = mono();
    for (;;) {
      pthread_mutex_lock(&mu);
      int f = g_w.flag;
      pthread_mutex_unlock(&mu);
      if (f == 0) break;
    }
    spun += mono() - a;
    __atomic_store_n(&g_w.flag, 3, __ATOMIC_RELEASE);
  }
  pthread_join(t, NULL);
  int missed = 0, m = 0;
  for (int i = 0; i < n; i++) {
    if (late[i] < 0) missed++;
    else late[m++] = late[i];
  }
  char label[96];
  snprintf(label, sizeof label, "%s", what);
  for (char* p = label; *p; p++) {
    if (*p == ' ') *p = '-';
  }
  printf("phase wake-%s %lld %lld %lld\n", label, (long long)start, (long long)mono(), (long long)spun);
  snprintf(label, sizeof label, "wake %s", what);
  if (m) pct(label, late, m);
  if (missed) printf("  (%d of %d timed out before the signal)\n", missed, n);
}

static void steps(const char* what, clockid_t c, int64_t window) {
  static int64_t d[200000];
  int nd = 0;
  int64_t calls = 0, back = 0, nsteps = 0, last = clock_ns(c), m0 = mono(), first = last;
  while (mono() - m0 < window) {
    int64_t x = clock_ns(c);
    calls++;
    if (x > last) {
      nsteps++;
      if (nd < 200000) d[nd++] = x - last;
    }
    if (x < last) back++;
    last = x;
  }
  const int64_t wall = mono() - m0;
  qsort(d, nd, sizeof *d, cmp);
  printf("%-26s %8lld calls %7.1f ns each, %8lld steps (min %lld ns, median %lld ns), %lld backwards, ran %.3f of wall\n",
         what, (long long)calls, (double)wall / calls, (long long)nsteps, (long long)(nd ? d[0] : 0),
         (long long)(nd ? d[nd / 2] : 0), (long long)back, (double)(last - first) / wall);
}

static volatile int g_spin_stop;
static void* spinner(void* arg) {
  (void)arg;
  while (!g_spin_stop) {
  }
  return NULL;
}

static int64_t g_last_real;
static int64_t g_cross_back, g_cross_back_max;
static clockid_t g_cross_clock;
static void* cross_reader(void* arg) {
  int64_t until = mono() + (int64_t)(intptr_t)arg;
  while (mono() < until) {
    int64_t seen = __atomic_load_n(&g_last_real, __ATOMIC_ACQUIRE);
    int64_t r = clock_ns(g_cross_clock);
    if (r < seen) {
      __atomic_fetch_add(&g_cross_back, 1, __ATOMIC_RELAXED);
      int64_t by = seen - r, m = __atomic_load_n(&g_cross_back_max, __ATOMIC_RELAXED);
      while (by > m && !__atomic_compare_exchange_n(&g_cross_back_max, &m, by, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      }
    }
    while (r > seen && !__atomic_compare_exchange_n(&g_last_real, &seen, r, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    }
  }
  return NULL;
}

static void clocks(void) {
  struct timespec r;
  const clockid_t ids[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID};
  const char* const names[] = {"realtime", "monotonic", "process cpu", "thread cpu"};
  printf("clock_getres:");
  for (int i = 0; i < 4; i++) {
    clock_getres(ids[i], &r);
    printf(" %s %lld ns%s", names[i], (long long)r.tv_sec * 1000000000 + r.tv_nsec, i < 3 ? "," : "\n");
  }
  steps("CLOCK_MONOTONIC", CLOCK_MONOTONIC, 200000000);
  steps("CLOCK_REALTIME", CLOCK_REALTIME, 200000000);
  steps("CLOCK_THREAD_CPUTIME_ID", CLOCK_THREAD_CPUTIME_ID, 200000000);
  steps("CLOCK_PROCESS_CPUTIME_ID", CLOCK_PROCESS_CPUTIME_ID, 200000000);
  {
    int64_t calls = 0, nd = 0, last = 0, mind = 0, back = 0, m0 = mono();
    struct timeval tv;
    while (mono() - m0 < 200000000) {
      gettimeofday(&tv, NULL);
      int64_t x = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
      calls++;
      if (last && x > last) {
        if (!nd || x - last < mind) mind = x - last;
        nd++;
      }
      if (x < last) back++;
      last = x;
    }
    printf("%-26s %8lld calls %7.1f ns each, %7lld steps (min %lld us), %lld backwards\n", "gettimeofday", (long long)calls,
           (double)(mono() - m0) / calls, (long long)nd, (long long)mind, (long long)back);
  }
  int64_t c0 = clock_ns(CLOCK_THREAD_CPUTIME_ID), m0 = mono();
  struct timespec nap = ts_of(100000000);
  nanosleep(&nap, NULL);
  printf("thread cpu over a 100 ms sleep: %.3f ms (wall %.1f ms)\n", (clock_ns(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e6,
         (mono() - m0) / 1e6);
  pthread_t sp[2];
  g_spin_stop = 0;
  for (int i = 0; i < 2; i++) pthread_create(&sp[i], NULL, spinner, NULL);
  int64_t p0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
  c0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  m0 = mono();
  spin(300000000);
  int64_t p1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID), c1 = clock_ns(CLOCK_THREAD_CPUTIME_ID), m1 = mono();
  g_spin_stop = 1;
  for (int i = 0; i < 2; i++) pthread_join(sp[i], NULL);
  printf("spinning 300 ms beside 2 spinning threads: thread cpu %.3f of wall, process cpu %.3f of wall (3 expected)\n",
         (double)(c1 - c0) / (m1 - m0), (double)(p1 - p0) / (m1 - m0));
  int64_t lo = INT64_MAX, hi = INT64_MIN;
  m0 = mono();
  int64_t r0 = real(), mm0 = m0;
  while (mono() - m0 < 3000000000LL) {
    int64_t a = mono(), x = real(), b = mono();
    int64_t off = x - (a + b) / 2;
    if (off < lo) lo = off;
    if (off > hi) hi = off;
    spin(20000);
  }
  printf("realtime - monotonic over 3 s: spread %.3f ms; realtime ran %.6f of monotonic\n", (hi - lo) / 1e6,
         (double)(real() - r0) / (mono() - mm0));
  for (int k = 0; k < 2; k++) {
    pthread_t rd[4];
    g_cross_clock = k ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    g_last_real = g_cross_back = g_cross_back_max = 0;
    for (int i = 0; i < 4; i++) pthread_create(&rd[i], NULL, cross_reader, (void*)(intptr_t)1000000000);
    for (int i = 0; i < 4; i++) pthread_join(rd[i], NULL);
    printf("%s across 4 threads for 1 s: %lld reads went backwards (max %.3f us)\n", k ? "monotonic" : "realtime",
           (long long)g_cross_back, g_cross_back_max / 1e3);
  }
  printf("time() %lld, realtime %lld\n", (long long)time(NULL), (long long)(real() / 1000000000));
}

static struct {
  pthread_mutex_t mu;
  pthread_cond_t work, done;
  int queued, pending, stop;
  int64_t jobs, frames;
} g_pool = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0};
static pthread_condattr_t g_mono_attr;

static void* pool_worker(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_pool.mu);
  while (!g_pool.stop) {
    if (!g_pool.queued) {
      struct timespec d = ts_of(mono() + 100000000);
      pthread_cond_timedwait(&g_pool.work, &g_pool.mu, &d);
      continue;
    }
    g_pool.queued--;
    pthread_mutex_unlock(&g_pool.mu);
    spin(30000);
    pthread_mutex_lock(&g_pool.mu);
    g_pool.jobs++;
    if (--g_pool.pending == 0) pthread_cond_signal(&g_pool.done);
  }
  pthread_mutex_unlock(&g_pool.mu);
  return NULL;
}

#define TIMER_MAX 20000
static struct Timer {
  int64_t period;
  int n;
  int64_t late[TIMER_MAX];
  pthread_mutex_t mu;
  pthread_cond_t cv;
} g_timers[4];
static volatile int g_load_stop;

static void* timer_thread(void* arg) {
  struct Timer* t = arg;
  pthread_mutex_lock(&t->mu);
  while (!g_load_stop) {
    const int64_t deadline = mono() + t->period;
    struct timespec d = ts_of(deadline);
    while (pthread_cond_timedwait(&t->cv, &t->mu, &d) != ETIMEDOUT) {
    }
    if (t->n < TIMER_MAX) t->late[t->n++] = mono() - deadline;
  }
  pthread_mutex_unlock(&t->mu);
  return NULL;
}

static int64_t g_sleeps;
static void* fmod_thread(void* arg) {
  (void)arg;
  struct timespec t = ts_of(10000000);
  while (!g_load_stop) {
    nanosleep(&t, NULL);
    __atomic_fetch_add(&g_sleeps, 1, __ATOMIC_RELAXED);
  }
  return NULL;
}

static void load(int seconds, int with_short) {
  pthread_condattr_init(&g_mono_attr);
  pthread_condattr_setclock(&g_mono_attr, CLOCK_MONOTONIC);
  pthread_cond_init(&g_pool.work, &g_mono_attr);
  pthread_cond_init(&g_pool.done, &g_mono_attr);
  pthread_t workers[8], timers[4], fmod[3];
  for (int i = 0; i < 8; i++) pthread_create(&workers[i], NULL, pool_worker, NULL);
  const int64_t periods[4] = {5000000, 16000000, 33000000, 1000000};
  const int ntimers = with_short ? 4 : 3;
  for (int i = 0; i < ntimers; i++) {
    g_timers[i].period = periods[i];
    pthread_mutex_init(&g_timers[i].mu, NULL);
    pthread_cond_init(&g_timers[i].cv, &g_mono_attr);
    pthread_create(&timers[i], NULL, timer_thread, &g_timers[i]);
  }
  for (int i = 0; i < 3; i++) pthread_create(&fmod[i], NULL, fmod_thread, NULL);
  pthread_cond_t frame_cv;
  pthread_cond_init(&frame_cv, &g_mono_attr);
  static int64_t frame_late[TIMER_MAX];
  int nf = 0;
  const int64_t period = 1000000000 / 60, start = mono();
  int64_t next = start + period;
  pthread_mutex_lock(&g_pool.mu);
  while (mono() - start < (int64_t)seconds * 1000000000) {
    struct timespec d = ts_of(next);
    while (pthread_cond_timedwait(&frame_cv, &g_pool.mu, &d) != ETIMEDOUT) {
    }
    if (nf < TIMER_MAX) frame_late[nf++] = mono() - next;
    next += period;
    g_pool.queued += 40;
    g_pool.pending += 40;
    for (int j = 0; j < 40; j++) pthread_cond_signal(&g_pool.work);
    while (g_pool.pending) pthread_cond_wait(&g_pool.done, &g_pool.mu);
    g_pool.frames++;
  }
  g_pool.stop = 1;
  pthread_cond_broadcast(&g_pool.work);
  pthread_mutex_unlock(&g_pool.mu);
  g_load_stop = 1;
  const int64_t end = mono();
  for (int i = 0; i < 8; i++) pthread_join(workers[i], NULL);
  for (int i = 0; i < ntimers; i++) pthread_join(timers[i], NULL);
  for (int i = 0; i < 3; i++) pthread_join(fmod[i], NULL);
  printf("phase load %lld %lld 0\n", (long long)start, (long long)end);
  const double s = (end - start) / 1e9;
  printf("load %.1f s: %.0f frames/s, %.0f jobs/s, %.0f sleeps/s, timers", s, g_pool.frames / s, g_pool.jobs / s,
         g_sleeps / s);
  for (int i = 0; i < ntimers; i++) printf(" %.0f ms %.0f/s", g_timers[i].period / 1e6, g_timers[i].n / s);
  printf("\n");
  pct("frame deadline (cond_timedwait) late", frame_late, nf);
  for (int i = 0; i < ntimers; i++) {
    char what[64];
    snprintf(what, sizeof what, "timer %.0f ms late", g_timers[i].period / 1e6);
    pct(what, g_timers[i].late, g_timers[i].n);
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  srand(1234);
  if (argc > 2 && strcmp(argv[1], "load") == 0) {
    load(atoi(argv[2]), argc > 3 && strcmp(argv[3], "short") == 0);
    printf("done\n");
    return 0;
  }
  const int quick = argc > 1 && strcmp(argv[1], "quick") == 0;
  pthread_condattr_t a;
  pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&cv_mono, &a);
  pthread_cond_init(&cv_real, NULL);
  pthread_cond_init(&g_w.cv, &a);
  if (argc > 1 && strcmp(argv[1], "wake") == 0) {
    wake_latency("untimed", 0, 0, 300000, 1000);
    wake_latency("timed 50 ms", 1, 0, 300000, 1000);
    wake_latency("timed 1 ms at 0.6-0.9 ms", 1, 1000000, 600000, 1000);
    printf("done\n");
    return 0;
  }
  if (argc <= 1 || strcmp(argv[1], "clocks") != 0) {
    static const int64_t timeouts[] = {200000, 1000000, 5000000};
    for (int k = 0; k < 5; k++) {
      for (int i = 0; i < 3; i++) {
        int n = timeouts[i] >= 5000000 ? (quick ? 150 : 400) : (quick ? 300 : 1000);
        lateness(k, timeouts[i], n);
      }
    }
    const int n = quick ? 200 : 500;
    wake_latency("untimed", 0, 0, 300000, n);
    wake_latency("timed 50 ms", 1, 0, 300000, n);
    wake_latency("timed 1 ms at 0.6-0.9 ms", 1, 1000000, 600000, n);
  }
  clocks();
  printf("done\n");
  return 0;
}
