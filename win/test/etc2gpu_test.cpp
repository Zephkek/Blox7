#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include "blox7/graphics/etc2_bc_encoder.h"
#include "blox7/graphics/etc2_decoder.h"
#include "blox7/graphics/vulkan_etc2_emulation.h"
#include "blox7/graphics/vulkan_etc2_gpu_decoder.h"

namespace {

using blox7::graphics::BcBlockBytes;
using blox7::graphics::DecodeEtcImage;
using blox7::graphics::EncodeEtcImageToBc;
using blox7::graphics::DecodeEtcJobs;
using blox7::graphics::Etc2EmulatedFormat;
using blox7::graphics::Etc2GpuCopy;
using blox7::graphics::Etc2GpuDecodable;
using blox7::graphics::Etc2GpuDecoder;
using blox7::graphics::Etc2GpuEncoder;
using blox7::graphics::Etc2GpuUpload;
using blox7::graphics::EtcBlockBytes;
using blox7::graphics::EtcDecodedTexelBytes;
using blox7::graphics::EtcDecodeJob;
using blox7::graphics::EtcFormat;
using blox7::graphics::LookupEmulatedEtc2Format;
using blox7::graphics::VulkanEtc2Emulation;

#define INSTANCE_FUNCTIONS(X)               \
  X(EnumeratePhysicalDevices)               \
  X(GetPhysicalDeviceProperties)            \
  X(GetPhysicalDeviceFeatures)              \
  X(GetPhysicalDeviceFormatProperties)      \
  X(GetPhysicalDeviceQueueFamilyProperties) \
  X(GetPhysicalDeviceMemoryProperties)      \
  X(CreateDevice)                           \
  X(GetDeviceProcAddr)                      \
  X(DestroyInstance)

#define DEVICE_FUNCTIONS(X)      \
  X(DestroyDevice)               \
  X(GetDeviceQueue)              \
  X(DeviceWaitIdle)              \
  X(QueueWaitIdle)               \
  X(QueueSubmit)                 \
  X(CreateBuffer)                \
  X(DestroyBuffer)               \
  X(GetBufferMemoryRequirements) \
  X(GetImageMemoryRequirements)  \
  X(AllocateMemory)              \
  X(FreeMemory)                  \
  X(BindBufferMemory)            \
  X(BindImageMemory)             \
  X(MapMemory)                   \
  X(CreateCommandPool)           \
  X(DestroyCommandPool)          \
  X(ResetCommandPool)            \
  X(AllocateCommandBuffers)      \
  X(BeginCommandBuffer)          \
  X(EndCommandBuffer)            \
  X(CmdPipelineBarrier)          \
  X(CmdFillBuffer)               \
  X(CmdCopyBuffer)               \
  X(CreateImage)                 \
  X(DestroyImage)                \
  X(CmdCopyBufferToImage)        \
  X(CmdBlitImage)                \
  X(CmdCopyImageToBuffer)

struct Vk {
#define MEMBER(name) PFN_vk##name name = nullptr;
  INSTANCE_FUNCTIONS(MEMBER)
  DEVICE_FUNCTIONS(MEMBER)
#undef MEMBER
} vk;

PFN_vkGetInstanceProcAddr gipa = nullptr;
VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice physical = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
VkPhysicalDeviceProperties properties{};
VkPhysicalDeviceMemoryProperties memory_properties{};
std::vector<VkQueueFamilyProperties> families;

struct Queue {
  const char* name = "";
  std::uint32_t family = 0;
  bool compute = false;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
};

std::vector<Queue> queues;
int failures = 0;
bool has_copy2 = false;
bool has_bc = false;

const EtcFormat kFormats[] = {
    EtcFormat::kEtc2Rgb8,  EtcFormat::kEtc2Rgb8A1,   EtcFormat::kEtc2Rgba8,
    EtcFormat::kEacR11,    EtcFormat::kEacR11Signed, EtcFormat::kEacRg11,
    EtcFormat::kEacRg11Signed};

const char* Name(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgb8:
      return "ETC2_RGB8";
    case EtcFormat::kEtc2Rgb8A1:
      return "ETC2_RGB8A1";
    case EtcFormat::kEtc2Rgba8:
      return "ETC2_RGBA8";
    case EtcFormat::kEacR11:
      return "EAC_R11";
    case EtcFormat::kEacR11Signed:
      return "EAC_R11_SNORM";
    case EtcFormat::kEacRg11:
      return "EAC_RG11";
    case EtcFormat::kEacRg11Signed:
      return "EAC_RG11_SNORM";
  }
  return "?";
}

bool IsColor(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

std::uint64_t g_state = 0x9e3779b97f4a7c15ull;

std::uint64_t Next() {
  g_state ^= g_state >> 12;
  g_state ^= g_state << 25;
  g_state ^= g_state >> 27;
  return g_state * 2685821657736338717ull;
}

std::uint32_t Below(std::uint32_t limit) {
  return static_cast<std::uint32_t>(Next() % limit);
}

void FillRandom(std::uint8_t* data, std::size_t size) {
  for (std::size_t offset = 0; offset < size; offset += 8) {
    const std::uint64_t value = Next();
    std::memcpy(data + offset, &value, std::min<std::size_t>(8, size - offset));
  }
}

double Seconds(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

void Result(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  std::fflush(stdout);
  if (!ok) {
    ++failures;
  }
}

std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags wanted,
                         VkMemoryPropertyFlags unwanted = 0) {
  for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount;
       ++index) {
    const VkMemoryPropertyFlags flags =
        memory_properties.memoryTypes[index].propertyFlags;
    if ((bits & (1U << index)) != 0 && (flags & wanted) == wanted &&
        (flags & unwanted) == 0) {
      return index;
    }
  }
  return UINT32_MAX;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  std::uint8_t* data = nullptr;
  VkDeviceSize size = 0;
};

Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                  VulkanEtc2Emulation* tracker = nullptr,
                  bool device_local = false) {
  Buffer result;
  result.size = size;
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vk.CreateBuffer(device, &info, nullptr, &result.buffer) != VK_SUCCESS) {
    std::printf("vkCreateBuffer failed\n");
    std::exit(1);
  }
  VkMemoryRequirements requirements{};
  vk.GetBufferMemoryRequirements(device, result.buffer, &requirements);
  std::uint32_t type = UINT32_MAX;
  if (device_local) {
    type = MemoryType(requirements.memoryTypeBits,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX) {
      type = MemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
  } else {
    type = MemoryType(requirements.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) {
      type = MemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
  }
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  void* mapped = nullptr;
  bool bound = vk.AllocateMemory(device, &allocate, nullptr, &result.memory) ==
                   VK_SUCCESS &&
               (tracker != nullptr
                    ? tracker->BindBufferMemory(device, result.buffer,
                                                result.memory, 0)
                    : vk.BindBufferMemory(device, result.buffer, result.memory,
                                          0)) == VK_SUCCESS;
  if (bound && !device_local) {
    bound = (tracker != nullptr
                 ? tracker->MapMemory(device, result.memory, 0, VK_WHOLE_SIZE,
                                      0, &mapped)
                 : vk.MapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0,
                                &mapped)) == VK_SUCCESS;
  }
  if (!bound) {
    std::printf("buffer of %llu bytes failed\n",
                static_cast<unsigned long long>(size));
    std::exit(1);
  }
  result.data = static_cast<std::uint8_t*>(mapped);
  return result;
}

void FreeBuffer(Buffer* buffer, VulkanEtc2Emulation* tracker = nullptr) {
  if (tracker != nullptr) {
    tracker->DestroyBuffer(device, buffer->buffer, nullptr);
    tracker->FreeMemory(device, buffer->memory, nullptr);
  } else {
    vk.DestroyBuffer(device, buffer->buffer, nullptr);
    vk.FreeMemory(device, buffer->memory, nullptr);
  }
  *buffer = Buffer{};
}

void Begin(Queue& queue, VkCommandBufferUsageFlags flags =
                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) {
  vk.ResetCommandPool(device, queue.pool, 0);
  VkCommandBufferBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  info.flags = flags;
  vk.BeginCommandBuffer(queue.command, &info);
}

void MemoryBarrier(VkCommandBuffer command, VkPipelineStageFlags source_stage,
                   VkAccessFlags source_access,
                   VkPipelineStageFlags target_stage,
                   VkAccessFlags target_access) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = target_access;
  vk.CmdPipelineBarrier(command, source_stage, target_stage, 0, 1, &barrier, 0,
                        nullptr, 0, nullptr);
}

void HostBarrier(VkCommandBuffer command) {
  MemoryBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
}

bool SubmitAndWait(Queue& queue) {
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &queue.command;
  return vk.QueueSubmit(queue.queue, 1, &submit, VK_NULL_HANDLE) ==
             VK_SUCCESS &&
         vk.QueueWaitIdle(queue.queue) == VK_SUCCESS;
}

void FillDevice(Queue& queue, const std::vector<Buffer>& buffers) {
  Begin(queue);
  for (const Buffer& buffer : buffers) {
    vk.CmdFillBuffer(queue.command, buffer.buffer, 0, VK_WHOLE_SIZE,
                     0xcdcdcdcdu);
  }
  vk.EndCommandBuffer(queue.command);
  SubmitAndWait(queue);
}

