#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LOSED_QUEUES 32

struct losed_real {
  void (*get_queue)(VkDevice, uint32_t, uint32_t, VkQueue*);
  void (*get_queue2)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
  VkResult (*acquire)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
  VkResult (*acquire2)(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*);
  VkResult (*present)(VkQueue, const VkPresentInfoKHR*);
  VkResult (*submit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
  VkResult (*submit2)(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
  VkResult (*submit2khr)(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
  VkResult (*wait_fences)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
  VkResult (*fence_status)(VkDevice, VkFence);
  VkResult (*device_idle)(VkDevice);
  VkResult (*queue_idle)(VkQueue);
  VkResult (*create_swapchain)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
  void (*destroy_device)(VkDevice, const VkAllocationCallbacks*);
};

static struct losed_real g_losed_real[2];
static volatile int g_losed_lock;
static struct {
  VkQueue queue;
  VkDevice device;
} g_losed_queues[LOSED_QUEUES];
static VkDevice g_losed_device;
static int g_losed_read, g_losed_times, g_losed_done, g_losed_idle_ok;
static double g_losed_after, g_losed_gap, g_losed_armed, g_losed_next, g_losed_lost;
static unsigned long long g_losed_presents, g_losed_refused, g_losed_frames;

static double losed_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void losed_config(void) {
  if (g_losed_read) return;
  g_losed_read = 1;
  const char* text = getenv("BLOX7_VK_LOSE_DEVICE");
  if (text == NULL || text[0] == '\0') return;
  char* end = NULL;
  g_losed_after = strtod(text, &end);
  g_losed_times = 1;
  g_losed_gap = 8;
  if (end && *end == ',') g_losed_times = (int)strtol(end + 1, &end, 10);
  if (end && *end == ',') g_losed_gap = strtod(end + 1, &end);
  if (end && *end == ',') g_losed_idle_ok = end[1] == 'i';
  fprintf(stderr, "[lose device] after %.1f s, %d times, %.1f s apart, %s\n", g_losed_after, g_losed_times, g_losed_gap,
          g_losed_idle_ok ? "waits for idle still answer success" : "every call fails");
}

static VkDevice losed_queue_device(VkQueue queue) {
  for (int i = 0; i < LOSED_QUEUES; i++)
    if (g_losed_queues[i].queue == queue) return g_losed_queues[i].device;
  return VK_NULL_HANDLE;
}

static void losed_note_queue(VkDevice device, VkQueue queue) {
  if (queue == VK_NULL_HANDLE) return;
  lock(&g_losed_lock);
  int free_slot = -1;
  for (int i = 0; i < LOSED_QUEUES; i++) {
    if (g_losed_queues[i].queue == queue) {
      g_losed_queues[i].device = device;
      free_slot = -2;
      break;
    }
    if (free_slot < 0 && g_losed_queues[i].queue == VK_NULL_HANDLE) free_slot = i;
  }
  if (free_slot >= 0) {
    g_losed_queues[free_slot].queue = queue;
    g_losed_queues[free_slot].device = device;
  }
  unlock(&g_losed_lock);
}

static int losed_refuse(VkDevice device, const char* call) {
  if (device == VK_NULL_HANDLE) return 0;
  lock(&g_losed_lock);
  const int refused = device == g_losed_device;
  const unsigned long long count = refused ? ++g_losed_refused : 0;
  unlock(&g_losed_lock);
  if (count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000 || count == 100000)
    fprintf(stderr, "[lose device] %llu calls refused on the lost device, last %s\n", count, call);
  return refused;
}

static int losed_refuse_queue(VkQueue queue, const char* call) {
  lock(&g_losed_lock);
  const VkDevice device = losed_queue_device(queue);
  unlock(&g_losed_lock);
  return losed_refuse(device, call);
}

static int losed_present(VkQueue queue) {
  losed_config();
  lock(&g_losed_lock);
  const VkDevice device = losed_queue_device(queue);
  const unsigned long long presents = ++g_losed_presents;
  int lost = 0;
  double after = 0;
  if (device != VK_NULL_HANDLE && g_losed_device == VK_NULL_HANDLE && g_losed_done < g_losed_times) {
    const double now = losed_now();
    if (g_losed_next == 0) {
      g_losed_armed = now;
      g_losed_next = now + g_losed_after;
    }
    if (now >= g_losed_next) {
      g_losed_device = device;
      g_losed_done++;
      g_losed_refused = 0;
      lost = g_losed_done;
      after = now - g_losed_armed;
      g_losed_lost = now;
    }
  }
  unlock(&g_losed_lock);
  if (lost)
    fprintf(stderr, "[lose device] loss %d: present %llu reports the device lost, %.1f s in\n", lost, presents, after);
  return losed_refuse(device, "vkQueuePresentKHR");
}

static void losed_frame(VkResult result) {
  if (result < 0) return;
  lock(&g_losed_lock);
  const unsigned long long frames = g_losed_done && g_losed_device == VK_NULL_HANDLE ? ++g_losed_frames : 0;
  const int done = g_losed_done;
  const double lost = g_losed_lost;
  unlock(&g_losed_lock);
  if (frames == 1 || frames == 10 || frames == 100 || frames == 1000)
    fprintf(stderr, "[lose device] loss %d: frame %llu on the next device, %.0f ms after the loss\n", done, frames,
            (losed_now() - lost) * 1000);
}

static void losed_created(VkDevice device) {
  lock(&g_losed_lock);
  const int done = g_losed_done;
  const int still_lost = g_losed_device != VK_NULL_HANDLE;
  const double since = losed_now() - g_losed_lost;
  unlock(&g_losed_lock);
  if (done)
    fprintf(stderr, "[lose device] loss %d: device %p created %.0f ms after the loss%s\n", done, (void*)device,
            since * 1000, still_lost ? ", the lost one is still alive" : "");
}

static void losed_destroyed(VkDevice device) {
  lock(&g_losed_lock);
  const int lost = device != VK_NULL_HANDLE && device == g_losed_device;
  const unsigned long long refused = g_losed_refused;
  const int done = g_losed_done;
  const double since = losed_now() - g_losed_lost;
  if (lost) {
    g_losed_device = VK_NULL_HANDLE;
    g_losed_frames = 0;
    g_losed_armed = losed_now();
    g_losed_next = g_losed_armed + g_losed_gap;
  }
  for (int i = 0; i < LOSED_QUEUES; i++) {
    if (device == VK_NULL_HANDLE || g_losed_queues[i].device != device) continue;
    g_losed_queues[i].queue = VK_NULL_HANDLE;
    g_losed_queues[i].device = VK_NULL_HANDLE;
  }
  unlock(&g_losed_lock);
  if (lost)
    fprintf(stderr, "[lose device] loss %d: the lost device was destroyed %.0f ms after the loss, %llu refused calls\n",
            done, since * 1000, refused);
}

#define LOSED_WRAPPERS(k)                                                                                              \
  static void losed_get_queue_##k(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {                  \
    g_losed_real[k].get_queue(device, family, index, queue);                                                           \
    if (queue) losed_note_queue(device, *queue);                                                                       \
  }                                                                                                                    \
  static void losed_get_queue2_##k(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {                  \
    g_losed_real[k].get_queue2(device, info, queue);                                                                   \
    if (queue) losed_note_queue(device, *queue);                                                                       \
  }                                                                                                                    \
  static VkResult losed_acquire_##k(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,                       \
                                    VkSemaphore semaphore, VkFence fence, uint32_t* index) {                          \
    if (losed_refuse(device, "vkAcquireNextImageKHR")) return VK_ERROR_DEVICE_LOST;                                    \
    return g_losed_real[k].acquire(device, swapchain, timeout, semaphore, fence, index);                               \
  }                                                                                                                    \
  static VkResult losed_acquire2_##k(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index) {        \
    if (losed_refuse(device, "vkAcquireNextImage2KHR")) return VK_ERROR_DEVICE_LOST;                                   \
    return g_losed_real[k].acquire2(device, info, index);                                                              \
  }                                                                                                                    \
  static VkResult losed_present_##k(VkQueue queue, const VkPresentInfoKHR* info) {                                     \
    if (losed_present(queue)) {                                                                                        \
      for (uint32_t i = 0; info && info->pResults && i < info->swapchainCount; i++)                                    \
        info->pResults[i] = VK_ERROR_DEVICE_LOST;                                                                      \
      return VK_ERROR_DEVICE_LOST;                                                                                     \
    }                                                                                                                  \
    const VkResult result = g_losed_real[k].present(queue, info);                                                      \
    losed_frame(result);                                                                                               \
    return result;                                                                                                     \
  }                                                                                                                    \
  static VkResult losed_submit_##k(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {        \
    if (losed_refuse_queue(queue, "vkQueueSubmit")) return VK_ERROR_DEVICE_LOST;                                       \
    return g_losed_real[k].submit(queue, count, submits, fence);                                                       \
  }                                                                                                                    \
  static VkResult losed_submit2_##k(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {      \
    if (losed_refuse_queue(queue, "vkQueueSubmit2")) return VK_ERROR_DEVICE_LOST;                                      \
    return g_losed_real[k].submit2(queue, count, submits, fence);                                                      \
  }                                                                                                                    \
  static VkResult losed_submit2khr_##k(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence) {   \
    if (losed_refuse_queue(queue, "vkQueueSubmit2KHR")) return VK_ERROR_DEVICE_LOST;                                   \
    return g_losed_real[k].submit2khr(queue, count, submits, fence);                                                   \
  }                                                                                                                    \
  static VkResult losed_wait_fences_##k(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all,          \
                                        uint64_t timeout) {                                                           \
    if (losed_refuse(device, "vkWaitForFences")) return VK_ERROR_DEVICE_LOST;                                          \
    return g_losed_real[k].wait_fences(device, count, fences, all, timeout);                                           \
  }                                                                                                                    \
  static VkResult losed_fence_status_##k(VkDevice device, VkFence fence) {                                             \
    if (losed_refuse(device, "vkGetFenceStatus")) return VK_ERROR_DEVICE_LOST;                                         \
    return g_losed_real[k].fence_status(device, fence);                                                                \
  }                                                                                                                    \
  static VkResult losed_device_idle_##k(VkDevice device) {                                                             \
    const VkResult result = g_losed_real[k].device_idle(device);                                                       \
    return !g_losed_idle_ok && losed_refuse(device, "vkDeviceWaitIdle") ? VK_ERROR_DEVICE_LOST : result;               \
  }                                                                                                                    \
  static VkResult losed_queue_idle_##k(VkQueue queue) {                                                                \
    const VkResult result = g_losed_real[k].queue_idle(queue);                                                         \
    return !g_losed_idle_ok && losed_refuse_queue(queue, "vkQueueWaitIdle") ? VK_ERROR_DEVICE_LOST : result;           \
  }                                                                                                                    \
  static VkResult losed_create_swapchain_##k(VkDevice device, const VkSwapchainCreateInfoKHR* info,                    \
                                             const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {     \
    if (losed_refuse(device, "vkCreateSwapchainKHR")) return VK_ERROR_DEVICE_LOST;                                     \
    return g_losed_real[k].create_swapchain(device, info, allocator, swapchain);                                       \
  }                                                                                                                    \
  static void losed_destroy_device_##k(VkDevice device, const VkAllocationCallbacks* allocator) {                      \
    losed_destroyed(device);                                                                                           \
    g_losed_real[k].destroy_device(device, allocator);                                                                 \
  }

