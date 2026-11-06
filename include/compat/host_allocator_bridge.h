#ifndef BLOX7_COMPAT_HOST_ALLOCATOR_BRIDGE_H_
#define BLOX7_COMPAT_HOST_ALLOCATOR_BRIDGE_H_

#include <cstddef>

namespace blox7::compat {

void* HostAllocate(size_t size) noexcept;
void* HostAlignedAllocate(size_t size, size_t alignment) noexcept;
void* HostReallocate(void* pointer, size_t size) noexcept;
void HostFree(void* pointer) noexcept;
size_t HostUsableSize(void* pointer) noexcept;
void* HostAllocatorObjectAllocate(void* object, size_t size,
                                  size_t alignment) noexcept;

}  // namespace blox7::compat

#endif  // BLOX7_COMPAT_HOST_ALLOCATOR_BRIDGE_H_
