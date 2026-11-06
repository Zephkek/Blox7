#ifndef BLOX7_COMPAT_BIONIC_ABI_EXPORTS_H_
#define BLOX7_COMPAT_BIONIC_ABI_EXPORTS_H_

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "compat/bionic_stdio_runtime.h"

extern volatile uintptr_t g_blox7_abort_libroblox_base;

extern "C" {
int blox7_system_property_get(const char* name, char* value);
void blox7_import_trace_once(const char* what, const char* question, const char* answer,
                                const void* caller);
unsigned long blox7_traced_getauxval(unsigned long type);
long blox7_traced_sysconf(int name);
void blox7_set_traced_dl_iterate_phdr(void* real);
int blox7_traced_dl_iterate_phdr(int (*callback)(dl_phdr_info*, size_t, void*), void* data);
int blox7_traced_dladdr(const void* address, Dl_info* info);
}

namespace blox7::compat {

void SetLegacyBionicDiagnosticsEnabled(bool enabled);

}  // namespace blox7::compat

using Blox7BionicMutexAttr = int64_t;

struct Blox7BionicPthreadAttr {
  std::uint32_t flags;
  void* stack_base;
  std::size_t stack_size;
  std::size_t guard_size;
  std::int32_t sched_policy;
  std::int32_t sched_priority;
  char reserved[16];
};
static_assert(sizeof(Blox7BionicPthreadAttr) == 56);

inline constexpr std::uint32_t kBlox7BionicAttrFlagDetached = 0x1;
inline constexpr std::uint32_t kBlox7BionicAttrFlagUserStack = 0x2;

extern "C" {

void blox7_set_current_jni_env(void* env);
void* blox7_get_current_jni_env();

[[noreturn]] void blox7_abort();
int __system_property_get(const char* name, char* value);

void* __memcpy_chk(void* dst, const void* src, size_t count, size_t dst_len);
char* __strcpy_chk(char* dst, const char* src, size_t dst_len);
int __vsprintf_chk(char* dst, int flags, size_t dst_len, const char* format,
                   va_list args);
int __vsnprintf_chk(char* dst, size_t count, int flags, size_t dst_len,
                    const char* format, va_list args);
ssize_t __read_chk(int fd, void* buf, size_t count, size_t buf_len);

int blox7_pthread_condattr_init(pthread_condattr_t* attr);
int blox7_pthread_condattr_destroy(pthread_condattr_t* attr);
int blox7_pthread_condattr_setclock(pthread_condattr_t* attr,
                                       clockid_t clock_id);
int blox7_pthread_cond_init(pthread_cond_t* cond,
                               const pthread_condattr_t* attr);
int blox7_pthread_cond_destroy(pthread_cond_t* cond);
int blox7_pthread_cond_signal(pthread_cond_t* cond);
int blox7_pthread_cond_broadcast(pthread_cond_t* cond);
int blox7_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int blox7_pthread_cond_timedwait(pthread_cond_t* cond,
                                    pthread_mutex_t* mutex,
                                    const timespec* abstime);
int blox7_pthread_mutexattr_init(Blox7BionicMutexAttr* attr);
int blox7_pthread_mutexattr_destroy(Blox7BionicMutexAttr* attr);
int blox7_pthread_mutexattr_settype(Blox7BionicMutexAttr* attr, int type);
int blox7_pthread_mutex_init(pthread_mutex_t* mutex,
                                const Blox7BionicMutexAttr* attr);
int blox7_pthread_mutex_destroy(pthread_mutex_t* mutex);
int blox7_pthread_mutex_lock(pthread_mutex_t* mutex);
int blox7_pthread_mutex_trylock(pthread_mutex_t* mutex);
int blox7_pthread_mutex_unlock(pthread_mutex_t* mutex);
int blox7_pthread_once(pthread_once_t* once_control,
                          void (*init_routine)(void));
int blox7_pthread_spin_init(pthread_spinlock_t* lock, int pshared);
int blox7_pthread_spin_destroy(pthread_spinlock_t* lock);
int blox7_pthread_spin_lock(pthread_spinlock_t* lock);
int blox7_pthread_spin_trylock(pthread_spinlock_t* lock);
int blox7_pthread_spin_unlock(pthread_spinlock_t* lock);
int blox7_pthread_barrier_init(pthread_barrier_t* barrier,
                                  const pthread_barrierattr_t* attr,
                                  unsigned count);
int blox7_pthread_barrier_destroy(pthread_barrier_t* barrier);
int blox7_pthread_barrier_wait(pthread_barrier_t* barrier);

int blox7_pthread_attr_init(Blox7BionicPthreadAttr* attr);
int blox7_pthread_attr_destroy(Blox7BionicPthreadAttr* attr);
int blox7_pthread_attr_setstacksize(Blox7BionicPthreadAttr* attr,
                                       size_t stack_size);
int blox7_pthread_attr_setdetachstate(Blox7BionicPthreadAttr* attr,
                                         int detach_state);
int blox7_pthread_attr_setschedparam(Blox7BionicPthreadAttr* attr,
                                        const struct sched_param* parameters);
int blox7_pthread_getattr_np(pthread_t thread,
                                Blox7BionicPthreadAttr* attr);
int blox7_pthread_attr_getstack(const Blox7BionicPthreadAttr* attr,
                                   void** stack_base, size_t* stack_size);

extern void* blox7_gameactivity_on_start_native;
extern void* blox7_gameactivity_on_resume_native;
extern void* blox7_gameactivity_on_surface_created_native;
extern void* blox7_gameactivity_on_surface_changed_native;
extern void* blox7_gameactivity_on_surface_redraw_needed_native;

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_ABI_EXPORTS_H_
