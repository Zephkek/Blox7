#ifndef BLOX7_COMPAT_BIONIC_ATFORK_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_ATFORK_RUNTIME_H_

namespace blox7::compat {

using BionicAtForkCallback = void (*)(void);

int RegisterBionicAtFork(BionicAtForkCallback prepare,
                         BionicAtForkCallback parent,
                         BionicAtForkCallback child, void* dso_handle) noexcept;

}  // namespace blox7::compat

extern "C" {

int blox7_bionic_register_atfork(
    blox7::compat::BionicAtForkCallback prepare,
    blox7::compat::BionicAtForkCallback parent,
    blox7::compat::BionicAtForkCallback child, void* dso_handle);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_ATFORK_RUNTIME_H_
