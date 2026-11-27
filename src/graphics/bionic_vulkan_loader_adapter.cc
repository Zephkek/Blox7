#include <dlfcn.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "blox7/graphics/android_vulkan_wsi_adapter.h"
#include "blox7/graphics/present_mode_policy.h"
#include "blox7/graphics/vulkan_capture_swapchain.h"
#include "blox7/graphics/vulkan_etc2_emulation.h"
#include "blox7/graphics/vulkan_text_overlay_compositor.h"
#include "blox7/platform/platform_runtime.h"

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* create_info,
                 const VkAllocationCallbacks* allocator, VkInstance* instance);
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance, const void* create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);
VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* property_count,
    VkExtensionProperties* properties);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    std::uint32_t* property_count, VkLayerProperties* properties);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char* name);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device);
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device,
                                            std::uint32_t queue_family_index,
                                            std::uint32_t queue_index,
                                            VkQueue* queue);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* queue_info, VkQueue* queue);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* acquire_info,
    std::uint32_t* image_index);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences,
    VkBool32 wait_all, std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences);
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice device, VkQueryPool query_pool, std::uint32_t first_query,
    std::uint32_t query_count, std::size_t data_size, void* data,
    VkDeviceSize stride, VkQueryResultFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* allocate_info,
    VkCommandBuffer* command_buffers);
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool,
    std::uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers);
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool command_pool,
    const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo* begin_info);
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer);
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue);
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device);
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* present_mode_count, VkPresentModeKHR* present_modes);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physical_device, std::uint32_t queue_family_index,
    VkSurfaceKHR surface, VkBool32* supported);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* format_count, VkSurfaceFormatKHR* formats);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
    std::uint32_t* format_count, VkSurfaceFormat2KHR* formats);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
    VkSurfaceCapabilities2KHR* capabilities);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* image_count,
    VkImage* images);
VKAPI_ATTR VkResult VKAPI_CALL vkGetRefreshCycleDurationGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain,
    VkRefreshCycleDurationGOOGLE* display_timing_properties);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain,
    std::uint32_t* presentation_timing_count,
    VkPastPresentationTimingGOOGLE* presentation_timings);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* device_count,
    VkPhysicalDevice* devices);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, std::uint32_t* group_count,
    VkPhysicalDeviceGroupProperties* groups);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* layer_name,
    std::uint32_t* property_count, VkExtensionProperties* properties);
}

namespace {

using BackendWindowFn = void* (*)();
using UsesDirectVulkanFn = bool (*)();
using NotePresentFn = void (*)();
using NoteHostPresentBeginFn = void (*)();
using NoteHostPresentEndFn = void (*)(std::int32_t);
using NoteVulkanCallBeginFn = std::uint64_t (*)(const char*);
using NoteVulkanCallEndFn = void (*)(std::uint64_t, std::int32_t);
using NoteSurfaceOutOfDateFn = void (*)();
using WindowHoldsPresentsFn = bool (*)();
using WindowDimensionFn = int (*)();

constexpr char kFullScreenExclusiveExtension[] = "VK_EXT_full_screen_exclusive";
constexpr std::int32_t kFullScreenExclusiveDisallowed = 2;

struct SurfaceFullScreenExclusiveInfo {
  VkStructureType sType;
  const void* pNext;
  std::int32_t fullScreenExclusive;
};
static_assert(sizeof(SurfaceFullScreenExclusiveInfo) == 24,
              "VkSurfaceFullScreenExclusiveInfoEXT");

struct AdapterState {
  struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    bool full_screen_exclusive = false;
    VkQueue spare_queue = VK_NULL_HANDLE;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueSubmit2 queue_submit2 = nullptr;
    PFN_vkQueueSubmit2KHR queue_submit2_khr = nullptr;
    PFN_vkQueueBindSparse queue_bind_sparse = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkAcquireNextImage2KHR acquire_next_image2 = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkWaitSemaphores wait_semaphores = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_khr = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkResetCommandPool reset_command_pool = nullptr;
    PFN_vkGetQueryPoolResults get_query_pool_results = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkResetCommandBuffer reset_command_buffer = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE get_refresh_cycle_duration = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE get_past_presentation_timing =
        nullptr;
  };

  struct QueueBinding {
    VkQueue queue = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
  };

  struct CommandBufferBinding {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
  };

  std::mutex mutex;
  blox7::graphics::SdlVulkanWsi host_wsi;
  blox7::graphics::AndroidVulkanWsiAdapter android_wsi;
  PFN_vkGetInstanceProcAddr host_get_instance_proc_addr = nullptr;
  std::atomic<PFN_vkGetDeviceProcAddr> host_get_device_proc_addr{nullptr};
  std::vector<DeviceDispatch> device_dispatches;
  std::vector<QueueBinding> queue_bindings;
  std::vector<CommandBufferBinding> command_buffer_bindings;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_surface_capabilities =
      nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_surface_present_modes =
      nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR host_surface_support = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR host_surface_formats = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormats2KHR host_surface_formats2 = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR host_surface_capabilities2 =
      nullptr;
  VkInstance latest_instance = VK_NULL_HANDLE;
  std::unordered_set<VkInstance> full_screen_exclusive_instances;
  size_t active_instance_count = 0;
  VkInstance kept_instance = VK_NULL_HANDLE;
  std::string kept_instance_key;
  VkSurfaceKHR kept_surface = VK_NULL_HANDLE;
  VkInstance kept_surface_instance = VK_NULL_HANDLE;
  VkDevice kept_device = VK_NULL_HANDLE;
  std::string kept_device_key;
  VkSwapchainKHR retired_swapchain = VK_NULL_HANDLE;
  VkDevice retired_swapchain_device = VK_NULL_HANDLE;
  VkSurfaceKHR retired_swapchain_surface = VK_NULL_HANDLE;
  std::vector<VkSurfaceKHR> lost_surfaces;
  std::uint64_t lost_surface_noted_ns = 0;
  std::vector<VkDevice> lost_devices;
  std::uint64_t lost_device_noted_ns = 0;
  std::uint64_t lost_device_times_ns[4] = {};
  std::unordered_map<VkInstance, std::string> instance_keys;
  std::unordered_map<VkPhysicalDevice, VkInstance> physical_device_instances;
  std::unordered_map<VkDevice, std::string> device_keys;
  std::unordered_map<VkDevice, std::int64_t> device_allocations;
  std::unordered_map<VkSwapchainKHR, VkSurfaceKHR> swapchain_surfaces;
  std::unordered_map<VkSwapchainKHR, std::vector<std::uint32_t>>
      hidden_images;
  std::atomic<std::size_t> hidden_image_count{0};
  std::mutex spare_queue_mutex;
  VkSurfaceKHR shown_surface = VK_NULL_HANDLE;
  VkSurfaceCapabilitiesKHR shown_capabilities{};
  blox7::graphics::VulkanTextOverlayCompositor text_overlay;
  blox7::graphics::VulkanCaptureSwapchains capture;
  blox7::graphics::VulkanEtc2Emulation etc2;
  std::atomic<NotePresentFn> note_present{nullptr};
  std::atomic<NoteHostPresentBeginFn> note_host_present_begin{nullptr};
  std::atomic<NoteHostPresentEndFn> note_host_present_end{nullptr};
  std::atomic<NoteVulkanCallBeginFn> note_vulkan_call_begin{nullptr};
  std::atomic<NoteVulkanCallEndFn> note_vulkan_call_end{nullptr};
  std::atomic<NoteSurfaceOutOfDateFn> note_surface_out_of_date{nullptr};
  std::atomic<WindowHoldsPresentsFn> window_holds_presents{nullptr};
  std::atomic<WindowHoldsPresentsFn> window_hides_presents{nullptr};
  bool extent_translation_logged = false;
  bool present_policy_logged = false;
  bool initialized = false;
};

static std::atomic<bool> g_vulkan_call_observation_active{false};

AdapterState& State() {
  static AdapterState state;
  return state;
}

template <typename Function>
Function ResolveProcessFunction(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, name));
}

void WaitWhileWindowHoldsPresents() {
  const WindowHoldsPresentsFn holds =
      State().window_holds_presents.load(std::memory_order_acquire);
  if (holds == nullptr) {
    return;
  }
  const timespec pause{0, 1000000};
  while (holds()) {
    nanosleep(&pause, nullptr);
  }
}

bool EnsureInitialized() {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.initialized) {
    return true;
  }

  const BackendWindowFn backend_window =
      ResolveProcessFunction<BackendWindowFn>("blox7_window_backend_window");
  const UsesDirectVulkanFn uses_direct_vulkan =
      ResolveProcessFunction<UsesDirectVulkanFn>(
          "blox7_window_uses_direct_vulkan");
  state.note_present.store(ResolveProcessFunction<NotePresentFn>(
                                "blox7_window_note_vulkan_present"),
                           std::memory_order_release);
  state.note_host_present_begin.store(
      ResolveProcessFunction<NoteHostPresentBeginFn>(
          "blox7_window_note_vulkan_host_present_begin"),
      std::memory_order_release);
  state.note_host_present_end.store(
      ResolveProcessFunction<NoteHostPresentEndFn>(
          "blox7_window_note_vulkan_host_present_end"),
      std::memory_order_release);
  const auto call_begin = ResolveProcessFunction<NoteVulkanCallBeginFn>(
      "blox7_window_note_vulkan_call_begin");
  const auto call_end = ResolveProcessFunction<NoteVulkanCallEndFn>(
      "blox7_window_note_vulkan_call_end");
  state.note_vulkan_call_begin.store(call_begin, std::memory_order_release);
  state.note_vulkan_call_end.store(call_end, std::memory_order_release);
  g_vulkan_call_observation_active.store(
      call_begin != nullptr && call_end != nullptr, std::memory_order_release);
  state.note_surface_out_of_date.store(
      ResolveProcessFunction<NoteSurfaceOutOfDateFn>(
          "blox7_window_note_vulkan_surface_out_of_date"),
      std::memory_order_release);
  state.window_holds_presents.store(
      ResolveProcessFunction<WindowHoldsPresentsFn>(
          "blox7_window_holds_presents"),
      std::memory_order_release);
  state.window_hides_presents.store(
      ResolveProcessFunction<WindowHoldsPresentsFn>(
          "blox7_window_hides_presents"),
      std::memory_order_release);
  if (backend_window == nullptr || uses_direct_vulkan == nullptr ||
      !uses_direct_vulkan() || backend_window() == nullptr) {
    std::fprintf(stderr,
                 "  [vulkan] direct SDL Vulkan window is unavailable\n");
    return false;
  }

  blox7::platform::NativeWindowDescriptor descriptor;
  descriptor.surface_api = blox7::platform::WindowSurfaceApi::kDirectVulkan;
  descriptor.backend_window = backend_window();
  blox7::Status status = state.host_wsi.Initialize(descriptor);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] SDL WSI initialization failed: %s\n",
                 status.message().c_str());
    return false;
  }
  status = state.android_wsi.Initialize(&state.host_wsi);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] Android WSI initialization failed: %s\n",
                 status.message().c_str());
    state.host_wsi.Shutdown();
    return false;
  }
  state.host_get_instance_proc_addr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          state.host_wsi.GetInstanceProcAddress());
  if (state.host_get_instance_proc_addr == nullptr) {
    state.android_wsi.Shutdown();
    state.host_wsi.Shutdown();
    return false;
  }
  state.initialized = true;
  std::fprintf(stderr, "  [vulkan] Android WSI -> SDL3 host adapter ready\n");
  return true;
}

void NoteSuboptimalTranslation() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr,
                 "  [vulkan] accepted VK_SUBOPTIMAL_KHR; the acquired image "
                 "remains usable\n");
  }
}

VkResult NormalizeSwapchainResult(VkResult result) {
  if (result == VK_SUBOPTIMAL_KHR) {
    NoteSuboptimalTranslation();
  }
  return blox7::graphics::NormalizeAndroidSwapchainResult(result);
}

void NoteSurfaceOutOfDate() {
  const NoteSurfaceOutOfDateFn note_surface_out_of_date =
      State().note_surface_out_of_date.load(std::memory_order_acquire);
  if (note_surface_out_of_date != nullptr) {
    note_surface_out_of_date();
  }
}

void NoteLostSurface(VkSurfaceKHR surface) {
  constexpr std::uint64_t kRepeatNs = 1000000000ULL;
  bool first = false;
  bool due = true;
  if (surface != VK_NULL_HANDLE) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const std::uint64_t now_ns =
        static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(now.tv_nsec);
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    first = std::find(state.lost_surfaces.begin(), state.lost_surfaces.end(),
                      surface) == state.lost_surfaces.end();
    if (first) {
      state.lost_surfaces.push_back(surface);
    }
    due = first || now_ns - state.lost_surface_noted_ns >= kRepeatNs;
    if (due) {
      state.lost_surface_noted_ns = now_ns;
    }
  }
  if (first) {
    std::fprintf(stderr,
                 "  [vulkan] the host lost the window surface "
                 "(VK_ERROR_SURFACE_LOST_KHR); the next one is created new\n");
  }
  if (due) {
    NoteSurfaceOutOfDate();
  }
}

VkResult NoteSurfaceResult(VkSurfaceKHR surface, VkResult result) {
  if (result == VK_ERROR_SURFACE_LOST_KHR) {
    NoteLostSurface(surface);
  }
  return result;
}

VkSurfaceKHR SurfaceOf(const VkPhysicalDeviceSurfaceInfo2KHR* surface_info) {
  return surface_info != nullptr ? surface_info->surface : VK_NULL_HANDLE;
}

void NoteSwapchainResult(VkSwapchainKHR swapchain, VkResult result) {
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    NoteSurfaceOutOfDate();
    return;
  }
  if (result != VK_ERROR_SURFACE_LOST_KHR) {
    return;
  }
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (const auto found = state.swapchain_surfaces.find(swapchain);
        found != state.swapchain_surfaces.end()) {
      surface = found->second;
    }
  }
  NoteLostSurface(surface);
}

extern "C" [[noreturn]] void __w32_fatal(const char* message);

void NoteLostDevice(VkDevice device, bool drawing) {
  constexpr std::uint64_t kRepeatNs = 1000000000ULL;
  constexpr std::uint64_t kGiveUpNs = 60000000000ULL;
  if (device == VK_NULL_HANDLE) {
    return;
  }
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const std::uint64_t now_ns =
      static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(now.tv_nsec);
  bool first = false;
  bool unanswered = false;
  bool fifth = false;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    first = std::find(state.lost_devices.begin(), state.lost_devices.end(),
                      device) == state.lost_devices.end();
    if (first) {
      state.lost_devices.push_back(device);
      state.lost_device_noted_ns = now_ns;
      std::uint64_t* const times = state.lost_device_times_ns;
      fifth = times[0] != 0 && now_ns - times[0] < kGiveUpNs;
      times[0] = times[1];
      times[1] = times[2];
      times[2] = times[3];
      times[3] = now_ns;
    }
    unanswered =
        drawing && now_ns - state.lost_device_noted_ns >= kRepeatNs;
    if (unanswered) {
      state.lost_device_noted_ns = now_ns;
    }
  }
  if (fifth) {
    std::fprintf(stderr,
                 "  [vulkan] the host lost the device for the fifth time in a "
                 "minute (VK_ERROR_DEVICE_LOST); closing\n");
    __w32_fatal("The graphics driver keeps restarting, Roblox can't go on. "
                "Start it again.");
  }
  if (first) {
    std::fprintf(stderr,
                 "  [vulkan] the host lost the device (VK_ERROR_DEVICE_LOST); "
                 "the next one is created new\n");
  }
  if (unanswered) {
    std::fprintf(stderr,
                 "  [vulkan] Roblox still draws on the lost device; asking "
                 "for a new surface so it builds its renderer again\n");
    NoteSurfaceOutOfDate();
  }
}

VkResult NoteDeviceResult(VkDevice device, VkResult result, bool drawing) {
  if (__builtin_expect(result == VK_ERROR_DEVICE_LOST, 0)) {
    NoteLostDevice(device, drawing);
  }
  return result;
}

