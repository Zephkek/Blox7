#include "blox7/audio/roblox_output_device_bridge.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio/roblox_output_device_bridge_internal.h"
#include "audio/native_input_capture.h"
#include "compat/fmod_output_device_contract.h"
#include "linker/linker.h"

namespace blox7::audio {
namespace {

constexpr std::size_t kOutputCountVtableIndex = 5;
constexpr std::size_t kOutputInfoVtableIndex = 6;
constexpr std::size_t kGuestStringSize = 24;
constexpr std::size_t kGuestDeviceInfoSize = kGuestStringSize * 2 + 1;
constexpr std::size_t kStringConstructorContractSize = 24;

std::atomic<RobloxOutputDeviceBridge*> g_active_bridge{nullptr};

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool IsValidElfImage(std::uintptr_t image_base) {
  if (image_base == 0) {
    return false;
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  return std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
         header->e_ident[EI_CLASS] == ELFCLASS64 && header->e_phoff != 0 &&
         header->e_phnum != 0;
}

bool IsProgramHeaderRange(std::uintptr_t image_base, std::uintptr_t rva,
                          std::size_t size, std::uint32_t type,
                          std::uint32_t required_flags) {
  if (!IsValidElfImage(image_base) || size == 0 ||
      rva > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  const auto* program_headers = reinterpret_cast<const Elf64_Phdr*>(
      image_base + static_cast<std::uintptr_t>(header->e_phoff));
  const std::uintptr_t range_end = rva + size;
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr& segment = program_headers[index];
    if (segment.p_type != type ||
        (segment.p_flags & required_flags) != required_flags ||
        segment.p_vaddr >
            std::numeric_limits<std::uintptr_t>::max() - segment.p_memsz) {
      continue;
    }
    const std::uintptr_t segment_end =
        static_cast<std::uintptr_t>(segment.p_vaddr + segment.p_memsz);
    if (rva >= segment.p_vaddr && range_end <= segment_end) {
      return true;
    }
  }
  return false;
}

bool IsExecutableImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                            std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_LOAD, PF_R | PF_X);
}

bool IsRelroImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                       std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_GNU_RELRO, 0);
}

std::array<std::uintptr_t, 4> InputMethodsFromVtable(
    std::uintptr_t image_base,
    const compat::FmodOutputDeviceBridgeProfile& profile) {
  const auto indexes = profile.input_vtable_indexes();
  std::array<std::uintptr_t, 4> rvas{};
  for (std::size_t i = 0; i < indexes.size(); ++i) {
    const std::uintptr_t slot_rva =
        profile.vtable_rva + indexes[i] * sizeof(std::uintptr_t);
    if (!IsRelroImageRange(image_base, slot_rva, sizeof(std::uintptr_t))) {
      return {};
    }
    const std::uintptr_t method =
        *reinterpret_cast<const std::uintptr_t*>(image_base + slot_rva);
    const std::size_t size = compat::kFmodInputMethodContracts[i].size();
    if (method <= image_base ||
        !IsExecutableImageRange(image_base, method - image_base, size) ||
        !compat::HasFmodInputMethodContract(
            i, std::string_view(reinterpret_cast<const char*>(method), size))) {
      return {};
    }
    rvas[i] = method - image_base;
  }
  return rvas;
}

bool SetVtableWritable(std::uintptr_t* vtable, std::size_t select_index,
                       bool writable) {
  if (vtable == nullptr) {
    return false;
  }
  const long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return false;
  }
  const std::uintptr_t page_size = static_cast<std::uintptr_t>(page_size_value);
  const std::uintptr_t first =
      reinterpret_cast<std::uintptr_t>(vtable + 4);
  const std::uintptr_t last =
      reinterpret_cast<std::uintptr_t>(vtable + select_index + 1);
  if (last <= first) {
    return false;
  }
  const std::uintptr_t page_begin = first - first % page_size;
  if (last > std::numeric_limits<std::uintptr_t>::max() - page_size + 1) {
    return false;
  }
  const std::uintptr_t page_end =
      ((last + page_size - 1) / page_size) * page_size;
  const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
  return mprotect(reinterpret_cast<void*>(page_begin), page_end - page_begin,
                  protection) == 0;
}

