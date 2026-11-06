// win8+ gives 64-bit threads a 0 fs base, so the canary reads fault and get emulated here. cursed

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <dlfcn.h>

#include "platform/bionic/tls.h"
#include "platform/bionic/tls_defines.h"
#include "w32_exception.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

extern "C" uintptr_t __stack_chk_guard;
extern "C" void* __w32_attach_thread();

namespace {

constexpr DWORD kAccessViolation = 0xc0000005, kIllegalInstruction = 0xc000001d, kPrivileged = 0xc0000096;
constexpr DWORD kIntDivide = 0xc0000094, kIntOverflow = 0xc0000095, kBreakpoint = 0x80000003, kSingleStep = 0x80000004;
constexpr DWORD kStackOverflow = 0xc00000fd, kInPageError = 0xc0000006;
constexpr long kContinueExecution = -1, kContinueSearch = 0;

struct Action {
  uintptr_t handler;
  unsigned long flags;
  uint64_t mask;
};

Action g_actions[65];
W32_SRWLOCK g_actions_lock;

bool emulate_fs_read(WinContext* c) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(c->rip);
  size_t i = 0;
  if (p[i] != 0x64) return false;
  i++;
  uint8_t rex = 0;
  if ((p[i] & 0xf0) == 0x40) rex = p[i++];
  if (!(rex & 8)) return false;
  uint8_t op = p[i++];
  if (op != 0x8b && op != 0x2b && op != 0x33 && op != 0x3b) return false;
  uint8_t modrm = p[i++];
  if ((modrm >> 6) != 0 || (modrm & 7) != 4 || p[i++] != 0x25) return false;
  int32_t disp;
  memcpy(&disp, p + i, 4);
  i += 4;
  if (disp < 0 || disp > MAX_TLS_SLOT * 8 || (disp & 7)) return false;
  void** tls = __get_tls();
  if (!tls) return false;
  int reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
  uint64_t* regs[16] = {&c->rax, &c->rcx, &c->rdx, &c->rbx, &c->rsp, &c->rbp, &c->rsi, &c->rdi,
                        &c->r8,  &c->r9,  &c->r10, &c->r11, &c->r12, &c->r13, &c->r14, &c->r15};
  uint64_t value = disp == TLS_SLOT_SELF * 8 ? reinterpret_cast<uint64_t>(tls) : reinterpret_cast<uint64_t>(tls[disp / 8]);
  uint64_t dst = *regs[reg], result = 0;
  bool write = true;
  switch (op) {
    case 0x8b: *regs[reg] = value; c->rip += i; return true;
    case 0x2b: result = dst - value; break;
    case 0x33: result = dst ^ value; break;
    case 0x3b: result = dst - value; write = false; break;
  }
  if (write) *regs[reg] = result;
  DWORD flags = c->eflags & ~0x8d5u;
  if (result == 0) flags |= 0x40;
  if (result >> 63) flags |= 0x80;
  if (op != 0x33 && dst < value) flags |= 0x1;
  c->eflags = flags;
  c->rip += i;
  return true;
}

void to_ucontext(const WinContext* c, ucontext_t* uc) {
  memset(uc, 0, sizeof(*uc));
  greg_t* g = uc->uc_mcontext.gregs;
  g[REG_R8] = static_cast<greg_t>(c->r8); g[REG_R9] = static_cast<greg_t>(c->r9);
  g[REG_R10] = static_cast<greg_t>(c->r10); g[REG_R11] = static_cast<greg_t>(c->r11);
  g[REG_R12] = static_cast<greg_t>(c->r12); g[REG_R13] = static_cast<greg_t>(c->r13);
  g[REG_R14] = static_cast<greg_t>(c->r14); g[REG_R15] = static_cast<greg_t>(c->r15);
  g[REG_RDI] = static_cast<greg_t>(c->rdi); g[REG_RSI] = static_cast<greg_t>(c->rsi);
  g[REG_RBP] = static_cast<greg_t>(c->rbp); g[REG_RBX] = static_cast<greg_t>(c->rbx);
  g[REG_RDX] = static_cast<greg_t>(c->rdx); g[REG_RAX] = static_cast<greg_t>(c->rax);
  g[REG_RCX] = static_cast<greg_t>(c->rcx); g[REG_RSP] = static_cast<greg_t>(c->rsp);
  g[REG_RIP] = static_cast<greg_t>(c->rip); g[REG_EFL] = static_cast<greg_t>(c->eflags);
  static_assert(sizeof(uc->__fpregs_mem) >= 512);
  memcpy(&uc->__fpregs_mem, c->flt_save, 512);
  uc->uc_mcontext.fpregs = &uc->__fpregs_mem;
}

