#include "blox7/graphics/roblox_renderer_checks.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace blox7 {
namespace graphics {
namespace {

bool HasDrawingQueue(PFN_vkGetPhysicalDeviceQueueFamilyProperties queues_of,
                     VkPhysicalDevice device) {
  std::uint32_t count = 0;
  queues_of(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  queues_of(device, &count, families.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    if (families[i].queueCount != 0 &&
        (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      return true;
    }
  }
  return false;
}

bool AnyDeviceRobloxPicks(PFN_vkGetInstanceProcAddr get_proc,
                          VkInstance instance) {
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get_proc(instance, "vkEnumeratePhysicalDevices"));
  const auto properties_of =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          get_proc(instance, "vkGetPhysicalDeviceProperties"));
  const auto queues_of =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  if (enumerate == nullptr || properties_of == nullptr ||
      queues_of == nullptr) {
    return false;
  }
  std::uint32_t count = 0;
  if (enumerate(instance, &count, nullptr) != VK_SUCCESS) {
    count = 0;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (count != 0 && enumerate(instance, &count, devices.data()) < VK_SUCCESS) {
    count = 0;
  }
  devices.resize(count);
  if (devices.empty()) {
    std::fprintf(stderr, "  [vulkan] the Vulkan loader lists no devices\n");
    return false;
  }

  unsigned picked_vendor = 0;
  unsigned picked_device = 0;
  const char* picked = std::getenv("BLOX7_VULKAN_DEVICE");
  const bool has_pick =
      picked != nullptr &&
      std::sscanf(picked, "%x:%x", &picked_vendor, &picked_device) == 2 &&
      picked_vendor != 0;
  std::vector<VkPhysicalDeviceProperties> properties(devices.size());
  bool pick_present = false;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    properties_of(devices[i], &properties[i]);
    pick_present |= has_pick && properties[i].vendorID == picked_vendor &&
                    properties[i].deviceID == picked_device;
  }

  bool found = false;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    const VkPhysicalDeviceProperties& device = properties[i];
    const bool hidden = pick_present && (device.vendorID != picked_vendor ||
                                         device.deviceID != picked_device);
    const bool cpu = device.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const bool draws = HasDrawingQueue(queues_of, devices[i]);
    std::fprintf(stderr, "  [vulkan] device %s (%04x:%04x)%s\n",
                 device.deviceName, device.vendorID, device.deviceID,
                 hidden ? ": not the picked gpu"
                 : cpu  ? ": a CPU device, Roblox skips those"
                 : !draws ? ": no queue that draws"
                          : "");
    found |= !hidden && !cpu && draws;
  }
  return found;
}

}  // namespace

bool RobloxAcceptsGlesVersion(const char* gl_version) {
  return gl_version != nullptr &&
         std::strncmp(gl_version, "OpenGL ES ", 10) == 0 &&
         gl_version[10] > '2';
}

bool RobloxFindsVulkanDevice() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return true;
  }
  bool found = false;
  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    std::fprintf(stderr, "  [vulkan] no Vulkan loader: %s\n", SDL_GetError());
  } else {
    const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        SDL_Vulkan_GetVkGetInstanceProcAddr());
    const auto create =
        get_proc != nullptr ? reinterpret_cast<PFN_vkCreateInstance>(
                                  get_proc(VK_NULL_HANDLE, "vkCreateInstance"))
                            : nullptr;
    const auto instance_version =
        get_proc != nullptr
            ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                  get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"))
            : nullptr;
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_0;
    std::uint32_t host_api = VK_API_VERSION_1_0;
    if (instance_version != nullptr &&
        instance_version(&host_api) == VK_SUCCESS &&
        host_api >= VK_API_VERSION_1_2) {
      application.apiVersion = VK_API_VERSION_1_2;
    }
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (create == nullptr || create(&info, nullptr, &instance) != VK_SUCCESS) {
      std::fprintf(stderr,
                   "  [vulkan] the Vulkan loader cannot make an instance\n");
    } else {
      found = AnyDeviceRobloxPicks(get_proc, instance);
      const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
          get_proc(instance, "vkDestroyInstance"));
      if (destroy != nullptr) {
        destroy(instance, nullptr);
      }
    }
    SDL_Vulkan_UnloadLibrary();
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return found;
}

}  // namespace graphics
}  // namespace blox7