template <typename Function>
std::uintptr_t FunctionAddress(Function function) {
  return reinterpret_cast<std::uintptr_t>(function);
}

HostAudioDirection RouteDirection(bool input) {
  return input ? HostAudioDirection::kInput : HostAudioDirection::kOutput;
}

void LogRoutes(HostAudioRoutes& routes, HostAudioDirection direction,
               bool list) {
  const std::size_t count = routes.Count(direction);
  HostAudioRoute selected;
  const bool known =
      count != 0 && routes.Route(direction, routes.Current(direction),
                                 &selected);
  std::fprintf(stderr, "  [audio-menu] exposed %zu host %s routes; selected=%s\n",
               count,
               direction == HostAudioDirection::kOutput ? "output" : "input",
               known ? selected.name.c_str() : "disabled");
  for (std::size_t index = 0; list && index < count; ++index) {
    HostAudioRoute route;
    if (routes.Route(direction, index, &route)) {
      std::fprintf(stderr, "    [audio-menu] %s\n", route.name.c_str());
    }
  }
}

}  // namespace

RobloxOutputDeviceBridge::RobloxOutputDeviceBridge() = default;

namespace internal {

bool HasExpectedFmodStringConstructorContract(const std::uint8_t* code,
                                              std::size_t size) {
  constexpr std::array<std::uint8_t, kStringConstructorContractSize>
      kExpectedPrefix = {
          0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
          0x41, 0x54, 0x53, 0x48, 0x89, 0xd3, 0x49, 0x89,
          0xf6, 0x49, 0x89, 0xff, 0x48, 0x83, 0xfa, 0x16,
      };
  return code != nullptr && size >= kExpectedPrefix.size() &&
         std::memcmp(code, kExpectedPrefix.data(), kExpectedPrefix.size()) == 0;
}

bool HasExpectedFmodOutputDeviceVtable(
    const std::uintptr_t* vtable, std::uintptr_t image_base,
    const compat::FmodOutputDeviceBridgeProfile& profile) {
  const std::size_t kCurrentOutputVtableIndex = profile.current_vtable_index();
  const std::size_t kSelectOutputVtableIndex = profile.select_vtable_index();
  if (vtable == nullptr || image_base == 0 || !profile.valid_vtable_layout())
    return false;
  if (profile.has_input_devices()) {
    const auto indexes = profile.input_vtable_indexes();
    for (std::size_t i = 0; i < indexes.size(); ++i) {
      if (profile.input_method_rvas[i] == 0 ||
          vtable[indexes[i]] != image_base + profile.input_method_rvas[i])
        return false;
    }
  }
  return vtable[kOutputCountVtableIndex] ==
             image_base + profile.count_method_rva &&
         vtable[kOutputInfoVtableIndex] ==
             image_base + profile.info_method_rva &&
         vtable[kCurrentOutputVtableIndex] ==
             image_base + profile.current_method_rva &&
         vtable[kSelectOutputVtableIndex] ==
             image_base + profile.select_method_rva;
}

}  // namespace internal

RobloxOutputDeviceBridge::~RobloxOutputDeviceBridge() { Shutdown(); }

