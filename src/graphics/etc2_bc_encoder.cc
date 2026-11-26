#include "blox7/graphics/etc2_bc_encoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace blox7::graphics {
namespace {

using uint = std::uint32_t;
using std::abs;
using std::clamp;
using std::max;
using std::min;

int bitCount(uint value) { return __builtin_popcount(value); }

int findLSB(uint value) { return value == 0 ? -1 : __builtin_ctz(value); }

#define BC_TABLE(type, name, count) constexpr type name[count] = {
#define BC_TABLE_END };
#define BC_IN_ARRAY(type, name, count) const type(&name)[count]
#define BC_INOUT_ARRAY(type, name, count) type(&name)[count]
#define BC_INOUT(type, name) type& name
#include "shaders/bc_block.glsl"
#undef BC_TABLE
#undef BC_TABLE_END
#undef BC_IN_ARRAY
#undef BC_INOUT_ARRAY
#undef BC_INOUT

bool IsColor(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

void EncodeColorBlock(EtcFormat format, const std::uint8_t* source, uint valid,
                      std::uint8_t* destination) {
  std::uint8_t texels[64];
  DecodeEtcImage(format, source, EtcBlockBytes(format), 4, 4, texels,
                 sizeof(texels));
  uint px[16];
  std::memcpy(px, texels, sizeof(px));
  uint bytes[8];
  const std::uint8_t* color =
      format == EtcFormat::kEtc2Rgba8 ? source + 8 : source;
  for (int i = 0; i < 8; ++i) {
    bytes[i] = color[i];
  }
  uint block[4];
  BcEncodeColor(px, valid, bytes, format == EtcFormat::kEtc2Rgb8A1, block);
  std::memcpy(destination, block, sizeof(block));
}

void EncodeScalarBlocks(EtcFormat format, const std::uint8_t* source,
                        uint valid, std::uint8_t* destination) {
  const bool is_signed =
      format == EtcFormat::kEacR11Signed || format == EtcFormat::kEacRg11Signed;
  const int channels = static_cast<int>(EtcBlockBytes(format) / 8);
  std::uint8_t texels[64];
  DecodeEtcImage(format, source, EtcBlockBytes(format), 4, 4, texels,
                 sizeof(texels));
  for (int channel = 0; channel < channels; ++channel) {
    int values[16];
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t* texel = texels + (i * channels + channel) * 2;
      const int wide = texel[0] | (texel[1] << 8);
      if (is_signed) {
        const int whole = static_cast<std::int16_t>(wide);
        values[i] = whole < 0 ? -(-whole >> 5) : whole >> 5;
      } else {
        values[i] = wide >> 5;
      }
    }
    uint block[2];
    BcEncodeScalar(values, valid, is_signed, block);
    std::memcpy(destination + channel * 8, block, sizeof(block));
  }
}

}  // namespace

std::size_t BcBlockBytes(EtcFormat format) {
  return format == EtcFormat::kEacR11 || format == EtcFormat::kEacR11Signed
             ? 8
             : 16;
}

bool EncodeEtcImageToBc(EtcFormat format, const std::uint8_t* source,
                        std::size_t source_bytes, std::uint32_t width,
                        std::uint32_t height, std::uint8_t* destination,
                        std::size_t destination_bytes) {
  return EncodeEtcImageToBcBlockRows(format, source, source_bytes, width,
                                     height, 0, (height + 3) / 4, destination,
                                     destination_bytes);
}

bool EncodeEtcImageToBcBlockRows(EtcFormat format, const std::uint8_t* source,
                                 std::size_t source_bytes, std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t first_block_row,
                                 std::uint32_t block_row_count,
                                 std::uint8_t* destination,
                                 std::size_t destination_bytes) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0) {
    return false;
  }
  const std::uint64_t blocks_wide = (static_cast<std::uint64_t>(width) + 3) / 4;
  const std::uint64_t blocks_high =
      (static_cast<std::uint64_t>(height) + 3) / 4;
  const std::size_t etc_bytes = EtcBlockBytes(format);
  const std::size_t bc_bytes = BcBlockBytes(format);
  const std::uint64_t end_block_row =
      static_cast<std::uint64_t>(first_block_row) + block_row_count;
  if (blocks_wide * blocks_high * etc_bytes > source_bytes ||
      blocks_wide * blocks_high * bc_bytes > destination_bytes ||
      end_block_row > blocks_high) {
    return false;
  }
  for (std::uint64_t by = first_block_row; by < end_block_row; ++by) {
    for (std::uint64_t bx = 0; bx < blocks_wide; ++bx) {
      uint valid = 0;
      for (std::uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (std::uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          valid |= 1u << (y * 4 + x);
        }
      }
      const std::uint64_t block = by * blocks_wide + bx;
      if (IsColor(format)) {
        EncodeColorBlock(format, source + block * etc_bytes, valid,
                         destination + block * bc_bytes);
      } else {
        EncodeScalarBlocks(format, source + block * etc_bytes, valid,
                           destination + block * bc_bytes);
      }
    }
  }
  return true;
}

}  // namespace blox7::graphics
