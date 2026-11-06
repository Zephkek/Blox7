#ifndef BLOX7_COMPAT_HOST_ABI_EXPERIMENT_H_
#define BLOX7_COMPAT_HOST_ABI_EXPERIMENT_H_

#include <cstddef>
#include <cstdint>

#include "compat/host_abi_profile.h"

namespace blox7::compat {

struct HostAbiBridgeTargets {
  void *allocate = nullptr;
  void *reallocate = nullptr;
  void *aligned_allocate = nullptr;
  void *free = nullptr;
  void *usable_size = nullptr;
  void *allocator_object_allocate = nullptr;
  void *null_vtable_stub = nullptr;
};

struct HostAbiExperimentOptions {
  bool install_allocator_bridges = true;
  bool seed_allocator_object = true;
  bool seed_empty_string = true;
  bool initialize_arena = true;
  bool seed_jni_singleton = true;
};

struct HostAbiExperimentResult {
  bool installed = false;
  bool uses_native_mimalloc = false;
  size_t bridge_entries_installed = 0;
  const char *error = nullptr;

  explicit operator bool() const noexcept { return installed; }
};

enum class NativeMimallocBootstrapStatus {
  kNotRequired,
  kInitialized,
  kFailed,
};

enum class NativePreJniBootstrapStatus {
  kNotRequired,
  kAlreadyInitialized,
  kInitialized,
  kFailed,
};

HostAbiExperimentResult
InstallHostAbiExperiment(uintptr_t library_base, const HostAbiProfile &profile,
                         const HostAbiBridgeTargets &targets,
                         const HostAbiExperimentOptions &options) noexcept;

bool InitializeHostAbiThread(uintptr_t library_base,
                             const HostAbiProfile &profile) noexcept;

NativeMimallocBootstrapStatus CompleteNativeMimallocConstructor(
    uintptr_t library_base, const HostAbiProfile &profile,
    size_t constructor_index) noexcept;

NativePreJniBootstrapStatus InitializeNativePreJniRegistry(
    uintptr_t library_base, const HostAbiProfile &profile) noexcept;

} // namespace blox7::compat

#endif // BLOX7_COMPAT_HOST_ABI_EXPERIMENT_H_
