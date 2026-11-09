#include "blox7/graphics/vulkan_etc2_emulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "blox7/graphics/texture_override.h"

namespace blox7::graphics {
namespace {

template <typename T>
T Handle(std::uintptr_t value) {
  if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<T>(value);
  else
    return static_cast<T>(value);
}

class VulkanEtc2Test : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    active = this;
    unsetenv("BLOX7_TEXTURE_OVERRIDE_DIR");
    unsetenv("BLOX7_TEXTURE_DUMP_DIR");
    memory_properties.memoryTypeCount = GetParam() ? 2 : 1;
    memory_properties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory_properties.memoryTypes[1].propertyFlags =
        memory_properties.memoryTypes[0].propertyFlags |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    Start(true, "1");
  }

  void TearDown() override {
    emulation->DestroyDevice(device);
    emulation.reset();
    active = nullptr;
  }

  void Start(bool emulated, const char* upscale) {
    if (emulation) emulation->DestroyDevice(device);
    if (upscale != nullptr) {
      setenv("BLOX7_SMALL_TEXTURE_UPSCALE", upscale, 1);
    } else {
      unsetenv("BLOX7_SMALL_TEXTURE_UPSCALE");
    }
    emulation = std::make_unique<VulkanEtc2Emulation>();
    emulation->RegisterDevice(device, Handle<VkPhysicalDevice>(2), emulated,
                              memory_properties, GetProc);
  }

  struct Source {
    VkBuffer buffer;
    VkDeviceMemory memory;
    std::uint8_t* data;
  };

  Source CreateSource(std::size_t bytes, bool mapped = true) {
    const VkBuffer buffer = Handle<VkBuffer>(next++);
    const VkDeviceMemory memory = Handle<VkDeviceMemory>(next++);
    sizes[buffer] = bytes;
    memory_bytes[memory].resize(bytes);
    EXPECT_EQ(emulation->BindBufferMemory(device, buffer, memory, 0),
              VK_SUCCESS);
    void* data = memory_bytes[memory].data();
    if (mapped) {
      EXPECT_EQ(emulation->MapMemory(device, memory, 0, bytes, 0, &data),
                VK_SUCCESS);
    }
    return {buffer, memory, static_cast<std::uint8_t*>(data)};
  }

