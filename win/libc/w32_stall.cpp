// stall dumps. keep the threshold in seconds, at 80 ms it froze the game for a minute

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "w32_exception.h"
#include "w32_kernel.h"

extern "C" void* __w32_attach_thread();

namespace {

int64_t g_frames_gap_ns;
struct FrameWindow {
  int64_t first_ns, start_ns, last_ns, longest_ns;
  uint32_t frames, over25, over50, over100;
};
FrameWindow g_frames;

void note_frame() {
  const int64_t now = w32_monotonic_ns();
  FrameWindow& f = g_frames;
  if (f.first_ns == 0) {
    f.first_ns = f.start_ns = f.last_ns = now;
    return;
  }
  const int64_t gap = now - f.last_ns;
  f.last_ns = now;
  f.frames++;
  if (gap > 25000000) f.over25++;
  if (gap > 50000000) f.over50++;
  if (gap > 100000000) f.over100++;
  if (gap > f.longest_ns) f.longest_ns = gap;
  if (gap > g_frames_gap_ns) {
    w32_debug("frames: t=%.3f gap %.1f ms", static_cast<double>(now - f.first_ns) / 1e9, static_cast<double>(gap) / 1e6);
  }
  if (now - f.start_ns >= 5000000000) {
    const double seconds = static_cast<double>(now - f.start_ns) / 1e9;
    w32_debug("frames: t=%.1f %u in %.1f s (%.1f per second), gaps over 25/50/100 ms: %u/%u/%u, longest %.1f ms",
              static_cast<double>(now - f.first_ns) / 1e9, f.frames, seconds, f.frames / seconds, f.over25, f.over50,
              f.over100, static_cast<double>(f.longest_ns) / 1e6);
    f.start_ns = now;
    f.frames = f.over25 = f.over50 = f.over100 = 0;
    f.longest_ns = 0;
  }
}

bool g_stall_on;
int64_t g_stall_ns;
DWORD g_beat_thread;
int64_t g_beat_ns;
uint64_t g_beat_count;

constexpr uint32_t kContextControl = 0x00100001;
constexpr int kMaxLooks = 3;

constexpr size_t kStackWords = 8192;
uintptr_t g_stack[kStackWords];

struct Range {
  uintptr_t start, end;
};
Range g_elf_code[256];
int g_elf_count;

void collect(void*, uintptr_t start, uintptr_t, uintptr_t end, uint64_t, const char*) {
  W32_MEMORY_BASIC_INFORMATION m;
  if (g_elf_count < 256 && w32.VirtualQuery(reinterpret_cast<void*>(start), &m, sizeof m) && (m.protect & 0xf0)) {
    g_elf_code[g_elf_count++] = Range{start, end};
  }
}

bool is_code(uintptr_t v) {
  if (v < 0x10000) return false;
  for (int i = 0; i < g_elf_count; i++) {
    if (v >= g_elf_code[i].start && v < g_elf_code[i].end) return true;
  }
  W32_MEMORY_BASIC_INFORMATION m;
  return w32.VirtualQuery(reinterpret_cast<void*>(v), &m, sizeof m) && m.type == 0x1000000 /* MEM_IMAGE */ &&
         (m.protect & 0xf0);
}

void name_of(uintptr_t addr, char* out, size_t cap) {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_fname) {
    const char* base = strrchr(info.dli_fname, '/');
    snprintf(out, cap, "%s+0x%lx", base ? base + 1 : info.dli_fname,
             static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(info.dli_fbase)));
    return;
  }
  HANDLE module = nullptr;
  if (w32.GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, reinterpret_cast<void*>(addr), &module)) {
    WCHAR wide[260];
    char name[260] = "?";
    if (w32.GetModuleFileNameW(module, wide, 260)) w32_utf16_to_utf8(wide, name, sizeof name);
    const char* base = strrchr(name, '\\');
    snprintf(out, cap, "%s+0x%lx", base ? base + 1 : name, static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(module)));
    return;
  }
  snprintf(out, cap, "%p", reinterpret_cast<void*>(addr));
}

