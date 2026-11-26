#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LOSE_SWAPCHAINS 32

struct lose_real {
  VkResult (*acquire)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
  VkResult (*acquire2)(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*);
  VkResult (*present)(VkQueue, const VkPresentInfoKHR*);
  VkResult (*create_swapchain)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
  void (*destroy_swapchain)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
  VkResult (*capabilities)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
  VkResult (*capabilities2)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR*);
  VkResult (*support)(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*);
  VkResult (*formats)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
  VkResult (*formats2)(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t*, VkSurfaceFormat2KHR*);
  VkResult (*present_modes)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
};

static struct lose_real g_lose_real[2];
static volatile int g_lose_lock;
static struct {
  VkSwapchainKHR swapchain;
  VkSurfaceKHR surface;
} g_lose_chains[LOSE_SWAPCHAINS];
static VkSurfaceKHR g_lose_surface;
static int g_lose_read, g_lose_times, g_lose_done, g_lose_all_queries;
static double g_lose_after, g_lose_gap, g_lose_armed, g_lose_next, g_lose_lost;
static unsigned long long g_lose_acquires, g_lose_refused, g_lose_frames;

static double lose_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void lose_config(void) {
  if (g_lose_read) return;
  g_lose_read = 1;
  const char* text = getenv("BLOX7_VK_LOSE_SURFACE");
  if (text == NULL || text[0] == '\0') return;
  char* end = NULL;
  g_lose_after = strtod(text, &end);
  g_lose_times = 1;
  g_lose_gap = 8;
  g_lose_all_queries = 1;
  if (end && *end == ',') g_lose_times = (int)strtol(end + 1, &end, 10);
  if (end && *end == ',') g_lose_gap = strtod(end + 1, &end);
  if (end && *end == ',') g_lose_all_queries = end[1] != 's';
  fprintf(stderr, "[lose surface] after %.1f s, %d times, %.1f s apart, %s\n", g_lose_after, g_lose_times, g_lose_gap,
          g_lose_all_queries ? "every call fails" : "surface queries but the support one still answer");
}

static VkSurfaceKHR lose_chain_surface(VkSwapchainKHR swapchain) {
  for (int i = 0; i < LOSE_SWAPCHAINS; i++)
    if (g_lose_chains[i].swapchain == swapchain) return g_lose_chains[i].surface;
  return VK_NULL_HANDLE;
}

static int lose_refuse(VkSurfaceKHR surface, const char* call) {
  if (surface == VK_NULL_HANDLE) return 0;
  lock(&g_lose_lock);
  const int refused = surface == g_lose_surface;
  const unsigned long long count = refused ? ++g_lose_refused : 0;
  unlock(&g_lose_lock);
  if (count == 1 || count == 100 || count == 1000 || count == 10000 || count == 100000)
    fprintf(stderr, "[lose surface] %llu calls refused on the lost surface, last %s\n", count, call);
  return refused;
}

static int lose_acquire(VkSwapchainKHR swapchain) {
  lose_config();
  lock(&g_lose_lock);
  const VkSurfaceKHR surface = lose_chain_surface(swapchain);
  const unsigned long long acquires = ++g_lose_acquires;
  int lost = 0;
  double after = 0;
  if (surface != VK_NULL_HANDLE && g_lose_surface == VK_NULL_HANDLE && g_lose_done < g_lose_times) {
    const double now = lose_now();
    if (g_lose_next == 0) {
      g_lose_armed = now;
      g_lose_next = now + g_lose_after;
    }
    if (now >= g_lose_next) {
      g_lose_surface = surface;
      g_lose_done++;
      g_lose_refused = 0;
      lost = g_lose_done;
      after = now - g_lose_armed;
      g_lose_lost = now;
    }
  }
  unlock(&g_lose_lock);
  if (lost)
    fprintf(stderr, "[lose surface] loss %d: acquire %llu reports the surface lost, %.1f s in\n", lost, acquires, after);
  return lose_refuse(surface, "vkAcquireNextImageKHR");
}

static void lose_frame(VkResult result) {
  if (result < 0) return;
  lock(&g_lose_lock);
  const unsigned long long frames = g_lose_done && g_lose_surface == VK_NULL_HANDLE ? ++g_lose_frames : 0;
  const int done = g_lose_done;
  const double lost = g_lose_lost;
  unlock(&g_lose_lock);
  if (frames == 1 || frames == 10 || frames == 100 || frames == 1000)
    fprintf(stderr, "[lose surface] loss %d: frame %llu on the next surface, %.0f ms after the loss\n", done, frames,
            (lose_now() - lost) * 1000);
}

