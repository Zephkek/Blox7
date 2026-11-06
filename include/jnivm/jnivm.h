#ifndef BLOX7_JNIVM_JNIVM_H_
#define BLOX7_JNIVM_JNIVM_H_

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jnivm {

class VM;
class Class;

class Object {
public:
  explicit Object(std::shared_ptr<Class> klass) : klass_(std::move(klass)) {}
  virtual ~Object() = default;

  std::shared_ptr<Class> GetClass() const { return klass_; }

private:
  std::shared_ptr<Class> klass_;
};

using MethodCallback = std::function<void(JNIEnv *, jobject)>;

struct RobloxAuthIdentity {
  int64_t user_id = -1;
  std::string username;
  std::string display_name;
};

struct PlatformIdentity {
  bool touch_enabled = false;
  bool mouse_enabled = true;
  bool keyboard_enabled = true;
  bool pc_hardware = true;
  std::string platform_name = "Linux";
  std::string device_name = "Blox7 Headless";
  std::string manufacturer = "Blox7";
  std::string model = "Blox7 Headless";
  std::string brand = "Blox7";
#if defined(__aarch64__)
  std::string device_code = "linux-arm64";
  std::string device_sku = "blox7-arm64";
  std::string soc_model = "aarch64";
#else
  std::string device_code = "linux-x86_64";
  std::string device_sku = "blox7-x86_64";
  std::string soc_model = "x86_64";
#endif
};

struct RobloxCredentialView {
  const char *data = nullptr;
  std::size_t size = 0;
};

using RobloxCredentialProvider = RobloxCredentialView (*)(const void *context);
using RobloxCookieGetter = jstring (*)(JNIEnv* env, jclass clazz,
                                       jstring domain);

jobject CreateAndroidConfiguration(JNIEnv *env);

struct RobloxCredentialSinkCallbacks {
  bool (*store)(void *context, const char *data, std::size_t size) = nullptr;
};

struct FmodAudioDeviceCallbacks {
  bool (*init)(void *context, const void *identity, int channels,
               int sample_rate_hz, int block_size_frames,
               int block_count) = nullptr;
  bool (*write)(void *context, const void *identity, const std::uint8_t *data,
                std::size_t size) = nullptr;
  bool (*close)(void *context, const void *identity) = nullptr;
  void (*shutdown)(void *context) = nullptr;
};

struct WebRtcAudioManagerParameters {
  int sample_rate_hz = 0;
  int output_channels = 0;
  int input_channels = 0;
  bool hardware_aec = false;
  bool hardware_agc = false;
  bool hardware_ns = false;
  bool low_latency_output = false;
  bool low_latency_input = false;
  bool pro_audio = false;
  bool aaudio = false;
  int output_buffer_size_frames = 0;
  int input_buffer_size_frames = 0;
};

struct WebRtcAudioManagerCallbacks {
  bool (*get_parameters)(void* context,
                         WebRtcAudioManagerParameters* parameters) = nullptr;
  bool (*init)(void* context, const void* identity) = nullptr;
  void (*dispose)(void* context, const void* identity) = nullptr;
  void (*set_microphone_mute)(void* context, bool muted) = nullptr;
};

using WebRtcAudioRecordDataCallback = void (*)(void *context,
                                               const void *identity,
                                               std::size_t size_bytes);

struct WebRtcAudioRecordCallbacks {
  int (*init)(void *context, const void *identity, int sample_rate_hz,
              int channels, WebRtcAudioRecordDataCallback data_callback,
              void *data_context, void **direct_buffer,
              std::size_t *direct_buffer_capacity) = nullptr;
  bool (*start)(void *context, const void *identity) = nullptr;
  bool (*stop)(void *context, const void *identity) = nullptr;
  void (*close)(void *context, const void *identity) = nullptr;
  void (*shutdown)(void *context) = nullptr;
};

using WebRtcAudioTrackDataCallback = void (*)(void *context,
                                              const void *identity,
                                              std::size_t size_bytes);

struct WebRtcAudioTrackCallbacks {
  int (*init)(void *context, const void *identity, int sample_rate_hz,
              int channels, double buffer_size_factor,
              WebRtcAudioTrackDataCallback data_callback, void *data_context,
              void **direct_buffer,
              std::size_t *direct_buffer_capacity) = nullptr;
  int (*buffer_size_frames)(void *context, const void *identity) = nullptr;
  bool (*start)(void *context, const void *identity) = nullptr;
  bool (*stop)(void *context, const void *identity) = nullptr;
  void (*close)(void *context, const void *identity) = nullptr;
};

struct AndroidWindowCallbacks {
  bool (*set_flags)(void *context, int flags, int mask) = nullptr;
};

struct RobloxTextBoxInfo {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float font_size = 0.0f;
  bool multiline = false;
  int x_alignment = 0;
  int y_alignment = 0;
  int text_color = 0;
  int font = 0;
  int text_input_type = 0;
  int return_key_type = 0;
  bool manual_focus_release = false;
  bool text_wrapped = false;
};

struct RobloxTextInputShowRequest {
  std::int64_t text_box = 0;
  bool show_native_input = false;
  std::string text;
  RobloxTextBoxInfo info;
};

struct RobloxTextInputCallbacks {
  void (*show)(void *context,
               const RobloxTextInputShowRequest &request) = nullptr;
  void (*hide)(void *context) = nullptr;
  void (*replace_text)(void *context, const std::string &text) = nullptr;
  void (*properties_changed)(void *context) = nullptr;
  void (*shutdown)(void *context) = nullptr;
};

struct MessageBusRawCallbacks {
  void (*run)(void *context, JNIEnv *env, jstring message) = nullptr;
};

struct MessageBusRequestHandlerCallbacks {
  std::string (*run)(void *context, JNIEnv *env, jstring message) = nullptr;
};

struct MessageBusAsyncRequestHandlerCallbacks {
  void (*run)(void* context, JNIEnv* env, jstring message,
              jstring response_id) = nullptr;
};

struct MemStorageCallbackCallbacks {
  void (*on_item_set)(void *context, JNIEnv *env, jstring value) = nullptr;
};

struct RobloxDataModelNotificationCallbacks {
  void (*on_notification)(void *context, JNIEnv *env, jstring type,
                          jstring data) = nullptr;
  void (*on_app_bridge_notification)(void* context, JNIEnv* env, jstring type,
                                     jstring data) = nullptr;
  void (*on_native_overlay)(void* context, JNIEnv* env, jstring title,
                            jstring url) = nullptr;
  void (*on_open_web_activity)(void* context, JNIEnv* env, jstring url,
                               jstring title) = nullptr;
  void (*on_sync_cookies)(void* context, JNIEnv* env, jstring cookie) = nullptr;
  void (*on_set_cookie)(void* context, JNIEnv* env, jstring cookie,
                        jstring url) = nullptr;
};

struct RobloxExperienceLifecycleCallbacks {
  void (*on_lua_app_did_return)(void *context) = nullptr;
};

class Class {
public:
  explicit Class(std::string name) : name_(std::move(name)) {}

  const std::string &GetName() const { return name_; }

  void RegisterMethod(const std::string &method_name,
                      const std::string &signature, MethodCallback callback);

  const MethodCallback *FindMethod(const std::string &method_name,
                                   const std::string &signature) const;

  const std::unordered_map<std::string, MethodCallback> &GetMethods() const {
    return methods_;
  }

private:
  std::string name_;

  std::unordered_map<std::string, MethodCallback> methods_;
};

class VM {
public:
  VM();
  ~VM();

  VM(const VM &) = delete;
  VM &operator=(const VM &) = delete;

  std::shared_ptr<Class> RegisterClass(const std::string &class_name);

  std::shared_ptr<Class> FindClass(const std::string &class_name) const;

  JavaVM *GetJavaVM() { return java_vm_; }

  static VM *FromJavaVM(JavaVM *java_vm);

  JNIEnv *GetJNIEnv();

  std::size_t GetClassCount() const { return class_registry_.size(); }

  void SetRobloxAuthIdentity(const RobloxAuthIdentity &identity);

  void ClearRobloxAuthIdentity();

  RobloxAuthIdentity GetRobloxAuthIdentitySnapshot() const;

  void SetPlatformIdentity(const PlatformIdentity &identity);
  PlatformIdentity GetPlatformIdentitySnapshot() const;

  void SetRobloxCredentialProvider(const void *context,
                                   RobloxCredentialProvider provider);
  void ClearRobloxCredentialProvider();

  bool CopyRobloxCredentialFromProvider(std::string *credential) const;

  void SetRobloxCookieGetter(RobloxCookieGetter getter);
  bool RefreshRobloxCredentialFromEngine(JNIEnv* env);

  void SetRobloxCredentialSink(std::shared_ptr<void> context,
                               const RobloxCredentialSinkCallbacks &callbacks);
  void ClearRobloxCredentialSink();
  bool DispatchRobloxCredential(const char *data, std::size_t size);

  void SetFmodAudioDeviceCallbacks(std::shared_ptr<void> context,
                                   const FmodAudioDeviceCallbacks &callbacks);
  void ClearFmodAudioDeviceCallbacks();

  bool DispatchFmodAudioDeviceInit(const void *identity, int channels,
                                   int sample_rate_hz, int block_size_frames,
                                   int block_count);
  bool DispatchFmodAudioDeviceWrite(const void *identity,
                                    const std::uint8_t *data, std::size_t size);
  bool DispatchFmodAudioDeviceClose(const void *identity);

  void SetWebRtcAudioManagerCallbacks(
      std::shared_ptr<void> context,
      const WebRtcAudioManagerCallbacks& callbacks);
  void ClearWebRtcAudioManagerCallbacks();
  void RegisterWebRtcAudioManagerNative(const char* name, const char* signature,
                                        void* function);
  bool DispatchWebRtcAudioManagerConstruct(jobject manager,
                                           jlong native_audio_manager);
  bool DispatchWebRtcAudioManagerInit(jobject manager);
  void DispatchWebRtcAudioManagerDispose(jobject manager);
  void DispatchWebRtcAudioManagerMicrophoneMute(jobject manager, bool muted);

  void
  SetWebRtcAudioRecordCallbacks(std::shared_ptr<void> context,
                                const WebRtcAudioRecordCallbacks &callbacks);
  void ClearWebRtcAudioRecordCallbacks();
  int DispatchWebRtcAudioRecordInit(const void *identity, int sample_rate_hz,
                                    int channels, void **direct_buffer,
                                    std::size_t *direct_buffer_capacity);
  bool DispatchWebRtcAudioRecordStart(const void *identity);
  bool DispatchWebRtcAudioRecordStop(const void *identity);
  void DispatchWebRtcAudioRecordClose(const void *identity);
  void DispatchWebRtcAudioRecordData(const void *identity,
                                     std::size_t size_bytes);

  void RegisterWebRtcAudioRecordNative(const char *name, const char *signature,
                                       void *function);

  void SetWebRtcAudioTrackCallbacks(std::shared_ptr<void> context,
                                    const WebRtcAudioTrackCallbacks &callbacks);
  void ClearWebRtcAudioTrackCallbacks();
  int DispatchWebRtcAudioTrackInit(const void *identity, int sample_rate_hz,
                                   int channels, double buffer_size_factor,
                                   void **direct_buffer,
                                   std::size_t *direct_buffer_capacity);
  int DispatchWebRtcAudioTrackBufferSizeFrames(const void *identity);
  bool DispatchWebRtcAudioTrackStart(const void *identity);
  bool DispatchWebRtcAudioTrackStop(const void *identity);
  void DispatchWebRtcAudioTrackClose(const void *identity);
  void DispatchWebRtcAudioTrackData(const void *identity,
                                    std::size_t size_bytes);

  void RegisterWebRtcAudioTrackNative(const char *name, const char *signature,
                                      void *function);

  void SetAndroidWindowCallbacks(std::shared_ptr<void> context,
                                 const AndroidWindowCallbacks &callbacks);
  void ClearAndroidWindowCallbacks();
  bool DispatchAndroidWindowFlags(int flags, int mask);

  void SetRobloxTextInputCallbacks(std::shared_ptr<void> context,
                                   const RobloxTextInputCallbacks &callbacks);
  void ClearRobloxTextInputCallbacks();
  bool DispatchRobloxTextInputShow(const RobloxTextInputShowRequest &request);
  bool DispatchRobloxTextInputHide();
  bool DispatchRobloxTextInputReplaceText(const std::string &text);
  bool DispatchRobloxTextInputPropertiesChanged();

  jobject CreateMessageBusRawCallback(std::shared_ptr<void> context,
                                      const MessageBusRawCallbacks &callbacks);
  void ClearMessageBusRawCallback(jobject callback);
  bool DispatchMessageBusRawCallback(jobject callback, JNIEnv *env,
                                     jstring message);
  jobject CreateMessageBusRequestHandler(
      std::shared_ptr<void> context,
      const MessageBusRequestHandlerCallbacks &callbacks);
  void ClearMessageBusRequestHandler(jobject handler);
  jstring DispatchMessageBusRequestHandler(jobject handler, JNIEnv *env,
                                           jstring message);

  jobject CreateMessageBusAsyncRequestHandler(
      std::shared_ptr<void> context,
      const MessageBusAsyncRequestHandlerCallbacks& callbacks);
  void ClearMessageBusAsyncRequestHandler(jobject handler);
  bool DispatchMessageBusAsyncRequestHandler(jobject handler, JNIEnv* env,
                                             jstring message,
                                             jstring response_id);

  jobject CreateMemStorageCallback(
      std::shared_ptr<void> context,
      const MemStorageCallbackCallbacks& callbacks);
  void ClearMemStorageCallback(jobject callback);
  bool DispatchMemStorageCallback(jobject callback, JNIEnv *env, jstring value);

  void SetRobloxDataModelNotificationCallbacks(
      std::shared_ptr<void> context,
      const RobloxDataModelNotificationCallbacks &callbacks);
  void ClearRobloxDataModelNotificationCallbacks();
  bool DispatchRobloxDataModelNotification(JNIEnv *env, jstring type,
                                           jstring data);
  bool DispatchRobloxAppBridgeNotification(JNIEnv* env, jstring type,
                                           jstring data);
  bool DispatchRobloxNativeOverlay(JNIEnv* env, jstring title, jstring url);
  bool DispatchRobloxOpenWebActivity(JNIEnv* env, jstring url, jstring title);
  bool DispatchRobloxCookieSync(JNIEnv* env, jstring cookie);
  bool DispatchRobloxCookieSet(JNIEnv* env, jobjectArray cookies, jstring url);

  void SetRobloxExperienceLifecycleCallbacks(
      std::shared_ptr<void> context,
      const RobloxExperienceLifecycleCallbacks &callbacks);
  void ClearRobloxExperienceLifecycleCallbacks();
  bool DispatchRobloxExperienceLuaAppDidReturn();

  void RestoreFunctions();

private:
  JNIInvokeInterface_ invoke_interface_ = {};
  JavaVM java_vm_storage_ = {};
  JavaVM *java_vm_ = nullptr;

  JNINativeInterface_ native_interface_ = {};
  JNIEnv jni_env_storage_ = {};
  JNIEnv *jni_env_ = nullptr;

  std::unordered_map<std::string, std::shared_ptr<Class>> class_registry_;

  mutable std::mutex roblox_auth_identity_mutex_;
  RobloxAuthIdentity roblox_auth_identity_;

  mutable std::mutex platform_identity_mutex_;
  PlatformIdentity platform_identity_;

  mutable std::mutex roblox_credential_provider_mutex_;
  const void *roblox_credential_provider_context_ = nullptr;
  RobloxCredentialProvider roblox_credential_provider_ = nullptr;
  std::string roblox_credential_override_;

  mutable std::mutex roblox_cookie_getter_mutex_;
  RobloxCookieGetter roblox_cookie_getter_ = nullptr;

  struct RobloxCredentialSinkBinding {
    std::shared_ptr<void> context;
    RobloxCredentialSinkCallbacks callbacks;
  };
  mutable std::mutex roblox_credential_sink_mutex_;
  RobloxCredentialSinkBinding roblox_credential_sink_binding_;

  struct FmodAudioDeviceBinding {
    std::shared_ptr<void> context;
    FmodAudioDeviceCallbacks callbacks;
  };
  mutable std::mutex fmod_audio_device_mutex_;
  FmodAudioDeviceBinding fmod_audio_device_binding_;

  struct WebRtcAudioManagerBinding {
    std::shared_ptr<void> context;
    WebRtcAudioManagerCallbacks callbacks;
  };
  mutable std::mutex webrtc_audio_manager_mutex_;
  WebRtcAudioManagerBinding webrtc_audio_manager_binding_;
  void* webrtc_cache_audio_parameters_native_ = nullptr;

  struct WebRtcAudioRecordBinding {
    std::shared_ptr<void> context;
    WebRtcAudioRecordCallbacks callbacks;
  };
  mutable std::mutex webrtc_audio_record_mutex_;
  WebRtcAudioRecordBinding webrtc_audio_record_binding_;

  mutable std::mutex webrtc_audio_record_native_mutex_;
  void *webrtc_cache_direct_buffer_native_ = nullptr;
  void *webrtc_data_is_recorded_native_ = nullptr;

  struct WebRtcAudioTrackBinding {
    std::shared_ptr<void> context;
    WebRtcAudioTrackCallbacks callbacks;
  };
  mutable std::mutex webrtc_audio_track_mutex_;
  WebRtcAudioTrackBinding webrtc_audio_track_binding_;

  mutable std::mutex webrtc_audio_track_native_mutex_;
  void *webrtc_track_cache_direct_buffer_native_ = nullptr;
  void *webrtc_get_playout_data_native_ = nullptr;

  struct AndroidWindowBinding {
    std::shared_ptr<void> context;
    AndroidWindowCallbacks callbacks;
  };
  mutable std::mutex android_window_mutex_;
  AndroidWindowBinding android_window_binding_;

  struct RobloxTextInputBinding;
  mutable std::mutex roblox_text_input_mutex_;
  std::shared_ptr<RobloxTextInputBinding> roblox_text_input_binding_;

  struct MessageBusRawBinding {
    std::shared_ptr<void> context;
    MessageBusRawCallbacks callbacks;
  };
  mutable std::mutex message_bus_raw_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MessageBusRawBinding>>
      message_bus_raw_bindings_;

  struct MessageBusRequestHandlerBinding {
    std::shared_ptr<void> context;
    MessageBusRequestHandlerCallbacks callbacks;
  };
  mutable std::mutex message_bus_request_handler_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MessageBusRequestHandlerBinding>>
      message_bus_request_handler_bindings_;

  struct MessageBusAsyncRequestHandlerBinding {
    std::shared_ptr<void> context;
    MessageBusAsyncRequestHandlerCallbacks callbacks;
  };
  mutable std::mutex message_bus_async_request_handler_mutex_;
  std::unordered_map<jobject,
                     std::shared_ptr<MessageBusAsyncRequestHandlerBinding>>
      message_bus_async_request_handler_bindings_;

  struct MemStorageCallbackBinding {
    std::shared_ptr<void> context;
    MemStorageCallbackCallbacks callbacks;
  };
  mutable std::mutex mem_storage_callback_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MemStorageCallbackBinding>>
      mem_storage_callback_bindings_;

  struct RobloxDataModelNotificationBinding {
    std::shared_ptr<void> context;
    RobloxDataModelNotificationCallbacks callbacks;
  };
  mutable std::mutex roblox_data_model_notification_mutex_;
  std::shared_ptr<RobloxDataModelNotificationBinding>
      roblox_data_model_notification_binding_;

  struct RobloxExperienceLifecycleBinding {
    std::shared_ptr<void> context;
    RobloxExperienceLifecycleCallbacks callbacks;
  };
  mutable std::mutex roblox_experience_lifecycle_mutex_;
  std::shared_ptr<RobloxExperienceLifecycleBinding>
      roblox_experience_lifecycle_binding_;

  void InitJNIFunctionTables();
};

} // namespace jnivm

#endif  // BLOX7_JNIVM_JNIVM_H_