PFN_vkVoidFunction HostInstanceProc(VkInstance instance, const char* name) {
  if (!EnsureInitialized() || name == nullptr) {
    return nullptr;
  }
  return State().host_get_instance_proc_addr(instance, name);
}

PFN_vkVoidFunction HostDeviceProc(VkDevice device, const char* name) {
  if (!EnsureInitialized() || name == nullptr) {
    return nullptr;
  }
  const PFN_vkGetDeviceProcAddr host_get_device_proc_addr =
      State().host_get_device_proc_addr.load(std::memory_order_acquire);
  return host_get_device_proc_addr != nullptr
             ? host_get_device_proc_addr(device, name)
             : nullptr;
}

static std::atomic<uint64_t> g_dispatch_generation{1};

struct ThreadQueueDispatchCache {
  uint64_t generation = 0;
  VkQueue queue = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadQueueDispatchCache t_queue_cache;

struct ThreadDeviceDispatchCache {
  uint64_t generation = 0;
  VkDevice device = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadDeviceDispatchCache t_device_cache;

struct ThreadCommandBufferDispatchCache {
  uint64_t generation = 0;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadCommandBufferDispatchCache t_command_buffer_cache;

void InvalidateFastDispatchCaches() {
  g_dispatch_generation.fetch_add(1, std::memory_order_release);
}

static const AdapterState::DeviceDispatch kEmptyDeviceDispatch{};

const AdapterState::DeviceDispatch& HostDispatchForQueue(VkQueue queue) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(queue != VK_NULL_HANDLE &&
                           t_queue_cache.queue == queue &&
                           t_queue_cache.generation == gen,
                       1)) {
    return t_queue_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding =
      std::find_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                   [queue](const AdapterState::QueueBinding& candidate) {
                     return candidate.queue == queue;
                   });
  if (binding == state.queue_bindings.end()) {
    return kEmptyDeviceDispatch;
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_queue_cache.generation = gen;
    t_queue_cache.queue = queue;
    t_queue_cache.dispatch = *dispatch;
    return t_queue_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

const AdapterState::DeviceDispatch& HostDispatchForDevice(VkDevice device) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(device != VK_NULL_HANDLE &&
                           t_device_cache.device == device &&
                           t_device_cache.generation == gen,
                       1)) {
    return t_device_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [device](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_device_cache.generation = gen;
    t_device_cache.device = device;
    t_device_cache.dispatch = *dispatch;
    return t_device_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

const AdapterState::DeviceDispatch& HostDispatchForCommandBuffer(
    VkCommandBuffer command_buffer) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(command_buffer != VK_NULL_HANDLE &&
                           t_command_buffer_cache.command_buffer ==
                               command_buffer &&
                           t_command_buffer_cache.generation == gen,
                       1)) {
    return t_command_buffer_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding = std::find_if(
      state.command_buffer_bindings.begin(),
      state.command_buffer_bindings.end(),
      [command_buffer](const AdapterState::CommandBufferBinding& candidate) {
        return candidate.command_buffer == command_buffer;
      });
  if (binding == state.command_buffer_bindings.end()) {
    return kEmptyDeviceDispatch;
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_command_buffer_cache.generation = gen;
    t_command_buffer_cache.command_buffer = command_buffer;
    t_command_buffer_cache.dispatch = *dispatch;
    return t_command_buffer_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

void RegisterHostDeviceDispatch(VkDevice device,
                                VkPhysicalDevice physical_device,
                                PFN_vkGetDeviceProcAddr get_device_proc_addr,
                                bool full_screen_exclusive,
                                VkQueue spare_queue) {
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  AdapterState::DeviceDispatch dispatch;
  dispatch.device = device;
  dispatch.physical_device = physical_device;
  dispatch.full_screen_exclusive = full_screen_exclusive;
  dispatch.spare_queue = spare_queue;
  dispatch.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(
      get_device_proc_addr(device, "vkQueuePresentKHR"));
  dispatch.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(
      get_device_proc_addr(device, "vkQueueSubmit"));
  dispatch.queue_submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(
      get_device_proc_addr(device, "vkQueueSubmit2"));
  dispatch.queue_submit2_khr = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
      get_device_proc_addr(device, "vkQueueSubmit2KHR"));
  dispatch.queue_bind_sparse = reinterpret_cast<PFN_vkQueueBindSparse>(
      get_device_proc_addr(device, "vkQueueBindSparse"));
  dispatch.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(
      get_device_proc_addr(device, "vkQueueWaitIdle"));
  dispatch.acquire_next_image = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
      get_device_proc_addr(device, "vkAcquireNextImageKHR"));
  dispatch.acquire_next_image2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
      get_device_proc_addr(device, "vkAcquireNextImage2KHR"));
  dispatch.wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(
      get_device_proc_addr(device, "vkWaitForFences"));
  dispatch.wait_semaphores = reinterpret_cast<PFN_vkWaitSemaphores>(
      get_device_proc_addr(device, "vkWaitSemaphores"));
  dispatch.wait_semaphores_khr = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
      get_device_proc_addr(device, "vkWaitSemaphoresKHR"));
  dispatch.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      get_device_proc_addr(device, "vkDeviceWaitIdle"));
  dispatch.reset_fences = reinterpret_cast<PFN_vkResetFences>(
      get_device_proc_addr(device, "vkResetFences"));
  dispatch.reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(
      get_device_proc_addr(device, "vkResetCommandPool"));
  dispatch.get_query_pool_results =
      reinterpret_cast<PFN_vkGetQueryPoolResults>(
          get_device_proc_addr(device, "vkGetQueryPoolResults"));
  dispatch.allocate_command_buffers =
      reinterpret_cast<PFN_vkAllocateCommandBuffers>(
          get_device_proc_addr(device, "vkAllocateCommandBuffers"));
  dispatch.free_command_buffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
      get_device_proc_addr(device, "vkFreeCommandBuffers"));
  dispatch.destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      get_device_proc_addr(device, "vkDestroyCommandPool"));
  dispatch.begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      get_device_proc_addr(device, "vkBeginCommandBuffer"));
  dispatch.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
      get_device_proc_addr(device, "vkEndCommandBuffer"));
  dispatch.reset_command_buffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
      get_device_proc_addr(device, "vkResetCommandBuffer"));
  dispatch.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(
      get_device_proc_addr(device, "vkAllocateMemory"));
  dispatch.get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
      get_device_proc_addr(device, "vkGetSwapchainImagesKHR"));
  dispatch.get_refresh_cycle_duration =
      reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
          get_device_proc_addr(device, "vkGetRefreshCycleDurationGOOGLE"));
  dispatch.get_past_presentation_timing =
      reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
          get_device_proc_addr(device, "vkGetPastPresentationTimingGOOGLE"));

  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto existing = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [device](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == device;
      });
  if (existing != state.device_dispatches.end()) {
    *existing = dispatch;
  } else {
    state.device_dispatches.push_back(dispatch);
  }
  InvalidateFastDispatchCaches();
}

void PaceHiddenPresents() {
  static std::atomic<std::uint64_t> next_ns{0};
  constexpr std::uint64_t kFrameNs = 16666667;
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const std::uint64_t now_ns =
      static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(now.tv_nsec);
  const std::uint64_t due = next_ns.load(std::memory_order_relaxed);
  if (due > now_ns && due - now_ns <= kFrameNs) {
    const timespec pause{0, static_cast<long>(due - now_ns)};
    nanosleep(&pause, nullptr);
    next_ns.store(due + kFrameNs, std::memory_order_relaxed);
    return;
  }
  next_ns.store(now_ns + kFrameNs, std::memory_order_relaxed);
}

bool KeepHiddenPresent(VkQueue queue, const VkPresentInfoKHR* present_info,
                       VkResult* result) {
  const WindowHoldsPresentsFn hides =
      State().window_hides_presents.load(std::memory_order_acquire);
  if (hides == nullptr || present_info == nullptr || !hides()) {
    return false;
  }
  const AdapterState::DeviceDispatch& dispatch = HostDispatchForQueue(queue);
  const PFN_vkQueueSubmit host_submit = dispatch.queue_submit;
  if (dispatch.spare_queue == VK_NULL_HANDLE || host_submit == nullptr) {
    return false;
  }
  if (present_info->waitSemaphoreCount != 0) {
    const std::vector<VkPipelineStageFlags> stages(
        present_info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = present_info->waitSemaphoreCount;
    submit.pWaitSemaphores = present_info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    const VkResult status = host_submit(queue, 1, &submit, VK_NULL_HANDLE);
    if (status != VK_SUCCESS) {
      *result = status;
      return true;
    }
  }
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (std::uint32_t index = 0; index < present_info->swapchainCount;
         ++index) {
      state.hidden_images[present_info->pSwapchains[index]].push_back(
          present_info->pImageIndices[index]);
    }
    state.hidden_image_count.fetch_add(present_info->swapchainCount,
                                       std::memory_order_release);
  }
  if (present_info->pResults != nullptr) {
    std::fill(present_info->pResults,
              present_info->pResults + present_info->swapchainCount,
              VK_SUCCESS);
  }
  PaceHiddenPresents();
  *result = VK_SUCCESS;
  return true;
}

bool TakeHiddenImage(VkDevice device, VkSwapchainKHR swapchain,
                     VkSemaphore semaphore, VkFence fence,
                     std::uint32_t* image_index, VkResult* result) {
  AdapterState& state = State();
  if (state.hidden_image_count.load(std::memory_order_acquire) == 0) {
    return false;
  }
  const AdapterState::DeviceDispatch& dispatch = HostDispatchForDevice(device);
  const VkQueue spare_queue = dispatch.spare_queue;
  const PFN_vkQueueSubmit host_submit = dispatch.queue_submit;
  if (spare_queue == VK_NULL_HANDLE || host_submit == nullptr ||
      image_index == nullptr) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.hidden_images.find(swapchain);
    if (found == state.hidden_images.end() || found->second.empty()) {
      return false;
    }
    *image_index = found->second.front();
    found->second.erase(found->second.begin());
    state.hidden_image_count.fetch_sub(1, std::memory_order_release);
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.signalSemaphoreCount = semaphore != VK_NULL_HANDLE ? 1 : 0;
  submit.pSignalSemaphores = &semaphore;
  std::lock_guard<std::mutex> lock(state.spare_queue_mutex);
  *result = host_submit(spare_queue, 1, &submit, fence);
  return true;
}

VkResult NoteQueueResult(VkQueue queue, VkResult result, bool drawing) {
  if (__builtin_expect(result == VK_ERROR_DEVICE_LOST, 0)) {
    NoteLostDevice(HostDispatchForQueue(queue).device, drawing);
  }
  return result;
}

void RegisterHostQueueBinding(VkDevice device, VkQueue queue) {
  if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto existing =
      std::find_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                   [queue](const AdapterState::QueueBinding& candidate) {
                     return candidate.queue == queue;
                   });
  if (existing != state.queue_bindings.end()) {
    existing->device = device;
  } else {
    state.queue_bindings.push_back({queue, device});
  }
  InvalidateFastDispatchCaches();
}

void RegisterHostCommandBuffers(VkDevice device, VkCommandPool command_pool,
                                std::uint32_t command_buffer_count,
                                const VkCommandBuffer* command_buffers) {
  if (device == VK_NULL_HANDLE || command_buffers == nullptr) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (std::uint32_t index = 0; index < command_buffer_count; ++index) {
    const VkCommandBuffer command_buffer = command_buffers[index];
    if (command_buffer == VK_NULL_HANDLE) {
      continue;
    }
    const auto existing = std::find_if(
        state.command_buffer_bindings.begin(),
        state.command_buffer_bindings.end(),
        [command_buffer](const AdapterState::CommandBufferBinding& candidate) {
          return candidate.command_buffer == command_buffer;
        });
    if (existing != state.command_buffer_bindings.end()) {
      existing->device = device;
      existing->command_pool = command_pool;
    } else {
      state.command_buffer_bindings.push_back(
          {command_buffer, device, command_pool});
    }
  }
  InvalidateFastDispatchCaches();
}

void RemoveHostCommandBuffers(VkDevice device, VkCommandPool command_pool,
                              std::uint32_t command_buffer_count,
                              const VkCommandBuffer* command_buffers) {
  if (command_buffers == nullptr || command_buffer_count == 0) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device, command_pool, command_buffer_count, command_buffers](
              const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device &&
                   binding.command_pool == command_pool &&
                   std::find(command_buffers,
                             command_buffers + command_buffer_count,
                             binding.command_buffer) !=
                       command_buffers + command_buffer_count;
          }),
      state.command_buffer_bindings.end());
  InvalidateFastDispatchCaches();
}

void RemoveHostCommandPoolBindings(VkDevice device,
                                   VkCommandPool command_pool) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device, command_pool](
              const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device &&
                   binding.command_pool == command_pool;
          }),
      state.command_buffer_bindings.end());
  InvalidateFastDispatchCaches();
}

void RemoveHostDeviceDispatch(VkDevice device) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.queue_bindings.erase(
      std::remove_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                     [device](const AdapterState::QueueBinding& binding) {
                       return binding.device == device;
                     }),
      state.queue_bindings.end());
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device](const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device;
          }),
      state.command_buffer_bindings.end());
  state.device_dispatches.erase(
      std::remove_if(
          state.device_dispatches.begin(), state.device_dispatches.end(),
          [device](const AdapterState::DeviceDispatch& dispatch) {
            return dispatch.device == device;
          }),
      state.device_dispatches.end());
  InvalidateFastDispatchCaches();
}

bool IsHostWsiExtension(const char* name) {
  if (name == nullptr) {
    return false;
  }
  const auto& extensions = State().host_wsi.required_instance_extensions();
  return std::find(extensions.begin(), extensions.end(), name) !=
         extensions.end();
}

std::vector<VkExtensionProperties> AndroidVisibleExtensions(
    const char* layer_name, VkResult* result) {
  std::vector<VkExtensionProperties> output;
  const auto host_enumerate =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
          HostInstanceProc(VK_NULL_HANDLE,
                           "vkEnumerateInstanceExtensionProperties"));
  if (host_enumerate == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
    return output;
  }
  std::uint32_t count = 0;
  *result = host_enumerate(layer_name, &count, nullptr);
  if (*result != VK_SUCCESS || count == 0) {
    return output;
  }
  output.resize(count);
  *result = host_enumerate(layer_name, &count, output.data());
  if (*result != VK_SUCCESS && *result != VK_INCOMPLETE) {
    output.clear();
    return output;
  }
  output.resize(count);
  if (layer_name != nullptr) {
    return output;
  }

  output.erase(
      std::remove_if(output.begin(), output.end(),
                     [](const auto& property) {
                       return IsHostWsiExtension(property.extensionName) &&
                              std::strcmp(property.extensionName,
                                          VK_KHR_SURFACE_EXTENSION_NAME) != 0;
                     }),
      output.end());
  const bool has_android =
      std::any_of(output.begin(), output.end(), [](const auto& property) {
        return std::strcmp(property.extensionName,
                           blox7::graphics::kAndroidSurfaceExtension) == 0;
      });
  if (!has_android) {
    VkExtensionProperties android{};
    std::strncpy(android.extensionName,
                 blox7::graphics::kAndroidSurfaceExtension,
                 sizeof(android.extensionName) - 1);
    android.specVersion = 6;
    output.push_back(android);
  }
  *result = VK_SUCCESS;
  return output;
}

bool ListsExtension(const std::vector<VkExtensionProperties>& extensions,
                    const char* name) {
  return std::any_of(extensions.begin(), extensions.end(),
                     [name](const VkExtensionProperties& extension) {
                       return std::strcmp(extension.extensionName, name) == 0;
                     });
}

