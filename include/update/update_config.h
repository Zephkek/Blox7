#ifndef BLOX7_UPDATE_UPDATE_CONFIG_H_
#define BLOX7_UPDATE_UPDATE_CONFIG_H_

#include <filesystem>
#include <string>
#include <vector>

namespace blox7::update {

struct UpdateConfig {
  bool automatic = true;
  bool launch_after_update = false;
  std::string source = "apk-pure";
  std::string apk_directory;
  std::string github_repository = "SPDMTiahh/roblox-updates";
};

struct UpdateConfigResult {
  UpdateConfig config;
  bool file_loaded = false;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

UpdateConfigResult LoadUpdateConfig(const std::filesystem::path& path);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_UPDATE_CONFIG_H_
