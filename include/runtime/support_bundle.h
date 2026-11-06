#ifndef BLOX7_RUNTIME_SUPPORT_BUNDLE_H_
#define BLOX7_RUNTIME_SUPPORT_BUNDLE_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace blox7 {
namespace runtime {

struct SupportBundleCollectionResult {
  bool attempted = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

SupportBundleCollectionResult CollectFailureSupportBundle(
    const Environment& environment, const RuntimePaths& paths,
    std::string_view context, std::string_view reason, int exit_code,
    const std::filesystem::path& source_log = {});

class FailureSupportBundleGuard {
 public:
  FailureSupportBundleGuard(const Environment& environment,
                            const RuntimePaths& paths, bool armed);
  ~FailureSupportBundleGuard();

  FailureSupportBundleGuard(const FailureSupportBundleGuard&) = delete;
  FailureSupportBundleGuard& operator=(const FailureSupportBundleGuard&) =
      delete;

  void SetExitCode(int exit_code);
  void Disarm();

 private:
  const Environment* environment_ = nullptr;
  const RuntimePaths* paths_ = nullptr;
  int exit_code_ = 1;
  bool armed_ = false;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_SUPPORT_BUNDLE_H_
