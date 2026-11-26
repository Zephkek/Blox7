#include "blox7/graphics/vulkan_etc2_emulation.h"

#include "blox7/graphics/etc2_bc_encoder.h"
#include "blox7/graphics/texture_override.h"
#include "blox7/graphics/vulkan_etc2_gpu_decoder.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace blox7::graphics {

bool LookupEmulatedEtc2Format(VkFormat format, Etc2EmulatedFormat* out) {
  Etc2EmulatedFormat mapped;
  switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8,
                VK_FORMAT_BC7_UNORM_BLOCK};
      break;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8,
                VK_FORMAT_BC7_SRGB_BLOCK};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8A1,
                VK_FORMAT_BC7_UNORM_BLOCK};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8A1,
                VK_FORMAT_BC7_SRGB_BLOCK};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgba8,
                VK_FORMAT_BC7_UNORM_BLOCK};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgba8,
                VK_FORMAT_BC7_SRGB_BLOCK};
      break;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16_UNORM, EtcFormat::kEacR11,
                VK_FORMAT_BC4_UNORM_BLOCK};
      break;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16_SNORM, EtcFormat::kEacR11Signed,
                VK_FORMAT_BC4_SNORM_BLOCK};
      break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_UNORM, EtcFormat::kEacRg11,
                VK_FORMAT_BC5_UNORM_BLOCK};
      break;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_SNORM, EtcFormat::kEacRg11Signed,
                VK_FORMAT_BC5_SNORM_BLOCK};
      break;
    default:
      return false;
  }
  if (out != nullptr) {
    *out = mapped;
  }
  return true;
}

VkFormatFeatureFlags EmulatedEtc2FormatFeatures(
    VkFormatFeatureFlags host_features) {
  constexpr VkFormatFeatureFlags kCompressedFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return host_features & kCompressedFeatures;
}

bool Etc2UploadByteCounts(EtcFormat format, const VkExtent3D& extent,
                          std::uint32_t layer_count, VkDeviceSize* compressed,
                          VkDeviceSize* decoded) {
  if (extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
      layer_count == 0 || compressed == nullptr || decoded == nullptr) {
    return false;
  }
  const VkDeviceSize blocks =
      ((static_cast<VkDeviceSize>(extent.width) + 3) / 4) *
      ((static_cast<VkDeviceSize>(extent.height) + 3) / 4);
  *compressed = blocks * EtcBlockBytes(format) * layer_count;
  *decoded = static_cast<VkDeviceSize>(extent.width) * extent.height *
             EtcDecodedTexelBytes(format) * layer_count;
  return true;
}

namespace {

bool EmulationDisabledByEnvironment() {
  static const bool disabled = [] {
    const char* value = std::getenv("BLOX7_DISABLE_ETC2_EMULATION");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return disabled;
}

}  // namespace

bool Etc2SupportAdvertised(const char* value) {
  return value == nullptr || std::strcmp(value, "0") != 0;
}

bool Etc2SupportAdvertised() {
  static const bool advertised =
      Etc2SupportAdvertised(std::getenv("BLOX7_ADVERTISE_ETC2"));
  return advertised;
}

bool Etc2StoredAsBc(const char* value) {
  return value != nullptr && std::strcmp(value, "bc") == 0;
}

bool Etc2StoredAsBc() {
  static const bool bc = Etc2StoredAsBc(std::getenv("BLOX7_ETC2_STORAGE"));
  return bc;
}

std::uint32_t SmallTextureUpscale(const char* value) {
  if (value == nullptr || value[0] < '0' || value[0] > '9') {
    return kDefaultSmallTextureUpscale;
  }
  const long parsed = std::strtol(value, nullptr, 10);
  if (parsed < 1) {
    return 1;
  }
  return parsed > 8 ? 8 : static_cast<std::uint32_t>(parsed);
}

namespace {

__attribute__((target("sse4.1"))) void CopyFromMapped(std::uint8_t* destination, const std::uint8_t* source,
                                                      std::size_t size) {
  while (size != 0 && (reinterpret_cast<std::uintptr_t>(source) & 15) != 0) {
    *destination++ = *source++;
    --size;
  }
  auto* in = reinterpret_cast<__m128i*>(const_cast<std::uint8_t*>(source));
  for (; size >= 64; size -= 64, in += 4, destination += 64) {
    const __m128i a = _mm_stream_load_si128(in);
    const __m128i b = _mm_stream_load_si128(in + 1);
    const __m128i c = _mm_stream_load_si128(in + 2);
    const __m128i d = _mm_stream_load_si128(in + 3);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), a);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 16), b);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 32), c);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + 48), d);
  }
  std::memcpy(destination, in, size);
}

bool ShouldLog(std::atomic<unsigned>* counter, unsigned limit) {
  return counter->fetch_add(1, std::memory_order_relaxed) < limit;
}

bool EncodeRows(const EtcDecodeJob& job, std::uint32_t first_block_row,
                std::uint32_t block_row_count) {
  return EncodeEtcImageToBcBlockRows(
      job.format, job.source, job.source_bytes, job.width, job.height,
      first_block_row, block_row_count, job.destination, job.destination_bytes);
}

unsigned DecodeWorkerCount() {
  static const unsigned count =
      std::min(8U, std::max(1U, std::thread::hardware_concurrency() / 2));
  return count;
}

struct StagingMemory {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  std::uint8_t* mapped = nullptr;
  bool host_cached = false;
};

struct StagingHole {
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
};

struct StagingBlock {
  StagingMemory memory;
  VkDeviceSize used = 0;
  std::vector<StagingHole> holes;
};

struct StagingPool {
  std::mutex mutex;
  std::vector<StagingBlock> blocks[2];
};

struct HostDevice {
  VkDevice device = VK_NULL_HANDLE;
  bool emulated = false;
  bool bc = false;
  VkPhysicalDeviceMemoryProperties memory{};
  PFN_vkCreateImage create_image = nullptr;
  PFN_vkDestroyImage destroy_image = nullptr;
  PFN_vkCreateImageView create_image_view = nullptr;
  PFN_vkGetDeviceImageMemoryRequirements image_requirements = nullptr;
  PFN_vkGetDeviceImageSparseMemoryRequirements image_sparse_requirements =
      nullptr;
  PFN_vkGetDeviceImageSubresourceLayout image_subresource_layout = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
  PFN_vkBindBufferMemory2 bind_buffer_memory2 = nullptr;
  PFN_vkMapMemory map_memory = nullptr;
  PFN_vkMapMemory2 map_memory2 = nullptr;
  PFN_vkUnmapMemory unmap_memory = nullptr;
  PFN_vkUnmapMemory2 unmap_memory2 = nullptr;
  PFN_vkFreeMemory free_memory = nullptr;
  PFN_vkDestroyBuffer destroy_buffer = nullptr;
  PFN_vkCmdCopyBufferToImage copy_buffer_to_image = nullptr;
  PFN_vkCmdCopyBufferToImage2 copy_buffer_to_image2 = nullptr;
  PFN_vkCmdCopyImage copy_image = nullptr;
  PFN_vkCmdCopyImage2 copy_image2 = nullptr;
  PFN_vkCmdCopyImageToBuffer copy_image_to_buffer = nullptr;
  PFN_vkCmdCopyImageToBuffer2 copy_image_to_buffer2 = nullptr;
  PFN_vkCmdBlitImage blit_image = nullptr;
  PFN_vkCmdExecuteCommands execute_commands = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
  std::unique_ptr<Etc2GpuDecoder> gpu;
  mutable StagingPool staging;
};

template <typename Function>
Function DeviceProc(PFN_vkGetDeviceProcAddr get, VkDevice device,
                    const char* name, const char* alias = nullptr) {
  PFN_vkVoidFunction proc = get(device, name);
  if (proc == nullptr && alias != nullptr) {
    proc = get(device, alias);
  }
  return reinterpret_cast<Function>(proc);
}

struct BufferBinding {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};

struct Mapping {
  void* data = nullptr;
  VkDeviceSize offset = 0;
  VkDeviceSize size = VK_WHOLE_SIZE;
};

struct ImageRecord {
  VkDevice device = VK_NULL_HANDLE;
  Etc2EmulatedFormat format;
  std::uint32_t scale = 1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::shared_ptr<const RgbaImage> override;
  std::shared_ptr<const RgbaImage> base;
  bool bc = false;
};

