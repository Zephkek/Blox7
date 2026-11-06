#ifndef BLOX7_GRAPHICS_TEXT_OVERLAY_FRAME_H_
#define BLOX7_GRAPHICS_TEXT_OVERLAY_FRAME_H_

#include <cstddef>
#include <cstdint>

struct Blox7TextOverlayFrameInfo {
  static constexpr std::uint32_t kAbiVersion = 3;

  std::uint32_t abi_version = kAbiVersion;
  std::uint32_t visible = 0;
  std::uint64_t revision = 0;
  std::uint32_t coordinate_width = 0;
  std::uint32_t coordinate_height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_bytes = 0;
  std::uint64_t rgba_bytes = 0;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
};

extern "C" {

bool blox7_text_overlay_may_present();
bool blox7_text_overlay_query(Blox7TextOverlayFrameInfo* frame);
bool blox7_text_overlay_copy(std::uint64_t revision, void* rgba,
                                std::size_t rgba_capacity);

}  // extern "C"

#endif  // BLOX7_GRAPHICS_TEXT_OVERLAY_FRAME_H_
