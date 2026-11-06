#ifndef BLOX7_RUNTIME_ROBLOX_LAUNCH_URI_H_
#define BLOX7_RUNTIME_ROBLOX_LAUNCH_URI_H_

#include <cstddef>
#include <string_view>

#include "blox7/status.h"
#include "runtime/roblox_experience_launch_bridge.h"

namespace blox7 {
namespace runtime {

inline constexpr std::size_t kMaximumRobloxLaunchUriBytes = 64 * 1024;

Status ParseRobloxLaunchUri(std::string_view uri,
                            RobloxExperienceLaunchRequest* request);

Status ParseRobloxWebViewLaunchGame(std::string_view command,
                                    RobloxExperienceLaunchRequest* request);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_LAUNCH_URI_H_