using LookupFn = void*(WINAPI*)(uint64_t, uint64_t*, void*);
using UnwindFn = void*(WINAPI*)(DWORD, uint64_t, uint64_t, void*, void*, void**, uint64_t*, void*);
LookupFn g_lookup;
UnwindFn g_unwind;

void find_unwinder() {
  void* ntdll = w32_boot->GetModuleHandleW(reinterpret_cast<const uint16_t*>(u"ntdll.dll"));
  if (!ntdll) return;
  g_unwind = reinterpret_cast<UnwindFn>(w32_boot->GetProcAddress(ntdll, "RtlVirtualUnwind"));
  if (g_unwind) g_lookup = reinterpret_cast<LookupFn>(w32_boot->GetProcAddress(ntdll, "RtlLookupFunctionEntry"));
}

bool in_elf(uintptr_t v) {
  for (int i = 0; i < g_elf_count; i++) {
    if (v >= g_elf_code[i].start && v < g_elf_code[i].end) return true;
  }
  return false;
}

int unwind(WinContext* c, uintptr_t lo, uintptr_t hi, uintptr_t* pcs, int max) {
  int n = 0;
  while (n < max && c->rip >= 0x10000 && c->rsp >= lo && c->rsp < hi) {
    pcs[n++] = c->rip;
    uint64_t image = 0;
    void* entry = in_elf(c->rip) || !g_lookup ? nullptr : g_lookup(c->rip, &image, nullptr);
    const uintptr_t sp = c->rsp;
    if (entry) {
      void* data = nullptr;
      uint64_t frame = 0;
      g_unwind(0, image, c->rip, entry, c, &data, &frame, nullptr);
    } else if (in_elf(c->rip)) {
      const uintptr_t bp = c->rbp;
      if (bp < sp || bp + 16 > hi || (bp & 7)) break;
      c->rip = reinterpret_cast<const uintptr_t*>(bp)[1];
      c->rbp = reinterpret_cast<const uintptr_t*>(bp)[0];
      c->rsp = bp + 16;
    } else if (is_code(c->rip)) {
      c->rip = *reinterpret_cast<const uintptr_t*>(sp);
      c->rsp = sp + 8;
    } else {
      break;
    }
    if (c->rsp <= sp) break;
  }
  return n;
}

int sample(DWORD tid, char* out, size_t cap, int frames) {
  out[0] = 0;
  HANDLE h = w32.OpenThread(0x0002 | 0x0008 | 0x0040, 0, tid);
  if (!h) return 0;
  alignas(16) static uint8_t buffer[1536];
  memset(buffer, 0, sizeof buffer);
  auto* context = reinterpret_cast<WinContext*>(buffer);
  context->context_flags = kContextControl | 0x00100002;
  size_t words = 0;
  uintptr_t rip = 0;
  static uintptr_t pcs[64];
  int exact = 0;
  if (frames > 64) frames = 64;
  if (w32.SuspendThread(h) != static_cast<DWORD>(-1)) {
    if (w32.GetThreadContext(h, context)) {
      rip = context->rip;
      const uintptr_t rsp = context->rsp;
      W32_MEMORY_BASIC_INFORMATION m;
      if (rsp && w32.VirtualQuery(reinterpret_cast<void*>(rsp), &m, sizeof m)) {
        const uintptr_t end = reinterpret_cast<uintptr_t>(m.base) + m.region_size;
        words = (end - rsp) / sizeof(uintptr_t);
        if (words > kStackWords) words = kStackWords;
        memcpy(g_stack, reinterpret_cast<const void*>(rsp), words * sizeof(uintptr_t));
        exact = unwind(context, rsp, end, pcs, frames);
      }
    }
    w32.ResumeThread(h);
  }
  w32.CloseHandle(h);
  if (!rip) return 0;
  char name[160];
  name_of(rip, name, sizeof name);
  int used = snprintf(out, cap, "%s; stack", name);
  for (int i = 1; i < exact && used < static_cast<int>(cap) - 80; i++) {
    name_of(pcs[i], name, sizeof name);
    used += snprintf(out + used, cap - static_cast<size_t>(used), " %s", name);
  }
  if (exact >= 8) return used;
  used += snprintf(out + used, cap - static_cast<size_t>(used), " ~");
  int found = 0;
  for (size_t i = 0; i < words && found < frames && used < static_cast<int>(cap) - 80; i++) {
    if (!is_code(g_stack[i])) continue;
    name_of(g_stack[i], name, sizeof name);
    used += snprintf(out + used, cap - static_cast<size_t>(used), " %s", name);
    found++;
  }
  return used;
}

