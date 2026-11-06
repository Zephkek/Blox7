// vulkan bridge smoke test, debug messenger and allocator callbacks included
#define VK_NO_PROTOTYPES 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan_core.h>

static PFN_vkGetInstanceProcAddr gipa;
static volatile int g_allocations, g_frees, g_messages, g_bad_thread;

#define CHECK(call)                                                       \
  do {                                                                    \
    VkResult r_ = (call);                                                 \
    if (r_ != VK_SUCCESS) {                                               \
      printf("FAIL %s = %d (line %d)\n", #call, (int)r_, __LINE__);       \
      return 1;                                                           \
    }                                                                     \
  } while (0)

static void* VKAPI_PTR allocate(void* user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
  (void)scope;
  if (user != (void*)0x1234 || pthread_self() == 0) g_bad_thread = 1;
  if (alignment < 16) alignment = 16;
  char* raw = malloc(size + alignment + 16);
  if (!raw) return NULL;
  char* p = (char*)(((uintptr_t)raw + 16 + alignment - 1) & ~(uintptr_t)(alignment - 1));
  ((void**)p)[-1] = raw;
  ((size_t*)p)[-2] = size;
  __atomic_add_fetch(&g_allocations, 1, __ATOMIC_RELAXED);
  return p;
}
static void VKAPI_PTR release(void* user, void* memory) {
  (void)user;
  if (!memory) return;
  __atomic_add_fetch(&g_frees, 1, __ATOMIC_RELAXED);
  free(((void**)memory)[-1]);
}
static void* VKAPI_PTR reallocate(void* user, void* original, size_t size, size_t alignment, VkSystemAllocationScope scope) {
  void* p = allocate(user, size, alignment, scope);
  if (p && original) {
    size_t old = ((size_t*)original)[-2];
    memcpy(p, original, old < size ? old : size);
    release(user, original);
  }
  return p;
}

static VkBool32 VKAPI_PTR on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  (void)types;
  if (user != (void*)0x5678) g_bad_thread = 1;
  g_messages++;
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) printf("  message: %s\n", data->pMessage);
  return VK_FALSE;
}

