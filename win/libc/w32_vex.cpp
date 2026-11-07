// Intercept syscalls by parking a debug register on each one and cosplay as Linux yes

#include <emmintrin.h>
#include <stdint.h>
#include <string.h>

#include "w32_exception.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"


extern "C" {
extern int g_vex_site_count;
extern uint64_t g_vex_site_addr[3];
long WINAPI vex_handler(WinExceptionPointers*);
}

namespace {

constexpr long kContinueExecution = -1;
constexpr int kMaxSites = 3;
constexpr int kSyscallLength = 2;
constexpr uint32_t kResumeFlag = 0x10000;
constexpr uint32_t kContextIntegerControl = 0x00100003u;
constexpr uintptr_t kSiteSpan = 0x1000;
constexpr uintptr_t kPastFunction = 64 * 1024;
constexpr int kMaxCandidates = 16;

// the first three in the function are the ones that actually run
constexpr uint8_t kSiteTail[] = {0x45, 0x31, 0xD2, 0x45, 0x31, 0xC0, 0x45, 0x31, 0xC9, 0x0F, 0x05};
constexpr size_t kTailPrefix = sizeof(kSiteTail) - kSyscallLength;

bool g_installed;

int find_sites(uintptr_t lo, uintptr_t hi, uint64_t* sites) {
  if (hi - lo < 64) return 0;
  uintptr_t found[kMaxCandidates];
  int n = 0;
  const __m128i first = _mm_set1_epi8(0x0F), second = _mm_set1_epi8(0x05);
  for (uintptr_t p = hi - 17; p >= lo + kTailPrefix; p -= 16) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 1));
    unsigned mask = static_cast<unsigned>(
        _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, second))));
    while (mask && n < kMaxCandidates) {
      const int bit = 31 - __builtin_clz(mask);
      mask &= ~(1u << bit);
      const uintptr_t at = p + bit;
      if (memcmp(reinterpret_cast<const void*>(at - kTailPrefix), kSiteTail, kTailPrefix) == 0) found[n++] = at;
    }
    if (n && p + kPastFunction < found[n - 1]) break;
  }
  const int count = n < kMaxSites ? n : kMaxSites;
  if (count == 0 || found[n - count] - found[n - 1] > kSiteSpan) return 0;
  for (int i = 0; i < count; i++) sites[i] = found[n - 1 - i];
  return count;
}

void fill_debug_registers(WinContext* c) {
  const int count = __atomic_load_n(&g_vex_site_count, __ATOMIC_ACQUIRE);
  uint64_t dr7 = 0;
  for (int i = 0; i < kMaxSites; i++) {
    c->dr[i] = i < count ? g_vex_site_addr[i] : 0;
    if (i < count) dr7 |= uint64_t{1} << (2 * i);
  }
  c->dr[5] = dr7;
  c->context_flags = kWinContextDebugRegisters;
}

bool arm_suspended(HANDLE thread) {
  if (__atomic_load_n(&g_vex_site_count, __ATOMIC_ACQUIRE) == 0) return false;
  alignas(16) uint8_t context[1536] = {};
  auto* c = reinterpret_cast<WinContext*>(context);
  fill_debug_registers(c);
  return w32.SetThreadContext(thread, context) != 0;
}

bool arm_running(HANDLE thread) {
  if (__atomic_load_n(&g_vex_site_count, __ATOMIC_ACQUIRE) == 0) return false;
  alignas(16) uint8_t context[1536] = {};
  auto* c = reinterpret_cast<WinContext*>(context);
  c->context_flags = kWinContextDebugRegisters;
  if (!w32.GetThreadContext(thread, context)) return false;
  fill_debug_registers(c);
  return w32.SetThreadContext(thread, context) != 0;
}

struct ArmRequest {
  HANDLE target;
  HANDLE done;
};

DWORD WINAPI arm_helper(void* param) {
  auto* request = static_cast<ArmRequest*>(param);
  if (w32.SuspendThread(request->target) != 0xFFFFFFFFu) arm_running(request->target);
  w32.ResumeThread(request->target);
  w32.CloseHandle(request->target);
  w32.SetEvent(request->done);
  return 0;
}

} //nmspc

extern "C" {

long WINAPI w32_vex_hit(WinExceptionPointers* ep, long) {
  WinContext* c = ep->context;
  const long nr = static_cast<int>(c->rax);
  c->rax = static_cast<uint64_t>(__w32_syscall_raw(nr, static_cast<long>(c->rdi), static_cast<long>(c->rsi),
                                                   static_cast<long>(c->rdx), static_cast<long>(c->r10),
                                                   static_cast<long>(c->r8), static_cast<long>(c->r9)));
  c->rip += kSyscallLength;
  c->eflags |= kResumeFlag;
  c->context_flags |= kContextIntegerControl;
  return kContinueExecution;
}

void w32_init_vex() {
  if (!w32.AddVectoredExceptionHandler(1, reinterpret_cast<void*>(&vex_handler))) {
    w32_debug("w32_vex: AddVectoredExceptionHandler failed");
    return;
  }
  g_installed = true;
}

void w32_vex_set_roblox_range(uintptr_t text_lo, uintptr_t text_hi) {
  if (!g_installed) return;
  const int count = find_sites(text_lo, text_hi, g_vex_site_addr);
  __atomic_store_n(&g_vex_site_count, count, __ATOMIC_RELEASE);
  if (count != kMaxSites) w32_debug("w32_vex: %d/%d syscall sites found", count, kMaxSites);
  if (count) w32_vex_arm_thread();
}

void w32_vex_arm_thread() {
  if (!g_installed || __atomic_load_n(&g_vex_site_count, __ATOMIC_ACQUIRE) == 0) return;

  HANDLE process = w32.GetCurrentProcess();
  HANDLE duplicate = nullptr;
  if (!w32.DuplicateHandle(process, reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)), process, &duplicate,
                           0x1F03FF, 0, 0))
    return;
  HANDLE done = w32.CreateEventW(nullptr, 0, 0, nullptr);
  if (!done) {
    w32.CloseHandle(duplicate);
    return;
  }
  // TODO ?: stop threads from editing their own debug registers pls (for now use this janky halper)
  ArmRequest request{duplicate, done};
  HANDLE helper = w32.CreateThread(nullptr, 32 * 1024, arm_helper, &request, 0, nullptr);
  if (!helper) {
    w32.CloseHandle(duplicate);
    w32.CloseHandle(done);
    return;
  }
  w32.WaitForSingleObject(done, 0xFFFFFFFF);
  w32.CloseHandle(helper);
  w32.CloseHandle(done);
}

void w32_vex_arm_thread_suspended(HANDLE thread) {
  if (!g_installed || !thread) return;
  arm_suspended(thread);
}

}
