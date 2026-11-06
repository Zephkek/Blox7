#ifndef BLOX7_RUNTIME_FRAME_RATE_POLICY_H_
#define BLOX7_RUNTIME_FRAME_RATE_POLICY_H_

#include <string>
#include <string_view>

namespace blox7 {
namespace runtime {

inline constexpr int kMaximumSupportedRobloxSchedulerFps = 240;

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

  bool valid() const { return mode != FrameRateLimitMode::kInvalid; }
};

FrameRatePolicy ParseFrameRatePolicy(std::string_view value);

bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error);

} // namespace runtime
} // namespace blox7

#endif // BLOX7_RUNTIME_FRAME_RATE_POLICY_H_