#define LOAD(instance, name) PFN_##name name = (PFN_##name)gipa(instance, #name)

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) { printf("FAIL SDL_Init: %s\n", SDL_GetError()); return 1; }
  if (!SDL_Vulkan_LoadLibrary(NULL)) { printf("FAIL SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError()); return 1; }
  gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
  if (!gipa) { printf("FAIL no vkGetInstanceProcAddr\n"); return 1; }
  SDL_Window* window = SDL_CreateWindow("vktest", 640, 360, SDL_WINDOW_VULKAN);
  if (!window) { printf("FAIL SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

  LOAD(NULL, vkEnumerateInstanceVersion);
  LOAD(NULL, vkEnumerateInstanceExtensionProperties);
  LOAD(NULL, vkCreateInstance);
  uint32_t version = 0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
  printf("loader instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
         VK_API_VERSION_PATCH(version));
  uint32_t count = 0;
  CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
  VkExtensionProperties* props = calloc(count, sizeof *props);
  CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, props));
  int debug_utils = 0;
  for (uint32_t i = 0; i < count; i++) debug_utils |= strcmp(props[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
  printf("%u instance extensions, debug utils %d\n", count, debug_utils);

  Uint32 sdl_count = 0;
  const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
  const char* extensions[16];
  uint32_t n = 0;
  for (Uint32 i = 0; i < sdl_count && n < 15; i++) extensions[n++] = sdl_extensions[i];
  if (debug_utils) extensions[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

  VkAllocationCallbacks allocator = {(void*)0x1234, allocate, reallocate, release, NULL, NULL};
  VkDebugUtilsMessengerCreateInfoEXT messenger_info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  messenger_info.messageSeverity = 0x1111;
  messenger_info.messageType = 0x7;
  messenger_info.pfnUserCallback = on_message;
  messenger_info.pUserData = (void*)0x5678;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, "blox7", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pNext = debug_utils ? &messenger_info : NULL;
  instance_info.pApplicationInfo = &app;
  instance_info.enabledExtensionCount = n;
  instance_info.ppEnabledExtensionNames = extensions;
  VkInstance instance;
  CHECK(vkCreateInstance(&instance_info, &allocator, &instance));
  printf("instance created (allocations so far %d)\n", g_allocations);

  LOAD(instance, vkDestroyInstance);
  LOAD(instance, vkEnumeratePhysicalDevices);
  LOAD(instance, vkGetPhysicalDeviceProperties);
  LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
  LOAD(instance, vkCreateDevice);
  LOAD(instance, vkGetDeviceProcAddr);
  LOAD(instance, vkCreateDebugUtilsMessengerEXT);
  LOAD(instance, vkDestroyDebugUtilsMessengerEXT);
  LOAD(instance, vkDestroySurfaceKHR);
  LOAD(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  LOAD(instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
  LOAD(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  if (debug_utils) CHECK(vkCreateDebugUtilsMessengerEXT(instance, &messenger_info, &allocator, &messenger));

  uint32_t devices = 0;
  CHECK(vkEnumeratePhysicalDevices(instance, &devices, NULL));
  VkPhysicalDevice physical[8];
  if (devices > 8) devices = 8;
  CHECK(vkEnumeratePhysicalDevices(instance, &devices, physical));
  for (uint32_t i = 0; i < devices; i++) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(physical[i], &p);
    printf("device %u: %s, API %u.%u.%u, vendor %04x device %04x, type %d\n", i, p.deviceName,
           VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
           p.vendorID, p.deviceID, (int)p.deviceType);
  }
  if (devices == 0) { printf("FAIL no devices\n"); return 1; }

  VkSurfaceKHR surface;
  if (!SDL_Vulkan_CreateSurface(window, instance, &allocator, &surface)) {
    printf("FAIL SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    return 1;
  }
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &families, NULL);
  VkQueueFamilyProperties family_props[16];
  if (families > 16) families = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &families, family_props);
  uint32_t family = UINT32_MAX;
  for (uint32_t i = 0; i < families && family == UINT32_MAX; i++) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical[0], i, surface, &present);
    if ((family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = i;
  }
  if (family == UINT32_MAX) { printf("FAIL no graphics queue that presents\n"); return 1; }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority};
  const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = 1;
  device_info.ppEnabledExtensionNames = device_extensions;
  VkDevice device;
  CHECK(vkCreateDevice(physical[0], &device_info, &allocator, &device));
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
  DLOAD(vkDestroyDevice);
  DLOAD(vkGetDeviceQueue);
  DLOAD(vkCreateSwapchainKHR);
  DLOAD(vkDestroySwapchainKHR);
  DLOAD(vkGetSwapchainImagesKHR);
  DLOAD(vkAcquireNextImageKHR);
  DLOAD(vkQueuePresentKHR);
  DLOAD(vkCreateCommandPool);
  DLOAD(vkDestroyCommandPool);
  DLOAD(vkAllocateCommandBuffers);
  DLOAD(vkBeginCommandBuffer);
  DLOAD(vkEndCommandBuffer);
  DLOAD(vkCmdPipelineBarrier);
  DLOAD(vkCmdClearColorImage);
  DLOAD(vkQueueSubmit);
  DLOAD(vkCreateSemaphore);
  DLOAD(vkDestroySemaphore);
  DLOAD(vkCreateFence);
  DLOAD(vkDestroyFence);
  DLOAD(vkWaitForFences);
  DLOAD(vkResetFences);
  DLOAD(vkDeviceWaitIdle);
  DLOAD(vkResetCommandBuffer);
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);

  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical[0], surface, &caps));
  uint32_t formats = 0;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surface, &formats, NULL));
  VkSurfaceFormatKHR format[32];
  if (formats > 32) formats = 32;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical[0], surface, &formats, format));
  VkSwapchainCreateInfoKHR swap_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swap_info.surface = surface;
  swap_info.minImageCount = caps.minImageCount + 1;
  swap_info.imageFormat = format[0].format;
  swap_info.imageColorSpace = format[0].colorSpace;
  swap_info.imageExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){640, 360};
  swap_info.imageArrayLayers = 1;
  swap_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swap_info.preTransform = caps.currentTransform;
  swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swap_info.clipped = VK_TRUE;
  VkSwapchainKHR swapchain;
  CHECK(vkCreateSwapchainKHR(device, &swap_info, &allocator, &swapchain));
  uint32_t images = 0;
  CHECK(vkGetSwapchainImagesKHR(device, swapchain, &images, NULL));
  VkImage image[8];
  if (images > 8) images = 8;
  CHECK(vkGetSwapchainImagesKHR(device, swapchain, &images, image));
  printf("swapchain %ux%u, %u images, format %d\n", swap_info.imageExtent.width, swap_info.imageExtent.height, images,
         (int)swap_info.imageFormat);

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

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  const int frames = 240;
  for (int f = 0; f < frames; f++) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
    }
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(device, 1, &fence));
    uint32_t index;
    VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) { printf("FAIL acquire %d\n", (int)acquire); return 1; }
    CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &begin));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image[index], range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &to_dst);
    const float t = (float)f / frames;
    VkClearColorValue colour = {{t, 0.3f, 1.0f - t, 1.0f}};
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
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) { printf("FAIL present %d\n", (int)presented); return 1; }
  }
  CHECK(vkDeviceWaitIdle(device));
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double seconds = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  printf("%d frames presented in %.2f s (%.1f per second)\n", frames, seconds, frames / seconds);

  vkDestroyFence(device, fence, NULL);
  vkDestroySemaphore(device, acquired, NULL);
  vkDestroySemaphore(device, rendered, NULL);
  vkDestroyCommandPool(device, pool, NULL);
  vkDestroySwapchainKHR(device, swapchain, &allocator);
  vkDestroyDevice(device, &allocator);
  SDL_Vulkan_DestroySurface(instance, surface, &allocator);
  if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, &allocator);
  vkDestroyInstance(instance, &allocator);
  printf("allocation callbacks: %d allocations, %d frees; %d debug messages; callback data %s\n", g_allocations, g_frees,
         g_messages, g_bad_thread ? "WRONG" : "ok");
  printf("PASS\n");
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
