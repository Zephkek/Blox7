#include "blox7/graphics/vulkan_etc2_gpu_decoder.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "gen/etc2_decode_shader.h"

namespace blox7::graphics {

bool Etc2GpuDecodable(EtcFormat format, std::uint32_t width) {
  return width != 0 && (EtcDecodedTexelBytes(format) == 4 || width % 2 == 0);
}

namespace {

#define BLOX7_ETC2_GPU_FUNCTIONS(X) \
  X(CreateShaderModule)             \
  X(DestroyShaderModule)            \
  X(CreateDescriptorSetLayout)      \
  X(DestroyDescriptorSetLayout)     \
  X(CreatePipelineLayout)           \
  X(DestroyPipelineLayout)          \
  X(CreateComputePipelines)         \
  X(DestroyPipeline)                \
  X(CreateDescriptorPool)           \
  X(DestroyDescriptorPool)          \
  X(ResetDescriptorPool)            \
  X(AllocateDescriptorSets)         \
  X(UpdateDescriptorSets)           \
  X(CreateCommandPool)              \
  X(DestroyCommandPool)             \
  X(ResetCommandPool)               \
  X(AllocateCommandBuffers)         \
  X(BeginCommandBuffer)             \
  X(EndCommandBuffer)               \
  X(CmdCopyBuffer)                  \
  X(CmdPipelineBarrier)             \
  X(CmdBindPipeline)                \
  X(CmdBindDescriptorSets)          \
  X(CmdPushConstants)               \
  X(CmdDispatch)                    \
  X(QueueSubmit)                    \
  X(CreateFence)                    \
  X(DestroyFence)                   \
  X(GetFenceStatus)                 \
  X(WaitForFences)                  \
  X(ResetFences)                    \
  X(CreateBuffer)                   \
  X(DestroyBuffer)                  \
  X(GetBufferMemoryRequirements)    \
  X(AllocateMemory)                 \
  X(FreeMemory)                     \
  X(BindBufferMemory)               \
  X(MapMemory)

struct Functions {
#define BLOX7_ETC2_GPU_FUNCTION(name) PFN_vk##name name = nullptr;
  BLOX7_ETC2_GPU_FUNCTIONS(BLOX7_ETC2_GPU_FUNCTION)
#undef BLOX7_ETC2_GPU_FUNCTION
};

struct Job {
  std::uint32_t format = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t blocks_wide = 0;
  std::uint32_t blocks_high = 0;
  std::uint32_t source_word = 0;
  std::uint32_t source_row_words = 0;
  std::uint32_t source_layer_words = 0;
  std::uint32_t target_word = 0;
  std::uint32_t target_row_words = 0;
  std::uint32_t target_layer_words = 0;
};

static_assert(sizeof(Job) == 44);

struct Band {
  const Etc2GpuUpload* upload = nullptr;
  std::uint32_t layers = 0;
  std::uint32_t first_row = 0;
  std::uint32_t rows = 0;
  VkDeviceSize source_offset = 0;
  VkDeviceSize source_bytes = 0;
  VkDeviceSize target_offset = 0;
  VkDeviceSize target_bytes = 0;
  VkDeviceSize blocks_offset = 0;
};

struct Piece {
  const std::uint8_t* data = nullptr;
  VkDeviceSize size = 0;
  VkBuffer target = VK_NULL_HANDLE;
  VkDeviceSize target_offset = 0;
  VkDeviceSize carry_offset = 0;
};

struct Slot {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  std::uint32_t descriptor_capacity = 0;
  VkBuffer blocks = VK_NULL_HANDLE;
  VkDeviceMemory blocks_memory = VK_NULL_HANDLE;
  VkDeviceSize blocks_capacity = 0;
  VkBuffer carry = VK_NULL_HANDLE;
  VkDeviceMemory carry_memory = VK_NULL_HANDLE;
  std::uint8_t* carry_data = nullptr;
  VkDeviceSize carry_capacity = 0;
  bool pending = false;
  std::uint64_t serial = 0;
};

constexpr std::size_t kMaxSlots = 8;
constexpr VkDeviceSize kPassBytes = VkDeviceSize{16} << 20;
constexpr VkDeviceSize kMinBufferBytes = VkDeviceSize{64} << 10;
constexpr VkDeviceSize kMinDescriptorSets = 16;
constexpr VkDeviceSize kCarryAlignment = 16;
constexpr std::uint32_t kGroupSize = 8;

std::uint32_t ShaderFormat(EtcFormat format) {
  switch (format) {
    case EtcFormat::kEtc2Rgb8:
      return 0;
    case EtcFormat::kEtc2Rgb8A1:
      return 1;
    case EtcFormat::kEtc2Rgba8:
      return 2;
    case EtcFormat::kEacR11:
      return 3;
    case EtcFormat::kEacR11Signed:
      return 4;
    case EtcFormat::kEacRg11:
      return 5;
    case EtcFormat::kEacRg11Signed:
      return 6;
  }
  return 0;
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

VkDeviceSize PowerOfTwoAtLeast(VkDeviceSize value) {
  VkDeviceSize result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

void Barrier(const Functions& vk, VkCommandBuffer command,
             VkPipelineStageFlags source_stage, VkAccessFlags source_access,
             VkPipelineStageFlags target_stage, VkAccessFlags target_access) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = target_access;
  vk.CmdPipelineBarrier(command, source_stage, target_stage, 0, 1, &barrier, 0,
                        nullptr, 0, nullptr);
}

}  // namespace

struct Etc2GpuDecoder::State {
  ~State();

