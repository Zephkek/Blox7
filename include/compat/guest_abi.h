#ifndef BLOX7_COMPAT_GUEST_ABI_H_
#define BLOX7_COMPAT_GUEST_ABI_H_

#include <elf.h>
#include <string_view>

namespace blox7::compat {

#if defined(__aarch64__)
inline constexpr std::string_view kGuestAbi = "arm64-v8a";
inline constexpr std::string_view kGuestApkSplitName = "config.arm64_v8a";
inline constexpr std::string_view kGuestSplitApkFile =
    "split_config.arm64_v8a.apk";
inline constexpr std::string_view kGuestSplitApkHashKey = "arm64_v8a_split_apk";
inline constexpr std::string_view kGuestCpuName = "aarch64";
inline constexpr int kGuestElfMachine = EM_AARCH64;
#elif defined(__x86_64__)
inline constexpr std::string_view kGuestAbi = "x86_64";
inline constexpr std::string_view kGuestApkSplitName = "config.x86_64";
inline constexpr std::string_view kGuestSplitApkFile = "split_config.x86_64.apk";
inline constexpr std::string_view kGuestSplitApkHashKey = "x86_64_split_apk";
inline constexpr std::string_view kGuestCpuName = "x86_64";
inline constexpr int kGuestElfMachine = EM_X86_64;
#else
#error "blox7 only supports aarch64 and x86_64 hosts"
#endif

inline constexpr std::string_view kGuestLibApkEntryPrefix = "lib/";

}  // namespace blox7::compat

#endif  // BLOX7_COMPAT_GUEST_ABI_H_
