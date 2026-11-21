#ifndef BLOX7_RUNTIME_FRAME_RATE_POLICY_H_
#define BLOX7_RUNTIME_FRAME_RATE_POLICY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

inline constexpr int kMaximumSupportedRobloxSchedulerFps = 240;
inline constexpr int kUnlimitedRobloxFramerateCap = 9999;

enum class FrameRateLimitMode {
  kUnmanaged,
  kDisplay,
  kFixed,
  kUnlimited,
  kInvalid,
};

struct FrameRatePolicy {
  FrameRateLimitMode mode = FrameRateLimitMode::kUnmanaged;
  int fixed_fps = 0;
  bool held_by_roblox_setting = false;

  bool valid() const { return mode != FrameRateLimitMode::kInvalid; }
};

FrameRatePolicy ParseFrameRatePolicy(std::string_view value);

// roblox stops DFIntTaskSchedulerTargetFps at 240 but not its own Maximum
// Frame Rate setting, so unlimited and anything above 240 go through
// FramerateCap instead
std::optional<int> RobloxFramerateCapFor(const FrameRatePolicy &policy);

bool SyncRobloxFramerateCap(const std::filesystem::path &app_data,
                            const std::filesystem::path &loan_file,
                            std::optional<int> cap, std::string *error);

bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error);

} // namespace runtime
} // namespace blox7

#endif // BLOX7_RUNTIME_FRAME_RATE_POLICY_H_