void from_ucontext(const ucontext_t* uc, WinContext* c) {
  const greg_t* g = uc->uc_mcontext.gregs;
  c->r8 = static_cast<uint64_t>(g[REG_R8]); c->r9 = static_cast<uint64_t>(g[REG_R9]);
  c->r10 = static_cast<uint64_t>(g[REG_R10]); c->r11 = static_cast<uint64_t>(g[REG_R11]);
  c->r12 = static_cast<uint64_t>(g[REG_R12]); c->r13 = static_cast<uint64_t>(g[REG_R13]);
  c->r14 = static_cast<uint64_t>(g[REG_R14]); c->r15 = static_cast<uint64_t>(g[REG_R15]);
  c->rdi = static_cast<uint64_t>(g[REG_RDI]); c->rsi = static_cast<uint64_t>(g[REG_RSI]);
  c->rbp = static_cast<uint64_t>(g[REG_RBP]); c->rbx = static_cast<uint64_t>(g[REG_RBX]);
  c->rdx = static_cast<uint64_t>(g[REG_RDX]); c->rax = static_cast<uint64_t>(g[REG_RAX]);
  c->rcx = static_cast<uint64_t>(g[REG_RCX]); c->rsp = static_cast<uint64_t>(g[REG_RSP]);
  c->rip = static_cast<uint64_t>(g[REG_RIP]);
  c->eflags = static_cast<DWORD>(g[REG_EFL]);
  memcpy(c->flt_save, &uc->__fpregs_mem, 512);
}

bool deliver(int sig, siginfo_t* info, ucontext_t* uc) {
  Action a;
  w32.AcquireSRWLockExclusive(&g_actions_lock);
  a = g_actions[sig];
  if (a.handler > 1 && (a.flags & SA_RESETHAND)) g_actions[sig] = Action{0, 0, 0};
  w32.ReleaseSRWLockExclusive(&g_actions_lock);
  if (a.handler == 1) return true;  // SIG_IGN
  if (a.handler == 0) return false;
  if (a.flags & SA_SIGINFO) {
    reinterpret_cast<void (*)(int, siginfo_t*, void*)>(a.handler)(sig, info, uc);
  } else {
    reinterpret_cast<void (*)(int)>(a.handler)(sig);
  }
  return true;
}

void describe(const char* what, uintptr_t addr) {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(addr), &info) && info.dli_fname) {
    const char* base = strrchr(info.dli_fname, '/');
    w32_debug("  %s %p %s+0x%lx %s+0x%lx", what, reinterpret_cast<void*>(addr), base ? base + 1 : info.dli_fname,
              static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(info.dli_fbase)),
              info.dli_sname ? info.dli_sname : "?",
              static_cast<unsigned long>(info.dli_saddr ? addr - reinterpret_cast<uintptr_t>(info.dli_saddr) : 0));
  } else if (HANDLE module = nullptr;
             w32.GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, reinterpret_cast<void*>(addr), &module)) {
    WCHAR wide[260];
    char name[260] = "?";
    if (w32.GetModuleFileNameW(module, wide, 260)) w32_utf16_to_utf8(wide, name, sizeof name);
    const char* base = strrchr(name, '\\');
    w32_debug("  %s %p %s+0x%lx", what, reinterpret_cast<void*>(addr), base ? base + 1 : name,
              static_cast<unsigned long>(addr - reinterpret_cast<uintptr_t>(module)));
  } else {
    w32_debug("  %s %p (no module)", what, reinterpret_cast<void*>(addr));
  }
}

