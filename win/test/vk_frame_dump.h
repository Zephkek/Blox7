#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VKDUMP_SLOTS 16

PFN_vkVoidFunction vkb_m_vkGetInstanceProcAddr(VkInstance instance, const char* name);
PFN_vkVoidFunction vkb_m_vkGetDeviceProcAddr(VkDevice device, const char* name);

struct vkdump_real {
  VkResult (*present)(VkQueue, const VkPresentInfoKHR*);
  VkResult (*create_swapchain)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
  void (*destroy_swapchain)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
  void (*get_queue)(VkDevice, uint32_t, uint32_t, VkQueue*);
  void (*get_queue2)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
};

struct vkdump_chain {
  VkSwapchainKHR swapchain;
  VkDevice device;
  VkFormat format;
  uint32_t width, height;
  VkImageUsageFlags usage;
};

static struct vkdump_real g_vkdump_real[2];
static volatile int g_vkdump_lock;
static struct vkdump_chain g_vkdump_chains[VKDUMP_SLOTS];
static struct {
  VkQueue queue;
  uint32_t family;
} g_vkdump_queues[VKDUMP_SLOTS];
static struct {
  VkDevice device;
  VkPhysicalDevice physical;
} g_vkdump_devices[VKDUMP_SLOTS];
static int g_vkdump_state, g_vkdump_count, g_vkdump_next;
static double g_vkdump_start, g_vkdump_at[32];
static char g_vkdump_dir[512];

static double vkdump_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void vkdump_device(VkPhysicalDevice physical, VkDevice device) {
  static int next;
  lock(&g_vkdump_lock);
  g_vkdump_devices[next].device = device;
  g_vkdump_devices[next].physical = physical;
  next = (next + 1) % VKDUMP_SLOTS;
  unlock(&g_vkdump_lock);
}

static int vkdump_due(char* path, size_t size) {
  if (g_vkdump_state < 0) return 0;
  if (g_vkdump_state == 0) {
    const char* dir = getenv("BLOX7_FRAME_DUMP");
    const char* times = getenv("BLOX7_FRAME_DUMP_AT");
    if (!dir || !*dir || !times) {
      g_vkdump_state = -1;
      return 0;
    }
    snprintf(g_vkdump_dir, sizeof g_vkdump_dir, "%s", dir);
    for (const char* p = times; *p && g_vkdump_count < 32;) {
      g_vkdump_at[g_vkdump_count++] = strtod(p, (char**)&p);
      while (*p == ',' || *p == ' ') p++;
    }
    g_vkdump_start = vkdump_now();
    g_vkdump_state = 1;
  }
  if (g_vkdump_next >= g_vkdump_count || vkdump_now() - g_vkdump_start < g_vkdump_at[g_vkdump_next]) return 0;
  snprintf(path, size, "%s/frame-%03d.bmp", g_vkdump_dir, (int)g_vkdump_at[g_vkdump_next]);
  g_vkdump_next++;
  return 1;
}

static void vkdump_write_bmp(const char* path, const unsigned char* pixels, uint32_t w, uint32_t h, int bgr) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  unsigned int size = 54 + w * h * 4;
  unsigned char head[54] = {'B', 'M'};
  memcpy(head + 2, &size, 4);
  unsigned int v = 54;
  memcpy(head + 10, &v, 4);
  v = 40;
  memcpy(head + 14, &v, 4);
  memcpy(head + 18, &w, 4);
  memcpy(head + 22, &h, 4);
  head[26] = 1;
  head[28] = 32;
  fwrite(head, 1, 54, f);
  unsigned char* row = malloc((size_t)w * 4);
  for (uint32_t y = h; y-- > 0;) {
    const unsigned char* src = pixels + (size_t)y * w * 4;
    for (uint32_t x = 0; x < w; x++) {
      row[x * 4] = src[x * 4 + (bgr ? 0 : 2)];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + (bgr ? 2 : 0)];
      row[x * 4 + 3] = 255;
    }
    fwrite(row, 1, (size_t)w * 4, f);
  }
  free(row);
  fclose(f);
}

