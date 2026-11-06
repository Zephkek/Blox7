#ifndef BLOX7_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
#define BLOX7_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_

#include <memory>

#include "blox7/platform/sdl_gamepad_manager.h"
#include "blox7/platform/text_clipboard.h"
#include "blox7/status.h"
#include "runtime/roblox_input_native_adapter.h"
#include "runtime/roblox_text_surface_overlay.h"

namespace blox7 {
namespace runtime {

class RobloxWindowInputRuntime final {
 public:
  RobloxWindowInputRuntime(JniEnvironmentProvider environment,
                           RobloxInputSymbols symbols);
  ~RobloxWindowInputRuntime();

  RobloxWindowInputRuntime(const RobloxWindowInputRuntime&) = delete;
  RobloxWindowInputRuntime& operator=(const RobloxWindowInputRuntime&) = delete;

  Status Initialize();
  Status Shutdown();
  Status BeginTextFocusSession(RobloxTextFocusSession session);
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status QueryCurrentTextBoxInfo(RobloxNativeTextBoxInfoQueryResult* result);
  Status UpdateTextFocusProperties(uint64_t generation,
                                   const RobloxTextFocusProperties& properties);
  RobloxInputSnapshot Snapshot() const;

 private:
  static void PlatformEventCallback(void* context,
                                    const platform::PlatformEvent& event);
  static bool MouseLockQueryCallback(void* context, bool* locked_center);
  static void GamepadEventCallback(void* context,
                                   const platform::PlatformEvent& event);

  RobloxTextSurfaceOverlay text_surface_overlay_;
  std::unique_ptr<platform::TextClipboard> text_clipboard_;
  RobloxInputRuntime runtime_;
  platform::SdlGamepadManager gamepads_;
  bool observer_registered_ = false;
  bool mouse_lock_query_registered_ = false;
  bool initialized_ = false;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