void scan_stack(const uintptr_t* words, size_t count, int limit) {
  int shown = 0;
  for (size_t i = 0; i < count && shown < limit; i++) {
    uintptr_t v = words[i];
    if (v < 0x10000) continue;
    W32_MEMORY_BASIC_INFORMATION code;
    if (w32.VirtualQuery(reinterpret_cast<void*>(v - 1), &code, sizeof code) && code.state == 0x1000 && (code.protect & 0xf0)) {
      describe("called from", v);
      shown++;
    }
  }
}

DWORD WINAPI stack_dumper(void* seconds) {
  w32.Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(seconds)) * 1000);
  __w32_attach_thread();
  constexpr size_t kWords = 4096;
  auto* words = static_cast<uintptr_t*>(malloc(kWords * sizeof(uintptr_t)));
  alignas(16) static uint8_t context_buffer[2048];
  HANDLE snapshot = w32.CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
  DWORD entry[7] = {sizeof entry};  // THREADENTRY32
  DWORD self = w32.GetCurrentThreadId(), pid = w32.GetCurrentProcessId();
  w32_debug("stack dump: all threads");
  for (BOOL more = w32.Thread32First(snapshot, entry); more; more = w32.Thread32Next(snapshot, entry)) {
    if (entry[3] != pid || entry[2] == self) continue;
    HANDLE thread = w32.OpenThread(0x0002 | 0x0008 | 0x0040, 0, entry[2]);
    if (!thread) continue;
    auto* c = reinterpret_cast<WinContext*>(context_buffer);
    memset(context_buffer, 0, sizeof context_buffer);
    c->context_flags = 0x100003;  // CONTEXT_CONTROL | CONTEXT_INTEGER
    size_t count = 0;
    if (w32.SuspendThread(thread) != 0xffffffff) {
      if (w32.GetThreadContext(thread, c)) {
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->rsp);
        for (; count < kWords; count++) {
          W32_MEMORY_BASIC_INFORMATION m;
          if ((count == 0 || (reinterpret_cast<uintptr_t>(sp + count) & 4095) == 0) &&
              (!w32.VirtualQuery(sp + count, &m, sizeof m) || m.state != 0x1000 || !(m.protect & 0x66) || (m.protect & 0x100)))
            break;
          words[count] = sp[count];
        }
      }
      w32.ResumeThread(thread);
    }
    w32.CloseHandle(thread);
    w32_debug("thread %lu:", static_cast<unsigned long>(entry[2]));
    describe("at", c->rip);
    scan_stack(words, count, 48);
  }
  w32.CloseHandle(snapshot);
  free(words);
  w32_debug("stack dump: end");
  return 0;
}

struct ProfThread {
  DWORD id;
  HANDLE handle;
  uint64_t cycles;
  uint32_t samples;
  uint64_t user0, kernel0;
};
struct ProfHit {
  uintptr_t rip, via;
  DWORD thread;
  uint32_t count;
};

uint64_t prof_filetime(W32_FILETIME t) { return static_cast<uint64_t>(t.hi) << 32 | t.lo; }
struct ProfRange {
  uintptr_t start, end;
};

ProfThread g_prof_threads[512];
int g_prof_thread_count;
ProfHit g_prof_hits[1 << 16];
uint32_t g_prof_overflow;
ProfRange g_prof_code[256];
int g_prof_code_count;

uintptr_t g_prof_code_lo = ~uintptr_t{0}, g_prof_code_hi;

void prof_collect_segment(void*, uintptr_t start, uintptr_t, uintptr_t end, uint64_t, const char*) {
  W32_MEMORY_BASIC_INFORMATION m;
  if (g_prof_code_count < 256 && w32.VirtualQuery(reinterpret_cast<void*>(start), &m, sizeof m) && (m.protect & 0xf0)) {
    g_prof_code[g_prof_code_count++] = ProfRange{start, end};
    if (start < g_prof_code_lo) g_prof_code_lo = start;
    if (end > g_prof_code_hi) g_prof_code_hi = end;
  }
}

bool prof_in_code(uintptr_t v) {
  if (v < g_prof_code_lo || v >= g_prof_code_hi) return false;
  for (int i = 0; i < g_prof_code_count; i++) {
    if (v >= g_prof_code[i].start && v < g_prof_code[i].end) return true;
  }
  return false;
}