void look(DWORD tid, int64_t stalled_ns, int round) {
  static char where[1900];
  if (!sample(tid, where, sizeof where, 40)) return;
  w32_debug("stall: thread %lu without a frame for %.0f ms (look %d): at %s", static_cast<unsigned long>(tid),
            static_cast<double>(stalled_ns) / 1e6, round, where);
}

DWORD WINAPI watchdog(void*) {
  w32.Sleep(2000);
  __w32_attach_thread();
  find_unwinder();
  w32.timeBeginPeriod(1);
  w32_loader_segments(collect, nullptr);
  uint64_t generation = w32_loader_generation();
  uint64_t stalled_beat = ~0ull;
  int round = 0;
  for (;;) {
    w32.Sleep(5);
    const uint64_t beats = __atomic_load_n(&g_beat_count, __ATOMIC_ACQUIRE);
    if (beats == 0) continue;
    const int64_t since = w32_monotonic_ns() - __atomic_load_n(&g_beat_ns, __ATOMIC_RELAXED);
    if (beats != stalled_beat) round = 0;
    if (round >= kMaxLooks || since < g_stall_ns * (round + 1)) continue;
    if (w32_loader_generation() != generation) {
      g_elf_count = 0;
      w32_loader_segments(collect, nullptr);
      generation = w32_loader_generation();
    }
    stalled_beat = beats;
    round++;
    look(__atomic_load_n(&g_beat_thread, __ATOMIC_RELAXED), since, round);
  }
}

int64_t g_report_ns = 400000000;
int g_reports;

uint64_t filetime(W32_FILETIME t) { return static_cast<uint64_t>(t.hi) << 32 | t.lo; }

uint64_t thread_cpu(DWORD id) {
  HANDLE h = w32.OpenThread(0x0800 /* QUERY_LIMITED_INFORMATION */, 0, id);
  if (!h) return 0;
  W32_FILETIME created, exited, kernel{}, user{};
  uint64_t t = 0;
  if (w32.GetThreadTimes(h, &created, &exited, &kernel, &user)) t = filetime(kernel) + filetime(user);
  w32.CloseHandle(h);
  return t;
}

struct CpuMark {
  DWORD id;
  uint64_t cpu, spent;
};
CpuMark g_marks[512];
int g_mark_count;

void mark_threads() {
  g_mark_count = 0;
  HANDLE snapshot = w32.CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
  DWORD entry[7] = {sizeof entry};  // THREADENTRY32
  const DWORD pid = w32.GetCurrentProcessId(), self = w32.GetCurrentThreadId();
  for (BOOL more = w32.Thread32First(snapshot, entry); more && g_mark_count < 512;
       more = w32.Thread32Next(snapshot, entry)) {
    if (entry[3] != pid || entry[2] == self) continue;
    g_marks[g_mark_count++] = CpuMark{entry[2], thread_cpu(entry[2]), 0};
  }
  w32.CloseHandle(snapshot);
}

struct Busy {
  DWORD id;
  uint64_t cpu;
};

