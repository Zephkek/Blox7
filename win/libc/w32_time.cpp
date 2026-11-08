// clocks and sleeping on Windows these replace bionic's vdso.cpp


#include <cpuid.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <x86intrin.h>

#include "private/bionic_globals.h"
#include "private/bionic_vdso.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

int64_t g_w32_tail_ns = 200000;
bool w32_hires_timers();
bool w32_hires_sleep(int64_t ns);

namespace {

int64_t g_qpc_frequency;

int64_t qpc_ns() {
  int64_t q;
  w32.QueryPerformanceCounter(&q);
  return (q / g_qpc_frequency) * 1000000000 + (q % g_qpc_frequency) * 1000000000 / g_qpc_frequency;
}


struct TscClock {
  uint64_t tsc0;
  int64_t ns0;
  uint64_t ns_per_tick_32;
};
TscClock g_tsc;
bool g_use_tsc;
bool g_rdtscp;

// ordered rdtsc, a plain one runs ahead and time went backwards by 3 us
uint64_t ordered_tsc() {
  if (g_rdtscp) {
    unsigned aux;
    return __rdtscp(&aux);
  }
  _mm_lfence();
  return __rdtsc();
}

int64_t tsc_ns() {
  const uint64_t ticks = ordered_tsc() - g_tsc.tsc0;
  return g_tsc.ns0 + static_cast<int64_t>((static_cast<unsigned __int128>(ticks) * g_tsc.ns_per_tick_32) >> 32);
}

uint64_t tsc_hz() {
  unsigned a, b, c, d;
  __cpuid(0x80000000, a, b, c, d);
  if (a < 0x80000007) return 0;
  __cpuid(0x80000007, a, b, c, d);
  if (!((d >> 8) & 1)) return 0;
  __cpuid(0, a, b, c, d);
  if (a >= 0x15) {
    __cpuid(0x15, a, b, c, d);
    if (a && b && c) return static_cast<uint64_t>(c) * b / a;
  }
  __cpuid(1, a, b, c, d);
  if ((c >> 31) & 1) {
    __cpuid(0x40000000, a, b, c, d);
    if (a >= 0x40000010) {
      __cpuid(0x40000010, a, b, c, d);
      if (a) return static_cast<uint64_t>(a) * 1000;
    }
  }
  return 0;
}

void init_tsc_clock() {
  const char* choice = getenv("BLOX7_W32_CLOCK");
  if (choice && strcmp(choice, "qpc") == 0) return;
  const uint64_t hz = tsc_hz();
  if (hz < 100000000) return;  // ?
  unsigned a, b, c, d;
  __cpuid(0x80000001, a, b, c, d);
  g_rdtscp = (d >> 27) & 1;
  const uint64_t before = __rdtsc();
  const int64_t ns = qpc_ns();
  const uint64_t after = __rdtsc();
  g_tsc = TscClock{before + (after - before) / 2, ns,
                   static_cast<uint64_t>((static_cast<unsigned __int128>(1000000000) << 32) / hz)};
  g_use_tsc = true;
}

using CyclesFn = BOOL(WINAPI*)(HANDLE, uint64_t*);
CyclesFn g_process_cycles;

int64_t cycles_ns(uint64_t cycles) {
  return static_cast<int64_t>((static_cast<unsigned __int128>(cycles) * g_tsc.ns_per_tick_32) >> 32);
}

int64_t filetime_sum_ns(W32_FILETIME a, W32_FILETIME b) {
  uint64_t x = (static_cast<uint64_t>(a.hi) << 32) | a.lo;
  uint64_t y = (static_cast<uint64_t>(b.hi) << 32) | b.lo;
  return static_cast<int64_t>((x + y) * 100);
}

void to_timespec(int64_t ns, timespec* ts) {
  ts->tv_sec = ns / 1000000000;
  ts->tv_nsec = ns % 1000000000;
}

long gettime(int clock, timespec* ts) {
  W32_FILETIME c, e, k, u;
  uint64_t cycles;
  switch (clock) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_TAI:
      to_timespec(w32_realtime_ns(), ts);
      return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
      to_timespec(w32_monotonic_ns(), ts);
      return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
      if (g_use_tsc && g_process_cycles && g_process_cycles(w32.GetCurrentProcess(), &cycles)) {
        to_timespec(cycles_ns(cycles), ts);
        return 0;
      }
      if (!w32.GetProcessTimes(w32.GetCurrentProcess(), &c, &e, &k, &u)) return w32_fail();
      to_timespec(filetime_sum_ns(k, u), ts);
      return 0;
    case CLOCK_THREAD_CPUTIME_ID:
      if (g_use_tsc && w32.QueryThreadCycleTime(w32.GetCurrentThread(), &cycles)) {
        to_timespec(cycles_ns(cycles), ts);
        return 0;
      }
      if (!w32.GetThreadTimes(w32.GetCurrentThread(), &c, &e, &k, &u)) return w32_fail();
      to_timespec(filetime_sum_ns(k, u), ts);
      return 0;
    default:
      return -EINVAL;
  }
}