void prof_refresh_threads(DWORD self, DWORD pid) {
  HANDLE snapshot = w32.CreateToolhelp32Snapshot(4 /* TH32CS_SNAPTHREAD */, 0);
  DWORD entry[7] = {sizeof entry};
  for (BOOL more = w32.Thread32First(snapshot, entry); more; more = w32.Thread32Next(snapshot, entry)) {
    if (entry[3] != pid || entry[2] == self) continue;
    bool known = false;
    for (int i = 0; i < g_prof_thread_count && !known; i++) known = g_prof_threads[i].id == entry[2];
    if (known || g_prof_thread_count == 512) continue;
    HANDLE h = w32.OpenThread(0x0002 | 0x0008 | 0x0040, 0, entry[2]);
    if (!h) continue;
    uint64_t cycles = 0;
    w32.QueryThreadCycleTime(h, &cycles);
    W32_FILETIME created, exited, kernel{}, user{};
    w32.GetThreadTimes(h, &created, &exited, &kernel, &user);
    g_prof_threads[g_prof_thread_count++] = ProfThread{entry[2], h, cycles, 0, prof_filetime(user), prof_filetime(kernel)};
  }
  w32.CloseHandle(snapshot);
}

void prof_count(uintptr_t rip, uintptr_t via, DWORD thread) {
  const uint64_t key = (rip * 0x9e3779b97f4a7c15ull) ^ (via * 0xc2b2ae3d27d4eb4full) ^ (thread * 0x165667b19e3779f9ull);
  const size_t mask = sizeof g_prof_hits / sizeof *g_prof_hits - 1;
  for (size_t i = (key >> 16) & mask, probes = 0; probes < 64; i = (i + 1) & mask, probes++) {
    ProfHit& h = g_prof_hits[i];
    if (h.count == 0) {
      h = ProfHit{rip, via, thread, 1};
      return;
    }
    if (h.rip == rip && h.via == via && h.thread == thread) {
      h.count++;
      return;
    }
  }
  g_prof_overflow++;
}

