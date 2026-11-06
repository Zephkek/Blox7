#include "compat/bionic_abi_exports.h"

#include <linux/futex.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <dlfcn.h>
#include <link.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "compat/bionic_pthread_create_runtime.h"
#include "compat/guest_abi.h"
#include "compat/http_client_spin_guard.h"
#include "window/window.h"

volatile uintptr_t g_blox7_abort_libroblox_base = 0;
std::atomic<void*> g_current_jni_env_for_publish{nullptr};

namespace {

std::atomic<bool> g_legacy_bionic_diagnostics_enabled{false};

const char* EnvironmentOr(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : fallback;
}

const char* GuestAbiPropertyValue() {
#if defined(__aarch64__)
  return "arm64-v8a";
#else
  return "x86_64";
#endif
}

int CallHostVsnprintf(char* dst, size_t count, const char* format,
                      va_list args) {
  using HostVsnprintf = int (*)(char*, size_t, const char*, va_list);
  HostVsnprintf volatile host_vsnprintf = &std::vsnprintf;
  return host_vsnprintf(dst, count, format, args);
}

ssize_t CallHostRead(int fd, void* buf, size_t count) {
  using HostRead = ssize_t (*)(int, void*, size_t);
  HostRead volatile host_read = &::read;
  return host_read(fd, buf, count);
}

ssize_t CallHostWrite(int fd, const void* buf, size_t count) {
  using HostWrite = ssize_t (*)(int, const void*, size_t);
  HostWrite volatile host_write = &::write;
  return host_write(fd, buf, count);
}

ssize_t CallHostPread(int fd, void* buf, size_t count, off_t offset) {
  using HostPread = ssize_t (*)(int, void*, size_t, off_t);
  HostPread volatile host_pread = &::pread;
  return host_pread(fd, buf, count, offset);
}

int CallHostPoll(struct pollfd* fds, nfds_t nfds, int timeout) {
  using HostPoll = int (*)(struct pollfd*, nfds_t, int);
  HostPoll volatile host_poll = &::poll;
  return host_poll(fds, nfds, timeout);
}

ssize_t CallHostSendto(int socket_fd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest, socklen_t dest_len) {
  using HostSendto = ssize_t (*)(int, const void*, size_t, int,
                                 const struct sockaddr*, socklen_t);
  HostSendto volatile host_sendto = &::sendto;
  return host_sendto(socket_fd, buf, len, flags, dest, dest_len);
}

}  // namespace

namespace blox7::compat {

void SetLegacyBionicDiagnosticsEnabled(bool enabled) {
  g_legacy_bionic_diagnostics_enabled.store(enabled, std::memory_order_release);
}

}  // namespace blox7::compat