Status RobloxOutputDeviceBridge::Install(const compat::BuildProfile& profile) {
  if (installed_) {
    return FailedPrecondition("Roblox output-device bridge is already armed");
  }
  if (!profile.fmod_output_device_bridge.has_value()) {
    return Status::Ok();
  }
  if (!profile.allow_host_abi_bridges) {
    return FailedPrecondition(
        "FMOD output-device profile requires host ABI bridges");
  }
  if (!profile.fmod_output_device_bridge->valid_vtable_layout()) {
    return FailedPrecondition("unsupported FMOD output-device vtable layout");
  }

  RobloxOutputDeviceBridge* expected = nullptr;
  if (!g_active_bridge.compare_exchange_strong(expected, this,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    return FailedPrecondition(
        "another Roblox output-device bridge owns the process");
  }
  profile_ = *profile.fmod_output_device_bridge;
  if (!linker::SetAndroidLibraryLoadObserver(&ObserveAndroidLibrary, this)) {
    g_active_bridge.store(nullptr, std::memory_order_release);
    profile_ = {};
    return FailedPrecondition(
        "Android library load observer already has an owner");
  }
  installed_ = true;
  std::fprintf(stderr,
               "  [audio-menu] host output bridge armed for Build ID %s\n",
               profile.elf_build_id.c_str());
  return Status::Ok();
}

void RobloxOutputDeviceBridge::Shutdown() {
  if (!installed_) {
    return;
  }
  linker::ClearAndroidLibraryLoadObserver(this);
  routes_.Stop();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && !RestoreVtableLocked()) {
      std::fprintf(stderr,
                   "  [audio-menu] failed to restore FmodAudioDevice "
                   "vtable protection\n");
    }
    std::atomic_store_explicit(&g_native_capture,
                              std::shared_ptr<NativeInputCapture>{},
                              std::memory_order_release);
    if (capture_) capture_->Shutdown();
    capture_.reset();
    active_ = false;
    input_enabled_ = false;
    string_constructor_ = nullptr;
    library_base_ = 0;
    vtable_ = nullptr;
  }
  RobloxOutputDeviceBridge* expected = this;
  (void)g_active_bridge.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
  profile_ = {};
  installed_ = false;
}

bool RobloxOutputDeviceBridge::ObserveAndroidLibrary(
    void* context, std::string_view logical_name, std::uintptr_t image_base) {
  if (logical_name != "libroblox") {
    return true;
  }
  auto* bridge = static_cast<RobloxOutputDeviceBridge*>(context);
  if (bridge == nullptr) {
    return false;
  }
  const Status status = bridge->Activate(image_base);
  if (!status.ok()) {
    std::fprintf(stderr, "  [audio-menu] activation failed: %s\n",
                 status.message().c_str());
    return false;
  }
  return true;
}

Status RobloxOutputDeviceBridge::Activate(std::uintptr_t image_base) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!installed_) {
    return FailedPrecondition("Roblox output-device bridge is not armed");
  }
  if (active_) {
    return image_base == library_base_
               ? Status::Ok()
               : FailedPrecondition(
                     "Roblox output-device bridge saw a second image");
  }
  if (!IsValidElfImage(image_base)) {
    return InvalidArgument("libroblox image base is not a valid ELF64 image");
  }
  const std::uintptr_t largest_rva =
      std::max({profile_.vtable_rva, profile_.string_constructor_rva,
                profile_.count_method_rva, profile_.info_method_rva,
                profile_.current_method_rva, profile_.select_method_rva});
  if (image_base > std::numeric_limits<std::uintptr_t>::max() - largest_rva) {
    return InvalidArgument("FMOD output-device profile address overflows");
  }

  if (!profile_.has_input_devices() && profile_.vtable_layout_version == 2) {
    profile_.input_method_rvas = InputMethodsFromVtable(image_base, profile_);
    if (profile_.has_input_devices()) {
      std::fprintf(stderr,
                   "  [audio-menu] profile had no input methods; found them "
                   "in the device vtable\n");
    }
  }
  const char *input_target = std::getenv("BLOX7_AUDIO_INPUT_DEVICE");
  input_enabled_ = profile_.has_input_devices() &&
                   (input_target == nullptr ||
                    std::string_view(input_target) != "disabled");
  Status status = routes_.Start(input_enabled_);
  if (!status.ok()) {
    input_enabled_ = false;
    return status;
  }

  library_base_ = image_base;
  vtable_ = reinterpret_cast<std::uintptr_t*>(image_base + profile_.vtable_rva);
  string_constructor_ =
      reinterpret_cast<void*>(image_base + profile_.string_constructor_rva);
  status = PatchVtableLocked();
  if (!status.ok()) {
    routes_.Stop();
    input_enabled_ = false;
    string_constructor_ = nullptr;
    library_base_ = 0;
    vtable_ = nullptr;
    return status;
  }
  active_ = true;
  LogRoutes(routes_, HostAudioDirection::kOutput, true);
  if (profile_.has_input_devices()) {
    LogRoutes(routes_, HostAudioDirection::kInput, false);
  }
  return Status::Ok();
}