void prof_name(uintptr_t addr, char* out, size_t cap) {
  if (addr == 0) {
    snprintf(out, cap, "-");
    return;
  }
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

struct ProfModule {
  DWORD thread;
  uintptr_t base;
  uint32_t count;
  char name[48];
};
ProfModule g_prof_modules[2048];
int g_prof_module_count;

void prof_module_count(DWORD thread, uintptr_t rip, uint32_t count) {
  uintptr_t base = 0;
  char name[260] = "?";
  Dl_info info{};
  HANDLE module = nullptr;
  if (dladdr(reinterpret_cast<void*>(rip), &info) && info.dli_fname) {
    base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  } else if (w32.GetModuleHandleExW(6 /* FROM_ADDRESS | UNCHANGED_REFCOUNT */, reinterpret_cast<void*>(rip), &module)) {
    base = reinterpret_cast<uintptr_t>(module);
  }
  for (int i = 0; i < g_prof_module_count; i++) {
    if (g_prof_modules[i].thread == thread && g_prof_modules[i].base == base) {
      g_prof_modules[i].count += count;
      return;
    }
  }
  if (g_prof_module_count == 2048) return;
  if (info.dli_fname) {
    snprintf(name, sizeof name, "%s", info.dli_fname);
  } else if (module) {
    WCHAR wide[260];
    if (w32.GetModuleFileNameW(module, wide, 260)) w32_utf16_to_utf8(wide, name, sizeof name);
  }
  const char* slash = strrchr(name, '/');
  const char* backslash = strrchr(name, '\\');
  const char* short_name = backslash > slash ? backslash + 1 : slash ? slash + 1 : name;
  ProfModule& m = g_prof_modules[g_prof_module_count++];
  m = ProfModule{thread, base, count, {}};
  snprintf(m.name, sizeof m.name, "%s", short_name);
}

void prof_module_report(DWORD thread, uint32_t samples) {
  if (samples == 0) return;
  char line[400];
  size_t used = 0;
  bool shown[2048] = {};
  for (int round = 0; round < 8; round++) {
    int best = -1;
    for (int i = 0; i < g_prof_module_count; i++) {
      if (!shown[i] && g_prof_modules[i].thread == thread &&
          (best < 0 || g_prof_modules[i].count > g_prof_modules[best].count)) {
        best = i;
      }
    }
    if (best < 0) break;
    shown[best] = true;
    const double share = 100.0 * g_prof_modules[best].count / samples;
    if (share < 1.0) break;
    const int n = snprintf(line + used, sizeof line - used, "%s%s %.0f%%", used ? ", " : "", g_prof_modules[best].name, share);
    if (n < 0 || used + n >= sizeof line) break;
    used += n;
  }
  if (used) w32_debug("prof:   modules: %s", line);
}

struct ProfConfig {
  DWORD start_s, seconds, gap_ms;
};
ProfConfig g_prof_config;

// don't stop threads that barely ran, stopping 50 threads a ms tanks the game we're measuring
constexpr uint64_t kProfMinCycles = 200000;
constexpr int kProfDeepWords = 2048;
uintptr_t g_prof_stack[kProfDeepWords];

DWORD WINAPI profiler(void*) {
  w32.Sleep(g_prof_config.start_s * 1000);
  const DWORD seconds = g_prof_config.seconds;
  const DWORD gap_ms = g_prof_config.gap_ms;
  __w32_attach_thread();
  w32.timeBeginPeriod(1);
  w32_loader_segments(prof_collect_segment, nullptr);
  const DWORD self = w32.GetCurrentThreadId(), pid = w32.GetCurrentProcessId();
  alignas(16) static uint8_t context_buffer[2048];
  auto* c = reinterpret_cast<WinContext*>(context_buffer);
  int64_t frequency = 1, begin = 0, now = 0, refreshed = 0;
  w32.QueryPerformanceFrequency(&frequency);
  w32.QueryPerformanceCounter(&begin);
  uint32_t total = 0, rounds = 0;
  w32_debug("prof: sampling for %u s, %u ms between rounds", static_cast<unsigned>(seconds), static_cast<unsigned>(gap_ms));
  for (now = begin; now - begin < static_cast<int64_t>(seconds) * frequency; w32.QueryPerformanceCounter(&now)) {
    if (now - refreshed > frequency / 4) {
      prof_refresh_threads(self, pid);
      refreshed = now;
    }
    rounds++;
    const DWORD present = w32_present_thread();
    for (int t = 0; t < g_prof_thread_count; t++) {
      ProfThread& th = g_prof_threads[t];
      uint64_t cycles = 0;
      if (!w32.QueryThreadCycleTime(th.handle, &cycles) || cycles == th.cycles) continue;
      const uint64_t ran = cycles - th.cycles;
      th.cycles = cycles;
      if (ran < kProfMinCycles && th.id != present) continue;
      if (w32.SuspendThread(th.handle) == 0xffffffff) continue;
      memset(context_buffer, 0, sizeof context_buffer);
      c->context_flags = 0x100003;  // CONTEXT_CONTROL | CONTEXT_INTEGER
      uintptr_t rip = 0, via = 0;
      int words = 0;
      if (w32.GetThreadContext(th.handle, c)) {
        rip = c->rip;
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->rsp);
        W32_MEMORY_BASIC_INFORMATION m;
        if (w32.VirtualQuery(sp, &m, sizeof m) && m.state == 0x1000 && (m.protect & 0x66) && !(m.protect & 0x100)) {
          const uintptr_t end = reinterpret_cast<uintptr_t>(m.base) + m.region_size;
          const uintptr_t room = (end - reinterpret_cast<uintptr_t>(sp)) / sizeof(uintptr_t);
          const int want = prof_in_code(rip) ? 64 : kProfDeepWords;
          words = static_cast<int>(room < static_cast<uintptr_t>(want) ? room : want);
          memcpy(g_prof_stack, sp, static_cast<size_t>(words) * sizeof(uintptr_t));
        }
      }
      w32.ResumeThread(th.handle);
      for (int i = 0; i < words; i++) {
        if (g_prof_stack[i] != rip && prof_in_code(g_prof_stack[i])) {
          via = g_prof_stack[i];
          break;
        }
      }
      if (rip) {
        prof_count(rip, via, th.id);
        th.samples++;
        total++;
      }
    }
    w32.Sleep(gap_ms);
  }
  const double elapsed = static_cast<double>(now - begin) / static_cast<double>(frequency);
  w32_debug("prof: %u samples of busy threads in %.1f s (%u rounds, %u not counted)", total, elapsed, rounds,
            g_prof_overflow);
  const size_t slots = sizeof g_prof_hits / sizeof *g_prof_hits;
  for (size_t i = 0; i < slots; i++) {
    if (g_prof_hits[i].count) prof_module_count(g_prof_hits[i].thread, g_prof_hits[i].rip, g_prof_hits[i].count);
  }
  for (int t = 0; t < g_prof_thread_count; t++) {
    ProfThread& th = g_prof_threads[t];
    W32_FILETIME created, exited, kernel{}, user{};
    w32.GetThreadTimes(th.handle, &created, &exited, &kernel, &user);
    const double user_ms = static_cast<double>(prof_filetime(user) - th.user0) / 1e4;
    const double kernel_ms = static_cast<double>(prof_filetime(kernel) - th.kernel0) / 1e4;
    if (th.samples || user_ms + kernel_ms >= 1) {
      w32_debug("prof: thread %lu '%s' cpu user %.0f ms kernel %.0f ms, %u samples", static_cast<unsigned long>(th.id),
                w32_thread_name_of(th.id), user_ms, kernel_ms, th.samples);
      prof_module_report(th.id, th.samples);
    }
    w32.CloseHandle(th.handle);
  }
  for (int shown = 0; shown < 1500; shown++) {
    size_t best = slots;
    for (size_t i = 0; i < slots; i++) {
      if (g_prof_hits[i].count && (best == slots || g_prof_hits[i].count > g_prof_hits[best].count)) best = i;
    }
    if (best == slots) break;
    char at[160], via[160];
    prof_name(g_prof_hits[best].rip, at, sizeof at);
    prof_name(g_prof_hits[best].via, via, sizeof via);
    w32_debug("prof: %u t%lu %s <- %s", g_prof_hits[best].count, static_cast<unsigned long>(g_prof_hits[best].thread), at,
              via);
    g_prof_hits[best].count = 0;
  }
  w32_debug("prof: end");
  return 0;
}