void vkb_test_surface_destroyed(VkSurfaceKHR surface) {
  lock(&g_lose_lock);
  const int lost = surface != VK_NULL_HANDLE && surface == g_lose_surface;
  const unsigned long long refused = g_lose_refused;
  const int done = g_lose_done;
  const double since = lose_now() - g_lose_lost;
  if (lost) {
    g_lose_surface = VK_NULL_HANDLE;
    g_lose_frames = 0;
    g_lose_armed = lose_now();
    g_lose_next = g_lose_armed + g_lose_gap;
  }
  unlock(&g_lose_lock);
  if (lost)
    fprintf(stderr, "[lose surface] loss %d: the lost surface was destroyed %.0f ms after the loss, %llu refused calls\n",
            done, since * 1000, refused);
}

#define LOSE_WRAPPERS(k)                                                                                               \
  static VkResult lose_acquire_##k(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, \
                                   VkFence fence, uint32_t* index) {                                                  \
    if (lose_acquire(swapchain)) return VK_ERROR_SURFACE_LOST_KHR;                                                     \
    const VkResult result = g_lose_real[k].acquire(device, swapchain, timeout, semaphore, fence, index);               \
    lose_frame(result);                                                                                                \
    return result;                                                                                                     \
  }                                                                                                                    \
  static VkResult lose_acquire2_##k(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {         \
    if (info && lose_acquire(info->swapchain)) return VK_ERROR_SURFACE_LOST_KHR;                                       \
    const VkResult result = g_lose_real[k].acquire2(device, info, index);                                              \
    lose_frame(result);                                                                                                \
    return result;                                                                                                     \
  }                                                                                                                    \
  static VkResult lose_present_##k(VkQueue queue, const VkPresentInfoKHR* info) {                                      \
    int refused = 0;                                                                                                   \
    for (uint32_t i = 0; info && i < info->swapchainCount; i++) {                                                      \
      lock(&g_lose_lock);                                                                                              \
      const VkSurfaceKHR surface = lose_chain_surface(info->pSwapchains[i]);                                           \
      unlock(&g_lose_lock);                                                                                            \
      const int one = lose_refuse(surface, "vkQueuePresentKHR");                                                       \
      if (info->pResults) info->pResults[i] = one ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;                            \
      refused |= one;                                                                                                  \
    }                                                                                                                  \
    return refused ? VK_ERROR_SURFACE_LOST_KHR : g_lose_real[k].present(queue, info);                                  \
  }                                                                                                                    \
  static VkResult lose_create_swapchain_##k(VkDevice device, const VkSwapchainCreateInfoKHR* info,                     \
                                            const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {      \
    if (info && lose_refuse(info->surface, "vkCreateSwapchainKHR")) return VK_ERROR_SURFACE_LOST_KHR;                  \
    const VkResult result = g_lose_real[k].create_swapchain(device, info, allocator, swapchain);                       \
    if (result == VK_SUCCESS && info && swapchain) {                                                                   \
      lock(&g_lose_lock);                                                                                              \
      for (int i = 0; i < LOSE_SWAPCHAINS; i++) {                                                                      \
        if (g_lose_chains[i].swapchain != VK_NULL_HANDLE) continue;                                                    \
        g_lose_chains[i].swapchain = *swapchain;                                                                       \
        g_lose_chains[i].surface = info->surface;                                                                      \
        break;                                                                                                         \
      }                                                                                                                \
      unlock(&g_lose_lock);                                                                                            \
    }                                                                                                                  \
    return result;                                                                                                     \
  }                                                                                                                    \
  static void lose_destroy_swapchain_##k(VkDevice device, VkSwapchainKHR swapchain,                                    \
                                         const VkAllocationCallbacks* allocator) {                                    \
    lock(&g_lose_lock);                                                                                                \
    for (int i = 0; i < LOSE_SWAPCHAINS; i++) {                                                                        \
      if (swapchain == VK_NULL_HANDLE || g_lose_chains[i].swapchain != swapchain) continue;                            \
      g_lose_chains[i].swapchain = VK_NULL_HANDLE;                                                                     \
      g_lose_chains[i].surface = VK_NULL_HANDLE;                                                                       \
    }                                                                                                                  \
    unlock(&g_lose_lock);                                                                                              \
    g_lose_real[k].destroy_swapchain(device, swapchain, allocator);                                                    \
  }                                                                                                                    \
  static VkResult lose_capabilities_##k(VkPhysicalDevice device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* out) {\
    if (g_lose_all_queries && lose_refuse(surface, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))                       \
      return VK_ERROR_SURFACE_LOST_KHR;                                                                                \
    return g_lose_real[k].capabilities(device, surface, out);                                                          \
  }                                                                                                                    \
  static VkResult lose_capabilities2_##k(VkPhysicalDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* info,         \
                                         VkSurfaceCapabilities2KHR* out) {                                            \
    if (g_lose_all_queries && info && lose_refuse(info->surface, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))        \
      return VK_ERROR_SURFACE_LOST_KHR;                                                                                \
    return g_lose_real[k].capabilities2(device, info, out);                                                            \
  }                                                                                                                    \
  static VkResult lose_support_##k(VkPhysicalDevice device, uint32_t family, VkSurfaceKHR surface, VkBool32* out) {    \
    if (lose_refuse(surface, "vkGetPhysicalDeviceSurfaceSupportKHR")) return VK_ERROR_SURFACE_LOST_KHR;                \
    return g_lose_real[k].support(device, family, surface, out);                                                       \
  }                                                                                                                    \
  static VkResult lose_formats_##k(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t* count,                     \
                                   VkSurfaceFormatKHR* out) {                                                         \
    if (g_lose_all_queries && lose_refuse(surface, "vkGetPhysicalDeviceSurfaceFormatsKHR"))                            \
      return VK_ERROR_SURFACE_LOST_KHR;                                                                                \
    return g_lose_real[k].formats(device, surface, count, out);                                                        \
  }                                                                                                                    \
  static VkResult lose_formats2_##k(VkPhysicalDevice device, const VkPhysicalDeviceSurfaceInfo2KHR* info,              \
                                    uint32_t* count, VkSurfaceFormat2KHR* out) {                                      \
    if (g_lose_all_queries && info && lose_refuse(info->surface, "vkGetPhysicalDeviceSurfaceFormats2KHR"))             \
      return VK_ERROR_SURFACE_LOST_KHR;                                                                                \
    return g_lose_real[k].formats2(device, info, count, out);                                                          \
  }                                                                                                                    \
  static VkResult lose_present_modes_##k(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t* count,               \
                                         VkPresentModeKHR* out) {                                                     \
    if (g_lose_all_queries && lose_refuse(surface, "vkGetPhysicalDeviceSurfacePresentModesKHR"))                       \
      return VK_ERROR_SURFACE_LOST_KHR;                                                                                \
    return g_lose_real[k].present_modes(device, surface, count, out);                                                  \
  }

