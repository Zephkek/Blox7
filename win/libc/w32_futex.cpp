// futexes on win7: no WaitOnAddress, so hashed srw locks and condvars like it's 2009

#include <errno.h>
#include <linux/futex.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "w32_kernel.h"
#include "w32_syscalls.h"

extern int64_t g_w32_tail_ns;

namespace {

struct Waiter {
  const volatile int* addr;
  uint32_t bitset;
  bool woken;
  W32_CONDITION_VARIABLE cv;
  HANDLE event;
  Waiter* next;
};

struct Bucket {
  W32_SRWLOCK lock;
  Waiter* head;
};

Bucket g_buckets[1024];

Bucket& bucket_for(const volatile void* addr) {
  uintptr_t a = reinterpret_cast<uintptr_t>(addr) >> 2;
  return g_buckets[(a ^ (a >> 10) ^ (a >> 20)) & 1023];
}

void unlink(Bucket& b, Waiter* w) {
  for (Waiter** p = &b.head; *p; p = &(*p)->next) {
    if (*p == w) {
      *p = w->next;
      return;
    }
  }
}

// SetEvent after unlocking or the boosted waiter just runs face first into our lock
struct Later {
  HANDLE events[16];
  int count = 0;
  void flush() {
    for (int i = 0; i < count; i++) w32.SetEvent(events[i]);
  }
};

void wake(Waiter* w, Later& later) {
  __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
  if (!w->event) {
    w32.WakeConditionVariable(&w->cv);
  } else if (later.count < 16) {
    later.events[later.count++] = w->event;
  } else {
    w32.SetEvent(w->event);
  }
}

struct Slot {
  HANDLE event, timer;
  uint32_t next;
};

constexpr uint32_t kSlots = 1024, kNoSlot = 0xffff;
Slot g_slots[kSlots];
uint32_t g_slots_made;
uint64_t g_free_slots = kNoSlot;

using NtWaitFn = long(WINAPI*)(HANDLE, unsigned char, const int64_t*);
using CreateTimerFn = HANDLE(WINAPI*)(void*, const WCHAR*, DWORD, DWORD);
NtWaitFn g_nt_wait;
CreateTimerFn g_create_hires_timer;
bool g_timers_ready;

void init_timers() {
  void* ntdll = w32_boot->GetModuleHandleW(reinterpret_cast<const uint16_t*>(u"ntdll.dll"));
  auto wait = reinterpret_cast<NtWaitFn>(w32_boot->GetProcAddress(ntdll, "NtWaitForSingleObject"));
  CreateTimerFn hires = nullptr;
  const char* want = getenv("BLOX7_W32_HIRES_TIMER");
  if (!want || strcmp(want, "0") != 0) {
    void* kernel32 = w32_boot->GetModuleHandleW(reinterpret_cast<const uint16_t*>(u"kernel32.dll"));
    auto create = reinterpret_cast<CreateTimerFn>(w32_boot->GetProcAddress(kernel32, "CreateWaitableTimerExW"));
    // 2 = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, only win10 1803+ takes it
    if (HANDLE probe = create ? create(nullptr, nullptr, 2, 0x1f0003) : nullptr) {
      w32.CloseHandle(probe);
      hires = create;
    }
  }
  __atomic_store_n(&g_nt_wait, wait, __ATOMIC_RELAXED);
  __atomic_store_n(&g_create_hires_timer, hires, __ATOMIC_RELAXED);
  __atomic_store_n(&g_timers_ready, true, __ATOMIC_RELEASE);
}

void give_slot(Slot* s) {
  if (!s) return;
  const uint32_t index = static_cast<uint32_t>(s - g_slots);
  uint64_t head = __atomic_load_n(&g_free_slots, __ATOMIC_ACQUIRE);
  do {
    s->next = static_cast<uint32_t>(head & 0xffff);
  } while (!__atomic_compare_exchange_n(&g_free_slots, &head, index | (((head >> 16) + 1) << 16), true, __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE));
}

Slot* take_slot() {
  if (__builtin_expect(!__atomic_load_n(&g_timers_ready, __ATOMIC_ACQUIRE), 0)) init_timers();
  if (!g_nt_wait) return nullptr;
  Slot* s = nullptr;
  uint64_t head = __atomic_load_n(&g_free_slots, __ATOMIC_ACQUIRE);
  while ((head & 0xffff) != kNoSlot) {
    Slot* top = &g_slots[head & 0xffff];
    if (__atomic_compare_exchange_n(&g_free_slots, &head, top->next | (((head >> 16) + 1) << 16), true, __ATOMIC_ACQUIRE,
                                    __ATOMIC_ACQUIRE)) {
      s = top;
      break;
    }
  }
  if (!s) {
    const uint32_t index = __atomic_fetch_add(&g_slots_made, 1, __ATOMIC_RELAXED);
    if (index >= kSlots) return nullptr;
    s = &g_slots[index];
  }
  if (!s->event) {
    s->event = w32.CreateEventW(nullptr, 0, 0, nullptr);
    if (g_create_hires_timer && s->event) s->timer = g_create_hires_timer(nullptr, nullptr, 2, 0x1f0003);
    if (!s->event) {
      give_slot(s);
      return nullptr;
    }
  }
  return s;
}

int64_t clock_now(bool realtime) { return realtime ? w32_realtime_ns() : w32_monotonic_ns(); }

bool word_moved(const Waiter& w, const volatile int* addr, int value) {
  return __atomic_load_n(&w.addr, __ATOMIC_RELAXED) == addr && *addr != value;
}

constexpr int64_t kShortBelowNs = 2000000;
constexpr int64_t kSlackNs = 50000;

long wait_on_slot(Bucket& b, Waiter& w, Slot& s, const volatile int* addr, int value, int64_t deadline, bool realtime) {
  int64_t now = clock_now(realtime);
  const bool short_wait = deadline - now < kShortBelowNs;
  const int64_t tail = short_wait ? g_w32_tail_ns : kSlackNs;
  bool consumed = false;
  long result = 0;
  for (;;) {
    if (w.woken) break;
    const int64_t left = deadline - now;
    if (left <= 0) {
      result = -ETIMEDOUT;
      break;
    }
    w32.ReleaseSRWLockExclusive(&b.lock);
    consumed = false;
    if (left <= tail) {
      while (!__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE) && !word_moved(w, addr, value) && clock_now(realtime) < deadline) {
        w32.SwitchToThread();
      }
    } else if (s.timer) {
      const int64_t due = -((left - (short_wait ? tail : 0) + 99) / 100);
      w32.SetWaitableTimer(s.timer, &due, 0, nullptr, nullptr, 0);
      HANDLE both[2] = {s.event, s.timer};
      consumed = w32.WaitForMultipleObjects(2, both, 0, 0xffffffff) == 0;
      if (consumed) w32.CancelWaitableTimer(s.timer);
    } else {
      const int64_t due = short_wait ? -1 : -((left + 99) / 100);
      consumed = g_nt_wait(s.event, 0, &due) == 0;
    }
    w32.AcquireSRWLockExclusive(&b.lock);
    if (!w.woken && word_moved(w, addr, value)) break;
    now = clock_now(realtime);
  }
  if (w.woken && !consumed) w32.ResetEvent(s.event);
  return result;
}