void crash_report(const WinExceptionRecord* r, const WinContext* c, int sig) {
  w32_debug("crash: signal %d (exception 0x%08x), fault address %p, thread %lu", sig, r->code,
            reinterpret_cast<void*>(r->information[1]), static_cast<unsigned long>(w32.GetCurrentThreadId()));
  describe("at", c->rip);
  w32_debug("  rax %016lx rbx %016lx rcx %016lx rdx %016lx", c->rax, c->rbx, c->rcx, c->rdx);
  w32_debug("  rsi %016lx rdi %016lx rbp %016lx rsp %016lx", c->rsi, c->rdi, c->rbp, c->rsp);
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->rsp);
  W32_MEMORY_BASIC_INFORMATION m;
  size_t count = 0;
  if (w32.VirtualQuery(sp, &m, sizeof m) && m.state == 0x1000) {
    uintptr_t end = reinterpret_cast<uintptr_t>(m.base) + m.region_size;
    count = (end - c->rsp) / sizeof(uintptr_t);
    if (count > 2048) count = 2048;
  }
  scan_stack(sp, count, 24);
}

bool in_windows_image(void* address) {
  W32_MEMORY_BASIC_INFORMATION mbi;
  return w32.VirtualQuery(address, &mbi, sizeof mbi) && mbi.type == 0x1000000 /* MEM_IMAGE */;
}

