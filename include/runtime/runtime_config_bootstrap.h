#ifndef BLOX7_RUNTIME_RUNTIME_CONFIG_BOOTSTRAP_H_
#define BLOX7_RUNTIME_RUNTIME_CONFIG_BOOTSTRAP_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

enum class RuntimeConfigBootstrapStatus {
  kCreated,
  kPreservedExisting,
};

struct RuntimeConfigBootstrapResult {
  RuntimeConfigBootstrapStatus status =
      RuntimeConfigBootstrapStatus::kPreservedExisting;
  std::string error;

  explicit operator bool() const { return error.empty(); }
  bool created() const {
    return status == RuntimeConfigBootstrapStatus::kCreated;
  }
};

std::string_view DefaultRuntimeConfigYaml();

RuntimeConfigBootstrapResult EnsureRuntimeConfigFile(
    const std::filesystem::path& config_file);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_RUNTIME_CONFIG_BOOTSTRAP_H_
