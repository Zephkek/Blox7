#ifndef BLOX7_RUNTIME_ROBLOX_TEXT_SURFACE_OVERLAY_H_
#define BLOX7_RUNTIME_ROBLOX_TEXT_SURFACE_OVERLAY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "blox7/graphics/text_overlay_frame.h"
#include "blox7/status.h"
#include "runtime/roblox_text_display_state.h"

namespace blox7 {
namespace runtime {

class RobloxTextSurfaceOverlay final {
 public:
  RobloxTextSurfaceOverlay();
  ~RobloxTextSurfaceOverlay();

  RobloxTextSurfaceOverlay(const RobloxTextSurfaceOverlay&) = delete;
  RobloxTextSurfaceOverlay& operator=(const RobloxTextSurfaceOverlay&) = delete;

  Status Initialize(RobloxTextOverlayViewport viewport);
  Status UpdateViewport(RobloxTextOverlayViewport viewport);
  Status Shutdown();
  RobloxTextDisplaySink sink();

  bool QueryFrame(Blox7TextOverlayFrameInfo* frame);
  bool CopyFrame(std::uint64_t revision, void* rgba, std::size_t rgba_capacity);
  std::uint64_t revision();
  bool HitTest(float x, float y, bool clamp, std::size_t* code_points);

 private:
  struct RasterCache;

  static void UpdateCallback(void* context,
                             const RobloxTextDisplayUpdate& update);
  static bool HitTestCallback(void* context, float x, float y, bool clamp,
                              std::size_t* code_points);
  void ApplyUpdate(const RobloxTextDisplayUpdate& update);
  Status RasterizeLocked();
  void ClearFrameLocked();
  void RecordFailureLocked(Status status);

  std::mutex mutex_;
  RobloxTextDisplayState state_;
  RobloxTextOverlayViewport viewport_;
  std::vector<std::uint8_t> rgba_;
  int raster_width_ = 0;
  int raster_height_ = 0;
  std::unique_ptr<RasterCache> raster_cache_;
  std::uint64_t state_revision_ = 0;
  std::uint64_t raster_revision_ = 0;
  Status failure_;
  bool initialized_ = false;
  bool failure_logged_ = false;
  bool ready_logged_ = false;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_TEXT_SURFACE_OVERLAY_H_
