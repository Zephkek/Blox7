#ifndef BLOX7_GRAPHICS_VULKAN_CAPTURE_SWAPCHAIN_H_
#define BLOX7_GRAPHICS_VULKAN_CAPTURE_SWAPCHAIN_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

namespace blox7 {
namespace graphics {

class VulkanCaptureSwapchains final {
 public:
  VulkanCaptureSwapchains();
  ~VulkanCaptureSwapchains();

  VulkanCaptureSwapchains(const VulkanCaptureSwapchains&) = delete;
  VulkanCaptureSwapchains& operator=(const VulkanCaptureSwapchains&) = delete;

  void RegisterDevice(VkInstance instance, VkPhysicalDevice physical_device,
                      VkDevice device,
                      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr,
                      VkQueue spare_queue, std::mutex* spare_queue_mutex);
  void DestroyDevice(VkDevice device);
  void RegisterQueue(VkDevice device, VkQueue queue, std::uint32_t family);

  bool CreateSurface(const void* create_info, VkSurfaceKHR* surface,
                     VkResult* result);
  bool DestroySurface(VkSurfaceKHR surface);
  bool SurfaceSupport(VkPhysicalDevice physical_device, std::uint32_t family,
                      VkSurfaceKHR surface, VkBool32* supported,
                      VkResult* result);
  bool SurfaceCapabilities(VkSurfaceKHR surface,
                           VkSurfaceCapabilitiesKHR* capabilities,
                           VkResult* result);
  bool SurfaceCapabilities2(const VkPhysicalDeviceSurfaceInfo2KHR* info,
                            VkSurfaceCapabilities2KHR* capabilities,
                            VkResult* result);
  bool SurfaceFormats(VkSurfaceKHR surface, std::uint32_t* count,
                      VkSurfaceFormatKHR* formats, VkResult* result);
  bool SurfaceFormats2(const VkPhysicalDeviceSurfaceInfo2KHR* info,
                       std::uint32_t* count, VkSurfaceFormat2KHR* formats,
                       VkResult* result);
  bool PresentModes(VkSurfaceKHR surface, std::uint32_t* count,
                    VkPresentModeKHR* modes, VkResult* result);

  bool CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                       VkSwapchainKHR* swapchain, VkResult* result);
  bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);
  bool SwapchainImages(VkDevice device, VkSwapchainKHR swapchain,
                       std::uint32_t* count, VkImage* images, VkResult* result);
  bool AcquireImage(VkDevice device, VkSwapchainKHR swapchain,
                    std::uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                    std::uint32_t* index, VkResult* result);
  bool RefreshCycle(VkDevice device, VkSwapchainKHR swapchain,
                    VkRefreshCycleDurationGOOGLE* duration, VkResult* result);
  bool PastPresentationTiming(VkDevice device, VkSwapchainKHR swapchain,
                              std::uint32_t* count,
                              VkPastPresentationTimingGOOGLE* timings,
                              VkResult* result);

  VkResult BeforeSubmit(VkQueue queue, std::uint32_t submit_count,
                        const VkSubmitInfo* submits);
  VkResult BeforeSubmit2(VkQueue queue, std::uint32_t submit_count,
                         const VkSubmitInfo2* submits);
  VkResult BeforeBindSparse(VkQueue queue, std::uint32_t bind_info_count,
                            const VkBindSparseInfo* bind_info);
  bool QueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info,
                    PFN_vkQueuePresentKHR next, VkResult* result);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace graphics
}  // namespace blox7

#endif  // BLOX7_GRAPHICS_VULKAN_CAPTURE_SWAPCHAIN_H_