constexpr int64_t kShortBelowNs = 2000000;
constexpr int64_t kSlackNs = 50000;

DWORD whole_ms(int64_t ns) {
  const int64_t ms = (ns + 999999) / 1000000;
  return static_cast<DWORD>(ms < 0x7fffffff ? ms : 0x7fffffff);
}

// win7 has no timer finer than its tick, but 100 ns past the last tick is always the next one
void next_tick() {
  const int64_t due = -1;
  w32.NtDelayExecution(0, &due);
}

void sleep_until(int64_t deadline, bool realtime) {
  const bool stats = g_w32_stats;
  int64_t now = realtime ? w32_realtime_ns() : w32_monotonic_ns();
  const int64_t begin = now;
  const bool short_sleep = deadline - begin < kShortBelowNs;
  const bool hires = w32_hires_timers();
  const int64_t tail = short_sleep ? g_w32_tail_ns : hires ? kSlackNs : 0;
  int64_t spin_begin = 0;
  uint32_t yields = 0;
  for (;;) {
    const int64_t left = deadline - now;
    if (left <= 0) break;
    if (left <= tail) {
      if (stats && !spin_begin) spin_begin = now;
      w32.SwitchToThread();
      yields++;
    } else if (!hires || !w32_hires_sleep(short_sleep ? left - tail : left)) {
      if (short_sleep) {
        next_tick();
      } else {
        w32.Sleep(whole_ms(left));
      }
    }
    now = realtime ? w32_realtime_ns() : w32_monotonic_ns();
  }
  if (stats) w32_stats_sleep(deadline - begin, now - begin, spin_begin ? now - spin_begin : 0, yields, w32_stats_caller());
  w32_fs_rearm();
}

using PreciseTimeFn = void(WINAPI*)(W32_FILETIME*);
PreciseTimeFn g_precise_time;

int64_t tick_time_ns() {
  W32_FILETIME ft;
  w32.GetSystemTimeAsFileTime(&ft);
  return w32_filetime_to_ns(ft);
}

// win7 wall clock = monotonic + an anchor that gets nudged toward the tick time
struct Anchor {
  int64_t mono, wall, slow;
};
Anchor g_anchors[2];
uint32_t g_anchor_seq;
int g_anchor_busy;
constexpr int64_t kAheadNs = 4000000, kSetBackNs = 1000000000;

int64_t anchored(const Anchor& a, int64_t mono) {
  const int64_t d = mono - a.mono;
  return a.wall + d - (a.slow ? d >> 9 : 0);
}

void store_anchor(Anchor& to, int64_t mono, int64_t wall, int64_t slow) {
  __atomic_store_n(&to.mono, mono, __ATOMIC_RELAXED);
  __atomic_store_n(&to.wall, wall, __ATOMIC_RELAXED);
  __atomic_store_n(&to.slow, slow, __ATOMIC_RELAXED);
}

