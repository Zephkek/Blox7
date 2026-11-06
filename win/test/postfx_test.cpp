// post fx on the real gpu, headless. postfx_test <out dir> [fx] [other.spv]
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "blox7/graphics/vulkan_text_overlay_compositor.h"

namespace {

PFN_vkGetInstanceProcAddr gipa;
PFN_vkGetDeviceProcAddr gdpa;
VkInstance instance;
VkPhysicalDevice physical;
VkDevice device;
VkQueue queue;
uint32_t family = UINT32_MAX;
constexpr uint32_t kWidth = 256, kHeight = 128;

template <class T>
T dev(const char* name) {
  return reinterpret_cast<T>(gdpa(device, name));
}

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
  VkPhysicalDeviceMemoryProperties props{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
  return 0;
}

VkCommandPool pool;

VkCommandBuffer begin() {
  VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cmd;
  dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &info, &cmd);
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd, &begin_info);
  return cmd;
}

void finish(VkCommandBuffer cmd, VkSemaphore wait = VK_NULL_HANDLE) {
  dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd);
  const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = wait ? 1 : 0;
  submit.pWaitSemaphores = &wait;
  submit.pWaitDstStageMask = &stage;
  submit.commandBufferCount = cmd ? 1 : 0;
  submit.pCommandBuffers = &cmd;
  dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE);
  dev<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                        0, 0, nullptr, 0, nullptr, 1, &b);
}

void fill_pattern(VkImage image) {
  VkCommandBuffer cmd = begin();
  barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const auto clear = dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkClearColorValue gray{{0.5f, 0.5f, 0.5f, 1}};
  clear(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &gray, 1, &range);
  barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  finish(cmd);
}

