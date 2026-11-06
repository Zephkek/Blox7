#ifndef BLOX7_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_
#define BLOX7_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "blox7/status.h"
#include "window/window_surface_lifecycle.h"

namespace blox7 {
namespace window {

enum class WindowResizeReadinessState {
  kDisabled,
  kWaitingForFirstPresent,
  kReadyToRequestResize,
  kResizeRequested,
  kSurfaceCommitted,
  kPostRebindPresented,
  kStopped,
  kFailed,
};

struct WindowResizeReadinessConfig {
  bool enabled = false;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
};

struct WindowResizeRequest {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct WindowResizeReadinessSnapshot {
  WindowResizeReadinessState state = WindowResizeReadinessState::kDisabled;
  uint64_t surface_generation = 0;
  uint64_t first_present_serial = 0;
  uint64_t post_rebind_present_serial = 0;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  std::string failure_reason;

  bool complete() const {
    return state == WindowResizeReadinessState::kDisabled ||
           state == WindowResizeReadinessState::kStopped;
  }
};

class WindowResizeReadinessGate final {
 public:
  Status Activate(WindowResizeReadinessConfig config,
                  const WindowSurfaceSnapshot& initial_surface);
  Status RecordPresent(uint64_t frame_serial);
  bool TakeResizeRequest(WindowResizeRequest* request);
  Status RecordCommittedSurfaceEvent(const WindowSurfaceEvent& event);
  Status RecordStopped();
  Status CompletionStatus() const;
  WindowResizeReadinessSnapshot Snapshot() const;

 private:
  Status FailLocked(std::string reason);

  mutable std::mutex mutex_;
  WindowResizeReadinessSnapshot snapshot_;
};

const char* WindowResizeReadinessStateName(WindowResizeReadinessState state);

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_
