#ifndef BLOX7_GRAPHICS_ETC2_BC_ENCODER_H_
#define BLOX7_GRAPHICS_ETC2_BC_ENCODER_H_

#include <cstddef>
#include <cstdint>

#include "blox7/graphics/etc2_decoder.h"

namespace blox7::graphics {

std::size_t BcBlockBytes(EtcFormat format);

bool EncodeEtcImageToBc(EtcFormat format, const std::uint8_t* source,
                        std::size_t source_bytes, std::uint32_t width,
                        std::uint32_t height, std::uint8_t* destination,
                        std::size_t destination_bytes);

using EtcToBcEncoder = bool (*)(EtcFormat format, const std::uint8_t* source,
                                std::size_t source_bytes, std::uint32_t width,
                                std::uint32_t height, std::uint8_t* destination,
                                std::size_t destination_bytes);

bool EncodeEtcImageToBcBlockRows(EtcFormat format, const std::uint8_t* source,
                                 std::size_t source_bytes, std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t first_block_row,
                                 std::uint32_t block_row_count,
                                 std::uint8_t* destination,
                                 std::size_t destination_bytes);

}  // namespace blox7::graphics

#endif  // BLOX7_GRAPHICS_ETC2_BC_ENCODER_H_