Status RobloxOutputDeviceBridge::PatchVtableLocked() {
  const std::size_t kCurrentOutputVtableIndex = profile_.current_vtable_index();
  const std::size_t kSelectOutputVtableIndex = profile_.select_vtable_index();
  const auto capture_indexes = profile_.capture_vtable_indexes();
  if (profile_.vtable_rva >
      std::numeric_limits<std::uintptr_t>::max() -
          (kSelectOutputVtableIndex + 1) * sizeof(std::uintptr_t)) {
    return FailedPrecondition("FMOD output-device vtable range overflows");
  }
  const std::uintptr_t first_slot_rva =
      profile_.vtable_rva + 4 * sizeof(std::uintptr_t);
  const std::size_t slot_range_size =
      (kSelectOutputVtableIndex - 4 + 1) *
      sizeof(std::uintptr_t);
  if (!IsRelroImageRange(library_base_, first_slot_rva, slot_range_size) ||
      !IsExecutableImageRange(library_base_, profile_.string_constructor_rva,
                              kStringConstructorContractSize) ||
      !IsExecutableImageRange(library_base_, profile_.count_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.info_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.current_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.select_method_rva, 1)) {
    return FailedPrecondition(
        "FMOD output-device profile points outside verified image ranges");
  }
  if (profile_.has_input_devices()) {
    for (const auto rva : profile_.input_method_rvas) {
      if (rva == 0 ||
          rva > std::numeric_limits<std::uintptr_t>::max() - library_base_ ||
          !IsExecutableImageRange(library_base_, rva, 1))
        return FailedPrecondition(
            "FMOD input profile points outside executable image");
    }
  }
  if (!internal::HasExpectedFmodOutputDeviceVtable(vtable_, library_base_,
                                                   profile_)) {
    return FailedPrecondition(
        "FmodAudioDevice output vtable does not match the Build-ID profile");
  }
  const auto* constructor_code = reinterpret_cast<const std::uint8_t*>(
      library_base_ + profile_.string_constructor_rva);
  if (!internal::HasExpectedFmodStringConstructorContract(
          constructor_code, kStringConstructorContractSize)) {
    return FailedPrecondition(
        "FmodAudioDevice string ABI contract validation failed");
  }

  if (profile_.has_input_devices()) {
    for (std::size_t i = 0; i < capture_indexes.size(); ++i) {
      const auto method = vtable_[capture_indexes[i]];
      if (method < library_base_ ||
          !IsExecutableImageRange(library_base_, method - library_base_, 1) ||
          std::find(profile_.input_method_rvas.begin(),
                    profile_.input_method_rvas.end(), method - library_base_) !=
              profile_.input_method_rvas.end() ||
          std::find(original_capture_methods_.begin(),
                    original_capture_methods_.begin() + i, method) !=
              original_capture_methods_.begin() + i)
        return FailedPrecondition("native input capture vtable ABI mismatch");
      original_capture_methods_[i] = method;
    }
    capture_ = std::make_shared<NativeInputCapture>(
        NativeInputCapture::SinkAbi{
            library_base_, &IsRelroImageRange, &IsExecutableImageRange,
            reinterpret_cast<NativeInputCapture::OriginalLatency>(
                original_capture_methods_[1])},
        input_enabled_);
  }

  original_methods_ = {
      vtable_[kOutputCountVtableIndex],
      vtable_[kOutputInfoVtableIndex],
      vtable_[kCurrentOutputVtableIndex],
      vtable_[kSelectOutputVtableIndex],
  };
  if (profile_.has_input_devices()) {
    const auto indexes = profile_.input_vtable_indexes();
    for (std::size_t i = 0; i < indexes.size(); ++i)
      original_methods_[4 + i] = vtable_[indexes[i]];
  }
  if (!SetVtableWritable(vtable_, kSelectOutputVtableIndex, true)) {
    return Status::Error(StatusCode::kPlatformError,
                         "cannot make FmodAudioDevice vtable writable");
  }

  __atomic_store_n(&vtable_[kOutputCountVtableIndex],
                   FunctionAddress(&GetOutputDeviceCount), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kOutputInfoVtableIndex],
                   FunctionAddress(&GetOutputDeviceInfo), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kCurrentOutputVtableIndex],
                   FunctionAddress(&GetCurrentOutputDevice), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kSelectOutputVtableIndex],
                   FunctionAddress(&SetCurrentOutputDevice), __ATOMIC_RELEASE);
  if (profile_.has_input_devices()) {
    const auto indexes = profile_.input_vtable_indexes();
    const std::array<std::uintptr_t, 4> methods = {
        FunctionAddress(&GetInputDeviceCount),
        FunctionAddress(&GetInputDeviceInfo),
        FunctionAddress(&GetCurrentInputDevice),
        FunctionAddress(&SetCurrentInputDevice)};
    for (std::size_t i = 0; i < indexes.size(); ++i)
      __atomic_store_n(&vtable_[indexes[i]], methods[i], __ATOMIC_RELEASE);
  }
  if (capture_) {
    std::atomic_store_explicit(&g_native_capture, capture_,
                              std::memory_order_release);
    const std::array<std::uintptr_t, 6> methods{
        FunctionAddress(&NativeInputCapture::Format),
        FunctionAddress(&NativeInputCapture::Latency),
        FunctionAddress(&NativeInputCapture::Start),
        FunctionAddress(&NativeInputCapture::Stop),
        FunctionAddress(&NativeInputCapture::Poll),
        FunctionAddress(&NativeInputCapture::Recording)};
    for (std::size_t i = 0; i < capture_indexes.size(); ++i)
      __atomic_store_n(&vtable_[capture_indexes[i]], methods[i], __ATOMIC_RELEASE);
  }
  if (!SetVtableWritable(vtable_, kSelectOutputVtableIndex, false)) {
    __atomic_store_n(&vtable_[kOutputCountVtableIndex], original_methods_[0],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kOutputInfoVtableIndex], original_methods_[1],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kCurrentOutputVtableIndex], original_methods_[2],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kSelectOutputVtableIndex], original_methods_[3],
                     __ATOMIC_RELEASE);
    if (profile_.has_input_devices()) {
      const auto indexes = profile_.input_vtable_indexes();
      for (std::size_t i = 0; i < indexes.size(); ++i)
        __atomic_store_n(&vtable_[indexes[i]], original_methods_[4 + i],
                         __ATOMIC_RELEASE);
    }
    if (capture_) {
      for (std::size_t i = 0; i < capture_indexes.size(); ++i)
        __atomic_store_n(&vtable_[capture_indexes[i]], original_capture_methods_[i],
                         __ATOMIC_RELEASE);
      std::atomic_store_explicit(&g_native_capture,
                                std::shared_ptr<NativeInputCapture>{},
                                std::memory_order_release);
      capture_->Shutdown();
      capture_.reset();
    }
    (void)SetVtableWritable(vtable_, kSelectOutputVtableIndex, false);
    return Status::Error(StatusCode::kPlatformError,
                         "cannot restore FmodAudioDevice RELRO protection");
  }
  return Status::Ok();
}

