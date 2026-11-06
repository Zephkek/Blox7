#ifndef BLOX7_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_
#define BLOX7_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_

#include <cstdint>
#include <deque>
#include <mutex>

#include "blox7/status.h"

namespace blox7 {
namespace window {

enum class WindowSurfaceEventType {
  kCreated,
  kChanged,
  kDestroyed,
};

struct WindowSurfaceSnapshot {
  uint64_t generation = 0;
  uintptr_t native_window = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool available = false;
  float dpi_scale = 1.0f;
};

struct WindowSurfaceEvent {
  WindowSurfaceEventType type = WindowSurfaceEventType::kChanged;
  WindowSurfaceSnapshot surface;
};

class WindowSurfaceLifecycle final {
 public:
  Status Activate(uintptr_t native_window, uint32_t width, uint32_t height,
                  float dpi_scale = 1.0f);
  void Deactivate();
  Status Observe(uintptr_t native_window, uint32_t width, uint32_t height,
                 float dpi_scale = 1.0f);
  Status Refresh();
  Status Recreate();
  bool Poll(WindowSurfaceEvent* event);
  WindowSurfaceSnapshot Snapshot() const;
  bool active() const;

 private:
  void QueueLocked(WindowSurfaceEventType type,
                   const WindowSurfaceSnapshot& surface);
  Status RestoreLocked(uintptr_t native_window, uint32_t width,
                       uint32_t height, float dpi_scale);

  mutable std::mutex mutex_;
  std::deque<WindowSurfaceEvent> events_;
  WindowSurfaceSnapshot current_;
  bool active_ = false;
};

const char* WindowSurfaceEventTypeName(WindowSurfaceEventType type);

}  // namespace window
}  // namespace blox7

#endif  // BLOX7_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_
