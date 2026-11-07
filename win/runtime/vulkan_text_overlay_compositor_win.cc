// upstream's text overlay slot, now doing post fx
#include "blox7/graphics/vulkan_text_overlay_compositor.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gen/post_shaders.h"

namespace blox7 {
namespace graphics {
namespace {

struct Effects {
  float sharpen = 0, vibrance = 0, saturation = 1, contrast = 1, brightness = 0, gamma = 1;
  bool fxaa = false;
  bool on() const {
    return sharpen > 0 || vibrance != 0 || saturation != 1 || contrast != 1 || brightness != 0 || gamma != 1 || fxaa;
  }
};

Effects ReadEffects() {
  Effects fx;
  const char* value = std::getenv("BLOX7_POST_FX");
  if (value == nullptr) return fx;
  const std::string text = value;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find(',', at);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(at, end - at);
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      const std::string key = item.substr(0, eq);
      const float number = std::strtof(item.c_str() + eq + 1, nullptr);
      if (key == "sharpen") fx.sharpen = std::clamp(number, 0.0f, 1.0f);
      else if (key == "vibrance") fx.vibrance = std::clamp(number, -1.0f, 1.0f);
      else if (key == "saturation") fx.saturation = std::clamp(number, 0.0f, 2.0f);
      else if (key == "contrast") fx.contrast = std::clamp(number, 0.5f, 1.5f);
      else if (key == "brightness") fx.brightness = std::clamp(number, -0.25f, 0.25f);
      else if (key == "gamma") fx.gamma = std::clamp(number, 0.5f, 2.0f);
      else if (key == "fxaa") fx.fxaa = number > 0.5f;
    }
    at = end + 1;
  }
  return fx;
}

struct Push {
  float texel[2];
  float sharpen, vibrance, saturation, contrast, brightness, gamma, fxaa;
};

std::string CustomShaderPath() {
  const char* path = std::getenv("BLOX7_POST_SHADER");
  return path == nullptr ? std::string() : std::string(path);
}

std::vector<std::uint32_t> ReadSpirv(const std::string& path) {
  if (path.empty()) return {};
  std::vector<std::uint32_t> words;
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    std::uint32_t word = 0;
    while (std::fread(&word, sizeof word, 1, file) == 1 && words.size() < (1u << 22)) words.push_back(word);
    std::fclose(file);
  }
  if (words.size() < 5 || words[0] != 0x07230203u) {
    std::fprintf(stderr, "  [post-fx] %s is not SPIR-V, using the built-in effects\n", path.c_str());
    return {};
  }
  return words;
}

std::int64_t FileStamp(const std::string& path) {
  struct stat info {};
  if (path.empty() || stat(path.c_str(), &info) != 0) return 0;
#ifdef __BIONIC__
  return static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec + info.st_size;
#else
  return static_cast<std::int64_t>(info.st_mtime) * 1000000000 + info.st_size;
#endif
}

#define POST_DEVICE_FUNCTIONS(X)                                                                               \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)       \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)       \
  X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                              \
  X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateRenderPass)  \
  X(vkDestroyRenderPass) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateFramebuffer)             \
  X(vkDestroyFramebuffer) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)     \
  X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)        \
  X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)               \
  X(vkCmdCopyImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                         \
  X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw)         \
  X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)            \
  X(vkResetFences) X(vkQueueSubmit)

struct Functions {
#define POST_DECLARE(name) PFN_##name name = nullptr;
  POST_DEVICE_FUNCTIONS(POST_DECLARE)
#undef POST_DECLARE
};

struct Frame {
  VkImage target = VK_NULL_HANDLE;
  VkImageView target_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkImage copy = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView copy_view = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkSemaphore done = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
};

struct Device {
  VkDevice handle = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  Functions f;
  VkPhysicalDeviceMemoryProperties memory{};
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule vertex = VK_NULL_HANDLE;
  VkShaderModule fragment = VK_NULL_HANDLE;
};