void read_back(VkImage image, const std::string& path, std::vector<uint8_t>* pixels) {
  const VkDeviceSize size = kWidth * kHeight * 4;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &buffer);
  VkMemoryRequirements req;
  dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  VkDeviceMemory memory;
  dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &alloc, nullptr, &memory);
  dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, 0);
  VkCommandBuffer cmd = begin();
  barrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kWidth, kHeight, 1};
  dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  finish(cmd);
  void* mapped = nullptr;
  dev<PFN_vkMapMemory>("vkMapMemory")(device, memory, 0, size, 0, &mapped);
  pixels->assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + size);
  FILE* file = std::fopen(path.c_str(), "wb");
  BITMAPFILEHEADER header{0x4D42, static_cast<DWORD>(54 + size), 0, 0, 54};
  BITMAPINFOHEADER bmp{sizeof bmp, static_cast<LONG>(kWidth), -static_cast<LONG>(kHeight), 1, 32, BI_RGB};
  std::fwrite(&header, sizeof header, 1, file);
  std::fwrite(&bmp, sizeof bmp, 1, file);
  std::fwrite(pixels->data(), 1, size, file);
  std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string out = argv[1];
  const std::string effects = argc > 2 ? argv[2] : "sharpen=1,vibrance=0.6,contrast=1.2";
  SetEnvironmentVariableA("BLOX7_POST_FX", effects.c_str());
  _putenv_s("BLOX7_POST_FX", effects.c_str());

  HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
  gipa = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
  if (!gipa) return std::puts("no vulkan-1.dll"), 1;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "postfx_test", 1, nullptr, 0, VK_API_VERSION_1_1};
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
  if (reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&instance_info, nullptr, &instance) != VK_SUCCESS)
    return std::puts("vkCreateInstance failed"), 1;
  uint32_t count = 1;
  reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &count, &physical);
  VkPhysicalDeviceProperties props{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"))(physical, &props);
  uint32_t families = 0;
  const auto get_families =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  get_families(physical, &families, nullptr);
  std::vector<VkQueueFamilyProperties> family_props(families);
  get_families(physical, &families, family_props.data());
  for (uint32_t i = 0; i < families && family == UINT32_MAX; ++i)
    if (family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
  const float priority = 1;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
  const char* extensions[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue_info, 0, nullptr, 1, extensions};
  if (reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(physical, &device_info, nullptr, &device) != VK_SUCCESS)
    return std::puts("vkCreateDevice failed"), 1;
  gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
  dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
  dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pool_info, nullptr, &pool);
  std::printf("device: %s\n", props.deviceName);

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
  image_info.extent = {kWidth, kHeight, 1};
  image_info.mipLevels = image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage image;
  dev<PFN_vkCreateImage>("vkCreateImage")(device, &image_info, nullptr, &image);
  VkMemoryRequirements req;
  dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                             memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  VkDeviceMemory memory;
  dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &alloc, nullptr, &memory);
  dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, 0);

  std::vector<uint8_t> pattern(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y)
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t* p = &pattern[(y * kWidth + x) * 4];
      uint8_t r, g, b;
      if (x < 64) r = g = b = ((x / 4) % 2) ? 200 : 60;
      else if (x < 128) r = g = b = static_cast<uint8_t>((x - 64) * 4);
      else if (y < 64) r = 150, g = 110, b = 100;
      else if (x >= 192) r = g = b = (x - 192) * 2 > (y - 64) ? 230 : 20;
      else r = 90, g = 120, b = 150;
      p[0] = b, p[1] = g, p[2] = r, p[3] = 255;
    }
  fill_pattern(image);
  const auto upload = [&] {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = pattern.size();
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging;
    dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &staging);
    VkMemoryRequirements sreq;
    dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, staging, &sreq);
    VkMemoryAllocateInfo salloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, sreq.size,
                                memory_type(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory smem;
    dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &salloc, nullptr, &smem);
    dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, staging, smem, 0);
    void* mapped;
    dev<PFN_vkMapMemory>("vkMapMemory")(device, smem, 0, pattern.size(), 0, &mapped);
    std::memcpy(mapped, pattern.data(), pattern.size());
    VkCommandBuffer cmd = begin();
    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    finish(cmd);
  };
  upload();
  std::vector<uint8_t> before, after;
  read_back(image, out + "\\before.bmp", &before);

  blox7::graphics::VulkanTextOverlayCompositor compositor;
  compositor.RegisterDevice(instance, physical, device, VK_API_VERSION_1_1, extensions, 1, family, 1, nullptr, gipa, gdpa);
  compositor.RegisterQueue(device, queue, family, 0);
  VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swapchain_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  swapchain_info.imageExtent = {kWidth, kHeight};
  swapchain_info.imageUsage = image_info.usage;
  const VkSwapchainKHR fake = reinterpret_cast<VkSwapchainKHR>(static_cast<uintptr_t>(0x1234));
  if (!compositor.RegisterSwapchain(device, fake, swapchain_info, &image, 1)) return std::puts("RegisterSwapchain refused"), 1;

  VkSemaphore rendered;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &semaphore_info, nullptr, &rendered);
  const uint32_t index = 0;
  static bool waited_on_compositor = false;
  const auto present_once = [&] {
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    signal.signalSemaphoreCount = 1;
    signal.pSignalSemaphores = &rendered;
    dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &signal, VK_NULL_HANDLE);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &fake;
    present.pImageIndices = &index;
    return compositor.QueuePresent(queue, &present, [](VkQueue q, const VkPresentInfoKHR* info) -> VkResult {
      waited_on_compositor = info->waitSemaphoreCount == 1;
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.waitSemaphoreCount = info->waitSemaphoreCount;
      submit.pWaitSemaphores = info->pWaitSemaphores;
      submit.pWaitDstStageMask = &stage;
      dev<PFN_vkQueueSubmit>("vkQueueSubmit")(q, 1, &submit, VK_NULL_HANDLE);
      return dev<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(q);
    });
  };
  const auto mean_change = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    long long diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return static_cast<double>(diff) / static_cast<double>(a.size());
  };
  const VkResult result = present_once();
  read_back(image, out + "\\after.bmp", &after);
  std::printf("present=%d waited_on_compositor=%d mean_abs_change=%.2f\n", result, waited_on_compositor,
              mean_change(before, after));
  const char* shader = std::getenv("BLOX7_POST_SHADER");
  if (argc > 3 && shader && CopyFileA(argv[3], shader, FALSE)) {
    for (int i = 0; i < 31; ++i) {
      upload();
      present_once();
    }
    std::vector<uint8_t> reloaded;
    read_back(image, out + "\\reloaded.bmp", &reloaded);
    std::printf("after the swap: mean_abs_change vs the first shader=%.2f\n", mean_change(after, reloaded));
  }
  compositor.DestroySwapchain(device, fake);
  compositor.DestroyDevice(device);
  return result == VK_SUCCESS && waited_on_compositor ? 0 : 1;
}
