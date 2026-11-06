// BLOX7_W32_STATS: who sleeps, waits and spins, and which roblox code is behind it

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "w32_kernel.h"

extern "C" void* __w32_attach_thread();

bool g_w32_stats;

namespace {

uint64_t add(uint64_t* counter, uint64_t value) { return __atomic_fetch_add(counter, value, __ATOMIC_RELAXED); }
uint64_t take(uint64_t* counter) { return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED); }
void raise_to(uint64_t* counter, uint64_t value) {
  uint64_t seen = __atomic_load_n(counter, __ATOMIC_RELAXED);
  while (value > seen && !__atomic_compare_exchange_n(counter, &seen, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

uint64_t g_sleeps[6];
uint64_t g_sleep_asked_ns, g_sleep_slept_ns, g_sleep_spun_ns, g_sleep_yields, g_sleep_late_ns, g_sleep_late_max_ns;
uint64_t g_waits[5];
uint64_t g_wait_woken, g_wait_timed_out, g_wait_changed, g_wait_ns, g_wait_late_ns, g_wait_late_max_ns;
uint64_t g_wake_calls, g_wake_found, g_woken;
uint64_t g_yields;
uint64_t g_dontneed, g_dontneed_bytes, g_free, g_free_bytes;

struct Site {
  uintptr_t caller;
  uint64_t sleeps, spun_ns, yields, waits, wait_ns;
};
Site g_sites[1024];
uint64_t g_sites_dropped;

Site* site(uintptr_t caller) {
  if (!caller) return nullptr;
  const size_t mask = sizeof g_sites / sizeof *g_sites - 1;
  for (size_t i = (caller * 0x9e3779b97f4a7c15ull) >> 54 & mask, probes = 0; probes < 32; i = (i + 1) & mask, probes++) {
    uintptr_t seen = __atomic_load_n(&g_sites[i].caller, __ATOMIC_RELAXED);
    if (seen == caller) return &g_sites[i];
    if (seen == 0) {
      if (__atomic_compare_exchange_n(&g_sites[i].caller, &seen, caller, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ||
          seen == caller) {
        return &g_sites[i];
      }
    }
  }
  add(&g_sites_dropped, 1);
  return nullptr;
}

struct Range {
  uintptr_t start, end;
};
Range g_code[256];
int g_code_count;
uint64_t g_code_generation = ~0ull;
volatile int g_code_lock;

void collect(void*, uintptr_t start, uintptr_t, uintptr_t end, uint64_t, const char* path) {
  const char* base = path ? strrchr(path, '/') : nullptr;
  base = base ? base + 1 : (path ? path : "");
  if (strcmp(base, "libc.so") == 0 || strcmp(base, "libc++_shared.so") == 0) return;
  W32_MEMORY_BASIC_INFORMATION m;
  if (g_code_count < 256 && w32.VirtualQuery(reinterpret_cast<void*>(start), &m, sizeof m) && (m.protect & 0xf0)) {
    g_code[g_code_count++] = Range{start, end};
  }
}

bool in_code(uintptr_t v) {
  for (int i = 0; i < g_code_count; i++) {
    if (v >= g_code[i].start && v < g_code[i].end) return true;
  }
  return false;
}

uintptr_t stack_end(uintptr_t sp) {
  W32_MEMORY_BASIC_INFORMATION m;
  if (!w32.VirtualQuery(reinterpret_cast<void*>(sp), &m, sizeof m)) return sp;
  return reinterpret_cast<uintptr_t>(m.base) + m.region_size;
}

const char* name_of(uintptr_t addr, char* out, size_t cap) {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_fname) {
    const char* base = strrchr(info.dli_fname, '/');
    snprintf(out, cap, "%s+0x%lx", base ? base + 1 : info.dli_fname,
             static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(info.dli_fbase)));
  } else {
    snprintf(out, cap, "%p", reinterpret_cast<void*>(addr));
  }
  return out;
}

double ms(uint64_t ns) { return static_cast<double>(ns) / 1e6; }
unsigned long long ull(uint64_t v) { return v; }

void report(unsigned period) {
  uint64_t s[6], w[5];
  for (int i = 0; i < 6; i++) s[i] = take(&g_sleeps[i]);
  for (int i = 0; i < 5; i++) w[i] = take(&g_waits[i]);
  const uint64_t sleeps = s[0] + s[1] + s[2] + s[3] + s[4] + s[5];
  w32_debug("stats: %u s: %llu sleeps (asked <0.1ms %llu, <1ms %llu, <1.5ms %llu, <5ms %llu, <20ms %llu, more %llu): asked "
            "%.1f ms, slept %.1f ms, spun %.1f ms in %llu SwitchToThread, late %.1f ms (max %.2f)",
            period, ull(sleeps), ull(s[0]), ull(s[1]), ull(s[2]),
            ull(s[3]), ull(s[4]), ull(s[5]), ms(take(&g_sleep_asked_ns)),
            ms(take(&g_sleep_slept_ns)), ms(take(&g_sleep_spun_ns)), ull(take(&g_sleep_yields)),
            ms(take(&g_sleep_late_ns)), ms(take(&g_sleep_late_max_ns)));
  w32_debug("stats: %llu futex waits (lasting <10us %llu, <100us %llu, <1ms %llu, <10ms %llu, more %llu): woken %llu, timed "
            "out %llu (late %.1f ms, max %.2f), value changed %llu, %.1f ms waited; %llu wakes, %llu found waiters, %llu woken",
            ull(w[0] + w[1] + w[2] + w[3] + w[4]), ull(w[0]), ull(w[1]),
            ull(w[2]), ull(w[3]), ull(w[4]), ull(take(&g_wait_woken)),
            ull(take(&g_wait_timed_out)), ms(take(&g_wait_late_ns)), ms(take(&g_wait_late_max_ns)),
            ull(take(&g_wait_changed)), ms(take(&g_wait_ns)), ull(take(&g_wake_calls)),
            ull(take(&g_wake_found)), ull(take(&g_woken)));
  w32_debug("stats: %llu sched_yield; madvise DONTNEED %llu (%.1f MB), FREE %llu (%.1f MB)", ull(take(&g_yields)),
            ull(take(&g_dontneed)), static_cast<double>(take(&g_dontneed_bytes)) / 1048576.0,
            ull(take(&g_free)), static_cast<double>(take(&g_free_bytes)) / 1048576.0);
  struct Row {
    uintptr_t caller;
    uint64_t sleeps, spun_ns, yields, waits, wait_ns;
  };
  static Row rows[1024];
  int n = 0;
  for (Site& site : g_sites) {
    const uintptr_t caller = __atomic_load_n(&site.caller, __ATOMIC_RELAXED);
    if (!caller) continue;
    Row r{caller, take(&site.sleeps), take(&site.spun_ns), take(&site.yields), take(&site.waits), take(&site.wait_ns)};
    if (r.sleeps || r.yields || r.waits) rows[n++] = r;
  }
  char name[160];
  for (int pick = 0; pick < 8; pick++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      if ((rows[i].spun_ns || rows[i].yields) &&
          (best < 0 || rows[i].spun_ns + rows[i].yields * 1000 > rows[best].spun_ns + rows[best].yields * 1000)) {
        best = i;
      }
    }
    if (best < 0) break;
    w32_debug("stats: spins %s: %llu sleeps spun %.1f ms, %llu sched_yield", name_of(rows[best].caller, name, sizeof name),
              ull(rows[best].sleeps), ms(rows[best].spun_ns), ull(rows[best].yields));
    rows[best].spun_ns = rows[best].yields = 0;
  }
  for (int pick = 0; pick < 8; pick++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (rows[i].waits && (best < 0 || rows[i].waits > rows[best].waits)) best = i;
    }
    if (best < 0) break;
    w32_debug("stats: waits %s: %llu futex waits, %.1f ms", name_of(rows[best].caller, name, sizeof name),
              ull(rows[best].waits), ms(rows[best].wait_ns));
    rows[best].waits = 0;
  }
  if (const uint64_t dropped = take(&g_sites_dropped)) w32_debug("stats: %llu calls had no room in the caller table", ull(dropped));
}