#define VKDUMP_FN(name) const PFN_##name name = (PFN_##name)vkb_m_vkGetDeviceProcAddr(device, #name)

static int vkdump_capture(VkQueue queue, const VkPresentInfoKHR* info, const char* path) {
  if (info->swapchainCount == 0 || info->pSwapchains == NULL || info->pImageIndices == NULL ||
      info->waitSemaphoreCount > 8)
    return 0;
  struct vkdump_chain chain = {0};
  uint32_t family = UINT32_MAX;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  lock(&g_vkdump_lock);
  for (int i = 0; i < VKDUMP_SLOTS; i++)
    if (g_vkdump_chains[i].swapchain == info->pSwapchains[0]) chain = g_vkdump_chains[i];
  for (int i = 0; i < VKDUMP_SLOTS; i++)
    if (g_vkdump_queues[i].queue == queue) family = g_vkdump_queues[i].family;
  for (int i = 0; i < VKDUMP_SLOTS; i++)
    if (chain.device != VK_NULL_HANDLE && g_vkdump_devices[i].device == chain.device) physical = g_vkdump_devices[i].physical;
  unlock(&g_vkdump_lock);
  const int bgr = chain.format == VK_FORMAT_B8G8R8A8_UNORM || chain.format == VK_FORMAT_B8G8R8A8_SRGB;
  const int rgb = chain.format == VK_FORMAT_R8G8B8A8_UNORM || chain.format == VK_FORMAT_R8G8B8A8_SRGB;
  const VkInstance instance = __atomic_load_n(&g_instance, __ATOMIC_ACQUIRE);
  if (chain.swapchain == VK_NULL_HANDLE || family == UINT32_MAX || physical == VK_NULL_HANDLE || instance == VK_NULL_HANDLE ||
      (!bgr && !rgb) || !(chain.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    fprintf(stderr, "[frame dump] cannot read this swapchain: format %d usage 0x%x family %u\n", (int)chain.format,
            (unsigned)chain.usage, (unsigned)family);
    return 0;
  }
  const VkDevice device = chain.device;
  const PFN_vkGetPhysicalDeviceMemoryProperties memory_properties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)vkb_m_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
  VKDUMP_FN(vkGetSwapchainImagesKHR);
  VKDUMP_FN(vkCreateBuffer);
  VKDUMP_FN(vkDestroyBuffer);
  VKDUMP_FN(vkGetBufferMemoryRequirements);
  VKDUMP_FN(vkAllocateMemory);
  VKDUMP_FN(vkFreeMemory);
  VKDUMP_FN(vkBindBufferMemory);
  VKDUMP_FN(vkMapMemory);
  VKDUMP_FN(vkUnmapMemory);
  VKDUMP_FN(vkCreateCommandPool);
  VKDUMP_FN(vkDestroyCommandPool);
  VKDUMP_FN(vkAllocateCommandBuffers);
  VKDUMP_FN(vkBeginCommandBuffer);
  VKDUMP_FN(vkEndCommandBuffer);
  VKDUMP_FN(vkCmdPipelineBarrier);
  VKDUMP_FN(vkCmdCopyImageToBuffer);
  VKDUMP_FN(vkCreateFence);
  VKDUMP_FN(vkDestroyFence);
  VKDUMP_FN(vkWaitForFences);
  VKDUMP_FN(vkQueueSubmit);
  if (!memory_properties || !vkGetSwapchainImagesKHR || !vkCreateBuffer || !vkDestroyBuffer || !vkGetBufferMemoryRequirements ||
      !vkAllocateMemory || !vkFreeMemory || !vkBindBufferMemory || !vkMapMemory || !vkUnmapMemory || !vkCreateCommandPool ||
      !vkDestroyCommandPool || !vkAllocateCommandBuffers || !vkBeginCommandBuffer || !vkEndCommandBuffer ||
      !vkCmdPipelineBarrier || !vkCmdCopyImageToBuffer || !vkCreateFence || !vkDestroyFence || !vkWaitForFences || !vkQueueSubmit)
    return 0;
  VkImage images[16];
  uint32_t image_count = 16;
  const VkResult listed = vkGetSwapchainImagesKHR(device, chain.swapchain, &image_count, images);
  const uint32_t index = info->pImageIndices[0];
  if ((listed != VK_SUCCESS && listed != VK_INCOMPLETE) || index >= image_count) return 0;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int consumed = 0;
  const VkDeviceSize size = (VkDeviceSize)chain.width * chain.height * 4;
  const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          .size = size,
                                          .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  if (vkCreateBuffer(device, &buffer_info, NULL, &buffer) != VK_SUCCESS) goto done;
  VkMemoryRequirements need;
  vkGetBufferMemoryRequirements(device, buffer, &need);
  VkPhysicalDeviceMemoryProperties types;
  memory_properties(physical, &types);
  uint32_t type = UINT32_MAX;
  const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < types.memoryTypeCount && type == UINT32_MAX; i++)
    if ((need.memoryTypeBits & (1u << i)) && (types.memoryTypes[i].propertyFlags & wanted) == wanted) type = i;
  const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .allocationSize = need.size,
                                         .memoryTypeIndex = type};
  if (type == UINT32_MAX || vkAllocateMemory(device, &allocate, NULL, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
    goto done;
  const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
  if (vkCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) goto done;
  const VkCommandBufferAllocateInfo commands_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                     .commandPool = pool,
                                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                     .commandBufferCount = 1};
  VkCommandBuffer commands = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &commands_info, &commands) != VK_SUCCESS) goto done;
  const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) goto done;
  VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = images[index],
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier);
  const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {chain.width, chain.height, 1}};
  vkCmdCopyImageToBuffer(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
                       1, &barrier);
  if (vkEndCommandBuffer(commands) != VK_SUCCESS) goto done;
  const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS) goto done;
  VkPipelineStageFlags stages[8];
  for (uint32_t i = 0; i < 8; i++) stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .waitSemaphoreCount = info->waitSemaphoreCount,
                               .pWaitSemaphores = info->pWaitSemaphores,
                               .pWaitDstStageMask = stages,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &commands};
  if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) goto done;
  consumed = 1;
  void* mapped = NULL;
  if (vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS &&
      vkMapMemory(device, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
    vkdump_write_bmp(path, (const unsigned char*)mapped, chain.width, chain.height, bgr);
    vkUnmapMemory(device, memory);
    fprintf(stderr, "[frame dump] %s %ux%u format %d\n", path, chain.width, chain.height, (int)chain.format);
  }
done:
  if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, NULL);
  if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, NULL);
  if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, NULL);
  if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, NULL);
  return consumed;
}