void ReadBack(Queue& queue, const std::vector<Buffer>& from,
              const std::vector<Buffer>& to) {
  Begin(queue);
  MemoryBarrier(queue.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
  for (std::size_t index = 0; index < from.size(); ++index) {
    const VkBufferCopy region{0, 0, from[index].size};
    vk.CmdCopyBuffer(queue.command, from[index].buffer, to[index].buffer, 1,
                     &region);
  }
  HostBarrier(queue.command);
  vk.EndCommandBuffer(queue.command);
  SubmitAndWait(queue);
}

const char* RgbMode(const std::uint8_t* b, bool punchthrough) {
  static const int kDelta[8] = {0, 1, 2, 3, -4, -3, -2, -1};
  if (!punchthrough && (b[3] & 2) == 0) {
    return "individual";
  }
  if ((b[0] >> 3) + kDelta[b[0] & 7] < 0 ||
      (b[0] >> 3) + kDelta[b[0] & 7] > 31) {
    return "T";
  }
  if ((b[1] >> 3) + kDelta[b[1] & 7] < 0 ||
      (b[1] >> 3) + kDelta[b[1] & 7] > 31) {
    return "H";
  }
  if ((b[2] >> 3) + kDelta[b[2] & 7] < 0 ||
      (b[2] >> 3) + kDelta[b[2] & 7] > 31) {
    return "planar";
  }
  return "differential";
}

struct Spec {
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  std::uint32_t width = 4;
  std::uint32_t height = 4;
  std::uint32_t layers = 1;
  std::uint32_t row_pad = 0;
  std::uint32_t layer_pad = 0;
  VkDeviceSize source_gap = 0;
  VkDeviceSize target_gap = 0;
  std::size_t source_buffer = 0;
  std::size_t target_buffer = 0;
  bool bc = false;
};

std::size_t StoredLayerBytes(const Spec& spec) {
  return spec.bc ? static_cast<std::size_t>((spec.width + 3) / 4) *
                       ((spec.height + 3) / 4) * BcBlockBytes(spec.format)
                 : static_cast<std::size_t>(spec.width) * spec.height *
                       EtcDecodedTexelBytes(spec.format);
}

using Filler = void (*)(EtcFormat format, std::uint8_t* blocks,
                        std::size_t count);

struct Outcome {
  std::uint64_t blocks = 0;
  std::uint64_t mismatches = 0;
  std::uint64_t stray = 0;
  double seconds = 0;
  std::uint64_t modes[5] = {0, 0, 0, 0, 0};
};

struct Options {
  bool expect = true;
  Filler fill = nullptr;
  Outcome* out = nullptr;
  bool quiet = false;
  bool host_targets = false;
};

void ShowMismatch(const Spec& spec, std::uint32_t layer, std::size_t byte,
                  const std::uint8_t* packed, const std::uint8_t* gpu,
                  const std::uint8_t* cpu) {
  const std::size_t texel_bytes =
      spec.bc ? BcBlockBytes(spec.format) : EtcDecodedTexelBytes(spec.format);
  const std::size_t texel = byte / texel_bytes;
  const std::uint32_t row = spec.bc ? (spec.width + 3) / 4 : spec.width;
  const std::uint32_t x =
      static_cast<std::uint32_t>(texel % row) * (spec.bc ? 4 : 1);
  const std::uint32_t y =
      static_cast<std::uint32_t>(texel / row) * (spec.bc ? 4 : 1);
  const std::size_t block_bytes = EtcBlockBytes(spec.format);
  const std::size_t block = (y / 4) * ((spec.width + 3) / 4) + (x / 4);
  std::printf("    %s %ux%u layer %u texel (%u,%u) block", Name(spec.format),
              spec.width, spec.height, layer, x, y);
  for (std::size_t k = 0; k < block_bytes; ++k) {
    std::printf(" %02x", packed[block * block_bytes + k]);
  }
  std::printf("\n      gpu");
  for (std::size_t k = 0; k < texel_bytes; ++k) {
    std::printf(" %02x", gpu[texel * texel_bytes + k]);
  }
  std::printf(" cpu");
  for (std::size_t k = 0; k < texel_bytes; ++k) {
    std::printf(" %02x", cpu[texel * texel_bytes + k]);
  }
  std::printf("\n");
}

bool Run(const std::string& name, Etc2GpuDecoder& decoder, Queue& queue,
         const std::vector<Spec>& specs, const Options& options = Options()) {
  std::size_t source_count = 0;
  std::size_t target_count = 0;
  for (const Spec& spec : specs) {
    source_count = std::max(source_count, spec.source_buffer + 1);
    target_count = std::max(target_count, spec.target_buffer + 1);
  }
  std::vector<VkDeviceSize> source_end(source_count, 0);
  std::vector<VkDeviceSize> target_end(target_count, 0);
  std::vector<Etc2GpuUpload> uploads(specs.size());
  Outcome outcome;
  for (std::size_t index = 0; index < specs.size(); ++index) {
    const Spec& spec = specs[index];
    Etc2GpuUpload& upload = uploads[index];
    const VkDeviceSize block_bytes = EtcBlockBytes(spec.format);
    const VkDeviceSize blocks_wide = (spec.width + 3) / 4;
    const VkDeviceSize block_rows = (spec.height + 3) / 4;
    upload.format = spec.format;
    upload.width = spec.width;
    upload.height = spec.height;
    upload.layers = spec.layers;
    upload.source_row_stride = (blocks_wide + spec.row_pad) * block_bytes;
    upload.source_layer_stride =
        (block_rows + spec.layer_pad) * upload.source_row_stride;
    upload.source_offset = source_end[spec.source_buffer] + spec.source_gap;
    source_end[spec.source_buffer] =
        upload.source_offset + (spec.layers - 1) * upload.source_layer_stride +
        (block_rows - 1) * upload.source_row_stride + blocks_wide * block_bytes;
    upload.bc = spec.bc;
    upload.target_offset = target_end[spec.target_buffer] + spec.target_gap;
    target_end[spec.target_buffer] =
        upload.target_offset + StoredLayerBytes(spec) * spec.layers;
    outcome.blocks += blocks_wide * block_rows * spec.layers;
  }
  std::vector<Buffer> sources(source_count);
  std::vector<Buffer> targets(target_count);
  std::vector<Buffer> readbacks;
  for (std::size_t index = 0; index < source_count; ++index) {
    sources[index] = MakeBuffer((source_end[index] + 259) / 4 * 4,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    FillRandom(sources[index].data, sources[index].size);
  }
  const VkBufferUsageFlags target_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  for (std::size_t index = 0; index < target_count; ++index) {
    const VkDeviceSize size = (target_end[index] + 259) / 4 * 4;
    targets[index] =
        MakeBuffer(size, target_usage, nullptr, !options.host_targets);
    if (options.host_targets) {
      std::memset(targets[index].data, 0xcd, targets[index].size);
    } else {
      readbacks.push_back(MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    }
  }
  if (!options.host_targets) {
    FillDevice(queue, targets);
  }
  for (std::size_t index = 0; index < specs.size(); ++index) {
    const Spec& spec = specs[index];
    uploads[index].source = sources[spec.source_buffer].buffer;
    uploads[index].target = targets[spec.target_buffer].buffer;
    if (options.fill != nullptr) {
      options.fill(spec.format,
                   sources[spec.source_buffer].data +
                       uploads[index].source_offset,
                   ((spec.width + 3) / 4) * ((spec.height + 3) / 4));
    }
  }
  const auto start = std::chrono::steady_clock::now();
  const bool decoded =
      decoder.Decode(queue.queue, queue.family, uploads.data(), uploads.size());
  vk.QueueWaitIdle(queue.queue);
  outcome.seconds = Seconds(start);
  if (options.host_targets) {
    Begin(queue);
    HostBarrier(queue.command);
    vk.EndCommandBuffer(queue.command);
    SubmitAndWait(queue);
  } else {
    ReadBack(queue, targets, readbacks);
  }
  const std::vector<Buffer>& results =
      options.host_targets ? targets : readbacks;

  bool ok = decoded == options.expect;
  std::vector<std::vector<bool>> covered(target_count);
  for (std::size_t index = 0; index < target_count; ++index) {
    covered[index].assign(results[index].size, false);
  }
  unsigned shown = 0;
  for (std::size_t index = 0; decoded && index < specs.size(); ++index) {
    const Spec& spec = specs[index];
    const Etc2GpuUpload& upload = uploads[index];
    const std::size_t block_bytes = EtcBlockBytes(spec.format);
    const std::size_t blocks_wide = (spec.width + 3) / 4;
    const std::size_t block_rows = (spec.height + 3) / 4;
    const std::size_t layer_bytes = StoredLayerBytes(spec);
    std::vector<std::uint8_t> packed(blocks_wide * block_rows * block_bytes);
    std::vector<std::uint8_t> expected(layer_bytes);
    for (std::uint32_t layer = 0; layer < spec.layers; ++layer) {
      for (std::size_t row = 0; row < block_rows; ++row) {
        std::memcpy(packed.data() + row * blocks_wide * block_bytes,
                    sources[spec.source_buffer].data + upload.source_offset +
                        layer * upload.source_layer_stride +
                        row * upload.source_row_stride,
                    blocks_wide * block_bytes);
      }
      if (IsColor(spec.format)) {
        static const char* const kModes[5] = {"individual", "differential",
                                              "T", "H", "planar"};
        for (std::size_t block = 0; block < blocks_wide * block_rows;
             ++block) {
          const std::uint8_t* rgb =
              packed.data() + block * block_bytes +
              (spec.format == EtcFormat::kEtc2Rgba8 ? 8 : 0);
          const char* mode =
              RgbMode(rgb, spec.format == EtcFormat::kEtc2Rgb8A1);
          for (int m = 0; m < 5; ++m) {
            if (std::strcmp(mode, kModes[m]) == 0) {
              ++outcome.modes[m];
            }
          }
        }
      }
      if (!(spec.bc ? EncodeEtcImageToBc(spec.format, packed.data(),
                                         packed.size(), spec.width,
                                         spec.height, expected.data(),
                                         expected.size())
                    : DecodeEtcImage(spec.format, packed.data(), packed.size(),
                                     spec.width, spec.height, expected.data(),
                                     expected.size()))) {
        std::printf("    CPU decoder refused %s %ux%u\n", Name(spec.format),
                    spec.width, spec.height);
        ok = false;
        continue;
      }
      const std::size_t offset = upload.target_offset + layer * layer_bytes;
      const std::uint8_t* gpu = results[spec.target_buffer].data + offset;
      std::vector<bool>& mask = covered[spec.target_buffer];
      std::fill(mask.begin() + offset, mask.begin() + offset + layer_bytes,
                true);
      if (std::memcmp(gpu, expected.data(), layer_bytes) != 0) {
        const std::size_t texel_bytes = spec.bc
                                            ? BcBlockBytes(spec.format)
                                            : EtcDecodedTexelBytes(spec.format);
        for (std::size_t byte = 0; byte < layer_bytes; byte += texel_bytes) {
          if (std::memcmp(gpu + byte, expected.data() + byte, texel_bytes) !=
              0) {
            if (shown < 4) {
              ShowMismatch(spec, layer, byte, packed.data(), gpu,
                           expected.data());
              ++shown;
            }
            ++outcome.mismatches;
          }
        }
      }
    }
  }
  for (std::size_t index = 0; index < target_count; ++index) {
    for (VkDeviceSize byte = 0; byte < results[index].size; ++byte) {
      if (!covered[index][byte] && results[index].data[byte] != 0xcd) {
        if (outcome.stray < 4) {
          std::printf("    stray write in target %zu at byte %llu\n", index,
                      static_cast<unsigned long long>(byte));
        }
        ++outcome.stray;
      }
    }
  }
  ok = ok && outcome.mismatches == 0 && outcome.stray == 0;
  for (Buffer& buffer : sources) {
    FreeBuffer(&buffer);
  }
  for (Buffer& buffer : targets) {
    FreeBuffer(&buffer);
  }
  for (Buffer& buffer : readbacks) {
    FreeBuffer(&buffer);
  }
  char line[512];
  std::snprintf(line, sizeof(line),
                "%s: %zu uploads, %llu blocks, decode %s, %llu mismatching "
                "texels, %llu stray bytes (%.1f ms)",
                name.c_str(), specs.size(),
                static_cast<unsigned long long>(outcome.blocks),
                decoded ? "ran" : "refused",
                static_cast<unsigned long long>(outcome.mismatches),
                static_cast<unsigned long long>(outcome.stray),
                outcome.seconds * 1000);
  if (!options.quiet) {
    Result(ok, line);
  } else if (!ok) {
    std::printf("     failed: %s\n", line);
  }
  if (options.out != nullptr) {
    *options.out = outcome;
  }
  return ok;
}

std::uint32_t EvenFor(EtcFormat format, std::uint32_t width) {
  return Etc2GpuDecodable(format, width) ? width : width + 1;
}

void SweepEac(EtcFormat format, std::uint8_t* blocks, std::size_t count) {
  const std::size_t block_bytes = EtcBlockBytes(format);
  for (std::size_t index = 0; index < count; ++index) {
    std::uint8_t* block = blocks + index * block_bytes;
    const std::size_t combo = index & 0xffff;
    block[0] = static_cast<std::uint8_t>(combo & 255);
    block[1] = static_cast<std::uint8_t>(((combo >> 8) & 15) << 4 |
                                         ((combo >> 12) & 15));
    if (format == EtcFormat::kEacRg11 || format == EtcFormat::kEacRg11Signed) {
      const std::size_t other = (combo * 40503u + 12345u) & 0xffff;
      block[8] = static_cast<std::uint8_t>(other & 255);
      block[9] = static_cast<std::uint8_t>(((other >> 8) & 15) << 4 |
                                           ((other >> 12) & 15));
    }
  }
}

void DecoderTests(Etc2GpuDecoder& decoder, Etc2GpuDecoder& stress,
                  Queue& main, std::uint64_t random_blocks) {
  for (EtcFormat format : kFormats) {
    if (IsColor(format) && format != EtcFormat::kEtc2Rgba8) {
      continue;
    }
    Spec spec;
    spec.format = format;
    spec.width = 1024;
    spec.height = 1024;
    Options options;
    options.fill = SweepEac;
    Run(std::string("every EAC base, multiplier and table, ") + Name(format),
        decoder, main, {spec}, options);
  }

  for (EtcFormat format : kFormats) {
    std::vector<Spec> specs;
    std::uint64_t blocks = 0;
    while (blocks < random_blocks) {
      Spec spec;
      spec.format = format;
      spec.width = 2048;
      spec.height = 2048;
      spec.target_buffer = specs.size();
      specs.push_back(spec);
      blocks += 512 * 512;
    }
    Outcome outcome;
    Options options;
    options.out = &outcome;
    const bool ok = Run(std::string("random blocks, ") + Name(format), decoder,
                        main, specs, options);
    if (ok && IsColor(format)) {
      std::printf("     modes: individual %llu, differential %llu, T %llu, "
                  "H %llu, planar %llu\n",
                  static_cast<unsigned long long>(outcome.modes[0]),
                  static_cast<unsigned long long>(outcome.modes[1]),
                  static_cast<unsigned long long>(outcome.modes[2]),
                  static_cast<unsigned long long>(outcome.modes[3]),
                  static_cast<unsigned long long>(outcome.modes[4]));
    }
  }

  const std::uint32_t sizes[][2] = {
      {1, 1},     {2, 1},   {1, 2},    {2, 2},   {3, 3},    {4, 4},
      {5, 5},     {6, 2},   {7, 9},    {8, 8},   {12, 4},   {13, 11},
      {16, 16},   {31, 33}, {64, 64},  {100, 37}, {257, 129}, {4, 1024},
      {1024, 4}};
  Options quiet;
  quiet.quiet = true;
  for (EtcFormat format : kFormats) {
    std::vector<Spec> batch;
    bool single_ok = true;
    for (const auto& size : sizes) {
      Spec spec;
      spec.format = format;
      spec.width = EvenFor(format, size[0]);
      spec.height = size[1];
      batch.push_back(spec);
      single_ok = Run("single", decoder, main, {spec}, quiet) && single_ok;
    }
    Result(single_ok, std::string("every size alone, ") + Name(format));
    Run(std::string("every size in one batch, ") + Name(format), decoder,
        main, batch);
  }

  for (EtcFormat format : kFormats) {
    std::vector<Spec> specs;
    const std::uint32_t shapes[][7] = {{37, 21, 3, 2, 3, 0, 0},
                                       {64, 8, 6, 0, 0, 0, 0},
                                       {2, 2, 5, 1, 1, 0, 0},
                                       {18, 30, 2, 0, 0, 4, 16},
                                       {33, 7, 4, 7, 0, 8, 48},
                                       {90, 90, 1, 3, 0, 4100, 4112}};
    for (const auto& shape : shapes) {
      Spec spec;
      spec.format = format;
      spec.width = EvenFor(format, shape[0]);
      spec.height = shape[1];
      spec.layers = shape[2];
      spec.row_pad = shape[3];
      spec.layer_pad = shape[4];
      spec.source_gap = shape[5];
      spec.target_gap = shape[6];
      specs.push_back(spec);
    }
    Run(std::string("layers, padded rows and layers, odd offsets, ") +
            Name(format),
        decoder, main, specs);
  }

  {
    std::vector<Spec> specs;
    for (EtcFormat format : kFormats) {
      Spec spec;
      spec.format = format;
      spec.width = EvenFor(format, 61);
      spec.height = 47;
      spec.layers = 2;
      spec.row_pad = 1;
      spec.target_gap = 12;
      specs.push_back(spec);
    }
    Options options;
    options.host_targets = true;
    Run("every format into a host-visible target", decoder, main, specs,
        options);
  }

  for (int round = 0; round < 2; ++round) {
    Etc2GpuDecoder& subject = round == 0 ? decoder : stress;
    const std::uint32_t max_width = round == 0 ? 300 : 200;
    bool all = true;
    std::uint64_t blocks = 0;
    std::uint64_t uploads = 0;
    for (int iteration = 0; iteration < 40; ++iteration) {
      std::vector<Spec> specs(1 + Below(60));
      for (Spec& spec : specs) {
        spec.format = kFormats[Below(7)];
        spec.width =
            std::min(EvenFor(spec.format, 1 + Below(max_width)), max_width);
        spec.height = 1 + Below(300);
        spec.layers = 1 + Below(4);
        spec.row_pad = Below(4);
        spec.layer_pad = Below(3);
        spec.source_gap = Below(17) * 4;
        spec.target_gap = Below(17) * 4;
        spec.source_buffer = Below(3);
        spec.target_buffer = Below(3);
        blocks += VkDeviceSize{(spec.width + 3) / 4} *
                  ((spec.height + 3) / 4) * spec.layers;
      }
      uploads += specs.size();
      all = Run("random batch", subject, main, specs, quiet) && all;
    }
    Result(all, std::string(round == 0 ? "40 random batches"
                                       : "40 random batches, split into "
                                         "bands and passes by tiny limits") +
                    ", " + std::to_string(uploads) + " uploads, " +
                    std::to_string(blocks) + " blocks");
  }

  {
    Spec wide;
    wide.format = EtcFormat::kEtc2Rgba8;
    wide.width = 1024;
    wide.height = 8;
    Options refused;
    refused.expect = false;
    Run("an upload wider than the tiny limits allow is refused untouched",
        stress, main, {wide}, refused);
    Spec odd;
    odd.format = EtcFormat::kEacR11;
    odd.width = 5;
    odd.height = 4;
    Spec fine;
    fine.format = EtcFormat::kEtc2Rgb8;
    Run("an odd-width R11 upload is refused untouched", decoder, main,
        {fine, odd}, refused);
    Etc2GpuUpload probe;
    probe.format = EtcFormat::kEacR11;
    probe.source = reinterpret_cast<VkBuffer>(1);
    probe.target = reinterpret_cast<VkBuffer>(1);
    probe.width = 6;
    probe.height = 4;
    probe.source_row_stride = 16;
    const bool even = decoder.Accepts(probe);
    probe.width = 5;
    const bool odd_accepted = decoder.Accepts(probe);
    probe.width = 6;
    probe.target_offset = 2;
    const bool unaligned = decoder.Accepts(probe);
    Result(even && !odd_accepted && !unaligned,
           "Accepts: even R11 yes, odd R11 no, unaligned target no");
  }

  for (Queue& queue : queues) {
    if (&queue == &main) {
      continue;
    }
    std::vector<Spec> specs;
    for (EtcFormat format : kFormats) {
      Spec spec;
      spec.format = format;
      spec.width = EvenFor(format, 77);
      spec.height = 45;
      spec.layers = 2;
      spec.row_pad = 1;
      specs.push_back(spec);
    }
    Options options;
    options.expect = queue.compute;
    Run(std::string("every format on the ") + queue.name + " queue", decoder,
        queue, specs, options);
  }
}

void EncoderTests(Etc2GpuDecoder& encoder, Etc2GpuDecoder& stress,
                  Etc2GpuDecoder& plain, Queue& main,
                  std::uint64_t random_blocks) {
  for (EtcFormat format : kFormats) {
    if (IsColor(format) && format != EtcFormat::kEtc2Rgba8) {
      continue;
    }
    Spec spec;
    spec.format = format;
    spec.width = 1024;
    spec.height = 1024;
    spec.bc = true;
    Options options;
    options.fill = SweepEac;
    Run(std::string("encoded: every EAC base, multiplier and table, ") +
            Name(format),
        encoder, main, {spec}, options);
  }

  for (EtcFormat format : kFormats) {
    std::vector<Spec> specs;
    std::uint64_t blocks = 0;
    while (blocks < random_blocks) {
      Spec spec;
      spec.format = format;
      spec.width = 2048;
      spec.height = 2048;
      spec.bc = true;
      spec.target_buffer = specs.size();
      specs.push_back(spec);
      blocks += 512 * 512;
    }
    Run(std::string("encoded: random blocks, ") + Name(format), encoder, main,
        specs);
  }

  const std::uint32_t sizes[][2] = {
      {1, 1},     {2, 1},   {1, 2},    {2, 2},   {3, 3},    {4, 4},
      {5, 5},     {6, 2},   {7, 9},    {8, 8},   {12, 4},   {13, 11},
      {16, 16},   {31, 33}, {64, 64},  {100, 37}, {257, 129}, {4, 1024},
      {1024, 4}};
  Options quiet;
  quiet.quiet = true;
  for (EtcFormat format : kFormats) {
    std::vector<Spec> batch;
    bool single_ok = true;
    for (const auto& size : sizes) {
      Spec spec;
      spec.format = format;
      spec.width = size[0];
      spec.height = size[1];
      spec.bc = true;
      batch.push_back(spec);
      single_ok = Run("single", encoder, main, {spec}, quiet) && single_ok;
    }
    Result(single_ok,
           std::string("encoded: every size alone, ") + Name(format));
    Run(std::string("encoded: every size in one batch, ") + Name(format),
        encoder, main, batch);
  }

  for (EtcFormat format : kFormats) {
    std::vector<Spec> specs;
    const std::uint32_t shapes[][7] = {{37, 21, 3, 2, 3, 0, 0},
                                       {64, 8, 6, 0, 0, 0, 0},
                                       {2, 2, 5, 1, 1, 0, 0},
                                       {18, 30, 2, 0, 0, 4, 16},
                                       {33, 7, 4, 7, 0, 8, 48},
                                       {90, 90, 1, 3, 0, 4100, 4112}};
    for (const auto& shape : shapes) {
      Spec spec;
      spec.format = format;
      spec.width = shape[0];
      spec.height = shape[1];
      spec.layers = shape[2];
      spec.row_pad = shape[3];
      spec.layer_pad = shape[4];
      spec.source_gap = shape[5];
      spec.target_gap = shape[6];
      spec.bc = true;
      specs.push_back(spec);
    }
    Run(std::string("encoded: layers, padded rows and layers, odd offsets, ") +
            Name(format),
        encoder, main, specs);
  }

  for (int round = 0; round < 2; ++round) {
    Etc2GpuDecoder& subject = round == 0 ? encoder : stress;
    const std::uint32_t max_width = round == 0 ? 300 : 200;
    bool all = true;
    std::uint64_t blocks = 0;
    std::uint64_t uploads = 0;
    for (int iteration = 0; iteration < 40; ++iteration) {
      std::vector<Spec> specs(1 + Below(60));
      for (Spec& spec : specs) {
        spec.format = kFormats[Below(7)];
        spec.bc = Below(4) != 0;
        spec.width = 1 + Below(max_width);
        if (!spec.bc) {
          spec.width = std::min(EvenFor(spec.format, spec.width), max_width);
        }
        spec.height = 1 + Below(300);
        spec.layers = 1 + Below(4);
        spec.row_pad = Below(4);
        spec.layer_pad = Below(3);
        spec.source_gap = Below(17) * 4;
        spec.target_gap = Below(17) * 4;
        spec.source_buffer = Below(3);
        spec.target_buffer = Below(3);
        blocks += VkDeviceSize{(spec.width + 3) / 4} *
                  ((spec.height + 3) / 4) * spec.layers;
      }
      uploads += specs.size();
      all = Run("random batch", subject, main, specs, quiet) && all;
    }
    Result(all, std::string(round == 0
                                ? "40 random batches of encodes and decodes"
                                : "40 random batches of encodes and decodes, "
                                  "split into bands and passes by tiny "
                                  "limits") +
                    ", " + std::to_string(uploads) + " uploads, " +
                    std::to_string(blocks) + " blocks");
  }

  {
    Spec spec;
    spec.format = EtcFormat::kEtc2Rgb8;
    spec.width = 64;
    spec.height = 64;
    spec.bc = true;
    Options refused;
    refused.expect = false;
    Run("a decoder built without the encoder refuses an encode untouched",
        plain, main, {spec}, refused);
  }

  for (Queue& queue : queues) {
    if (&queue == &main) {
      continue;
    }
    std::vector<Spec> specs;
    for (EtcFormat format : kFormats) {
      Spec spec;
      spec.format = format;
      spec.width = 77;
      spec.height = 45;
      spec.layers = 2;
      spec.row_pad = 1;
      spec.bc = true;
      specs.push_back(spec);
    }
    Options options;
    options.expect = queue.compute;
    Run(std::string("encoded: every format on the ") + queue.name + " queue",
        encoder, queue, specs, options);
  }
}

void PlainBlocks(EtcFormat format, std::uint8_t* blocks, std::size_t count) {
  const std::size_t block_bytes = EtcBlockBytes(format);
  for (std::size_t index = 0; index < count; ++index) {
    std::uint8_t* block = blocks + index * block_bytes;
    if (IsColor(format)) {
      std::uint8_t* color = format == EtcFormat::kEtc2Rgba8 ? block + 8 : block;
      const std::uint32_t pick = Below(4);
      color[0] = static_cast<std::uint8_t>(Below(32) << 3);
      color[1] = static_cast<std::uint8_t>(Below(32) << 3);
      color[2] = static_cast<std::uint8_t>(Below(32) << 3);
      const std::uint32_t table = Below(8);
      color[3] = static_cast<std::uint8_t>((table << 5) | (table << 2) | 2 |
                                           Below(2));
      color[4] = color[5] = (pick & 2) != 0 ? 0xff : 0;
      color[6] = color[7] = (pick & 1) != 0 ? 0xff : 0;
      if (format == EtcFormat::kEtc2Rgba8) {
        block[0] = static_cast<std::uint8_t>(Below(256));
        block[1] = static_cast<std::uint8_t>(Below(16));
      }
      continue;
    }
    for (std::size_t channel = 0; channel < block_bytes / 8; ++channel) {
      std::uint8_t* eac = block + channel * 8;
      std::uint64_t bits = 0;
      const std::uint64_t pick = Below(8);
      for (int texel = 0; texel < 16; ++texel) {
        bits |= pick << (texel * 3);
      }
      eac[0] = static_cast<std::uint8_t>(Below(256));
      eac[1] = static_cast<std::uint8_t>(Below(256));
      for (int byte = 0; byte < 6; ++byte) {
        eac[2 + byte] = static_cast<std::uint8_t>(bits >> (40 - byte * 8));
      }
    }
  }
}

void GentleBlocks(EtcFormat format, std::uint8_t* blocks, std::size_t count) {
  static const std::uint8_t kSteps[5] = {0, 1, 2, 7, 6};
  const std::size_t block_bytes = EtcBlockBytes(format);
  for (std::size_t index = 0; index < count; ++index) {
    std::uint8_t* block = blocks + index * block_bytes;
    if (IsColor(format)) {
      std::uint8_t* color = format == EtcFormat::kEtc2Rgba8 ? block + 8 : block;
      for (int channel = 0; channel < 3; ++channel) {
        color[channel] = static_cast<std::uint8_t>(((4 + Below(24)) << 3) |
                                                   kSteps[Below(5)]);
      }
      color[3] = static_cast<std::uint8_t>((Below(3) << 5) | (Below(3) << 2) |
                                           2 | Below(2));
      if (format == EtcFormat::kEtc2Rgba8) {
        block[0] = static_cast<std::uint8_t>(Below(256));
        block[1] = static_cast<std::uint8_t>(((1 + Below(4)) << 4) | Below(16));
      }
      continue;
    }
    for (std::size_t channel = 0; channel < block_bytes / 8; ++channel) {
      block[channel * 8] = static_cast<std::uint8_t>(Below(256));
      block[channel * 8 + 1] =
          static_cast<std::uint8_t>(((1 + Below(4)) << 4) | Below(16));
    }
  }
}

const char* g_wanted_kind = "";

void KindBlocks(EtcFormat format, std::uint8_t* blocks, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    std::uint8_t* block = blocks + index * 8;
    do {
      FillRandom(block, 8);
    } while (std::strcmp(RgbMode(block, false), g_wanted_kind) != 0);
  }
}

VkImage MakeImage(VkFormat format, std::uint32_t width, std::uint32_t height,
                  VkDeviceMemory* memory) {
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  if (vk.CreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  VkMemoryRequirements requirements{};
  vk.GetImageMemoryRequirements(device, image, &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vk.AllocateMemory(device, &allocate, nullptr, memory);
  vk.BindImageMemory(device, image, *memory, 0);
  return image;
}

void MoveImage(VkCommandBuffer command, VkImage image, VkImageLayout from,
               VkImageLayout to, std::uint32_t mips = 1,
               std::uint32_t layers = 1) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
}

struct StoredFormats {
  VkFormat stored;
  VkFormat plain;
};

StoredFormats FormatsFor(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEacR11:
      return {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R16_UNORM};
    case EtcFormat::kEacR11Signed:
      return {VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_R16_SNORM};
    case EtcFormat::kEacRg11:
      return {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R16G16_UNORM};
    case EtcFormat::kEacRg11Signed:
      return {VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_R16G16_SNORM};
    default:
      return {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM};
  }
}

bool ThroughDriver(Queue& queue, EtcFormat format, std::uint32_t width,
                   std::uint32_t height, const Buffer& blocks,
                   std::vector<std::uint8_t>* texels) {
  const StoredFormats formats = FormatsFor(format);
  VkFormatProperties plain_properties{};
  vk.GetPhysicalDeviceFormatProperties(physical, formats.plain,
                                       &plain_properties);
  VkFormatProperties stored_properties{};
  vk.GetPhysicalDeviceFormatProperties(physical, formats.stored,
                                       &stored_properties);
  if ((plain_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0 ||
      (stored_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0) {
    return false;
  }
  VkDeviceMemory stored_memory = VK_NULL_HANDLE;
  VkDeviceMemory plain_memory = VK_NULL_HANDLE;
  const VkImage stored = MakeImage(formats.stored, width, height, &stored_memory);
  const VkImage plain = MakeImage(formats.plain, width, height, &plain_memory);
  const std::size_t bytes =
      static_cast<std::size_t>(width) * height * EtcDecodedTexelBytes(format);
  Buffer readback = MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  Begin(queue);
  MoveImage(queue.command, stored, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vk.CmdCopyBufferToImage(queue.command, blocks.buffer, stored,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  MoveImage(queue.command, stored, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  MoveImage(queue.command, plain, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit blit{};
  blit.srcSubresource = region.imageSubresource;
  blit.dstSubresource = region.imageSubresource;
  blit.srcOffsets[1] = {static_cast<std::int32_t>(width),
                        static_cast<std::int32_t>(height), 1};
  blit.dstOffsets[1] = blit.srcOffsets[1];
  vk.CmdBlitImage(queue.command, stored, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  plain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                  VK_FILTER_NEAREST);
  MoveImage(queue.command, plain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vk.CmdCopyImageToBuffer(queue.command, plain,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                          1, &region);
  HostBarrier(queue.command);
  vk.EndCommandBuffer(queue.command);
  const bool ok = SubmitAndWait(queue);
  texels->assign(readback.data, readback.data + bytes);
  FreeBuffer(&readback);
  vk.DestroyImage(device, stored, nullptr);
  vk.DestroyImage(device, plain, nullptr);
  vk.FreeMemory(device, stored_memory, nullptr);
  vk.FreeMemory(device, plain_memory, nullptr);
  return ok;
}

struct Fidelity {
  double color = 0;
  double alpha = 0;
  int worst = 0;
  bool ran = false;
};

Fidelity StoredAgainstExact(Etc2GpuDecoder& encoder, Queue& queue,
                            EtcFormat format, Filler fill) {
  const std::uint32_t kSize = 256;
  const std::size_t blocks = (kSize / 4) * (kSize / 4);
  const std::size_t etc_bytes = blocks * EtcBlockBytes(format);
  const std::size_t bc_bytes = blocks * BcBlockBytes(format);
  Buffer source = MakeBuffer(etc_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  Buffer stored = MakeBuffer(bc_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  FillRandom(source.data, source.size);
  if (fill != nullptr) {
    fill(format, source.data, blocks);
  }
  Etc2GpuUpload upload;
  upload.format = format;
  upload.source = source.buffer;
  upload.source_row_stride = (kSize / 4) * EtcBlockBytes(format);
  upload.target = stored.buffer;
  upload.width = kSize;
  upload.height = kSize;
  upload.bc = true;
  Fidelity result;
  std::vector<std::uint8_t> got;
  if (encoder.Decode(queue.queue, queue.family, &upload, 1) &&
      vk.QueueWaitIdle(queue.queue) == VK_SUCCESS &&
      ThroughDriver(queue, format, kSize, kSize, stored, &got)) {
    std::vector<std::uint8_t> exact(got.size());
    DecodeEtcImage(format, source.data, etc_bytes, kSize, kSize, exact.data(),
                   exact.size());
    result.ran = true;
    double color = 0;
    double alpha = 0;
    const bool wide = EtcDecodedTexelBytes(format) == 4 && !IsColor(format);
    const std::size_t texels = static_cast<std::size_t>(kSize) * kSize;
    const bool is_signed = format == EtcFormat::kEacR11Signed ||
                           format == EtcFormat::kEacRg11Signed;
    for (std::size_t texel = 0; texel < texels; ++texel) {
      if (IsColor(format)) {
        for (int channel = 0; channel < 4; ++channel) {
          const int gap = static_cast<int>(got[texel * 4 + channel]) -
                          exact[texel * 4 + channel];
          (channel < 3 ? color : alpha) += static_cast<double>(gap) * gap;
          result.worst = std::max(result.worst, std::abs(gap));
        }
        continue;
      }
      for (int channel = 0; channel < (wide ? 2 : 1); ++channel) {
        const std::size_t at = (texel * (wide ? 2 : 1) + channel) * 2;
        std::uint16_t a = 0;
        std::uint16_t b = 0;
        std::memcpy(&a, got.data() + at, 2);
        std::memcpy(&b, exact.data() + at, 2);
        const int gap = is_signed ? static_cast<std::int16_t>(a) -
                                        static_cast<std::int16_t>(b)
                                  : static_cast<int>(a) - b;
        color += static_cast<double>(gap) * gap;
        result.worst = std::max(result.worst, std::abs(gap));
      }
    }
    const double peak = IsColor(format) ? 255.0 : 65535.0;
    const double samples =
        static_cast<double>(texels) * (IsColor(format) ? 3 : (wide ? 2 : 1));
    result.color =
        color == 0 ? 99.0 : 10 * std::log10(peak * peak * samples / color);
    result.alpha =
        alpha == 0 ? 99.0 : 10 * std::log10(peak * peak * texels / alpha);
  }
  FreeBuffer(&source);
  FreeBuffer(&stored);
  return result;
}

void StoredFidelity(Etc2GpuDecoder& encoder, Queue& queue) {
  for (EtcFormat format : kFormats) {
    const Fidelity plain =
        StoredAgainstExact(encoder, queue, format, PlainBlocks);
    const Fidelity gentle =
        StoredAgainstExact(encoder, queue, format, GentleBlocks);
    const Fidelity random = StoredAgainstExact(encoder, queue, format, nullptr);
    if (!plain.ran || !gentle.ran || !random.ran) {
      std::printf("     %s: the driver cannot blit this pair of formats, "
                  "stored image not read back\n",
                  Name(format));
      continue;
    }
    const bool color = IsColor(format);
    const bool mixed = format == EtcFormat::kEtc2Rgba8;
    const bool ok = (color ? plain.worst == 0 : plain.color >= 70) &&
                    gentle.color >= (mixed ? 32 : (color ? 48 : 42)) &&
                    gentle.alpha >= 36 && random.color >= (color ? 20 : 34) &&
                    random.alpha >= 28;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "%s stored as BC and read back through the driver: one "
                  "colour blocks %s (%.1f dB), gentle blocks %.1f dB (alpha "
                  "%.1f dB), random blocks %.1f dB (alpha %.1f dB)",
                  Name(format), plain.worst == 0 ? "exact" : "close",
                  plain.color, gentle.color, gentle.alpha, random.color,
                  random.alpha);
    Result(ok, line);
  }
}

void StoredKinds(Etc2GpuDecoder& encoder, Queue& queue) {
  const struct {
    const char* kind;
    double floor;
  } kinds[] = {{"individual", 25}, {"differential", 25.5}, {"T", 31}, {"H", 28}, {"planar", 28}};
  bool ok = true;
  char line[320] = "ETC2_RGB8 stored as BC, random blocks of one kind each:";
  for (const auto& kind : kinds) {
    g_wanted_kind = kind.kind;
    const Fidelity result =
        StoredAgainstExact(encoder, queue, EtcFormat::kEtc2Rgb8, KindBlocks);
    ok = ok && result.ran && result.color >= kind.floor;
    char part[64];
    std::snprintf(part, sizeof(part), " %s %.1f dB", kind.kind, result.color);
    std::strncat(line, part, sizeof(line) - std::strlen(line) - 1);
  }
  Result(ok, line);
}

void EncoderBenchmark(Etc2GpuDecoder& encoder, Queue& main) {
  const std::uint32_t kCount = 16;
  const std::uint32_t kSize = 1024;
  for (int kind = 0; kind < 3; ++kind) {
    std::vector<Spec> specs;
    for (std::uint32_t index = 0; index < kCount; ++index) {
      Spec spec;
      spec.format = EtcFormat::kEtc2Rgba8;
      spec.width = kSize;
      spec.height = kSize;
      spec.bc = true;
      spec.target_buffer = index % 4;
      specs.push_back(spec);
    }
    Outcome warm;
    Options options;
    options.quiet = true;
    options.fill = kind == 0 ? PlainBlocks : (kind == 1 ? GentleBlocks : nullptr);
    Run("benchmark warm-up", encoder, main, specs, options);
    options.out = &warm;
    Run("benchmark", encoder, main, specs, options);
    std::printf("     encode %u x %ux%u ETC2_RGBA8, %s blocks: GPU %.1f ms "
                "submit to done, %.1f million blocks a second\n",
                kCount, kSize, kSize,
                kind == 0 ? "one colour" : (kind == 1 ? "gentle" : "random"),
                warm.seconds * 1000, warm.blocks / warm.seconds / 1e6);
  }
}

struct CopySpec {
  VkDeviceSize size = 0;
  VkDeviceSize gap = 0;
  std::size_t target = 0;
};

bool RunCopies(const std::string& name, Etc2GpuDecoder& decoder, Queue& queue,
               const std::vector<CopySpec>& specs) {
  std::size_t target_count = 0;
  for (const CopySpec& spec : specs) {
    target_count = std::max(target_count, spec.target + 1);
  }
  std::vector<VkDeviceSize> target_end(target_count, 0);
  std::vector<Etc2GpuCopy> copies(specs.size());
  std::vector<std::vector<std::uint8_t>> data(specs.size());
  VkDeviceSize bytes = 0;
  for (std::size_t index = 0; index < specs.size(); ++index) {
    data[index].resize(specs[index].size);
    FillRandom(data[index].data(), data[index].size());
    copies[index].data = data[index].data();
    copies[index].size = specs[index].size;
    copies[index].target_offset = target_end[specs[index].target] +
                                  specs[index].gap;
    target_end[specs[index].target] =
        copies[index].target_offset + specs[index].size;
    bytes += specs[index].size;
  }
  std::vector<Buffer> targets;
  std::vector<Buffer> readbacks;
  for (std::size_t index = 0; index < target_count; ++index) {
    const VkDeviceSize size = (target_end[index] + 259) / 4 * 4;
    targets.push_back(MakeBuffer(
        size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        nullptr, true));
    readbacks.push_back(MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT));
  }
  for (std::size_t index = 0; index < specs.size(); ++index) {
    copies[index].target = targets[specs[index].target].buffer;
  }
  FillDevice(queue, targets);
  const auto start = std::chrono::steady_clock::now();
  const bool copied =
      decoder.Copy(queue.queue, queue.family, copies.data(), copies.size());
  vk.QueueWaitIdle(queue.queue);
  const double seconds = Seconds(start);
  ReadBack(queue, targets, readbacks);
  std::uint64_t wrong = 0;
  std::uint64_t stray = 0;
  std::vector<std::vector<bool>> covered(target_count);
  for (std::size_t index = 0; index < target_count; ++index) {
    covered[index].assign(readbacks[index].size, false);
  }
  for (std::size_t index = 0; copied && index < specs.size(); ++index) {
    const std::uint8_t* result =
        readbacks[specs[index].target].data + copies[index].target_offset;
    for (VkDeviceSize byte = 0; byte < specs[index].size; ++byte) {
      if (result[byte] != data[index][byte]) {
        ++wrong;
      }
    }
    std::vector<bool>& mask = covered[specs[index].target];
    std::fill(mask.begin() + copies[index].target_offset,
              mask.begin() + copies[index].target_offset + specs[index].size,
              true);
  }
  for (std::size_t index = 0; index < target_count; ++index) {
    for (VkDeviceSize byte = 0; byte < readbacks[index].size; ++byte) {
      if (!covered[index][byte] && readbacks[index].data[byte] != 0xcd) {
        ++stray;
      }
    }
    FreeBuffer(&targets[index]);
    FreeBuffer(&readbacks[index]);
  }
  const bool ok = copied && wrong == 0 && stray == 0;
  char line[512];
  std::snprintf(line, sizeof(line),
                "%s: %zu copies, %llu bytes, copy %s, %llu wrong bytes, %llu "
                "stray bytes (%.1f ms)",
                name.c_str(), specs.size(),
                static_cast<unsigned long long>(bytes),
                copied ? "ran" : "refused",
                static_cast<unsigned long long>(wrong),
                static_cast<unsigned long long>(stray), seconds * 1000);
  Result(ok, line);
  return ok;
}

void CopyTests(Etc2GpuDecoder& decoder, Etc2GpuDecoder& stress) {
  for (Queue& queue : queues) {
    std::vector<CopySpec> odd;
    const VkDeviceSize odd_sizes[] = {1, 2, 3, 5, 7, 4093, 65537, 33};
    for (std::size_t index = 0; index < 8; ++index) {
      odd.push_back({odd_sizes[index], index * 3 % 7, index % 2});
    }
    RunCopies(std::string("odd sizes at odd offsets on the ") + queue.name +
                  " queue",
              decoder, queue, odd);
  }
  RunCopies("copies larger than a pass", decoder, queues[0],
            {{VkDeviceSize{40} << 20, 0, 0},
             {(VkDeviceSize{16} << 20) + 1, 5, 1},
             {33, 0, 0}});
  std::vector<CopySpec> many;
  for (int index = 0; index < 200; ++index) {
    many.push_back({1 + Below(300000), Below(65), Below(3)});
  }
  RunCopies("200 random copies", decoder, queues[0], many);
  std::vector<CopySpec> small;
  for (int index = 0; index < 100; ++index) {
    small.push_back({1 + Below(20000), Below(65), Below(2)});
  }
  RunCopies("100 copies split into 4 KiB passes", stress, queues[0], small);
}

struct ImageCase {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t mips;
  std::uint32_t layers;
  std::uint32_t row_pad;
  std::uint32_t height_pad;
  VkDeviceSize gap;
};

bool EmulatedUpload(VulkanEtc2Emulation& emulation, Queue& queue,
                    VkFormat format, const ImageCase& shape, bool tracked,
                    bool copy2, bool resubmit, std::uint32_t* gpu_eligible,
                    std::uint32_t* uploads_total, bool bc = false) {
  Etc2EmulatedFormat mapped;
  LookupEmulatedEtc2Format(format, &mapped);
  const EtcFormat etc = mapped.etc_format;
  const VkDeviceSize block_bytes = EtcBlockBytes(etc);
  const auto stored_bytes = [etc, bc](std::uint32_t width,
                                      std::uint32_t height) {
    Spec spec;
    spec.format = etc;
    spec.width = width;
    spec.height = height;
    spec.bc = bc;
    return StoredLayerBytes(spec);
  };

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {shape.width, shape.height, 1};
  image_info.mipLevels = shape.mips;
  image_info.arrayLayers = shape.layers;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  if (emulation.CreateImage(device, &image_info, nullptr, &image) !=
      VK_SUCCESS) {
    std::printf("    emulated vkCreateImage failed\n");
    return false;
  }
  VkMemoryRequirements requirements{};
  vk.GetImageMemoryRequirements(device, image, &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  vk.AllocateMemory(device, &allocate, nullptr, &image_memory);
  vk.BindImageMemory(device, image, image_memory, 0);

  std::vector<VkBufferImageCopy> regions;
  std::vector<VkBufferImageCopy2> regions2;
  std::vector<VkBufferImageCopy> readbacks;
  VkDeviceSize source_end = 0;
  VkDeviceSize readback_end = 0;
  for (std::uint32_t mip = 0; mip < shape.mips; ++mip) {
    const std::uint32_t width = std::max(1U, shape.width >> mip);
    const std::uint32_t height = std::max(1U, shape.height >> mip);
    const std::uint32_t blocks_wide = (width + 3) / 4;
    const std::uint32_t block_rows = (height + 3) / 4;
    VkBufferImageCopy region{};
    region.bufferOffset =
        (source_end + shape.gap + block_bytes - 1) / block_bytes * block_bytes;
    region.bufferRowLength =
        shape.row_pad != 0 ? (blocks_wide + shape.row_pad) * 4 : 0;
    region.bufferImageHeight =
        shape.height_pad != 0 ? (block_rows + shape.height_pad) * 4 : 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0,
                               shape.layers};
    region.imageExtent = {width, height, 1};
    const VkDeviceSize row_stride =
        VkDeviceSize{blocks_wide + shape.row_pad} * block_bytes;
    const VkDeviceSize layer_stride =
        VkDeviceSize{block_rows + shape.height_pad} * row_stride;
    source_end = region.bufferOffset + (shape.layers - 1) * layer_stride +
                 (block_rows - 1) * row_stride + blocks_wide * block_bytes;
    regions.push_back(region);
    VkBufferImageCopy2 region2{};
    region2.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region2.bufferOffset = region.bufferOffset;
    region2.bufferRowLength = region.bufferRowLength;
    region2.bufferImageHeight = region.bufferImageHeight;
    region2.imageSubresource = region.imageSubresource;
    region2.imageOffset = region.imageOffset;
    region2.imageExtent = region.imageExtent;
    regions2.push_back(region2);
    VkBufferImageCopy readback{};
    readback.bufferOffset = readback_end;
    readback.imageSubresource = region.imageSubresource;
    readback.imageExtent = region.imageExtent;
    readbacks.push_back(readback);
    readback_end +=
        (stored_bytes(width, height) * shape.layers + 15) / 16 * 16;
    ++*uploads_total;
    if (bc || Etc2GpuDecodable(etc, width)) {
      ++*gpu_eligible;
    }
  }
  Buffer source = MakeBuffer(source_end + 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             tracked ? &emulation : nullptr);
  Buffer readback =
      MakeBuffer(readback_end + 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

  Begin(queue, resubmit ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, shape.mips, 0,
                              shape.layers};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vk.CmdPipelineBarrier(queue.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
  if (copy2) {
    VkCopyBufferToImageInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    info.srcBuffer = source.buffer;
    info.dstImage = image;
    info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    info.regionCount = static_cast<std::uint32_t>(regions2.size());
    info.pRegions = regions2.data();
    emulation.CmdCopyBufferToImage2(device, queue.command, &info);
  } else {
    emulation.CmdCopyBufferToImage(
        device, queue.command, source.buffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(regions.size()), regions.data());
  }
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vk.CmdPipelineBarrier(queue.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &barrier);
  vk.CmdCopyImageToBuffer(queue.command, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.buffer,
                          static_cast<std::uint32_t>(readbacks.size()),
                          readbacks.data());
  HostBarrier(queue.command);
  vk.EndCommandBuffer(queue.command);

  bool ok = true;
  for (int pass = 0; pass < (resubmit ? 2 : 1); ++pass) {
    FillRandom(source.data, source.size);
    std::memset(readback.data, 0xcd, readback.size);
    emulation.PrepareSubmit(queue.queue, &queue.command, 1);
    if (!SubmitAndWait(queue)) {
      std::printf("    submit failed\n");
      ok = false;
      break;
    }
    for (std::uint32_t mip = 0; mip < shape.mips; ++mip) {
      const VkBufferImageCopy& region = regions[mip];
      const std::uint32_t width = region.imageExtent.width;
      const std::uint32_t height = region.imageExtent.height;
      const std::uint32_t blocks_wide = (width + 3) / 4;
      const std::uint32_t block_rows = (height + 3) / 4;
      const VkDeviceSize row_stride =
          VkDeviceSize{blocks_wide + shape.row_pad} * block_bytes;
      const VkDeviceSize layer_stride =
          VkDeviceSize{block_rows + shape.height_pad} * row_stride;
      const std::size_t layer_bytes = stored_bytes(width, height);
      std::vector<std::uint8_t> packed(blocks_wide * block_rows * block_bytes);
      std::vector<std::uint8_t> expected(layer_bytes);
      for (std::uint32_t layer = 0; layer < shape.layers; ++layer) {
        for (std::uint32_t row = 0; row < block_rows; ++row) {
          std::memcpy(packed.data() + row * blocks_wide * block_bytes,
                      source.data + region.bufferOffset +
                          layer * layer_stride + row * row_stride,
                      blocks_wide * block_bytes);
        }
        if (bc) {
          EncodeEtcImageToBc(etc, packed.data(), packed.size(), width, height,
                             expected.data(), expected.size());
        } else {
          DecodeEtcImage(etc, packed.data(), packed.size(), width, height,
                         expected.data(), expected.size());
        }
        if (std::memcmp(readback.data + readbacks[mip].bufferOffset +
                            layer * layer_bytes,
                        expected.data(), layer_bytes) != 0) {
          std::printf("    format %d %ux%u mip %u layer %u differs (pass %d)\n",
                      static_cast<int>(format), width, height, mip, layer,
                      pass);
          ok = false;
        }
      }
    }
  }
  emulation.ReleaseCommandBuffer(queue.command);
  FreeBuffer(&readback);
  FreeBuffer(&source, tracked ? &emulation : nullptr);
  emulation.DestroyImage(device, image, nullptr);
  vk.FreeMemory(device, image_memory, nullptr);
  return ok;
}

const VkFormat kVkFormats[] = {
    VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,   VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
    VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
    VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
    VK_FORMAT_EAC_R11_UNORM_BLOCK,       VK_FORMAT_EAC_R11_SNORM_BLOCK,
    VK_FORMAT_EAC_R11G11_UNORM_BLOCK,    VK_FORMAT_EAC_R11G11_SNORM_BLOCK};

const std::vector<ImageCase> kEvenShapes = {{64, 64, 6, 1, 0, 0, 0},
                                            {256, 128, 7, 1, 0, 0, 0},
                                            {40, 24, 3, 3, 2, 1, 0},
                                            {6, 10, 2, 2, 0, 0, 8},
                                            {512, 512, 9, 1, 0, 0, 0}};
const std::vector<ImageCase> kFullChains = {{64, 32, 7, 1, 0, 0, 0},
                                            {37, 21, 6, 3, 1, 2, 16},
                                            {1, 1, 1, 1, 0, 0, 0},
                                            {3, 5, 3, 2, 0, 0, 0}};

bool EmulatedSet(VulkanEtc2Emulation& emulation, Queue& queue, bool gpu_only,
                 std::uint32_t* eligible, std::uint32_t* total,
                 bool bc = false) {
  bool ok = true;
  for (VkFormat format : kVkFormats) {
    Etc2EmulatedFormat mapped;
    LookupEmulatedEtc2Format(format, &mapped);
    const bool narrow = EtcDecodedTexelBytes(mapped.etc_format) == 2;
    for (const ImageCase& shape : gpu_only ? kEvenShapes : kFullChains) {
      ImageCase used = shape;
      const auto all_even = [&used](std::uint32_t mips) {
        for (std::uint32_t mip = 0; mip < mips; ++mip) {
          if (std::max(1U, used.width >> mip) % 2 != 0) {
            return false;
          }
        }
        return true;
      };
      if (gpu_only && narrow && !bc) {
        while (used.mips > 1 && !all_even(used.mips)) {
          --used.mips;
        }
        if (!all_even(used.mips)) {
          continue;
        }
      }
      for (int variant = 0; variant < 2; ++variant) {
        ok = EmulatedUpload(emulation, queue, format, used, !gpu_only,
                            variant == 1 && has_copy2, variant == 1, eligible,
                            total, bc) &&
             ok;
      }
    }
  }
  return ok;
}

void EmulationTests(PFN_vkGetDeviceProcAddr gdpa) {
  {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa,
                             &properties.limits, families.data(),
                             static_cast<std::uint32_t>(families.size()));
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    for (Queue& queue : queues) {
      for (int mode = 0; mode < 2; ++mode) {
        const bool gpu_only = queue.compute && mode == 0;
        if (!queue.compute && mode == 0) {
          continue;
        }
        std::uint32_t eligible = 0;
        std::uint32_t total = 0;
        bool ok = EmulatedSet(emulation, queue, gpu_only, &eligible, &total);
        if (gpu_only && eligible != total) {
          std::printf("    %u uploads were not GPU eligible\n",
                      total - eligible);
          ok = false;
        }
        char line[256];
        std::snprintf(
            line, sizeof(line),
            "emulated uploads on the %s queue, %s: %u uploads (%u GPU "
            "eligible), images read back match the CPU decode",
            queue.name,
            gpu_only ? "source memory hidden from the CPU path"
                     : (queue.compute
                            ? "mixed GPU and CPU uploads"
                            : "CPU decode carried into GPU staging"),
            total, eligible);
        Result(ok, line);
      }
    }
    emulation.DestroyDevice(device);
  }
  {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa);
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    std::uint32_t eligible = 0;
    std::uint32_t total = 0;
    const bool ok =
        EmulatedSet(emulation, queues[0], false, &eligible, &total);
    Result(ok, "emulated uploads without a GPU decoder (host staging, CPU "
               "decode): " +
                   std::to_string(total) +
                   " uploads, images read back match the CPU decode");
    emulation.DestroyDevice(device);
  }
}

bool GrownTexture(VulkanEtc2Emulation& emulation, Queue& queue,
                  VkFormat format, bool bc) {
  Etc2EmulatedFormat mapped;
  LookupEmulatedEtc2Format(format, &mapped);
  const EtcFormat etc = mapped.etc_format;
  const std::uint32_t kSmall = 64;
  const std::uint32_t kSmallMips = 7;
  const VkDeviceSize block_bytes = EtcBlockBytes(etc);
  const auto stored_bytes = [etc, bc](std::uint32_t size) {
    Spec spec;
    spec.format = etc;
    spec.width = size;
    spec.height = size;
    spec.bc = bc;
    return StoredLayerBytes(spec);
  };
  VkImage images[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  for (int index = 0; index < 2; ++index) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {kSmall << index, kSmall << index, 1};
    info.mipLevels = kSmallMips + index;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (emulation.CreateImage(device, &info, nullptr, &images[index]) !=
        VK_SUCCESS) {
      std::printf("    emulated vkCreateImage failed\n");
      return false;
    }
    VkMemoryRequirements requirements{};
    vk.GetImageMemoryRequirements(device, images[index], &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk.AllocateMemory(device, &allocate, nullptr, &memories[index]);
    vk.BindImageMemory(device, images[index], memories[index], 0);
  }

  std::vector<VkBufferImageCopy> small_uploads;
  std::vector<VkImageCopy> copies;
  std::vector<VkBufferImageCopy> readbacks;
  VkDeviceSize source_end = 0;
  VkDeviceSize readback_end = 0;
  for (std::uint32_t mip = 0; mip < kSmallMips; ++mip) {
    const std::uint32_t size = std::max(1U, kSmall >> mip);
    VkBufferImageCopy upload{};
    upload.bufferOffset = source_end;
    upload.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    upload.imageExtent = {size, size, 1};
    source_end += VkDeviceSize{(size + 3) / 4} * ((size + 3) / 4) * block_bytes;
    small_uploads.push_back(upload);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 0, 1};
    copy.extent = {size, size, 1};
    copies.push_back(copy);
  }
  VkBufferImageCopy top{};
  top.bufferOffset = source_end;
  top.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  top.imageExtent = {kSmall * 2, kSmall * 2, 1};
  source_end += VkDeviceSize{kSmall / 2} * (kSmall / 2) * block_bytes;
  for (std::uint32_t mip = 0; mip <= kSmallMips; ++mip) {
    const std::uint32_t size = std::max(1U, (kSmall * 2) >> mip);
    VkBufferImageCopy readback{};
    readback.bufferOffset = readback_end;
    readback.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    readback.imageExtent = {size, size, 1};
    readbacks.push_back(readback);
    readback_end += (stored_bytes(size) + 15) / 16 * 16;
  }
  Buffer source = MakeBuffer(source_end + 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             &emulation);
  Buffer readback =
      MakeBuffer(readback_end + 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  FillRandom(source.data, source.size);
  std::memset(readback.data, 0xcd, readback.size);

  Begin(queue);
  MoveImage(queue.command, images[0], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSmallMips);
  MoveImage(queue.command, images[1], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSmallMips + 1);
  emulation.CmdCopyBufferToImage(
      device, queue.command, source.buffer, images[0],
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      static_cast<std::uint32_t>(small_uploads.size()), small_uploads.data());
  MoveImage(queue.command, images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kSmallMips);
  emulation.CmdCopyImage(device, queue.command, images[0],
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[1],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         static_cast<std::uint32_t>(copies.size()),
                         copies.data());
  emulation.CmdCopyBufferToImage(device, queue.command, source.buffer,
                                 images[1],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &top);
  MoveImage(queue.command, images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kSmallMips + 1);
  vk.CmdCopyImageToBuffer(queue.command, images[1],
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback.buffer,
                          static_cast<std::uint32_t>(readbacks.size()),
                          readbacks.data());
  HostBarrier(queue.command);
  vk.EndCommandBuffer(queue.command);
  emulation.PrepareSubmit(queue.queue, &queue.command, 1);
  bool ok = SubmitAndWait(queue);
  for (std::uint32_t mip = 0; ok && mip <= kSmallMips; ++mip) {
    const std::uint32_t size = std::max(1U, (kSmall * 2) >> mip);
    const VkDeviceSize from =
        mip == 0 ? top.bufferOffset : small_uploads[mip - 1].bufferOffset;
    const std::size_t packed =
        static_cast<std::size_t>((size + 3) / 4) * ((size + 3) / 4) *
        block_bytes;
    std::vector<std::uint8_t> expected(stored_bytes(size));
    if (bc) {
      EncodeEtcImageToBc(etc, source.data + from, packed, size, size,
                         expected.data(), expected.size());
    } else {
      DecodeEtcImage(etc, source.data + from, packed, size, size,
                     expected.data(), expected.size());
    }
    if (std::memcmp(readback.data + readbacks[mip].bufferOffset,
                    expected.data(), expected.size()) != 0) {
      std::printf("    format %d: level %u of the grown texture differs\n",
                  static_cast<int>(format), mip);
      ok = false;
    }
  }
  emulation.ReleaseCommandBuffer(queue.command);
  FreeBuffer(&readback);
  FreeBuffer(&source, &emulation);
  for (int index = 0; index < 2; ++index) {
    emulation.DestroyImage(device, images[index], nullptr);
    vk.FreeMemory(device, memories[index], nullptr);
  }
  return ok;
}

void StoredEmulationTests(PFN_vkGetDeviceProcAddr gdpa) {
  for (int stored = 0; stored < 2; ++stored) {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa,
                             &properties.limits, families.data(),
                             static_cast<std::uint32_t>(families.size()),
                             stored == 1);
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    bool grown = true;
    for (VkFormat format : kVkFormats) {
      grown = GrownTexture(emulation, queues[0], format, stored == 1) && grown;
    }
    Result(grown,
           std::string("a texture grown the way Roblox does it (every level "
                       "copied one level down into a larger image), ") +
               (stored == 1 ? "stored as BC" : "stored decoded") +
               ": every format, every level matches");
    if (stored == 0) {
      emulation.DestroyDevice(device);
      continue;
    }
    emulation.WaitForEncoder(device);
    for (Queue& queue : queues) {
      for (int mode = 0; mode < 2; ++mode) {
        const bool gpu_only = queue.compute && mode == 0;
        if (!queue.compute && mode == 0) {
          continue;
        }
        std::uint32_t eligible = 0;
        std::uint32_t total = 0;
        bool ok =
            EmulatedSet(emulation, queue, gpu_only, &eligible, &total, true);
        if (gpu_only && eligible != total) {
          ok = false;
        }
        char line[256];
        std::snprintf(
            line, sizeof(line),
            "emulated uploads stored as BC on the %s queue, %s: %u uploads, "
            "images read back match the CPU encoder",
            queue.name,
            gpu_only ? "source memory hidden from the CPU path"
                     : (queue.compute ? "mixed GPU and CPU uploads"
                                      : "CPU encode carried into GPU staging"),
            total);
        Result(ok, line);
      }
    }
    emulation.DestroyDevice(device);
    emulation.ForgetDevice(device);
  }
  {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa,
                             nullptr, nullptr, 0, true);
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    std::uint32_t eligible = 0;
    std::uint32_t total = 0;
    const bool ok =
        EmulatedSet(emulation, queues[0], false, &eligible, &total, true);
    Result(ok, "emulated uploads stored as BC without a GPU encoder (host "
               "staging, CPU encode): " +
                   std::to_string(total) +
                   " uploads, images read back match the CPU encoder");
    emulation.DestroyDevice(device);
  }
}

bool OneBitOff(EtcFormat format, const std::uint8_t* source,
               std::size_t source_bytes, std::uint32_t width,
               std::uint32_t height, std::uint8_t* destination,
               std::size_t destination_bytes) {
  const bool ok = EncodeEtcImageToBc(format, source, source_bytes, width,
                                     height, destination, destination_bytes);
  if (ok && format == EtcFormat::kEacRg11Signed) {
    destination[destination_bytes - 1] ^= 1;
  }
  return ok;
}

void DoubtedEncoderTests(PFN_vkGetDeviceProcAddr gdpa) {
  std::shared_ptr<Etc2GpuEncoder> doubted =
      Etc2GpuEncoder::Start(device, gdpa, OneBitOff);
  if (doubted != nullptr) {
    doubted->Wait();
  }
  std::unique_ptr<Etc2GpuDecoder> decoder = Etc2GpuDecoder::Create(
      device, properties.limits, memory_properties, families.data(),
      static_cast<std::uint32_t>(families.size()), gdpa, doubted);
  if (doubted == nullptr || decoder == nullptr) {
    Result(false, "an encoder with another reference could not be made");
    return;
  }
  const bool offered = decoder->Encodes();
  Spec spec;
  spec.format = EtcFormat::kEtc2Rgb8;
  spec.width = 64;
  spec.height = 64;
  spec.bc = true;
  Options refused;
  refused.expect = false;
  Run("an encoder one bit away from its reference is refused on its first "
      "upload, target untouched",
      *decoder, queues[0], {spec}, refused);
  Result(offered && doubted->Checked() && !decoder->Encodes(),
         "after the failed check the decoder takes no more encodes");
  spec.bc = false;
  Run("and it still decodes", *decoder, queues[0], {spec});
  decoder.reset();
  doubted.reset();

  VulkanEtc2Emulation emulation;
  emulation.RegisterDevice(device, physical, true, memory_properties, gdpa,
                           &properties.limits, families.data(),
                           static_cast<std::uint32_t>(families.size()), true,
                           OneBitOff);
  for (const Queue& queue : queues) {
    emulation.RegisterQueue(device, queue.queue, queue.family);
  }
  emulation.WaitForEncoder(device);
  std::uint32_t eligible = 0;
  std::uint32_t total = 0;
  const bool ok =
      EmulatedSet(emulation, queues[0], false, &eligible, &total, true);
  Result(ok, "emulated uploads stored as BC with a GPU encoder that fails "
             "its check: " +
                 std::to_string(total) +
                 " uploads, images read back match the CPU encoder");
  emulation.DestroyDevice(device);
  emulation.ForgetDevice(device);
}

void DecoderBenchmark(Etc2GpuDecoder& decoder, Queue& main) {
  const std::uint32_t kCount = 64;
  const std::uint32_t kSize = 1024;
  std::vector<Spec> specs;
  for (std::uint32_t index = 0; index < kCount; ++index) {
    Spec spec;
    spec.format = EtcFormat::kEtc2Rgba8;
    spec.width = kSize;
    spec.height = kSize;
    spec.target_buffer = index % 4;
    specs.push_back(spec);
  }
  Outcome warm;
  Options options;
  options.quiet = true;
  Run("benchmark warm-up", decoder, main, specs, options);
  options.out = &warm;
  Run("benchmark", decoder, main, specs, options);
  const std::size_t compressed =
      static_cast<std::size_t>(kSize / 4) * (kSize / 4) * 16;
  const std::size_t decoded = static_cast<std::size_t>(kSize) * kSize * 4;
  std::vector<std::uint8_t> source(compressed * kCount);
  std::vector<std::uint8_t> target(decoded * kCount);
  FillRandom(source.data(), source.size());
  std::vector<EtcDecodeJob> jobs(kCount);
  for (std::uint32_t index = 0; index < kCount; ++index) {
    jobs[index].format = EtcFormat::kEtc2Rgba8;
    jobs[index].source = source.data() + index * compressed;
    jobs[index].source_bytes = compressed;
    jobs[index].width = kSize;
    jobs[index].height = kSize;
    jobs[index].destination = target.data() + index * decoded;
    jobs[index].destination_bytes = decoded;
  }
  auto start = std::chrono::steady_clock::now();
  DecodeEtcJobs(jobs.data(), jobs.size(), 8);
  const double cpu8 = Seconds(start);
  start = std::chrono::steady_clock::now();
  DecodeEtcJobs(jobs.data(), 8, 1);
  const double cpu1 = Seconds(start) * kCount / 8;
  std::printf("     %u x %ux%u ETC2_RGBA8 (%zu MiB decoded): GPU %.1f ms "
              "submit to done, CPU with 8 threads %.1f ms, CPU on one thread "
              "%.1f ms\n",
              kCount, kSize, kSize, decoded * kCount >> 20,
              warm.seconds * 1000, cpu8 * 1000, cpu1 * 1000);
}

void RenderThreadCost(VulkanEtc2Emulation& emulation, Queue& queue,
                      const char* label) {
  const std::uint32_t kImages = 8;
  const std::uint32_t kSize = 1024;
  const std::uint32_t kMips = 11;
  std::vector<VkImage> images(kImages);
  std::vector<VkDeviceMemory> memories(kImages);
  std::vector<VkBufferImageCopy> regions;
  VkDeviceSize source_end = 0;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    const std::uint32_t size = std::max(1U, kSize >> mip);
    VkBufferImageCopy region{};
    region.bufferOffset = source_end;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    region.imageExtent = {size, size, 1};
    regions.push_back(region);
    source_end += VkDeviceSize{(size + 3) / 4} * ((size + 3) / 4) * 16;
  }
  Buffer source = MakeBuffer(source_end * kImages,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &emulation);
  for (std::uint32_t index = 0; index < kImages; ++index) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
    info.extent = {kSize, kSize, 1};
    info.mipLevels = kMips;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    emulation.CreateImage(device, &info, nullptr, &images[index]);
    VkMemoryRequirements requirements{};
    vk.GetImageMemoryRequirements(device, images[index], &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk.AllocateMemory(device, &allocate, nullptr, &memories[index]);
    vk.BindImageMemory(device, images[index], memories[index], 0);
  }
  Begin(queue, 0);
  for (std::uint32_t index = 0; index < kImages; ++index) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kMips, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.CmdPipelineBarrier(queue.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &barrier);
    std::vector<VkBufferImageCopy> image_regions = regions;
    for (VkBufferImageCopy& region : image_regions) {
      region.bufferOffset += index * source_end;
    }
    emulation.CmdCopyBufferToImage(device, queue.command, source.buffer,
                                   images[index],
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                                   image_regions.data());
  }
  vk.EndCommandBuffer(queue.command);
  double best_prepare = 1e9;
  double best_queue = 1e9;
  for (int rep = 0; rep < 5; ++rep) {
    FillRandom(source.data, source.size);
    auto start = std::chrono::steady_clock::now();
    emulation.PrepareSubmit(queue.queue, &queue.command, 1);
    best_prepare = std::min(best_prepare, Seconds(start));
    start = std::chrono::steady_clock::now();
    SubmitAndWait(queue);
    best_queue = std::min(best_queue, Seconds(start));
  }
  std::printf("     %s: %u x %ux%u ETC2_RGBA8 with %u mips, PrepareSubmit "
              "%.2f ms on the submitting thread, queue idle %.2f ms later\n",
              label, kImages, kSize, kSize, kMips, best_prepare * 1000,
              best_queue * 1000);
  emulation.ReleaseCommandBuffer(queue.command);
  for (std::uint32_t index = 0; index < kImages; ++index) {
    emulation.DestroyImage(device, images[index], nullptr);
    vk.FreeMemory(device, memories[index], nullptr);
  }
  FreeBuffer(&source, &emulation);
}

void RenderThreadBenchmarks(PFN_vkGetDeviceProcAddr gdpa) {
  {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa,
                             &properties.limits, families.data(),
                             static_cast<std::uint32_t>(families.size()));
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    RenderThreadCost(emulation, queues[0], "GPU decode, graphics queue");
    for (Queue& queue : queues) {
      if (!queue.compute) {
        RenderThreadCost(emulation, queue,
                         "CPU decode carried to GPU staging, transfer queue");
      }
    }
    emulation.DestroyDevice(device);
  }
  {
    VulkanEtc2Emulation emulation;
    emulation.RegisterDevice(device, physical, true, memory_properties, gdpa);
    for (const Queue& queue : queues) {
      emulation.RegisterQueue(device, queue.queue, queue.family);
    }
    RenderThreadCost(emulation, queues[0],
                     "CPU decode into host staging (before this change)");
    emulation.DestroyDevice(device);
  }
}

bool Setup() {
#if defined(_WIN32)
  HMODULE module = LoadLibraryA("vulkan-1.dll");
  gipa = module != nullptr
             ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                   reinterpret_cast<void (*)()>(
                       GetProcAddress(module, "vkGetInstanceProcAddr")))
             : nullptr;
#else
  if (!SDL_Init(SDL_INIT_VIDEO) || !SDL_Vulkan_LoadLibrary(nullptr)) {
    std::printf("SDL Vulkan: %s\n", SDL_GetError());
    return false;
  }
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      SDL_Vulkan_GetVkGetInstanceProcAddr());
#endif
  if (gipa == nullptr) {
    std::printf("no Vulkan loader\n");
    return false;
  }
  const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "etc2gpu_test";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &app;
  if (create_instance == nullptr ||
      create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
    std::printf("vkCreateInstance failed\n");
    return false;
  }
#define LOAD_INSTANCE(name) \
  vk.name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));
  INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
  std::uint32_t count = 0;
  vk.EnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vk.EnumeratePhysicalDevices(instance, &count, devices.data());
  const char* wanted = std::getenv("ETC2GPU_DEVICE");
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties candidate_properties{};
    vk.GetPhysicalDeviceProperties(candidate, &candidate_properties);
    if (wanted != nullptr && wanted[0] != 0) {
      if (std::strstr(candidate_properties.deviceName, wanted) != nullptr) {
        physical = candidate;
        properties = candidate_properties;
        break;
      }
      continue;
    }
    if (physical == VK_NULL_HANDLE ||
        candidate_properties.deviceType ==
            VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physical = candidate;
      properties = candidate_properties;
      if (candidate_properties.deviceType ==
          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        break;
      }
    }
  }
  if (physical == VK_NULL_HANDLE) {
    std::printf("no Vulkan device\n");
    return false;
  }
  vk.GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  families.resize(count);
  vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  const struct {
    const char* name;
    VkQueueFlags wanted;
    VkQueueFlags unwanted;
  } kinds[] = {{"graphics", VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0},
               {"compute-only", VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT},
               {"transfer-only", VK_QUEUE_TRANSFER_BIT,
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT}};
  for (const auto& kind : kinds) {
    for (std::uint32_t family = 0; family < count; ++family) {
      if ((families[family].queueFlags & kind.wanted) == kind.wanted &&
          (families[family].queueFlags & kind.unwanted) == 0 &&
          families[family].queueCount != 0) {
        Queue queue;
        queue.name = kind.name;
        queue.family = family;
        queue.compute =
            (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        queues.push_back(queue);
        break;
      }
    }
  }
  if (queues.empty() || std::string(queues[0].name) != "graphics") {
    std::printf("no graphics and compute queue family\n");
    return false;
  }
  const float priority = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> queue_infos;
  for (const Queue& queue : queues) {
    VkDeviceQueueCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.queueFamilyIndex = queue.family;
    info.queueCount = 1;
    info.pQueuePriorities = &priority;
    queue_infos.push_back(info);
  }
  VkPhysicalDeviceFeatures supported{};
  vk.GetPhysicalDeviceFeatures(physical, &supported);
  VkPhysicalDeviceFeatures enabled{};
  enabled.textureCompressionBC = supported.textureCompressionBC;
  has_bc = supported.textureCompressionBC == VK_TRUE;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount =
      static_cast<std::uint32_t>(queue_infos.size());
  device_info.pQueueCreateInfos = queue_infos.data();
  device_info.pEnabledFeatures = &enabled;
  if (vk.CreateDevice(physical, &device_info, nullptr, &device) !=
      VK_SUCCESS) {
    std::printf("vkCreateDevice failed\n");
    return false;
  }
#define LOAD_DEVICE(name)                                        \
  vk.name = reinterpret_cast<PFN_vk##name>(                      \
      vk.GetDeviceProcAddr(device, "vk" #name));                 \
  if (vk.name == nullptr) {                                      \
    std::printf("missing vk" #name "\n");                        \
    return false;                                                \
  }
  DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
  has_copy2 = vk.GetDeviceProcAddr(device, "vkCmdCopyBufferToImage2") !=
              nullptr;
  for (Queue& queue : queues) {
    vk.GetDeviceQueue(device, queue.family, 0, &queue.queue);
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue.family;
    vk.CreateCommandPool(device, &pool_info, nullptr, &queue.pool);
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = queue.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    vk.AllocateCommandBuffers(device, &allocate, &queue.command);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t random_blocks =
      argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (1ULL << 20);
  const bool benchmarks = argc <= 2 || std::strcmp(argv[2], "nobench") != 0;
  if (!Setup()) {
    return 1;
  }
  const VkPhysicalDeviceLimits& limits = properties.limits;
  std::printf("device: %s, maxStorageBufferRange %u, storage offset "
              "alignment %llu, queues:",
              properties.deviceName, limits.maxStorageBufferRange,
              static_cast<unsigned long long>(
                  limits.minStorageBufferOffsetAlignment));
  for (const Queue& queue : queues) {
    std::printf(" %s (family %u)", queue.name, queue.family);
  }
  std::printf("\n");

  const auto gdpa = vk.GetDeviceProcAddr;
  const auto start = std::chrono::steady_clock::now();
  std::unique_ptr<Etc2GpuDecoder> decoder = Etc2GpuDecoder::Create(
      device, limits, memory_properties, families.data(),
      static_cast<std::uint32_t>(families.size()), gdpa);
  Result(decoder != nullptr,
         "decoder created in " + std::to_string(Seconds(start) * 1000) +
             " ms");
  VkPhysicalDeviceLimits tiny = limits;
  tiny.maxStorageBufferRange = 4096;
  tiny.minStorageBufferOffsetAlignment = 256;
  tiny.maxComputeWorkGroupCount[0] = 64;
  tiny.maxComputeWorkGroupCount[1] = 2;
  tiny.maxComputeWorkGroupCount[2] = 1;
  std::unique_ptr<Etc2GpuDecoder> stress = Etc2GpuDecoder::Create(
      device, tiny, memory_properties, families.data(),
      static_cast<std::uint32_t>(families.size()), gdpa);
  Result(stress != nullptr, "decoder with tiny limits created");
  if (decoder == nullptr || stress == nullptr) {
    return 1;
  }

  const auto encoder_start = std::chrono::steady_clock::now();
  std::shared_ptr<Etc2GpuEncoder> built = Etc2GpuEncoder::Start(device, gdpa);
  const double started = Seconds(encoder_start);
  std::unique_ptr<Etc2GpuDecoder> encoder = Etc2GpuDecoder::Create(
      device, limits, memory_properties, families.data(),
      static_cast<std::uint32_t>(families.size()), gdpa, built);
  const bool early = encoder != nullptr && encoder->Encodes();
  if (built != nullptr) {
    built->Wait();
  }
  Result(encoder != nullptr && encoder->Encodes(),
         "BC encoder started in " + std::to_string(started * 1000) +
             " ms, built " + std::to_string(Seconds(encoder_start) * 1000) +
             " ms after the start" +
             (early ? "" : ", the decoder took encodes only once it was built"));
  std::unique_ptr<Etc2GpuDecoder> stress_encoder = Etc2GpuDecoder::Create(
      device, tiny, memory_properties, families.data(),
      static_cast<std::uint32_t>(families.size()), gdpa, built);
  Result(stress_encoder != nullptr && stress_encoder->Encodes(),
         "encoder with tiny limits created");
  if (encoder == nullptr || stress_encoder == nullptr) {
    return 1;
  }

  DecoderTests(*decoder, *stress, queues[0], random_blocks);
  CopyTests(*decoder, *stress);
  EmulationTests(gdpa);
  EncoderTests(*encoder, *stress_encoder, *decoder, queues[0], random_blocks);
  Result(built->Checked() && encoder->Encodes(),
         "the GPU encoder passed its check against the CPU encoder before "
         "its first upload");
  if (has_bc) {
    StoredFidelity(*encoder, queues[0]);
    StoredKinds(*encoder, queues[0]);
    StoredEmulationTests(gdpa);
    DoubtedEncoderTests(gdpa);
  } else {
    std::printf("     no BC formats on this device: stored images not "
                "tested\n");
  }
  if (benchmarks) {
    DecoderBenchmark(*decoder, queues[0]);
    EncoderBenchmark(*encoder, queues[0]);
    RenderThreadBenchmarks(gdpa);
  }

  decoder.reset();
  stress.reset();
  encoder.reset();
  stress_encoder.reset();
  built.reset();
  vk.DeviceWaitIdle(device);
  for (Queue& queue : queues) {
    vk.DestroyCommandPool(device, queue.pool, nullptr);
  }
  vk.DestroyDevice(device, nullptr);
  vk.DestroyInstance(instance, nullptr);
  if (failures == 0) {
    std::printf("all GPU decodes and encodes match the CPU\n");
    return 0;
  }
  std::printf("%d FAILED\n", failures);
  return 1;
}
