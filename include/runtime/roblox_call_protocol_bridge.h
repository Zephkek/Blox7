#ifndef BLOX7_RUNTIME_ROBLOX_CALL_PROTOCOL_BRIDGE_H_
#define BLOX7_RUNTIME_ROBLOX_CALL_PROTOCOL_BRIDGE_H_

#include "runtime/roblox_message_bus_request_bridge.h"

namespace blox7 {
namespace runtime {

class RobloxCallProtocolBridge final {
 public:
  RobloxCallProtocolBridge(JniEnvironmentProvider environment,
                           RobloxMessageBusSymbols symbols,
                           RobloxMessageBusObjects objects);
  Status Initialize();
  Status Shutdown();

 private:
  RobloxMessageBusRequestBridge bridge_;
};

}  // namespace runtime
}  // namespace blox7
#endif
