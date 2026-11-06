#include "runtime/roblox_call_protocol_bridge.h"

namespace blox7 {
namespace runtime {

RobloxCallProtocolBridge::RobloxCallProtocolBridge(
    JniEnvironmentProvider environment, RobloxMessageBusSymbols symbols,
    RobloxMessageBusObjects objects)
    : bridge_(environment, symbols, objects, "Call", {"getCallState"},
              [](JNIEnv* env, jstring request, std::size_t) {
                if (request && env->GetStringUTFLength(request) > 65536) {
                  return RobloxMessageBusReply{"{}", 13, "oversized_request"};
                }
                return RobloxMessageBusReply{
                    R"({"status":"Idle","muted":true,"camEnabled":false})", 0,
                    "idle"};
              }) {}

Status RobloxCallProtocolBridge::Initialize() {
  return bridge_.Initialize();
}

Status RobloxCallProtocolBridge::Shutdown() {
  return bridge_.Shutdown();
}

}  // namespace runtime
}  // namespace blox7
