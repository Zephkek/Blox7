#include <errno.h>
#include <linux/sched.h>
#include <sched.h>
#include <string.h>

#include "pthread_internal.h"
#include "w32_exception.h"
#include "w32_kernel.h"
#include "w32_syscalls.h"

extern "C" void w32_run_on_stack(void* stack_top, void (*fn)(void*, void*), void* a, void* b, void** saved_rsp);
extern "C" __noreturn void w32_leave_stack(void* saved_rsp, void (*fn)(void*, void*, size_t), void* a, void* b,
                                           size_t c);
extern "C" __LIBC_HIDDEN__ void __start_thread(int (*fn)(void*), void* arg);
extern "C" uintptr_t __stack_chk_guard;

namespace {

constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD PAGE_READWRITE = 0x04;
constexpr DWORD PAGE_EXECUTE_READ = 0x20;
constexpr DWORD CREATE_SUSPENDED = 0x4;
constexpr size_t kWindowsStack = 256 * 1024;

struct W32Thread {
  void* windows_rsp;
  int* child_tid;
  void* stack_base;
  void* stack_limit;
  char comm[16];
};

struct Start {
  int (*fn)(void*);
  void* arg;
  void* tls;
  int* child_tid;
  uint32_t flags;
  char comm[16];
};

DWORD g_thread_slot;
DWORD g_main_thread_id;
constexpr char kAndroidProcessName[] = "com.roblox.client";
char g_main_comm[16];
char g_host_comm[16];

inline void** teb() {
  void** t;
  __asm__("mov %%gs:0x30, %0" : "=r"(t));
  return t;
}

constexpr size_t kTlsSlots = 0x1480 / sizeof(void*);
constexpr size_t kFsBlock = 256;
constexpr size_t kFsChunk = 64 * 1024;

DWORD g_fs_block_slot, g_fs_rip_slot, g_fs_save_slot;
const uint8_t* g_fs_rearm;
W32_SRWLOCK g_fs_lock;
uintptr_t* g_fs_free;

inline uintptr_t read_fs_base() {
  uintptr_t value;
  __asm__ volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xc0" : "=a"(value));
  return value;
}

inline void write_fs_base(uintptr_t value) {
  __asm__ volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xd0" : : "a"(value) : "memory");
}

uint8_t* put_gs(uint8_t* p, const uint8_t* op, size_t size, DWORD slot) {
  *p++ = 0x65;
  memcpy(p, op, size);
  p += size;
  const uint32_t disp = static_cast<uint32_t>((kTlsSlots + slot) * sizeof(void*));
  memcpy(p, &disp, sizeof disp);
  return p + sizeof disp;
}

// windows wipes %fs on every nap, so the first canary read after faults in here and we just put it back lol
bool init_fs_modern() {
  struct {
    uint32_t size, major, minor, build, platform;
    uint16_t service_pack[128];
  } version{sizeof version, 0, 0, 0, 0, {}};
  if (w32.RtlGetVersion(&version) != 0 || version.major < 6 || (version.major == 6 && version.minor < 2)) return false;
  if (!w32.IsProcessorFeaturePresent(22)) return false;
  g_fs_block_slot = w32.TlsAlloc();
  g_fs_rip_slot = w32.TlsAlloc();
  g_fs_save_slot = w32.TlsAlloc();
  if (g_fs_block_slot >= 64 || g_fs_rip_slot >= 64 || g_fs_save_slot >= 64) return false;
  auto* code = static_cast<uint8_t*>(w32.VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!code) return false;
  static const uint8_t store_rax[] = {0x48, 0x89, 0x04, 0x25}, load_rax[] = {0x48, 0x8b, 0x04, 0x25},
                       jump[] = {0xff, 0x24, 0x25}, wrfsbase_rax[] = {0xf3, 0x48, 0x0f, 0xae, 0xd0};
  uint8_t* p = put_gs(code, store_rax, sizeof store_rax, g_fs_save_slot);
  p = put_gs(p, load_rax, sizeof load_rax, g_fs_block_slot);
  memcpy(p, wrfsbase_rax, sizeof wrfsbase_rax);
  p += sizeof wrfsbase_rax;
  p = put_gs(p, load_rax, sizeof load_rax, g_fs_save_slot);
  put_gs(p, jump, sizeof jump, g_fs_rip_slot);
  DWORD old;
  if (!w32.VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) return false;
  w32.FlushInstructionCache(w32.GetCurrentProcess(), code, 4096);
  g_fs_rearm = code;
  return true;
}

uintptr_t* take_fs_block() {
  w32.AcquireSRWLockExclusive(&g_fs_lock);
  uintptr_t* block = g_fs_free;
  if (block) g_fs_free = reinterpret_cast<uintptr_t*>(block[0]);
  w32.ReleaseSRWLockExclusive(&g_fs_lock);
  if (block) return block;
  auto* chunk = static_cast<uint8_t*>(w32.VirtualAlloc(nullptr, kFsChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!chunk) return nullptr;
  w32.AcquireSRWLockExclusive(&g_fs_lock);
  for (size_t at = kFsBlock; at < kFsChunk; at += kFsBlock) {
    auto* spare = reinterpret_cast<uintptr_t*>(chunk + at);
    spare[0] = reinterpret_cast<uintptr_t>(g_fs_free);
    g_fs_free = spare;
  }
  w32.ReleaseSRWLockExclusive(&g_fs_lock);
  return reinterpret_cast<uintptr_t*>(chunk);
}

void give_fs_block(uintptr_t* block) {
  w32.AcquireSRWLockExclusive(&g_fs_lock);
  block[0] = reinterpret_cast<uintptr_t>(g_fs_free);
  g_fs_free = block;
  w32.ReleaseSRWLockExclusive(&g_fs_lock);
}

}  // namespace

bool g_w32_fs_modern;

void w32_fs_rearm_now() {
  const auto block = reinterpret_cast<uintptr_t>(teb()[kTlsSlots + g_fs_block_slot]);
  if (block && read_fs_base() != block) write_fs_base(block);
}

bool w32_fs_redirect(WinContext* c) {
  if (!g_w32_fs_modern) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(c->rip);
  size_t i = 0;
  bool fs = false;
  for (; i < 6; ++i) {
    const uint8_t b = p[i];
    if (b == 0x64) fs = true;
    else if (b != 0x66 && b != 0x67 && b != 0xf2 && b != 0xf3 && b != 0x2e && b != 0x3e && b != 0x26 && b != 0x36 &&
             b != 0x65)
      break;
  }
  if (!fs) return false;
  if ((p[i] & 0xf0) == 0x40) ++i;
  if (p[i] == 0x0f) i += (p[i + 1] == 0x38 || p[i + 1] == 0x3a) ? 3 : 2;
  else ++i;
  if ((p[i] >> 6) != 0 || (p[i] & 7) != 4 || p[i + 1] != 0x25) return false;
  int32_t disp;
  memcpy(&disp, p + i + 2, sizeof disp);
  if (disp < 0 || static_cast<size_t>(disp) >= kFsBlock) return false;
  void** t = teb();
  if (!t[kTlsSlots + g_fs_block_slot]) w32_thread_attach_fs();
  if (!t[kTlsSlots + g_fs_block_slot]) return false;
  t[kTlsSlots + g_fs_rip_slot] = reinterpret_cast<void*>(c->rip);
  c->rip = reinterpret_cast<uint64_t>(g_fs_rearm);
  return true;
}

__attribute__((no_stack_protector)) void w32_thread_attach_fs() {
  if (g_w32_fs_modern) {
    void** t = teb();
    auto* block = static_cast<uintptr_t*>(t[kTlsSlots + g_fs_block_slot]);
    if (!block) {
      block = take_fs_block();
      if (!block) return;
      t[kTlsSlots + g_fs_block_slot] = block;
    }
    volatile uintptr_t* slots = block;
    slots[TLS_SLOT_SELF] = reinterpret_cast<uintptr_t>(block);
    slots[TLS_SLOT_THREAD_ID] = reinterpret_cast<uintptr_t>(__get_tls()[TLS_SLOT_THREAD_ID]);
    slots[TLS_SLOT_STACK_GUARD] = __stack_chk_guard;
    write_fs_base(reinterpret_cast<uintptr_t>(block));
    return;
  }
  uintptr_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(teb()) + 0x2000);
  if (base >= W32BOOT_FS_REGION_START && base + 4096 <= W32BOOT_FS_REGION_END &&
      w32.VirtualAlloc(reinterpret_cast<void*>(base), 4096, MEM_COMMIT, PAGE_READWRITE)) {
    volatile uintptr_t* slots = reinterpret_cast<volatile uintptr_t*>(base);
    slots[TLS_SLOT_SELF] = base;
    slots[TLS_SLOT_THREAD_ID] = reinterpret_cast<uintptr_t>(__get_tls()[TLS_SLOT_THREAD_ID]);
    slots[TLS_SLOT_STACK_GUARD] = __stack_chk_guard;
    return;
  }
}

