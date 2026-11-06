#ifndef BLOX7_COMPAT_BIONIC_PTHREAD_KEY_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_PTHREAD_KEY_RUNTIME_H_

#include <pthread.h>

#include <cstddef>
#include <cstdint>

namespace blox7::compat {

constexpr size_t kBionicPthreadKeyCount = 130;
constexpr uint32_t kBionicPthreadKeyValidFlag = uint32_t{1} << 31;

using BionicPthreadKeyDestructor = void (*)(void*);

class BionicPthreadKeyRuntime {
 public:
  static BionicPthreadKeyRuntime& Instance() noexcept;

  int Create(pthread_key_t* key, BionicPthreadKeyDestructor destructor) noexcept;
  int Delete(pthread_key_t key) noexcept;
  void* Get(pthread_key_t key) noexcept;
  int Set(pthread_key_t key, const void* value) noexcept;

 private:
  BionicPthreadKeyRuntime() = default;
};

bool IsBionicPthreadKey(pthread_key_t key) noexcept;

}  // namespace blox7::compat

extern "C" {

int blox7_bionic_pthread_key_create(
    pthread_key_t* key, void (*destructor)(void*));
int blox7_bionic_pthread_key_delete(pthread_key_t key);
void* blox7_bionic_pthread_getspecific(pthread_key_t key);
int blox7_bionic_pthread_setspecific(pthread_key_t key,
                                        const void* value);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_PTHREAD_KEY_RUNTIME_H_
