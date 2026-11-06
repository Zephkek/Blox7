#ifndef BLOX7_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_
#define BLOX7_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace blox7::compat {

inline constexpr char kFmodDeviceListsSelectContract[] =
    "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x50\x41"
    "\x89\xf7\x49\x89\xfe\x4c\x8b\x25\x00\x00\x00\x00\x49\x8b\x04\x24"
    "\x48\x89\x45\xd8\x80\x3d\x00\x00\x00\x00\x00\x74\x25\x49\x8b\x04"
    "\x24\x48\x3b\x45\xd8\x0f\x85\x00\x00\x00\x00\x4c\x89\xf7\x44\x89"
    "\xfe\x48\x83\xc4\x50\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xe9\x00\x00"
    "\x00\x00\x49\x8b\x06\x4c\x89\xf7\xff\x50\x28\x44\x39\xf8\x0f\x8e"
    "\x00\x00\x00\x00\x31\xdb\x45\x85\xff\x41\x0f\x4f\xdf\x49\x8b\x7e"
    "\x28\x89\xde\xe8\x00\x00\x00\x00";
inline constexpr std::size_t kFmodDeviceListsSelectContractSize =
    sizeof(kFmodDeviceListsSelectContract) - 1;

// 2.743 tests the flag through a register now, same code 3 bytes later
inline constexpr char kFmodDeviceListsSelectIndirectFlagContract[] =
    "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x50\x41"
    "\x89\xf7\x49\x89\xfe\x4c\x8b\x25\x00\x00\x00\x00\x49\x8b\x04\x24"
    "\x48\x89\x45\xd8\x48\x8d\x05\x00\x00\x00\x00\x80\x38\x00\x74\x25"
    "\x49\x8b\x04\x24\x48\x3b\x45\xd8\x0f\x85\x00\x00\x00\x00\x4c\x89"
    "\xf7\x44\x89\xfe\x48\x83\xc4\x50\x5b\x41\x5c\x41\x5e\x41\x5f\x5d"
    "\xe9\x00\x00\x00\x00\x49\x8b\x06\x4c\x89\xf7\xff\x50\x28\x44\x39"
    "\xf8\x0f\x8e\x00\x00\x00\x00\x31\xdb\x45\x85\xff\x41\x0f\x4f\xdf"
    "\x49\x8b\x7e\x28\x89\xde\xe8\x00\x00\x00\x00";
inline constexpr std::size_t kFmodDeviceListsSelectIndirectFlagContractSize =
    sizeof(kFmodDeviceListsSelectIndirectFlagContract) - 1;

// 2.746 wants two flags now (mov al, [a]; or al, [b]), so everything after is 8 bytes later
inline constexpr char kFmodDeviceListsSelectTwoFlagContract[] =
    "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x50\x41"
    "\x89\xf7\x49\x89\xfe\x4c\x8b\x25\x00\x00\x00\x00\x49\x8b\x04\x24"
    "\x48\x89\x45\xd8\x48\x8d\x05\x00\x00\x00\x00\x8a\x00\x48\x8d\x0d"
    "\x00\x00\x00\x00\x0a\x01\x74\x25\x49\x8b\x04\x24\x48\x3b\x45\xd8"
    "\x0f\x85\x00\x00\x00\x00\x4c\x89\xf7\x44\x89\xfe\x48\x83\xc4\x50"
    "\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xe9\x00\x00\x00\x00\x49\x8b\x06"
    "\x4c\x89\xf7\xff\x50\x28\x44\x39\xf8\x0f\x8e\x00\x00\x00\x00\x31"
    "\xdb\x45\x85\xff\x41\x0f\x4f\xdf\x49\x8b\x7e\x28\x89\xde\xe8\x00"
    "\x00\x00\x00";
inline constexpr std::size_t kFmodDeviceListsSelectTwoFlagContractSize =
    sizeof(kFmodDeviceListsSelectTwoFlagContract) - 1;
inline constexpr std::size_t kFmodDeviceListsSelectContractMaximumSize =
    kFmodDeviceListsSelectTwoFlagContractSize;

inline bool MatchesFmodSelectContract(
    std::string_view code, const char* contract, std::size_t size,
    std::initializer_list<std::size_t> relocated_starts) {
  if (code.size() < size) return false;
  for (std::size_t index = 0; index < size; ++index) {
    bool relocated = false;
    for (const std::size_t start : relocated_starts) {
      if (index >= start && index < start + 4) relocated = true;
    }
    if (!relocated && code[index] != contract[index]) return false;
  }
  return true;
}

inline bool HasFmodDeviceListsSelectContract(std::string_view code) {
  return MatchesFmodSelectContract(code, kFmodDeviceListsSelectContract,
                                   kFmodDeviceListsSelectContractSize,
                                   {24, 38, 55, 78, 96, 116}) ||
         MatchesFmodSelectContract(
             code, kFmodDeviceListsSelectIndirectFlagContract,
             kFmodDeviceListsSelectIndirectFlagContractSize,
             {24, 39, 58, 81, 99, 119}) ||
         MatchesFmodSelectContract(
             code, kFmodDeviceListsSelectTwoFlagContract,
             kFmodDeviceListsSelectTwoFlagContractSize,
             {24, 39, 48, 66, 89, 107, 127});
}

// mic half of the layout-2 vtable, same bytes 2.736 through 2.746
inline constexpr std::array<std::string_view, 4> kFmodInputMethodContracts = {
    std::string_view("\x48\x8b\x47\x60\x48\x2b\x47\x58\x48\xc1\xe8\x07\xc3",
                     13),
    std::string_view("\x55\x48\x89\xe5\x41\x56\x53\x48"
                     "\x89\xfb\x31\xc0\x85\xd2\x0f\x4f",
                     16),
    std::string_view("\x48\x8b\x47\x58\x48\x8b\x4f\x60\x48\x39\xc8\x74", 12),
    std::string_view("\x55\x48\x89\xe5\x41\x57\x41\x56"
                     "\x41\x54\x53\x48\x81\xec\x80\x00",
                     16),
};

inline bool HasFmodInputMethodContract(std::size_t index,
                                       std::string_view code) {
  return index < kFmodInputMethodContracts.size() &&
         code.size() >= kFmodInputMethodContracts[index].size() &&
         code.substr(0, kFmodInputMethodContracts[index].size()) ==
             kFmodInputMethodContracts[index];
}

}  // namespace blox7::compat

#endif  // BLOX7_COMPAT_FMOD_OUTPUT_DEVICE_CONTRACT_H_