  bool CreatePipeline();
  bool Plan(const Etc2GpuUpload& upload, std::vector<Band>* out) const;
  Slot* Acquire(std::uint32_t family);
  bool CreateSlot(std::uint32_t family, Slot* slot);
  void DestroySlot(Slot* slot);
  bool AllocateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host,
                      VkBuffer* buffer, VkDeviceMemory* allocation,
                      std::uint8_t** mapped);
  bool ReserveBlocks(Slot* slot, VkDeviceSize bytes, std::size_t set_count);
  bool ReserveCarry(Slot* slot, VkDeviceSize bytes);
  bool Begin(Slot* slot);
  bool Finish(VkQueue queue, Slot* slot);
  bool SubmitDecodes(VkQueue queue, Slot* slot, const Band* first,
                     std::size_t count);
  bool SubmitCopies(VkQueue queue, Slot* slot, const Piece* first,
                    std::size_t count, bool after_compute);
  void Drain(const std::vector<Slot*>& submitted);

  VkDevice device = VK_NULL_HANDLE;
  Functions vk;
  VkPhysicalDeviceMemoryProperties memory{};
  VkDeviceSize max_range = 0;
  VkDeviceSize alignment = 1;
  VkDeviceSize blocks_alignment = 16;
  VkDeviceSize target_slack = 0;
  VkDeviceSize pass_bytes = 0;
  std::uint32_t max_groups[3] = {0, 0, 0};
  std::vector<bool> compute;
  std::vector<bool> transfer;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::mutex mutex;
  std::vector<std::vector<std::unique_ptr<Slot>>> slots;
  std::uint64_t serial = 0;
  std::vector<Band> bands;
  std::vector<Piece> pieces;
  std::vector<VkBufferCopy> regions;
  std::vector<VkDescriptorSetLayout> layouts;
  std::vector<VkDescriptorSet> sets;
  std::vector<VkDescriptorBufferInfo> buffers;
  std::vector<VkWriteDescriptorSet> writes;
};

Etc2GpuDecoder::State::~State() {
  for (auto& family : slots) {
    for (auto& slot : family) {
      DestroySlot(slot.get());
    }
  }
  if (pipeline != VK_NULL_HANDLE) {
    vk.DestroyPipeline(device, pipeline, nullptr);
  }
  if (pipeline_layout != VK_NULL_HANDLE) {
    vk.DestroyPipelineLayout(device, pipeline_layout, nullptr);
  }
  if (set_layout != VK_NULL_HANDLE) {
    vk.DestroyDescriptorSetLayout(device, set_layout, nullptr);
  }
}

bool Etc2GpuDecoder::State::CreatePipeline() {
  VkDescriptorSetLayoutBinding bindings[2] = {};
  for (std::uint32_t index = 0; index < 2; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.bindingCount = 2;
  set_info.pBindings = bindings;
  VkDescriptorSetLayout created_set = VK_NULL_HANDLE;
  if (vk.CreateDescriptorSetLayout(device, &set_info, nullptr, &created_set) !=
      VK_SUCCESS) {
    return false;
  }
  set_layout = created_set;

  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Job)};
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  VkPipelineLayout created_layout = VK_NULL_HANDLE;
  if (vk.CreatePipelineLayout(device, &layout_info, nullptr, &created_layout) !=
      VK_SUCCESS) {
    return false;
  }
  pipeline_layout = created_layout;

  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = sizeof(kEtc2DecodeShader);
  module_info.pCode = kEtc2DecodeShader;
  VkShaderModule module = VK_NULL_HANDLE;
  if (vk.CreateShaderModule(device, &module_info, nullptr, &module) !=
      VK_SUCCESS) {
    return false;
  }
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = pipeline_layout;
  VkPipeline created = VK_NULL_HANDLE;
  const VkResult result = vk.CreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &created);
  vk.DestroyShaderModule(device, module, nullptr);
  if (result != VK_SUCCESS) {
    return false;
  }
  pipeline = created;
  return true;
}