extern "C" {
void* blox7_egl_display() { return blox7::window::GetEGLDisplay(); }
void* blox7_egl_surface() { return blox7::window::GetEGLSurface(); }
void* blox7_egl_context() { return blox7::window::GetEGLContext(); }
void* blox7_egl_config() { return blox7::window::GetEGLConfig(); }
void* blox7_native_window() { return blox7::window::GetNativeWindow(); }
__attribute__((visibility("default"))) void blox7_set_current_jni_env(
    void* env) {
  g_current_jni_env_for_publish.store(env, std::memory_order_release);
}
__attribute__((visibility("default"))) void* blox7_get_current_jni_env() {
  return g_current_jni_env_for_publish.load(std::memory_order_acquire);
}
#ifndef BLOX7_WIN32
__attribute__((visibility("default"))) uintptr_t __stack_chk_guard =
    0x595e9fbd94fda766ULL;
int* __errno() { return &errno; }
#endif
#ifndef BLOX7_WIN32
void __assert(const char* file, int line, const char* failed_expression) {
  std::fprintf(stderr, "Bionic assertion failed: %s:%d: %s\n",
               file ? file : "(unknown)", line,
               failed_expression ? failed_expression : "(unknown)");
  std::abort();
}
#endif
#ifndef BLOX7_WIN32
void __assert2(const char* file, int line, const char* function,
               const char* failed_expression) {
  std::fprintf(stderr, "Bionic assertion failed: %s:%d: %s: %s\n",
               file ? file : "(unknown)", line,
               function ? function : "(unknown)",
               failed_expression ? failed_expression : "(unknown)");
  std::abort();
}
#endif
__attribute__((noreturn)) void blox7_abort() {
  uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  uintptr_t rax = 0;
  uintptr_t rbx = 0;
  uintptr_t rdi = 0;
  uintptr_t r15 = 0;
#if defined(__x86_64__)
  asm volatile("mov %%rax,%0" : "=r"(rax));
  asm volatile("mov %%rbx,%0" : "=r"(rbx));
  asm volatile("mov %%rdi,%0" : "=r"(rdi));
  asm volatile("mov %%r15,%0" : "=r"(r15));
#endif
  uintptr_t base = static_cast<uintptr_t>(g_blox7_abort_libroblox_base);
  uintptr_t caller_offset = (base != 0 && caller >= base) ? caller - base : 0;
  std::fprintf(stderr,
               "  [abort] bionic abort caller=%p off=0x%lx "
               "rax=%p rbx=%p rdi=%p r15=%p\n",
               reinterpret_cast<void*>(caller),
               static_cast<unsigned long>(caller_offset),
               reinterpret_cast<void*>(rax), reinterpret_cast<void*>(rbx),
               reinterpret_cast<void*>(rdi), reinterpret_cast<void*>(r15));
  if (g_legacy_bionic_diagnostics_enabled.load(std::memory_order_acquire) &&
      caller_offset == 0x2c18f35 && rbx >= 0x10000 &&
      rbx < 0x0000800000000000ULL) {
    const auto* key = reinterpret_cast<const uint64_t*>(rbx);
    std::fprintf(stderr,
                 "  [abort] emutls key=%p size=0x%llx align=0x%llx "
                 "index=0x%llx init=0x%llx\n",
                 reinterpret_cast<const void*>(key),
                 static_cast<unsigned long long>(key[0]),
                 static_cast<unsigned long long>(key[1]),
                 static_cast<unsigned long long>(key[2]),
                 static_cast<unsigned long long>(key[3]));
  }
  std::fflush(stderr);
  std::abort();
}
#ifndef BLOX7_WIN32
char* __gnu_strerror_r(int error_number, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) {
    errno = ERANGE;
    return buf;
  }
  const char* message = std::strerror(error_number);
  if (message == nullptr) {
    std::snprintf(buf, buf_len, "Unknown error %d", error_number);
  } else {
    std::snprintf(buf, buf_len, "%s", message);
  }
  return buf;
}
#endif
// our libc's own __system_property_get finds no property area, so roblox gets ours under that name
int blox7_system_property_get(const char* name, char* value) {
  if (value == nullptr) {
    return 0;
  }
  const char* result = "";
  if (name == nullptr) {
    result = "";
  } else if (std::strcmp(name, "ro.build.version.sdk") == 0) {
    result = "36";
  } else if (std::strcmp(name, "ro.build.version.release") == 0) {
    result = "16";
  } else if (std::strcmp(name, "ro.build.id") == 0) {
    result = "BP2A.250605.031.A3";
  } else if (std::strcmp(name, "ro.build.version.incremental") == 0) {
    result = "S908BXXSNGZD7";
  } else if (std::strcmp(name, "ro.build.fingerprint") == 0) {
    result = "samsung/b0sxeea/b0s:16/BP2A.250605.031.A3/S908BXXSNGZD7:user/release-keys";
  } else if (std::strcmp(name, "ro.build.type") == 0) {
    result = "user";
  } else if (std::strcmp(name, "ro.build.tags") == 0) {
    result = "release-keys";
  } else if (std::strcmp(name, "ro.soc.manufacturer") == 0) {
    result = "Samsung";
  } else if (std::strcmp(name, "ro.soc.model") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_SOC_MODEL", "s5e9925");
  } else if (std::strcmp(name, "ro.product.cpu.abi") == 0 ||
             std::strcmp(name, "ro.product.cpu.abilist") == 0 ||
             std::strcmp(name, "ro.product.cpu.abilist64") == 0) {
    result = GuestAbiPropertyValue();
  } else if (std::strcmp(name, "ro.product.cpu.abilist32") == 0) {
    result = "";
  } else if (std::strcmp(name, "ro.product.manufacturer") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_MANUFACTURER", "samsung");
  } else if (std::strcmp(name, "ro.product.model") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_MODEL", "SM-S908B");
  } else if (std::strcmp(name, "ro.product.brand") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_BRAND", "samsung");
  } else if (std::strcmp(name, "ro.product.device") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_CODE", "b0s");
  } else if (std::strcmp(name, "ro.product.name") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_SKU", "b0sxeea");
  } else if (std::strcmp(name, "ro.product.board") == 0 ||
             std::strcmp(name, "ro.hardware") == 0) {
    result = EnvironmentOr("BLOX7_DEVICE_SOC_MODEL", "s5e9925");
  } else if (std::strcmp(name, "debug.hwui.renderer") == 0) {
    result = "skiagl";
  }
  constexpr size_t kAndroidPropertyValueMax = 92;
  std::strncpy(value, result, kAndroidPropertyValueMax - 1);
  value[kAndroidPropertyValueMax - 1] = '\0';
  return static_cast<int>(std::strlen(value));
}
#ifndef BLOX7_WIN32
int __system_property_get(const char* name, char* value) {
  return blox7_system_property_get(name, value);
}
#endif

}  // extern "C"

