#ifndef BLOX7_GRAPHICS_GLES_TEXT_OVERLAY_COMPOSITOR_H_
#define BLOX7_GRAPHICS_GLES_TEXT_OVERLAY_COMPOSITOR_H_

#include <memory>

#include "blox7/graphics/text_overlay_frame.h"

struct SDL_Window;

namespace blox7::graphics {

struct GlesTextOverlaySource {
  bool (*may_present)() = nullptr;
  bool (*query)(Blox7TextOverlayFrameInfo *) = nullptr;
  bool (*copy)(std::uint64_t, void *, std::size_t) = nullptr;
};

class GlesTextOverlayCompositor {
public:
  explicit GlesTextOverlayCompositor(GlesTextOverlaySource source);
  ~GlesTextOverlayCompositor();

  bool Draw(SDL_Window *window);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace blox7::graphics

#endif // BLOX7_GRAPHICS_GLES_TEXT_OVERLAY_COMPOSITOR_H_