long WINAPI on_exception(WinExceptionPointers* ep) {
  WinExceptionRecord* r = ep->record;
  WinContext* c = ep->context;
  // not a bionic thread, not our problem
  if (__get_tls() == nullptr) return kContinueSearch;
  int sig = 0, code = 0;
  void* addr = r->address;
  switch (r->code) {
    case kAccessViolation:
      if (r->information[1] < 0x10000 && w32_fs_redirect(c)) return kContinueExecution;
      if (r->information[0] == 0 && r->information[1] != c->rip && emulate_fs_read(c)) return kContinueExecution;
      sig = SIGSEGV;
      code = SEGV_ACCERR;
      addr = reinterpret_cast<void*>(r->information[1]);
      break;
    case kInPageError: sig = SIGBUS; code = BUS_ADRERR; addr = reinterpret_cast<void*>(r->information[1]); break;
    case kIllegalInstruction: case kPrivileged: sig = SIGILL; code = ILL_ILLOPC; break;
    case kIntDivide: sig = SIGFPE; code = FPE_INTDIV; break;
    case kIntOverflow: sig = SIGFPE; code = FPE_INTOVF; break;
    case kBreakpoint: case kSingleStep: sig = SIGTRAP; code = TRAP_BRKPT; break;
    case kStackOverflow: return kContinueSearch;
    default: return kContinueSearch;
  }
  if (in_windows_image(r->address)) return kContinueSearch;
  siginfo_t info;
  memset(&info, 0, sizeof info);
  info.si_signo = sig;
  info.si_code = code;
  info.si_addr = addr;
  ucontext_t uc;
  to_ucontext(c, &uc);
  uc.uc_mcontext.gregs[REG_TRAPNO] = sig == SIGSEGV ? 14 : sig == SIGILL ? 6 : sig == SIGFPE ? 0 : 3;
  uc.uc_mcontext.gregs[REG_ERR] = sig == SIGSEGV ? static_cast<greg_t>(4 | (r->information[0] == 1 ? 2 : 0)) : 0;
  uc.uc_mcontext.gregs[REG_CR2] = reinterpret_cast<greg_t>(info.si_addr);
  if (!deliver(sig, &info, &uc)) {
    crash_report(r, c, sig);
    return kContinueSearch;
  }
  from_ucontext(&uc, c);
  return kContinueExecution;
}

[[noreturn]] void default_action(int sig) {
  switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
      __builtin_unreachable();
    default:
      w32_debug("terminated by signal %d", sig);
      w32.ExitProcess(128 + static_cast<DWORD>(sig));
      __builtin_unreachable();
  }
}

bool ignored_by_default(int sig) { return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT; }

long send_to_self(int sig, siginfo_t* info) {
  if (sig == 0) return 0;
  if (sig < 1 || sig > 64) return -EINVAL;
  siginfo_t local;
  if (!info) {
    memset(&local, 0, sizeof local);
    local.si_signo = sig;
    local.si_code = SI_TKILL;
    local.si_pid = static_cast<pid_t>(w32.GetCurrentProcessId());
    info = &local;
  }
  ucontext_t uc;
  memset(&uc, 0, sizeof uc);
  if (!deliver(sig, info, &uc) && !ignored_by_default(sig)) default_action(sig);
  return 0;
}

bool is_self(long pid) {
  return pid == 0 || pid == static_cast<long>(w32.GetCurrentProcessId()) || pid == -1;
}

}  // namespace

void w32_init_signals() {
  w32.AddVectoredExceptionHandler(1, reinterpret_cast<void*>(on_exception));
  if (const char* after = getenv("BLOX7_W32_STACKDUMP_AFTER")) {
    uintptr_t seconds = static_cast<uintptr_t>(atoi(after));
    if (seconds > 0) w32.CloseHandle(w32.CreateThread(nullptr, 0, stack_dumper, reinterpret_cast<void*>(seconds), 0, nullptr));
  }
  if (const char* profile = getenv("BLOX7_W32_PROFILE")) {
    const char* colon = strchr(profile, ':');
    const char* colon2 = colon ? strchr(colon + 1, ':') : nullptr;
    const int start = atoi(profile), seconds = colon ? atoi(colon + 1) : 10, gap = colon2 ? atoi(colon2 + 1) : 1;
    g_prof_config = ProfConfig{static_cast<DWORD>(start > 0 ? start : 0), static_cast<DWORD>(seconds),
                               static_cast<DWORD>(gap > 0 ? gap : 1)};
    if (seconds > 0) w32.CloseHandle(w32.CreateThread(nullptr, 0, profiler, nullptr, 0, nullptr));
  }
}

struct KernelSigaction {
  uintptr_t handler;
  unsigned long flags;
  uintptr_t restorer;
  uint64_t mask;
};

