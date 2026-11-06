#ifndef BLOX7_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_
#define BLOX7_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "blox7/status.h"

namespace blox7 {
namespace runtime {

class RobloxSecretStore;

struct ResolvedFollowUserPlace {
  int64_t place_id = 0;
  std::string game_instance_id;
};

std::string ReadStoredRoblosecurityCookie(
    std::shared_ptr<RobloxSecretStore> secrets = nullptr);

Status ResolveFollowUserPlace(int64_t user_id, const std::string& auth_cookie,
                              ResolvedFollowUserPlace* out);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_