DWORD wait_ms(int64_t deadline_ns, bool realtime) {
  if (deadline_ns < 0) return 0xffffffff;
  int64_t now = clock_now(realtime);
  if (deadline_ns <= now) return 0;
  int64_t ms = (deadline_ns - now + 999999) / 1000000;
  return ms > 0x7fffffff ? 0x7fffffff : static_cast<DWORD>(ms);
}

long futex_wait_counted(const volatile int* addr, int value, int64_t deadline_ns, bool realtime, uint32_t bitset);

long futex_wait(const volatile int* addr, int value, int64_t deadline_ns, bool realtime, uint32_t bitset) {
  if (__builtin_expect(!g_w32_stats, 1)) {
    const long result = futex_wait_counted(addr, value, deadline_ns, realtime, bitset);
    w32_fs_rearm();
    return result;
  }
  const int64_t begin = w32_monotonic_ns();
  const long result = futex_wait_counted(addr, value, deadline_ns, realtime, bitset);
  const int64_t end = w32_monotonic_ns();
  int64_t late = 0;
  if (result == -ETIMEDOUT && deadline_ns >= 0) late = (realtime ? w32_realtime_ns() : end) - deadline_ns;
  w32_stats_futex_wait(end - begin, late, result, w32_stats_caller());
  w32_fs_rearm();
  return result;
}

long futex_wait_counted(const volatile int* addr, int value, int64_t deadline_ns, bool realtime, uint32_t bitset) {
  if (bitset == 0) return -EINVAL;
  Slot* slot = nullptr;
  if (deadline_ns >= 0) {
    if (deadline_ns <= clock_now(realtime)) return *addr != value ? -EAGAIN : -ETIMEDOUT;
    slot = take_slot();
  }
  Bucket& b = bucket_for(addr);
  Waiter w{addr, bitset, false, {nullptr}, slot ? slot->event : nullptr, nullptr};
  w32.AcquireSRWLockExclusive(&b.lock);
  if (*addr != value) {
    w32.ReleaseSRWLockExclusive(&b.lock);
    give_slot(slot);
    return -EAGAIN;
  }
  w.next = b.head;
  b.head = &w;
  long result = 0;
  if (slot) {
    result = wait_on_slot(b, w, *slot, addr, value, deadline_ns, realtime);
  } else {
    while (!w.woken) {
      DWORD ms = wait_ms(deadline_ns, realtime);
      if (ms == 0 || !w32.SleepConditionVariableSRW(&w.cv, &b.lock, ms, 0)) {
        if (!w.woken && (ms == 0 || w32.GetLastError() == W32_ERROR_TIMEOUT) &&
            wait_ms(deadline_ns, realtime) == 0) {
          result = -ETIMEDOUT;
          break;
        }
      }
    }
  }
  if (!w.woken) unlink(b, &w);
  w32.ReleaseSRWLockExclusive(&b.lock);
  give_slot(slot);
  return w.woken ? 0 : result;
}