extern "C" {
#ifndef BLOX7_WIN32
void* __memcpy_chk(void* dst, const void* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memcpy(dst, src, count);
}
#endif
#ifndef BLOX7_WIN32
void* __memmove_chk(void* dst, const void* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memmove(dst, src, count);
}
#endif
#ifndef BLOX7_WIN32
void* __memset_chk(void* dst, int value, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memset(dst, value, count);
}
#endif
#ifndef BLOX7_WIN32
size_t __strlen_chk(const char* value, size_t) {
  return std::strlen(value);
}
#endif
#ifndef BLOX7_WIN32
char* __strchr_chk(const char* value, int ch, size_t) {
  return const_cast<char*>(std::strchr(value, ch));
}
#endif
#ifndef BLOX7_WIN32
char* __strcpy_chk(char* dst, const char* src, size_t dst_len) {
  size_t src_len = std::strlen(src) + 1;
  if (src_len > dst_len) {
    errno = ERANGE;
    src_len = dst_len;
  }
  if (src_len > 0) {
    std::memcpy(dst, src, src_len);
    dst[src_len - 1] = '\0';
  }
  return dst;
}
#endif
#ifndef BLOX7_WIN32
char* __strncpy_chk(char* dst, const char* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::strncpy(dst, src, count);
}
#endif
#ifndef BLOX7_WIN32
char* __strncpy_chk2(char* dst, const char* src, size_t count, size_t dst_len,
                     size_t src_len) {
  size_t bounded_count = count;
  if (bounded_count > dst_len) {
    errno = ERANGE;
    bounded_count = dst_len;
  }
  if (bounded_count > src_len) {
    errno = ERANGE;
    bounded_count = src_len;
  }
  return std::strncpy(dst, src, bounded_count);
}
#endif
#ifndef BLOX7_WIN32
char* __strcat_chk(char* dst, const char* src, size_t dst_len) {
  size_t dst_used = std::strlen(dst);
  if (dst_used >= dst_len) {
    errno = ERANGE;
    return dst;
  }
  size_t remaining = dst_len - dst_used - 1;
  std::strncat(dst, src, remaining);
  return dst;
}
#endif
#ifndef BLOX7_WIN32
int __vsprintf_chk(char* dst, int, size_t dst_len, const char* format,
                   va_list args) {
  return CallHostVsnprintf(dst, dst_len, format, args);
}
#endif
#ifndef BLOX7_WIN32
int __vsnprintf_chk(char* dst, size_t count, int, size_t dst_len,
                    const char* format, va_list args) {
  size_t bounded_count = std::min(count, dst_len);
  return CallHostVsnprintf(dst, bounded_count, format, args);
}
#endif
#ifndef BLOX7_WIN32
ssize_t __read_chk(int fd, void* buf, size_t count, size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostRead(fd, buf, count);
}
#endif
#ifndef BLOX7_WIN32
ssize_t __write_chk(int fd, const void* buf, size_t count, size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostWrite(fd, buf, count);
}
#endif
#ifndef BLOX7_WIN32
ssize_t __pread64_chk(int fd, void* buf, size_t count, off_t offset,
                      size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostPread(fd, buf, count, offset);
}
#endif
bool blox7_is_low_bionic_pointer(const void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr) < 4096;
}

constexpr size_t kBionicMutexSize = 40;
constexpr uint16_t kBionicMutexDestroyedState = 0xffff;
constexpr uint16_t kBionicMutexTypeShift = 14;
constexpr uint16_t kBionicMutexTypeMask = 0x3;
constexpr uint16_t kBionicMutexSharedMask = 1u << 13;
constexpr uint16_t kBionicMutexStateMask = 0x3;
constexpr uint16_t kBionicMutexUnlocked = 0;
constexpr uint16_t kBionicMutexLockedUncontended = 1;
constexpr uint16_t kBionicMutexLockedContended = 2;
constexpr uint16_t kBionicMutexCounterMask = 0x1ffcu;
constexpr uint16_t kBionicMutexCounterOne = 1u << 2;
constexpr uint16_t kBionicMutexTypeFieldMask =
    kBionicMutexTypeMask << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsNormal = 0;
constexpr uint16_t kBionicMutexTypeBitsRecursive =
    1u << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsErrorCheck =
    2u << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsPi = kBionicMutexTypeFieldMask;
constexpr int64_t kBionicMutexAttrTypeMask = 0x0f;
constexpr int kBionicMutexNormal = 0;
constexpr int kBionicMutexRecursive = 1;
constexpr int kBionicMutexErrorCheck = 2;
static_assert(sizeof(pthread_mutex_t) >= kBionicMutexSize);

int blox7_bionic_mutex_type_from_attributes(
    const Blox7BionicMutexAttr* guest_attributes) {
  if (guest_attributes == nullptr) {
    return kBionicMutexNormal;
  }
  int64_t attributes = 0;
  std::memcpy(&attributes, guest_attributes, sizeof(attributes));
  const int type = static_cast<int>(attributes & kBionicMutexAttrTypeMask);
  return type <= kBionicMutexErrorCheck ? type : -1;
}

