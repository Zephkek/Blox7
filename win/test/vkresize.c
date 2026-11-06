// vulkan present across a window resize, "mailbox" for MAILBOX
#define VK_NO_PROTOTYPES 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vulkan/vulkan_core.h>

static PFN_vkGetInstanceProcAddr gipa;

#define CHECK(call)                                                       \
  do {                                                                    \
    VkResult r_ = (call);                                                 \
    if (r_ != VK_SUCCESS) {                                               \
      printf("FAIL %s = %d (line %d)\n", #call, (int)r_, __LINE__);       \
      return 1;                                                           \
    }                                                                     \
  } while (0)

#define LOAD(instance, name) PFN_##name name = (PFN_##name)gipa(instance, #name)

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
  const double magenta_seconds = argc > 1 ? atof(argv[1]) : 12.0;
  const VkPresentModeKHR mode = argc > 2 && argv[2][0] == 'm' ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
  if (!SDL_Init(SDL_INIT_VIDEO)) { printf("FAIL SDL_Init: %s\n", SDL_GetError()); return 1; }
  if (!SDL_Vulkan_LoadLibrary(NULL)) { printf("FAIL SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError()); return 1; }
  gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
  SDL_Window* window = SDL_CreateWindow("vkresize", 640, 360, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
  if (!window) { printf("FAIL SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

  LOAD(NULL, vkCreateInstance);
  Uint32 ext_count = 0;
  const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkresize", 1, "none", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, ext_count, exts};
  VkInstance instance;
  CHECK(vkCreateInstance(&instance_info, NULL, &instance));
  LOAD(instance, vkEnumeratePhysicalDevices);
  LOAD(instance, vkGetPhysicalDeviceProperties);
  LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
  LOAD(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  LOAD(instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
  LOAD(instance, vkCreateDevice);
  LOAD(instance, vkGetDeviceProcAddr);
  VkSurfaceKHR surface;
  if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) { printf("FAIL surface: %s\n", SDL_GetError()); return 1; }
  uint32_t count = 1;
  VkPhysicalDevice physical;
  vkEnumeratePhysicalDevices(instance, &count, &physical);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical, &props);
  printf("device %s, type %d, API %u.%u.%u\n", props.deviceName, (int)props.deviceType, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
  uint32_t family = 0;
  for (uint32_t i = 0; i < families; i++) {
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &ok);
    if (ok) { family = i; break; }
  }
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority};
  const char* device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info, 0, NULL, 1, device_exts, NULL};
  VkDevice device;
  CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
  DLOAD(vkGetDeviceQueue);
  DLOAD(vkCreateSwapchainKHR);
  DLOAD(vkDestroySwapchainKHR);
  DLOAD(vkGetSwapchainImagesKHR);
  DLOAD(vkAcquireNextImageKHR);
  DLOAD(vkQueuePresentKHR);
  DLOAD(vkCreateCommandPool);
  DLOAD(vkAllocateCommandBuffers);
  DLOAD(vkBeginCommandBuffer);
  DLOAD(vkEndCommandBuffer);
  DLOAD(vkResetCommandBuffer);
  DLOAD(vkCmdPipelineBarrier);
  DLOAD(vkCmdClearColorImage);
  DLOAD(vkQueueSubmit);
  DLOAD(vkCreateSemaphore);
  DLOAD(vkCreateFence);
  DLOAD(vkWaitForFences);
  DLOAD(vkResetFences);
  DLOAD(vkDeviceWaitIdle);
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                       VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
  VkCommandBufferAllocateInfo cb_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb;
  CHECK(vkAllocateCommandBuffers(device, &cb_info, &cb));
  VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired, rendered;
  CHECK(vkCreateSemaphore(device, &sem_info, NULL, &acquired));
  CHECK(vkCreateSemaphore(device, &sem_info, NULL, &rendered));
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
  VkFence fence;
  CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkImage image[8];
  uint32_t images = 0;
  int recreated = 0, frames[2] = {0, 0}, out_of_date = 0, suboptimal = 0;
  int phase = 0;
  const double start = now_s();
  double phase_start = start;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
    }
    const double t = now_s();
    if (phase == 0 && t - phase_start > 2.0) {
      SDL_MaximizeWindow(window);
      SDL_SyncWindow(window);
      phase = 1;
      phase_start = now_s();
    } else if (phase == 1 && t - phase_start > magenta_seconds) {
      break;
    }
    if (swapchain == VK_NULL_HANDLE || out_of_date) {
      CHECK(vkDeviceWaitIdle(device));
      VkSurfaceCapabilitiesKHR caps;
      CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
      VkSurfaceFormatKHR format[16];
      uint32_t formats = 16;
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, format);
      VkSwapchainCreateInfoKHR swap_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
      swap_info.surface = surface;
      swap_info.minImageCount = caps.minImageCount + 1;
      swap_info.imageFormat = format[0].format;
      swap_info.imageColorSpace = format[0].colorSpace;
      swap_info.imageExtent = caps.currentExtent;
      swap_info.imageArrayLayers = 1;
      swap_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      swap_info.preTransform = caps.currentTransform;
      swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      swap_info.presentMode = mode;
      swap_info.clipped = VK_TRUE;
      swap_info.oldSwapchain = swapchain;
      VkSwapchainKHR next;
      CHECK(vkCreateSwapchainKHR(device, &swap_info, NULL, &next));
      if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, NULL);
      swapchain = next;
      images = 8;
      CHECK(vkGetSwapchainImagesKHR(device, swapchain, &images, image));
      printf("%.2f s: swapchain %ux%u, %u images (window %s)\n", now_s() - start, swap_info.imageExtent.width,
             swap_info.imageExtent.height, images, recreated ? "resized" : "new");
      recreated++;
      out_of_date = 0;
    }
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    uint32_t index;
    VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) { out_of_date = 1; continue; }
    if (acquire == VK_SUBOPTIMAL_KHR) suboptimal++;
    else if (acquire != VK_SUCCESS) { printf("FAIL acquire %d\n", (int)acquire); return 1; }
    CHECK(vkResetFences(device, 1, &fence));
    CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &begin));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image[index], range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    VkClearColorValue colour = {{phase ? 1.0f : 0.0f, phase ? 0.0f : 1.0f, phase ? 1.0f : 0.0f, 1.0f}};
    vkCmdClearColorImage(cb, image[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                         &to_present);
    CHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &wait_stage, 1, &cb, 1, &rendered};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered, 1, &swapchain, &index, NULL};
    VkResult presented = vkQueuePresentKHR(queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR) out_of_date = 1;
    else if (presented == VK_SUBOPTIMAL_KHR) { suboptimal++; out_of_date = 1; }
    else if (presented != VK_SUCCESS) { printf("FAIL present %d\n", (int)presented); return 1; }
    frames[phase]++;
  }
  vkDeviceWaitIdle(device);
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window, &w, &h);
  printf("green frames %d, magenta frames %d, swapchains %d, suboptimal %d, window now %dx%d\n", frames[0], frames[1],
         recreated, suboptimal, w, h);
  printf("DONE\n");
  return 0;
}
