#ifndef BLOX7_MEDIA_ENCODING_H_
#define BLOX7_MEDIA_ENCODING_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blox7::media {

struct EncodedPacket {
  std::vector<std::uint8_t> data;
  std::int64_t time_us = 0;
  std::uint32_t flags = 0;
};

struct PictureLayout {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t luma_stride = 0;
  std::int32_t chroma_stride = 0;
  bool interleaved = false;

  static PictureLayout For(std::int32_t width, std::int32_t height) {
    PictureLayout layout;
    layout.width = width & ~1;
    layout.height = height & ~1;
    layout.luma_stride = (layout.width + 15) & ~15;
    layout.chroma_stride = (layout.width / 2 + 15) & ~15;
    return layout;
  }
  static PictureLayout InterleavedFor(std::int32_t width, std::int32_t height) {
    PictureLayout layout = For(width, height);
    layout.chroma_stride = layout.luma_stride;
    layout.interleaved = true;
    return layout;
  }
  std::size_t LumaSize() const {
    return static_cast<std::size_t>(luma_stride) * height;
  }
  std::size_t ChromaSize() const {
    return static_cast<std::size_t>(chroma_stride) * (height / 2);
  }
  std::size_t Size() const {
    return LumaSize() + (interleaved ? 1 : 2) * ChromaSize();
  }
};

}  // namespace blox7::media

#endif  // BLOX7_MEDIA_ENCODING_H_
