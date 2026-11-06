#ifndef BLOX7_UPDATE_UNSAFE_LATEST_RUNNER_H_
#define BLOX7_UPDATE_UNSAFE_LATEST_RUNNER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace blox7::update {

struct UnsafeLatestRunOptions {
  std::filesystem::path runtime_binary;
  std::filesystem::path payload_directory;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path host_abi_profile;

  std::vector<std::string> inherited_environment;
};

struct UnsafeLatestRunResult {
  int exit_code = -1;
  std::string error;

  explicit operator bool() const { return error.empty() && exit_code == 0; }
};

UnsafeLatestRunResult RunUnsafeLatestCandidate(
    const UnsafeLatestRunOptions& options);

}  // namespace blox7::update

#endif  // BLOX7_UPDATE_UNSAFE_LATEST_RUNNER_H_
