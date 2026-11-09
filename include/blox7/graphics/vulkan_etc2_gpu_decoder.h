#ifndef BLOX7_GRAPHICS_VULKAN_ETC2_GPU_DECODER_H_
#define BLOX7_GRAPHICS_VULKAN_ETC2_GPU_DECODER_H_

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "blox7/graphics/etc2_decoder.h"

namespace blox7::graphics {

inline constexpr VkDeviceSize kEtc2GpuTargetAlignment = 16;

bool Etc2GpuDecodable(EtcFormat format, std::uint32_t width);

struct Etc2GpuUpload {
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize source_row_stride = 0;
  VkDeviceSize source_layer_stride = 0;
  VkBuffer target = VK_NULL_HANDLE;
  VkDeviceSize target_offset = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 1;
};

struct Etc2GpuCopy {
  const std::uint8_t* data = nullptr;
  VkDeviceSize size = 0;
  VkBuffer target = VK_NULL_HANDLE;
  VkDeviceSize target_offset = 0;
};

class Etc2GpuDecoder final {
 public:
  static std::unique_ptr<Etc2GpuDecoder> Create(
      VkDevice device, const VkPhysicalDeviceLimits& limits,
      const VkPhysicalDeviceMemoryProperties& memory,
      const VkQueueFamilyProperties* families, std::uint32_t family_count,
      PFN_vkGetDeviceProcAddr get_device_proc_addr);
  ~Etc2GpuDecoder();
  Etc2GpuDecoder(const Etc2GpuDecoder&) = delete;
  Etc2GpuDecoder& operator=(const Etc2GpuDecoder&) = delete;

  bool DecodesOn(std::uint32_t family) const;
  bool CopiesOn(std::uint32_t family) const;
  bool Accepts(const Etc2GpuUpload& upload) const;
  bool Decode(VkQueue queue, std::uint32_t family,
              const Etc2GpuUpload* uploads, std::size_t count);
  bool Copy(VkQueue queue, std::uint32_t family, const Etc2GpuCopy* copies,
            std::size_t count);

  struct State;

 private:
  explicit Etc2GpuDecoder(State* state);
  State* state_;
};

}  // namespace blox7::graphics

#endif  // BLOX7_GRAPHICS_VULKAN_ETC2_GPU_DECODER_H_