void blox7_store_bionic_mutex_attributes(Blox7BionicMutexAttr* guest_attr,
                                            int64_t value) {
  std::memcpy(guest_attr, &value, sizeof(value));
}

uint16_t* blox7_bionic_mutex_state(pthread_mutex_t* mutex) {
  return reinterpret_cast<uint16_t*>(mutex);
}

int32_t* blox7_bionic_mutex_owner(pthread_mutex_t* mutex) {
  return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(mutex) + 4);
}

pid_t blox7_bionic_tid() {
  thread_local pid_t tid = 0;
  if (__builtin_expect(tid == 0, 0)) {
    tid = ::gettid();
  }
  return tid;
}

void blox7_initialize_bionic_mutex_storage(pthread_mutex_t* guest_mutex,
                                              int type) {
  std::memset(guest_mutex, 0, kBionicMutexSize);
  const uint16_t guest_state = static_cast<uint16_t>(
      (type & kBionicMutexTypeMask) << kBionicMutexTypeShift);
  __atomic_store_n(blox7_bionic_mutex_state(guest_mutex), guest_state,
                   __ATOMIC_RELAXED);
}

int blox7_bionic_futex(uint32_t* addr, int op, uint32_t val,
                          const timespec* timeout, uint32_t bitset);

int blox7_bionic_mutex_futex(pthread_mutex_t* mutex, int wait_op,
                                uint16_t expected, bool shared) {
  uint32_t* word = reinterpret_cast<uint32_t*>(mutex);
  const int op = shared ? wait_op : (wait_op | FUTEX_PRIVATE_FLAG);
  return blox7_bionic_futex(word, op, expected, nullptr, 0);
}

