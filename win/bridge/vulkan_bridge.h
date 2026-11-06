#pragma once

#include <vulkan/vulkan_core.h>

#include "bridge_common.h"

#ifdef __cplusplus
extern "C" {
#endif

enum vkb_kind { VKB_GLOBAL, VKB_INSTANCE, VKB_DEVICE };

struct vkb_command {
  const char* name;
  void** slot;
  void* wrapper;
  void** device_slot;
  void* device_wrapper;
  enum vkb_kind kind;
};

extern const struct vkb_command vkb_commands[];
extern const size_t vkb_command_count;

size_t vkb_struct_size(VkStructureType type);

const VkAllocationCallbacks* vkb_allocator(const VkAllocationCallbacks* callbacks);

void* vkb_install(void* loader_get_instance_proc_addr);

#ifdef __cplusplus
}
#endif