DWORD WINAPI reporter(void* arg) {
  const unsigned period = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
  w32.Sleep(period * 1000);
  __w32_attach_thread();
  for (;;) {
    report(period);
    w32.Sleep(period * 1000);
  }
}

}  // namespace

void w32_stats_start() {
  const char* value = getenv("BLOX7_W32_STATS");
  if (!value) return;
  unsigned period = static_cast<unsigned>(atoi(value));
  if (period == 0) period = 5;
  g_w32_stats = true;
  w32.CloseHandle(w32.CreateThread(nullptr, 0, reporter, reinterpret_cast<void*>(static_cast<uintptr_t>(period)), 0, nullptr));
}

uintptr_t w32_stats_caller() {
  const uint64_t generation = w32_loader_generation();
  if (__atomic_load_n(&g_code_generation, __ATOMIC_ACQUIRE) != generation) {
    while (__atomic_exchange_n(&g_code_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (g_code_generation != generation) {
      g_code_count = 0;
      w32_loader_segments(collect, nullptr);
      __atomic_store_n(&g_code_generation, generation, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_code_lock, 0, __ATOMIC_RELEASE);
  }
  auto* sp = static_cast<uintptr_t*>(__builtin_frame_address(0));
  const uintptr_t end = stack_end(reinterpret_cast<uintptr_t>(sp));
  for (int i = 0; i < 2048 && reinterpret_cast<uintptr_t>(sp + i + 1) <= end; i++) {
    if (in_code(sp[i])) return sp[i];
  }
  return 0;
}

void w32_stats_sleep(int64_t asked_ns, int64_t slept_ns, int64_t spun_ns, uint32_t yields, uintptr_t caller) {
  const int bucket = asked_ns < 100000 ? 0 : asked_ns < 1000000 ? 1 : asked_ns < 1500000 ? 2 : asked_ns < 5000000 ? 3
                   : asked_ns < 20000000 ? 4 : 5;
  add(&g_sleeps[bucket], 1);
  add(&g_sleep_asked_ns, static_cast<uint64_t>(asked_ns > 0 ? asked_ns : 0));
  add(&g_sleep_slept_ns, static_cast<uint64_t>(slept_ns));
  add(&g_sleep_spun_ns, static_cast<uint64_t>(spun_ns));
  add(&g_sleep_yields, yields);
  if (slept_ns > asked_ns && asked_ns >= 0) {
    add(&g_sleep_late_ns, static_cast<uint64_t>(slept_ns - asked_ns));
    raise_to(&g_sleep_late_max_ns, static_cast<uint64_t>(slept_ns - asked_ns));
  }
  if (Site* s = site(caller)) {
    add(&s->sleeps, 1);
    add(&s->spun_ns, static_cast<uint64_t>(spun_ns));
  }
}

void w32_stats_futex_wait(int64_t waited_ns, int64_t late_ns, long result, uintptr_t caller) {
  const int bucket = waited_ns < 10000 ? 0 : waited_ns < 100000 ? 1 : waited_ns < 1000000 ? 2 : waited_ns < 10000000 ? 3 : 4;
  add(&g_waits[bucket], 1);
  add(&g_wait_ns, static_cast<uint64_t>(waited_ns));
  if (result == 0) {
    add(&g_wait_woken, 1);
  } else if (result == -ETIMEDOUT) {
    add(&g_wait_timed_out, 1);
    if (late_ns > 0) {
      add(&g_wait_late_ns, static_cast<uint64_t>(late_ns));
      raise_to(&g_wait_late_max_ns, static_cast<uint64_t>(late_ns));
    }
  } else {
    add(&g_wait_changed, 1);
  }
  if (Site* s = site(caller)) {
    add(&s->waits, 1);
    add(&s->wait_ns, static_cast<uint64_t>(waited_ns));
  }
}

void w32_stats_futex_wake(long woken) {
  add(&g_wake_calls, 1);
  if (woken > 0) {
    add(&g_wake_found, 1);
    add(&g_woken, static_cast<uint64_t>(woken));
  }
}

void w32_stats_yield(uintptr_t caller) {
  add(&g_yields, 1);
  if (Site* s = site(caller)) add(&s->yields, 1);
}

void w32_stats_madvise(int advice, size_t bytes) {
  if (advice == MADV_DONTNEED || advice == MADV_REMOVE) {
    add(&g_dontneed, 1);
    add(&g_dontneed_bytes, bytes);
  } else if (advice == MADV_FREE) {
    add(&g_free, 1);
    add(&g_free_bytes, bytes);
  }
}