__attribute__((no_stack_protector)) void w32_init_threads() {
  g_thread_slot = w32.TlsAlloc();
  g_main_thread_id = w32.GetCurrentThreadId();
  // zygote cuts com.roblox.client down to m.roblox.client, so we do too
  constexpr size_t kCommLen = 15;
  const size_t name_len = sizeof(kAndroidProcessName) - 1;
  const size_t comm_len = name_len > kCommLen ? kCommLen : name_len;
  memcpy(g_main_comm, kAndroidProcessName + (name_len - comm_len), comm_len + 1);
  uint16_t cw = 0x37f;
  __asm__ volatile("fldcw %0" : : "m"(cw));
  g_w32_fs_modern = init_fs_modern();
}

long sys_gettid(long, long, long, long, long, long) {
  DWORD id = w32.GetCurrentThreadId();
  return id == g_main_thread_id ? static_cast<long>(w32.GetCurrentProcessId()) : static_cast<long>(id);
}

long sys_getpid(long, long, long, long, long, long) { return static_cast<long>(w32.GetCurrentProcessId()); }
long sys_getppid(long, long, long, long, long, long) { return 1; }

long sys_set_tid_address(long tidptr, long, long, long, long, long) {
  if (W32Thread* self = static_cast<W32Thread*>(w32.TlsGetValue(g_thread_slot))) self->child_tid = reinterpret_cast<int*>(tidptr);
  return sys_gettid(0, 0, 0, 0, 0, 0);
}