bool Etc2GpuDecoder::State::Plan(const Etc2GpuUpload& upload,
                                 std::vector<Band>* out) const {
  if (upload.source == VK_NULL_HANDLE || upload.target == VK_NULL_HANDLE ||
      upload.height == 0 || upload.layers == 0 ||
      !Etc2GpuDecodable(upload.format, upload.width) ||
      upload.target_offset % 4 != 0) {
    return false;
  }
  const VkDeviceSize block_bytes = EtcBlockBytes(upload.format);
  const VkDeviceSize blocks_wide = (VkDeviceSize{upload.width} + 3) / 4;
  const VkDeviceSize block_rows = (VkDeviceSize{upload.height} + 3) / 4;
  const VkDeviceSize packed_row = blocks_wide * block_bytes;
  const VkDeviceSize row_stride = upload.source_row_stride;
  const VkDeviceSize layer_stride = upload.source_layer_stride;
  const VkDeviceSize layer_span = (block_rows - 1) * row_stride + packed_row;
  if (row_stride < packed_row || row_stride % 4 != 0 ||
      row_stride / 4 > UINT32_MAX ||
      (upload.layers > 1 &&
       (layer_stride < layer_span || layer_stride % 4 != 0 ||
        layer_stride / 4 > UINT32_MAX)) ||
      blocks_wide > VkDeviceSize{max_groups[0]} * kGroupSize) {
    return false;
  }
  const VkDeviceSize texel_bytes = EtcDecodedTexelBytes(upload.format);
  const VkDeviceSize target_row = VkDeviceSize{upload.width} * texel_bytes;
  const VkDeviceSize target_layer = target_row * upload.height;
  const VkDeviceSize source_budget = std::min(max_range, pass_bytes);
  const VkDeviceSize target_budget = max_range - target_slack;
  const VkDeviceSize max_rows = VkDeviceSize{max_groups[1]} * kGroupSize;
  const VkDeviceSize span = (upload.layers - 1) * layer_stride + layer_span;
  if (upload.layers <= max_groups[2] && block_rows <= max_rows &&
      span <= source_budget && target_layer * upload.layers <= target_budget) {
    if (out != nullptr) {
      Band band;
      band.upload = &upload;
      band.layers = upload.layers;
      band.rows = static_cast<std::uint32_t>(block_rows);
      band.source_offset = upload.source_offset;
      band.source_bytes = span;
      band.target_offset = upload.target_offset;
      band.target_bytes = target_layer * upload.layers;
      out->push_back(band);
    }
    return true;
  }
  if (packed_row > source_budget || target_row * 4 > target_budget) {
    return false;
  }
  if (out == nullptr) {
    return true;
  }
  const VkDeviceSize rows =
      std::min({block_rows, max_rows,
                (source_budget - packed_row) / row_stride + 1,
                target_budget / (target_row * 4)});
  for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
    for (VkDeviceSize row = 0; row < block_rows; row += rows) {
      const VkDeviceSize count = std::min(rows, block_rows - row);
      Band band;
      band.upload = &upload;
      band.layers = 1;
      band.first_row = static_cast<std::uint32_t>(row);
      band.rows = static_cast<std::uint32_t>(count);
      band.source_offset =
          upload.source_offset + layer * layer_stride + row * row_stride;
      band.source_bytes = (count - 1) * row_stride + packed_row;
      band.target_offset =
          upload.target_offset + layer * target_layer + row * 4 * target_row;
      band.target_bytes =
          std::min(VkDeviceSize{upload.height} - row * 4, count * 4) *
          target_row;
      out->push_back(band);
    }
  }
  return true;
}

