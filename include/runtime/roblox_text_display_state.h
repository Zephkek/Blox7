#ifndef BLOX7_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_
#define BLOX7_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "blox7/status.h"
#include "runtime/roblox_text_editor.h"

namespace blox7 {
namespace runtime {

struct RobloxTextOverlayViewport {
  int width = 0;
  int height = 0;
  float density_scale = 1.0F;

  bool valid() const {
    return width > 0 && height > 0 && std::isfinite(density_scale) &&
           density_scale > 0.0F;
  }
  int coordinate_width() const;
  int coordinate_height() const;
};

struct RobloxTextOverlayGeometry {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool used_fallback = false;

  bool valid() const { return width > 0 && height > 0; }

  bool operator==(const RobloxTextOverlayGeometry& other) const {
    return x == other.x && y == other.y && width == other.width &&
           height == other.height && used_fallback == other.used_fallback;
  }
  bool operator!=(const RobloxTextOverlayGeometry& other) const {
    return !(*this == other);
  }
};

struct RobloxTextOverlayPresentation {
  RobloxTextOverlayPresentation() = default;
  ~RobloxTextOverlayPresentation();

  RobloxTextOverlayPresentation(const RobloxTextOverlayPresentation&) = delete;
  RobloxTextOverlayPresentation& operator=(
      const RobloxTextOverlayPresentation&) = delete;
  RobloxTextOverlayPresentation(RobloxTextOverlayPresentation&& other);
  RobloxTextOverlayPresentation& operator=(
      RobloxTextOverlayPresentation&& other);

  void ClearSensitive();

  bool visible = false;
  uint64_t generation = 0;
  RobloxTextOverlayGeometry geometry;
  float font_size = 0.0F;
  bool multiline = false;
  int32_t x_alignment = 0;
  int32_t y_alignment = 1;
  int32_t text_color = 0;
  int32_t font = 0;
  int32_t text_input_type = 0;
  bool text_wrapped = false;
  std::string display_utf8;
  std::size_t caret_utf8_byte = 0;
  std::size_t selection_begin_utf8_byte = 0;
  std::size_t selection_end_utf8_byte = 0;
};

bool IsRobloxPasswordTextInputType(int32_t text_input_type);

RobloxTextOverlayGeometry ResolveRobloxTextOverlayGeometry(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport);

Status BuildRobloxTextOverlayPresentation(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport,
    RobloxTextOverlayPresentation* presentation);

class RobloxTextDisplayState final {
 public:
  RobloxTextDisplayState() = default;
  ~RobloxTextDisplayState() = default;

  RobloxTextDisplayState(const RobloxTextDisplayState&) = delete;
  RobloxTextDisplayState& operator=(const RobloxTextDisplayState&) = delete;

  Status Apply(const RobloxTextDisplayUpdate& update,
               const RobloxTextOverlayViewport& viewport, bool* changed);
  void Reset();

  const RobloxTextOverlayPresentation& presentation() const {
    return presentation_;
  }

 private:
  RobloxTextOverlayPresentation presentation_;
};

}  // namespace runtime
}  // namespace blox7

#endif  // BLOX7_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_