bool RobloxOutputDeviceBridge::RestoreVtableLocked() {
  const std::size_t kCurrentOutputVtableIndex = profile_.current_vtable_index();
  const std::size_t kSelectOutputVtableIndex = profile_.select_vtable_index();
  if (vtable_ == nullptr ||
      !SetVtableWritable(vtable_, kSelectOutputVtableIndex, true)) {
    return false;
  }
  __atomic_store_n(&vtable_[kOutputCountVtableIndex], original_methods_[0],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kOutputInfoVtableIndex], original_methods_[1],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kCurrentOutputVtableIndex], original_methods_[2],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kSelectOutputVtableIndex], original_methods_[3],
                   __ATOMIC_RELEASE);
  if (profile_.has_input_devices()) {
    const auto indexes = profile_.input_vtable_indexes();
    for (std::size_t i = 0; i < indexes.size(); ++i)
      __atomic_store_n(&vtable_[indexes[i]], original_methods_[4 + i],
                       __ATOMIC_RELEASE);
  }
  if (capture_) {
    const auto capture_indexes = profile_.capture_vtable_indexes();
    for (std::size_t i = 0; i < capture_indexes.size(); ++i)
      __atomic_store_n(&vtable_[capture_indexes[i]], original_capture_methods_[i],
                       __ATOMIC_RELEASE);
  }
  return SetVtableWritable(vtable_, kSelectOutputVtableIndex, false);
}

