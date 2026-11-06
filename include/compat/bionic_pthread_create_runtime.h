#ifndef BLOX7_COMPAT_BIONIC_PTHREAD_CREATE_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_PTHREAD_CREATE_RUNTIME_H_

#include <pthread.h>
#include <sched.h>
#include <sys/types.h>

#include <cstddef>

namespace blox7::compat {

using NativeThreadInitializer = void (*)();

inline constexpr size_t kBionicLp64DefaultThreadStackSize =
    (1U * 1024U * 1024U) - (32U * 1024U);
static_assert(kBionicLp64DefaultThreadStackSize == 1015808U);

inline constexpr size_t kBionicLp64FallbackThreadStackSize = 2U * 1024U * 1024U;

void ConfigureBionicPthreadThreadInitializer(
    NativeThreadInitializer initializer) noexcept;

}  // namespace blox7::compat

struct Blox7BionicPthreadAttr;

extern "C" int blox7_bionic_pthread_create(
    pthread_t* thread, const Blox7BionicPthreadAttr* attr,
    void* (*start_routine)(void*), void* argument);

extern "C" int blox7_bionic_pthread_setschedparam(
    pthread_t thread, int policy, const struct sched_param* parameters);
extern "C" int blox7_bionic_sched_setscheduler(
    pid_t tid, int policy, const struct sched_param* parameters);

#endif  // BLOX7_COMPAT_BIONIC_PTHREAD_CREATE_RUNTIME_H_
