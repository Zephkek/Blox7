#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"
#include "private/bionic_tls.h"
#include "pthread_internal.h"
#include "w32_kernel.h"

extern "C" __attribute__((visibility("default"))) void* __w32_attach_thread() {
  if (__get_tls() != nullptr) return __get_thread();

  ThreadMapping mapping = __allocate_thread_mapping(0, PTHREAD_GUARD_SIZE);
  auto* thread = static_cast<pthread_internal_t*>(
      mmap(nullptr, sizeof(pthread_internal_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (mapping.mmap_base == nullptr || thread == MAP_FAILED) w32_fatal("out of memory adopting a Windows thread");

  const StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
  auto tcb = reinterpret_cast<bionic_tcb*>(mapping.static_tls + layout.offset_bionic_tcb());
  auto tls = reinterpret_cast<bionic_tls*>(mapping.static_tls + layout.offset_bionic_tls());
  __init_static_tls(mapping.static_tls);
  __init_tcb(tcb, thread);
  __init_tcb_dtv(tcb);
  __init_tcb_stack_guard(tcb);
  __init_bionic_tls_ptrs(tcb, tls);

  void** teb;
  __asm__("mov %%gs:0x30, %0" : "=r"(teb));
  pthread_attr_init(&thread->attr);
  thread->attr.flags |= PTHREAD_ATTR_FLAG_DETACHED;
  thread->attr.stack_base = teb[2];
  thread->attr.stack_size = static_cast<char*>(teb[1]) - static_cast<char*>(teb[2]);
  thread->attr.guard_size = 0;
  thread->mmap_base = mapping.mmap_base;
  thread->mmap_size = mapping.mmap_size;
  thread->mmap_base_unguarded = mapping.mmap_base_unguarded;
  thread->mmap_size_unguarded = mapping.mmap_size_unguarded;
  thread->stack_top = reinterpret_cast<uintptr_t>(teb[1]);

  __set_tls(&tcb->tls_slot(0));
  thread->tid = gettid();
  thread->set_cached_pid(getpid());
  w32_thread_attach_fs();
  w32_vex_arm_thread();
  uint16_t cw = 0x37f;  // x87 like linux
  __asm__ volatile("fldcw %0" : : "m"(cw));
  __init_thread(thread);
  __pthread_internal_add(thread);
  return thread;
}

extern "C" __attribute__((visibility("default"))) void* __w32_pe_open(const char* utf8_path) {
  WCHAR wide[W32_PATH_MAX];
  if (w32_utf8_to_utf16(utf8_path, wide, W32_PATH_MAX) < 0) return nullptr;
  return w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(wide));
}

extern "C" __attribute__((visibility("default"))) void* __w32_pe_symbol(void* module, const char* name) {
  return module ? w32_boot->GetProcAddress(module, name) : nullptr;
}

extern "C" __attribute__((visibility("default"))) int __w32_win32_path(const char* path, char* out, size_t capacity) {
  WCHAR wide[W32_PATH_MAX];
  long r = w32_path(AT_FDCWD, path, wide, W32_PATH_MAX);
  if (r < 0) return static_cast<int>(-r);
  const WCHAR* p = wide;
  size_t n = 0;
  while (p[n]) n++;
  if (n < 4 + 260 && p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\' && p[5] == ':') p += 4;
  return w32_utf16_to_utf8(p, out, capacity) < 0 ? ENAMETOOLONG : 0;
}

extern "C" __attribute__((visibility("default"))) const char* __w32_exe_dir() { return w32_boot->exe_dir; }

struct W32DataBlob {
  DWORD size;
  uint8_t* data;
};

static int w32_crypt(bool protect, const void* in, size_t size, void** out, size_t* out_size) {
  using Crypt = BOOL(WINAPI*)(W32DataBlob*, void*, W32DataBlob*, void*, void*, DWORD, W32DataBlob*);
  using Free = void*(WINAPI*)(void*);
  void* crypt32 = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(u"crypt32.dll"));
  void* kernel32 = w32_boot->LoadLibraryW(reinterpret_cast<const uint16_t*>(u"kernel32.dll"));
  const auto crypt = reinterpret_cast<Crypt>(
      crypt32 ? w32_boot->GetProcAddress(crypt32, protect ? "CryptProtectData" : "CryptUnprotectData") : nullptr);
  const auto local_free = reinterpret_cast<Free>(kernel32 ? w32_boot->GetProcAddress(kernel32, "LocalFree") : nullptr);
  if (!crypt || !local_free) return ENOSYS;
  if (size > 0x1000000) return E2BIG;
  static const char kEntropy[] = "blox7 roblox session";
  W32DataBlob input{static_cast<DWORD>(size), static_cast<uint8_t*>(const_cast<void*>(in))};
  W32DataBlob entropy{sizeof kEntropy - 1, reinterpret_cast<uint8_t*>(const_cast<char*>(kEntropy))};
  W32DataBlob output{0, nullptr};
  if (!crypt(&input, nullptr, &entropy, nullptr, nullptr, 1 /* CRYPTPROTECT_UI_FORBIDDEN */, &output)) return EIO;
  void* copy = malloc(output.size ? output.size : 1);
  if (copy) memcpy(copy, output.data, output.size);
  volatile uint8_t* wipe = output.data;
  for (DWORD i = 0; i < output.size; i++) wipe[i] = 0;
  local_free(output.data);
  if (!copy) return ENOMEM;
  *out = copy;
  *out_size = output.size;
  return 0;
}

extern "C" __attribute__((visibility("default"))) int __w32_protect(const void* in, size_t size, void** out, size_t* out_size) {
  return w32_crypt(true, in, size, out, out_size);
}

extern "C" __attribute__((visibility("default"))) int __w32_unprotect(const void* in, size_t size, void** out, size_t* out_size) {
  return w32_crypt(false, in, size, out, out_size);
}

extern "C" __attribute__((visibility("default"))) int pthread_tryjoin_np(pthread_t t, void** result) {
  pthread_internal_t* thread = __pthread_internal_find(t, "pthread_tryjoin_np");
  if (thread == nullptr) return ESRCH;
  ThreadJoinState state = atomic_load(&thread->join_state);
  if (state == THREAD_DETACHED || state == THREAD_JOINED) return EINVAL;
  if (state != THREAD_EXITED_NOT_JOINED) return EBUSY;
  return pthread_join(t, result);
}
