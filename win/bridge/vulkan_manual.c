#include <stdbool.h>

#include "vulkan_bridge.h"

extern PFN_vkVoidFunction (BRIDGE_MS* vkb_ms_vkGetInstanceProcAddr)(VkInstance, const char*);
extern PFN_vkVoidFunction (BRIDGE_MS* vkb_ms_vkGetDeviceProcAddr)(VkDevice, const char*);
extern VkResult (BRIDGE_MS* vkb_ms_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*,
                                                     VkInstance*);
extern VkResult (BRIDGE_MS* vkb_ms_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                                   const VkAllocationCallbacks*, VkDevice*);
extern VkResult (BRIDGE_MS* vkb_ms_vkCreateDebugUtilsMessengerEXT)(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*,
                                                                   const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT*);
extern VkResult (BRIDGE_MS* vkb_ms_vkCreateDebugReportCallbackEXT)(VkInstance, const VkDebugReportCallbackCreateInfoEXT*,
                                                                   const VkAllocationCallbacks*, VkDebugReportCallbackEXT*);

static void lock(volatile int* l) {
  while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
  }
}
static void unlock(volatile int* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

struct allocator {
  VkAllocationCallbacks ms;
  VkAllocationCallbacks app;
};

static BRIDGE_MS void* allocation(void* user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
  bridge_attach();
  const struct allocator* a = (const struct allocator*)user;
  return a->app.pfnAllocation(a->app.pUserData, size, alignment, scope);
}

static BRIDGE_MS void* reallocation(void* user, void* original, size_t size, size_t alignment,
                                    VkSystemAllocationScope scope) {
  bridge_attach();
  const struct allocator* a = (const struct allocator*)user;
  return a->app.pfnReallocation(a->app.pUserData, original, size, alignment, scope);
}

static BRIDGE_MS void release(void* user, void* memory) {
  bridge_attach();
  const struct allocator* a = (const struct allocator*)user;
  a->app.pfnFree(a->app.pUserData, memory);
}

static BRIDGE_MS void internal_allocation(void* user, size_t size, VkInternalAllocationType type,
                                          VkSystemAllocationScope scope) {
  bridge_attach();
  const struct allocator* a = (const struct allocator*)user;
  a->app.pfnInternalAllocation(a->app.pUserData, size, type, scope);
}

static BRIDGE_MS void internal_free(void* user, size_t size, VkInternalAllocationType type,
                                    VkSystemAllocationScope scope) {
  bridge_attach();
  const struct allocator* a = (const struct allocator*)user;
  a->app.pfnInternalFree(a->app.pUserData, size, type, scope);
}

enum { kAllocators = 256 };
static struct allocator g_allocators[kAllocators];
static int g_allocator_count;
static volatile int g_allocator_lock;

const VkAllocationCallbacks* vkb_allocator(const VkAllocationCallbacks* app) {
  if (app == NULL) return NULL;
  const int known = __atomic_load_n(&g_allocator_count, __ATOMIC_ACQUIRE);
  for (int i = 0; i < known; i++) {
    if (memcmp(&g_allocators[i].app, app, sizeof *app) == 0) return &g_allocators[i].ms;
  }
  lock(&g_allocator_lock);
  int i = 0;
  for (; i < g_allocator_count; i++) {
    if (memcmp(&g_allocators[i].app, app, sizeof *app) == 0) break;
  }
  if (i == g_allocator_count) {
    if (i == kAllocators) {
      fprintf(stderr, "[vulkan bridge] more than %d distinct allocation callbacks\n", kAllocators);
      abort();
    }
    struct allocator* a = &g_allocators[i];
    a->app = *app;
    a->ms.pUserData = a;
    a->ms.pfnAllocation = app->pfnAllocation ? (PFN_vkAllocationFunction)allocation : NULL;
    a->ms.pfnReallocation = app->pfnReallocation ? (PFN_vkReallocationFunction)reallocation : NULL;
    a->ms.pfnFree = app->pfnFree ? (PFN_vkFreeFunction)release : NULL;
    a->ms.pfnInternalAllocation =
        app->pfnInternalAllocation ? (PFN_vkInternalAllocationNotification)internal_allocation : NULL;
    a->ms.pfnInternalFree = app->pfnInternalFree ? (PFN_vkInternalFreeNotification)internal_free : NULL;
    __atomic_store_n(&g_allocator_count, i + 1, __ATOMIC_RELEASE);
  }
  unlock(&g_allocator_lock);
  return &g_allocators[i].ms;
}

static BRIDGE_MS VkBool32 debug_utils(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* context) {
  bridge_attach();
  const struct bridge_context* c = (const struct bridge_context*)context;
  return ((PFN_vkDebugUtilsMessengerCallbackEXT)c->fn)(severity, types, data, c->userdata);
}

static BRIDGE_MS VkBool32 debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type, uint64_t object,
                                       size_t location, int32_t code, const char* layer, const char* message,
                                       void* context) {
  bridge_attach();
  const struct bridge_context* c = (const struct bridge_context*)context;
  return ((PFN_vkDebugReportCallbackEXT)c->fn)(flags, type, object, location, code, layer, message, c->userdata);
}