long futex_wake(const volatile int* addr, int count, uint32_t bitset) {
  if (bitset == 0) return -EINVAL;
  Bucket& b = bucket_for(addr);
  long woken = 0;
  Later later;
  w32.AcquireSRWLockExclusive(&b.lock);
  for (Waiter** p = &b.head; *p && woken < count;) {
    Waiter* w = *p;
    if (w->addr == addr && (w->bitset & bitset)) {
      *p = w->next;
      wake(w, later);
      woken++;
    } else {
      p = &w->next;
    }
  }
  w32.ReleaseSRWLockExclusive(&b.lock);
  later.flush();
  if (__builtin_expect(g_w32_stats, 0)) w32_stats_futex_wake(woken);
  return woken;
}

long futex_requeue(const volatile int* addr, int wake_count, int requeue, const volatile int* addr2, bool check, int value) {
  Bucket& b1 = bucket_for(addr);
  Bucket& b2 = bucket_for(addr2);
  Bucket* first = &b1 < &b2 ? &b1 : &b2;
  Bucket* second = &b1 < &b2 ? &b2 : &b1;
  Later later;
  w32.AcquireSRWLockExclusive(&first->lock);
  if (second != first) w32.AcquireSRWLockExclusive(&second->lock);
  long result = 0;
  if (check && *addr != value) {
    result = -EAGAIN;
  } else {
    for (Waiter** p = &b1.head; *p;) {
      Waiter* w = *p;
      if (w->addr != addr) {
        p = &w->next;
        continue;
      }
      if (wake_count > 0) {
        *p = w->next;
        wake(w, later);
        wake_count--;
        result++;
      } else if (requeue > 0) {
        *p = w->next;
        __atomic_store_n(&w->addr, addr2, __ATOMIC_RELAXED);
        if (&b1 == &b2) {
          w->next = b2.head;
          b2.head = w;
        } else {
          wake(w, later);
        }
        requeue--;
        result++;
      } else {
        break;
      }
    }
  }
  if (second != first) w32.ReleaseSRWLockExclusive(&second->lock);
  w32.ReleaseSRWLockExclusive(&first->lock);
  later.flush();
  return result;
}

int64_t timespec_ns(const timespec* ts) {
  return static_cast<int64_t>(ts->tv_sec) * 1000000000 + ts->tv_nsec;
}

}  // namespace

bool w32_hires_timers() {
  if (!__atomic_load_n(&g_timers_ready, __ATOMIC_ACQUIRE)) init_timers();
  return g_create_hires_timer != nullptr;
}

bool w32_hires_sleep(int64_t ns) {
  Slot* s = take_slot();
  if (!s || !s->timer) {
    give_slot(s);
    return false;
  }
  const int64_t due = -((ns + 99) / 100);
  w32.SetWaitableTimer(s->timer, &due, 0, nullptr, nullptr, 0);
  w32.WaitForSingleObject(s->timer, 0xffffffff);
  give_slot(s);
  return true;
}

long w32_futex_wake(const volatile int* addr, int count) {
  return futex_wake(addr, count, FUTEX_BITSET_MATCH_ANY);
}

long sys_futex(long uaddr, long op, long val, long timeout, long uaddr2, long val3) {
  auto addr = reinterpret_cast<const volatile int*>(uaddr);
  auto ts = reinterpret_cast<const timespec*>(timeout);
  bool realtime = (op & FUTEX_CLOCK_REALTIME) != 0;
  switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT: {
      int64_t deadline = ts ? w32_monotonic_ns() + timespec_ns(ts) : -1;
      return futex_wait(addr, static_cast<int>(val), deadline, false, FUTEX_BITSET_MATCH_ANY);
    }
    case FUTEX_WAIT_BITSET:
      return futex_wait(addr, static_cast<int>(val), ts ? timespec_ns(ts) : -1, realtime, static_cast<uint32_t>(val3));
    case FUTEX_WAKE:
      return futex_wake(addr, static_cast<int>(val), FUTEX_BITSET_MATCH_ANY);
    case FUTEX_WAKE_BITSET:
      return futex_wake(addr, static_cast<int>(val), static_cast<uint32_t>(val3));
    case FUTEX_REQUEUE:
      return futex_requeue(addr, static_cast<int>(val), static_cast<int>(timeout), reinterpret_cast<const volatile int*>(uaddr2), false, 0);
    case FUTEX_CMP_REQUEUE:
      return futex_requeue(addr, static_cast<int>(val), static_cast<int>(timeout), reinterpret_cast<const volatile int*>(uaddr2), true,
                           static_cast<int>(val3));
    default:
      return -ENOSYS;
  }
}