// windows priorities are strict and linux nice is a suggestion. fmod asks for nice 19 and starved
namespace {

constexpr int kSchedFifo = 1, kSchedRr = 2, kSchedBatch = 3, kSchedIdle = 5, kResetOnFork = 0x40000000;

int nice_level(int nice) {
  if (nice <= -11) return 2;   // THREAD_PRIORITY_HIGHEST
  if (nice < 0) return 1;      // THREAD_PRIORITY_ABOVE_NORMAL
  return 0;                    // THREAD_PRIORITY_NORMAL
}

int level_nice(int level) {
  return level >= 2 ? -11 : level == 1 ? -1 : 0;
}

struct NiceEntry {
  DWORD tid;
  int nice, level;
};
NiceEntry g_nice[1024];
volatile int g_nice_lock;

NiceEntry* nice_entry(DWORD tid, bool add) {
  for (size_t i = (tid * 2654435761u) % 1024, probes = 0; probes < 1024; i = (i + 1) % 1024, probes++) {
    if (g_nice[i].tid == tid) return &g_nice[i];
    if (g_nice[i].tid == 0) {
      if (!add) return nullptr;
      g_nice[i].tid = tid;
      return &g_nice[i];
    }
  }
  return nullptr;
}

DWORD windows_thread_id(long who) {
  if (who == 0) return w32.GetCurrentThreadId();
  if (who == static_cast<long>(w32.GetCurrentProcessId())) return g_main_thread_id;
  return static_cast<DWORD>(who);
}

HANDLE thread_handle(DWORD tid, DWORD access, bool* own) {
  *own = false;
  if (tid == w32.GetCurrentThreadId()) return w32.GetCurrentThread();
  *own = true;
  return w32.OpenThread(access, 0, tid);
}

}  // namespace

