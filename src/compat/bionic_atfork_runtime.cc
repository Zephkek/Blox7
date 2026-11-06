#include "compat/bionic_atfork_runtime.h"

#include <pthread.h>

namespace blox7::compat {

int RegisterBionicAtFork(BionicAtForkCallback prepare,
                         BionicAtForkCallback parent,
                         BionicAtForkCallback child,
                         void* dso_handle) noexcept {
  static_cast<void>(dso_handle);
  return pthread_atfork(prepare, parent, child);
}

}  // namespace blox7::compat

extern "C" int
blox7_bionic_register_atfork(blox7::compat::BionicAtForkCallback prepare,
                                blox7::compat::BionicAtForkCallback parent,
                                blox7::compat::BionicAtForkCallback child,
                                void* dso_handle) {
  return blox7::compat::RegisterBionicAtFork(prepare, parent, child,
                                                dso_handle);
}
