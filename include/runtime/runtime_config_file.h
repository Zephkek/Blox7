#ifndef BLOX7_RUNTIME_RUNTIME_CONFIG_FILE_H_
#define BLOX7_RUNTIME_RUNTIME_CONFIG_FILE_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_config.h"

namespace blox7 {
namespace runtime {

struct RuntimeConfigLoadResult {
  RuntimeConfig config;
  bool file_loaded = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

RuntimeConfigLoadResult LoadRuntimeConfig(
    const Environment& environment, const std::filesystem::path& config_file);

bool ExportRuntimeConfigEnvironment(const RuntimeConfig& config,
                                    std::string* error = nullptr);
bool ExportNetworkProxyEnvironment(const RuntimeConfig& config,
                                   std::string* error = nullptr);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_RUNTIME_CONFIG_FILE_H_