int busiest(Busy* top, int count) {
  for (int k = 0; k < count; k++) top[k] = Busy{};
  for (int i = 0; i < g_mark_count; i++) {
    const uint64_t now_cpu = thread_cpu(g_marks[i].id);
    if (now_cpu > g_marks[i].cpu && now_cpu - g_marks[i].cpu > g_marks[i].spent) g_marks[i].spent = now_cpu - g_marks[i].cpu;
    const uint64_t spent = g_marks[i].spent;
    for (int k = 0; k < count; k++) {
      if (spent <= top[k].cpu) continue;
      for (int j = count - 1; j > k; j--) top[j] = top[j - 1];
      top[k] = Busy{g_marks[i].id, spent};
      break;
    }
  }
  int n = 0;
  while (n < count && top[n].cpu >= 100000 /* 10 ms */) n++;
  return n;
}

int sample_waiting(DWORD present, DWORD main_thread, char* out, size_t cap) {
  int used = 0, sampled = 0;
  out[0] = 0;
  static char stack[1400];
  for (int i = 0; i < g_mark_count && sampled < 8 && used < static_cast<int>(cap) - 1600; i++) {
    const DWORD id = g_marks[i].id;
    if (id == present || id == main_thread) continue;
    const char* name = w32_thread_name_of(id);
    if (!strstr(name, "RBX") && strcmp(name, "Main") != 0) continue;
    if (!sample(id, stack, sizeof stack, 20)) continue;
    used += snprintf(out + used, cap - static_cast<size_t>(used), "%s%s/%lu at %s", sampled ? " | " : "", name,
                     static_cast<unsigned long>(id), stack);
    sampled++;
  }
  return used;
}

void say(const char* text) {
  write(2, text, strlen(text));
  if (w32_log_wanted()) w32_debug("%s", text);
}

DWORD WINAPI reporter(void*) {
  __w32_attach_thread();
  find_unwinder();
  w32_loader_segments(collect, nullptr);
  uint64_t generation = w32_loader_generation();
  struct Tick {
    int64_t ns;
    uint64_t beats;
  };
  constexpr int kTicks = 128;  // 25 ms apart, so a bit over 3 s back
  static Tick ticks[kTicks];
  int head = 0;
  enum { kIdle, kArmed, kSampled } phase = kIdle;
  uint64_t stall_beats = 0;
  int64_t stall_from = 0, armed_at = 0;
  static char present_at[2200], busy_at[1600], main_at[1600], waiting[12000], busy[400], line[18000];
  for (;;) {
    w32.Sleep(25);
    const int64_t now = w32_monotonic_ns();
    const uint64_t beats = __atomic_load_n(&g_beat_count, __ATOMIC_ACQUIRE);
    const int64_t last = __atomic_load_n(&g_beat_ns, __ATOMIC_RELAXED);
    ticks[head] = Tick{now, beats};
    head = (head + 1) % kTicks;
    if (beats == 0 || g_reports >= 200) continue;
    if (phase != kIdle && beats != stall_beats) {
      if (phase == kSampled) {
        Busy top[4];
        const int n = busiest(top, 4);
        int used = 0;
        busy[0] = 0;
        for (int k = 0; k < n; k++) {
          const char* name = w32_thread_name_of(top[k].id);
          used += snprintf(busy + used, sizeof busy - static_cast<size_t>(used), "%s%s/%lu %.0f ms", k ? ", " : "",
                           name[0] ? name : "?", static_cast<unsigned long>(top[k].id),
                           static_cast<double>(top[k].cpu) / 1e4);
        }
        W32_FILETIME wall;
        w32.GetSystemTimeAsFileTime(&wall);
        const uint64_t ms = filetime(wall) / 10000 - static_cast<uint64_t>(w32_monotonic_ns() - stall_from) / 1000000;
        const DWORD present = __atomic_load_n(&g_beat_thread, __ATOMIC_RELAXED);
        const char* present_name = w32_thread_name_of(present);
        snprintf(line, sizeof line,
                 "  [stall] %02u:%02u:%02u.%03uZ no frame for %.0f ms; cpu since it began: %s; presenting thread "
                 "%s/%lu at %s%s%s%s%s%s%s\n",
                 static_cast<unsigned>(ms / 3600000 % 24), static_cast<unsigned>(ms / 60000 % 60),
                 static_cast<unsigned>(ms / 1000 % 60), static_cast<unsigned>(ms % 1000),
                 static_cast<double>(last - stall_from) / 1e6, busy[0] ? busy : "nobody",
                 present_name[0] ? present_name : "?", static_cast<unsigned long>(present),
                 present_at[0] ? present_at : "?", busy_at[0] ? "; busiest at " : "", busy_at,
                 main_at[0] ? "; main thread at " : "", main_at, waiting[0] ? "; waiting " : "", waiting);
        say(line);
        g_reports++;
      }
      phase = kIdle;
      continue;
    }
    const int64_t since = now - last;
    if (phase == kIdle && since >= g_report_ns / 2) {
      const Tick* before = nullptr;
      for (const Tick& t : ticks) {
        if (t.ns && t.ns <= last - 2000000000 && (!before || t.ns > before->ns)) before = &t;
      }
      if (!before || beats - before->beats < 30) {
        stall_beats = beats;
        phase = kArmed;
        armed_at = 0;
        continue;
      }
      if (w32_loader_generation() != generation) {
        g_elf_count = 0;
        w32_loader_segments(collect, nullptr);
        generation = w32_loader_generation();
      }
      mark_threads();
      stall_beats = beats;
      stall_from = last;
      armed_at = w32_monotonic_ns();
      phase = kArmed;
      continue;
    }
    if (phase == kArmed && armed_at && since >= g_report_ns) {
      const DWORD present = __atomic_load_n(&g_beat_thread, __ATOMIC_RELAXED);
      sample(present, present_at, sizeof present_at, 48);
      Busy top[1];
      busy_at[0] = 0;
      const bool busy_found = busiest(top, 1) && top[0].cpu >= 500000 /* 50 ms */;
      if (busy_found && top[0].id != present) {
        sample(top[0].id, busy_at, sizeof busy_at, 32);
      }
      const DWORD main_thread = w32_main_thread_id();
      main_at[0] = 0;
      if (main_thread && main_thread != present && (!busy_found || main_thread != top[0].id)) {
        sample(main_thread, main_at, sizeof main_at, 24);
      }
      waiting[0] = 0;
      if (!busy_found) sample_waiting(present, main_thread, waiting, sizeof waiting);
      phase = kSampled;
    }
  }
}

}  // namespace