bool Etc2GpuDecoder::State::CreateSlot(std::uint32_t family, Slot* slot) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = family;
  VkCommandPool pool = VK_NULL_HANDLE;
  if (vk.CreateCommandPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    return false;
  }
  slot->pool = pool;
  VkCommandBufferAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = slot->pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  if (vk.AllocateCommandBuffers(device, &allocate_info, &command) !=
      VK_SUCCESS) {
    return false;
  }
  slot->command = command;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  if (vk.CreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    return false;
  }
  slot->fence = fence;
  return true;
}

void Etc2GpuDecoder::State::DestroySlot(Slot* slot) {
  if (slot->pending) {
    vk.WaitForFences(device, 1, &slot->fence, VK_TRUE, UINT64_MAX);
  }
  if (slot->blocks != VK_NULL_HANDLE) {
    vk.DestroyBuffer(device, slot->blocks, nullptr);
  }
  if (slot->blocks_memory != VK_NULL_HANDLE) {
    vk.FreeMemory(device, slot->blocks_memory, nullptr);
  }
  if (slot->carry != VK_NULL_HANDLE) {
    vk.DestroyBuffer(device, slot->carry, nullptr);
  }
  if (slot->carry_memory != VK_NULL_HANDLE) {
    vk.FreeMemory(device, slot->carry_memory, nullptr);
  }
  if (slot->descriptors != VK_NULL_HANDLE) {
    vk.DestroyDescriptorPool(device, slot->descriptors, nullptr);
  }
  if (slot->fence != VK_NULL_HANDLE) {
    vk.DestroyFence(device, slot->fence, nullptr);
  }
  if (slot->pool != VK_NULL_HANDLE) {
    vk.DestroyCommandPool(device, slot->pool, nullptr);
  }
  *slot = Slot{};
}

Slot* Etc2GpuDecoder::State::Acquire(std::uint32_t family) {
  std::vector<std::unique_ptr<Slot>>& pool = slots[family];
  for (const auto& slot : pool) {
    if (!slot->pending) {
      return slot.get();
    }
  }
  Slot* oldest = nullptr;
  for (const auto& slot : pool) {
    const VkResult status = vk.GetFenceStatus(device, slot->fence);
    if (status == VK_SUCCESS) {
      slot->pending = false;
      return slot.get();
    }
    if (status != VK_NOT_READY) {
      return nullptr;
    }
    if (oldest == nullptr || slot->serial < oldest->serial) {
      oldest = slot.get();
    }
  }
  if (pool.size() < kMaxSlots) {
    auto slot = std::make_unique<Slot>();
    if (!CreateSlot(family, slot.get())) {
      DestroySlot(slot.get());
      return nullptr;
    }
    pool.push_back(std::move(slot));
    return pool.back().get();
  }
  if (oldest == nullptr || vk.WaitForFences(device, 1, &oldest->fence, VK_TRUE,
                                            UINT64_MAX) != VK_SUCCESS) {
    return nullptr;
  }
  oldest->pending = false;
  return oldest;
}

