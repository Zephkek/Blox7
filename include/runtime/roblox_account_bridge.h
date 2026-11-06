#ifndef BLOX7_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_
#define BLOX7_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_

#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "blox7/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace blox7 {
namespace runtime {

struct RobloxAccountProtocolSymbols {
  using GetStringFn = jstring (*)(JNIEnv*, jclass);

  GetStringFn get_protocol_name = nullptr;
  GetStringFn get_device_integrity_available_method_name = nullptr;
  GetStringFn get_integrity_token_method_name = nullptr;
  GetStringFn get_support_key = nullptr;
  GetStringFn get_request_hash_key = nullptr;
  GetStringFn get_timeout_key = nullptr;
  GetStringFn get_token_key = nullptr;
  GetStringFn get_result_key = nullptr;
  void (*set_request_handler_raw)(JNIEnv*, jobject, jstring, jstring, jobject) = nullptr;
  void (*clear_request_handler)(JNIEnv*, jobject, jstring, jstring) = nullptr;

  bool complete() const {
    return get_protocol_name && get_device_integrity_available_method_name &&
           get_integrity_token_method_name && get_support_key && get_request_hash_key &&
           get_timeout_key && get_token_key && get_result_key && set_request_handler_raw &&
           clear_request_handler;
  }
};

struct RobloxAccountProtocolObjects {
  jobject message_bus = nullptr;
  void* context = nullptr;
  jobject (*create_request_handler)(void*, std::shared_ptr<void>,
                                    std::string (*)(void*, JNIEnv*, jstring)) = nullptr;
  void (*clear_request_handler)(void*, jobject) = nullptr;

  bool complete() const {
    return message_bus && context && create_request_handler && clear_request_handler;
  }
};

// no play integrity, so the device-integrity handlers answer like the apk does without it
class RobloxAccountBridge final {
 public:
  RobloxAccountBridge(JniEnvironmentProvider environment, RobloxAccountProtocolSymbols symbols,
                      RobloxAccountProtocolObjects objects);
  ~RobloxAccountBridge();
  RobloxAccountBridge(const RobloxAccountBridge&) = delete;
  RobloxAccountBridge& operator=(const RobloxAccountBridge&) = delete;

  Status Initialize();
  Status Shutdown();

 private:
  struct State;
  Status ShutdownLocked();

  const JniEnvironmentProvider environment_;
  const RobloxAccountProtocolSymbols symbols_;
  const RobloxAccountProtocolObjects objects_;
  std::mutex lifecycle_mutex_;
  std::shared_ptr<State> state_;
};

std::string RobloxIntegrityTokenAnswer(const std::string& request_json, const std::string& request_hash_key,
                                       const std::string& token_key, const std::string& result_key);

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_ACCOUNT_BRIDGE_H_