long sys_setpriority(long which, long who, long prio, long, long, long) {
  if (which != 0) return 0;
  const int nice = prio < -20 ? -20 : prio > 19 ? 19 : static_cast<int>(prio);
  const DWORD tid = windows_thread_id(who);
  bool own;
  HANDLE h = thread_handle(tid, 0x0020 | 0x0040 /* SET_INFORMATION | QUERY_INFORMATION */, &own);
  if (!h) return -ESRCH;
  const BOOL ok = w32.SetThreadPriority(h, nice_level(nice));
  if (own) w32.CloseHandle(h);
  if (!ok) return -ESRCH;
  while (__atomic_exchange_n(&g_nice_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  if (NiceEntry* e = nice_entry(tid, true)) {
    e->nice = nice;
    e->level = nice_level(nice);
  }
  __atomic_store_n(&g_nice_lock, 0, __ATOMIC_RELEASE);
  w32_debug("sched: thread %lu '%s' nice %d, level %d", static_cast<unsigned long>(tid),
            tid == w32.GetCurrentThreadId() ? w32_thread_comm() : "?", nice, nice_level(nice));
  return 0;
}

long sys_getpriority(long which, long who, long, long, long, long) {
  if (which != 0) return 20;
  const DWORD tid = windows_thread_id(who);
  bool own;
  HANDLE h = thread_handle(tid, 0x0040 /* QUERY_INFORMATION */, &own);
  if (!h) return -ESRCH;
  const int level = w32.GetThreadPriority(h);
  if (own) w32.CloseHandle(h);
  if (level == 0x7fffffff /* THREAD_PRIORITY_ERROR_RETURN */) return -ESRCH;
  int nice = level_nice(level);
  while (__atomic_exchange_n(&g_nice_lock, 1, __ATOMIC_ACQUIRE)) {
  }
  if (NiceEntry* e = nice_entry(tid, false); e && e->level == level) nice = e->nice;
  __atomic_store_n(&g_nice_lock, 0, __ATOMIC_RELEASE);
  return 20 - nice;
}

long sys_sched_setscheduler(long, long policy_l, long param, long, long, long) {
  const int policy = static_cast<int>(policy_l) & ~kResetOnFork;
  if (policy == kSchedFifo || policy == kSchedRr) return -EPERM;
  if (policy != 0 && policy != kSchedBatch && policy != kSchedIdle) return -EINVAL;
  if (!param || *reinterpret_cast<const int*>(param) != 0) return -EINVAL;
  return 0;
}

long sys_sched_setparam(long, long param, long, long, long, long) {
  if (!param) return -EINVAL;
  return *reinterpret_cast<const int*>(param) == 0 ? 0 : -EINVAL;
}

long sys_sched_get_priority_max(long policy, long, long, long, long, long) {
  if (policy == kSchedFifo || policy == kSchedRr) return 99;
  return policy == 0 || policy == kSchedBatch || policy == kSchedIdle ? 0 : -EINVAL;
}

long sys_sched_get_priority_min(long policy, long, long, long, long, long) {
  if (policy == kSchedFifo || policy == kSchedRr) return 1;
  return policy == 0 || policy == kSchedBatch || policy == kSchedIdle ? 0 : -EINVAL;
}

long sys_sched_yield(long, long, long, long, long, long) {
  if (__builtin_expect(g_w32_stats, 0)) w32_stats_yield(w32_stats_caller());
  w32.SwitchToThread();
  return 0;
}

static void run_thread(void* start_ptr, void*) {
  Start s = *static_cast<Start*>(start_ptr);
  __start_thread(s.fn, s.arg);
}

__attribute__((no_stack_protector)) static DWORD WINAPI thread_entry(void* param) {
  Start* start = static_cast<Start*>(param);
  W32Thread self{};
  self.child_tid = (start->flags & CLONE_CHILD_CLEARTID) ? start->child_tid : nullptr;
  memcpy(self.comm, start->comm, sizeof self.comm);
  w32.TlsSetValue(g_thread_slot, &self);
  w32_note_thread_name(self.comm);
  uint16_t cw = 0x37f;
  __asm__ volatile("fldcw %0" : : "m"(cw));
  if (start->flags & CLONE_SETTLS) __set_tls(start->tls);
  w32_thread_attach_fs();

  void** t = teb();
  self.stack_base = t[1];
  self.stack_limit = t[2];
  pthread_internal_t* thread = static_cast<pthread_internal_t*>(__get_tls()[TLS_SLOT_THREAD_ID]);
  if ((start->flags & CLONE_SETTLS) && thread && thread->attr.stack_base) {
    t[2] = thread->attr.stack_base;
    t[1] = static_cast<char*>(thread->attr.stack_base) + thread->attr.stack_size;
  }
  w32_run_on_stack(start, run_thread, start, nullptr, &self.windows_rsp);
  return 0;
}

struct NamedThread {
  DWORD id;
  char comm[16];
};
NamedThread g_named[1024];

void w32_note_thread_name(const char* comm) {
  const DWORD id = w32.GetCurrentThreadId();
  for (size_t i = id % 1024, probes = 0; probes < 1024; i = (i + 1) % 1024, probes++) {
    DWORD seen = __atomic_load_n(&g_named[i].id, __ATOMIC_ACQUIRE);
    if (seen == 0) __atomic_compare_exchange_n(&g_named[i].id, &seen, id, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (seen != 0 && seen != id) continue;
    memcpy(g_named[i].comm, comm, 15);
    g_named[i].comm[15] = 0;
    return;
  }
}

const char* w32_thread_name_of(DWORD id) {
  for (size_t i = id % 1024, probes = 0; probes < 1024; i = (i + 1) % 1024, probes++) {
    const DWORD seen = __atomic_load_n(&g_named[i].id, __ATOMIC_ACQUIRE);
    if (seen == id) return g_named[i].comm;
    if (seen == 0) break;
  }
  return id == g_main_thread_id ? g_main_comm : "";
}

char* w32_thread_comm() {
  if (W32Thread* self = static_cast<W32Thread*>(w32.TlsGetValue(g_thread_slot))) return self->comm;
  return w32.GetCurrentThreadId() == g_main_thread_id ? g_main_comm : g_host_comm;
}

const char* w32_process_name() { return kAndroidProcessName; }

const char* w32_process_comm() { return g_main_comm; }

volatile int g_live_threads = 1;

int w32_thread_count() { return __atomic_load_n(&g_live_threads, __ATOMIC_RELAXED); }

extern "C" __LIBC_HIDDEN__ pid_t __bionic_clone(uint32_t flags, void* child_stack, int* parent_tid, void* tls,
                                                int* child_tid, int (*fn)(void*), void* arg) {
  if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD)) {
    errno = ENOSYS;
    return -1;
  }
  uintptr_t top = (reinterpret_cast<uintptr_t>(child_stack) - sizeof(Start)) & ~uintptr_t{15};
  Start* start = reinterpret_cast<Start*>(top);
  *start = Start{fn, arg, tls, child_tid, flags, {}};
  memcpy(start->comm, w32_thread_comm(), sizeof start->comm);
  DWORD id;
  HANDLE h = w32.CreateThread(nullptr, kWindowsStack, thread_entry, start, CREATE_SUSPENDED | 0x10000 /* reservation */, &id);
  if (h == nullptr) {
    errno = w32_errno_from_win32(w32.GetLastError()) == ENOMEM ? ENOMEM : EAGAIN;
    return -1;
  }
  if (flags & CLONE_PARENT_SETTID) *parent_tid = static_cast<int>(id);
  if (flags & CLONE_CHILD_SETTID) *child_tid = static_cast<int>(id);
  __atomic_add_fetch(&g_live_threads, 1, __ATOMIC_RELAXED);
  w32_vex_arm_thread_suspended(h);
  w32.ResumeThread(h);
  w32.CloseHandle(h);
  return static_cast<pid_t>(id);
}

static void finish_thread(void* self_ptr, void* unmap_base, size_t unmap_size) {
  W32Thread* self = static_cast<W32Thread*>(self_ptr);
  __set_tls(nullptr);
  if (g_w32_fs_modern) {
    void** t = teb();
    if (auto* block = static_cast<uintptr_t*>(t[kTlsSlots + g_fs_block_slot])) {
      t[kTlsSlots + g_fs_block_slot] = nullptr;
      write_fs_base(0);
      give_fs_block(block);
    }
  }
  if (unmap_base) sys_munmap(reinterpret_cast<long>(unmap_base), static_cast<long>(unmap_size), 0, 0, 0, 0);
  if (self && self->child_tid) {
    __atomic_store_n(self->child_tid, 0, __ATOMIC_SEQ_CST);
    w32_futex_wake(self->child_tid, 0x7fffffff);
  }
  if (self) __atomic_sub_fetch(&g_live_threads, 1, __ATOMIC_RELAXED);
  w32.ExitThread(0);
}

static __noreturn void end_thread(void* unmap_base, size_t unmap_size) {
  W32Thread* self = static_cast<W32Thread*>(w32.TlsGetValue(g_thread_slot));
  if (self == nullptr) {
    finish_thread(nullptr, nullptr, 0);
    __builtin_unreachable();
  }
  void** t = teb();
  t[1] = self->stack_base;
  t[2] = self->stack_limit;
  w32_leave_stack(self->windows_rsp, finish_thread, self, unmap_base, unmap_size);
}

extern "C" __LIBC_HIDDEN__ __noreturn void _exit_with_stack_teardown(void* base, size_t size) {
  end_thread(base, size);
}

long sys_exit(long, long, long, long, long, long) { end_thread(nullptr, 0); }

long sys_exit_group(long status, long, long, long, long, long) {
  w32.ExitProcess(static_cast<DWORD>(status));
  __builtin_unreachable();
}