bool IsColorFormat(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

bool HostImageInfo(std::uint32_t upscale, bool bc,
                   const VkImageCreateInfo& info, VkImageCreateInfo* host,
                   Etc2EmulatedFormat* format, std::uint32_t* scale) {
  if (!LookupEmulatedEtc2Format(info.format, format)) {
    return false;
  }
  *host = info;
  host->format = bc ? format->bc_format : format->host_format;
  host->flags &= ~static_cast<VkImageCreateFlags>(
      VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT);
  *scale = 1;
  if (!bc && upscale > 1 && IsColorFormat(format->etc_format) &&
      info.imageType == VK_IMAGE_TYPE_2D && info.arrayLayers == 1 &&
      info.extent.depth == 1 && info.extent.width <= kSmallTextureMaxExtent &&
      info.extent.height <= kSmallTextureMaxExtent) {
    *scale = upscale;
    host->extent.width *= upscale;
    host->extent.height *= upscale;
  }
  return true;
}

struct Staging {
  VkDevice device = VK_NULL_HANDLE;
  StagingMemory memory;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
  bool host = false;
  bool dedicated = false;
};

struct PendingUpload {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize source_row_stride = 0;
  VkDeviceSize source_layer_stride = 0;
  VkDeviceSize source_span = 0;
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  VkDeviceSize target_offset = 0;
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  VkImage image = VK_NULL_HANDLE;
  std::uint32_t mip_level = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 0;
  std::uint32_t scale = 1;
  std::uint32_t source_width = 0;
  std::uint32_t source_height = 0;
  std::uint32_t target_width = 0;
  std::uint32_t target_height = 0;
  bool bc = false;
  bool full_mip = false;
  VkDeviceSize target_bytes = 0;
  std::uint8_t* target = nullptr;
  bool target_cached = false;
  VkBuffer target_buffer = VK_NULL_HANDLE;
  bool gpu = false;
};

struct CommandRecord {
  std::vector<PendingUpload> uploads;
  std::vector<Staging> staging;
  std::vector<VkCommandBuffer> secondaries;
};

std::atomic<unsigned> g_image_logs{0};
std::atomic<unsigned> g_decode_logs{0};
std::atomic<unsigned> g_gpu_decode_logs{0};
std::atomic<unsigned> g_failure_logs{0};

void LogFailure(const char* message) {
  if (ShouldLog(&g_failure_logs, 8)) {
    std::fprintf(stderr, "  [vulkan] ETC2 emulation: %s\n", message);
  }
}

bool CreateStaging(const HostDevice& dev, VkDeviceSize size,
                   StagingMemory* out) {
  if (dev.create_buffer == nullptr || dev.allocate_memory == nullptr ||
      dev.get_buffer_memory_requirements == nullptr ||
      dev.bind_buffer_memory == nullptr || dev.map_memory == nullptr ||
      dev.destroy_buffer == nullptr || dev.free_memory == nullptr ||
      size == 0) {
    return false;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (dev.create_buffer(dev.device, &buffer_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dev.get_buffer_memory_requirements(dev.device, buffer, &requirements);
  constexpr VkMemoryPropertyFlags kRequired =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  std::uint32_t type = UINT32_MAX;
  for (std::uint32_t index = 0; index < dev.memory.memoryTypeCount; ++index) {
    if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
        (dev.memory.memoryTypes[index].propertyFlags & kRequired) ==
            kRequired) {
      if (type == UINT32_MAX) {
        type = index;
      }
      if ((dev.memory.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
        type = index;
        break;
      }
    }
  }
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkMemoryAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = type;
  void* data = nullptr;
  if (type == UINT32_MAX ||
      dev.allocate_memory(dev.device, &allocate_info, nullptr, &memory) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    return false;
  }
  if (dev.bind_buffer_memory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
      dev.map_memory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &data) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    dev.free_memory(dev.device, memory, nullptr);
    return false;
  }
  *out = {buffer, memory, static_cast<std::uint8_t*>(data),
          (dev.memory.memoryTypes[type].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0};
  return true;
}

bool CreateDeviceStaging(const HostDevice& dev, VkDeviceSize size,
                         StagingMemory* out) {
  if (dev.create_buffer == nullptr || dev.allocate_memory == nullptr ||
      dev.get_buffer_memory_requirements == nullptr ||
      dev.bind_buffer_memory == nullptr || dev.destroy_buffer == nullptr ||
      dev.free_memory == nullptr || size == 0) {
    return false;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (dev.create_buffer(dev.device, &buffer_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dev.get_buffer_memory_requirements(dev.device, buffer, &requirements);
  constexpr VkMemoryPropertyFlags kAvoided =
      VK_MEMORY_PROPERTY_PROTECTED_BIT |
      VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
      VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
      VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD |
      VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  for (int rank = 0; rank < 2 && memory == VK_NULL_HANDLE; ++rank) {
    for (std::uint32_t index = 0;
         index < dev.memory.memoryTypeCount && memory == VK_NULL_HANDLE;
         ++index) {
      const VkMemoryPropertyFlags flags =
          dev.memory.memoryTypes[index].propertyFlags;
      const bool visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
      if ((requirements.memoryTypeBits & (1U << index)) == 0 ||
          (flags & kAvoided) != 0 ||
          (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0 ||
          visible != (rank == 1)) {
        continue;
      }
      VkMemoryAllocateInfo allocate_info{};
      allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocate_info.allocationSize = requirements.size;
      allocate_info.memoryTypeIndex = index;
      VkDeviceMemory candidate = VK_NULL_HANDLE;
      if (dev.allocate_memory(dev.device, &allocate_info, nullptr,
                              &candidate) == VK_SUCCESS) {
        memory = candidate;
      }
    }
  }
  if (memory == VK_NULL_HANDLE ||
      dev.bind_buffer_memory(dev.device, buffer, memory, 0) != VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) {
      dev.free_memory(dev.device, memory, nullptr);
    }
    return false;
  }
  *out = {buffer, memory, nullptr, false};
  return true;
}

bool CreateMemory(const HostDevice& dev, bool host, VkDeviceSize size,
                  StagingMemory* out) {
  return host ? CreateStaging(dev, size, out)
              : CreateDeviceStaging(dev, size, out);
}

void DestroyMemory(const HostDevice& dev, const StagingMemory& memory) {
  if (dev.destroy_buffer != nullptr) {
    dev.destroy_buffer(dev.device, memory.buffer, nullptr);
  }
  if (dev.free_memory != nullptr) {
    dev.free_memory(dev.device, memory.memory, nullptr);
  }
}

bool TakeRange(StagingBlock* block, VkDeviceSize size, VkDeviceSize* offset) {
  for (auto hole = block->holes.begin(); hole != block->holes.end(); ++hole) {
    if (hole->size < size) {
      continue;
    }
    *offset = hole->offset;
    hole->offset += size;
    hole->size -= size;
    if (hole->size == 0) {
      block->holes.erase(hole);
    }
    block->used += size;
    return true;
  }
  return false;
}

void ReturnRange(StagingBlock* block, VkDeviceSize offset, VkDeviceSize size) {
  std::vector<StagingHole>& holes = block->holes;
  const auto next = std::find_if(
      holes.begin(), holes.end(),
      [offset](const StagingHole& hole) { return hole.offset > offset; });
  const bool joins_previous =
      next != holes.begin() &&
      std::prev(next)->offset + std::prev(next)->size == offset;
  const bool joins_next = next != holes.end() && offset + size == next->offset;
  if (joins_previous && joins_next) {
    std::prev(next)->size += size + next->size;
    holes.erase(next);
  } else if (joins_previous) {
    std::prev(next)->size += size;
  } else if (joins_next) {
    next->offset = offset;
    next->size += size;
  } else {
    holes.insert(next, {offset, size});
  }
  block->used -= size;
}

bool AcquireStaging(const HostDevice& dev, bool host, VkDeviceSize bytes,
                    Staging* staging) {
  const VkDeviceSize size = (bytes + kEtc2StagingAlignment - 1) /
                            kEtc2StagingAlignment * kEtc2StagingAlignment;
  if (size == 0) {
    return false;
  }
  if (size <= kEtc2StagingBlockBytes) {
    {
      std::lock_guard<std::mutex> lock(dev.staging.mutex);
      for (StagingBlock& block : dev.staging.blocks[host]) {
        VkDeviceSize offset = 0;
        if (TakeRange(&block, size, &offset)) {
          *staging = {dev.device, block.memory, offset, size, host, false};
          return true;
        }
      }
    }
    StagingBlock block;
    if (CreateMemory(dev, host, kEtc2StagingBlockBytes, &block.memory)) {
      block.used = size;
      if (size < kEtc2StagingBlockBytes) {
        block.holes.push_back({size, kEtc2StagingBlockBytes - size});
      }
      *staging = {dev.device, block.memory, 0, size, host, false};
      std::lock_guard<std::mutex> lock(dev.staging.mutex);
      dev.staging.blocks[host].push_back(std::move(block));
      return true;
    }
  }
  StagingMemory memory;
  if (!CreateMemory(dev, host, size, &memory)) {
    return false;
  }
  *staging = {dev.device, memory, 0, size, host, true};
  return true;
}

void ReleaseStaging(const HostDevice& dev, const Staging& staging) {
  if (staging.dedicated) {
    DestroyMemory(dev, staging.memory);
    return;
  }
  std::vector<StagingBlock> idle;
  {
    std::lock_guard<std::mutex> lock(dev.staging.mutex);
    std::vector<StagingBlock>& blocks = dev.staging.blocks[staging.host];
    const auto owner = std::find_if(
        blocks.begin(), blocks.end(), [&staging](const StagingBlock& block) {
          return block.memory.memory == staging.memory.memory;
        });
    if (owner == blocks.end()) {
      return;
    }
    ReturnRange(&*owner, staging.offset, staging.size);
    if (owner->used != 0) {
      return;
    }
    bool kept = false;
    for (auto block = blocks.begin(); block != blocks.end();) {
      if (block->used == 0 && kept) {
        idle.push_back(std::move(*block));
        block = blocks.erase(block);
      } else {
        kept = kept || block->used == 0;
        ++block;
      }
    }
  }
  for (const StagingBlock& block : idle) {
    DestroyMemory(dev, block.memory);
  }
}

void ReleaseAllStaging(const HostDevice& dev) {
  std::vector<StagingBlock> doomed;
  {
    std::lock_guard<std::mutex> lock(dev.staging.mutex);
    for (std::vector<StagingBlock>& blocks : dev.staging.blocks) {
      for (StagingBlock& block : blocks) {
        doomed.push_back(std::move(block));
      }
      blocks.clear();
    }
  }
  for (const StagingBlock& block : doomed) {
    DestroyMemory(dev, block.memory);
  }
}

template <typename Region>
bool PrepareStaging(const HostDevice& dev, bool gpu, VkDeviceSize total,
                    Staging* staging, std::vector<PendingUpload>* uploads,
                    std::vector<Region>* regions) {
  const bool host = !gpu || !AcquireStaging(dev, false, total, staging);
  if (host && !AcquireStaging(dev, true, total, staging)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return false;
  }
  for (PendingUpload& upload : *uploads) {
    upload.target_offset += staging->offset;
    upload.target_buffer = staging->memory.buffer;
    if (host) {
      upload.target = staging->memory.mapped + upload.target_offset;
      upload.target_cached = staging->memory.host_cached;
      upload.gpu = false;
    }
  }
  for (Region& region : *regions) {
    region.bufferOffset += staging->offset;
  }
  return true;
}

bool ScaledAxis(std::uint32_t base, std::uint32_t scale, std::uint32_t mip,
                std::int32_t offset, std::uint32_t extent,
                std::uint32_t* texels, std::uint32_t* host_start,
                std::uint32_t* host_end) {
  const std::uint32_t app_mip = std::max<std::uint32_t>(1, base >> mip);
  const std::uint32_t host_mip =
      std::max<std::uint32_t>(1, (base * scale) >> mip);
  if (offset < 0 || static_cast<std::uint32_t>(offset) >= app_mip) {
    return false;
  }
  const std::uint32_t app_end =
      std::min(app_mip, static_cast<std::uint32_t>(offset) + extent);
  const std::uint32_t start = static_cast<std::uint32_t>(offset) * scale;
  const std::uint32_t end =
      app_end == app_mip ? host_mip : std::min(host_mip, app_end * scale);
  if (end <= start) {
    return false;
  }
  *texels = app_end - static_cast<std::uint32_t>(offset);
  *host_start = start;
  *host_end = end;
  return true;
}

bool HostRegion(const ImageRecord* record, std::uint32_t mip,
                VkOffset3D offset, VkExtent3D extent, VkOffset3D* start,
                VkOffset3D* end) {
  if (record == nullptr || record->scale <= 1) {
    *start = offset;
    *end = {offset.x + static_cast<std::int32_t>(extent.width),
            offset.y + static_cast<std::int32_t>(extent.height),
            offset.z + static_cast<std::int32_t>(extent.depth)};
    return true;
  }
  std::uint32_t texels = 0;
  std::uint32_t x0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t y0 = 0;
  std::uint32_t y1 = 0;
  if (!ScaledAxis(record->width, record->scale, mip, offset.x, extent.width,
                  &texels, &x0, &x1) ||
      !ScaledAxis(record->height, record->scale, mip, offset.y,
                  extent.height, &texels, &y0, &y1)) {
    return false;
  }
  *start = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), 0};
  *end = {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1), 1};
  return true;
}

template <typename Region>
bool PlanRegions(const ImageRecord& record, VkDevice device, VkBuffer source,
                 VkImage destination, std::uint32_t count,
                 const Region* regions, bool gpu,
                 std::vector<Region>* rewritten,
                 std::vector<PendingUpload>* uploads, VkDeviceSize* total) {
  *total = 0;
  if (regions == nullptr || count == 0) {
    return false;
  }
  const std::uint32_t scale = record.scale;
  for (std::uint32_t index = 0; index < count; ++index) {
    const Region& region = regions[index];
    const VkDeviceSize alignment =
        gpu || record.bc ? kEtc2GpuTargetAlignment : 4;
    *total = (*total + alignment - 1) / alignment * alignment;
    PendingUpload upload;
    if (!Etc2UploadByteCounts(record.format.etc_format, region.imageExtent,
                              region.imageSubresource.layerCount,
                              &upload.compressed, &upload.decoded)) {
      return false;
    }
    if (scale > 1 && region.imageSubresource.layerCount != 1) {
      return false;
    }
    upload.bc = record.bc;
    if (record.bc) {
      upload.decoded = upload.compressed /
                       EtcBlockBytes(record.format.etc_format) *
                       BcBlockBytes(record.format.etc_format);
    }
    upload.device = device;
    upload.source = source;
    upload.source_offset = region.bufferOffset;
    upload.target_offset = *total;
    upload.format = record.format.etc_format;
    upload.image = destination;
    upload.mip_level = region.imageSubresource.mipLevel;
    upload.width = region.imageExtent.width;
    upload.height = region.imageExtent.height;
    upload.layers = region.imageSubresource.layerCount;
    const VkDeviceSize row_texels =
        region.bufferRowLength != 0 ? region.bufferRowLength : upload.width;
    const VkDeviceSize layer_rows =
        region.bufferImageHeight != 0 ? region.bufferImageHeight : upload.height;
    if (row_texels < upload.width || layer_rows < upload.height) {
      return false;
    }
    const VkDeviceSize block_bytes = EtcBlockBytes(upload.format);
    const VkDeviceSize block_rows =
        (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
    const VkDeviceSize packed_row =
        ((static_cast<VkDeviceSize>(upload.width) + 3) / 4) * block_bytes;
    upload.source_row_stride = ((row_texels + 3) / 4) * block_bytes;
    upload.source_layer_stride =
        upload.source_row_stride * ((layer_rows + 3) / 4);
    upload.source_span = (upload.layers - 1) * upload.source_layer_stride +
                         (block_rows - 1) * upload.source_row_stride + packed_row;
    upload.scale = scale;
    upload.gpu = gpu && scale == 1 &&
                 (upload.bc || Etc2GpuDecodable(upload.format, upload.width));
    upload.source_width = upload.width;
    upload.source_height = upload.height;
    upload.target_width = upload.width;
    upload.target_height = upload.height;
    upload.target_bytes = upload.decoded;
    Region copy = region;
    copy.bufferOffset = *total;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    if (scale > 1) {
      const std::uint32_t mip = region.imageSubresource.mipLevel;
      const auto axis = [&](std::uint32_t base, std::int32_t offset,
                            std::uint32_t extent, std::uint32_t* source,
                            std::uint32_t* target, std::int32_t* host_offset,
                            std::uint32_t* host_extent) {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        if (!ScaledAxis(base, scale, mip, offset, extent, source, &start,
                        &end)) {
          return false;
        }
        *target = end - start;
        *host_offset = static_cast<std::int32_t>(start);
        *host_extent = end - start;
        return true;
      };
      if (!axis(record.width, region.imageOffset.x, region.imageExtent.width,
                &upload.source_width, &upload.target_width,
                &copy.imageOffset.x, &copy.imageExtent.width) ||
          !axis(record.height, region.imageOffset.y,
                region.imageExtent.height, &upload.source_height,
                &upload.target_height, &copy.imageOffset.y,
                &copy.imageExtent.height)) {
        return false;
      }
      upload.target_bytes = static_cast<VkDeviceSize>(upload.target_width) *
                            upload.target_height *
                            EtcDecodedTexelBytes(record.format.etc_format);
      upload.full_mip =
          region.imageOffset.x == 0 && region.imageOffset.y == 0 &&
          upload.source_width == std::max<std::uint32_t>(1, record.width >> mip) &&
          upload.source_height == std::max<std::uint32_t>(1, record.height >> mip);
    }
    rewritten->push_back(copy);
    uploads->push_back(upload);
    *total += upload.target_bytes;
  }
  return true;
}

}  // namespace

struct VulkanEtc2Emulation::State {
  std::mutex mutex;
  std::shared_mutex devices_mutex;
  std::mutex commands_mutex;
  std::mutex memory_mutex;
  std::unordered_map<VkPhysicalDevice, bool> physical_devices;
  std::unordered_map<VkPhysicalDevice, bool> bc_devices;
  std::unordered_map<VkDevice, std::shared_ptr<Etc2GpuEncoder>> encoders;
  std::vector<std::unique_ptr<HostDevice>> devices;
  std::unordered_map<VkImage, ImageRecord> images;
  std::unordered_map<VkBuffer, BufferBinding> buffers;
  std::unordered_map<VkDeviceMemory, Mapping> mappings;
  std::unordered_map<VkCommandBuffer, CommandRecord> commands;
  std::unordered_map<VkQueue, std::pair<VkDevice, std::uint32_t>> queues;
  std::atomic<bool> has_commands{false};
  TextureOverrides overrides = TextureOverrides::FromEnvironment();
  const std::uint32_t upscale =
      SmallTextureUpscale(std::getenv("BLOX7_SMALL_TEXTURE_UPSCALE"));
  const bool bc = Etc2StoredAsBc() && upscale == 1 && !overrides.enabled();

  std::shared_ptr<const RgbaImage> FindOverride(const PendingUpload& upload,
                                                const std::uint8_t* compressed,
                                                const std::uint8_t* decoded) {
    if (!overrides.enabled() || upload.layers != 1 ||
        !IsColorFormat(upload.format)) {
      return nullptr;
    }
    std::shared_ptr<const RgbaImage> replacement;
    if (upload.mip_level == 0) {
      const std::uint64_t hash = HashBytes(compressed, upload.compressed);
      overrides.Dump(hash, upload.width, upload.height, decoded);
      replacement = overrides.Lookup(hash);
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        record->second.override = replacement;
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        replacement = record->second.override;
      }
    }
    return replacement;
  }

  void EmitUpload(const PendingUpload& upload, const std::uint8_t* compressed,
                  const std::uint8_t* decoded) {
    const std::shared_ptr<const RgbaImage> replacement =
        FindOverride(upload, compressed, decoded);
    if (replacement != nullptr) {
      ResampleRgba(*replacement, upload.target_width, upload.target_height,
                   upload.target);
    } else if (upload.scale > 1) {
      std::shared_ptr<const RgbaImage> base;
      if (upload.full_mip) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = images.find(upload.image);
        if (record != images.end()) {
          if (upload.mip_level == 0) {
            auto level0 = std::make_shared<RgbaImage>();
            level0->width = upload.source_width;
            level0->height = upload.source_height;
            level0->pixels.assign(decoded, decoded + upload.decoded);
            if (upload.source_width != upload.width ||
                upload.source_height != upload.height) {
              level0->pixels.clear();
              for (std::uint32_t y = 0; y < upload.source_height; ++y) {
                const std::uint8_t* row =
                    decoded + static_cast<std::size_t>(y) * upload.width * 4;
                level0->pixels.insert(level0->pixels.end(), row,
                                      row + upload.source_width * 4);
              }
            }
            record->second.base = level0;
          }
          base = record->second.base;
        }
      }
      if (base != nullptr) {
        ResampleRgba(*base, upload.target_width, upload.target_height,
                     upload.target);
        return;
      }
      const std::uint8_t* source = decoded;
      std::vector<std::uint8_t> cropped;
      if (upload.source_width != upload.width ||
          upload.source_height != upload.height) {
        const std::size_t row = static_cast<std::size_t>(upload.source_width) * 4;
        cropped.resize(row * upload.source_height);
        for (std::uint32_t y = 0; y < upload.source_height; ++y) {
          std::memcpy(cropped.data() + y * row,
                      decoded + static_cast<std::size_t>(y) * upload.width * 4,
                      row);
        }
        source = cropped.data();
      }
      ResampleRgba(source, upload.source_width, upload.source_height,
                   upload.target_width, upload.target_height, upload.target);
    } else {
      std::memcpy(upload.target, decoded, upload.decoded);
    }
  }

  const HostDevice* Find(VkDevice device) {
    std::shared_lock<std::shared_mutex> lock(devices_mutex);
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  bool BlitScaledCopy(const HostDevice& dev, VkCommandBuffer command_buffer,
                      VkImage source, VkImageLayout source_layout,
                      VkImage destination, VkImageLayout destination_layout,
                      std::uint32_t count, const VkImageCopy* regions);
  bool MixedCopy(const HostDevice& dev, VkImage source, VkImage destination);

  bool LookupImage(const HostDevice& dev, VkImage image,
                   ImageRecord* record) {
    if (!dev.emulated) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(image);
    if (found == images.end()) {
      return false;
    }
    *record = found->second;
    return true;
  }

  void Record(VkCommandBuffer command_buffer,
              std::vector<PendingUpload> uploads, const Staging& staging) {
    std::lock_guard<std::mutex> lock(commands_mutex);
    CommandRecord& record = commands[command_buffer];
    record.uploads.insert(record.uploads.end(), uploads.begin(),
                          uploads.end());
    record.staging.push_back(staging);
    has_commands.store(true, std::memory_order_release);
  }

  struct Route {
    const HostDevice* dev = nullptr;
    std::uint32_t family = 0;
  };

  Route LookupQueue(VkQueue queue) {
    Route route;
    VkDevice device = VK_NULL_HANDLE;
    {
      std::shared_lock<std::shared_mutex> lock(devices_mutex);
      const auto found = queues.find(queue);
      if (found == queues.end()) {
        return route;
      }
      device = found->second.first;
      route.family = found->second.second;
    }
    route.dev = Find(device);
    return route;
  }

  void DecodeOnGpu(const Route& route, VkQueue queue,
                   std::vector<PendingUpload>* uploads) {
    if (route.dev == nullptr || route.dev->gpu == nullptr ||
        !route.dev->gpu->DecodesOn(route.family)) {
      return;
    }
    std::vector<Etc2GpuUpload> work;
    std::vector<bool> picked(uploads->size(), false);
    for (std::size_t index = 0; index < uploads->size(); ++index) {
      const PendingUpload& upload = (*uploads)[index];
      if (!upload.gpu || upload.device != route.dev->device) {
        continue;
      }
      Etc2GpuUpload item;
      item.format = upload.format;
      item.source = upload.source;
      item.source_offset = upload.source_offset;
      item.source_row_stride = upload.source_row_stride;
      item.source_layer_stride = upload.source_layer_stride;
      item.target = upload.target_buffer;
      item.target_offset = upload.target_offset;
      item.width = upload.width;
      item.height = upload.height;
      item.layers = upload.layers;
      item.bc = upload.bc;
      if (route.dev->gpu->Accepts(item)) {
        work.push_back(item);
        picked[index] = true;
      }
    }
    if (work.empty()) {
      return;
    }
    if (!route.dev->gpu->Decode(queue, route.family, work.data(),
                                work.size())) {
      LogFailure(route.dev->bc ? "GPU encode failed, encoding on the CPU"
                               : "GPU decode failed, decoding on the CPU");
      return;
    }
    for (const Etc2GpuUpload& item : work) {
      if (ShouldLog(&g_gpu_decode_logs, 4)) {
        std::fprintf(stderr,
                     "  [vulkan] ETC2 upload %s on the GPU %ux%u "
                     "layers=%u\n",
                     item.bc ? "encoded to BC" : "decoded", item.width,
                     item.height, item.layers);
      }
    }
    std::size_t kept = 0;
    for (std::size_t index = 0; index < uploads->size(); ++index) {
      if (!picked[index]) {
        (*uploads)[kept++] = (*uploads)[index];
      }
    }
    uploads->resize(kept);
  }

  void CopyToDevice(const Route& route, VkQueue queue,
                    const std::vector<Etc2GpuCopy>& copies) {
    if (route.dev == nullptr || route.dev->gpu == nullptr ||
        !route.dev->gpu->Copy(queue, route.family, copies.data(),
                              copies.size())) {
      LogFailure("could not copy CPU-decoded uploads to device staging");
    }
  }
};

VulkanEtc2Emulation::VulkanEtc2Emulation() : state_(new State) {}

VulkanEtc2Emulation::~VulkanEtc2Emulation() {
  for (auto& [device, encoder] : state_->encoders) {
    Etc2GpuEncoder::KeepUntilExit(std::move(encoder));
  }
  delete state_;
}

bool VulkanEtc2Emulation::PhysicalDeviceNeedsEmulation(
    VkPhysicalDevice physical_device, PFN_vkGetPhysicalDeviceFeatures host) {
  if (EmulationDisabledByEnvironment() || host == nullptr ||
      physical_device == VK_NULL_HANDLE) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->physical_devices.find(physical_device);
    if (found != state_->physical_devices.end()) {
      return found->second;
    }
  }
  VkPhysicalDeviceFeatures features{};
  host(physical_device, &features);
  const bool needed = features.textureCompressionETC2 != VK_TRUE;
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->physical_devices[physical_device] = needed;
  state_->bc_devices[physical_device] =
      needed && state_->bc && features.textureCompressionBC == VK_TRUE;
  return needed;
}

bool VulkanEtc2Emulation::PhysicalDeviceStoresBc(
    VkPhysicalDevice physical_device, PFN_vkGetPhysicalDeviceFeatures host) {
  if (!state_->bc || !PhysicalDeviceNeedsEmulation(physical_device, host)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->bc_devices.find(physical_device);
  return found != state_->bc_devices.end() && found->second;
}

void VulkanEtc2Emulation::RegisterDevice(
    VkDevice device, VkPhysicalDevice physical_device, bool emulated,
    const VkPhysicalDeviceMemoryProperties& memory,
    PFN_vkGetDeviceProcAddr get_device_proc_addr,
    const VkPhysicalDeviceLimits* limits,
    const VkQueueFamilyProperties* queue_families,
    std::uint32_t queue_family_count, bool bc, EtcToBcEncoder bc_reference) {
  static_cast<void>(physical_device);
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  auto dev = std::make_unique<HostDevice>();
  const auto get = get_device_proc_addr;
  dev->device = device;
  dev->emulated = emulated;
  dev->bc = emulated && bc;
  dev->memory = memory;
  dev->create_image = DeviceProc<PFN_vkCreateImage>(get, device, "vkCreateImage");
  dev->destroy_image =
      DeviceProc<PFN_vkDestroyImage>(get, device, "vkDestroyImage");
  dev->create_image_view =
      DeviceProc<PFN_vkCreateImageView>(get, device, "vkCreateImageView");
  dev->image_requirements = DeviceProc<PFN_vkGetDeviceImageMemoryRequirements>(
      get, device, "vkGetDeviceImageMemoryRequirements",
      "vkGetDeviceImageMemoryRequirementsKHR");
  dev->image_sparse_requirements =
      DeviceProc<PFN_vkGetDeviceImageSparseMemoryRequirements>(
          get, device, "vkGetDeviceImageSparseMemoryRequirements",
          "vkGetDeviceImageSparseMemoryRequirementsKHR");
  dev->image_subresource_layout =
      DeviceProc<PFN_vkGetDeviceImageSubresourceLayout>(
          get, device, "vkGetDeviceImageSubresourceLayout",
          "vkGetDeviceImageSubresourceLayoutKHR");
  dev->bind_buffer_memory =
      DeviceProc<PFN_vkBindBufferMemory>(get, device, "vkBindBufferMemory");
  dev->bind_buffer_memory2 = DeviceProc<PFN_vkBindBufferMemory2>(
      get, device, "vkBindBufferMemory2", "vkBindBufferMemory2KHR");
  dev->map_memory = DeviceProc<PFN_vkMapMemory>(get, device, "vkMapMemory");
  dev->map_memory2 = DeviceProc<PFN_vkMapMemory2>(get, device, "vkMapMemory2",
                                                  "vkMapMemory2KHR");
  dev->unmap_memory =
      DeviceProc<PFN_vkUnmapMemory>(get, device, "vkUnmapMemory");
  dev->unmap_memory2 = DeviceProc<PFN_vkUnmapMemory2>(
      get, device, "vkUnmapMemory2", "vkUnmapMemory2KHR");
  dev->free_memory = DeviceProc<PFN_vkFreeMemory>(get, device, "vkFreeMemory");
  dev->destroy_buffer =
      DeviceProc<PFN_vkDestroyBuffer>(get, device, "vkDestroyBuffer");
  dev->copy_buffer_to_image = DeviceProc<PFN_vkCmdCopyBufferToImage>(
      get, device, "vkCmdCopyBufferToImage");
  dev->copy_buffer_to_image2 = DeviceProc<PFN_vkCmdCopyBufferToImage2>(
      get, device, "vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR");
  dev->copy_image = DeviceProc<PFN_vkCmdCopyImage>(get, device, "vkCmdCopyImage");
  dev->copy_image2 = DeviceProc<PFN_vkCmdCopyImage2>(
      get, device, "vkCmdCopyImage2", "vkCmdCopyImage2KHR");
  dev->copy_image_to_buffer = DeviceProc<PFN_vkCmdCopyImageToBuffer>(
      get, device, "vkCmdCopyImageToBuffer");
  dev->copy_image_to_buffer2 = DeviceProc<PFN_vkCmdCopyImageToBuffer2>(
      get, device, "vkCmdCopyImageToBuffer2", "vkCmdCopyImageToBuffer2KHR");
  dev->blit_image = DeviceProc<PFN_vkCmdBlitImage>(get, device, "vkCmdBlitImage");
  dev->execute_commands =
      DeviceProc<PFN_vkCmdExecuteCommands>(get, device, "vkCmdExecuteCommands");
  dev->create_buffer =
      DeviceProc<PFN_vkCreateBuffer>(get, device, "vkCreateBuffer");
  dev->allocate_memory =
      DeviceProc<PFN_vkAllocateMemory>(get, device, "vkAllocateMemory");
  dev->get_buffer_memory_requirements =
      DeviceProc<PFN_vkGetBufferMemoryRequirements>(
          get, device, "vkGetBufferMemoryRequirements");
  if (emulated) {
    std::fputs(dev->bc ? "  [vulkan] ETC2/EAC emulation enabled: compressed "
                         "uploads are stored as BC7/BC4/BC5 host images\n"
                       : "  [vulkan] ETC2/EAC emulation enabled: compressed "
                         "uploads decode to RGBA8/R16 host images\n",
               stderr);
  }
  if (emulated && limits != nullptr && queue_families != nullptr &&
      !state_->overrides.enabled()) {
    std::shared_ptr<Etc2GpuEncoder> encoder;
    if (dev->bc) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      std::shared_ptr<Etc2GpuEncoder>& kept = state_->encoders[device];
      if (kept == nullptr) {
        kept = Etc2GpuEncoder::Start(device, get, bc_reference);
      }
      encoder = kept;
    }
    dev->gpu = Etc2GpuDecoder::Create(device, *limits, memory, queue_families,
                                      queue_family_count, get, encoder);
    if (dev->bc) {
      std::fputs(dev->gpu != nullptr
                     ? "  [vulkan] ETC2/EAC uploads are encoded to BC on the "
                       "GPU\n"
                     : "  [vulkan] ETC2/EAC uploads are encoded to BC on the "
                       "CPU: no compute encoder\n",
                 stderr);
    } else {
      std::fputs(dev->gpu != nullptr
                     ? "  [vulkan] ETC2/EAC uploads decode on the GPU\n"
                     : "  [vulkan] ETC2/EAC uploads decode on the CPU: no "
                       "compute decoder\n",
                 stderr);
    }
  }
  std::lock_guard<std::shared_mutex> lock(state_->devices_mutex);
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
  state_->devices.push_back(std::move(dev));
}

void VulkanEtc2Emulation::RegisterQueue(VkDevice device, VkQueue queue,
                                        std::uint32_t queue_family) {
  if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::shared_mutex> lock(state_->devices_mutex);
  state_->queues[queue] = {device, queue_family};
}

void VulkanEtc2Emulation::DestroyDevice(VkDevice device) {
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    for (auto it = state_->commands.begin(); it != state_->commands.end();) {
      const bool owned = std::any_of(
          it->second.staging.begin(), it->second.staging.end(),
          [device](const Staging& staging) { return staging.device == device; });
      if (owned) {
        doomed.insert(doomed.end(), it->second.staging.begin(),
                      it->second.staging.end());
        it = state_->commands.erase(it);
      } else {
        ++it;
      }
    }
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto it = state_->images.begin(); it != state_->images.end();) {
      it = it->second.device == device ? state_->images.erase(it) : std::next(it);
    }
  }
  if (const HostDevice* dev = state_->Find(device); dev != nullptr) {
    for (const Staging& staging : doomed) {
      ReleaseStaging(*dev, staging);
    }
    ReleaseAllStaging(*dev);
  }
  std::lock_guard<std::shared_mutex> lock(state_->devices_mutex);
  for (auto it = state_->queues.begin(); it != state_->queues.end();) {
    it = it->second.first == device ? state_->queues.erase(it) : std::next(it);
  }
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
}