long sys_rt_sigaction(long sig, long act_l, long oact_l, long, long, long) {
  if (sig < 1 || sig > 64 || sig == SIGKILL || (sig == SIGSTOP && act_l)) return -EINVAL;
  auto act = reinterpret_cast<const KernelSigaction*>(act_l);
  auto oact = reinterpret_cast<KernelSigaction*>(oact_l);
  w32.AcquireSRWLockExclusive(&g_actions_lock);
  if (oact) *oact = KernelSigaction{g_actions[sig].handler, g_actions[sig].flags, 0, g_actions[sig].mask};
  if (act) g_actions[sig] = Action{act->handler, act->flags, act->mask};
  w32.ReleaseSRWLockExclusive(&g_actions_lock);
  return 0;
}

// one mask for everybody, nothing ever gets blocked anyway
static uint64_t t_mask;

void w32_signal_masks(uint64_t* blocked, uint64_t* ignored, uint64_t* caught) {
  *blocked = t_mask;
  *ignored = *caught = 0;
  w32.AcquireSRWLockExclusive(&g_actions_lock);
  for (int sig = 1; sig < 65; sig++) {
    if (g_actions[sig].handler == 1) *ignored |= 1ull << (sig - 1);
    else if (g_actions[sig].handler > 1) *caught |= 1ull << (sig - 1);
  }
  w32.ReleaseSRWLockExclusive(&g_actions_lock);
}

long sys_rt_sigprocmask(long how, long set_l, long old_l, long, long, long) {
  if (old_l) *reinterpret_cast<uint64_t*>(old_l) = t_mask;
  if (set_l) {
    uint64_t set = *reinterpret_cast<const uint64_t*>(set_l);
    switch (how) {
      case SIG_BLOCK: t_mask |= set; break;
      case SIG_UNBLOCK: t_mask &= ~set; break;
      case SIG_SETMASK: t_mask = set; break;
      default: return -EINVAL;
    }
  }
  return 0;
}

long sys_rt_sigpending(long set, long, long, long, long, long) {
  *reinterpret_cast<uint64_t*>(set) = 0;
  return 0;
}

long sys_sigaltstack(long ss, long old, long, long, long, long) {
  if (old) {
    auto o = reinterpret_cast<stack_t*>(old);
    o->ss_sp = nullptr;
    o->ss_size = 0;
    o->ss_flags = SS_DISABLE;
  }
  (void)ss;
  return 0;
}

long w32_kill_child(pid_t pid, int sig);

long sys_kill(long pid, long sig, long, long, long, long) {
  if (!is_self(pid)) return w32_kill_child(static_cast<pid_t>(pid), static_cast<int>(sig));
  return send_to_self(static_cast<int>(sig), nullptr);
}

long sys_tgkill(long tgid, long tid, long sig, long, long, long) {
  (void)tid;
  if (!is_self(tgid)) return -ESRCH;
  return send_to_self(static_cast<int>(sig), nullptr);
}

long sys_tkill(long tid, long sig, long, long, long, long) {
  (void)tid;
  return send_to_self(static_cast<int>(sig), nullptr);
}

long sys_rt_sigqueueinfo(long pid, long sig, long info, long, long, long) {
  if (!is_self(pid)) return -ESRCH;
  return send_to_self(static_cast<int>(sig), reinterpret_cast<siginfo_t*>(info));
}

long sys_rt_tgsigqueueinfo(long pid, long, long sig, long info, long, long) {
  return sys_rt_sigqueueinfo(pid, sig, info, 0, 0, 0);
}

long sys_rt_sigsuspend(long, long, long, long, long, long) {
  w32.Sleep(0xffffffff);
  return -EINTR;
}

long sys_rt_sigtimedwait(long, long, long timeout, long, long, long) {
  if (timeout) {
    auto ts = reinterpret_cast<const timespec*>(timeout);
    w32.Sleep(static_cast<DWORD>(ts->tv_sec * 1000 + ts->tv_nsec / 1000000));
    return -EAGAIN;
  }
  w32.Sleep(0xffffffff);
  return -EINTR;
}

long sys_pause(long, long, long, long, long, long) {
  w32.Sleep(0xffffffff);
  return -EINTR;
}
