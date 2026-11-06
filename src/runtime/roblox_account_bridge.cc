#include "runtime/roblox_account_bridge.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace blox7 {
namespace runtime {
namespace {

constexpr char kAccountProtocolClass[] = "com/roblox/universalapp/account/JNIAccountProtocol";
constexpr std::size_t kMaximumNameBytes = 128;
constexpr std::size_t kMaximumRequestBytes = 64 * 1024;

enum Method : std::size_t { kDeviceIntegrityAvailable, kGetIntegrityToken, kMethodCount };
constexpr const char* kMethodLabels[kMethodCount] = {"DeviceIntegrityAvailable", "GetIntegrityToken"};

Status Unavailable(const char* message) { return Status::Error(StatusCode::kUnavailable, message); }

Status CheckJni(JNIEnv* env) {
  if (!env->ExceptionCheck()) return Status::Ok();
  env->ExceptionClear();
  return Unavailable("AccountProtocol JNI call failed");
}

bool ReadString(JNIEnv* env, jstring value, std::size_t maximum, std::string* text) {
  if (value == nullptr) return false;
  const jsize size = env->GetStringUTFLength(value);
  const char* bytes = env->GetStringUTFChars(value, nullptr);
  const bool valid = bytes != nullptr && size > 0 && static_cast<std::size_t>(size) <= maximum;
  if (valid) text->assign(bytes, static_cast<std::size_t>(size));
  if (bytes != nullptr) env->ReleaseStringUTFChars(value, bytes);
  return valid;
}

jstring RetainName(JNIEnv* env, jstring value) {
  std::string ignored;
  return ReadString(env, value, kMaximumNameBytes, &ignored) ? static_cast<jstring>(env->NewGlobalRef(value))
                                                             : nullptr;
}

}  // namespace

std::string RobloxIntegrityTokenAnswer(const std::string& request_json, const std::string& request_hash_key,
                                       const std::string& token_key, const std::string& result_key) {
  std::string request_hash;
  const nlohmann::json request = nlohmann::json::parse(request_json, nullptr, false);
  if (request.is_object()) {
    const auto found = request.find(request_hash_key);
    if (found != request.end()) {
      request_hash = found->is_string() ? found->get<std::string>() : found->dump();
    }
  }
  const char* status = request_hash.empty() ? "INVALID_NONCE" : "TOKEN_PROVIDER_UNINITIALIZED";
  nlohmann::ordered_json answer;
  answer[token_key] = "";
  answer[result_key] = status;
  return answer.dump();
}

struct RobloxAccountBridge::State {
  struct Endpoint {
    std::shared_ptr<State> state;
    Method method;
  };
  struct Registration {
    jobject handler = nullptr;
    jstring method = nullptr;
    bool registered = false;
  };

  jobject bus = nullptr;
  jstring protocol = nullptr;
  std::string support_key, request_hash_key, token_key, result_key;
  std::array<Registration, kMethodCount> registrations;
  std::mutex mutex;
  std::condition_variable drained;
  bool accepting = true;
  std::size_t in_flight = 0;

  static std::string Answer(void* context, JNIEnv* env, jstring request) {
    const auto* endpoint = static_cast<Endpoint*>(context);
    const std::shared_ptr<State> state = endpoint->state;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->accepting) return {};
      ++state->in_flight;
    }
    struct Finish {
      std::shared_ptr<State> state;
      ~Finish() {
        std::lock_guard<std::mutex> lock(state->mutex);
        --state->in_flight;
        state->drained.notify_all();
      }
    } finish{state};

    std::string answer;
    if (endpoint->method == kDeviceIntegrityAvailable) {
      nlohmann::ordered_json body;
      body[state->support_key] = true;
      answer = body.dump();
      std::fprintf(stderr, "  [account] AccountProtocol %s answered: available (as the APK)\n",
                   kMethodLabels[endpoint->method]);
    } else {
      std::string request_json;
      (void)ReadString(env, request, kMaximumRequestBytes, &request_json);
      answer = RobloxIntegrityTokenAnswer(request_json, state->request_hash_key, state->token_key,
                                          state->result_key);
      std::fprintf(stderr, "  [account] AccountProtocol %s answered: %s\n", kMethodLabels[endpoint->method],
                   answer.find("INVALID_NONCE") != std::string::npos ? "INVALID_NONCE"
                                                                      : "TOKEN_PROVIDER_UNINITIALIZED");
    }
    return answer;
  }
};

RobloxAccountBridge::RobloxAccountBridge(JniEnvironmentProvider environment, RobloxAccountProtocolSymbols symbols,
                                         RobloxAccountProtocolObjects objects)
    : environment_(environment), symbols_(symbols), objects_(objects) {}