void VulkanEtc2Emulation::ForgetDevice(VkDevice device) {
  std::shared_ptr<Etc2GpuEncoder> encoder;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->encoders.find(device);
    if (found == state_->encoders.end()) {
      return;
    }
    encoder = std::move(found->second);
    state_->encoders.erase(found);
  }
}

void VulkanEtc2Emulation::WaitForEncoder(VkDevice device) {
  std::shared_ptr<Etc2GpuEncoder> encoder;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->encoders.find(device);
    if (found != state_->encoders.end()) {
      encoder = found->second;
    }
  }
  if (encoder != nullptr) {
    encoder->Wait();
  }
}

VkResult VulkanEtc2Emulation::CreateImage(VkDevice device,
                                          const VkImageCreateInfo* create_info,
                                          const VkAllocationCallbacks* allocator,
                                          VkImage* image) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  VkImageCreateInfo host_info;
  std::uint32_t scale = 1;
  if (!dev->emulated || create_info == nullptr ||
      !HostImageInfo(state_->upscale, dev->bc, *create_info, &host_info,
                     &format, &scale)) {
    return dev->create_image(device, create_info, allocator, image);
  }
  const VkResult result = dev->create_image(device, &host_info, allocator, image);
  if (result == VK_SUCCESS && image != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->images[*image] = {device,
                                format,
                                scale,
                                create_info->extent.width,
                                create_info->extent.height,
                                nullptr,
                                nullptr,
                                dev->bc};
    }
    if (ShouldLog(&g_image_logs, 4)) {
      std::fprintf(stderr,
                   "  [vulkan] ETC2 image %ux%u mips=%u format=%d -> host "
                   "format=%d\n",
                   create_info->extent.width, create_info->extent.height,
                   create_info->mipLevels,
                   static_cast<int>(create_info->format),
                   static_cast<int>(host_info.format));
    }
  }
  return result;
}