static BRIDGE_MS void memory_report(const VkDeviceMemoryReportCallbackDataEXT* data, void* context) {
  bridge_attach();
  const struct bridge_context* c = (const struct bridge_context*)context;
  ((PFN_vkDeviceMemoryReportCallbackEXT)c->fn)(data, c->userdata);
}

static bool translate_callbacks(VkBaseOutStructure* s) {
  switch (s->sType) {
    case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT: {
      VkDebugUtilsMessengerCreateInfoEXT* info = (VkDebugUtilsMessengerCreateInfoEXT*)s;
      if (info->pfnUserCallback) {
        info->pUserData = bridge_context((void*)info->pfnUserCallback, info->pUserData);
        info->pfnUserCallback = (PFN_vkDebugUtilsMessengerCallbackEXT)debug_utils;
      }
      return true;
    }
    case VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT: {
      VkDebugReportCallbackCreateInfoEXT* info = (VkDebugReportCallbackCreateInfoEXT*)s;
      if (info->pfnCallback) {
        info->pUserData = bridge_context((void*)info->pfnCallback, info->pUserData);
        info->pfnCallback = (PFN_vkDebugReportCallbackEXT)debug_report;
      }
      return true;
    }
    case VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT: {
      VkDeviceDeviceMemoryReportCreateInfoEXT* info = (VkDeviceDeviceMemoryReportCreateInfoEXT*)s;
      if (info->pfnUserCallback) {
        info->pUserData = bridge_context((void*)info->pfnUserCallback, info->pUserData);
        info->pfnUserCallback = (PFN_vkDeviceMemoryReportCallbackEXT)memory_report;
      }
      return true;
    }
    case VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG:
      fprintf(stderr, "[vulkan bridge] VkDirectDriverLoadingListLUNARG is not supported\n");
      return false;
    default:
      return true;
  }
}

static bool carries_callbacks(VkStructureType type) {
  return type == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
         type == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT ||
         type == VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT ||
         type == VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG;
}

struct chain {
  void* blocks[32];
  int count;
};

static void free_chain(struct chain* c) {
  for (int i = 0; i < c->count; i++) free(c->blocks[i]);
  c->count = 0;
}

static bool translate_chain(const void* next, const void** out, struct chain* c) {
  *out = next;
  bool needed = false;
  for (const VkBaseInStructure* s = (const VkBaseInStructure*)next; s; s = s->pNext) {
    if (carries_callbacks(s->sType)) needed = true;
  }
  if (!needed) return true;
  VkBaseOutStructure* last = NULL;
  const void* head = NULL;
  for (const VkBaseInStructure* s = (const VkBaseInStructure*)next; s; s = s->pNext) {
    const size_t size = vkb_struct_size(s->sType);
    if (size == 0 || c->count == (int)(sizeof c->blocks / sizeof c->blocks[0])) {
      fprintf(stderr, "[vulkan bridge] cannot copy a pNext chain with structure type %d\n", (int)s->sType);
      free_chain(c);
      return false;
    }
    VkBaseOutStructure* copy = (VkBaseOutStructure*)malloc(size);
    if (copy == NULL) {
      free_chain(c);
      return false;
    }
    memcpy(copy, s, size);
    copy->pNext = NULL;
    c->blocks[c->count++] = copy;
    if (!translate_callbacks(copy)) {
      free_chain(c);
      return false;
    }
    if (last) last->pNext = copy;
    else head = copy;
    last = copy;
  }
  *out = head;
  return true;
}

VkResult vkb_m_vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                VkInstance* instance) {
  if (info == NULL) return vkb_ms_vkCreateInstance(info, vkb_allocator(allocator), instance);
  VkInstanceCreateInfo copy = *info;
  struct chain c = {{0}, 0};
  if (!translate_chain(info->pNext, &copy.pNext, &c)) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = vkb_ms_vkCreateInstance(&copy, vkb_allocator(allocator), instance);
  free_chain(&c);
  return result;
}

VkResult vkb_m_vkCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* info,
                              const VkAllocationCallbacks* allocator, VkDevice* device) {
  if (info == NULL) return vkb_ms_vkCreateDevice(physical_device, info, vkb_allocator(allocator), device);
  VkDeviceCreateInfo copy = *info;
  struct chain c = {{0}, 0};
  if (!translate_chain(info->pNext, &copy.pNext, &c)) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = vkb_ms_vkCreateDevice(physical_device, &copy, vkb_allocator(allocator), device);
  free_chain(&c);
  return result;
}

