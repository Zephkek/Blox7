#ifndef BLOX7_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
#define BLOX7_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_

#include <cstdint>
#include <vector>

namespace libc_shim {

void ConvertEtc1RgbBlockToBc1(const uint8_t *etc1, uint8_t *bc1);

bool IsBundledSkyTexture(const char *path);

bool TranscodeEtc1SkyTextureForVulkan(const char *path,
                                      std::vector<unsigned char> *data);

} // namespace libc_shim

#endif // BLOX7_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