void VulkanEtc2Emulation::DestroyImage(VkDevice device, VkImage image,
                                       const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_image == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->images.erase(image);
  }
  dev->destroy_image(device, image, allocator);
}

VkResult VulkanEtc2Emulation::CreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image_view == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  if (!dev->emulated || create_info == nullptr ||
      !LookupEmulatedEtc2Format(create_info->format, &format)) {
    return dev->create_image_view(device, create_info, allocator, view);
  }
  ImageRecord record;
  VkImageViewCreateInfo host_info = *create_info;
  host_info.format =
      state_->LookupImage(*dev, create_info->image, &record) && record.bc
          ? format.bc_format
          : format.host_format;
  if (format.etc_format == EtcFormat::kEtc2Rgb8) {
    VkComponentSwizzle* const swizzles[] = {
        &host_info.components.r, &host_info.components.g,
        &host_info.components.b, &host_info.components.a};
    for (VkComponentSwizzle* swizzle : swizzles) {
      if (*swizzle == VK_COMPONENT_SWIZZLE_A) {
        *swizzle = VK_COMPONENT_SWIZZLE_ONE;
      }
    }
    if (host_info.components.a == VK_COMPONENT_SWIZZLE_IDENTITY) {
      host_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return dev->create_image_view(device, &host_info, allocator, view);
}

void VulkanEtc2Emulation::GetDeviceImageMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->image_requirements == nullptr) {
    return;
  }
  Etc2EmulatedFormat format;
  VkImageCreateInfo host_image;
  std::uint32_t scale = 1;
  if (!dev->emulated || info == nullptr || info->pCreateInfo == nullptr ||
      !HostImageInfo(state_->upscale, dev->bc, *info->pCreateInfo, &host_image,
                     &format, &scale)) {
    dev->image_requirements(device, info, requirements);
    return;
  }
  VkDeviceImageMemoryRequirements host_info = *info;
  host_info.pCreateInfo = &host_image;
  dev->image_requirements(device, &host_info, requirements);
}

