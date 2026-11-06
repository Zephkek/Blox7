#ifndef BLOX7_RUNTIME_ROBLOX_TEXT_INPUT_JNI_BRIDGE_H_
#define BLOX7_RUNTIME_ROBLOX_TEXT_INPUT_JNI_BRIDGE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "blox7/status.h"
#include "runtime/roblox_input_native_adapter.h"
#include "window/window.h"

namespace jnivm {
class VM;
}  // namespace jnivm

namespace blox7 {
namespace runtime {

class RobloxWindowInputRuntime;

class RobloxTextInputJniBridgeBackend {
 public:
  virtual ~RobloxTextInputJniBridgeBackend() = default;

  virtual Status BeginTextFocusSession(RobloxTextFocusSession session) = 0;
  virtual Status EndTextFocusSession(int64_t textbox_handle,
                                     uint64_t generation,
                                     bool notify_native) = 0;
  virtual bool IsTextFocusSessionActive(uint64_t generation) = 0;
  virtual Status ReplaceFocusedTextFromEngine(
      uint64_t generation, std::string authoritative_utf8) = 0;
  virtual Status QueryCurrentTextBoxInfo(
      RobloxNativeTextBoxInfoQueryResult* result) = 0;
  virtual Status UpdateTextFocusProperties(
      uint64_t generation, const RobloxTextFocusProperties& properties) = 0;
  virtual bool RegisterMainThreadPump(window::PreTextInputPumpCallback callback,
                                      void* context) = 0;
  virtual void ClearMainThreadPump() = 0;
  virtual void SetTextInputOwnerEnabled(bool enabled) = 0;
  virtual bool RequestShowTextInput(
      uint64_t generation, const window::TextInputArea& area,
      const window::TextInputOptions& options) = 0;
  virtual bool RequestHideTextInput(uint64_t generation) = 0;
};

class RobloxTextInputJniBridge final {
 public:
  static Status Create(jnivm::VM* vm,
                       std::shared_ptr<RobloxWindowInputRuntime> input_runtime,
                       std::unique_ptr<RobloxTextInputJniBridge>* bridge);

  static Status CreateForTesting(
      jnivm::VM* vm, std::shared_ptr<RobloxTextInputJniBridgeBackend> backend,
      std::unique_ptr<RobloxTextInputJniBridge>* bridge);

  ~RobloxTextInputJniBridge();

  RobloxTextInputJniBridge(const RobloxTextInputJniBridge&) = delete;
  RobloxTextInputJniBridge& operator=(const RobloxTextInputJniBridge&) = delete;

  Status Shutdown();

 private:
  struct State;

  RobloxTextInputJniBridge(jnivm::VM* vm, std::shared_ptr<State> state);

  jnivm::VM* vm_ = nullptr;
  std::shared_ptr<State> state_;
  bool installed_ = false;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_TEXT_INPUT_JNI_BRIDGE_H_