DWORD g_present_thread;

DWORD w32_present_thread() { return __atomic_load_n(&g_present_thread, __ATOMIC_RELAXED); }

extern "C" __attribute__((visibility("default"))) void __w32_frame_beat() {
  __atomic_store_n(&g_present_thread, w32.GetCurrentThreadId(), __ATOMIC_RELAXED);
  if (g_frames_gap_ns) note_frame();
  if (!g_stall_on && !g_report_ns) return;
  __atomic_store_n(&g_beat_thread, w32.GetCurrentThreadId(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_beat_ns, w32_monotonic_ns(), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_beat_count, 1, __ATOMIC_RELEASE);
}

void w32_stall_start() {
  if (const char* frames = getenv("BLOX7_W32_FRAMES")) {
    const int ms = atoi(frames);
    g_frames_gap_ns = static_cast<int64_t>(ms > 0 ? ms : 25) * 1000000;
  }
  if (const char* report = getenv("BLOX7_STALL_REPORT")) g_report_ns = static_cast<int64_t>(atoi(report)) * 1000000;
  const char* value = getenv("BLOX7_W32_STALL_DUMP");
  if (!value) {
    if (g_report_ns > 0) w32.CloseHandle(w32.CreateThread(nullptr, 0, reporter, nullptr, 0, nullptr));
    else g_report_ns = 0;
    return;
  }
  g_report_ns = 0;
  const int ms = atoi(value);
  g_stall_ns = static_cast<int64_t>(ms > 0 ? ms : 100) * 1000000;
  g_stall_on = true;
  w32.CloseHandle(w32.CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr));
}