void VulkanEtc2Emulation::GetDeviceImageSparseMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    std::uint32_t* count, VkSparseImageMemoryRequirements2* requirements) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->image_sparse_requirements == nullptr) {
    return;
  }
  Etc2EmulatedFormat format;
  VkImageCreateInfo host_image;
  std::uint32_t scale = 1;
  if (!dev->emulated || info == nullptr || info->pCreateInfo == nullptr ||
      !HostImageInfo(state_->upscale, dev->bc, *info->pCreateInfo, &host_image,
                     &format, &scale)) {
    dev->image_sparse_requirements(device, info, count, requirements);
    return;
  }
  VkDeviceImageMemoryRequirements host_info = *info;
  host_info.pCreateInfo = &host_image;
  dev->image_sparse_requirements(device, &host_info, count, requirements);
}

void VulkanEtc2Emulation::GetDeviceImageSubresourceLayout(
    VkDevice device, const VkDeviceImageSubresourceInfo* info,
    VkSubresourceLayout2* layout) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->image_subresource_layout == nullptr) {
    return;
  }
  Etc2EmulatedFormat format;
  VkImageCreateInfo host_image;
  std::uint32_t scale = 1;
  if (!dev->emulated || info == nullptr || info->pCreateInfo == nullptr ||
      !HostImageInfo(state_->upscale, dev->bc, *info->pCreateInfo, &host_image,
                     &format, &scale)) {
    dev->image_subresource_layout(device, info, layout);
    return;
  }
  VkDeviceImageSubresourceInfo host_info = *info;
  host_info.pCreateInfo = &host_image;
  dev->image_subresource_layout(device, &host_info, layout);
}