VkResult vkb_m_vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* info,
                                              const VkAllocationCallbacks* allocator, VkDebugUtilsMessengerEXT* messenger) {
  if (info == NULL) return vkb_ms_vkCreateDebugUtilsMessengerEXT(instance, info, vkb_allocator(allocator), messenger);
  VkDebugUtilsMessengerCreateInfoEXT copy = *info;
  struct chain c = {{0}, 0};
  if (!translate_callbacks((VkBaseOutStructure*)&copy) || !translate_chain(info->pNext, &copy.pNext, &c))
    return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = vkb_ms_vkCreateDebugUtilsMessengerEXT(instance, &copy, vkb_allocator(allocator), messenger);
  free_chain(&c);
  return result;
}

VkResult vkb_m_vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT* info,
                                              const VkAllocationCallbacks* allocator, VkDebugReportCallbackEXT* callback) {
  if (info == NULL) return vkb_ms_vkCreateDebugReportCallbackEXT(instance, info, vkb_allocator(allocator), callback);
  VkDebugReportCallbackCreateInfoEXT copy = *info;
  struct chain c = {{0}, 0};
  if (!translate_callbacks((VkBaseOutStructure*)&copy) || !translate_chain(info->pNext, &copy.pNext, &c))
    return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = vkb_ms_vkCreateDebugReportCallbackEXT(instance, &copy, vkb_allocator(allocator), callback);
  free_chain(&c);
  return result;
}

static VkInstance g_instance;
static unsigned char g_dispatching[2][1024];

static int by_name(const void* key, const void* entry) {
  return strcmp((const char*)key, ((const struct vkb_command*)entry)->name);
}

// two drivers answering differently? then the loader's dispatching entry point it is
static void vkb_set(const struct vkb_command* c, void* fn, int device_lookup) {
  void** slot = device_lookup && c->device_slot ? c->device_slot : c->slot;
  const int which = slot == c->slot ? 0 : 1;
  const size_t index = (size_t)(c - vkb_commands);
  if (slot == (void**)&vkb_ms_vkGetInstanceProcAddr || __atomic_load_n(&g_dispatching[which][index], __ATOMIC_ACQUIRE))
    return;
  void* current = NULL;
  if (__atomic_compare_exchange_n(slot, &current, fn, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || current == fn)
    return;
  VkInstance instance = __atomic_load_n(&g_instance, __ATOMIC_ACQUIRE);
  void* loader = instance ? (void*)vkb_ms_vkGetInstanceProcAddr(instance, c->name) : NULL;
  if (loader == NULL) return;
  __atomic_store_n(slot, loader, __ATOMIC_RELEASE);
  __atomic_store_n(&g_dispatching[which][index], 1, __ATOMIC_RELEASE);
  fprintf(stderr, "[vulkan bridge] %s: two drivers answered; using the loader's entry point\n", c->name);
}

static PFN_vkVoidFunction answer(const char* name, void* fn, int device_lookup) {
  const struct vkb_command* c = (const struct vkb_command*)bsearch(name, vkb_commands, vkb_command_count,
                                                                    sizeof vkb_commands[0], by_name);
  if (c == NULL) {
    static volatile int noted_lock;
    static char noted[64][64];
    static int noted_count;
    lock(&noted_lock);
    int i = 0;
    while (i < noted_count && strcmp(noted[i], name) != 0) i++;
    if (i == noted_count && noted_count < 64) {
      snprintf(noted[noted_count++], sizeof noted[0], "%s", name);
      fprintf(stderr, "[vulkan bridge] no System V function for %s: answered as unavailable\n", name);
    }
    unlock(&noted_lock);
    return NULL;
  }
  vkb_set(c, fn, device_lookup);
  return (PFN_vkVoidFunction)(device_lookup && c->device_wrapper ? c->device_wrapper : c->wrapper);
}

PFN_vkVoidFunction vkb_m_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (name == NULL || vkb_ms_vkGetInstanceProcAddr == NULL) return NULL;
  void* fn = (void*)vkb_ms_vkGetInstanceProcAddr(instance, name);
  if (fn == NULL) return NULL;
  if (instance) __atomic_store_n(&g_instance, instance, __ATOMIC_RELEASE);
  return answer(name, fn, 0);
}

PFN_vkVoidFunction vkb_m_vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (name == NULL || vkb_ms_vkGetDeviceProcAddr == NULL) return NULL;
  void* fn = (void*)vkb_ms_vkGetDeviceProcAddr(device, name);
  return fn ? answer(name, fn, 1) : NULL;
}

void* vkb_install(void* loader_get_instance_proc_addr) {
  if (vkb_command_count > sizeof g_dispatching[0]) {
    fprintf(stderr, "[vulkan bridge] %zu commands: g_dispatching is too small\n", vkb_command_count);
    abort();
  }
  __atomic_store_n((void**)&vkb_ms_vkGetInstanceProcAddr, loader_get_instance_proc_addr, __ATOMIC_RELEASE);
  return (void*)vkb_m_vkGetInstanceProcAddr;
}
