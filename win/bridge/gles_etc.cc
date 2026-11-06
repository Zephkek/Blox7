#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <cstdint>
#include <cstdlib>

#include "blox7/graphics/etc2_decoder.h"

using blox7::graphics::EtcFormat;

extern "C" {

GLenum gles_etc_storage_format(GLenum format, int* decoder) {
  switch (format) {
    case GL_ETC1_RGB8_OES:  // etc1 is just etc2 lite
    case GL_COMPRESSED_RGB8_ETC2:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgb8);
      return GL_RGBA8;
    case GL_COMPRESSED_SRGB8_ETC2:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgb8);
      return GL_SRGB8_ALPHA8;
    case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgb8A1);
      return GL_RGBA8;
    case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgb8A1);
      return GL_SRGB8_ALPHA8;
    case GL_COMPRESSED_RGBA8_ETC2_EAC:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgba8);
      return GL_RGBA8;
    case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
      *decoder = static_cast<int>(EtcFormat::kEtc2Rgba8);
      return GL_SRGB8_ALPHA8;
    default:
      return 0;
  }
}

void* gles_etc_decode(int decoder, const void* data, size_t size, int w, int h, int layers) {
  if (!data || w <= 0 || h <= 0 || layers <= 0) return nullptr;
  const EtcFormat format = static_cast<EtcFormat>(decoder);
  const size_t layer_source = size / static_cast<size_t>(layers);
  const size_t layer_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  static thread_local std::uint8_t* buffer = nullptr;
  static thread_local size_t buffer_size = 0;
  const size_t need = layer_bytes * static_cast<size_t>(layers);
  if (need > buffer_size) {
    std::free(buffer);
    buffer = static_cast<std::uint8_t*>(std::malloc(need));
    buffer_size = buffer ? need : 0;
  }
  std::uint8_t* out = buffer;
  if (!out) return nullptr;
  blox7::graphics::EtcDecodeJob jobs[64];
  const int n = layers < 64 ? layers : 64;
  bool ok = layers <= 64;
  for (int i = 0; i < n; i++) {
    jobs[i].format = format;
    jobs[i].source = static_cast<const std::uint8_t*>(data) + layer_source * static_cast<size_t>(i);
    jobs[i].source_bytes = layer_source;
    jobs[i].width = static_cast<std::uint32_t>(w);
    jobs[i].height = static_cast<std::uint32_t>(h);
    jobs[i].destination = out + layer_bytes * static_cast<size_t>(i);
    jobs[i].destination_bytes = layer_bytes;
  }
  if (ok) {
    blox7::graphics::DecodeEtcJobs(jobs, static_cast<size_t>(n), 4);
    for (int i = 0; i < n; i++) ok = ok && jobs[i].ok;
  }
  return ok ? out : nullptr;
}

}  // extern "C"