VkResult VulkanEtc2Emulation::BindBufferMemory(VkDevice device, VkBuffer buffer,
                                               VkDeviceMemory memory,
                                               VkDeviceSize offset) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dev->bind_buffer_memory(device, buffer, memory, offset);
  if (result == VK_SUCCESS && dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    state_->buffers[buffer] = {memory, offset};
  }
  return result;
}

VkResult VulkanEtc2Emulation::BindBufferMemory2(
    VkDevice device, std::uint32_t count, const VkBindBufferMemoryInfo* infos) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = dev->bind_buffer_memory2(device, count, infos);
  if (result == VK_SUCCESS && dev->emulated && infos != nullptr) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      state_->buffers[infos[index].buffer] = {infos[index].memory,
                                              infos[index].memoryOffset};
    }
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory(VkDevice device, VkDeviceMemory memory,
                                        VkDeviceSize offset, VkDeviceSize size,
                                        VkMemoryMapFlags flags, void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
  }
  const VkResult result =
      dev->map_memory(device, memory, offset, size, flags, data);
  if (result == VK_SUCCESS && dev->emulated && data != nullptr) {
    state_->mappings[memory] = {*data, offset, size};
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory2(VkDevice device,
                                         const VkMemoryMapInfo* info,
                                         void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
  }
  const VkResult result = dev->map_memory2(device, info, data);
  if (result == VK_SUCCESS && dev->emulated && info != nullptr &&
      data != nullptr) {
    state_->mappings[info->memory] = {*data, info->offset, info->size};
  }
  return result;
}

void VulkanEtc2Emulation::UnmapMemory(VkDevice device, VkDeviceMemory memory) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
    state_->mappings.erase(memory);
  }
  dev->unmap_memory(device, memory);
}

VkResult VulkanEtc2Emulation::UnmapMemory2(VkDevice device,
                                           const VkMemoryUnmapInfo* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated && info != nullptr) {
    lock.lock();
    state_->mappings.erase(info->memory);
  }
  return dev->unmap_memory2(device, info);
}