bool HostDeviceExtension(VkInstance instance, VkPhysicalDevice physical_device,
                         const char* name) {
  const auto enumerate =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          HostInstanceProc(instance, "vkEnumerateDeviceExtensionProperties"));
  std::uint32_t count = 0;
  if (enumerate == nullptr ||
      enumerate(physical_device, nullptr, &count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  const VkResult result =
      enumerate(physical_device, nullptr, &count, extensions.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return false;
  }
  extensions.resize(count);
  return ListsExtension(extensions, name);
}

PFN_vkVoidFunction AdapterProc(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
#define BLOX7_VK_PROC(function)                         \
  if (std::strcmp(name, #function) == 0) {                 \
    return reinterpret_cast<PFN_vkVoidFunction>(function); \
  }
  BLOX7_VK_PROC(vkCreateInstance)
  BLOX7_VK_PROC(vkDestroyInstance)
  BLOX7_VK_PROC(vkCreateAndroidSurfaceKHR)
  BLOX7_VK_PROC(vkDestroySurfaceKHR)
  BLOX7_VK_PROC(vkEnumerateInstanceExtensionProperties)
  BLOX7_VK_PROC(vkEnumerateInstanceLayerProperties)
  BLOX7_VK_PROC(vkGetInstanceProcAddr)
  BLOX7_VK_PROC(vkGetDeviceProcAddr)
  BLOX7_VK_PROC(vkCreateDevice)
  BLOX7_VK_PROC(vkDestroyDevice)
  BLOX7_VK_PROC(vkGetDeviceQueue)
  BLOX7_VK_PROC(vkGetDeviceQueue2)
  BLOX7_VK_PROC(vkCreateSwapchainKHR)
  BLOX7_VK_PROC(vkDestroySwapchainKHR)
  BLOX7_VK_PROC(vkGetSwapchainImagesKHR)
  BLOX7_VK_PROC(vkGetRefreshCycleDurationGOOGLE)
  BLOX7_VK_PROC(vkGetPastPresentationTimingGOOGLE)
  BLOX7_VK_PROC(vkAcquireNextImageKHR)
  BLOX7_VK_PROC(vkAcquireNextImage2KHR)
  BLOX7_VK_PROC(vkWaitForFences)
  BLOX7_VK_PROC(vkResetFences)
  BLOX7_VK_PROC(vkResetCommandPool)
  BLOX7_VK_PROC(vkGetQueryPoolResults)
  BLOX7_VK_PROC(vkAllocateCommandBuffers)
  BLOX7_VK_PROC(vkFreeCommandBuffers)
  BLOX7_VK_PROC(vkDestroyCommandPool)
  BLOX7_VK_PROC(vkBeginCommandBuffer)
  BLOX7_VK_PROC(vkEndCommandBuffer)
  BLOX7_VK_PROC(vkResetCommandBuffer)
  BLOX7_VK_PROC(vkWaitSemaphores)
  BLOX7_VK_PROC(vkWaitSemaphoresKHR)
  BLOX7_VK_PROC(vkQueueSubmit)
  BLOX7_VK_PROC(vkQueueSubmit2)
  BLOX7_VK_PROC(vkQueueSubmit2KHR)
  BLOX7_VK_PROC(vkQueueBindSparse)
  BLOX7_VK_PROC(vkQueueWaitIdle)
  BLOX7_VK_PROC(vkDeviceWaitIdle)
  BLOX7_VK_PROC(vkQueuePresentKHR)
  BLOX7_VK_PROC(vkEnumeratePhysicalDevices)
  BLOX7_VK_PROC(vkEnumeratePhysicalDeviceGroups)
  BLOX7_VK_PROC(vkEnumerateDeviceExtensionProperties)
  BLOX7_VK_PROC(vkGetPhysicalDeviceFeatures)
  BLOX7_VK_PROC(vkGetPhysicalDeviceFeatures2)
  BLOX7_VK_PROC(vkGetPhysicalDeviceFormatProperties)
  BLOX7_VK_PROC(vkGetPhysicalDeviceFormatProperties2)
  BLOX7_VK_PROC(vkGetPhysicalDeviceImageFormatProperties)
  BLOX7_VK_PROC(vkGetPhysicalDeviceImageFormatProperties2)
  BLOX7_VK_PROC(vkCreateImage)
  BLOX7_VK_PROC(vkDestroyImage)
  BLOX7_VK_PROC(vkCreateImageView)
  BLOX7_VK_PROC(vkGetDeviceImageMemoryRequirements)
  BLOX7_VK_PROC(vkGetDeviceImageSparseMemoryRequirements)
  BLOX7_VK_PROC(vkGetDeviceImageSubresourceLayout)
  BLOX7_VK_PROC(vkBindBufferMemory)
  BLOX7_VK_PROC(vkBindBufferMemory2)
  BLOX7_VK_PROC(vkMapMemory)
  BLOX7_VK_PROC(vkMapMemory2)
  BLOX7_VK_PROC(vkUnmapMemory)
  BLOX7_VK_PROC(vkUnmapMemory2)
  BLOX7_VK_PROC(vkAllocateMemory)
  BLOX7_VK_PROC(vkFreeMemory)
  BLOX7_VK_PROC(vkDestroyBuffer)
  BLOX7_VK_PROC(vkCmdCopyBufferToImage)
  BLOX7_VK_PROC(vkCmdCopyImage)
  BLOX7_VK_PROC(vkCmdCopyImage2)
  BLOX7_VK_PROC(vkCmdCopyImageToBuffer)
  BLOX7_VK_PROC(vkCmdCopyImageToBuffer2)
  BLOX7_VK_PROC(vkCmdCopyBufferToImage2)
  BLOX7_VK_PROC(vkCmdExecuteCommands)
#undef BLOX7_VK_PROC
#define BLOX7_VK_ALIAS(alias, function)                 \
  if (std::strcmp(name, #alias) == 0) {                    \
    return reinterpret_cast<PFN_vkVoidFunction>(function); \
  }
  BLOX7_VK_ALIAS(vkEnumeratePhysicalDeviceGroupsKHR,
                    vkEnumeratePhysicalDeviceGroups)
  BLOX7_VK_ALIAS(vkGetPhysicalDeviceFeatures2KHR,
                    vkGetPhysicalDeviceFeatures2)
  BLOX7_VK_ALIAS(vkGetPhysicalDeviceFormatProperties2KHR,
                    vkGetPhysicalDeviceFormatProperties2)
  BLOX7_VK_ALIAS(vkGetPhysicalDeviceImageFormatProperties2KHR,
                    vkGetPhysicalDeviceImageFormatProperties2)
  BLOX7_VK_ALIAS(vkBindBufferMemory2KHR, vkBindBufferMemory2)
  BLOX7_VK_ALIAS(vkGetDeviceImageMemoryRequirementsKHR,
                    vkGetDeviceImageMemoryRequirements)
  BLOX7_VK_ALIAS(vkGetDeviceImageSparseMemoryRequirementsKHR,
                    vkGetDeviceImageSparseMemoryRequirements)
  BLOX7_VK_ALIAS(vkGetDeviceImageSubresourceLayoutKHR,
                    vkGetDeviceImageSubresourceLayout)
  BLOX7_VK_ALIAS(vkCmdCopyImage2KHR, vkCmdCopyImage2)
  BLOX7_VK_ALIAS(vkCmdCopyImageToBuffer2KHR, vkCmdCopyImageToBuffer2)
  BLOX7_VK_ALIAS(vkMapMemory2KHR, vkMapMemory2)
  BLOX7_VK_ALIAS(vkUnmapMemory2KHR, vkUnmapMemory2)
  BLOX7_VK_ALIAS(vkCmdCopyBufferToImage2KHR, vkCmdCopyBufferToImage2)
#undef BLOX7_VK_ALIAS
  return nullptr;
}

bool NameInList(const char* name, const char* const* names,
                std::size_t count) {
  return name != nullptr &&
         std::any_of(names, names + count, [name](const char* candidate) {
           return std::strcmp(name, candidate) == 0;
         });
}

bool IsEtc2DeviceProc(const char* name) {
  static constexpr const char* kNames[] = {
      "vkCreateImage",          "vkDestroyImage",
      "vkGetDeviceImageMemoryRequirements",
      "vkGetDeviceImageMemoryRequirementsKHR",
      "vkGetDeviceImageSparseMemoryRequirements",
      "vkGetDeviceImageSparseMemoryRequirementsKHR",
      "vkGetDeviceImageSubresourceLayout",
      "vkGetDeviceImageSubresourceLayoutKHR",
      "vkCreateImageView",      "vkBindBufferMemory",
      "vkBindBufferMemory2",    "vkBindBufferMemory2KHR",
      "vkMapMemory",            "vkMapMemory2",
      "vkMapMemory2KHR",        "vkUnmapMemory",
      "vkUnmapMemory2",         "vkUnmapMemory2KHR",
      "vkFreeMemory",           "vkDestroyBuffer",
      "vkCmdCopyBufferToImage", "vkCmdCopyBufferToImage2",
      "vkCmdCopyImage",         "vkCmdCopyImage2",
      "vkCmdCopyImage2KHR",     "vkCmdCopyImageToBuffer",
      "vkCmdCopyImageToBuffer2", "vkCmdCopyImageToBuffer2KHR",
      "vkCmdCopyBufferToImage2KHR", "vkCmdExecuteCommands",
  };
  return NameInList(name, kNames, sizeof(kNames) / sizeof(kNames[0]));
}

bool IsHostOnlyDeviceExtension(const char* name) {
  static constexpr const char* kNames[] = {
      "VK_EXT_full_screen_exclusive",    "VK_KHR_external_memory_win32",
      "VK_KHR_external_semaphore_win32", "VK_KHR_external_fence_win32",
      "VK_KHR_win32_keyed_mutex",        "VK_NV_external_memory_win32",
      "VK_NV_win32_keyed_mutex",         "VK_NV_acquire_winrt_display",
      "VK_EXT_present_timing",
  };
  return NameInList(name, kNames, sizeof(kNames) / sizeof(kNames[0]));
}

bool IsEtc2PhysicalDeviceProc(const char* name) {
  static constexpr const char* kNames[] = {
      "vkGetPhysicalDeviceFeatures",
      "vkGetPhysicalDeviceFeatures2",
      "vkGetPhysicalDeviceFeatures2KHR",
      "vkGetPhysicalDeviceFormatProperties",
      "vkGetPhysicalDeviceFormatProperties2",
      "vkGetPhysicalDeviceFormatProperties2KHR",
      "vkGetPhysicalDeviceImageFormatProperties",
      "vkGetPhysicalDeviceImageFormatProperties2",
      "vkGetPhysicalDeviceImageFormatProperties2KHR",
  };
  return NameInList(name, kNames, sizeof(kNames) / sizeof(kNames[0]));
}

bool IsDeviceAdapterProc(const char* name) {
  return name != nullptr && (IsEtc2DeviceProc(name) || std::strcmp(name, "vkDestroyDevice") == 0 ||
                             std::strcmp(name, "vkGetDeviceQueue") == 0 ||
                             std::strcmp(name, "vkGetDeviceQueue2") == 0 ||
                             std::strcmp(name, "vkCreateSwapchainKHR") == 0 ||
                             std::strcmp(name, "vkDestroySwapchainKHR") == 0 ||
                             std::strcmp(name, "vkGetSwapchainImagesKHR") == 0 ||
                             std::strcmp(name, "vkGetRefreshCycleDurationGOOGLE") == 0 ||
                             std::strcmp(name, "vkGetPastPresentationTimingGOOGLE") == 0 ||
                             std::strcmp(name, "vkAcquireNextImageKHR") == 0 ||
                             std::strcmp(name, "vkAcquireNextImage2KHR") == 0 ||
                             std::strcmp(name, "vkWaitForFences") == 0 ||
                             std::strcmp(name, "vkResetFences") == 0 ||
                             std::strcmp(name, "vkResetCommandPool") == 0 ||
                             std::strcmp(name, "vkGetQueryPoolResults") == 0 ||
                             std::strcmp(name, "vkAllocateCommandBuffers") ==
                                 0 ||
                             std::strcmp(name, "vkFreeCommandBuffers") == 0 ||
                             std::strcmp(name, "vkDestroyCommandPool") == 0 ||
                             std::strcmp(name, "vkBeginCommandBuffer") == 0 ||
                             std::strcmp(name, "vkEndCommandBuffer") == 0 ||
                             std::strcmp(name, "vkResetCommandBuffer") == 0 ||
                             std::strcmp(name, "vkWaitSemaphores") == 0 ||
                             std::strcmp(name, "vkWaitSemaphoresKHR") == 0 ||
                             std::strcmp(name, "vkQueueSubmit") == 0 ||
                             std::strcmp(name, "vkQueueSubmit2") == 0 ||
                             std::strcmp(name, "vkQueueSubmit2KHR") == 0 ||
                             std::strcmp(name, "vkQueueBindSparse") == 0 ||
                             std::strcmp(name, "vkQueueWaitIdle") == 0 ||
                             std::strcmp(name, "vkDeviceWaitIdle") == 0 ||
                             std::strcmp(name, "vkAllocateMemory") == 0 ||
                             std::strcmp(name, "vkQueuePresentKHR") == 0);
}

bool IsGlobalAdapterProc(const char* name) {
  return name != nullptr &&
         (std::strcmp(name, "vkCreateInstance") == 0 ||
          std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 ||
          std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0 ||
          std::strcmp(name, "vkGetInstanceProcAddr") == 0);
}

const VkBaseInStructure* FindFeature(const void* chain, VkStructureType type) {
  const auto* current = static_cast<const VkBaseInStructure*>(chain);
  while (current != nullptr) {
    if (current->sType == type) {
      return current;
    }
    current = current->pNext;
  }
  return nullptr;
}

struct EnabledDeviceFeatures {
  VkPhysicalDeviceFeatures2 root{};
  VkPhysicalDeviceVulkan11Features vulkan11{};
  VkPhysicalDeviceVulkan12Features vulkan12{};

  EnabledDeviceFeatures() {
    root.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    root.pNext = &vulkan11;
    vulkan11.pNext = &vulkan12;
  }
};

EnabledDeviceFeatures InspectEnabledFeatures(
    const VkDeviceCreateInfo& create_info) {
  EnabledDeviceFeatures enabled;
  if (create_info.pEnabledFeatures != nullptr) {
    enabled.root.features = *create_info.pEnabledFeatures;
  }
  if (const auto* features2 =
          reinterpret_cast<const VkPhysicalDeviceFeatures2*>(FindFeature(
              create_info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
      features2 != nullptr) {
    enabled.root.features = features2->features;
  }

  bool timeline = false;
  bool host_query_reset = false;
  if (const auto* vulkan12 =
          reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(FindFeature(
              create_info.pNext,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
      vulkan12 != nullptr) {
    timeline = vulkan12->timelineSemaphore == VK_TRUE;
    host_query_reset = vulkan12->hostQueryReset == VK_TRUE;
  }
  if (const auto* timeline_features = reinterpret_cast<
          const VkPhysicalDeviceTimelineSemaphoreFeatures*>(FindFeature(
          create_info.pNext,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
      timeline_features != nullptr) {
    timeline = timeline_features->timelineSemaphore == VK_TRUE;
  }
  if (const auto* host_query_features =
          reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(
              FindFeature(
                  create_info.pNext,
                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES));
      host_query_features != nullptr) {
    host_query_reset = host_query_features->hostQueryReset == VK_TRUE;
  }
  enabled.vulkan12.timelineSemaphore = timeline;
  enabled.vulkan12.hostQueryReset = host_query_reset;
  return enabled;
}

bool ShouldLogNegativeVulkanResult(VkResult result, std::uint64_t* occurrence) {
  static std::atomic<std::uint64_t> device_lost_count{0};
  static std::atomic<std::uint64_t> other_error_count{0};
  std::atomic<std::uint64_t>& counter =
      result == VK_ERROR_DEVICE_LOST ? device_lost_count : other_error_count;
  const std::uint64_t count =
      counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (occurrence != nullptr) {
    *occurrence = count;
  }
  return count <= 4 || (count & (count - 1)) == 0;
}

class VulkanCallObservation final {
 public:
  explicit VulkanCallObservation(const char* call_name)
      : call_name_(call_name) {
    if (__builtin_expect(
            g_vulkan_call_observation_active.load(std::memory_order_relaxed),
            0)) {
      AdapterState& state = State();
      begin_ = state.note_vulkan_call_begin.load(std::memory_order_acquire);
      end_ = state.note_vulkan_call_end.load(std::memory_order_acquire);
      if (begin_ != nullptr && end_ != nullptr) {
        sequence_ = begin_(call_name);
      }
    }
  }

  ~VulkanCallObservation() {
    if (__builtin_expect(sequence_ != 0 && end_ != nullptr, 0)) {
      end_(sequence_, static_cast<std::int32_t>(result_));
    }
  }

  VulkanCallObservation(const VulkanCallObservation&) = delete;
  VulkanCallObservation& operator=(const VulkanCallObservation&) = delete;

  void SetResult(VkResult result) {
    result_ = result;
    if (__builtin_expect(
            result < VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR, 0)) {
      std::uint64_t occurrence = 0;
      if (ShouldLogNegativeVulkanResult(result, &occurrence)) {
        std::fprintf(stderr,
                     "  [vulkan] unexpected negative result: call=%s "
                     "result=%d device_lost=%u occurrence=%llu\n",
                     call_name_ != nullptr ? call_name_ : "unknown",
                     static_cast<int>(result),
                     result == VK_ERROR_DEVICE_LOST ? 1U : 0U,
                     static_cast<unsigned long long>(occurrence));
      }
    }
  }

 private:
  NoteVulkanCallBeginFn begin_ = nullptr;
  NoteVulkanCallEndFn end_ = nullptr;
  const char* call_name_ = nullptr;
  std::uint64_t sequence_ = 0;
  VkResult result_ = VK_ERROR_UNKNOWN;
};

VkResult WaitForSemaphoresObserved(const char* call_name,
                                   PFN_vkWaitSemaphores host_wait,
                                   VkDevice device,
                                   const VkSemaphoreWaitInfo* wait_info,
                                   std::uint64_t timeout,
                                   VulkanCallObservation* observation) {
  if (host_wait == nullptr) {
    if (observation != nullptr) {
      observation->SetResult(VK_ERROR_INITIALIZATION_FAILED);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  static_cast<void>(call_name);
  const VkResult result =
      NoteDeviceResult(device, host_wait(device, wait_info, timeout), false);
  if (observation != nullptr) {
    observation->SetResult(result);
  }
  return result;
}

VkResult VKAPI_CALL
ObservedHostQueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info) {
  AdapterState& state = State();
  const PFN_vkQueuePresentKHR host_present =
      HostDispatchForQueue(queue).queue_present;
  if (host_present == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const NoteHostPresentBeginFn note_begin =
      state.note_host_present_begin.load(std::memory_order_acquire);
  if (note_begin != nullptr) {
    note_begin();
  }
  const VkResult result = host_present(queue, present_info);
  const NoteHostPresentEndFn note_end =
      state.note_host_present_end.load(std::memory_order_acquire);
  if (note_end != nullptr) {
    note_end(static_cast<std::int32_t>(result));
  }
  return result;
}

void ForgetInstanceLocked(AdapterState& state, VkInstance instance) {
  state.full_screen_exclusive_instances.erase(instance);
  for (auto it = state.physical_device_instances.begin();
       it != state.physical_device_instances.end();) {
    it = it->second == instance ? state.physical_device_instances.erase(it)
                                : std::next(it);
  }
  if (state.latest_instance != instance) {
    return;
  }
  state.latest_instance = VK_NULL_HANDLE;
  for (const auto& live : state.instance_keys) {
    if (live.first != instance) {
      state.latest_instance = live.first;
      return;
    }
  }
  if (state.kept_instance != instance) {
    state.latest_instance = state.kept_instance;
  }
}

void NotePhysicalDevices(VkInstance instance, const VkPhysicalDevice* devices,
                         std::uint32_t count) {
  if (devices == nullptr) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (std::uint32_t i = 0; i < count; ++i) {
    state.physical_device_instances[devices[i]] = instance;
  }
}

VkInstance InstanceForPhysicalDeviceLocked(AdapterState& state,
                                           VkPhysicalDevice physical_device) {
  const auto found = state.physical_device_instances.find(physical_device);
  return found != state.physical_device_instances.end() ? found->second
                                                        : state.latest_instance;
}

template <typename Function>
Function HostPhysicalDeviceProc(const char* name, const char* alias) {
  VkInstance instance = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    instance = state.latest_instance;
  }
  PFN_vkVoidFunction proc = HostInstanceProc(instance, name);
  if (proc == nullptr && alias != nullptr) {
    proc = HostInstanceProc(instance, alias);
  }
  return reinterpret_cast<Function>(proc);
}

bool Etc2EmulatedPhysicalDevice(VkPhysicalDevice physical_device) {
  return State().etc2.PhysicalDeviceNeedsEmulation(
      physical_device, HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFeatures>(
                           "vkGetPhysicalDeviceFeatures", nullptr));
}

VkFormat Etc2HostFormat(VkPhysicalDevice physical_device,
                        const blox7::graphics::Etc2EmulatedFormat& mapped) {
  return State().etc2.PhysicalDeviceStoresBc(
             physical_device,
             HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFeatures>(
                 "vkGetPhysicalDeviceFeatures", nullptr),
             HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceProperties>(
                 "vkGetPhysicalDeviceProperties", nullptr),
             HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
                 "vkGetPhysicalDeviceMemoryProperties", nullptr))
             ? mapped.bc_format
             : mapped.host_format;
}

bool EmulatedEtc2ImageSupported(VkImageType type, VkImageTiling tiling,
                                VkImageUsageFlags usage,
                                VkImageCreateFlags flags) {
  constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT;
  constexpr VkImageCreateFlags kFlags =
      VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
      VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT |
      VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
      VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
  return type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
         (usage & ~kUsage) == 0 && (flags & ~kFlags) == 0;
}

void ReleaseEtc2PoolCommandBuffers(VkDevice device,
                                   VkCommandPool command_pool) {
  std::vector<VkCommandBuffer> command_buffers;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& binding : state.command_buffer_bindings) {
      if (binding.device == device && binding.command_pool == command_pool) {
        command_buffers.push_back(binding.command_buffer);
      }
    }
  }
  for (VkCommandBuffer command_buffer : command_buffers) {
    State().etc2.ReleaseCommandBuffer(command_buffer);
  }
}

void PrepareEtc2Submit2(VkQueue queue, std::uint32_t submit_count,
                        const VkSubmitInfo2* submits) {
  if (submits == nullptr || !State().etc2.HasPendingUploads()) {
    return;
  }
  std::vector<VkCommandBuffer> command_buffers;
  for (std::uint32_t index = 0; index < submit_count; ++index) {
    for (std::uint32_t info = 0; info < submits[index].commandBufferInfoCount;
         ++info) {
      command_buffers.push_back(
          submits[index].pCommandBufferInfos[info].commandBuffer);
    }
  }
  State().etc2.PrepareSubmit(queue, command_buffers.data(),
                             static_cast<std::uint32_t>(command_buffers.size()));
}

bool KeepVulkanObjects() {
  static const bool keep = [] {
    const char* value = std::getenv("BLOX7_VULKAN_KEEP_DEVICE");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  return keep;
}

void AppendKey(std::string* key, const void* data, std::size_t size) {
  key->append(static_cast<const char*>(data), size);
}

void AppendKey(std::string* key, const char* text) {
  if (text != nullptr) {
    key->append(text);
  }
  key->push_back('\0');
}

std::string InstanceKey(const VkInstanceCreateInfo& info) {
  std::string key;
  if (info.pNext != nullptr || (info.pApplicationInfo != nullptr &&
                                info.pApplicationInfo->pNext != nullptr)) {
    return key;
  }
  AppendKey(&key, &info.flags, sizeof info.flags);
  if (const VkApplicationInfo* app = info.pApplicationInfo) {
    AppendKey(&key, app->pApplicationName);
    AppendKey(&key, &app->applicationVersion, sizeof app->applicationVersion);
    AppendKey(&key, app->pEngineName);
    AppendKey(&key, &app->engineVersion, sizeof app->engineVersion);
    AppendKey(&key, &app->apiVersion, sizeof app->apiVersion);
  }
  for (std::uint32_t index = 0; index < info.enabledLayerCount; ++index) {
    AppendKey(&key, info.ppEnabledLayerNames[index]);
  }
  key.push_back('\1');
  for (std::uint32_t index = 0; index < info.enabledExtensionCount; ++index) {
    AppendKey(&key, info.ppEnabledExtensionNames[index]);
  }
  return key;
}

std::size_t FeatureStructSize(VkStructureType type) {
  switch (type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
      return sizeof(VkPhysicalDeviceFeatures2);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
      return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
      return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
      return sizeof(VkPhysicalDeviceVulkan13Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
      return sizeof(VkPhysicalDeviceVulkan14Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
      return sizeof(VkPhysicalDevice16BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
      return sizeof(VkPhysicalDevice8BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
      return sizeof(VkPhysicalDeviceShaderFloat16Int8Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
      return sizeof(VkPhysicalDeviceMultiviewFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
      return sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
      return sizeof(VkPhysicalDeviceShaderDrawParametersFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
      return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
      return sizeof(VkPhysicalDeviceHostQueryResetFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
      return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
      return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
      return sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
      return sizeof(VkPhysicalDeviceImagelessFramebufferFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES:
      return sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
      return sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
      return sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
      return sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
      return sizeof(VkPhysicalDeviceSynchronization2Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
      return sizeof(VkPhysicalDeviceDynamicRenderingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
      return sizeof(VkPhysicalDeviceMaintenance4Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES:
      return sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceFaultFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES:
      return sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
      return sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
      return sizeof(VkPhysicalDeviceInlineUniformBlockFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceRobustness2FeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
      return sizeof(VkPhysicalDeviceImageRobustnessFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
      return sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES:
      return sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
      return sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES:
      return sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES:
      return sizeof(VkPhysicalDevicePrivateDataFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
      return sizeof(VkPhysicalDeviceProtectedMemoryFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
      return sizeof(VkPhysicalDeviceVariablePointersFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
      return sizeof(VkPhysicalDeviceShaderAtomicInt64Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES:
      return sizeof(VkPhysicalDeviceIndexTypeUint8Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES:
      return sizeof(VkPhysicalDeviceLineRasterizationFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES:
      return sizeof(VkPhysicalDeviceVertexAttributeDivisorFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceMemoryPriorityFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT:
      return sizeof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT);
    default:
      return 0;
  }
}

std::string DeviceKey(VkPhysicalDevice physical_device,
                      const VkDeviceCreateInfo& info,
                      VkStructureType* unknown) {
  std::string key;
  AppendKey(&key, &physical_device, sizeof physical_device);
  AppendKey(&key, &info.flags, sizeof info.flags);
  for (std::uint32_t index = 0; index < info.queueCreateInfoCount; ++index) {
    const VkDeviceQueueCreateInfo& queue = info.pQueueCreateInfos[index];
    if (queue.pNext != nullptr) {
      *unknown = static_cast<const VkBaseInStructure*>(queue.pNext)->sType;
      return {};
    }
    if (queue.pQueuePriorities == nullptr) {
      return {};
    }
    AppendKey(&key, &queue.flags, sizeof queue.flags);
    AppendKey(&key, &queue.queueFamilyIndex, sizeof queue.queueFamilyIndex);
    AppendKey(&key, &queue.queueCount, sizeof queue.queueCount);
    AppendKey(&key, queue.pQueuePriorities, sizeof(float) * queue.queueCount);
  }
  key.push_back('\1');
  for (std::uint32_t index = 0; index < info.enabledExtensionCount; ++index) {
    AppendKey(&key, info.ppEnabledExtensionNames[index]);
  }
  key.push_back('\1');
  if (info.pEnabledFeatures != nullptr) {
    AppendKey(&key, info.pEnabledFeatures, sizeof *info.pEnabledFeatures);
  }
  for (const auto* next = static_cast<const VkBaseInStructure*>(info.pNext);
       next != nullptr; next = next->pNext) {
    const std::size_t size = FeatureStructSize(next->sType);
    if (size == 0) {
      *unknown = next->sType;
      return {};
    }
    AppendKey(&key, &next->sType, sizeof next->sType);
    AppendKey(&key, reinterpret_cast<const char*>(next) + sizeof(VkBaseInStructure),
              size - sizeof(VkBaseInStructure));
  }
  return key;
}

void DestroyRetiredSwapchain(VkDevice only_device = VK_NULL_HANDLE) {
  VkDevice device = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (only_device != VK_NULL_HANDLE &&
        state.retired_swapchain_device != only_device) {
      return;
    }
    device = state.retired_swapchain_device;
    swapchain = state.retired_swapchain;
    state.retired_swapchain = VK_NULL_HANDLE;
    state.retired_swapchain_device = VK_NULL_HANDLE;
    state.retired_swapchain_surface = VK_NULL_HANDLE;
  }
  if (swapchain == VK_NULL_HANDLE) {
    return;
  }
  if (const auto destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
          HostDeviceProc(device, "vkDestroySwapchainKHR"))) {
    destroy(device, swapchain, nullptr);
  }
}

void ReleaseKeptDevice() {
  VkDevice device = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    device = state.kept_device;
    state.kept_device = VK_NULL_HANDLE;
    state.kept_device_key.clear();
  }
  if (device == VK_NULL_HANDLE) {
    return;
  }
  DestroyRetiredSwapchain(device);
  State().etc2.ForgetDevice(device);
  if (const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(
          HostDeviceProc(device, "vkDestroyDevice"))) {
    destroy(device, nullptr);
  }
}

void ReleaseKeptObjects() {
  ReleaseKeptDevice();
  DestroyRetiredSwapchain();
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkInstance surface_instance = VK_NULL_HANDLE;
  VkInstance instance = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    surface = state.kept_surface;
    surface_instance = state.kept_surface_instance;
    instance = state.kept_instance;
    state.kept_surface = VK_NULL_HANDLE;
    state.kept_surface_instance = VK_NULL_HANDLE;
    state.kept_instance = VK_NULL_HANDLE;
    state.kept_instance_key.clear();
    if (instance != VK_NULL_HANDLE) {
      ForgetInstanceLocked(state, instance);
    }
  }
  if (surface != VK_NULL_HANDLE) {
    State().android_wsi.DestroySurface(surface_instance, surface, nullptr);
  }
  if (instance != VK_NULL_HANDLE) {
    if (const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
            HostInstanceProc(instance, "vkDestroyInstance"))) {
      destroy(instance, nullptr);
    }
  }
}

void ShutdownIdleAdapterLocked(AdapterState& state) {
  if (state.active_instance_count != 0 || !state.initialized ||
      state.kept_instance != VK_NULL_HANDLE) {
    return;
  }
  state.android_wsi.Shutdown();
  state.host_wsi.Shutdown();
  state.host_get_instance_proc_addr = nullptr;
  state.host_get_device_proc_addr.store(nullptr, std::memory_order_release);
  state.device_dispatches.clear();
  state.queue_bindings.clear();
  state.command_buffer_bindings.clear();
  state.device_keys.clear();
  state.device_allocations.clear();
  state.swapchain_surfaces.clear();
  state.lost_surfaces.clear();
  state.lost_devices.clear();
  state.hidden_images.clear();
  state.hidden_image_count.store(0, std::memory_order_release);
  state.shown_surface = VK_NULL_HANDLE;
  InvalidateFastDispatchCaches();
  state.host_surface_capabilities = nullptr;
  state.host_surface_present_modes = nullptr;
  state.latest_instance = VK_NULL_HANDLE;
  state.full_screen_exclusive_instances.clear();
  state.note_present.store(nullptr, std::memory_order_release);
  state.note_host_present_begin.store(nullptr, std::memory_order_release);
  state.note_host_present_end.store(nullptr, std::memory_order_release);
  state.note_vulkan_call_begin.store(nullptr, std::memory_order_release);
  state.note_vulkan_call_end.store(nullptr, std::memory_order_release);
  g_vulkan_call_observation_active.store(false, std::memory_order_release);
  state.note_surface_out_of_date.store(nullptr, std::memory_order_release);
  state.window_holds_presents.store(nullptr, std::memory_order_release);
  state.window_hides_presents.store(nullptr, std::memory_order_release);
  state.extent_translation_logged = false;
  state.present_policy_logged = false;
  state.initialized = false;
  std::fprintf(stderr, "  [vulkan] SDL WSI adapter shut down\n");
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) void
blox7_vulkan_release_kept_objects() {
  ReleaseKeptObjects();
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ShutdownIdleAdapterLocked(state);
}

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* create_info,
                 const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!EnsureInitialized() || create_info == nullptr || instance == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::vector<std::string> requested;
  requested.reserve(create_info->enabledExtensionCount);
  for (std::uint32_t index = 0; index < create_info->enabledExtensionCount;
       ++index) {
    const char* extension = create_info->ppEnabledExtensionNames[index];
    if (extension == nullptr) {
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    requested.emplace_back(extension);
  }
  std::vector<std::string> translated;
  const blox7::Status status =
      State().android_wsi.TranslateInstanceExtensions(requested, &translated);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] instance extension rewrite failed: %s\n",
                 status.message().c_str());
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
  const auto enabled = [&translated](const char* name) {
    return std::find(translated.begin(), translated.end(), name) !=
           translated.end();
  };
  VkResult listed = VK_SUCCESS;
  const bool surface_capabilities2 =
      enabled(VK_KHR_SURFACE_EXTENSION_NAME) &&
      ListsExtension(AndroidVisibleExtensions(nullptr, &listed),
                     VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  if (surface_capabilities2 &&
      !enabled(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
    translated.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
  }
  std::vector<const char*> translated_names;
  translated_names.reserve(translated.size());
  for (const std::string& extension : translated) {
    translated_names.push_back(extension.c_str());
  }
  VkInstanceCreateInfo host_info = *create_info;
  host_info.enabledExtensionCount =
      static_cast<std::uint32_t>(translated_names.size());
  host_info.ppEnabledExtensionNames = translated_names.data();
  VkApplicationInfo host_application_info{};
  if (create_info->pApplicationInfo != nullptr) {
    host_application_info = *create_info->pApplicationInfo;
  } else {
    host_application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  }
  const std::uint32_t requested_api = host_application_info.apiVersion;
  const auto host_enumerate_version =
      reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
          HostInstanceProc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  std::uint32_t host_api = VK_API_VERSION_1_0;
  if (host_enumerate_version != nullptr) {
    (void)host_enumerate_version(&host_api);
  }
  if (host_api >= VK_API_VERSION_1_2 && requested_api < VK_API_VERSION_1_2) {
    host_application_info.apiVersion = VK_API_VERSION_1_2;
    host_info.pApplicationInfo = &host_application_info;
  }
  const bool full_screen_exclusive =
      surface_capabilities2 &&
      (host_application_info.apiVersion >= VK_API_VERSION_1_1 ||
       enabled(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME));
  const std::string key = KeepVulkanObjects() && allocator == nullptr
                              ? InstanceKey(host_info)
                              : std::string();
  {
    AdapterState& state = State();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.kept_instance != VK_NULL_HANDLE) {
      if (!key.empty() && key == state.kept_instance_key) {
        *instance = state.kept_instance;
        state.kept_instance = VK_NULL_HANDLE;
        state.kept_instance_key.clear();
        state.instance_keys[*instance] = key;
        ++state.active_instance_count;
        state.latest_instance = *instance;
        if (full_screen_exclusive) {
          state.full_screen_exclusive_instances.insert(*instance);
        }
        std::fprintf(stderr, "  [vulkan] host VkInstance reused\n");
        return VK_SUCCESS;
      }
      lock.unlock();
      ReleaseKeptObjects();
    }
  }
  const auto host_create = reinterpret_cast<PFN_vkCreateInstance>(
      HostInstanceProc(VK_NULL_HANDLE, "vkCreateInstance"));
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(&host_info, allocator, instance);
  if (result == VK_SUCCESS) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.active_instance_count;
    state.latest_instance = *instance;
    if (full_screen_exclusive) {
      state.full_screen_exclusive_instances.insert(*instance);
    }
    state.instance_keys[*instance] = key;
    state.host_get_device_proc_addr.store(
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            state.host_get_instance_proc_addr(*instance, "vkGetDeviceProcAddr")),
        std::memory_order_release);
    std::fprintf(stderr, "  [vulkan] host VkInstance created\n");
    if (host_application_info.apiVersion != requested_api) {
      std::fprintf(stderr,
                   "  [vulkan] instance API raised to 1.2 for imported "
                   "libplacebo device interop\n");
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  if (instance == VK_NULL_HANDLE) {
    return;
  }
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.instance_keys.find(instance);
    if (found != state.instance_keys.end()) {
      std::string key = std::move(found->second);
      state.instance_keys.erase(found);
      if (!key.empty() && allocator == nullptr &&
          state.kept_instance == VK_NULL_HANDLE) {
        state.kept_instance = instance;
        state.kept_instance_key = std::move(key);
        if (state.active_instance_count != 0) {
          --state.active_instance_count;
        }
        return;
      }
    }
  }
  ReleaseKeptObjects();
  const auto host_destroy = reinterpret_cast<PFN_vkDestroyInstance>(
      HostInstanceProc(instance, "vkDestroyInstance"));
  if (host_destroy != nullptr) {
    host_destroy(instance, allocator);
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  ForgetInstanceLocked(state, instance);
  if (state.active_instance_count != 0) {
    --state.active_instance_count;
  }
  ShutdownIdleAdapterLocked(state);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance, const void* create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.CreateSurface(create_info, surface, &captured)) {
    return captured;
  }
  if (!EnsureInitialized()) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (surface != nullptr && allocator == nullptr) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.kept_surface != VK_NULL_HANDLE &&
        state.kept_surface_instance == instance) {
      *surface = state.kept_surface;
      state.kept_surface = VK_NULL_HANDLE;
      state.kept_surface_instance = VK_NULL_HANDLE;
      std::fprintf(stderr, "  [vulkan] Android surface reuses the window's host surface\n");
      return VK_SUCCESS;
    }
  }
  const auto result = State().android_wsi.CreateAndroidSurface(
      instance, create_info, allocator, surface);
  if (result == VK_SUCCESS) {
    std::fprintf(stderr, "  [vulkan] Android surface mapped to SDL WSI\n");
  }
  return static_cast<VkResult>(result);
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks* allocator) {
  if (State().capture.DestroySurface(surface)) {
    return;
  }
  if (!EnsureInitialized()) {
    return;
  }
  bool retired_uses_surface = false;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto lost = std::find(state.lost_surfaces.begin(),
                                state.lost_surfaces.end(), surface);
    const bool was_lost = lost != state.lost_surfaces.end();
    if (was_lost) {
      state.lost_surfaces.erase(lost);
    }
    const auto key = state.instance_keys.find(instance);
    if (!was_lost && surface != VK_NULL_HANDLE && allocator == nullptr &&
        state.kept_surface == VK_NULL_HANDLE &&
        key != state.instance_keys.end() && !key->second.empty()) {
      state.kept_surface = surface;
      state.kept_surface_instance = instance;
      return;
    }
    retired_uses_surface = state.retired_swapchain_surface == surface;
    if (state.shown_surface == surface) {
      state.shown_surface = VK_NULL_HANDLE;
    }
  }
  if (retired_uses_surface) {
    DestroyRetiredSwapchain();
  }
  State().android_wsi.DestroySurface(instance, surface, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* property_count,
    VkExtensionProperties* properties) {
  if (property_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = VK_SUCCESS;
  const std::vector<VkExtensionProperties> visible =
      AndroidVisibleExtensions(layer_name, &result);
  if (result != VK_SUCCESS) {
    return result;
  }
  if (properties == nullptr) {
    *property_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t capacity = *property_count;
  const std::uint32_t copied =
      std::min(capacity, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, properties);
  *property_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    std::uint32_t* property_count, VkLayerProperties* properties) {
  const auto host_enumerate =
      reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(HostInstanceProc(
          VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
  return host_enumerate != nullptr ? host_enumerate(property_count, properties)
                                   : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (instance == VK_NULL_HANDLE) {
    const PFN_vkVoidFunction adapter = AdapterProc(name);
    return adapter != nullptr && IsGlobalAdapterProc(name)
               ? adapter
               : HostInstanceProc(VK_NULL_HANDLE, name);
  }
  if (name != nullptr &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
    const PFN_vkVoidFunction host = HostInstanceProc(instance, name);
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.host_surface_capabilities =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(host);
    return host != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(
                                 vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
                           : nullptr;
  }
  if (name != nullptr &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
    const PFN_vkVoidFunction host = HostInstanceProc(instance, name);
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.host_surface_present_modes =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(host);
    return host != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(
                                 vkGetPhysicalDeviceSurfacePresentModesKHR)
                           : nullptr;
  }
#define BLOX7_SURFACE_QUERY(function, field)                                \
  if (name != nullptr && std::strcmp(name, #function) == 0) {               \
    const PFN_vkVoidFunction host = HostInstanceProc(instance, name);       \
    AdapterState& state = State();                                          \
    std::lock_guard<std::mutex> lock(state.mutex);                          \
    state.field = reinterpret_cast<PFN_##function>(host);                   \
    return host != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(function) \
                           : nullptr;                                       \
  }
  BLOX7_SURFACE_QUERY(vkGetPhysicalDeviceSurfaceSupportKHR,
                      host_surface_support)
  BLOX7_SURFACE_QUERY(vkGetPhysicalDeviceSurfaceFormatsKHR,
                      host_surface_formats)
  BLOX7_SURFACE_QUERY(vkGetPhysicalDeviceSurfaceFormats2KHR,
                      host_surface_formats2)
  BLOX7_SURFACE_QUERY(vkGetPhysicalDeviceSurfaceCapabilities2KHR,
                      host_surface_capabilities2)
#undef BLOX7_SURFACE_QUERY
  if (const PFN_vkVoidFunction adapter = AdapterProc(name);
      adapter != nullptr) {
    if ((IsDeviceAdapterProc(name) || IsEtc2PhysicalDeviceProc(name)) &&
        HostInstanceProc(instance, name) == nullptr) {
      return nullptr;
    }
    return adapter;
  }
  return HostInstanceProc(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char* name) {
  if (name == nullptr || !EnsureInitialized()) {
    return nullptr;
  }
  const PFN_vkGetDeviceProcAddr host_get_device_proc_addr =
      State().host_get_device_proc_addr.load(std::memory_order_acquire);
  const PFN_vkVoidFunction host = host_get_device_proc_addr != nullptr
                                      ? host_get_device_proc_addr(device, name)
                                      : nullptr;
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return host != nullptr
               ? reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr)
               : nullptr;
  }
  if (!IsDeviceAdapterProc(name)) {
    return host;
  }
  return host != nullptr ? AdapterProc(name) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
  if (create_info == nullptr || device == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr host_get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr host_get_device_proc_addr = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    instance = InstanceForPhysicalDeviceLocked(state, physical_device);
    host_get_instance_proc_addr = state.host_get_instance_proc_addr;
    host_get_device_proc_addr =
        state.host_get_device_proc_addr.load(std::memory_order_acquire);
  }
  const auto host_create = reinterpret_cast<PFN_vkCreateDevice>(
      HostInstanceProc(instance, "vkCreateDevice"));
  if (host_create == nullptr || host_get_instance_proc_addr == nullptr ||
      host_get_device_proc_addr == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkPhysicalDeviceProperties properties{};
  const auto host_get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          HostInstanceProc(instance, "vkGetPhysicalDeviceProperties"));
  if (host_get_properties != nullptr) {
    host_get_properties(physical_device, &properties);
  }

  VkDeviceCreateInfo host_info = *create_info;
  bool full_screen_exclusive = false;
  bool exclusive_instance = false;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    exclusive_instance =
        state.full_screen_exclusive_instances.count(instance) != 0;
  }
  std::vector<const char*> host_extensions(
      create_info->ppEnabledExtensionNames,
      create_info->ppEnabledExtensionNames + create_info->enabledExtensionCount);
  const auto wants = [&host_extensions](const char* extension) {
    return std::any_of(host_extensions.begin(), host_extensions.end(),
                       [extension](const char* name) {
                         return std::strcmp(name, extension) == 0;
                       });
  };
  full_screen_exclusive = wants(kFullScreenExclusiveExtension);
  if (!full_screen_exclusive && exclusive_instance &&
      wants(VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
      properties.apiVersion >= VK_API_VERSION_1_1 &&
      HostDeviceExtension(instance, physical_device,
                          kFullScreenExclusiveExtension)) {
    host_extensions.push_back(kFullScreenExclusiveExtension);
    host_info.enabledExtensionCount =
        static_cast<std::uint32_t>(host_extensions.size());
    host_info.ppEnabledExtensionNames = host_extensions.data();
    full_screen_exclusive = true;
  }
  std::uint32_t queue_family_count = 0;
  const auto host_get_queue_families =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          HostInstanceProc(instance,
                           "vkGetPhysicalDeviceQueueFamilyProperties"));
  if (host_get_queue_families != nullptr) {
    host_get_queue_families(physical_device, &queue_family_count, nullptr);
  }
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  if (host_get_queue_families != nullptr && queue_family_count != 0) {
    host_get_queue_families(physical_device, &queue_family_count,
                            queue_families.data());
    queue_families.resize(queue_family_count);
  }
  std::vector<VkDeviceQueueCreateInfo> host_queues(
      create_info->pQueueCreateInfos,
      create_info->pQueueCreateInfos + create_info->queueCreateInfoCount);
  std::vector<float> spare_priorities;
  std::uint32_t spare_family = UINT32_MAX;
  std::uint32_t spare_index = 0;
  for (std::uint32_t family = 0;
       family < queue_families.size() && spare_family == UINT32_MAX;
       ++family) {
    const auto requested =
        std::find_if(host_queues.begin(), host_queues.end(),
                     [family](const VkDeviceQueueCreateInfo& queue) {
                       return queue.queueFamilyIndex == family;
                     });
    const std::uint32_t used =
        requested != host_queues.end() ? requested->queueCount : 0;
    const bool flagged =
        std::any_of(host_queues.begin(), host_queues.end(),
                    [family](const VkDeviceQueueCreateInfo& queue) {
                      return queue.queueFamilyIndex == family &&
                             queue.flags != 0;
                    });
    if (queue_families[family].queueCount <= used || flagged ||
        (requested != host_queues.end() &&
         requested->pQueuePriorities == nullptr)) {
      continue;
    }
    spare_family = family;
    spare_index = used;
    if (requested != host_queues.end()) {
      spare_priorities.assign(requested->pQueuePriorities,
                              requested->pQueuePriorities + used);
      spare_priorities.push_back(spare_priorities.front());
      requested->queueCount = used + 1;
      requested->pQueuePriorities = spare_priorities.data();
    } else {
      spare_priorities.push_back(1.0f);
      VkDeviceQueueCreateInfo spare{};
      spare.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      spare.queueFamilyIndex = family;
      spare.queueCount = 1;
      spare.pQueuePriorities = spare_priorities.data();
      host_queues.push_back(spare);
    }
    host_info.queueCreateInfoCount =
        static_cast<std::uint32_t>(host_queues.size());
    host_info.pQueueCreateInfos = host_queues.data();
  }
  EnabledDeviceFeatures enabled = InspectEnabledFeatures(*create_info);
  const bool etc2_emulated = State().etc2.PhysicalDeviceNeedsEmulation(
      physical_device, reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
                           HostInstanceProc(instance,
                                            "vkGetPhysicalDeviceFeatures")));
  const bool etc2_bc =
      etc2_emulated &&
      State().etc2.PhysicalDeviceStoresBc(
          physical_device,
          reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
              HostInstanceProc(instance, "vkGetPhysicalDeviceFeatures")),
          host_get_properties,
          reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
              HostInstanceProc(instance,
                               "vkGetPhysicalDeviceMemoryProperties")));
  VkPhysicalDeviceFeatures host_enabled_features{};
  VkPhysicalDeviceFeatures2* requested_features2 = nullptr;
  VkBool32 requested_features2_etc2 = VK_FALSE;
  VkBool32 requested_features2_bc = VK_FALSE;
  if (etc2_emulated) {
    enabled.root.features.textureCompressionETC2 = VK_FALSE;
    if (create_info->pEnabledFeatures != nullptr) {
      host_enabled_features = *create_info->pEnabledFeatures;
      host_enabled_features.textureCompressionETC2 = VK_FALSE;
      host_info.pEnabledFeatures = &host_enabled_features;
    }
    requested_features2 = const_cast<VkPhysicalDeviceFeatures2*>(
        reinterpret_cast<const VkPhysicalDeviceFeatures2*>(FindFeature(
            create_info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)));
    if (requested_features2 != nullptr) {
      requested_features2_etc2 =
          requested_features2->features.textureCompressionETC2;
      requested_features2->features.textureCompressionETC2 = VK_FALSE;
      requested_features2_bc =
          requested_features2->features.textureCompressionBC;
      if (etc2_bc) {
        requested_features2->features.textureCompressionBC = VK_TRUE;
      }
    } else if (etc2_bc) {
      host_enabled_features.textureCompressionBC = VK_TRUE;
      host_info.pEnabledFeatures = &host_enabled_features;
    }
  }
  enabled.root.pNext = &enabled.vulkan11;
  enabled.vulkan11.pNext = &enabled.vulkan12;
  enabled.vulkan12.pNext = nullptr;

  std::string key;
  if (KeepVulkanObjects() && allocator == nullptr) {
    VkStructureType unknown = VK_STRUCTURE_TYPE_MAX_ENUM;
    key = DeviceKey(physical_device, host_info, &unknown);
    static std::atomic<bool> unknown_logged{false};
    if (key.empty() && !unknown_logged.exchange(true)) {
      std::fprintf(stderr, "  [vulkan] VkDevice can't be kept across joins: struct %d\n",
                   static_cast<int>(unknown));
    }
  }
  bool reused = false;
  {
    AdapterState& state = State();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.kept_device != VK_NULL_HANDLE) {
      if (!key.empty() && key == state.kept_device_key) {
        *device = state.kept_device;
        state.kept_device = VK_NULL_HANDLE;
        state.kept_device_key.clear();
        reused = true;
      } else {
        lock.unlock();
        ReleaseKeptDevice();
      }
    }
  }
  const VkResult result =
      reused ? VK_SUCCESS
             : host_create(physical_device, &host_info, allocator, device);
  if (requested_features2 != nullptr) {
    requested_features2->features.textureCompressionETC2 =
        requested_features2_etc2;
    requested_features2->features.textureCompressionBC = requested_features2_bc;
  }
  if (result != VK_SUCCESS) {
    return result;
  }
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.device_keys[*device] = key;
  }
  if (reused) {
    std::fprintf(stderr, "  [vulkan] host VkDevice reused\n");
  }
  VkQueue spare_queue = VK_NULL_HANDLE;
  if (spare_family != UINT32_MAX) {
    if (const auto host_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            host_get_device_proc_addr(*device, "vkGetDeviceQueue"))) {
      host_get_queue(*device, spare_family, spare_index, &spare_queue);
    }
  }
  RegisterHostDeviceDispatch(*device, physical_device,
                             host_get_device_proc_addr, full_screen_exclusive,
                             spare_queue);
  State().capture.RegisterDevice(instance, physical_device, *device,
                                 host_get_instance_proc_addr,
                                 host_get_device_proc_addr, spare_queue,
                                 &State().spare_queue_mutex);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  if (const auto host_memory_properties =
          reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
              HostInstanceProc(instance,
                               "vkGetPhysicalDeviceMemoryProperties"));
      host_memory_properties != nullptr) {
    host_memory_properties(physical_device, &memory_properties);
  }

  State().etc2.RegisterDevice(
      *device, physical_device, etc2_emulated, memory_properties,
      host_get_device_proc_addr,
      host_get_properties != nullptr ? &properties.limits : nullptr,
      queue_families.data(), static_cast<std::uint32_t>(queue_families.size()),
      etc2_bc);
  std::uint32_t graphics_family = UINT32_MAX;
  std::uint32_t graphics_count = 0;
  for (std::uint32_t index = 0; index < create_info->queueCreateInfoCount;
       ++index) {
    const VkDeviceQueueCreateInfo& queue_info =
        create_info->pQueueCreateInfos[index];
    if (queue_info.queueFamilyIndex >= queue_families.size() ||
        queue_info.queueCount == 0 ||
        (queue_families[queue_info.queueFamilyIndex].queueFlags &
         VK_QUEUE_GRAPHICS_BIT) == 0) {
      continue;
    }
    graphics_family = queue_info.queueFamilyIndex;
    graphics_count = 1;
    break;
  }

  const bool compositor_ready = true;
  const bool registered =
      compositor_ready && graphics_family != UINT32_MAX &&
      State().text_overlay.RegisterDevice(
          instance, physical_device, *device, VK_API_VERSION_1_2,
          create_info->ppEnabledExtensionNames,
          create_info->enabledExtensionCount, graphics_family, graphics_count,
          &enabled.root, host_get_instance_proc_addr,
          host_get_device_proc_addr);
  if (!registered) {
    std::fprintf(stderr,
                 "  [vulkan] same-surface text compositor unavailable for "
                 "this VkDevice\n");
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
  if (device == VK_NULL_HANDLE) {
    return;
  }
  const auto host_destroy = reinterpret_cast<PFN_vkDestroyDevice>(
      HostDeviceProc(device, "vkDestroyDevice"));
  State().text_overlay.DestroyDevice(device);
  State().capture.DestroyDevice(device);
  State().etc2.DestroyDevice(device);
  std::string key;
  std::int64_t allocations = 0;
  bool lost = false;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (const auto found = std::find(state.lost_devices.begin(),
                                     state.lost_devices.end(), device);
        found != state.lost_devices.end()) {
      state.lost_devices.erase(found);
      lost = true;
    }
    if (const auto found = state.device_keys.find(device);
        found != state.device_keys.end()) {
      key = std::move(found->second);
      state.device_keys.erase(found);
    }
    if (const auto found = state.device_allocations.find(device);
        found != state.device_allocations.end()) {
      allocations = found->second;
      state.device_allocations.erase(found);
    }
  }
  bool keep = !lost && device != VK_NULL_HANDLE && allocator == nullptr &&
              !key.empty();
  if (keep && allocations != 0) {
    std::fprintf(stderr, "  [vulkan] VkDevice destroyed with %lld allocations left\n",
                 static_cast<long long>(allocations));
    keep = false;
  }
  if (keep) {
    const auto wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
        HostDeviceProc(device, "vkDeviceWaitIdle"));
    keep = wait_idle != nullptr && wait_idle(device) == VK_SUCCESS;
  }
  if (keep) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.kept_device == VK_NULL_HANDLE) {
      state.kept_device = device;
      state.kept_device_key = std::move(key);
    } else {
      keep = false;
    }
  }
  if (!keep) {
    DestroyRetiredSwapchain(device);
    State().etc2.ForgetDevice(device);
    if (host_destroy != nullptr) {
      host_destroy(device, allocator);
    }
  }
  RemoveHostDeviceDispatch(device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device,
                                            std::uint32_t queue_family_index,
                                            std::uint32_t queue_index,
                                            VkQueue* queue) {
  const auto host_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
      HostDeviceProc(device, "vkGetDeviceQueue"));
  if (host_get_queue == nullptr || queue == nullptr) {
    return;
  }
  host_get_queue(device, queue_family_index, queue_index, queue);
  if (*queue != VK_NULL_HANDLE) {
    RegisterHostQueueBinding(device, *queue);
    State().capture.RegisterQueue(device, *queue, queue_family_index);
    State().etc2.RegisterQueue(device, *queue, queue_family_index);
    (void)State().text_overlay.RegisterQueue(device, *queue, queue_family_index,
                                             queue_index);
  }
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* queue_info, VkQueue* queue) {
  const auto host_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue2>(
      HostDeviceProc(device, "vkGetDeviceQueue2"));
  if (host_get_queue == nullptr || queue_info == nullptr || queue == nullptr) {
    return;
  }
  host_get_queue(device, queue_info, queue);
  if (*queue != VK_NULL_HANDLE) {
    RegisterHostQueueBinding(device, *queue);
    State().capture.RegisterQueue(device, *queue, queue_info->queueFamilyIndex);
    State().etc2.RegisterQueue(device, *queue, queue_info->queueFamilyIndex);
    (void)State().text_overlay.RegisterQueue(
        device, *queue, queue_info->queueFamilyIndex, queue_info->queueIndex);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.CreateSwapchain(device, create_info, swapchain,
                                      &captured)) {
    return captured;
  }
  const auto host_create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
      HostDeviceProc(device, "vkCreateSwapchainKHR"));
  if (host_create == nullptr || create_info == nullptr ||
      swapchain == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkSwapchainCreateInfoKHR host_info = *create_info;
  const blox7::graphics::PresentModePolicy present_policy =
      blox7::graphics::CachedPresentModePolicy();
  const auto explicit_mode = blox7::graphics::ExplicitPresentMode(present_policy);
  auto image_count_policy = present_policy;
  if (present_policy != blox7::graphics::PresentModePolicy::kHostDefault) {
    bool mode_selected = false;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_caps = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_modes_query = nullptr;
    {
      AdapterState& state = State();
      std::lock_guard<std::mutex> lock(state.mutex);
      host_caps = state.host_surface_capabilities;
      host_modes_query = state.host_surface_present_modes;
    }
    const VkPhysicalDevice physical_device =
        HostDispatchForDevice(device).physical_device;
    if (host_modes_query != nullptr && physical_device != VK_NULL_HANDLE) {
      std::uint32_t host_count = 0;
      if (host_modes_query(physical_device, create_info->surface, &host_count,
                           nullptr) == VK_SUCCESS &&
          host_count != 0) {
        std::vector<VkPresentModeKHR> host_modes(host_count);
        const VkResult modes_result = host_modes_query(
            physical_device, create_info->surface, &host_count,
            host_modes.data());
        if (modes_result == VK_SUCCESS || modes_result == VK_INCOMPLETE) {
          host_modes.resize(host_count);
          const std::vector<VkPresentModeKHR> selected =
              blox7::graphics::FilterPresentModes(present_policy,
                                                     host_modes);
          if (!selected.empty()) {
            static std::atomic<bool> mode_logged{false};
            if (!mode_logged.exchange(true, std::memory_order_relaxed)) {
              std::fprintf(
                  stderr,
                  "  [vulkan] present_mode=%s requested=%s applied=%s%s\n",
                  blox7::graphics::PresentModePolicyName(present_policy),
                  blox7::graphics::PresentModeKhrName(
                      create_info->presentMode),
                  blox7::graphics::PresentModeKhrName(selected.front()),
                  explicit_mode && selected.front() != *explicit_mode
                      ? " (requested mode unsupported; using FIFO)" : "");
            }
            host_info.presentMode = selected.front();
            mode_selected = true;
            if (explicit_mode && selected.front() != *explicit_mode) {
              image_count_policy = blox7::graphics::PresentModePolicy::kFifo;
            }
          }
        }
      }
    }
    if (explicit_mode && !mode_selected) {
      std::fprintf(stderr, "  [vulkan] cannot resolve present_mode=%s for surface\n",
                   blox7::graphics::PresentModePolicyName(present_policy));
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    if (host_caps != nullptr && physical_device != VK_NULL_HANDLE &&
        host_caps(physical_device, create_info->surface, &capabilities) ==
            VK_SUCCESS) {
      const std::uint32_t preferred =
          blox7::graphics::PreferSwapchainMinImageCount(
              image_count_policy, create_info->minImageCount,
              capabilities.minImageCount, capabilities.maxImageCount);
      if (preferred != host_info.minImageCount) {
        host_info.minImageCount = preferred;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
          std::fprintf(stderr,
                       "  [vulkan] present policy=%s swapchain "
                       "minImageCount=%u (requested=%u max=%u)\n",
                       blox7::graphics::PresentModePolicyName(
                           present_policy),
                       preferred, create_info->minImageCount,
                       capabilities.maxImageCount);
        }
      }
    }
  }
  // the post pass copies the frame out of the swapchain image first
  if (const char* post_fx = std::getenv("BLOX7_POST_FX");
      post_fx != nullptr && post_fx[0] != '\0') {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_caps = nullptr;
    {
      AdapterState& state = State();
      std::lock_guard<std::mutex> lock(state.mutex);
      host_caps = state.host_surface_capabilities;
    }
    const VkPhysicalDevice physical_device =
        HostDispatchForDevice(device).physical_device;
    VkSurfaceCapabilitiesKHR capabilities{};
    if (host_caps != nullptr && physical_device != VK_NULL_HANDLE &&
        host_caps(physical_device, create_info->surface, &capabilities) ==
            VK_SUCCESS &&
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
      host_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
  }
  SurfaceFullScreenExclusiveInfo composited{
      VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT, host_info.pNext,
      kFullScreenExclusiveDisallowed};
  if (HostDispatchForDevice(device).full_screen_exclusive &&
      FindFeature(host_info.pNext,
                  VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT) ==
          nullptr) {
    host_info.pNext = &composited;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
      std::fprintf(stderr,
                   "  [vulkan] fullscreen stays composited, exclusive mode "
                   "disallowed\n");
    }
  }
  VkSwapchainKHR chained = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.retired_swapchain != VK_NULL_HANDLE &&
        create_info->oldSwapchain == VK_NULL_HANDLE &&
        state.retired_swapchain_device == device &&
        state.retired_swapchain_surface == create_info->surface) {
      chained = state.retired_swapchain;
      state.retired_swapchain = VK_NULL_HANDLE;
      state.retired_swapchain_device = VK_NULL_HANDLE;
      state.retired_swapchain_surface = VK_NULL_HANDLE;
    }
  }
  if (chained == VK_NULL_HANDLE) {
    DestroyRetiredSwapchain();
  }
  if (chained != VK_NULL_HANDLE) {
    host_info.oldSwapchain = chained;
  }
  VkResult result = host_create(device, &host_info, allocator, swapchain);
  if (chained != VK_NULL_HANDLE) {
    if (const auto destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
            HostDeviceProc(device, "vkDestroySwapchainKHR"))) {
      destroy(device, chained, nullptr);
    }
    if (result != VK_SUCCESS) {
      host_info.oldSwapchain = VK_NULL_HANDLE;
      result = host_create(device, &host_info, allocator, swapchain);
    }
  }
  if (result != VK_SUCCESS) {
    NoteDeviceResult(device, result, false);
    return NoteSurfaceResult(create_info->surface, result);
  }
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.swapchain_surfaces[*swapchain] = create_info->surface;
  }
  const auto host_get_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
      HostDeviceProc(device, "vkGetSwapchainImagesKHR"));
  std::uint32_t image_count = 0;
  if (host_get_images == nullptr ||
      host_get_images(device, *swapchain, &image_count, nullptr) !=
          VK_SUCCESS ||
      image_count == 0) {
    return result;
  }
  std::vector<VkImage> images(image_count);
  const VkResult images_result =
      host_get_images(device, *swapchain, &image_count, images.data());
  if (images_result == VK_SUCCESS || images_result == VK_INCOMPLETE) {
    images.resize(image_count);
    (void)State().text_overlay.RegisterSwapchain(
        device, *swapchain, host_info, images.data(), image_count);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks* allocator) {
  if (State().capture.DestroySwapchain(device, swapchain)) {
    return;
  }
  const auto host_destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
      HostDeviceProc(device, "vkDestroySwapchainKHR"));
  State().text_overlay.DestroySwapchain(device, swapchain);
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (const auto hidden = state.hidden_images.find(swapchain);
        hidden != state.hidden_images.end()) {
      state.hidden_image_count.fetch_sub(hidden->second.size(),
                                         std::memory_order_release);
      state.hidden_images.erase(hidden);
    }
    if (const auto found = state.swapchain_surfaces.find(swapchain);
        found != state.swapchain_surfaces.end()) {
      surface = found->second;
      state.swapchain_surfaces.erase(found);
    }
  }
  if (KeepVulkanObjects() && allocator == nullptr &&
      surface != VK_NULL_HANDLE) {
    DestroyRetiredSwapchain();
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.retired_swapchain = swapchain;
    state.retired_swapchain_device = device;
    state.retired_swapchain_surface = surface;
    return;
  }
  if (host_destroy != nullptr) {
    host_destroy(device, swapchain, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index) {
  VulkanCallObservation observation("vkAcquireNextImageKHR");
  VkResult captured = VK_SUCCESS;
  if (State().capture.AcquireImage(device, swapchain, timeout, semaphore, fence,
                                   image_index, &captured)) {
    observation.SetResult(captured);
    return captured;
  }
  VkResult kept = VK_SUCCESS;
  if (TakeHiddenImage(device, swapchain, semaphore, fence, image_index,
                      &kept)) {
    NoteDeviceResult(device, kept, true);
    observation.SetResult(kept);
    return kept;
  }
  const PFN_vkAcquireNextImageKHR host_acquire =
      HostDispatchForDevice(device).acquire_next_image;
  if (host_acquire == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult host_result = host_acquire(
      device, swapchain, timeout, semaphore, fence, image_index);
  const VkResult result = NormalizeSwapchainResult(host_result);
  NoteSwapchainResult(swapchain, result);
  NoteDeviceResult(device, result, true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* acquire_info,
    std::uint32_t* image_index) {
  VulkanCallObservation observation("vkAcquireNextImage2KHR");
  VkResult captured = VK_SUCCESS;
  if (acquire_info != nullptr &&
      State().capture.AcquireImage(device, acquire_info->swapchain,
                                   acquire_info->timeout,
                                   acquire_info->semaphore, acquire_info->fence,
                                   image_index, &captured)) {
    observation.SetResult(captured);
    return captured;
  }
  const PFN_vkAcquireNextImage2KHR host_acquire =
      HostDispatchForDevice(device).acquire_next_image2;
  if (host_acquire == nullptr || acquire_info == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult kept = VK_SUCCESS;
  if (TakeHiddenImage(device, acquire_info->swapchain, acquire_info->semaphore,
                      acquire_info->fence, image_index, &kept)) {
    NoteDeviceResult(device, kept, true);
    observation.SetResult(kept);
    return kept;
  }
  const VkResult host_result =
      host_acquire(device, acquire_info, image_index);
  const VkResult result = NormalizeSwapchainResult(host_result);
  NoteSwapchainResult(acquire_info->swapchain, result);
  NoteDeviceResult(device, result, true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences,
    VkBool32 wait_all, std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitForFences");
  const PFN_vkWaitForFences host_wait =
      HostDispatchForDevice(device).wait_for_fences;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = NoteDeviceResult(
      device, host_wait(device, fence_count, fences, wait_all, timeout), false);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences) {
  VulkanCallObservation observation("vkResetFences");
  const PFN_vkResetFences host_reset =
      HostDispatchForDevice(device).reset_fences;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_reset(device, fence_count, fences);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags) {
  VulkanCallObservation observation("vkResetCommandPool");
  const PFN_vkResetCommandPool host_reset =
      HostDispatchForDevice(device).reset_command_pool;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  ReleaseEtc2PoolCommandBuffers(device, command_pool);
  const VkResult result = host_reset(device, command_pool, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice device, VkQueryPool query_pool, std::uint32_t first_query,
    std::uint32_t query_count, std::size_t data_size, void* data,
    VkDeviceSize stride, VkQueryResultFlags flags) {
  VulkanCallObservation observation("vkGetQueryPoolResults");
  const PFN_vkGetQueryPoolResults host_get_results =
      HostDispatchForDevice(device).get_query_pool_results;
  if (host_get_results == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_get_results(device, query_pool, first_query,
                                           query_count, data_size, data,
                                           stride, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* allocate_info,
    VkCommandBuffer* command_buffers) {
  VulkanCallObservation observation("vkAllocateCommandBuffers");
  const PFN_vkAllocateCommandBuffers host_allocate =
      HostDispatchForDevice(device).allocate_command_buffers;
  if (host_allocate == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      host_allocate(device, allocate_info, command_buffers);
  if (result == VK_SUCCESS && allocate_info != nullptr &&
      command_buffers != nullptr) {
    RegisterHostCommandBuffers(device, allocate_info->commandPool,
                               allocate_info->commandBufferCount,
                               command_buffers);
  }
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool,
    std::uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers) {
  VulkanCallObservation observation("vkFreeCommandBuffers");
  const PFN_vkFreeCommandBuffers host_free =
      HostDispatchForDevice(device).free_command_buffers;
  if (host_free == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return;
  }
  if (command_buffers != nullptr) {
    for (std::uint32_t index = 0; index < command_buffer_count; ++index) {
      State().etc2.ReleaseCommandBuffer(command_buffers[index]);
    }
  }
  host_free(device, command_pool, command_buffer_count, command_buffers);
  RemoveHostCommandBuffers(device, command_pool, command_buffer_count,
                           command_buffers);
  observation.SetResult(VK_SUCCESS);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool command_pool,
    const VkAllocationCallbacks* allocator) {
  VulkanCallObservation observation("vkDestroyCommandPool");
  const PFN_vkDestroyCommandPool host_destroy =
      HostDispatchForDevice(device).destroy_command_pool;
  if (host_destroy == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return;
  }
  ReleaseEtc2PoolCommandBuffers(device, command_pool);
  host_destroy(device, command_pool, allocator);
  RemoveHostCommandPoolBindings(device, command_pool);
  observation.SetResult(VK_SUCCESS);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo* begin_info) {
  VulkanCallObservation observation("vkBeginCommandBuffer");
  const PFN_vkBeginCommandBuffer host_begin =
      HostDispatchForCommandBuffer(command_buffer).begin_command_buffer;
  if (host_begin == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  State().etc2.ReleaseCommandBuffer(command_buffer);
  const VkResult result = host_begin(command_buffer, begin_info);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer) {
  VulkanCallObservation observation("vkEndCommandBuffer");
  const PFN_vkEndCommandBuffer host_end =
      HostDispatchForCommandBuffer(command_buffer).end_command_buffer;
  if (host_end == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_end(command_buffer);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags) {
  VulkanCallObservation observation("vkResetCommandBuffer");
  const PFN_vkResetCommandBuffer host_reset =
      HostDispatchForCommandBuffer(command_buffer).reset_command_buffer;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  State().etc2.ReleaseCommandBuffer(command_buffer);
  const VkResult result = host_reset(command_buffer, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphores");
  const PFN_vkWaitSemaphores host_wait =
      HostDispatchForDevice(device).wait_semaphores;
  return WaitForSemaphoresObserved("vkWaitSemaphores", host_wait, device,
                                   wait_info, timeout, &observation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphoresKHR");
  const PFN_vkWaitSemaphoresKHR host_wait =
      HostDispatchForDevice(device).wait_semaphores_khr;
  return WaitForSemaphoresObserved(
      "vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkWaitSemaphores>(host_wait),
      device, wait_info, timeout, &observation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit");
  const PFN_vkQueueSubmit host_submit =
      HostDispatchForQueue(queue).queue_submit;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (const VkResult signaled =
          State().capture.BeforeSubmit(queue, submit_count, submits);
      signaled != VK_SUCCESS) {
    observation.SetResult(signaled);
    return signaled;
  }
  if (submits != nullptr && State().etc2.HasPendingUploads()) {
    if (submit_count == 1) {
      State().etc2.PrepareSubmit(queue, submits->pCommandBuffers,
                                 submits->commandBufferCount);
    } else {
      std::vector<VkCommandBuffer> command_buffers;
      for (std::uint32_t index = 0; index < submit_count; ++index) {
        if (submits[index].commandBufferCount != 0) {
          command_buffers.insert(command_buffers.end(),
                                   submits[index].pCommandBuffers,
                                   submits[index].pCommandBuffers +
                                       submits[index].commandBufferCount);
        }
      }
      State().etc2.PrepareSubmit(
          queue, command_buffers.data(),
          static_cast<std::uint32_t>(command_buffers.size()));
    }
  }
  const VkResult result = NoteQueueResult(
      queue,
      State().text_overlay.QueueSubmit(queue, submit_count, submits, fence,
                                       host_submit),
      true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit2");
  const PFN_vkQueueSubmit2 host_submit =
      HostDispatchForQueue(queue).queue_submit2;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (const VkResult signaled =
          State().capture.BeforeSubmit2(queue, submit_count, submits);
      signaled != VK_SUCCESS) {
    observation.SetResult(signaled);
    return signaled;
  }
  PrepareEtc2Submit2(queue, submit_count, submits);
  const VkResult result = NoteQueueResult(
      queue,
      State().text_overlay.QueueSubmit2(queue, submit_count, submits, fence,
                                        host_submit),
      true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit2KHR");
  const PFN_vkQueueSubmit2KHR host_submit =
      HostDispatchForQueue(queue).queue_submit2_khr;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (const VkResult signaled =
          State().capture.BeforeSubmit2(queue, submit_count, submits);
      signaled != VK_SUCCESS) {
    observation.SetResult(signaled);
    return signaled;
  }
  PrepareEtc2Submit2(queue, submit_count, submits);
  const VkResult result = NoteQueueResult(
      queue,
      State().text_overlay.QueueSubmit2(
          queue, submit_count, submits, fence,
          reinterpret_cast<PFN_vkQueueSubmit2>(host_submit)),
      true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info, VkFence fence) {
  VulkanCallObservation observation("vkQueueBindSparse");
  const PFN_vkQueueBindSparse host_bind =
      HostDispatchForQueue(queue).queue_bind_sparse;
  if (host_bind == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (const VkResult signaled =
          State().capture.BeforeBindSparse(queue, bind_info_count, bind_info);
      signaled != VK_SUCCESS) {
    observation.SetResult(signaled);
    return signaled;
  }
  const VkResult result = NoteQueueResult(
      queue,
      State().text_overlay.QueueBindSparse(queue, bind_info_count, bind_info,
                                           fence, host_bind),
      false);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  VulkanCallObservation observation("vkQueueWaitIdle");
  const PFN_vkQueueWaitIdle host_wait =
      HostDispatchForQueue(queue).queue_wait_idle;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = NoteQueueResult(
      queue, State().text_overlay.QueueWaitIdle(queue, host_wait), false);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  VulkanCallObservation observation("vkDeviceWaitIdle");
  const PFN_vkDeviceWaitIdle host_wait =
      HostDispatchForDevice(device).device_wait_idle;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = NoteDeviceResult(
      device, State().text_overlay.DeviceWaitIdle(device, host_wait), false);
  observation.SetResult(result);
  return result;
}

namespace {

void RememberShownCapabilities(VkSurfaceKHR surface, VkResult result,
                               const VkSurfaceCapabilitiesKHR& capabilities) {
  if (result != VK_SUCCESS || capabilities.currentExtent.width == 0 ||
      capabilities.currentExtent.height == 0) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.shown_surface = surface;
  state.shown_capabilities = capabilities;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SurfaceCapabilities(surface, capabilities, &captured)) {
    return captured;
  }
  const WindowHoldsPresentsFn hides =
      State().window_hides_presents.load(std::memory_order_acquire);
  if (hides != nullptr && capabilities != nullptr && hides()) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.shown_surface == surface) {
      *capabilities = state.shown_capabilities;
      return VK_SUCCESS;
    }
  }
  WaitWhileWindowHoldsPresents();
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_capabilities = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_capabilities = state.host_surface_capabilities;
  }
  if (host_capabilities == nullptr || capabilities == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = NoteSurfaceResult(
      surface, host_capabilities(physical_device, surface, capabilities));
  if (result != VK_SUCCESS ||
      (capabilities->currentExtent.width != UINT32_MAX &&
       capabilities->currentExtent.height != UINT32_MAX)) {
    RememberShownCapabilities(surface, result, *capabilities);
    return result;
  }

  const WindowDimensionFn window_width =
      ResolveProcessFunction<WindowDimensionFn>("blox7_window_width");
  const WindowDimensionFn window_height =
      ResolveProcessFunction<WindowDimensionFn>("blox7_window_height");
  if (window_width == nullptr || window_height == nullptr ||
      window_width() <= 0 || window_height() <= 0) {
    return result;
  }
  capabilities->currentExtent.width = std::clamp(
      static_cast<std::uint32_t>(window_width()),
      capabilities->minImageExtent.width, capabilities->maxImageExtent.width);
  capabilities->currentExtent.height = std::clamp(
      static_cast<std::uint32_t>(window_height()),
      capabilities->minImageExtent.height, capabilities->maxImageExtent.height);

  RememberShownCapabilities(surface, result, *capabilities);
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.extent_translation_logged) {
    state.extent_translation_logged = true;
    std::fprintf(
        stderr, "  [vulkan] Android currentExtent translated to %ux%u\n",
        capabilities->currentExtent.width, capabilities->currentExtent.height);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* present_mode_count, VkPresentModeKHR* present_modes) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.PresentModes(surface, present_mode_count, present_modes,
                                   &captured)) {
    return captured;
  }
  if (present_mode_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_present_modes;
  }
  if (host_query == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::uint32_t host_count = 0;
  VkResult result = host_query(physical_device, surface, &host_count, nullptr);
  if (result != VK_SUCCESS) {
    return NoteSurfaceResult(surface, result);
  }
  std::vector<VkPresentModeKHR> host_modes(host_count);
  if (host_count != 0) {
    result =
        host_query(physical_device, surface, &host_count, host_modes.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      return NoteSurfaceResult(surface, result);
    }
    host_modes.resize(host_count);
  }
  const blox7::graphics::PresentModePolicy policy = blox7::graphics::CachedPresentModePolicy();
  const std::vector<VkPresentModeKHR> visible =
      blox7::graphics::FilterPresentModes(policy, host_modes);
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.present_policy_logged) {
      state.present_policy_logged = true;
      std::fprintf(stderr, "  [vulkan] present policy=%s modes=%zu\n",
                   blox7::graphics::PresentModePolicyName(policy),
                   visible.size());
    }
  }
  if (present_modes == nullptr) {
    *present_mode_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t capacity = *present_mode_count;
  const std::uint32_t copied =
      std::min(capacity, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, present_modes);
  *present_mode_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physical_device, std::uint32_t queue_family_index,
    VkSurfaceKHR surface, VkBool32* supported) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SurfaceSupport(physical_device, queue_family_index,
                                     surface, supported, &captured)) {
    return captured;
  }
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_support;
  }
  return host_query != nullptr
             ? NoteSurfaceResult(surface,
                                 host_query(physical_device, queue_family_index,
                                            surface, supported))
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* format_count, VkSurfaceFormatKHR* formats) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SurfaceFormats(surface, format_count, formats,
                                     &captured)) {
    return captured;
  }
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_formats;
  }
  return host_query != nullptr
             ? NoteSurfaceResult(surface, host_query(physical_device, surface,
                                                     format_count, formats))
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
    std::uint32_t* format_count, VkSurfaceFormat2KHR* formats) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SurfaceFormats2(surface_info, format_count, formats,
                                      &captured)) {
    return captured;
  }
  PFN_vkGetPhysicalDeviceSurfaceFormats2KHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_formats2;
  }
  return host_query != nullptr
             ? NoteSurfaceResult(SurfaceOf(surface_info),
                                 host_query(physical_device, surface_info,
                                            format_count, formats))
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceSurfaceInfo2KHR* surface_info,
    VkSurfaceCapabilities2KHR* capabilities) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SurfaceCapabilities2(surface_info, capabilities,
                                           &captured)) {
    return captured;
  }
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_capabilities2;
  }
  return host_query != nullptr
             ? NoteSurfaceResult(SurfaceOf(surface_info),
                                 host_query(physical_device, surface_info,
                                            capabilities))
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* image_count,
    VkImage* images) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.SwapchainImages(device, swapchain, image_count, images,
                                      &captured)) {
    return captured;
  }
  const PFN_vkGetSwapchainImagesKHR host_get =
      HostDispatchForDevice(device).get_swapchain_images;
  return host_get != nullptr ? host_get(device, swapchain, image_count, images)
                             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRefreshCycleDurationGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain,
    VkRefreshCycleDurationGOOGLE* display_timing_properties) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.RefreshCycle(device, swapchain, display_timing_properties,
                                   &captured)) {
    return captured;
  }
  const PFN_vkGetRefreshCycleDurationGOOGLE host_get =
      HostDispatchForDevice(device).get_refresh_cycle_duration;
  return host_get != nullptr
             ? host_get(device, swapchain, display_timing_properties)
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain,
    std::uint32_t* presentation_timing_count,
    VkPastPresentationTimingGOOGLE* presentation_timings) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.PastPresentationTiming(device, swapchain,
                                             presentation_timing_count,
                                             presentation_timings, &captured)) {
    return captured;
  }
  const PFN_vkGetPastPresentationTimingGOOGLE host_get =
      HostDispatchForDevice(device).get_past_presentation_timing;
  return host_get != nullptr
             ? host_get(device, swapchain, presentation_timing_count,
                        presentation_timings)
             : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {
  VkResult captured = VK_SUCCESS;
  if (State().capture.QueuePresent(queue, present_info, vkQueuePresentKHR,
                                   &captured)) {
    return captured;
  }
  VkResult kept = VK_SUCCESS;
  if (KeepHiddenPresent(queue, present_info, &kept)) {
    return NoteQueueResult(queue, kept, true);
  }
  WaitWhileWindowHoldsPresents();
  VulkanCallObservation observation("vkQueuePresentKHR(adapter)");
  AdapterState& state = State();
  const PFN_vkQueuePresentKHR host_present =
      HostDispatchForQueue(queue).queue_present;
  const NotePresentFn note_present =
      state.note_present.load(std::memory_order_acquire);
  if (host_present == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = state.text_overlay.QueuePresent(
      queue, present_info, ObservedHostQueuePresent);
  const VkResult normalized_result = NormalizeSwapchainResult(result);
  if (normalized_result == VK_ERROR_SURFACE_LOST_KHR &&
      present_info != nullptr) {
    for (std::uint32_t index = 0; index < present_info->swapchainCount;
         ++index) {
      if (present_info->pResults == nullptr ||
          present_info->pResults[index] == VK_ERROR_SURFACE_LOST_KHR) {
        NoteSwapchainResult(present_info->pSwapchains[index],
                            VK_ERROR_SURFACE_LOST_KHR);
      }
    }
  } else {
    NoteSwapchainResult(VK_NULL_HANDLE, normalized_result);
  }
  NoteQueueResult(queue, normalized_result, true);
  if (normalized_result == VK_SUCCESS && note_present != nullptr) {
    note_present();
  }
  if (present_info != nullptr && present_info->pResults != nullptr) {
    for (std::uint32_t index = 0; index < present_info->swapchainCount;
         ++index) {
      present_info->pResults[index] =
          NormalizeSwapchainResult(present_info->pResults[index]);
    }
  }
  observation.SetResult(normalized_result);
  return normalized_result;
}

namespace {

bool DevicePicked() {
  const char* picked = std::getenv("BLOX7_VULKAN_DEVICE");
  return picked != nullptr && picked[0] != '\0';
}

// roblox grabs the first gpu it sees, so it only gets to see one
bool PickedDeviceVisible(VkInstance instance, VkPhysicalDevice device) {
  static const std::pair<unsigned, unsigned> picked = [] {
    unsigned vendor = 0, id = 0;
    const char* value = std::getenv("BLOX7_VULKAN_DEVICE");
    if (value == nullptr || std::sscanf(value, "%x:%x", &vendor, &id) != 2) {
      return std::pair<unsigned, unsigned>{0, 0};
    }
    return std::pair<unsigned, unsigned>{vendor, id};
  }();
  const auto properties_of = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      HostInstanceProc(instance, "vkGetPhysicalDeviceProperties"));
  if (picked.first == 0 || properties_of == nullptr) {
    return true;
  }
  VkPhysicalDeviceProperties properties{};
  properties_of(device, &properties);
  return properties.vendorID == picked.first &&
         properties.deviceID == picked.second;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* device_count,
    VkPhysicalDevice* devices) {
  const auto host = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      HostInstanceProc(instance, "vkEnumeratePhysicalDevices"));
  if (host == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!DevicePicked() || device_count == nullptr) {
    const VkResult result = host(instance, device_count, devices);
    if (result >= VK_SUCCESS && device_count != nullptr) {
      NotePhysicalDevices(instance, devices, *device_count);
    }
    return result;
  }
  std::uint32_t total = 0;
  VkResult result = host(instance, &total, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }
  std::vector<VkPhysicalDevice> all(total);
  result = host(instance, &total, all.data());
  if (result < VK_SUCCESS) {
    return result;
  }
  all.resize(total);
  std::vector<VkPhysicalDevice> visible;
  for (VkPhysicalDevice device : all) {
    if (PickedDeviceVisible(instance, device)) {
      visible.push_back(device);
    }
  }
  if (visible.empty()) {
    visible = all;
  }
  if (devices == nullptr) {
    *device_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t copied =
      std::min<std::uint32_t>(*device_count, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, devices);
  *device_count = copied;
  NotePhysicalDevices(instance, devices, copied);
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, std::uint32_t* group_count,
    VkPhysicalDeviceGroupProperties* groups) {
  auto host = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
      HostInstanceProc(instance, "vkEnumeratePhysicalDeviceGroups"));
  if (host == nullptr) {
    host = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(
        HostInstanceProc(instance, "vkEnumeratePhysicalDeviceGroupsKHR"));
  }
  if (host == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!DevicePicked() || group_count == nullptr) {
    return host(instance, group_count, groups);
  }
  std::uint32_t total = 0;
  VkResult result = host(instance, &total, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkPhysicalDeviceGroupProperties blank{};
  blank.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
  std::vector<VkPhysicalDeviceGroupProperties> all(total, blank);
  result = host(instance, &total, all.data());
  if (result < VK_SUCCESS) {
    return result;
  }
  all.resize(total);
  std::vector<VkPhysicalDeviceGroupProperties> visible;
  for (const VkPhysicalDeviceGroupProperties& group : all) {
    if (group.physicalDeviceCount > 0 &&
        PickedDeviceVisible(instance, group.physicalDevices[0])) {
      visible.push_back(group);
    }
  }
  if (visible.empty()) {
    visible = all;
  }
  if (groups == nullptr) {
    *group_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t copied =
      std::min<std::uint32_t>(*group_count, static_cast<std::uint32_t>(visible.size()));
  for (std::uint32_t index = 0; index < copied; ++index) {
    groups[index].physicalDeviceCount = visible[index].physicalDeviceCount;
    std::copy_n(visible[index].physicalDevices, VK_MAX_DEVICE_GROUP_SIZE,
                groups[index].physicalDevices);
    groups[index].subsetAllocation = visible[index].subsetAllocation;
  }
  *group_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* layer_name,
    std::uint32_t* property_count, VkExtensionProperties* properties) {
  const auto host =
      HostPhysicalDeviceProc<PFN_vkEnumerateDeviceExtensionProperties>(
          "vkEnumerateDeviceExtensionProperties", nullptr);
  if (host == nullptr || property_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (layer_name != nullptr) {
    return host(physical_device, layer_name, property_count, properties);
  }
  std::uint32_t total = 0;
  VkResult result = host(physical_device, nullptr, &total, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }
  std::vector<VkExtensionProperties> visible(total);
  result = host(physical_device, nullptr, &total, visible.data());
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    return result;
  }
  visible.resize(total);
  visible.erase(std::remove_if(visible.begin(), visible.end(),
                               [](const VkExtensionProperties& extension) {
                                 return IsHostOnlyDeviceExtension(
                                     extension.extensionName);
                               }),
                visible.end());
  if (properties == nullptr) {
    *property_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t copied = std::min<std::uint32_t>(
      *property_count, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, properties);
  *property_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures* features) {
  const auto host = HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFeatures>(
      "vkGetPhysicalDeviceFeatures", nullptr);
  if (host == nullptr || features == nullptr) {
    return;
  }
  host(physical_device, features);
  if (Etc2EmulatedPhysicalDevice(physical_device) &&
      blox7::graphics::Etc2SupportAdvertised()) {
    features->textureCompressionETC2 = VK_TRUE;
  }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2* features) {
  const auto host = HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFeatures2>(
      "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR");
  if (host == nullptr || features == nullptr) {
    return;
  }
  host(physical_device, features);
  if (Etc2EmulatedPhysicalDevice(physical_device) &&
      blox7::graphics::Etc2SupportAdvertised()) {
    features->features.textureCompressionETC2 = VK_TRUE;
  }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties* properties) {
  const auto host =
      HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFormatProperties>(
          "vkGetPhysicalDeviceFormatProperties", nullptr);
  if (host == nullptr || properties == nullptr) {
    return;
  }
  blox7::graphics::Etc2EmulatedFormat mapped;
  if (!blox7::graphics::LookupEmulatedEtc2Format(format, &mapped) ||
      !Etc2EmulatedPhysicalDevice(physical_device)) {
    host(physical_device, format, properties);
    return;
  }
  host(physical_device, Etc2HostFormat(physical_device, mapped), properties);
  properties->linearTilingFeatures = 0;
  properties->optimalTilingFeatures =
      blox7::graphics::EmulatedEtc2FormatFeatures(
          properties->optimalTilingFeatures);
  properties->bufferFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatProperties2* properties) {
  const auto host =
      HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceFormatProperties2>(
          "vkGetPhysicalDeviceFormatProperties2",
          "vkGetPhysicalDeviceFormatProperties2KHR");
  if (host == nullptr || properties == nullptr) {
    return;
  }
  blox7::graphics::Etc2EmulatedFormat mapped;
  if (!blox7::graphics::LookupEmulatedEtc2Format(format, &mapped) ||
      !Etc2EmulatedPhysicalDevice(physical_device)) {
    host(physical_device, format, properties);
    return;
  }
  host(physical_device, Etc2HostFormat(physical_device, mapped), properties);
  VkFormatProperties& base = properties->formatProperties;
  base.linearTilingFeatures = 0;
  base.optimalTilingFeatures =
      blox7::graphics::EmulatedEtc2FormatFeatures(base.optimalTilingFeatures);
  base.bufferFeatures = 0;
  if (auto* extended = const_cast<VkFormatProperties3*>(
          reinterpret_cast<const VkFormatProperties3*>(FindFeature(
              properties->pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)));
      extended != nullptr) {
    extended->linearTilingFeatures = 0;
    extended->optimalTilingFeatures =
        blox7::graphics::EmulatedEtc2FormatFeatures(
            static_cast<VkFormatFeatureFlags>(
                extended->optimalTilingFeatures));
    extended->bufferFeatures = 0;
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physical_device, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
    VkImageFormatProperties* properties) {
  const auto host =
      HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceImageFormatProperties>(
          "vkGetPhysicalDeviceImageFormatProperties", nullptr);
  if (host == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  blox7::graphics::Etc2EmulatedFormat mapped;
  if (!blox7::graphics::LookupEmulatedEtc2Format(format, &mapped) ||
      !Etc2EmulatedPhysicalDevice(physical_device)) {
    return host(physical_device, format, type, tiling, usage, flags,
                properties);
  }
  if (!EmulatedEtc2ImageSupported(type, tiling, usage, flags)) {
    if (properties != nullptr) {
      *properties = {};
    }
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  const VkResult result =
      host(physical_device, Etc2HostFormat(physical_device, mapped), type,
           tiling, usage, flags, properties);
  if (result == VK_SUCCESS && properties != nullptr) {
    properties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceImageFormatInfo2* info,
    VkImageFormatProperties2* properties) {
  const auto host =
      HostPhysicalDeviceProc<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
          "vkGetPhysicalDeviceImageFormatProperties2",
          "vkGetPhysicalDeviceImageFormatProperties2KHR");
  if (host == nullptr || info == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  blox7::graphics::Etc2EmulatedFormat mapped;
  if (!blox7::graphics::LookupEmulatedEtc2Format(info->format, &mapped) ||
      !Etc2EmulatedPhysicalDevice(physical_device)) {
    return host(physical_device, info, properties);
  }
  if (!EmulatedEtc2ImageSupported(info->type, info->tiling, info->usage,
                                  info->flags)) {
    if (properties != nullptr) {
      properties->imageFormatProperties = {};
    }
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  VkPhysicalDeviceImageFormatInfo2 host_info = *info;
  host_info.format = Etc2HostFormat(physical_device, mapped);
  const VkResult result = host(physical_device, &host_info, properties);
  if (result == VK_SUCCESS && properties != nullptr) {
    properties->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
  return State().etc2.CreateImage(device, create_info, allocator, image);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
  State().etc2.DestroyImage(device, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
  return State().etc2.CreateImageView(device, create_info, allocator, view);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    VkMemoryRequirements2* requirements) {
  State().etc2.GetDeviceImageMemoryRequirements(device, info, requirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements* info,
    std::uint32_t* count, VkSparseImageMemoryRequirements2* requirements) {
  State().etc2.GetDeviceImageSparseMemoryRequirements(device, info, count,
                                                      requirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayout(
    VkDevice device, const VkDeviceImageSubresourceInfo* info,
    VkSubresourceLayout2* layout) {
  State().etc2.GetDeviceImageSubresourceLayout(device, info, layout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device,
                                                  VkBuffer buffer,
                                                  VkDeviceMemory memory,
                                                  VkDeviceSize offset) {
  return State().etc2.BindBufferMemory(device, buffer, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(
    VkDevice device, std::uint32_t count,
    const VkBindBufferMemoryInfo* infos) {
  return State().etc2.BindBufferMemory2(device, count, infos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device,
                                           VkDeviceMemory memory,
                                           VkDeviceSize offset,
                                           VkDeviceSize size,
                                           VkMemoryMapFlags flags,
                                           void** data) {
  return State().etc2.MapMemory(device, memory, offset, size, flags, data);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2(VkDevice device,
                                            const VkMemoryMapInfo* info,
                                            void** data) {
  return State().etc2.MapMemory2(device, info, data);
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device,
                                         VkDeviceMemory memory) {
  State().etc2.UnmapMemory(device, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2(VkDevice device,
                                              const VkMemoryUnmapInfo* info) {
  return State().etc2.UnmapMemory2(device, info);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
  const PFN_vkAllocateMemory host = HostDispatchForDevice(device).allocate_memory;
  if (host == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host(device, info, allocator, memory);
  if (result == VK_SUCCESS && KeepVulkanObjects()) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.device_allocations[device];
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator) {
  State().etc2.FreeMemory(device, memory, allocator);
  if (memory != VK_NULL_HANDLE && KeepVulkanObjects()) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (const auto found = state.device_allocations.find(device);
        found != state.device_allocations.end() && found->second > 0) {
      --found->second;
    }
  }
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator) {
  State().etc2.DestroyBuffer(device, buffer, allocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer command_buffer, VkBuffer source, VkImage destination,
    VkImageLayout layout, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  State().etc2.CmdCopyBufferToImage(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      source, destination, layout, region_count, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(
    VkCommandBuffer command_buffer, const VkCopyBufferToImageInfo2* info) {
  State().etc2.CmdCopyBufferToImage2(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      info);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(
    VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t region_count,
    const VkImageCopy* regions) {
  State().etc2.CmdCopyImage(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      source, source_layout, destination, destination_layout, region_count,
      regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer command_buffer,
                                           const VkCopyImageInfo2* info) {
  State().etc2.CmdCopyImage2(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      info);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(
    VkCommandBuffer command_buffer, VkImage source, VkImageLayout layout,
    VkBuffer destination, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  State().etc2.CmdCopyImageToBuffer(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      source, layout, destination, region_count, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(
    VkCommandBuffer command_buffer, const VkCopyImageToBufferInfo2* info) {
  State().etc2.CmdCopyImageToBuffer2(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      info);
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer command_buffer, std::uint32_t count,
    const VkCommandBuffer* secondaries) {
  State().etc2.CmdExecuteCommands(
      HostDispatchForCommandBuffer(command_buffer).device, command_buffer,
      count, secondaries);
}

}  // extern "C"