static void vkdump_queue(VkQueue queue, uint32_t family) {
  static int next;
  if (queue == VK_NULL_HANDLE) return;
  lock(&g_vkdump_lock);
  int found = 0;
  for (int i = 0; i < VKDUMP_SLOTS; i++) found |= g_vkdump_queues[i].queue == queue;
  if (!found) {
    g_vkdump_queues[next].queue = queue;
    g_vkdump_queues[next].family = family;
    next = (next + 1) % VKDUMP_SLOTS;
  }
  unlock(&g_vkdump_lock);
}

#define VKDUMP_WRAPPERS(k)                                                                                             \
  static VkResult vkdump_present_##k(VkQueue queue, const VkPresentInfoKHR* info) {                                    \
    char path[600];                                                                                                    \
    if (info && vkdump_due(path, sizeof path) && vkdump_capture(queue, info, path)) {                                  \
      VkPresentInfoKHR copy = *info;                                                                                   \
      copy.waitSemaphoreCount = 0;                                                                                     \
      copy.pWaitSemaphores = NULL;                                                                                     \
      return g_vkdump_real[k].present(queue, &copy);                                                                   \
    }                                                                                                                  \
    return g_vkdump_real[k].present(queue, info);                                                                      \
  }                                                                                                                    \
  static VkResult vkdump_create_swapchain_##k(VkDevice device, const VkSwapchainCreateInfoKHR* info,                   \
                                              const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {    \
    static int next;                                                                                                   \
    VkSwapchainCreateInfoKHR readable;                                                                                 \
    const VkSwapchainCreateInfoKHR* used = info;                                                                       \
    if (info && getenv("BLOX7_FRAME_DUMP")) {                                                                          \
      readable = *info;                                                                                                \
      readable.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;                                                          \
      used = &readable;                                                                                                \
    }                                                                                                                  \
    VkResult result = g_vkdump_real[k].create_swapchain(device, used, allocator, swapchain);                           \
    if (result != VK_SUCCESS && used != info) {                                                                        \
      used = info;                                                                                                     \
      result = g_vkdump_real[k].create_swapchain(device, used, allocator, swapchain);                                  \
    }                                                                                                                  \
    if (result == VK_SUCCESS && used && swapchain) {                                                                   \
      lock(&g_vkdump_lock);                                                                                            \
      g_vkdump_chains[next] = (struct vkdump_chain){*swapchain, device, used->imageFormat, used->imageExtent.width,    \
                                                    used->imageExtent.height, used->imageUsage};                       \
      next = (next + 1) % VKDUMP_SLOTS;                                                                                \
      unlock(&g_vkdump_lock);                                                                                          \
    }                                                                                                                  \
    return result;                                                                                                     \
  }                                                                                                                    \
  static void vkdump_destroy_swapchain_##k(VkDevice device, VkSwapchainKHR swapchain,                                  \
                                           const VkAllocationCallbacks* allocator) {                                  \
    lock(&g_vkdump_lock);                                                                                              \
    for (int i = 0; i < VKDUMP_SLOTS; i++)                                                                             \
      if (swapchain != VK_NULL_HANDLE && g_vkdump_chains[i].swapchain == swapchain)                                    \
        g_vkdump_chains[i] = (struct vkdump_chain){0};                                                                 \
    unlock(&g_vkdump_lock);                                                                                            \
    g_vkdump_real[k].destroy_swapchain(device, swapchain, allocator);                                                  \
  }                                                                                                                    \
  static void vkdump_get_queue_##k(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {                 \
    g_vkdump_real[k].get_queue(device, family, index, queue);                                                          \
    if (queue) vkdump_queue(*queue, family);                                                                           \
  }                                                                                                                    \
  static void vkdump_get_queue2_##k(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {                 \
    g_vkdump_real[k].get_queue2(device, info, queue);                                                                  \
    if (info && queue) vkdump_queue(*queue, info->queueFamilyIndex);                                                   \
  }

VKDUMP_WRAPPERS(0)
VKDUMP_WRAPPERS(1)

#define VKDUMP_ENTRY(command, member) \
  {command, offsetof(struct vkdump_real, member), {(void*)vkdump_##member##_0, (void*)vkdump_##member##_1}}

static PFN_vkVoidFunction vkdump_answer(const char* name, void* real, int k) {
  static const struct {
    const char* name;
    size_t slot;
    void* wrapper[2];
  } table[] = {
      VKDUMP_ENTRY("vkQueuePresentKHR", present),
      VKDUMP_ENTRY("vkCreateSwapchainKHR", create_swapchain),
      VKDUMP_ENTRY("vkDestroySwapchainKHR", destroy_swapchain),
      VKDUMP_ENTRY("vkGetDeviceQueue", get_queue),
      VKDUMP_ENTRY("vkGetDeviceQueue2", get_queue2),
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(table[i].name, name) != 0) continue;
    __atomic_store_n((void**)((char*)&g_vkdump_real[k] + table[i].slot), real, __ATOMIC_RELEASE);
    return (PFN_vkVoidFunction)table[i].wrapper[k];
  }
  return NULL;
}