void VulkanEtc2Emulation::FreeMemory(VkDevice device, VkDeviceMemory memory,
                                     const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->free_memory == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
    state_->mappings.erase(memory);
  }
  dev->free_memory(device, memory, allocator);
}

void VulkanEtc2Emulation::DestroyBuffer(VkDevice device, VkBuffer buffer,
                                        const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_buffer == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    state_->buffers.erase(buffer);
  }
  dev->destroy_buffer(device, buffer, allocator);
}

void VulkanEtc2Emulation::CmdCopyBufferToImage(
    VkDevice device, VkCommandBuffer command_buffer, VkBuffer source,
    VkImage destination, VkImageLayout layout, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image == nullptr) {
    return;
  }
  ImageRecord record;
  if (!state_->LookupImage(*dev, destination, &record)) {
    dev->copy_buffer_to_image(command_buffer, source, destination, layout,
                              region_count, regions);
    return;
  }
  std::vector<VkBufferImageCopy> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  const bool gpu = dev->gpu != nullptr && record.scale == 1 &&
                   !state_->overrides.enabled() &&
                   (!record.bc || dev->gpu->Encodes());
  if (!PlanRegions(record, device, source, destination, region_count, regions,
                   gpu, &rewritten, &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!PrepareStaging(*dev, gpu, total, &staging, &uploads, &rewritten)) {
    return;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  dev->copy_buffer_to_image(command_buffer, staging.memory.buffer, destination,
                            layout, region_count, rewritten.data());
}

void VulkanEtc2Emulation::CmdCopyBufferToImage2(
    VkDevice device, VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image2 == nullptr ||
      info == nullptr) {
    return;
  }
  ImageRecord record;
  if (!state_->LookupImage(*dev, info->dstImage, &record)) {
    dev->copy_buffer_to_image2(command_buffer, info);
    return;
  }
  std::vector<VkBufferImageCopy2> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  const bool gpu = dev->gpu != nullptr && record.scale == 1 &&
                   !state_->overrides.enabled() &&
                   (!record.bc || dev->gpu->Encodes());
  if (!PlanRegions(record, device, info->srcBuffer, info->dstImage,
                   info->regionCount, info->pRegions, gpu, &rewritten,
                   &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!PrepareStaging(*dev, gpu, total, &staging, &uploads, &rewritten)) {
    return;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  VkCopyBufferToImageInfo2 host_info = *info;
  host_info.srcBuffer = staging.memory.buffer;
  host_info.pRegions = rewritten.data();
  dev->copy_buffer_to_image2(command_buffer, &host_info);
}

bool VulkanEtc2Emulation::State::BlitScaledCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t count,
    const VkImageCopy* regions) {
  ImageRecord source_record;
  ImageRecord destination_record;
  const bool source_scaled =
      LookupImage(dev, source, &source_record) && source_record.scale > 1;
  const bool destination_scaled =
      LookupImage(dev, destination, &destination_record) &&
      destination_record.scale > 1;
  if ((!source_scaled && !destination_scaled) || dev.blit_image == nullptr ||
      regions == nullptr) {
    return false;
  }
  std::vector<VkImageBlit> blits;
  blits.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    VkImageBlit blit{};
    blit.srcSubresource = region.srcSubresource;
    blit.dstSubresource = region.dstSubresource;
    if (!HostRegion(source_scaled ? &source_record : nullptr,
                    region.srcSubresource.mipLevel, region.srcOffset,
                    region.extent, &blit.srcOffsets[0], &blit.srcOffsets[1]) ||
        !HostRegion(destination_scaled ? &destination_record : nullptr,
                    region.dstSubresource.mipLevel, region.dstOffset,
                    region.extent, &blit.dstOffsets[0], &blit.dstOffsets[1])) {
      LogFailure("skipped an image copy with an unsupported region");
      return true;
    }
    blits.push_back(blit);
  }
  const bool same_scale = source_scaled && destination_scaled &&
                          source_record.scale == destination_record.scale;
  dev.blit_image(command_buffer, source, source_layout, destination,
                 destination_layout, static_cast<std::uint32_t>(blits.size()),
                 blits.data(), same_scale ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  return true;
}

bool VulkanEtc2Emulation::State::MixedCopy(const HostDevice& dev,
                                           VkImage source,
                                           VkImage destination) {
  if (!dev.emulated) {
    return false;
  }
  ImageRecord source_record;
  ImageRecord destination_record;
  const bool source_emulated = LookupImage(dev, source, &source_record);
  const bool destination_emulated =
      LookupImage(dev, destination, &destination_record);
  if (!source_emulated && !destination_emulated) {
    return false;
  }
  const auto texel_bytes = [](VkFormat format) {
    return format == VK_FORMAT_R16_UNORM || format == VK_FORMAT_R16_SNORM ? 2 : 4;
  };
  const auto block_kind = [](EtcFormat format) {
    return IsColorFormat(format) ? 0 : static_cast<int>(BcBlockBytes(format));
  };
  if (source_emulated && destination_emulated &&
      (source_record.bc || destination_record.bc)) {
    if (source_record.bc == destination_record.bc &&
        block_kind(source_record.format.etc_format) ==
            block_kind(destination_record.format.etc_format)) {
      return false;
    }
  } else if (source_emulated && destination_emulated &&
             texel_bytes(source_record.format.host_format) ==
                 texel_bytes(destination_record.format.host_format)) {
    return false;
  }
  LogFailure("skipped an image copy between an ETC2 image and another format");
  return true;
}

void VulkanEtc2Emulation::CmdCopyImage(
    VkDevice device, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t region_count,
    const VkImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image == nullptr ||
      state_->MixedCopy(*dev, source, destination)) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 &&
      state_->BlitScaledCopy(*dev, command_buffer, source, source_layout,
                             destination, destination_layout, region_count,
                             regions)) {
    return;
  }
  dev->copy_image(command_buffer, source, source_layout, destination,
                  destination_layout, region_count, regions);
}

void VulkanEtc2Emulation::CmdCopyImage2(VkDevice device,
                                        VkCommandBuffer command_buffer,
                                        const VkCopyImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image2 == nullptr || info == nullptr ||
      state_->MixedCopy(*dev, info->srcImage, info->dstImage)) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 && info->pRegions != nullptr) {
    std::vector<VkImageCopy> regions;
    regions.reserve(info->regionCount);
    for (std::uint32_t index = 0; index < info->regionCount; ++index) {
      const VkImageCopy2& region = info->pRegions[index];
      regions.push_back({region.srcSubresource, region.srcOffset,
                         region.dstSubresource, region.dstOffset,
                         region.extent});
    }
    if (state_->BlitScaledCopy(*dev, command_buffer, info->srcImage,
                               info->srcImageLayout, info->dstImage,
                               info->dstImageLayout, info->regionCount,
                               regions.data())) {
      return;
    }
  }
  dev->copy_image2(command_buffer, info);
}

void VulkanEtc2Emulation::CmdCopyImageToBuffer(
    VkDevice device, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout layout, VkBuffer destination, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image_to_buffer == nullptr) {
    return;
  }
  ImageRecord record;
  if (dev->emulated && state_->LookupImage(*dev, source, &record)) {
    LogFailure("skipped a copy from an ETC2 image to a buffer");
    return;
  }
  dev->copy_image_to_buffer(command_buffer, source, layout, destination,
                            region_count, regions);
}

void VulkanEtc2Emulation::CmdCopyImageToBuffer2(
    VkDevice device, VkCommandBuffer command_buffer,
    const VkCopyImageToBufferInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image_to_buffer2 == nullptr || info == nullptr) {
    return;
  }
  ImageRecord record;
  if (dev->emulated && state_->LookupImage(*dev, info->srcImage, &record)) {
    LogFailure("skipped a copy from an ETC2 image to a buffer");
    return;
  }
  dev->copy_image_to_buffer2(command_buffer, info);
}

void VulkanEtc2Emulation::CmdExecuteCommands(
    VkDevice device, VkCommandBuffer command_buffer, std::uint32_t count,
    const VkCommandBuffer* secondaries) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->execute_commands == nullptr) {
    return;
  }
  if (dev->emulated && secondaries != nullptr &&
      state_->has_commands.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      if (state_->commands.count(secondaries[index]) != 0) {
        state_->commands[command_buffer].secondaries.push_back(
            secondaries[index]);
      }
    }
  }
  dev->execute_commands(command_buffer, count, secondaries);
}

bool VulkanEtc2Emulation::HasPendingUploads() const {
  return state_->has_commands.load(std::memory_order_acquire);
}