void move_anchor(uint32_t seen, int64_t wall, int64_t slow, bool back) {
  int idle = 0;
  if (!__atomic_compare_exchange_n(&g_anchor_busy, &idle, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  if (__atomic_load_n(&g_anchor_seq, __ATOMIC_RELAXED) == seen) {
    __atomic_store_n(&g_anchor_seq, seen + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const int64_t mono = w32_monotonic_ns();
    const int64_t here = anchored(g_anchors[(seen + 1) & 1], mono);
    const int64_t to = back || wall > here ? wall : here;
    store_anchor(g_anchors[seen & 1], mono, to, slow);
    __atomic_store_n(&g_anchor_seq, seen + 2, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    store_anchor(g_anchors[(seen + 1) & 1], mono, to, slow);
  }
  __atomic_store_n(&g_anchor_busy, 0, __ATOMIC_RELEASE);
}

int64_t anchored_realtime_ns() {
  for (bool moved = false;; moved = true) {
    const int64_t tick = tick_time_ns();
    Anchor a;
    int64_t mono;
    uint32_t seq;
    do {
      seq = __atomic_load_n(&g_anchor_seq, __ATOMIC_ACQUIRE);
      const Anchor& from = g_anchors[seq & 1];
      a.mono = __atomic_load_n(&from.mono, __ATOMIC_RELAXED);
      a.wall = __atomic_load_n(&from.wall, __ATOMIC_RELAXED);
      a.slow = __atomic_load_n(&from.slow, __ATOMIC_RELAXED);
      mono = w32_monotonic_ns();
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (__atomic_load_n(&g_anchor_seq, __ATOMIC_RELAXED) != seq);
    const int64_t wall = anchored(a, mono);
    if (moved) return wall;
    if (wall < tick || wall - tick > kSetBackNs) {
      move_anchor(seq, tick, 0, wall > tick);
      continue;
    }
    if (!a.slow && wall - tick > kAheadNs) {
      move_anchor(seq, INT64_MIN, 1, false);
    } else if (a.slow && wall - tick < kAheadNs / 2) {
      move_anchor(seq, INT64_MIN, 0, false);
    }
    return wall;
  }
}

void init_wall_clock() {
  const int64_t first = tick_time_ns(), give_up = w32_monotonic_ns() + 20000000;
  int64_t tick = first;
  while (tick == first && w32_monotonic_ns() < give_up) tick = tick_time_ns();
  move_anchor(__atomic_load_n(&g_anchor_seq, __ATOMIC_RELAXED), tick, 0, true);
}

}//nmspc


void w32_init_timezone() {
  if (getenv("TZ")) return;
  struct SystemTime {
    uint16_t year, month, day_of_week, day, hour, minute, second, milliseconds;
  };
  struct TimeZoneInformation {
    int32_t bias;
    WCHAR standard_name[32];
    SystemTime standard_date;
    int32_t standard_bias;
    WCHAR daylight_name[32];
    SystemTime daylight_date;
    int32_t daylight_bias;
  } zone{};
  if (w32.GetTimeZoneInformation(&zone) == 0xffffffff) return;
  char rule[128];
  size_t n = 0;
  auto add = [&](const char* format, auto... args) {
    int r = snprintf(rule + n, sizeof rule - n, format, args...);
    if (r > 0) n += static_cast<size_t>(r) < sizeof rule - n ? static_cast<size_t>(r) : sizeof rule - n - 1;
  };
  auto add_zone = [&](int32_t minutes_west) {
    const int32_t east = -minutes_west, size = east < 0 ? -east : east;
    add("<%c%02d%02d>", east < 0 ? '-' : '+', size / 60, size % 60);
    const int32_t west = minutes_west < 0 ? -minutes_west : minutes_west;
    add("%s%d", minutes_west < 0 ? "-" : "", west / 60);
    if (west % 60) add(":%02d", west % 60);
  };
  auto add_date = [&](const SystemTime& date) {
    add(",M%u.%u.%u/%u:%02u:%02u", date.month, date.day, date.day_of_week, date.hour, date.minute, date.second);
  };
  add_zone(zone.bias + zone.standard_bias);
  // windows retardation
  if (zone.daylight_date.month != 0 && zone.daylight_date.year == 0 && zone.standard_date.month != 0) {
    add_zone(zone.bias + zone.daylight_bias);
    add_date(zone.daylight_date);
    add_date(zone.standard_date);
  }
  setenv("TZ", rule, 0);
}

void w32_init_time() {
  w32.QueryPerformanceFrequency(&g_qpc_frequency);
  w32.timeBeginPeriod(1);
}

void w32_init_clock_source() {
  init_tsc_clock();
  if (const char* tail = getenv("BLOX7_W32_SLEEP_TAIL_US")) g_w32_tail_ns = static_cast<int64_t>(atoi(tail)) * 1000;
  void* kernel32 = w32_boot->GetModuleHandleW(reinterpret_cast<const uint16_t*>(u"kernel32.dll"));
  g_process_cycles = reinterpret_cast<CyclesFn>(w32_boot->GetProcAddress(kernel32, "QueryProcessCycleTime"));
  g_precise_time = reinterpret_cast<PreciseTimeFn>(w32_boot->GetProcAddress(kernel32, "GetSystemTimePreciseAsFileTime"));
  if (!g_precise_time) init_wall_clock();
}

int64_t w32_monotonic_ns() { return g_use_tsc ? tsc_ns() : qpc_ns(); }

int64_t w32_realtime_ns() {
  if (!g_precise_time) return anchored_realtime_ns();
  W32_FILETIME ft;
  g_precise_time(&ft);
  return w32_filetime_to_ns(ft);
}

void __libc_init_vdso(libc_globals*) {}

int clock_gettime(clockid_t clock, timespec* ts) {
  long r = gettime(clock, ts);
  if (r < 0) {
    errno = static_cast<int>(-r);
    return -1;
  }
  return 0;
}

int clock_getres(clockid_t clock, timespec* ts) {
  if (ts) {
    ts->tv_sec = 0;
    ts->tv_nsec = clock == CLOCK_REALTIME_COARSE ? 1000000 : 1;
  }
  return 0;
}

int gettimeofday(timeval* tv, struct timezone* tz) {
  if (tv) {
    int64_t ns = w32_realtime_ns();
    tv->tv_sec = ns / 1000000000;
    tv->tv_usec = (ns % 1000000000) / 1000;
  }
  if (tz) {
    tz->tz_minuteswest = 0;
    tz->tz_dsttime = 0;
  }
  return 0;
}

time_t time(time_t* t) {
  time_t now = static_cast<time_t>(w32_realtime_ns() / 1000000000);
  if (t) *t = now;
  return now;
}

long sys_clock_gettime(long clock, long ts, long, long, long, long) {
  return gettime(static_cast<int>(clock), reinterpret_cast<timespec*>(ts));
}

long sys_clock_getres(long clock, long ts, long, long, long, long) {
  clock_getres(static_cast<clockid_t>(clock), reinterpret_cast<timespec*>(ts));
  return 0;
}

long sys_gettimeofday(long tv, long tz, long, long, long, long) {
  gettimeofday(reinterpret_cast<timeval*>(tv), reinterpret_cast<struct timezone*>(tz));
  return 0;
}

long sys_clock_settime(long, long, long, long, long, long) { return -EPERM; }
long sys_settimeofday(long, long, long, long, long, long) { return -EPERM; }

long sys_nanosleep(long req, long rem, long, long, long, long) {
  auto ts = reinterpret_cast<const timespec*>(req);
  if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000 || ts->tv_sec < 0) return -EINVAL;
  sleep_until(w32_monotonic_ns() + static_cast<int64_t>(ts->tv_sec) * 1000000000 + ts->tv_nsec, false);
  if (rem) *reinterpret_cast<timespec*>(rem) = timespec{0, 0};
  return 0;
}

long sys_clock_nanosleep(long clock, long flags, long req, long rem, long, long) {
  auto ts = reinterpret_cast<const timespec*>(req);
  if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return -EINVAL;
  bool realtime = clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE || clock == CLOCK_TAI;
  int64_t t = static_cast<int64_t>(ts->tv_sec) * 1000000000 + ts->tv_nsec;
  if (flags & TIMER_ABSTIME) {
    sleep_until(t, realtime);
  } else {
    sleep_until((realtime ? w32_realtime_ns() : w32_monotonic_ns()) + t, realtime);
  }
  if (rem && !(flags & TIMER_ABSTIME)) *reinterpret_cast<timespec*>(rem) = timespec{0, 0};
  return 0;
}

long sys_times(long buf, long, long, long, long, long) {
  W32_FILETIME c, e, k, u;
  if (buf && w32.GetProcessTimes(w32.GetCurrentProcess(), &c, &e, &k, &u)) {
    auto t = reinterpret_cast<tms*>(buf);
    t->tms_utime = static_cast<clock_t>(filetime_sum_ns(u, W32_FILETIME{0, 0}) / 10000000);
    t->tms_stime = static_cast<clock_t>(filetime_sum_ns(k, W32_FILETIME{0, 0}) / 10000000);
    t->tms_cutime = t->tms_cstime = 0;
  }
  return static_cast<long>(w32_monotonic_ns() / 10000000);
}

long sys_getrusage(long who, long usage, long, long, long, long) {
  auto ru = reinterpret_cast<rusage*>(usage);
  *ru = rusage{};
  W32_FILETIME c, e, k, u;
  bool ok = who == RUSAGE_THREAD ? w32.GetThreadTimes(w32.GetCurrentThread(), &c, &e, &k, &u)
                                 : w32.GetProcessTimes(w32.GetCurrentProcess(), &c, &e, &k, &u);
  if (!ok) return w32_fail();
  int64_t user = filetime_sum_ns(u, W32_FILETIME{0, 0}), kernel = filetime_sum_ns(k, W32_FILETIME{0, 0});
  ru->ru_utime = timeval{static_cast<time_t>(user / 1000000000), static_cast<suseconds_t>(user % 1000000000 / 1000)};
  ru->ru_stime = timeval{static_cast<time_t>(kernel / 1000000000), static_cast<suseconds_t>(kernel % 1000000000 / 1000)};
  return 0;
}