  VkImage CreateImage(std::uint32_t width, std::uint32_t height,
                      std::uint32_t layers = 1, std::uint32_t mips = 1) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    info.extent = {width, height, 1};
    info.arrayLayers = layers;
    info.mipLevels = mips;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage image = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImage(device, &info, nullptr, &image),
              VK_SUCCESS);
    return image;
  }

  void Record(VkCommandBuffer command, const Source& source, VkImage image,
              std::uint32_t width, std::uint32_t height,
              std::uint32_t layers = 1, VkDeviceSize offset = 0,
              std::uint32_t row_length = 0, std::uint32_t image_height = 0) {
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.bufferRowLength = row_length;
    region.bufferImageHeight = image_height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent = {width, height, 1};
    emulation->CmdCopyBufferToImage(device, command, source.buffer, image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);
  }

  const std::vector<std::uint8_t>& Output(VkCommandBuffer command) const {
    return memory_bytes.at(bindings.at(recorded.at(command)).first);
  }

  static std::vector<std::uint8_t> Decode(const std::uint8_t* source,
                                          std::uint32_t width,
                                          std::uint32_t height) {
    std::vector<std::uint8_t> result(width * height * 4);
    EXPECT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgba8, source,
                               ((width + 3) / 4) * ((height + 3) / 4) * 16,
                               width, height, result.data(), result.size()));
    return result;
  }

  static VkResult VKAPI_CALL HostCreateImage(VkDevice,
                                             const VkImageCreateInfo* info,
                                             const VkAllocationCallbacks*,
                                             VkImage* image) {
    active->last_image = *info;
    *image = Handle<VkImage>(active->next++);
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostDestroyImage(VkDevice, VkImage,
                                          const VkAllocationCallbacks*) {}

  static VkResult VKAPI_CALL HostCreateBuffer(VkDevice,
                                              const VkBufferCreateInfo* info,
                                              const VkAllocationCallbacks*,
                                              VkBuffer* buffer) {
    *buffer = Handle<VkBuffer>(active->next++);
    active->sizes[*buffer] = info->size;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostRequirements(VkDevice, VkBuffer buffer,
                                          VkMemoryRequirements* requirements) {
    *requirements = {active->sizes.at(buffer), 16,
                     active->GetParam() ? 3U : 1U};
  }

  static VkResult VKAPI_CALL HostAllocate(VkDevice,
                                          const VkMemoryAllocateInfo* info,
                                          const VkAllocationCallbacks*,
                                          VkDeviceMemory* memory) {
    *memory = Handle<VkDeviceMemory>(active->next++);
    active->memory_bytes[*memory].resize(info->allocationSize, 0xa5);
    active->last_memory_type = info->memoryTypeIndex;
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostBind(VkDevice, VkBuffer buffer,
                                      VkDeviceMemory memory,
                                      VkDeviceSize offset) {
    active->bindings[buffer] = {memory, offset};
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostMap(VkDevice, VkDeviceMemory memory,
                                     VkDeviceSize offset, VkDeviceSize,
                                     VkMemoryMapFlags, void** data) {
    ++active->map_calls;
    *data = active->memory_bytes.at(memory).data() + offset;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostUnmap(VkDevice, VkDeviceMemory) {
    ++active->unmap_calls;
  }

  static void VKAPI_CALL HostFree(VkDevice, VkDeviceMemory memory,
                                  const VkAllocationCallbacks*) {
    active->memory_bytes.erase(memory);
  }

  static void VKAPI_CALL HostDestroyBuffer(VkDevice, VkBuffer buffer,
                                           const VkAllocationCallbacks*) {
    active->sizes.erase(buffer);
    active->bindings.erase(buffer);
  }

  static void VKAPI_CALL HostCopy(VkCommandBuffer command, VkBuffer buffer,
                                  VkImage, VkImageLayout, std::uint32_t count,
                                  const VkBufferImageCopy* regions) {
    active->recorded[command] = buffer;
    active->upload_regions[command] = {regions, regions + count};
  }

  static void VKAPI_CALL HostCopyImage(
      VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
      std::uint32_t count, const VkImageCopy* regions) {
    active->image_copies.assign(regions, regions + count);
  }

  static void VKAPI_CALL HostBlitImage(
      VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
      std::uint32_t, const VkImageBlit*, VkFilter) {
    ++active->blit_calls;
  }

  static void VKAPI_CALL HostExecute(VkCommandBuffer, std::uint32_t,
                                     const VkCommandBuffer*) {}

  static PFN_vkVoidFunction VKAPI_CALL GetProc(VkDevice, const char* name) {
    const std::pair<const char*, PFN_vkVoidFunction> functions[] = {
        {"vkCreateImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostCreateImage)},
        {"vkDestroyImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostDestroyImage)},
        {"vkCreateBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(HostCreateBuffer)},
        {"vkGetBufferMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(HostRequirements)},
        {"vkAllocateMemory",
         reinterpret_cast<PFN_vkVoidFunction>(HostAllocate)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(HostBind)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(HostMap)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(HostUnmap)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(HostFree)},
        {"vkDestroyBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(HostDestroyBuffer)},
        {"vkCmdCopyBufferToImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostCopy)},
        {"vkCmdCopyImage", reinterpret_cast<PFN_vkVoidFunction>(HostCopyImage)},
        {"vkCmdBlitImage", reinterpret_cast<PFN_vkVoidFunction>(HostBlitImage)},
        {"vkCmdExecuteCommands",
         reinterpret_cast<PFN_vkVoidFunction>(HostExecute)},
    };
    for (const auto& entry : functions) {
      if (std::strcmp(name, entry.first) == 0) return entry.second;
    }
    return nullptr;
  }

  inline static VulkanEtc2Test* active = nullptr;
  const VkDevice device = Handle<VkDevice>(1);
  std::uintptr_t next = 100;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkImageCreateInfo last_image{};
  std::uint32_t last_memory_type = UINT32_MAX;
  std::atomic<unsigned> map_calls{0};
  std::atomic<unsigned> unmap_calls{0};
  std::unordered_map<VkBuffer, VkDeviceSize> sizes;
  std::unordered_map<VkBuffer, std::pair<VkDeviceMemory, VkDeviceSize>>
      bindings;
  std::unordered_map<VkDeviceMemory, std::vector<std::uint8_t>> memory_bytes;
  std::unordered_map<VkCommandBuffer, VkBuffer> recorded;
  std::unordered_map<VkCommandBuffer, std::vector<VkBufferImageCopy>>
      upload_regions;
  std::vector<VkImageCopy> image_copies;
  unsigned blit_calls = 0;
  std::unique_ptr<VulkanEtc2Emulation> emulation;
};

TEST_P(VulkanEtc2Test, UsesCurrentSourceOnEverySubmission) {
  auto source = CreateSource(256 * 256);
  const auto image = CreateImage(256, 256);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 256, 256);
  EXPECT_EQ(last_memory_type, GetParam() ? 1U : 0U);
  for (std::uint8_t value : {0x12, 0x3a}) {
    std::memset(source.data, value, 256 * 256);
    const auto expected = Decode(source.data, 256, 256);
    emulation->PrepareSubmit(VK_NULL_HANDLE, &command, 1);
    EXPECT_EQ(Output(command), expected);
  }
  emulation->ReleaseCommandBuffer(command);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

TEST_P(VulkanEtc2Test, DecodesPaddedRowsAndArrayLayers) {
  auto source = CreateSource(16 + 288 * 3 * 2);
  for (unsigned i = 0; i < 16 + 288 * 3 * 2; ++i) source.data[i] = i * 29;
  const auto image = CreateImage(65, 5, 2);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 65, 5, 2, 16, 72, 12);
  std::vector<std::uint8_t> expected;
  for (unsigned layer = 0; layer < 2; ++layer) {
    std::vector<std::uint8_t> packed(272 * 2);
    for (unsigned row = 0; row < 2; ++row) {
      std::memcpy(packed.data() + row * 272,
                  source.data + 16 + layer * 288 * 3 + row * 288, 272);
    }
    const auto decoded = Decode(packed.data(), 65, 5);
    expected.insert(expected.end(), decoded.begin(), decoded.end());
  }
  emulation->PrepareSubmit(VK_NULL_HANDLE, &command, 1);
  EXPECT_EQ(Output(command), expected);
}

TEST_P(VulkanEtc2Test, HandlesConcurrentSubmissionsOfUnmappedSources) {
  auto source = CreateSource(256 * 256, false);
  std::memset(source.data, 0x39, 256 * 256);
  const auto expected = Decode(source.data, 256, 256);
  const auto image = CreateImage(256, 256);
  std::vector<VkCommandBuffer> commands;
  for (unsigned i = 0; i < 4; ++i) {
    commands.push_back(Handle<VkCommandBuffer>(10 + i));
    Record(commands.back(), source, image, 256, 256);
  }
  const unsigned mapped = map_calls.load();
  const unsigned unmapped = unmap_calls.load();
  std::vector<std::thread> callers;
  for (const auto command : commands) {
    callers.emplace_back([&, command] {
      emulation->PrepareSubmit(VK_NULL_HANDLE, &command, 1);
      EXPECT_EQ(Output(command), expected);
    });
  }
  for (auto& caller : callers) caller.join();
  EXPECT_EQ(map_calls.load() - mapped, commands.size());
  EXPECT_EQ(unmap_calls.load() - unmapped, commands.size());
}

TEST_P(VulkanEtc2Test, RepeatedSecondaryIsDecodedOnce) {
  auto source = CreateSource(256 * 256, false);
  const auto image = CreateImage(256, 256);
  const auto primary = Handle<VkCommandBuffer>(10);
  const auto secondary = Handle<VkCommandBuffer>(11);
  Record(secondary, source, image, 256, 256);
  const VkCommandBuffer repeated[] = {secondary, secondary};
  emulation->CmdExecuteCommands(device, primary, 2, repeated);
  emulation->PrepareSubmit(VK_NULL_HANDLE, &primary, 1);
  EXPECT_EQ(Output(secondary), Decode(source.data, 256, 256));
  emulation->ReleaseCommandBuffer(primary);
  emulation->ReleaseCommandBuffer(secondary);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

TEST_P(VulkanEtc2Test, UpscalingStillProducesTheSamePixels) {
  Start(true, "4");
  auto source = CreateSource(4 * 4 * 16);
  std::memset(source.data, 0x53, 4 * 4 * 16);
  const auto image = CreateImage(16, 16);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 16, 16);
  std::vector<std::uint8_t> expected(64 * 64 * 4);
  const auto decoded = Decode(source.data, 16, 16);
  ResampleRgba(decoded.data(), 16, 16, 64, 64, expected.data());
  emulation->PrepareSubmit(VK_NULL_HANDLE, &command, 1);
  EXPECT_EQ(Output(command), expected);
}

TEST_P(VulkanEtc2Test, DefaultPreservesStreamedMipChain) {
  Start(true, nullptr);
  constexpr std::uint32_t kMips = 7;
  const auto image = CreateImage(64, 64, 1, kMips);
  EXPECT_EQ(last_image.format, VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(last_image.extent.width, 64U);
  EXPECT_EQ(last_image.extent.height, 64U);
  EXPECT_EQ(last_image.mipLevels, kMips);

  std::vector<VkBufferImageCopy> regions;
  VkDeviceSize compressed_bytes = 0;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    const std::uint32_t size = 64U >> mip;
    VkBufferImageCopy region{};
    region.bufferOffset = compressed_bytes;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    region.imageExtent = {size, size, 1};
    regions.push_back(region);
    compressed_bytes += ((size + 3) / 4) * ((size + 3) / 4) * 16;
  }
  const auto source = CreateSource(compressed_bytes);
  std::vector<std::uint8_t> expected;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    const std::uint32_t size = regions[mip].imageExtent.width;
    auto* pixels = source.data + regions[mip].bufferOffset;
    std::memset(pixels, 0x12 + mip * 17,
                ((size + 3) / 4) * ((size + 3) / 4) * 16);
    const auto decoded = Decode(pixels, size, size);
    expected.insert(expected.end(), decoded.begin(), decoded.end());
  }
  const auto command = Handle<VkCommandBuffer>(10);
  emulation->CmdCopyBufferToImage(
      device, command, source.buffer, image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips, regions.data());
  emulation->PrepareSubmit(VK_NULL_HANDLE, &command, 1);
  const auto& uploads = upload_regions.at(command);
  ASSERT_EQ(uploads.size(), kMips);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_EQ(uploads[mip].imageSubresource.mipLevel, mip);
    EXPECT_EQ(uploads[mip].imageExtent.width, 64U >> mip);
    EXPECT_EQ(uploads[mip].imageExtent.height, 64U >> mip);
  }
  EXPECT_TRUE(Output(command) == expected);

  const auto larger = CreateImage(128, 128, 1, kMips + 1);
  std::vector<VkImageCopy> copies;
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip + 1, 0, 1};
    copy.extent = regions[mip].imageExtent;
    copies.push_back(copy);
  }
  emulation->CmdCopyImage(device, command, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, larger,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kMips,
                          copies.data());
  EXPECT_EQ(blit_calls, 0U);
  ASSERT_EQ(image_copies.size(), kMips);
  for (std::uint32_t mip = 0; mip < kMips; ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_EQ(image_copies[mip].srcSubresource.mipLevel, mip);
    EXPECT_EQ(image_copies[mip].dstSubresource.mipLevel, mip + 1);
    EXPECT_EQ(image_copies[mip].extent.width, 64U >> mip);
    EXPECT_EQ(image_copies[mip].extent.height, 64U >> mip);
  }
}

TEST_P(VulkanEtc2Test, NativeSupportPassesThroughWithoutCpuUploads) {
  Start(false, "4");
  const auto source = CreateSource(16);
  const auto image = CreateImage(4, 4);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 4, 4);
  EXPECT_EQ(last_image.format, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
  EXPECT_EQ(last_image.extent.width, 4U);
  EXPECT_EQ(recorded.at(command), source.buffer);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

INSTANTIATE_TEST_SUITE_P(HostMemory, VulkanEtc2Test, ::testing::Bool());

class VulkanEtc2GpuTest : public ::testing::Test {
 protected:
  struct Operation {
    bool dispatch = false;
    VkBuffer source = VK_NULL_HANDLE;
    VkBuffer target = VK_NULL_HANDLE;
    std::vector<VkBufferCopy> regions;
    std::vector<std::uint32_t> job;
    std::array<VkDescriptorBufferInfo, 2> buffers{};
    std::uint32_t groups[3] = {0, 0, 0};
  };

  struct Source {
    VkBuffer buffer;
    std::uint8_t* data;
  };

  void SetUp() override {
    active = this;
    unsetenv("BLOX7_TEXTURE_OVERRIDE_DIR");
    unsetenv("BLOX7_TEXTURE_DUMP_DIR");
    unsetenv("BLOX7_SMALL_TEXTURE_UPSCALE");
    memory_properties.memoryTypeCount = 2;
    memory_properties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    memory_properties.memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    limits.maxStorageBufferRange = 1U << 27;
    limits.minStorageBufferOffsetAlignment = 16;
    limits.maxComputeWorkGroupCount[0] = 65535;
    limits.maxComputeWorkGroupCount[1] = 65535;
    limits.maxComputeWorkGroupCount[2] = 65535;
    families[0].queueFlags =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    families[0].queueCount = 1;
    families[1].queueFlags = VK_QUEUE_TRANSFER_BIT;
    families[1].queueCount = 1;
    Start(true);
  }

  void TearDown() override {
    emulation->DestroyDevice(device);
    emulation.reset();
    active = nullptr;
  }

  void Start(bool decoder) {
    if (emulation) emulation->DestroyDevice(device);
    emulation = std::make_unique<VulkanEtc2Emulation>();
    if (decoder) {
      emulation->RegisterDevice(device, Handle<VkPhysicalDevice>(2), true,
                                memory_properties, GetProc, &limits,
                                families, 2);
    } else {
      emulation->RegisterDevice(device, Handle<VkPhysicalDevice>(2), true,
                                memory_properties, GetProc);
    }
    emulation->RegisterQueue(device, graphics, 0);
    emulation->RegisterQueue(device, transfer, 1);
  }

  Source CreateSource(std::size_t bytes) {
    const VkBuffer buffer = Handle<VkBuffer>(next++);
    const VkDeviceMemory memory = Handle<VkDeviceMemory>(next++);
    sizes[buffer] = bytes;
    memory_bytes[memory].resize(bytes);
    EXPECT_EQ(emulation->BindBufferMemory(device, buffer, memory, 0),
              VK_SUCCESS);
    void* data = nullptr;
    EXPECT_EQ(emulation->MapMemory(device, memory, 0, bytes, 0, &data),
              VK_SUCCESS);
    auto* bytes_out = static_cast<std::uint8_t*>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
      bytes_out[index] = static_cast<std::uint8_t>(index * 131 + 17);
    }
    return {buffer, bytes_out};
  }

  VkImage CreateImage(VkFormat format, std::uint32_t width,
                      std::uint32_t height, std::uint32_t mips) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.arrayLayers = 1;
    info.mipLevels = mips;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage image = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImage(device, &info, nullptr, &image),
              VK_SUCCESS);
    return image;
  }

  std::vector<VkBufferImageCopy> Upload(VkCommandBuffer command,
                                        VkFormat format, std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t mips, Source* source) {
    Etc2EmulatedFormat mapped;
    EXPECT_TRUE(LookupEmulatedEtc2Format(format, &mapped));
    etc = mapped.etc_format;
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
      VkBufferImageCopy region{};
      region.bufferOffset = offset;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
      region.imageExtent = {std::max(1U, width >> mip),
                            std::max(1U, height >> mip), 1};
      offset += ((region.imageExtent.width + 3) / 4) *
                ((region.imageExtent.height + 3) / 4) * EtcBlockBytes(etc);
      regions.push_back(region);
    }
    *source = CreateSource(offset);
    const VkImage image = CreateImage(format, width, height, mips);
    emulation->CmdCopyBufferToImage(
        device, command, source->buffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(regions.size()), regions.data());
    return regions;
  }

  std::vector<std::uint8_t> Expected(const Source& source,
                                     const VkBufferImageCopy& region) const {
    const std::uint32_t width = region.imageExtent.width;
    const std::uint32_t height = region.imageExtent.height;
    std::vector<std::uint8_t> decoded(static_cast<std::size_t>(width) *
                                      height * EtcDecodedTexelBytes(etc));
    EXPECT_TRUE(DecodeEtcImage(
        etc, source.data + region.bufferOffset,
        ((width + 3) / 4) * ((height + 3) / 4) * EtcBlockBytes(etc), width,
        height, decoded.data(), decoded.size()));
    return decoded;
  }

  const std::vector<std::uint8_t>& Staging(VkCommandBuffer command) const {
    return memory_bytes.at(bindings.at(staging_of.at(command)).first);
  }

  bool StagingMatches(VkCommandBuffer command, const Source& source,
                      const std::vector<VkBufferImageCopy>& regions,
                      std::uint32_t mip) const {
    const std::vector<std::uint8_t> expected = Expected(source, regions[mip]);
    const VkDeviceSize offset = rewritten.at(command)[mip].bufferOffset;
    const std::vector<std::uint8_t>& staging = Staging(command);
    return offset + expected.size() <= staging.size() &&
           std::memcmp(staging.data() + offset, expected.data(),
                       expected.size()) == 0;
  }

  std::size_t Dispatches() const {
    std::size_t count = 0;
    for (const Operation& operation : executed) {
      count += operation.dispatch ? 1 : 0;
    }
    return count;
  }

  template <typename Info, typename Object>
  static VkResult VKAPI_CALL Create(VkDevice, const Info*,
                                    const VkAllocationCallbacks*,
                                    Object* object) {
    *object = Handle<Object>(active->next++);
    return VK_SUCCESS;
  }

  template <typename Object>
  static void VKAPI_CALL Destroy(VkDevice, Object,
                                 const VkAllocationCallbacks*) {}

  template <typename Object, typename Flags>
  static VkResult VKAPI_CALL Reset(VkDevice, Object, Flags) {
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostCreateImage(VkDevice, const VkImageCreateInfo*,
                                             const VkAllocationCallbacks*,
                                             VkImage* image) {
    *image = Handle<VkImage>(active->next++);
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostCreateBuffer(VkDevice,
                                              const VkBufferCreateInfo* info,
                                              const VkAllocationCallbacks*,
                                              VkBuffer* buffer) {
    *buffer = Handle<VkBuffer>(active->next++);
    active->sizes[*buffer] = info->size;
    active->usages[*buffer] = info->usage;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostRequirements(VkDevice, VkBuffer buffer,
                                          VkMemoryRequirements* requirements) {
    *requirements = {active->sizes.at(buffer), 16, 3U};
  }

  static VkResult VKAPI_CALL HostAllocate(VkDevice,
                                          const VkMemoryAllocateInfo* info,
                                          const VkAllocationCallbacks*,
                                          VkDeviceMemory* memory) {
    if (active->fail_device_memory && info->memoryTypeIndex == 1) {
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *memory = Handle<VkDeviceMemory>(active->next++);
    active->memory_bytes[*memory].assign(info->allocationSize, 0xa5);
    active->memory_types[*memory] = info->memoryTypeIndex;
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostBind(VkDevice, VkBuffer buffer,
                                      VkDeviceMemory memory,
                                      VkDeviceSize offset) {
    active->bindings[buffer] = {memory, offset};
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostMap(VkDevice, VkDeviceMemory memory,
                                     VkDeviceSize offset, VkDeviceSize,
                                     VkMemoryMapFlags, void** data) {
    *data = active->memory_bytes.at(memory).data() + offset;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostUnmap(VkDevice, VkDeviceMemory) {}

  static void VKAPI_CALL HostFree(VkDevice, VkDeviceMemory memory,
                                  const VkAllocationCallbacks*) {
    active->memory_bytes.erase(memory);
  }

  static void VKAPI_CALL HostDestroyBuffer(VkDevice, VkBuffer buffer,
                                           const VkAllocationCallbacks*) {
    active->sizes.erase(buffer);
    active->bindings.erase(buffer);
  }

  static void VKAPI_CALL HostCopyToImage(VkCommandBuffer command,
                                         VkBuffer buffer, VkImage,
                                         VkImageLayout, std::uint32_t count,
                                         const VkBufferImageCopy* regions) {
    active->staging_of[command] = buffer;
    active->rewritten[command].assign(regions, regions + count);
  }

  static VkResult VKAPI_CALL HostCreatePipelines(
      VkDevice, VkPipelineCache, std::uint32_t count,
      const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*,
      VkPipeline* pipelines) {
    for (std::uint32_t index = 0; index < count; ++index) {
      pipelines[index] = Handle<VkPipeline>(active->next++);
    }
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostAllocateSets(
      VkDevice, const VkDescriptorSetAllocateInfo* info,
      VkDescriptorSet* sets) {
    for (std::uint32_t index = 0; index < info->descriptorSetCount; ++index) {
      sets[index] = Handle<VkDescriptorSet>(active->next++);
    }
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostUpdateSets(VkDevice, std::uint32_t count,
                                        const VkWriteDescriptorSet* writes,
                                        std::uint32_t,
                                        const VkCopyDescriptorSet*) {
    for (std::uint32_t index = 0; index < count; ++index) {
      active->set_buffers[writes[index].dstSet][writes[index].dstBinding] =
          *writes[index].pBufferInfo;
    }
  }

  static VkResult VKAPI_CALL HostAllocateCommands(
      VkDevice, const VkCommandBufferAllocateInfo* info,
      VkCommandBuffer* commands) {
    for (std::uint32_t index = 0; index < info->commandBufferCount; ++index) {
      commands[index] = Handle<VkCommandBuffer>(active->next++);
    }
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostBegin(VkCommandBuffer command,
                                       const VkCommandBufferBeginInfo*) {
    active->recorded[command].clear();
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostEnd(VkCommandBuffer) { return VK_SUCCESS; }

  static void VKAPI_CALL HostCopyBuffer(VkCommandBuffer command,
                                        VkBuffer source, VkBuffer target,
                                        std::uint32_t count,
                                        const VkBufferCopy* regions) {
    Operation operation;
    operation.source = source;
    operation.target = target;
    operation.regions.assign(regions, regions + count);
    active->recorded[command].push_back(operation);
  }

  static void VKAPI_CALL HostBarrier(VkCommandBuffer, VkPipelineStageFlags,
                                     VkPipelineStageFlags, VkDependencyFlags,
                                     std::uint32_t, const VkMemoryBarrier*,
                                     std::uint32_t,
                                     const VkBufferMemoryBarrier*,
                                     std::uint32_t,
                                     const VkImageMemoryBarrier*) {}

  static void VKAPI_CALL HostBindPipeline(VkCommandBuffer,
                                          VkPipelineBindPoint, VkPipeline) {}

  static void VKAPI_CALL HostBindSets(VkCommandBuffer command,
                                      VkPipelineBindPoint, VkPipelineLayout,
                                      std::uint32_t, std::uint32_t,
                                      const VkDescriptorSet* sets,
                                      std::uint32_t, const std::uint32_t*) {
    active->bound[command] = sets[0];
  }

  static void VKAPI_CALL HostPush(VkCommandBuffer command, VkPipelineLayout,
                                  VkShaderStageFlags, std::uint32_t,
                                  std::uint32_t size, const void* values) {
    const auto* words = static_cast<const std::uint32_t*>(values);
    active->pushed[command].assign(words, words + size / 4);
  }

  static void VKAPI_CALL HostDispatch(VkCommandBuffer command, std::uint32_t x,
                                      std::uint32_t y, std::uint32_t z) {
    Operation operation;
    operation.dispatch = true;
    operation.job = active->pushed[command];
    operation.buffers = active->set_buffers[active->bound[command]];
    operation.groups[0] = x;
    operation.groups[1] = y;
    operation.groups[2] = z;
    active->recorded[command].push_back(operation);
  }

  static VkResult VKAPI_CALL HostSubmit(VkQueue queue, std::uint32_t count,
                                        const VkSubmitInfo* submits,
                                        VkFence fence) {
    for (std::uint32_t index = 0; index < count; ++index) {
      for (std::uint32_t command = 0;
           command < submits[index].commandBufferCount; ++command) {
        for (const Operation& operation :
             active->recorded[submits[index].pCommandBuffers[command]]) {
          if (operation.dispatch && active->fail_dispatch_submits) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
          }
        }
      }
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      for (std::uint32_t command = 0;
           command < submits[index].commandBufferCount; ++command) {
        for (const Operation& operation :
             active->recorded[submits[index].pCommandBuffers[command]]) {
          if (!operation.dispatch) {
            const auto from = active->bindings.at(operation.source);
            const auto to = active->bindings.at(operation.target);
            for (const VkBufferCopy& region : operation.regions) {
              std::memcpy(active->memory_bytes.at(to.first).data() +
                              to.second + region.dstOffset,
                          active->memory_bytes.at(from.first).data() +
                              from.second + region.srcOffset,
                          region.size);
            }
          }
          active->executed.push_back(operation);
          active->executed_queues.push_back(queue);
        }
      }
    }
    if (fence != VK_NULL_HANDLE) {
      active->signaled[fence] = true;
    }
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostCreateFence(VkDevice,
                                             const VkFenceCreateInfo*,
                                             const VkAllocationCallbacks*,
                                             VkFence* fence) {
    *fence = Handle<VkFence>(active->next++);
    active->signaled[*fence] = false;
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostFenceStatus(VkDevice, VkFence fence) {
    return active->signaled[fence] ? VK_SUCCESS : VK_NOT_READY;
  }

  static VkResult VKAPI_CALL HostWait(VkDevice, std::uint32_t, const VkFence*,
                                      VkBool32, std::uint64_t) {
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostResetFences(VkDevice, std::uint32_t count,
                                             const VkFence* fences) {
    for (std::uint32_t index = 0; index < count; ++index) {
      active->signaled[fences[index]] = false;
    }
    return VK_SUCCESS;
  }

  static PFN_vkVoidFunction VKAPI_CALL GetProc(VkDevice, const char* name) {
    const auto proc = [](auto function) {
      return reinterpret_cast<PFN_vkVoidFunction>(function);
    };
    const std::pair<const char*, PFN_vkVoidFunction> functions[] = {
        {"vkCreateImage", proc(HostCreateImage)},
        {"vkDestroyImage", proc(Destroy<VkImage>)},
        {"vkCreateImageView",
         proc(Create<VkImageViewCreateInfo, VkImageView>)},
        {"vkCreateBuffer", proc(HostCreateBuffer)},
        {"vkGetBufferMemoryRequirements", proc(HostRequirements)},
        {"vkAllocateMemory", proc(HostAllocate)},
        {"vkBindBufferMemory", proc(HostBind)},
        {"vkMapMemory", proc(HostMap)},
        {"vkUnmapMemory", proc(HostUnmap)},
        {"vkFreeMemory", proc(HostFree)},
        {"vkDestroyBuffer", proc(HostDestroyBuffer)},
        {"vkCmdCopyBufferToImage", proc(HostCopyToImage)},
        {"vkCreateShaderModule",
         proc(Create<VkShaderModuleCreateInfo, VkShaderModule>)},
        {"vkDestroyShaderModule", proc(Destroy<VkShaderModule>)},
        {"vkCreateDescriptorSetLayout",
         proc(Create<VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout>)},
        {"vkDestroyDescriptorSetLayout",
         proc(Destroy<VkDescriptorSetLayout>)},
        {"vkCreatePipelineLayout",
         proc(Create<VkPipelineLayoutCreateInfo, VkPipelineLayout>)},
        {"vkDestroyPipelineLayout", proc(Destroy<VkPipelineLayout>)},
        {"vkCreateComputePipelines", proc(HostCreatePipelines)},
        {"vkDestroyPipeline", proc(Destroy<VkPipeline>)},
        {"vkCreateDescriptorPool",
         proc(Create<VkDescriptorPoolCreateInfo, VkDescriptorPool>)},
        {"vkDestroyDescriptorPool", proc(Destroy<VkDescriptorPool>)},
        {"vkResetDescriptorPool",
         proc(Reset<VkDescriptorPool, VkDescriptorPoolResetFlags>)},
        {"vkAllocateDescriptorSets", proc(HostAllocateSets)},
        {"vkUpdateDescriptorSets", proc(HostUpdateSets)},
        {"vkCreateCommandPool",
         proc(Create<VkCommandPoolCreateInfo, VkCommandPool>)},
        {"vkDestroyCommandPool", proc(Destroy<VkCommandPool>)},
        {"vkResetCommandPool",
         proc(Reset<VkCommandPool, VkCommandPoolResetFlags>)},
        {"vkAllocateCommandBuffers", proc(HostAllocateCommands)},
        {"vkBeginCommandBuffer", proc(HostBegin)},
        {"vkEndCommandBuffer", proc(HostEnd)},
        {"vkCmdCopyBuffer", proc(HostCopyBuffer)},
        {"vkCmdPipelineBarrier", proc(HostBarrier)},
        {"vkCmdBindPipeline", proc(HostBindPipeline)},
        {"vkCmdBindDescriptorSets", proc(HostBindSets)},
        {"vkCmdPushConstants", proc(HostPush)},
        {"vkCmdDispatch", proc(HostDispatch)},
        {"vkQueueSubmit", proc(HostSubmit)},
        {"vkCreateFence", proc(HostCreateFence)},
        {"vkDestroyFence", proc(Destroy<VkFence>)},
        {"vkGetFenceStatus", proc(HostFenceStatus)},
        {"vkWaitForFences", proc(HostWait)},
        {"vkResetFences", proc(HostResetFences)},
    };
    for (const auto& entry : functions) {
      if (std::strcmp(name, entry.first) == 0) return entry.second;
    }
    return nullptr;
  }

  inline static VulkanEtc2GpuTest* active = nullptr;
  const VkDevice device = Handle<VkDevice>(1);
  const VkQueue graphics = Handle<VkQueue>(3);
  const VkQueue transfer = Handle<VkQueue>(4);
  std::uintptr_t next = 100;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkPhysicalDeviceLimits limits{};
  VkQueueFamilyProperties families[2] = {};
  EtcFormat etc = EtcFormat::kEtc2Rgb8;
  bool fail_device_memory = false;
  bool fail_dispatch_submits = false;
  std::unordered_map<VkBuffer, VkDeviceSize> sizes;
  std::unordered_map<VkBuffer, VkBufferUsageFlags> usages;
  std::unordered_map<VkBuffer, std::pair<VkDeviceMemory, VkDeviceSize>>
      bindings;
  std::unordered_map<VkDeviceMemory, std::vector<std::uint8_t>> memory_bytes;
  std::unordered_map<VkDeviceMemory, std::uint32_t> memory_types;
  std::unordered_map<VkCommandBuffer, VkBuffer> staging_of;
  std::unordered_map<VkCommandBuffer, std::vector<VkBufferImageCopy>> rewritten;
  std::unordered_map<VkDescriptorSet, std::array<VkDescriptorBufferInfo, 2>>
      set_buffers;
  std::unordered_map<VkCommandBuffer, VkDescriptorSet> bound;
  std::unordered_map<VkCommandBuffer, std::vector<std::uint32_t>> pushed;
  std::unordered_map<VkCommandBuffer, std::vector<Operation>> recorded;
  std::unordered_map<VkFence, bool> signaled;
  std::vector<Operation> executed;
  std::vector<VkQueue> executed_queues;
  std::unique_ptr<VulkanEtc2Emulation> emulation;
};

TEST_F(VulkanEtc2GpuTest, ComputeQueueDecodesOnTheGpuAndLeavesTheCpuOut) {
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, 64,
                              48, 7, &source);
  const VkBuffer staging = staging_of.at(command);
  EXPECT_EQ(memory_types.at(bindings.at(staging).first), 1U);
  EXPECT_NE(usages.at(staging) & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0U);
  EXPECT_NE(usages.at(staging) & VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0U);
  emulation->PrepareSubmit(graphics, &command, 1);
  ASSERT_EQ(Dispatches(), regions.size());
  EXPECT_EQ(std::count(executed_queues.begin(), executed_queues.end(),
                       graphics),
            static_cast<std::ptrdiff_t>(executed.size()));
  std::size_t mip = 0;
  for (const Operation& operation : executed) {
    if (!operation.dispatch) {
      ASSERT_EQ(operation.source, source.buffer);
      ASSERT_EQ(operation.regions.size(), regions.size());
      for (std::size_t index = 0; index < regions.size(); ++index) {
        EXPECT_EQ(operation.regions[index].srcOffset,
                  regions[index].bufferOffset);
      }
      continue;
    }
    SCOPED_TRACE(mip);
    const VkBufferImageCopy& region = regions[mip];
    const VkBufferImageCopy& host = rewritten.at(command)[mip];
    ASSERT_EQ(operation.job.size(), 11U);
    EXPECT_EQ(operation.job[0], 2U);
    EXPECT_EQ(operation.job[1], region.imageExtent.width);
    EXPECT_EQ(operation.job[2], region.imageExtent.height);
    EXPECT_EQ(operation.job[3], (region.imageExtent.width + 3) / 4);
    EXPECT_EQ(operation.job[4], (region.imageExtent.height + 3) / 4);
    EXPECT_EQ(operation.job[9], region.imageExtent.width);
    EXPECT_EQ(operation.buffers[1].buffer, staging);
    EXPECT_EQ(operation.buffers[1].offset + operation.job[8] * 4,
              host.bufferOffset);
    EXPECT_EQ(host.bufferOffset % 16, 0U);
    EXPECT_EQ(host.bufferRowLength, 0U);
    EXPECT_EQ(operation.groups[0], (operation.job[3] + 7) / 8);
    EXPECT_EQ(operation.groups[1], (operation.job[4] + 7) / 8);
    EXPECT_EQ(operation.groups[2], 1U);
    ++mip;
  }
  const std::vector<std::uint8_t>& bytes = Staging(command);
  EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(),
                          [](std::uint8_t value) { return value == 0xa5; }));
}

TEST_F(VulkanEtc2GpuTest, TransferQueueCarriesTheCpuDecodeIntoGpuStaging) {
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 37,
                              21, 6, &source);
  emulation->PrepareSubmit(transfer, &command, 1);
  EXPECT_EQ(Dispatches(), 0U);
  ASSERT_FALSE(executed.empty());
  for (std::size_t mip = 0; mip < regions.size(); ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_TRUE(StagingMatches(command, source, regions, mip));
  }
  EXPECT_EQ(std::count(executed_queues.begin(), executed_queues.end(),
                       transfer),
            static_cast<std::ptrdiff_t>(executed.size()));
}

TEST_F(VulkanEtc2GpuTest, OddWidthR11MipsAreCarriedNextToGpuDecodes) {
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions =
      Upload(command, VK_FORMAT_EAC_R11_UNORM_BLOCK, 6, 6, 3, &source);
  emulation->PrepareSubmit(graphics, &command, 1);
  ASSERT_EQ(Dispatches(), 1U);
  for (const Operation& operation : executed) {
    if (operation.dispatch) {
      EXPECT_EQ(operation.job[0], 3U);
      EXPECT_EQ(operation.job[1], 6U);
      EXPECT_EQ(operation.job[9], 3U);
    }
  }
  EXPECT_TRUE(StagingMatches(command, source, regions, 1));
  EXPECT_TRUE(StagingMatches(command, source, regions, 2));
  for (const VkBufferImageCopy& region : rewritten.at(command)) {
    EXPECT_EQ(region.bufferOffset % 16, 0U);
  }
}

TEST_F(VulkanEtc2GpuTest, FailedGpuSubmitFallsBackToTheCarry) {
  fail_dispatch_submits = true;
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
                              16, 16, 5, &source);
  emulation->PrepareSubmit(graphics, &command, 1);
  EXPECT_EQ(Dispatches(), 0U);
  for (std::size_t mip = 0; mip < regions.size(); ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_TRUE(StagingMatches(command, source, regions, mip));
  }
}

TEST_F(VulkanEtc2GpuTest, DeviceStagingFailureKeepsTheCpuPath) {
  fail_device_memory = true;
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 20,
                              12, 4, &source);
  const VkBuffer staging = staging_of.at(command);
  EXPECT_EQ(memory_types.at(bindings.at(staging).first), 0U);
  EXPECT_EQ(usages.at(staging) & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0U);
  emulation->PrepareSubmit(graphics, &command, 1);
  EXPECT_EQ(Dispatches(), 0U);
  EXPECT_TRUE(executed.empty());
  for (std::size_t mip = 0; mip < regions.size(); ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_TRUE(StagingMatches(command, source, regions, mip));
  }
}

TEST_F(VulkanEtc2GpuTest, TextureOverridesKeepTheCpuPath) {
  char pattern[] = "/tmp/blox7-etc2-overrides-XXXXXX";
  const char* directory = mkdtemp(pattern);
  ASSERT_NE(directory, nullptr);
  std::FILE* placeholder =
      std::fopen((std::string(directory) + "/placeholder.png").c_str(), "wb");
  ASSERT_NE(placeholder, nullptr);
  std::fclose(placeholder);
  setenv("BLOX7_TEXTURE_OVERRIDE_DIR", directory, 1);
  Start(true);
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 8,
                              8, 4, &source);
  EXPECT_EQ(memory_types.at(bindings.at(staging_of.at(command)).first), 0U);
  emulation->PrepareSubmit(graphics, &command, 1);
  EXPECT_TRUE(executed.empty());
  for (std::size_t mip = 0; mip < regions.size(); ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_TRUE(StagingMatches(command, source, regions, mip));
  }
  unsetenv("BLOX7_TEXTURE_OVERRIDE_DIR");
  std::filesystem::remove_all(directory);
}

TEST_F(VulkanEtc2GpuTest, CpuOnlyStagingAlignsEachRegionToFourBytes) {
  Start(false);
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions =
      Upload(command, VK_FORMAT_EAC_R11_SNORM_BLOCK, 5, 3, 3, &source);
  emulation->PrepareSubmit(graphics, &command, 1);
  EXPECT_TRUE(executed.empty());
  for (std::size_t mip = 0; mip < regions.size(); ++mip) {
    SCOPED_TRACE(mip);
    EXPECT_EQ(rewritten.at(command)[mip].bufferOffset % 4, 0U);
    EXPECT_TRUE(StagingMatches(command, source, regions, mip));
  }
}

TEST_F(VulkanEtc2GpuTest, UnknownQueueSubmitsNothing) {
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  Upload(command, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 8, 8, 1, &source);
  emulation->PrepareSubmit(Handle<VkQueue>(99), &command, 1);
  EXPECT_TRUE(executed.empty());
}

TEST_F(VulkanEtc2GpuTest, ResubmissionDecodesTheCurrentSourceAgain) {
  const auto command = Handle<VkCommandBuffer>(10);
  Source source{};
  const auto regions = Upload(command, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 24,
                              24, 1, &source);
  emulation->PrepareSubmit(transfer, &command, 1);
  EXPECT_TRUE(StagingMatches(command, source, regions, 0));
  for (std::size_t index = 0; index < sizes.at(source.buffer); ++index) {
    source.data[index] = static_cast<std::uint8_t>(index * 7 + 3);
  }
  emulation->PrepareSubmit(transfer, &command, 1);
  EXPECT_TRUE(StagingMatches(command, source, regions, 0));
  emulation->PrepareSubmit(graphics, &command, 1);
  EXPECT_EQ(Dispatches(), 1U);
  emulation->ReleaseCommandBuffer(command);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

}
}
