#ifndef BLOX7_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
#define BLOX7_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>

namespace blox7 {
namespace window {

using MouseLockQueryCallback = bool (*)(void* context, bool* locked_center);

struct PointerPosition {
  float x = 0.0f;
  float y = 0.0f;
};

struct PointerCaptureRequest {
  bool relative_mode = false;
  bool cursor_visible = true;
  std::optional<PointerPosition> anchor;
  bool restore_position = true;
  // roblox's LockCenter: the pointer sits at the window center like the windows client, not where it was
  bool center = false;
};

class PointerCaptureBackend {
 public:
  virtual ~PointerCaptureBackend() = default;
  virtual bool Apply(const PointerCaptureRequest& request) = 0;
  // where the last Apply put the pointer, so roblox can be told (sdl drops that motion itself)
  virtual std::optional<PointerPosition> TakeWarp() { return std::nullopt; }
};

class WindowPointerCaptureOwner final {
 public:
  explicit WindowPointerCaptureOwner(PointerCaptureBackend* backend);
  ~WindowPointerCaptureOwner();

  WindowPointerCaptureOwner(const WindowPointerCaptureOwner&) = delete;
  WindowPointerCaptureOwner& operator=(const WindowPointerCaptureOwner&) =
      delete;

  bool RegisterQuery(MouseLockQueryCallback callback, void* context);
  void ClearQuery();
  bool Pump(bool text_input_active);
  bool OnRightButton(bool pressed, bool text_input_active,
                     std::optional<PointerPosition> position = std::nullopt);
  bool OnShiftKeyPressed(bool text_input_active);
  bool NeedsRightButtonReleaseRecovery(bool observed_pressed) const;
  bool ShouldDispatchMouseMotion();
  bool OnFocusGained(bool text_input_active);
  bool OnFocusLost();
  bool Shutdown();

  bool captured() const { return captured_; }
  bool cursor_visible() const { return cursor_visible_; }
  std::optional<PointerPosition> TakeWarp();

 private:
  bool Apply(bool capture, bool cursor_visible, bool restore_position = true, bool center = false);

  PointerCaptureBackend* backend_ = nullptr;
  std::mutex mutex_;
  std::condition_variable condition_;
  MouseLockQueryCallback callback_ = nullptr;
  void* context_ = nullptr;
  std::size_t in_flight_ = 0;
  bool clearing_ = false;
  bool focused_ = true;
  bool right_button_held_ = false;
  std::optional<PointerPosition> right_drag_anchor_;
  bool native_lock_observed_ = false;
  bool native_lock_was_active_before_right_drag_ = false;
  bool shift_key_pressed_during_right_drag_ = false;
  bool wait_for_native_unlock_after_right_drag_ = false;
  bool discard_next_motion_after_right_drag_ = false;
  bool captured_ = false;
  bool centered_ = false;
  bool cursor_visible_ = true;
  bool state_valid_ = true;
};

class SdlPointerCaptureBackend final : public PointerCaptureBackend {
 public:
  explicit SdlPointerCaptureBackend(void* window) : window_(window) {}

  bool Apply(const PointerCaptureRequest& request) override;
  std::optional<PointerPosition> TakeWarp() override;

 private:
  void* window_ = nullptr;
  float capture_anchor_x_ = 0.0f;
  float capture_anchor_y_ = 0.0f;
  bool relative_mode_ = false;
  bool capture_anchor_valid_ = false;
  bool capture_centered_ = false;
  std::optional<PointerPosition> warp_;
};

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