bool Etc2GpuDecoder::State::AllocateBuffer(VkDeviceSize bytes,
                                           VkBufferUsageFlags usage, bool host,
                                           VkBuffer* buffer,
                                           VkDeviceMemory* allocation,
                                           std::uint8_t** mapped) {
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = bytes;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer created = VK_NULL_HANDLE;
  if (vk.CreateBuffer(device, &buffer_info, nullptr, &created) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vk.GetBufferMemoryRequirements(device, created, &requirements);
  constexpr VkMemoryPropertyFlags kAvoided =
      VK_MEMORY_PROPERTY_PROTECTED_BIT |
      VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
      VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
      VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD |
      VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV;
  VkDeviceMemory memory_object = VK_NULL_HANDLE;
  for (int rank = 0; rank < 3 && memory_object == VK_NULL_HANDLE; ++rank) {
    for (std::uint32_t index = 0;
         index < memory.memoryTypeCount && memory_object == VK_NULL_HANDLE;
         ++index) {
      const VkMemoryPropertyFlags flags =
          memory.memoryTypes[index].propertyFlags;
      const bool local = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
      const bool visible =
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
          (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      const bool wanted =
          host ? visible && (rank == 0 ? !local : rank == 1 && local)
               : (rank == 0 && local && !visible) ||
                     (rank == 1 && local && visible) || (rank == 2 && !local);
      if ((requirements.memoryTypeBits & (1U << index)) == 0 ||
          (flags & kAvoided) != 0 || !wanted) {
        continue;
      }
      VkMemoryAllocateInfo allocate_info{};
      allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocate_info.allocationSize = requirements.size;
      allocate_info.memoryTypeIndex = index;
      VkDeviceMemory candidate = VK_NULL_HANDLE;
      if (vk.AllocateMemory(device, &allocate_info, nullptr, &candidate) ==
          VK_SUCCESS) {
        memory_object = candidate;
      }
    }
  }
  void* data = nullptr;
  if (memory_object == VK_NULL_HANDLE ||
      vk.BindBufferMemory(device, created, memory_object, 0) != VK_SUCCESS ||
      (host && vk.MapMemory(device, memory_object, 0, VK_WHOLE_SIZE, 0,
                            &data) != VK_SUCCESS)) {
    vk.DestroyBuffer(device, created, nullptr);
    if (memory_object != VK_NULL_HANDLE) {
      vk.FreeMemory(device, memory_object, nullptr);
    }
    return false;
  }
  *buffer = created;
  *allocation = memory_object;
  if (mapped != nullptr) {
    *mapped = static_cast<std::uint8_t*>(data);
  }
  return true;
}

bool Etc2GpuDecoder::State::ReserveBlocks(Slot* slot, VkDeviceSize bytes,
                                          std::size_t set_count) {
  if (slot->blocks_capacity < bytes) {
    if (slot->blocks != VK_NULL_HANDLE) {
      vk.DestroyBuffer(device, slot->blocks, nullptr);
      vk.FreeMemory(device, slot->blocks_memory, nullptr);
    }
    slot->blocks = VK_NULL_HANDLE;
    slot->blocks_memory = VK_NULL_HANDLE;
    slot->blocks_capacity = 0;
    const VkDeviceSize capacity =
        PowerOfTwoAtLeast(std::max(bytes, kMinBufferBytes));
    if (!AllocateBuffer(capacity,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        false, &slot->blocks, &slot->blocks_memory, nullptr)) {
      return false;
    }
    slot->blocks_capacity = capacity;
  }
  if (slot->descriptor_capacity < set_count) {
    if (slot->descriptors != VK_NULL_HANDLE) {
      vk.DestroyDescriptorPool(device, slot->descriptors, nullptr);
    }
    slot->descriptors = VK_NULL_HANDLE;
    slot->descriptor_capacity = 0;
    const auto capacity = static_cast<std::uint32_t>(PowerOfTwoAtLeast(
        std::max<VkDeviceSize>(set_count, kMinDescriptorSets)));
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    capacity * 2};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = capacity;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vk.CreateDescriptorPool(device, &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
      return false;
    }
    slot->descriptors = pool;
    slot->descriptor_capacity = capacity;
  } else if (vk.ResetDescriptorPool(device, slot->descriptors, 0) !=
             VK_SUCCESS) {
    return false;
  }
  layouts.assign(set_count, set_layout);
  sets.assign(set_count, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = slot->descriptors;
  allocate_info.descriptorSetCount = static_cast<std::uint32_t>(set_count);
  allocate_info.pSetLayouts = layouts.data();
  return vk.AllocateDescriptorSets(device, &allocate_info, sets.data()) ==
         VK_SUCCESS;
}

bool Etc2GpuDecoder::State::ReserveCarry(Slot* slot, VkDeviceSize bytes) {
  if (slot->carry_capacity >= bytes) {
    return true;
  }
  if (slot->carry != VK_NULL_HANDLE) {
    vk.DestroyBuffer(device, slot->carry, nullptr);
    vk.FreeMemory(device, slot->carry_memory, nullptr);
  }
  slot->carry = VK_NULL_HANDLE;
  slot->carry_memory = VK_NULL_HANDLE;
  slot->carry_data = nullptr;
  slot->carry_capacity = 0;
  const VkDeviceSize capacity =
      PowerOfTwoAtLeast(std::max(bytes, kMinBufferBytes));
  if (!AllocateBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                      &slot->carry, &slot->carry_memory, &slot->carry_data)) {
    return false;
  }
  slot->carry_capacity = capacity;
  return true;
}

bool Etc2GpuDecoder::State::Begin(Slot* slot) {
  if (vk.ResetCommandPool(device, slot->pool, 0) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vk.BeginCommandBuffer(slot->command, &begin_info) == VK_SUCCESS;
}

bool Etc2GpuDecoder::State::Finish(VkQueue queue, Slot* slot) {
  if (vk.EndCommandBuffer(slot->command) != VK_SUCCESS ||
      vk.ResetFences(device, 1, &slot->fence) != VK_SUCCESS) {
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot->command;
  if (vk.QueueSubmit(queue, 1, &submit, slot->fence) != VK_SUCCESS) {
    return false;
  }
  slot->pending = true;
  slot->serial = ++serial;
  return true;
}

bool Etc2GpuDecoder::State::SubmitDecodes(VkQueue queue, Slot* slot,
                                          const Band* first,
                                          std::size_t count) {
  buffers.resize(count * 2);
  writes.resize(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    const Band& band = first[index];
    const VkDeviceSize window =
        band.target_offset - band.target_offset % alignment;
    buffers[index * 2] = {slot->blocks, band.blocks_offset, band.source_bytes};
    buffers[index * 2 + 1] = {band.upload->target, window,
                              band.target_offset + band.target_bytes - window};
    for (std::uint32_t binding = 0; binding < 2; ++binding) {
      VkWriteDescriptorSet& write = writes[index * 2 + binding];
      write = VkWriteDescriptorSet{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = sets[index];
      write.dstBinding = binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &buffers[index * 2 + binding];
    }
  }
  vk.UpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                          writes.data(), 0, nullptr);
  if (!Begin(slot)) {
    return false;
  }
  for (std::size_t index = 0; index < count;) {
    const VkBuffer source = first[index].upload->source;
    regions.clear();
    for (; index < count && first[index].upload->source == source; ++index) {
      regions.push_back({first[index].source_offset,
                         first[index].blocks_offset,
                         first[index].source_bytes});
    }
    vk.CmdCopyBuffer(slot->command, source, slot->blocks,
                     static_cast<std::uint32_t>(regions.size()),
                     regions.data());
  }
  Barrier(vk, slot->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  vk.CmdBindPipeline(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  for (std::size_t index = 0; index < count; ++index) {
    const Band& band = first[index];
    const Etc2GpuUpload& upload = *band.upload;
    const VkDeviceSize target_row =
        VkDeviceSize{upload.width} * EtcDecodedTexelBytes(upload.format);
    Job job;
    job.format = ShaderFormat(upload.format);
    job.width = upload.width;
    job.height = static_cast<std::uint32_t>(
        std::min(VkDeviceSize{upload.height} - VkDeviceSize{band.first_row} * 4,
                 VkDeviceSize{band.rows} * 4));
    job.blocks_wide = (upload.width + 3) / 4;
    job.blocks_high = band.rows;
    job.source_row_words =
        static_cast<std::uint32_t>(upload.source_row_stride / 4);
    job.target_word = static_cast<std::uint32_t>(
        (band.target_offset - buffers[index * 2 + 1].offset) / 4);
    job.target_row_words = static_cast<std::uint32_t>(target_row / 4);
    if (band.layers > 1) {
      job.source_layer_words =
          static_cast<std::uint32_t>(upload.source_layer_stride / 4);
      job.target_layer_words =
          static_cast<std::uint32_t>(target_row * upload.height / 4);
    }
    vk.CmdBindDescriptorSets(slot->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipeline_layout, 0, 1, &sets[index], 0, nullptr);
    vk.CmdPushConstants(slot->command, pipeline_layout,
                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job), &job);
    vk.CmdDispatch(slot->command,
                   (job.blocks_wide + kGroupSize - 1) / kGroupSize,
                   (band.rows + kGroupSize - 1) / kGroupSize, band.layers);
  }
  Barrier(vk, slot->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  return Finish(queue, slot);
}

bool Etc2GpuDecoder::State::SubmitCopies(VkQueue queue, Slot* slot,
                                         const Piece* first, std::size_t count,
                                         bool after_compute) {
  for (std::size_t index = 0; index < count; ++index) {
    std::memcpy(slot->carry_data + first[index].carry_offset,
                first[index].data, first[index].size);
  }
  if (!Begin(slot)) {
    return false;
  }
  Barrier(vk, slot->command,
          after_compute ? VK_PIPELINE_STAGE_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                        : VK_PIPELINE_STAGE_TRANSFER_BIT,
          after_compute
              ? VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT
              : VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  for (std::size_t index = 0; index < count;) {
    const VkBuffer target = first[index].target;
    regions.clear();
    for (; index < count && first[index].target == target; ++index) {
      regions.push_back({first[index].carry_offset, first[index].target_offset,
                         first[index].size});
    }
    vk.CmdCopyBuffer(slot->command, slot->carry, target,
                     static_cast<std::uint32_t>(regions.size()),
                     regions.data());
  }
  Barrier(vk, slot->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  return Finish(queue, slot);
}

void Etc2GpuDecoder::State::Drain(const std::vector<Slot*>& submitted) {
  for (Slot* slot : submitted) {
    if (slot->pending && vk.WaitForFences(device, 1, &slot->fence, VK_TRUE,
                                          UINT64_MAX) == VK_SUCCESS) {
      slot->pending = false;
    }
  }
}

std::unique_ptr<Etc2GpuDecoder> Etc2GpuDecoder::Create(
    VkDevice device, const VkPhysicalDeviceLimits& limits,
    const VkPhysicalDeviceMemoryProperties& memory,
    const VkQueueFamilyProperties* families, std::uint32_t family_count,
    PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  const VkDeviceSize alignment =
      std::max<VkDeviceSize>(limits.minStorageBufferOffsetAlignment, 1);
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr ||
      families == nullptr || family_count == 0 ||
      (alignment & (alignment - 1)) != 0 || alignment > 256 ||
      limits.maxStorageBufferRange < 1024 ||
      limits.maxComputeWorkGroupCount[0] == 0 ||
      limits.maxComputeWorkGroupCount[1] == 0 ||
      limits.maxComputeWorkGroupCount[2] == 0) {
    return nullptr;
  }
  auto state = std::make_unique<State>();
  state->device = device;
  bool loaded = true;
#define BLOX7_ETC2_GPU_LOAD(name)                  \
  state->vk.name = reinterpret_cast<PFN_vk##name>( \
      get_device_proc_addr(device, "vk" #name));   \
  loaded = loaded && state->vk.name != nullptr;
  BLOX7_ETC2_GPU_FUNCTIONS(BLOX7_ETC2_GPU_LOAD)
#undef BLOX7_ETC2_GPU_LOAD
  if (!loaded) {
    return nullptr;
  }
  state->memory = memory;
  state->max_range = limits.maxStorageBufferRange;
  state->alignment = alignment;
  state->blocks_alignment = std::max<VkDeviceSize>(alignment, 16);
  state->target_slack = std::max<VkDeviceSize>(alignment, 4) - 4;
  state->pass_bytes = std::min(kPassBytes, state->max_range);
  for (int axis = 0; axis < 3; ++axis) {
    state->max_groups[axis] = limits.maxComputeWorkGroupCount[axis];
  }
  state->compute.resize(family_count);
  state->transfer.resize(family_count);
  bool any = false;
  for (std::uint32_t index = 0; index < family_count; ++index) {
    const VkQueueFlags flags = families[index].queueFlags;
    const bool present = families[index].queueCount != 0;
    state->compute[index] = present && (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    state->transfer[index] =
        present && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                             VK_QUEUE_TRANSFER_BIT)) != 0;
    any = any || state->compute[index];
  }
  state->slots.resize(family_count);
  if (!any || !state->CreatePipeline()) {
    return nullptr;
  }
  return std::unique_ptr<Etc2GpuDecoder>(new Etc2GpuDecoder(state.release()));
}

Etc2GpuDecoder::Etc2GpuDecoder(State* state) : state_(state) {}

Etc2GpuDecoder::~Etc2GpuDecoder() { delete state_; }

bool Etc2GpuDecoder::DecodesOn(std::uint32_t family) const {
  return family < state_->compute.size() && state_->compute[family];
}

bool Etc2GpuDecoder::CopiesOn(std::uint32_t family) const {
  return family < state_->transfer.size() && state_->transfer[family];
}

bool Etc2GpuDecoder::Accepts(const Etc2GpuUpload& upload) const {
  return state_->Plan(upload, nullptr);
}

bool Etc2GpuDecoder::Decode(VkQueue queue, std::uint32_t family,
                            const Etc2GpuUpload* uploads, std::size_t count) {
  if (count == 0) {
    return true;
  }
  if (queue == VK_NULL_HANDLE || uploads == nullptr || !DecodesOn(family)) {
    return false;
  }
  State& state = *state_;
  std::lock_guard<std::mutex> lock(state.mutex);
  state.bands.clear();
  for (std::size_t index = 0; index < count; ++index) {
    if (!state.Plan(uploads[index], &state.bands)) {
      return false;
    }
  }
  std::vector<Slot*> submitted;
  for (std::size_t first = 0; first < state.bands.size();) {
    VkDeviceSize used = 0;
    std::size_t last = first;
    for (; last < state.bands.size(); ++last) {
      Band& band = state.bands[last];
      const VkDeviceSize offset = AlignUp(used, state.blocks_alignment);
      if (last > first && offset + band.source_bytes > state.pass_bytes) {
        break;
      }
      band.blocks_offset = offset;
      used = offset + band.source_bytes;
    }
    Slot* slot = state.Acquire(family);
    if (slot == nullptr || !state.ReserveBlocks(slot, used, last - first) ||
        !state.SubmitDecodes(queue, slot, &state.bands[first], last - first)) {
      state.Drain(submitted);
      return false;
    }
    submitted.push_back(slot);
    first = last;
  }
  return true;
}

bool Etc2GpuDecoder::Copy(VkQueue queue, std::uint32_t family,
                          const Etc2GpuCopy* copies, std::size_t count) {
  if (count == 0) {
    return true;
  }
  if (queue == VK_NULL_HANDLE || copies == nullptr || !CopiesOn(family)) {
    return false;
  }
  State& state = *state_;
  std::lock_guard<std::mutex> lock(state.mutex);
  state.pieces.clear();
  for (std::size_t index = 0; index < count; ++index) {
    const Etc2GpuCopy& copy = copies[index];
    if (copy.data == nullptr || copy.size == 0 ||
        copy.target == VK_NULL_HANDLE) {
      return false;
    }
    for (VkDeviceSize offset = 0; offset < copy.size;
         offset += state.pass_bytes) {
      Piece piece;
      piece.data = copy.data + offset;
      piece.size = std::min(state.pass_bytes, copy.size - offset);
      piece.target = copy.target;
      piece.target_offset = copy.target_offset + offset;
      state.pieces.push_back(piece);
    }
  }
  std::vector<Slot*> submitted;
  for (std::size_t first = 0; first < state.pieces.size();) {
    VkDeviceSize used = 0;
    std::size_t last = first;
    for (; last < state.pieces.size(); ++last) {
      Piece& piece = state.pieces[last];
      const VkDeviceSize offset = AlignUp(used, kCarryAlignment);
      if (last > first && offset + piece.size > state.pass_bytes) {
        break;
      }
      piece.carry_offset = offset;
      used = offset + piece.size;
    }
    Slot* slot = state.Acquire(family);
    if (slot == nullptr || !state.ReserveCarry(slot, used) ||
        !state.SubmitCopies(queue, slot, &state.pieces[first], last - first,
                            DecodesOn(family))) {
      state.Drain(submitted);
      return false;
    }
    submitted.push_back(slot);
    first = last;
  }
  return true;
}

}  // namespace blox7::graphics
