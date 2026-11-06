#ifndef BLOX7_COMPAT_BIONIC_LARGE_FILE_RUNTIME_H_
#define BLOX7_COMPAT_BIONIC_LARGE_FILE_RUNTIME_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>

namespace blox7::compat {

using BionicOff64 = int64_t;

ssize_t BionicPread64(int fd, void* buffer, size_t count,
                      BionicOff64 offset) noexcept;
ssize_t BionicPwrite64(int fd, const void* buffer, size_t count,
                       BionicOff64 offset) noexcept;
BionicOff64 BionicLseek64(int fd, BionicOff64 offset, int whence) noexcept;

}  // namespace blox7::compat

extern "C" {

ssize_t blox7_bionic_pread64(int fd, void* buffer, size_t count,
                                blox7::compat::BionicOff64 offset);
ssize_t blox7_bionic_pwrite64(int fd, const void* buffer, size_t count,
                                 blox7::compat::BionicOff64 offset);
blox7::compat::BionicOff64
blox7_bionic_lseek64(int fd, blox7::compat::BionicOff64 offset,
                        int whence);

}  // extern "C"

#endif  // BLOX7_COMPAT_BIONIC_LARGE_FILE_RUNTIME_H_
