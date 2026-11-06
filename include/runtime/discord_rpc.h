#ifndef BLOX7_RUNTIME_DISCORD_RPC_H_
#define BLOX7_RUNTIME_DISCORD_RPC_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "runtime/roblox_experience_presence.h"
#include "runtime/runtime_config.h"

namespace blox7 {
namespace runtime {

struct DiscordRpcActivity {
  std::string details;
  std::string state;
  std::optional<int64_t> start_timestamp;
  std::string large_image;
  std::string large_text;
  std::string button_label;
  std::string button_url;
};

bool IsPublicDiscordJoin(const RobloxExperienceLaunchRequest& request);
std::string BuildDiscordJoinUrl(const RobloxExperienceLaunchRequest& request);
DiscordRpcActivity BuildDiscordRpcActivity(
    const DiscordRpcConfig& config, RobloxExperiencePresencePhase phase,
    const RobloxExperienceLaunchRequest* request, std::string place_name,
    int64_t session_started_at, std::string place_icon_url = {});

class DiscordRpcSession final {
 public:
  explicit DiscordRpcSession(DiscordRpcConfig config);
  ~DiscordRpcSession();

  DiscordRpcSession(const DiscordRpcSession&) = delete;
  DiscordRpcSession& operator=(const DiscordRpcSession&) = delete;

  bool Start(std::string* detail = nullptr);
  void Stop();
  RobloxExperiencePresenceObserver observer();

 private:
  class Impl;
  static void Notify(void* context, RobloxExperiencePresencePhase phase,
                     const RobloxExperienceLaunchRequest* request);

  std::unique_ptr<Impl> impl_;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_DISCORD_RPC_H_
