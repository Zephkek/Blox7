#ifndef BLOX7_RUNTIME_PLATFORM_CACHE_MIGRATION_H_
#define BLOX7_RUNTIME_PLATFORM_CACHE_MIGRATION_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

class Environment;
class RuntimePaths;

std::string BuildPlatformProfileRevision(std::string_view device_profile,
                                         bool touch_enabled, bool mouse_enabled,
                                         bool keyboard_enabled);

struct PlatformCacheMigrationResult {
  bool transitioned = false;
  bool app_storage_found = false;
  bool app_storage_updated = false;
  std::filesystem::path app_storage_file;
  std::filesystem::path fingerprint_file;
  std::string previous_revision;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

PlatformCacheMigrationResult MigratePlatformProfileCaches(
    const Environment& environment, const RuntimePaths& paths,
    std::string_view desired_revision);

struct RobloxThemeCacheResult {
  std::optional<bool> dark_theme;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

RobloxThemeCacheResult ReadRobloxThemeCache(
    const std::filesystem::path& app_storage_file,
    std::int64_t authenticated_user_id);

bool ApplyRobloxThemeCacheOverride(
    const std::filesystem::path& app_storage_file,
    std::int64_t authenticated_user_id, bool dark_theme, std::string* error);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_PLATFORM_CACHE_MIGRATION_H_
