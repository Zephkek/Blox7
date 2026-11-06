#ifndef BLOX7_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_
#define BLOX7_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_

#include "runtime/roblox_experience_launch_bridge.h"

namespace blox7 {
namespace runtime {

enum class RobloxExperiencePresencePhase {
  kBrowsing,
  kJoining,
  kPlaying,
};

using NotifyRobloxExperiencePresenceFn =
    void (*)(void* context, RobloxExperiencePresencePhase phase,
             const RobloxExperienceLaunchRequest* request);

struct RobloxExperiencePresenceObserver {
  void* context = nullptr;
  NotifyRobloxExperiencePresenceFn notify = nullptr;

  bool valid() const { return notify != nullptr; }
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_
