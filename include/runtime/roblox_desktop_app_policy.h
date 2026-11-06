#ifndef BLOX7_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_
#define BLOX7_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

struct DesktopAppPolicyResult {
  bool app_storage_created = false;
  bool updated = false;
  std::size_t normalized_policy_count = 0;
  std::string policy_json;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

DesktopAppPolicyResult ApplyDesktopAppPolicy(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& default_policy_file,
    std::int64_t authenticated_user_id, std::string_view theme_mode);

bool MergeDesktopAppPolicyClientSettingsOverride(std::string_view policy_json,
                                                 std::string_view base_json,
                                                 std::string* merged_json,
                                                 std::string* error);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_