LOSE_WRAPPERS(0)
LOSE_WRAPPERS(1)

#define LOSE_ENTRY(command, member) \
  {command, offsetof(struct lose_real, member), {(void*)lose_##member##_0, (void*)lose_##member##_1}}

static PFN_vkVoidFunction lose_answer(const char* name, void* real, int k) {
  static const struct {
    const char* name;
    size_t slot;
    void* wrapper[2];
  } table[] = {
      LOSE_ENTRY("vkAcquireNextImageKHR", acquire),
      LOSE_ENTRY("vkAcquireNextImage2KHR", acquire2),
      LOSE_ENTRY("vkQueuePresentKHR", present),
      LOSE_ENTRY("vkCreateSwapchainKHR", create_swapchain),
      LOSE_ENTRY("vkDestroySwapchainKHR", destroy_swapchain),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", capabilities),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfaceCapabilities2KHR", capabilities2),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfaceSupportKHR", support),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfaceFormatsKHR", formats),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfaceFormats2KHR", formats2),
      LOSE_ENTRY("vkGetPhysicalDeviceSurfacePresentModesKHR", present_modes),
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(table[i].name, name) != 0) continue;
    __atomic_store_n((void**)((char*)&g_lose_real[k] + table[i].slot), real, __ATOMIC_RELEASE);
    return (PFN_vkVoidFunction)table[i].wrapper[k];
  }
  return NULL;
}