int RobloxOutputDeviceBridge::GetOutputDeviceCount(void* self) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  return bridge != nullptr ? bridge->DeviceCount() : 0;
}

void* RobloxOutputDeviceBridge::GetOutputDeviceInfo(void* result, void* self,
                                                    int index) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge == nullptr) {
    if (result != nullptr) {
      std::memset(result, 0, kGuestDeviceInfoSize);
    }
    return result;
  }
  return bridge->DeviceInfo(result, index);
}

int RobloxOutputDeviceBridge::GetCurrentOutputDevice(void* self) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  return bridge != nullptr ? bridge->CurrentDevice() : 0;
}

void RobloxOutputDeviceBridge::SetCurrentOutputDevice(void* self, int index) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge != nullptr) {
    bridge->SelectDevice(index);
  }
}

int RobloxOutputDeviceBridge::GetInputDeviceCount(void *) {
  auto *bridge = g_active_bridge.load(std::memory_order_acquire);
  return bridge ? bridge->DeviceCount(true) : 0;
}

void *RobloxOutputDeviceBridge::GetInputDeviceInfo(void *result, void *,
                                                   int index) {
  auto *bridge = g_active_bridge.load(std::memory_order_acquire);
  if (bridge)
    return bridge->DeviceInfo(result, index, true);
  if (result)
    std::memset(result, 0, kGuestDeviceInfoSize);
  return result;
}

int RobloxOutputDeviceBridge::GetCurrentInputDevice(void *) {
  auto *bridge = g_active_bridge.load(std::memory_order_acquire);
  return bridge ? bridge->CurrentDevice(true) : 0;
}

void RobloxOutputDeviceBridge::SetCurrentInputDevice(void *, int index) {
  auto *bridge = g_active_bridge.load(std::memory_order_acquire);
  if (bridge)
    bridge->SelectDevice(index, true);
}

int RobloxOutputDeviceBridge::DeviceCount(bool input) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ ? static_cast<int>(routes_.Count(RouteDirection(input))) : 0;
}

void *RobloxOutputDeviceBridge::DeviceInfo(void *result, int index,
                                           bool input) {
  if (result == nullptr) {
    return nullptr;
  }
  std::memset(result, 0, kGuestDeviceInfoSize);
  std::lock_guard<std::mutex> lock(mutex_);
  HostAudioRoute route;
  if (!active_ || index < 0 ||
      !routes_.Route(RouteDirection(input), static_cast<std::size_t>(index),
                     &route)) {
    return result;
  }
  ConstructGuestString(result, route.name);
  ConstructGuestString(static_cast<std::uint8_t*>(result) + kGuestStringSize,
                       route.guid);
  static_cast<std::uint8_t*>(result)[kGuestStringSize * 2] = 1;
  return result;
}

int RobloxOutputDeviceBridge::CurrentDevice(bool input) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ ? static_cast<int>(routes_.Current(RouteDirection(input)))
                 : 0;
}

void RobloxOutputDeviceBridge::SelectDevice(int index, bool input) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || index < 0) {
      return;
    }
  }
  routes_.Select(RouteDirection(input), static_cast<std::size_t>(index));
}

void RobloxOutputDeviceBridge::ConstructGuestString(
    void* destination, std::string_view value) const {
  using GuestStringConstructor = void* (*)(void*, const char*, std::size_t);
  auto constructor =
      reinterpret_cast<GuestStringConstructor>(string_constructor_);
  constructor(destination, value.data(), value.size());
}

}  // namespace blox7::audio