LOSED_WRAPPERS(0)
LOSED_WRAPPERS(1)

#define LOSED_ENTRY(command, member) \
  {command, offsetof(struct losed_real, member), {(void*)losed_##member##_0, (void*)losed_##member##_1}}

static PFN_vkVoidFunction losed_answer(const char* name, void* real, int k) {
  static const struct {
    const char* name;
    size_t slot;
    void* wrapper[2];
  } table[] = {
      LOSED_ENTRY("vkGetDeviceQueue", get_queue),
      LOSED_ENTRY("vkGetDeviceQueue2", get_queue2),
      LOSED_ENTRY("vkAcquireNextImageKHR", acquire),
      LOSED_ENTRY("vkAcquireNextImage2KHR", acquire2),
      LOSED_ENTRY("vkQueuePresentKHR", present),
      LOSED_ENTRY("vkQueueSubmit", submit),
      LOSED_ENTRY("vkQueueSubmit2", submit2),
      LOSED_ENTRY("vkQueueSubmit2KHR", submit2khr),
      LOSED_ENTRY("vkWaitForFences", wait_fences),
      LOSED_ENTRY("vkGetFenceStatus", fence_status),
      LOSED_ENTRY("vkDeviceWaitIdle", device_idle),
      LOSED_ENTRY("vkQueueWaitIdle", queue_idle),
      LOSED_ENTRY("vkCreateSwapchainKHR", create_swapchain),
      LOSED_ENTRY("vkDestroyDevice", destroy_device),
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(table[i].name, name) != 0) continue;
    __atomic_store_n((void**)((char*)&g_losed_real[k] + table[i].slot), real, __ATOMIC_RELEASE);
    return (PFN_vkVoidFunction)table[i].wrapper[k];
  }
  return NULL;
}