struct Chain {
  VkDevice device = VK_NULL_HANDLE;
  VkExtent2D extent{};
  VkRenderPass pass = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkCommandPool commands = VK_NULL_HANDLE;
  std::vector<Frame> frames;
};

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src;
  barrier.dstAccessMask = dst;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return barrier;
}

}  // namespace

struct VulkanTextOverlayCompositor::Impl {
  std::mutex mutex;
  Effects fx;
  std::vector<std::uint32_t> custom;
  std::string custom_path;
  std::int64_t custom_stamp = 0;
  std::uint32_t presents = 0;
  std::unordered_map<VkDevice, Device> devices;
  std::unordered_map<VkQueue, std::pair<VkDevice, std::uint32_t>> queues;
  std::unordered_map<VkSwapchainKHR, Chain> chains;

  static bool MemoryType(const Device& d, std::uint32_t bits, std::uint32_t* index) {
    for (std::uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        *index = i;
        return true;
      }
    for (std::uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
      if (bits & (1u << i)) {
        *index = i;
        return true;
      }
    return false;
  }

  static void DestroyChain(Device& d, Chain& chain) {
    const Functions& f = d.f;
    for (Frame& frame : chain.frames)
      if (frame.fence) f.vkWaitForFences(d.handle, 1, &frame.fence, VK_TRUE, 1000000000ull);
    for (Frame& frame : chain.frames) {
      if (frame.fence) f.vkDestroyFence(d.handle, frame.fence, nullptr);
      if (frame.done) f.vkDestroySemaphore(d.handle, frame.done, nullptr);
      if (frame.framebuffer) f.vkDestroyFramebuffer(d.handle, frame.framebuffer, nullptr);
      if (frame.target_view) f.vkDestroyImageView(d.handle, frame.target_view, nullptr);
      if (frame.copy_view) f.vkDestroyImageView(d.handle, frame.copy_view, nullptr);
      if (frame.copy) f.vkDestroyImage(d.handle, frame.copy, nullptr);
      if (frame.memory) f.vkFreeMemory(d.handle, frame.memory, nullptr);
    }
    chain.frames.clear();
    if (chain.commands) f.vkDestroyCommandPool(d.handle, chain.commands, nullptr);
    if (chain.pool) f.vkDestroyDescriptorPool(d.handle, chain.pool, nullptr);
    if (chain.pipeline) f.vkDestroyPipeline(d.handle, chain.pipeline, nullptr);
    if (chain.pass) f.vkDestroyRenderPass(d.handle, chain.pass, nullptr);
    chain = Chain{};
  }

  static bool BuildChain(Device& d, Chain& chain, const VkSwapchainCreateInfoKHR& info, const VkImage* images,
                         std::uint32_t count) {
    const Functions& f = d.f;
    chain.device = d.handle;
    chain.extent = info.imageExtent;

    VkAttachmentDescription attachment{};
    attachment.format = info.imageFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    if (f.vkCreateRenderPass(d.handle, &pass_info, nullptr, &chain.pass) != VK_SUCCESS) return false;
    chain.pipeline = MakePipeline(d, chain.pass, d.fragment);
    if (chain.pipeline == VK_NULL_HANDLE) return false;

    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = count;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (f.vkCreateDescriptorPool(d.handle, &pool_info, nullptr, &chain.pool) != VK_SUCCESS) return false;
    return BuildFrames(d, chain, info, images, count);
  }

  static VkPipeline MakePipeline(const Device& d, VkRenderPass pass, VkShaderModule fragment) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d.vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = d.layout;
    pipeline_info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (d.f.vkCreateGraphicsPipelines(d.handle, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    return pipeline;
  }

  static bool BuildFrames(Device& d, Chain& chain, const VkSwapchainCreateInfoKHR& info, const VkImage* images,
                          std::uint32_t count) {
    const Functions& f = d.f;
    VkCommandPoolCreateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_info.queueFamilyIndex = d.family;
    if (f.vkCreateCommandPool(d.handle, &command_info, nullptr, &chain.commands) != VK_SUCCESS) return false;

    chain.frames.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      Frame& frame = chain.frames[i];
      frame.target = images[i];
      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = info.imageFormat;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      view_info.image = frame.target;
      if (f.vkCreateImageView(d.handle, &view_info, nullptr, &frame.target_view) != VK_SUCCESS) return false;
      VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = chain.pass;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &frame.target_view;
      framebuffer_info.width = info.imageExtent.width;
      framebuffer_info.height = info.imageExtent.height;
      framebuffer_info.layers = 1;
      if (f.vkCreateFramebuffer(d.handle, &framebuffer_info, nullptr, &frame.framebuffer) != VK_SUCCESS) return false;

      VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = info.imageFormat;
      image_info.extent = {info.imageExtent.width, info.imageExtent.height, 1};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (f.vkCreateImage(d.handle, &image_info, nullptr, &frame.copy) != VK_SUCCESS) return false;
      VkMemoryRequirements requirements{};
      f.vkGetImageMemoryRequirements(d.handle, frame.copy, &requirements);
      VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocate.allocationSize = requirements.size;
      if (!MemoryType(d, requirements.memoryTypeBits, &allocate.memoryTypeIndex) ||
          f.vkAllocateMemory(d.handle, &allocate, nullptr, &frame.memory) != VK_SUCCESS ||
          f.vkBindImageMemory(d.handle, frame.copy, frame.memory, 0) != VK_SUCCESS)
        return false;
      view_info.image = frame.copy;
      if (f.vkCreateImageView(d.handle, &view_info, nullptr, &frame.copy_view) != VK_SUCCESS) return false;

      VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      set_info.descriptorPool = chain.pool;
      set_info.descriptorSetCount = 1;
      set_info.pSetLayouts = &d.set_layout;
      if (f.vkAllocateDescriptorSets(d.handle, &set_info, &frame.set) != VK_SUCCESS) return false;
      const VkDescriptorImageInfo image_descriptor{d.sampler, frame.copy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = frame.set;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.pImageInfo = &image_descriptor;
      f.vkUpdateDescriptorSets(d.handle, 1, &write, 0, nullptr);

      VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      buffer_info.commandPool = chain.commands;
      buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      buffer_info.commandBufferCount = 1;
      if (f.vkAllocateCommandBuffers(d.handle, &buffer_info, &frame.commands) != VK_SUCCESS) return false;
      const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      if (f.vkCreateSemaphore(d.handle, &semaphore_info, nullptr, &frame.done) != VK_SUCCESS) return false;
      VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      if (f.vkCreateFence(d.handle, &fence_info, nullptr, &frame.fence) != VK_SUCCESS) return false;
    }
    return true;
  }

  bool Record(const Device& d, const Chain& chain, const Frame& frame) const {
    const Functions& f = d.f;
    VkCommandBuffer cmd = frame.commands;
    if (f.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (f.vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;

    const VkImageMemoryBarrier to_copy[2] = {
        Barrier(frame.target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(frame.copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT)};
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           to_copy);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {chain.extent.width, chain.extent.height, 1};
    f.vkCmdCopyImage(cmd, frame.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region);
    const VkImageMemoryBarrier to_draw[2] = {
        Barrier(frame.copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        Barrier(frame.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)};
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                           nullptr, 0, nullptr, 2, to_draw);

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = chain.pass;
    pass.framebuffer = frame.framebuffer;
    pass.renderArea = {{0, 0}, chain.extent};
    f.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, chain.pipeline);
    const VkViewport viewport{0, 0, static_cast<float>(chain.extent.width), static_cast<float>(chain.extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, chain.extent};
    f.vkCmdSetViewport(cmd, 0, 1, &viewport);
    f.vkCmdSetScissor(cmd, 0, 1, &scissor);
    f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &frame.set, 0, nullptr);
    const Push push{{1.0f / static_cast<float>(chain.extent.width), 1.0f / static_cast<float>(chain.extent.height)},
                    fx.sharpen, fx.vibrance, fx.saturation, fx.contrast, fx.brightness, fx.gamma, fx.fxaa ? 1.0f : 0.0f};
    f.vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    f.vkCmdDraw(cmd, 3, 1, 0, 0);
    f.vkCmdEndRenderPass(cmd);
    return f.vkEndCommandBuffer(cmd) == VK_SUCCESS;
  }

  void ReloadCustomShader() {
    const std::int64_t stamp = FileStamp(custom_path);
    if (stamp == 0 || stamp == custom_stamp) return;
    custom_stamp = stamp;
    const std::vector<std::uint32_t> words = ReadSpirv(custom_path);
    if (words.empty()) return;
    for (auto& [handle, d] : devices) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = words.size() * sizeof(std::uint32_t);
      info.pCode = words.data();
      VkShaderModule module = VK_NULL_HANDLE;
      if (d.f.vkCreateShaderModule(handle, &info, nullptr, &module) != VK_SUCCESS) {
        std::fprintf(stderr, "  [post-fx] the driver refused the new custom shader, keeping the old one\n");
        continue;
      }
      std::vector<std::pair<Chain*, VkPipeline>> fresh;
      bool built = true;
      for (auto& [swapchain, chain] : chains) {
        if (chain.device != handle) continue;
        const VkPipeline pipeline = MakePipeline(d, chain.pass, module);
        if (pipeline == VK_NULL_HANDLE) built = false;
        fresh.emplace_back(&chain, pipeline);
      }
      if (!built) {
        for (const auto& [chain, pipeline] : fresh)
          if (pipeline) d.f.vkDestroyPipeline(handle, pipeline, nullptr);
        d.f.vkDestroyShaderModule(handle, module, nullptr);
        std::fprintf(stderr, "  [post-fx] the new custom shader doesn't build, keeping the old one\n");
        continue;
      }
      for (auto& [chain, pipeline] : fresh) {
        for (Frame& frame : chain->frames) d.f.vkWaitForFences(handle, 1, &frame.fence, VK_TRUE, 1000000000ull);
        d.f.vkDestroyPipeline(handle, chain->pipeline, nullptr);
        chain->pipeline = pipeline;
      }
      d.f.vkDestroyShaderModule(handle, d.fragment, nullptr);
      d.fragment = module;
      std::fprintf(stderr, "  [post-fx] reloaded the custom shader\n");
    }
  }

  static void DestroyDevice(Device& d) {
    const Functions& f = d.f;
    if (d.vertex) f.vkDestroyShaderModule(d.handle, d.vertex, nullptr);
    if (d.fragment) f.vkDestroyShaderModule(d.handle, d.fragment, nullptr);
    if (d.layout) f.vkDestroyPipelineLayout(d.handle, d.layout, nullptr);
    if (d.set_layout) f.vkDestroyDescriptorSetLayout(d.handle, d.set_layout, nullptr);
    if (d.sampler) f.vkDestroySampler(d.handle, d.sampler, nullptr);
  }
};

VulkanTextOverlayCompositor::VulkanTextOverlayCompositor() {
  const Effects fx = ReadEffects();
  const std::string path = CustomShaderPath();
  std::vector<std::uint32_t> custom = ReadSpirv(path);
  if (!fx.on() && custom.empty()) return;
  impl_ = new Impl;
  impl_->fx = fx;
  impl_->custom = std::move(custom);
  impl_->custom_path = path;
  impl_->custom_stamp = FileStamp(path);
}

VulkanTextOverlayCompositor::~VulkanTextOverlayCompositor() { delete impl_; }

bool VulkanTextOverlayCompositor::RegisterDevice(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                                                 std::uint32_t, const char* const*, std::uint32_t,
                                                 std::uint32_t graphics_queue_family, std::uint32_t,
                                                 const VkPhysicalDeviceFeatures2*,
                                                 PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                                 PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  if (impl_ == nullptr || get_device_proc_addr == nullptr || get_instance_proc_addr == nullptr) return true;
  Device d;
  d.handle = device;
  d.family = graphics_queue_family;
  bool loaded = true;
#define POST_LOAD(name)                                                                     \
  d.f.name = reinterpret_cast<PFN_##name>(get_device_proc_addr(device, #name));             \
  loaded = loaded && d.f.name != nullptr;
  POST_DEVICE_FUNCTIONS(POST_LOAD)
#undef POST_LOAD
  const auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      get_instance_proc_addr(instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!loaded || memory_properties == nullptr) {
    std::fprintf(stderr, "  [post-fx] device functions missing, effects off\n");
    return true;
  }
  memory_properties(physical_device, &d.memory);

  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 0;
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &d.set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  VkShaderModuleCreateInfo vertex_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  vertex_info.codeSize = sizeof kPostVertexShader;
  vertex_info.pCode = kPostVertexShader;
  VkShaderModuleCreateInfo fragment_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  fragment_info.codeSize = sizeof kPostFragmentShader;
  fragment_info.pCode = kPostFragmentShader;
  VkShaderModuleCreateInfo custom_info = fragment_info;
  custom_info.codeSize = impl_->custom.size() * sizeof(std::uint32_t);
  custom_info.pCode = impl_->custom.data();
  if (d.f.vkCreateSampler(device, &sampler_info, nullptr, &d.sampler) != VK_SUCCESS ||
      d.f.vkCreateDescriptorSetLayout(device, &set_info, nullptr, &d.set_layout) != VK_SUCCESS ||
      d.f.vkCreatePipelineLayout(device, &layout_info, nullptr, &d.layout) != VK_SUCCESS ||
      d.f.vkCreateShaderModule(device, &vertex_info, nullptr, &d.vertex) != VK_SUCCESS) {
    std::fprintf(stderr, "  [post-fx] cannot create the effects pipeline objects, effects off\n");
    Impl::DestroyDevice(d);
    return true;
  }
  if (!impl_->custom.empty() && d.f.vkCreateShaderModule(device, &custom_info, nullptr, &d.fragment) != VK_SUCCESS) {
    std::fprintf(stderr, "  [post-fx] the driver refused the custom shader, using the built-in effects\n");
    d.fragment = VK_NULL_HANDLE;
  }
  if (!d.fragment && d.f.vkCreateShaderModule(device, &fragment_info, nullptr, &d.fragment) != VK_SUCCESS) {
    std::fprintf(stderr, "  [post-fx] cannot create the effects pipeline objects, effects off\n");
    Impl::DestroyDevice(d);
    return true;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->devices[device] = d;
  return true;
}

void VulkanTextOverlayCompositor::DestroyDevice(VkDevice device) {
  if (impl_ == nullptr) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->devices.find(device);
  if (found == impl_->devices.end()) return;
  for (auto it = impl_->chains.begin(); it != impl_->chains.end();) {
    if (it->second.device == device) {
      Impl::DestroyChain(found->second, it->second);
      it = impl_->chains.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = impl_->queues.begin(); it != impl_->queues.end();) {
    if (it->second.first == device) it = impl_->queues.erase(it);
    else ++it;
  }
  Impl::DestroyDevice(found->second);
  impl_->devices.erase(found);
}

bool VulkanTextOverlayCompositor::RegisterQueue(VkDevice device, VkQueue queue, std::uint32_t queue_family, std::uint32_t) {
  if (impl_ == nullptr) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->queues[queue] = {device, queue_family};
  return true;
}

bool VulkanTextOverlayCompositor::RegisterSwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkSwapchainCreateInfoKHR& create_info, const VkImage* images,
                                                    std::uint32_t image_count) {
  if (impl_ == nullptr || images == nullptr || image_count == 0) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->devices.find(device);
  if (found == impl_->devices.end()) return false;
  if ((create_info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
    std::fprintf(stderr, "  [post-fx] the swapchain can't be copied from on this driver, effects off\n");
    return false;
  }
  Chain chain;
  if (!Impl::BuildChain(found->second, chain, create_info, images, image_count)) {
    std::fprintf(stderr, "  [post-fx] cannot build the effects pass for this swapchain, effects off\n");
    Impl::DestroyChain(found->second, chain);
    return false;
  }
  const Effects& fx = impl_->fx;
  std::fprintf(stderr,
               "  [post-fx] on for %ux%u: sharpen=%.2f vibrance=%.2f saturation=%.2f contrast=%.2f brightness=%.2f "
               "gamma=%.2f\n",
               create_info.imageExtent.width, create_info.imageExtent.height, fx.sharpen, fx.vibrance, fx.saturation,
               fx.contrast, fx.brightness, fx.gamma);
  impl_->chains[swapchain] = std::move(chain);
  return true;
}

void VulkanTextOverlayCompositor::DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain) {
  if (impl_ == nullptr) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto chain = impl_->chains.find(swapchain);
  const auto found = impl_->devices.find(device);
  if (chain == impl_->chains.end() || found == impl_->devices.end()) return;
  Impl::DestroyChain(found->second, chain->second);
  impl_->chains.erase(chain);
}

VkResult VulkanTextOverlayCompositor::QueueSubmit(VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
                                                  VkFence fence, PFN_vkQueueSubmit fallback) {
  return fallback ? fallback(queue, submit_count, submits, fence) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanTextOverlayCompositor::QueueSubmit2(VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
                                                   VkFence fence, PFN_vkQueueSubmit2 fallback) {
  return fallback ? fallback(queue, submit_count, submits, fence) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanTextOverlayCompositor::QueueBindSparse(VkQueue queue, std::uint32_t bind_info_count,
                                                      const VkBindSparseInfo* bind_info, VkFence fence,
                                                      PFN_vkQueueBindSparse fallback) {
  return fallback ? fallback(queue, bind_info_count, bind_info, fence) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanTextOverlayCompositor::QueueWaitIdle(VkQueue queue, PFN_vkQueueWaitIdle fallback) {
  return fallback ? fallback(queue) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanTextOverlayCompositor::DeviceWaitIdle(VkDevice device, PFN_vkDeviceWaitIdle fallback) {
  return fallback ? fallback(device) : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VulkanTextOverlayCompositor::QueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info,
                                                   PFN_vkQueuePresentKHR fallback) {
  if (fallback == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  if (impl_ == nullptr || present_info == nullptr || present_info->swapchainCount != 1) return fallback(queue, present_info);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->custom_path.empty() && ++impl_->presents % 30 == 0) impl_->ReloadCustomShader();
  const auto queue_entry = impl_->queues.find(queue);
  const auto chain_entry = impl_->chains.find(present_info->pSwapchains[0]);
  if (queue_entry == impl_->queues.end() || chain_entry == impl_->chains.end()) return fallback(queue, present_info);
  const auto device_entry = impl_->devices.find(queue_entry->second.first);
  if (device_entry == impl_->devices.end()) return fallback(queue, present_info);
  const Device& d = device_entry->second;
  const Chain& chain = chain_entry->second;
  const std::uint32_t index = present_info->pImageIndices[0];
  if (queue_entry->second.second != d.family || chain.device != d.handle || index >= chain.frames.size())
    return fallback(queue, present_info);
  const Frame& frame = chain.frames[index];
  if (d.f.vkWaitForFences(d.handle, 1, &frame.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS ||
      !impl_->Record(d, chain, frame))
    return fallback(queue, present_info);
  d.f.vkResetFences(d.handle, 1, &frame.fence);
  std::vector<VkPipelineStageFlags> stages(present_info->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = present_info->waitSemaphoreCount;
  submit.pWaitSemaphores = present_info->pWaitSemaphores;
  submit.pWaitDstStageMask = stages.data();
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.commands;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &frame.done;
  if (d.f.vkQueueSubmit(queue, 1, &submit, frame.fence) != VK_SUCCESS) {
    d.f.vkQueueSubmit(queue, 0, nullptr, frame.fence);
    return fallback(queue, present_info);
  }
  VkPresentInfoKHR present = *present_info;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &frame.done;
  return fallback(queue, &present);
}

}  // namespace graphics
}  // namespace blox7