int blox7_bionic_normal_mutex_trylock(uint16_t* state, uint16_t shared) {
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t locked =
      static_cast<uint16_t>(shared | kBionicMutexLockedUncontended);
  uint16_t old_state = unlocked;
  if (__atomic_compare_exchange_n(state, &old_state, locked, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int blox7_bionic_normal_mutex_lock(pthread_mutex_t* mutex, uint16_t shared) {
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  if (blox7_bionic_normal_mutex_trylock(state, shared) == 0) {
    return 0;
  }
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t contended =
      static_cast<uint16_t>(shared | kBionicMutexLockedContended);
  for (;;) {
    const uint16_t old_state =
        __atomic_exchange_n(state, contended, __ATOMIC_ACQUIRE);
    if (old_state == unlocked) {
      return 0;
    }
    (void)blox7_bionic_mutex_futex(mutex, FUTEX_WAIT, contended,
                                      shared != 0);
  }
}

void blox7_bionic_normal_mutex_unlock(pthread_mutex_t* mutex,
                                         uint16_t shared) {
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t contended =
      static_cast<uint16_t>(shared | kBionicMutexLockedContended);
  const uint16_t old_state =
      __atomic_exchange_n(state, unlocked, __ATOMIC_RELEASE);
  if (old_state == contended) {
    (void)blox7_bionic_mutex_futex(mutex, FUTEX_WAKE, 1, shared != 0);
  }
}

int blox7_bionic_recursive_increment(uint16_t* state, uint16_t old_state) {
  if ((old_state & kBionicMutexCounterMask) == kBionicMutexCounterMask) {
    return EAGAIN;
  }
  __atomic_fetch_add(state, kBionicMutexCounterOne, __ATOMIC_RELAXED);
  return 0;
}

int blox7_bionic_owned_mutex_lock(pthread_mutex_t* mutex, uint16_t old_state) {
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  int32_t* owner = blox7_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  const pid_t tid = blox7_bionic_tid();
  if (tid == __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    if (mtype == kBionicMutexTypeBitsErrorCheck) {
      return EDEADLK;
    }
    return blox7_bionic_recursive_increment(state, old_state);
  }

  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  const uint16_t locked_uncontended =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedUncontended);
  const uint16_t locked_contended =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedContended);

  if (old_state == unlocked) {
    if (__atomic_compare_exchange_n(state, &old_state, locked_uncontended,
                                    false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
      return 0;
    }
  }

  for (;;) {
    if (old_state == unlocked) {
      if (__atomic_compare_exchange_n(state, &old_state, locked_contended, true,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
        return 0;
      }
      continue;
    }
    if ((old_state & kBionicMutexStateMask) == kBionicMutexLockedUncontended) {
      const uint16_t contended =
          static_cast<uint16_t>(old_state ^ (kBionicMutexLockedContended ^
                                             kBionicMutexLockedUncontended));
      if (!__atomic_compare_exchange_n(state, &old_state, contended, true,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        continue;
      }
      old_state = contended;
    }
    (void)blox7_bionic_mutex_futex(mutex, FUTEX_WAIT, old_state,
                                      shared != 0);
    old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  }
}

int blox7_bionic_owned_mutex_trylock(pthread_mutex_t* mutex,
                                        uint16_t old_state) {
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  int32_t* owner = blox7_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const pid_t tid = blox7_bionic_tid();
  if (tid == __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    if (mtype == kBionicMutexTypeBitsErrorCheck) {
      return EBUSY;
    }
    return blox7_bionic_recursive_increment(state, old_state);
  }
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  const uint16_t locked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedUncontended);
  old_state = unlocked;
  if (__atomic_compare_exchange_n(state, &old_state, locked, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
    return 0;
  }
  return EBUSY;
}

int blox7_bionic_owned_mutex_unlock(pthread_mutex_t* mutex,
                                       uint16_t old_state) {
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  int32_t* owner = blox7_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  if (blox7_bionic_tid() != __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    return EPERM;
  }
  if ((old_state & kBionicMutexCounterMask) != 0) {
    __atomic_fetch_sub(state, kBionicMutexCounterOne, __ATOMIC_RELAXED);
    return 0;
  }
  __atomic_store_n(owner, 0, __ATOMIC_RELAXED);
  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  old_state = __atomic_exchange_n(state, unlocked, __ATOMIC_RELEASE);
  if ((old_state & kBionicMutexStateMask) == kBionicMutexLockedContended) {
    (void)blox7_bionic_mutex_futex(mutex, FUTEX_WAKE, 1, shared != 0);
  }
  return 0;
}

constexpr uint32_t kBionicCondSharedMask = 0x1;
constexpr uint32_t kBionicCondClockMask = 0x2;
constexpr uint32_t kBionicCondCounterStep = 0x4;
constexpr uint32_t kBionicCondFlagsMask =
    kBionicCondSharedMask | kBionicCondClockMask;
constexpr uint32_t kBionicCondDestroyedState = 0xdeadc04d;
constexpr uint32_t kBionicCondAttrDestroyedState = 0xdeada11d;

uint32_t* blox7_bionic_cond_state(pthread_cond_t* cond) {
  return reinterpret_cast<uint32_t*>(cond);
}

void blox7_store_bionic_condattr_flags(pthread_condattr_t* attr,
                                          uint32_t flags) {
  std::memset(attr, 0, sizeof(*attr));
  std::memcpy(attr, &flags, sizeof(flags));
}

uint32_t blox7_load_bionic_condattr_flags(const pthread_condattr_t* attr) {
  uint32_t flags = 0;
  std::memcpy(&flags, attr, sizeof(flags));
  return flags;
}

int blox7_bionic_futex(uint32_t* addr, int op, uint32_t val,
                          const timespec* timeout, uint32_t bitset) {
  return static_cast<int>(
      ::syscall(SYS_futex, addr, op, val, timeout, nullptr, bitset));
}

int blox7_bionic_cond_pulse(pthread_cond_t* cond, int waiters) {
  uint32_t* state = blox7_bionic_cond_state(cond);
  const uint32_t previous =
      __atomic_fetch_add(state, kBionicCondCounterStep, __ATOMIC_RELAXED);
  const int op = (previous & kBionicCondSharedMask) != 0 ? FUTEX_WAKE
                                                         : FUTEX_WAKE_PRIVATE;
  (void)blox7_bionic_futex(state, op, static_cast<uint32_t>(waiters),
                              nullptr, 0);
  return 0;
}

int blox7_bionic_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                   bool use_realtime_clock,
                                   const timespec* abstime) {
  uint32_t* state = blox7_bionic_cond_state(cond);
  const uint32_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  const int unlock_result = blox7_pthread_mutex_unlock(mutex);
  if (unlock_result != 0) {
    return unlock_result;
  }

  const bool shared = (old_state & kBionicCondSharedMask) != 0;
  int wait_result = 0;
  for (;;) {
    int op = 0;
    uint32_t bitset = 0;
    if (abstime == nullptr) {
      op = shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    } else {
      op = shared ? FUTEX_WAIT_BITSET
                  : (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG);
      if (use_realtime_clock) {
        op |= FUTEX_CLOCK_REALTIME;
      }
      bitset = FUTEX_BITSET_MATCH_ANY;
    }
    const int rc =
        blox7_bionic_futex(state, op, old_state, abstime, bitset);
    if (rc == 0 || errno == EAGAIN) {
      wait_result = 0;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ETIMEDOUT) {
      wait_result = ETIMEDOUT;
      break;
    }
    wait_result = errno != 0 ? errno : EINVAL;
    break;
  }

  const int lock_result = blox7_pthread_mutex_lock(mutex);
  if (lock_result != 0) {
    return lock_result;
  }
  return wait_result;
}

int blox7_pthread_condattr_init(pthread_condattr_t* attr) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  blox7_store_bionic_condattr_flags(attr, 0);
  return 0;
}
int blox7_pthread_condattr_destroy(pthread_condattr_t* attr) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  blox7_store_bionic_condattr_flags(attr, kBionicCondAttrDestroyedState);
  return 0;
}
int blox7_pthread_condattr_setclock(pthread_condattr_t* attr,
                                       clockid_t clock_id) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) {
    return EINVAL;
  }
  uint32_t flags = blox7_load_bionic_condattr_flags(attr);
  flags = (flags & ~kBionicCondClockMask) |
          (static_cast<uint32_t>(clock_id) << 1);
  blox7_store_bionic_condattr_flags(attr, flags);
  return 0;
}
int blox7_pthread_cond_init(pthread_cond_t* cond,
                               const pthread_condattr_t* attr) {
  if (blox7_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  if (attr != nullptr && blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  uint32_t flags = 0;
  if (attr != nullptr) {
    flags = blox7_load_bionic_condattr_flags(attr) & kBionicCondFlagsMask;
  }
  std::memset(cond, 0, sizeof(*cond));
  __atomic_store_n(blox7_bionic_cond_state(cond), flags, __ATOMIC_RELAXED);
  return 0;
}
int blox7_pthread_cond_destroy(pthread_cond_t* cond) {
  if (blox7_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  __atomic_store_n(blox7_bionic_cond_state(cond), kBionicCondDestroyedState,
                   __ATOMIC_RELAXED);
  return 0;
}
int blox7_pthread_cond_signal(pthread_cond_t* cond) {
  if (blox7_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  return blox7_bionic_cond_pulse(cond, 1);
}
int blox7_pthread_cond_broadcast(pthread_cond_t* cond) {
  if (blox7_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  return blox7_bionic_cond_pulse(cond, INT_MAX);
}
int blox7_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  if (blox7_is_low_bionic_pointer(cond) ||
      blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (cond == nullptr || mutex == nullptr) {
    return EINVAL;
  }
  return blox7_bionic_cond_timedwait(cond, mutex, false, nullptr);
}
int blox7_pthread_cond_timedwait(pthread_cond_t* cond,
                                    pthread_mutex_t* mutex,
                                    const timespec* abstime) {
  if (blox7_is_low_bionic_pointer(cond) ||
      blox7_is_low_bionic_pointer(mutex)) {
    return ETIMEDOUT;
  }
  if (cond == nullptr || mutex == nullptr || abstime == nullptr) {
    return EINVAL;
  }
  const uint32_t flags =
      __atomic_load_n(blox7_bionic_cond_state(cond), __ATOMIC_RELAXED);
  const bool use_realtime_clock = (flags & kBionicCondClockMask) == 0;
  return blox7_bionic_cond_timedwait(cond, mutex, use_realtime_clock,
                                        abstime);
}

int blox7_pthread_mutexattr_init(Blox7BionicMutexAttr* attr) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  blox7_store_bionic_mutex_attributes(attr, kBionicMutexNormal);
  return 0;
}
int blox7_pthread_mutexattr_destroy(Blox7BionicMutexAttr* attr) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  blox7_store_bionic_mutex_attributes(attr, -1);
  return 0;
}
int blox7_pthread_mutexattr_settype(Blox7BionicMutexAttr* attr, int type) {
  if (attr == nullptr || blox7_is_low_bionic_pointer(attr) ||
      type < kBionicMutexNormal || type > kBionicMutexErrorCheck) {
    return EINVAL;
  }
  int64_t attributes = 0;
  std::memcpy(&attributes, attr, sizeof(attributes));
  attributes = (attributes & ~kBionicMutexAttrTypeMask) | type;
  blox7_store_bionic_mutex_attributes(attr, attributes);
  return 0;
}

int blox7_pthread_mutex_init(pthread_mutex_t* mutex,
                                const Blox7BionicMutexAttr* attr) {
  if (blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  if (attr != nullptr && blox7_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  const int bionic_type =
      blox7_bionic_mutex_type_from_attributes(attr);
  if (bionic_type < 0) {
    return EINVAL;
  }
  blox7_initialize_bionic_mutex_storage(mutex, bionic_type);
  return 0;
}
int blox7_pthread_mutex_destroy(pthread_mutex_t* mutex) {
  if (blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  if ((old_state & kBionicMutexStateMask) != kBionicMutexUnlocked) {
    return EBUSY;
  }
  if (__atomic_compare_exchange_n(state, &old_state, kBionicMutexDestroyedState,
                                  false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}
int blox7_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  if (mtype == kBionicMutexTypeBitsPi) {
    return EINVAL;
  }
  if (mtype == kBionicMutexTypeBitsNormal) {
    return blox7_bionic_normal_mutex_lock(
        mutex, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  }
  return blox7_bionic_owned_mutex_lock(mutex, old_state);
}
int blox7_pthread_mutex_trylock(pthread_mutex_t* mutex) {
  if (blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  if (mtype == kBionicMutexTypeBitsPi) {
    return EINVAL;
  }
  if (mtype == kBionicMutexTypeBitsNormal) {
    return blox7_bionic_normal_mutex_trylock(
        state, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  }
  return blox7_bionic_owned_mutex_trylock(mutex, old_state);
}
int blox7_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  if (blox7_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return 0;
  }
  uint16_t* state = blox7_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  int unlock_result = 0;
  if (mtype == kBionicMutexTypeBitsPi) {
    unlock_result = EINVAL;
  } else if (mtype == kBionicMutexTypeBitsNormal) {
    blox7_bionic_normal_mutex_unlock(
        mutex, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  } else {
    unlock_result = blox7_bionic_owned_mutex_unlock(mutex, old_state);
  }
  if (unlock_result == 0) {
    blox7::compat::ApplyHttpClientSpinGuard();
  }
  return unlock_result;
}

constexpr int kOnceNotStarted = 0;
constexpr int kOnceUnderway = 1;
constexpr int kOnceComplete = 2;
constexpr uint32_t kSpinUnlocked = 0;
constexpr uint32_t kSpinLocked = 1;
constexpr uint32_t kSpinContended = 2;
constexpr uint32_t kBarrierWaitState = 0;
constexpr uint32_t kBarrierReleaseState = 1;

int* blox7_once_word(pthread_once_t* once_control) {
  return reinterpret_cast<int*>(once_control);
}

int* blox7_spin_word(pthread_spinlock_t* lock) {
  return reinterpret_cast<int*>(lock);
}

uint32_t* blox7_barrier_init_count(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(barrier);
}

uint32_t* blox7_barrier_state(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(barrier) + 4);
}

uint32_t* blox7_barrier_wait_count(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(barrier) + 8);
}

int blox7_pthread_once(pthread_once_t* once_control,
                          void (*init_routine)(void)) {
  if (once_control == nullptr || init_routine == nullptr ||
      blox7_is_low_bionic_pointer(once_control)) {
    return EINVAL;
  }
  int* word = blox7_once_word(once_control);
  int old_value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  for (;;) {
    if (old_value == kOnceComplete) {
      return 0;
    }
    if (!__atomic_compare_exchange_n(word, &old_value, kOnceUnderway, true,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
      continue;
    }
    if (old_value == kOnceNotStarted) {
      init_routine();
      __atomic_store_n(word, kOnceComplete, __ATOMIC_RELEASE);
      (void)blox7_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                  FUTEX_WAKE_PRIVATE,
                                  static_cast<uint32_t>(INT_MAX), nullptr, 0);
      return 0;
    }
    (void)blox7_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAIT_PRIVATE,
                                static_cast<uint32_t>(old_value), nullptr, 0);
    old_value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  }
}

int blox7_pthread_spin_init(pthread_spinlock_t* lock, int pshared) {
  static_cast<void>(pshared);
  if (lock == nullptr ||
      reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  __atomic_store_n(blox7_spin_word(lock), static_cast<int>(kSpinUnlocked),
                   __ATOMIC_RELAXED);
  return 0;
}

int blox7_pthread_spin_destroy(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = blox7_spin_word(lock);
  int old_value = static_cast<int>(kSpinUnlocked);
  if (__atomic_compare_exchange_n(word, &old_value,
                                  static_cast<int>(kSpinUnlocked), false,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int blox7_pthread_spin_trylock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = blox7_spin_word(lock);
  int old_value = static_cast<int>(kSpinUnlocked);
  if (__atomic_compare_exchange_n(word, &old_value,
                                  static_cast<int>(kSpinLocked), false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int blox7_pthread_spin_lock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  if (blox7_pthread_spin_trylock(lock) == 0) {
    return 0;
  }
  int* word = blox7_spin_word(lock);
  for (;;) {
    const int old_value = __atomic_exchange_n(
        word, static_cast<int>(kSpinContended), __ATOMIC_ACQUIRE);
    if (old_value == static_cast<int>(kSpinUnlocked)) {
      return 0;
    }
    (void)blox7_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAIT_PRIVATE, kSpinContended, nullptr, 0);
  }
}

int blox7_pthread_spin_unlock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = blox7_spin_word(lock);
  const int old_value = __atomic_exchange_n(
      word, static_cast<int>(kSpinUnlocked), __ATOMIC_RELEASE);
  if (old_value == static_cast<int>(kSpinContended)) {
    (void)blox7_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAKE_PRIVATE, 1, nullptr, 0);
  }
  return 0;
}

int blox7_pthread_barrier_init(pthread_barrier_t* barrier,
                                  const pthread_barrierattr_t* attr,
                                  unsigned count) {
  static_cast<void>(attr);
  if (barrier == nullptr || blox7_is_low_bionic_pointer(barrier) ||
      count == 0) {
    return EINVAL;
  }
  std::memset(barrier, 0, sizeof(*barrier));
  __atomic_store_n(blox7_barrier_init_count(barrier), count,
                   __ATOMIC_RELAXED);
  __atomic_store_n(blox7_barrier_state(barrier), kBarrierWaitState,
                   __ATOMIC_RELAXED);
  __atomic_store_n(blox7_barrier_wait_count(barrier), 0u, __ATOMIC_RELAXED);
  return 0;
}

int blox7_pthread_barrier_destroy(pthread_barrier_t* barrier) {
  if (barrier == nullptr || blox7_is_low_bionic_pointer(barrier)) {
    return EINVAL;
  }
  if (__atomic_load_n(blox7_barrier_init_count(barrier),
                      __ATOMIC_RELAXED) == 0) {
    return EINVAL;
  }
  uint32_t* state = blox7_barrier_state(barrier);
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierReleaseState) {
    (void)blox7_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierReleaseState,
                                nullptr, 0);
  }
  if (__atomic_load_n(blox7_barrier_wait_count(barrier),
                      __ATOMIC_RELAXED) != 0) {
    return EBUSY;
  }
  __atomic_store_n(blox7_barrier_init_count(barrier), 0u, __ATOMIC_RELAXED);
  return 0;
}

int blox7_pthread_barrier_wait(pthread_barrier_t* barrier) {
  if (barrier == nullptr || blox7_is_low_bionic_pointer(barrier)) {
    return EINVAL;
  }
  const uint32_t init_count =
      __atomic_load_n(blox7_barrier_init_count(barrier), __ATOMIC_RELAXED);
  if (init_count == 0) {
    return EINVAL;
  }
  uint32_t* state = blox7_barrier_state(barrier);
  uint32_t* wait_count = blox7_barrier_wait_count(barrier);
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierReleaseState) {
    (void)blox7_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierReleaseState,
                                nullptr, 0);
  }
  uint32_t prev = __atomic_load_n(wait_count, __ATOMIC_RELAXED);
  for (;;) {
    if (prev >= init_count) {
      return EINVAL;
    }
    if (__atomic_compare_exchange_n(wait_count, &prev, prev + 1u, true,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      break;
    }
  }
  int result = 0;
  if (prev + 1u == init_count) {
    result = PTHREAD_BARRIER_SERIAL_THREAD;
    if (prev != 0) {
      __atomic_store_n(state, kBarrierReleaseState, __ATOMIC_RELEASE);
      (void)blox7_bionic_futex(state, FUTEX_WAKE_PRIVATE, prev, nullptr, 0);
    }
  } else {
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierWaitState) {
      (void)blox7_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierWaitState,
                                  nullptr, 0);
    }
  }
  if (__atomic_fetch_sub(wait_count, 1u, __ATOMIC_RELEASE) == 1u) {
    __atomic_store_n(state, kBarrierWaitState, __ATOMIC_RELEASE);
    (void)blox7_bionic_futex(state, FUTEX_WAKE_PRIVATE, init_count, nullptr,
                                0);
  }
  return result;
}
#ifndef BLOX7_WIN32
int __poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fds_len) {
  if (nfds > fds_len / sizeof(struct pollfd)) {
    errno = ERANGE;
    nfds = static_cast<nfds_t>(fds_len / sizeof(struct pollfd));
  }
  return CallHostPoll(fds, nfds, timeout);
}
#endif
#ifndef BLOX7_WIN32
ssize_t __sendto_chk(int socket_fd, const void* buf, size_t len, size_t buf_len,
                     int flags, const struct sockaddr* dest,
                     socklen_t dest_len) {
  if (len > buf_len) {
    errno = ERANGE;
    len = buf_len;
  }
  return CallHostSendto(socket_fd, buf, len, flags, dest, dest_len);
}
#endif
#ifndef BLOX7_WIN32
void __FD_SET_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return;
  }
  FD_SET(fd, set);
}
#endif
#ifndef BLOX7_WIN32
void __FD_CLR_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return;
  }
  FD_CLR(fd, set);
}
#endif
#ifndef BLOX7_WIN32
int __FD_ISSET_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return 0;
  }
  return FD_ISSET(fd, set);
}
#endif
extern void* blox7_gameactivity_on_start_native;
extern void* blox7_gameactivity_on_resume_native;
extern void* blox7_gameactivity_on_surface_created_native;
extern void* blox7_gameactivity_on_surface_changed_native;
extern void* blox7_gameactivity_on_surface_redraw_needed_native;
extern void* blox7_gameactivity_on_trim_memory_native;
}  // extern "C"