void VulkanEtc2Emulation::PrepareSubmit(VkQueue queue,
                                        const VkCommandBuffer* command_buffers,
                                        std::uint32_t count) {
  if (command_buffers == nullptr || count == 0 ||
      !state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  struct Work {
    PendingUpload upload;
    std::uint8_t* source = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
  };
  std::vector<PendingUpload> uploads;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    std::unordered_set<VkCommandBuffer> visited;
    auto collect = [this, &uploads, &visited](VkCommandBuffer command_buffer) {
      const auto record = state_->commands.find(command_buffer);
      if (record == state_->commands.end() ||
          !visited.insert(command_buffer).second) {
        return;
      }
      uploads.insert(uploads.end(), record->second.uploads.begin(),
                      record->second.uploads.end());
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      collect(command_buffers[index]);
      const auto record = state_->commands.find(command_buffers[index]);
      if (record != state_->commands.end()) {
        const std::vector<VkCommandBuffer> secondaries =
            record->second.secondaries;
        for (VkCommandBuffer secondary : secondaries) {
          collect(secondary);
        }
      }
    }
  }
  if (uploads.empty()) {
    return;
  }
  const State::Route route = state_->LookupQueue(queue);
  if (std::any_of(uploads.begin(), uploads.end(),
                  [](const PendingUpload& upload) { return upload.gpu; })) {
    state_->DecodeOnGpu(route, queue, &uploads);
  }
  if (uploads.empty()) {
    return;
  }
  std::vector<Work> work;
  work.reserve(uploads.size());
  std::unique_lock<std::mutex> memory_lock(state_->memory_mutex);
  {
    for (const PendingUpload& upload : uploads) {
      const auto binding = state_->buffers.find(upload.source);
      if (binding == state_->buffers.end()) {
        LogFailure("upload source buffer has no tracked memory binding");
        continue;
      }
      Work item;
      item.upload = upload;
      const VkDeviceSize absolute =
          binding->second.offset + upload.source_offset;
      const auto mapping = state_->mappings.find(binding->second.memory);
      if (mapping == state_->mappings.end()) {
        item.memory = binding->second.memory;
        item.memory_offset = absolute;
      } else if (absolute >= mapping->second.offset &&
                 (mapping->second.size == VK_WHOLE_SIZE ||
                  (absolute - mapping->second.offset <= mapping->second.size &&
                   upload.source_span <= mapping->second.size -
                                            (absolute - mapping->second.offset)))) {
        item.source = static_cast<std::uint8_t*>(mapping->second.data) +
                      (absolute - mapping->second.offset);
      } else {
        LogFailure("upload source lies outside the mapped range");
        continue;
      }
      work.push_back(item);
    }
  }
  if (work.empty()) {
    return;
  }
  struct TemporaryMapping {
    const HostDevice* dev = nullptr;
    std::uint8_t* data = nullptr;
  };
  std::unordered_map<VkDeviceMemory, TemporaryMapping> temporary;
  struct Copy {
    const std::uint8_t* source = nullptr;
    VkDeviceSize compressed = 0;
    VkDeviceSize decoded = 0;
  };
  std::vector<Copy> copies;
  std::vector<const PendingUpload*> copied_uploads;
  VkDeviceSize compressed_total = 0;
  VkDeviceSize decoded_total = 0;
  for (const Work& item : work) {
    const PendingUpload& upload = item.upload;
    const std::uint8_t* source = item.source;
    if (source == nullptr) {
      auto found = temporary.find(item.memory);
      if (found == temporary.end()) {
        const HostDevice* dev = state_->Find(upload.device);
        void* data = nullptr;
        if (dev == nullptr || dev->map_memory == nullptr ||
            dev->map_memory(dev->device, item.memory, 0, VK_WHOLE_SIZE, 0,
                            &data) != VK_SUCCESS) {
          LogFailure("could not map an unmapped upload source");
          continue;
        }
        found = temporary
                    .emplace(item.memory,
                             TemporaryMapping{
                                 dev, static_cast<std::uint8_t*>(data)})
                    .first;
      }
      source = found->second.data + item.memory_offset;
    }
    const bool direct = upload.target_cached && upload.scale == 1 &&
                        !state_->overrides.enabled();
    const VkDeviceSize decoded = direct ? 0 : upload.decoded;
    copies.push_back({source, upload.compressed, decoded});
    copied_uploads.push_back(&upload);
    compressed_total += upload.compressed;
    decoded_total += decoded;
  }

  static thread_local std::vector<std::uint8_t> scratch_source;
  static thread_local std::vector<std::uint8_t> scratch_decoded;
  if (scratch_source.size() < compressed_total) {
    scratch_source.resize(compressed_total);
  }
  if (scratch_decoded.size() < decoded_total) {
    scratch_decoded.resize(decoded_total);
  }
  std::vector<EtcDecodeJob> jobs;
  VkDeviceSize compressed_offset = 0;
  VkDeviceSize decoded_offset = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    const Copy& copy = copies[index];
    const PendingUpload& upload = *copied_uploads[index];
    std::uint8_t* source = scratch_source.data() + compressed_offset;
    std::uint8_t* decoded = copy.decoded == 0
                                ? upload.target
                                : scratch_decoded.data() + decoded_offset;
    const VkDeviceSize compressed_layer = upload.compressed / upload.layers;
    const VkDeviceSize decoded_layer = upload.decoded / upload.layers;
    if (upload.source_span == copy.compressed) {
      CopyFromMapped(source, copy.source, copy.compressed);
    } else {
      const VkDeviceSize block_rows =
          (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
      const VkDeviceSize packed_row = compressed_layer / block_rows;
      for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
        for (VkDeviceSize row = 0; row < block_rows; ++row) {
          CopyFromMapped(source + layer * compressed_layer + row * packed_row,
                         copy.source + layer * upload.source_layer_stride +
                             row * upload.source_row_stride,
                         packed_row);
        }
      }
    }
    for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
      EtcDecodeJob job;
      job.format = upload.format;
      job.source = source + layer * compressed_layer;
      job.source_bytes = compressed_layer;
      job.width = upload.width;
      job.height = upload.height;
      job.destination = decoded + layer * decoded_layer;
      job.destination_bytes = decoded_layer;
      if (upload.bc) {
        job.rows = EncodeRows;
      }
      jobs.push_back(job);
    }
    compressed_offset += copy.compressed;
    decoded_offset += copy.decoded;
    if (ShouldLog(&g_decode_logs, 4)) {
      std::fprintf(stderr, "  [vulkan] ETC2 upload %s %ux%u layers=%u\n",
                   upload.bc ? "encoded to BC" : "decoded", upload.width,
                   upload.height, upload.layers);
    }
  }
  for (const auto& [memory, mapping] : temporary) {
    if (mapping.dev->unmap_memory != nullptr) {
      mapping.dev->unmap_memory(mapping.dev->device, memory);
    }
  }
  memory_lock.unlock();
  DecodeEtcJobs(jobs.data(), jobs.size(), DecodeWorkerCount());
  for (const EtcDecodeJob& job : jobs) {
    if (!job.ok) {
      LogFailure("could not decode an upload layer");
    }
  }
  compressed_offset = 0;
  decoded_offset = 0;
  std::vector<Etc2GpuCopy> carries;
  for (std::size_t index = 0; index < copied_uploads.size(); ++index) {
    const PendingUpload& upload = *copied_uploads[index];
    if (copies[index].decoded != 0 && upload.target == nullptr) {
      Etc2GpuCopy carry;
      carry.data = scratch_decoded.data() + decoded_offset;
      carry.size = upload.decoded;
      carry.target = upload.target_buffer;
      carry.target_offset = upload.target_offset;
      carries.push_back(carry);
    } else if (copies[index].decoded != 0) {
      state_->EmitUpload(upload, scratch_source.data() + compressed_offset,
                         scratch_decoded.data() + decoded_offset);
    }
    compressed_offset += copies[index].compressed;
    decoded_offset += copies[index].decoded;
  }
  if (!carries.empty()) {
    state_->CopyToDevice(route, queue, carries);
  }
}

void VulkanEtc2Emulation::ReleaseCommandBuffer(VkCommandBuffer command_buffer) {
  if (!state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    const auto record = state_->commands.find(command_buffer);
    if (record == state_->commands.end()) {
      return;
    }
    doomed = std::move(record->second.staging);
    state_->commands.erase(record);
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  for (const Staging& staging : doomed) {
    if (const HostDevice* dev = state_->Find(staging.device); dev != nullptr) {
      ReleaseStaging(*dev, staging);
    }
  }
}

}  // namespace blox7::graphics