RobloxAccountBridge::~RobloxAccountBridge() { (void)Shutdown(); }

Status RobloxAccountBridge::Initialize() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_) return Status::Error(StatusCode::kFailedPrecondition, "AccountProtocol is already initialized");
  if (!environment_.valid() || !symbols_.complete() || !objects_.complete()) {
    return Unavailable("AccountProtocol prerequisites are incomplete");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;

  jclass protocol_class = env->FindClass(kAccountProtocolClass);
  if (protocol_class == nullptr) {
    (void)CheckJni(env);
    return Unavailable("JNIAccountProtocol class is unavailable");
  }
  const jstring protocol = symbols_.get_protocol_name(env, protocol_class);
  const jstring methods[kMethodCount] = {symbols_.get_device_integrity_available_method_name(env, protocol_class),
                                         symbols_.get_integrity_token_method_name(env, protocol_class)};
  const jstring keys[] = {symbols_.get_support_key(env, protocol_class),
                          symbols_.get_request_hash_key(env, protocol_class),
                          symbols_.get_token_key(env, protocol_class),
                          symbols_.get_result_key(env, protocol_class)};
  env->DeleteLocalRef(protocol_class);

  state_ = std::make_shared<State>();
  std::string* const key_targets[] = {&state_->support_key, &state_->request_hash_key, &state_->token_key,
                                      &state_->result_key};
  bool names_valid = true;
  for (std::size_t i = 0; i < std::size(keys); ++i) {
    names_valid = ReadString(env, keys[i], kMaximumNameBytes, key_targets[i]) && names_valid;
    if (keys[i] != nullptr) env->DeleteLocalRef(keys[i]);
  }
  state_->protocol = RetainName(env, protocol);
  if (protocol != nullptr) env->DeleteLocalRef(protocol);
  for (std::size_t i = 0; i < kMethodCount; ++i) {
    state_->registrations[i].method = RetainName(env, methods[i]);
    names_valid = state_->registrations[i].method != nullptr && names_valid;
    if (methods[i] != nullptr) env->DeleteLocalRef(methods[i]);
  }
  state_->bus = env->NewGlobalRef(objects_.message_bus);
  auto fail = [this](Status failure) {
    (void)ShutdownLocked();
    return failure;
  };
  if (!CheckJni(env).ok() || !names_valid || state_->protocol == nullptr || state_->bus == nullptr) {
    return fail(Unavailable("AccountProtocol names are unavailable"));
  }

  for (std::size_t i = 0; i < kMethodCount; ++i) {
    State::Registration& entry = state_->registrations[i];
    auto endpoint = std::make_shared<State::Endpoint>(State::Endpoint{state_, static_cast<Method>(i)});
    jobject handler = objects_.create_request_handler(objects_.context, endpoint, &State::Answer);
    if (handler != nullptr) {
      entry.handler = env->NewGlobalRef(handler);
      if (entry.handler == nullptr) objects_.clear_request_handler(objects_.context, handler);
      env->DeleteLocalRef(handler);
    }
    if (entry.handler == nullptr || !CheckJni(env).ok()) {
      return fail(Unavailable("could not create an AccountProtocol handler"));
    }
    entry.registered = true;
    symbols_.set_request_handler_raw(env, state_->bus, state_->protocol, entry.method, entry.handler);
    status = CheckJni(env);
    if (!status.ok()) return fail(status);
  }
  std::fprintf(stderr, "  [account] AccountProtocol device-integrity handlers ready (the APK's answers, no Play "
                       "Integrity)\n");
  return Status::Ok();
}

Status RobloxAccountBridge::Shutdown() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  return ShutdownLocked();
}

Status RobloxAccountBridge::ShutdownLocked() {
  if (!state_) return Status::Ok();
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    state_->drained.wait(lock, [this] { return state_->in_flight == 0; });
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;
  const Status pending = CheckJni(env);
  if (!pending.ok()) status = pending;
  for (State::Registration& entry : state_->registrations) {
    if (entry.registered) {
      symbols_.clear_request_handler(env, state_->bus, state_->protocol, entry.method);
      const Status cleared = CheckJni(env);
      if (!cleared.ok()) status = cleared;
    }
    if (entry.handler != nullptr) {
      objects_.clear_request_handler(objects_.context, entry.handler);
      env->DeleteGlobalRef(entry.handler);
    }
    if (entry.method != nullptr) env->DeleteGlobalRef(entry.method);
    entry = {};
  }
  if (state_->protocol != nullptr) env->DeleteGlobalRef(state_->protocol);
  if (state_->bus != nullptr) env->DeleteGlobalRef(state_->bus);
  state_.reset();
  return status;
}

}  // namespace runtime
}  // namespace blox7
