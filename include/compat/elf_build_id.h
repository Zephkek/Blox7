#ifndef BLOX7_COMPAT_ELF_BUILD_ID_H_
#define BLOX7_COMPAT_ELF_BUILD_ID_H_

#include <string>
#include <string_view>

namespace blox7::compat {

struct BuildIdResult {
  std::string build_id;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

BuildIdResult ReadElfBuildId(const std::string& path);

bool IsValidBuildId(std::string_view build_id) noexcept;

}  // namespace blox7::compat

#endif  // BLOX7_COMPAT_ELF_BUILD_ID_H_
