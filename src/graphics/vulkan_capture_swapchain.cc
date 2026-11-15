#include "blox7/graphics/vulkan_capture_swapchain.h"

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blox7/media/encoder_surface.h"

namespace blox7 {
namespace graphics {
namespace {

constexpr VkStructureType kAndroidSurfaceCreateInfo =
    static_cast<VkStructureType>(1000008000);
constexpr std::uint32_t kMinImages = 2;
constexpr std::uint32_t kMaxImages = 8;
constexpr std::uint64_t kRefreshNs = 16666667;
constexpr std::uint64_t kFenceSliceNs = 100000000;
constexpr std::uint64_t kForeverNs = 1ull << 60;
constexpr VkSurfaceFormatKHR kSurfaceFormats[] = {
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
constexpr std::uint32_t kSurfaceFormatCount = 2;

struct AndroidSurfaceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* window;
};

#define CAPTURE_DEVICE_FUNCTIONS(X)                                           \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)          \
  X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                   \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory)  \
  X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory)                             \
  X(vkInvalidateMappedMemoryRanges) X(vkCreateCommandPool)                    \
  X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
  X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer)     \
  X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)      \
  X(vkQueueSubmit)

struct Functions {
#define CAPTURE_DECLARE(name) PFN_##name name = nullptr;
  CAPTURE_DEVICE_FUNCTIONS(CAPTURE_DECLARE)
#undef CAPTURE_DECLARE
};

struct Device {
  VkDevice handle = VK_NULL_HANDLE;
  Functions f;
  VkPhysicalDeviceMemoryProperties memory{};
  VkQueue spare_queue = VK_NULL_HANDLE;
  std::mutex* spare_queue_mutex = nullptr;
};

struct Surface {
  const void* window = nullptr;
  const Blox7EncoderSurface* ops = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

enum class ImageState { kFree, kAcquired, kPresented };

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkFence fence = VK_NULL_HANDLE;
  ImageState state = ImageState::kFree;
  std::int64_t present_us = 0;
};

struct Recorded {
  VkCommandPool pool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commands;
};

struct Chain {
  std::shared_ptr<Device> device;
  Surface surface;
  VkExtent2D extent{};
  std::int32_t pixels = BLOX7_ENCODER_PIXELS_RGBA;
  bool coherent = true;
  std::vector<Image> images;
  std::unordered_map<std::uint32_t, Recorded> recorded;
  std::deque<std::uint32_t> free_images;
  std::deque<std::uint32_t> presented;
  bool retired = false;
  VkResult lost = VK_SUCCESS;
  std::atomic<bool> stop{false};
  std::mutex mutex;
  std::condition_variable changed;
  std::thread worker;
};

std::int64_t MonotonicUs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

bool FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                    std::uint32_t bits,
                    std::initializer_list<VkMemoryPropertyFlags> choices,
                    std::uint32_t* index) {
  for (const VkMemoryPropertyFlags wanted : choices) {
    for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
      if ((bits & (1u << type)) != 0 &&
          (memory.memoryTypes[type].propertyFlags & wanted) == wanted) {
        *index = type;
        return true;
      }
    }
  }
  return false;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout from,
                                  VkImageLayout to, VkAccessFlags source,
                                  VkAccessFlags target) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = source;
  barrier.dstAccessMask = target;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return barrier;
}

const void* FindStruct(const void* chain, VkStructureType type) {
  for (auto* current = static_cast<const VkBaseInStructure*>(chain);
       current != nullptr; current = current->pNext) {
    if (current->sType == type) {
      return current;
    }
  }
  return nullptr;
}

void DestroyResources(Chain& chain) {
  const Functions& f = chain.device->f;
  const VkDevice device = chain.device->handle;
  for (Image& image : chain.images) {
    if (image.state == ImageState::kPresented && image.fence != VK_NULL_HANDLE) {
      f.vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull);
      chain.surface.ops->submit(chain.surface.window, nullptr, 0, chain.pixels,
                                image.present_us);
    }
  }
  for (auto& [family, recorded] : chain.recorded) {
    f.vkDestroyCommandPool(device, recorded.pool, nullptr);
  }
  chain.recorded.clear();
  for (Image& image : chain.images) {
    if (image.fence != VK_NULL_HANDLE) {
      f.vkDestroyFence(device, image.fence, nullptr);
    }
    if (image.mapped != nullptr) {
      f.vkUnmapMemory(device, image.buffer_memory);
    }
    if (image.buffer != VK_NULL_HANDLE) {
      f.vkDestroyBuffer(device, image.buffer, nullptr);
    }
    if (image.buffer_memory != VK_NULL_HANDLE) {
      f.vkFreeMemory(device, image.buffer_memory, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
      f.vkDestroyImage(device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
      f.vkFreeMemory(device, image.memory, nullptr);
    }
  }
  chain.images.clear();
}

bool CreateImages(Chain& chain, const VkSwapchainCreateInfoKHR& info,
                  std::uint32_t count) {
  const Device& d = *chain.device;
  const Functions& f = d.f;
  chain.images.resize(count);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(chain.extent.width) *
                             chain.extent.height * 4;
  for (Image& image : chain.images) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if ((info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0) {
      image_info.flags =
          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
      image_info.pNext = FindStruct(
          info.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    }
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = info.imageFormat;
    image_info.extent = {chain.extent.width, chain.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = std::max(info.imageArrayLayers, 1u);
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = info.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = info.imageSharingMode;
    image_info.queueFamilyIndexCount =
        info.imageSharingMode == VK_SHARING_MODE_CONCURRENT
            ? info.queueFamilyIndexCount
            : 0;
    image_info.pQueueFamilyIndices =
        info.imageSharingMode == VK_SHARING_MODE_CONCURRENT
            ? info.pQueueFamilyIndices
            : nullptr;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.vkCreateImage(d.handle, &image_info, nullptr, &image.image) !=
        VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements{};
    f.vkGetImageMemoryRequirements(d.handle, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    if (!FindMemoryType(d.memory, requirements.memoryTypeBits,
                        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0},
                        &allocate.memoryTypeIndex) ||
        f.vkAllocateMemory(d.handle, &allocate, nullptr, &image.memory) !=
            VK_SUCCESS ||
        f.vkBindImageMemory(d.handle, image.image, image.memory, 0) !=
            VK_SUCCESS) {
      return false;
    }

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (f.vkCreateBuffer(d.handle, &buffer_info, nullptr, &image.buffer) !=
        VK_SUCCESS) {
      return false;
    }
    f.vkGetBufferMemoryRequirements(d.handle, image.buffer, &requirements);
    allocate.allocationSize = requirements.size;
    constexpr VkMemoryPropertyFlags kVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkMemoryPropertyFlags kCached =
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    constexpr VkMemoryPropertyFlags kCoherent =
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!FindMemoryType(d.memory, requirements.memoryTypeBits,
                        {kVisible | kCached | kCoherent, kVisible | kCached,
                         kVisible | kCoherent, kVisible},
                        &allocate.memoryTypeIndex) ||
        f.vkAllocateMemory(d.handle, &allocate, nullptr,
                           &image.buffer_memory) != VK_SUCCESS ||
        f.vkBindBufferMemory(d.handle, image.buffer, image.buffer_memory, 0) !=
            VK_SUCCESS ||
        f.vkMapMemory(d.handle, image.buffer_memory, 0, VK_WHOLE_SIZE, 0,
                      &image.mapped) != VK_SUCCESS) {
      image.mapped = nullptr;
      return false;
    }
    chain.coherent =
        (d.memory.memoryTypes[allocate.memoryTypeIndex].propertyFlags &
         kCoherent) != 0;

    const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (f.vkCreateFence(d.handle, &fence_info, nullptr, &image.fence) !=
        VK_SUCCESS) {
      return false;
    }
  }
  return true;
}

Recorded* RecordFor(Chain& chain, std::uint32_t family) {
  if (const auto found = chain.recorded.find(family);
      found != chain.recorded.end()) {
    return &found->second;
  }
  const Device& d = *chain.device;
  const Functions& f = d.f;
  Recorded recorded;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = family;
  if (f.vkCreateCommandPool(d.handle, &pool_info, nullptr, &recorded.pool) !=
      VK_SUCCESS) {
    return nullptr;
  }
  recorded.commands.resize(chain.images.size());
  VkCommandBufferAllocateInfo allocate{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = recorded.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount =
      static_cast<std::uint32_t>(recorded.commands.size());
  bool ok = f.vkAllocateCommandBuffers(d.handle, &allocate,
                                       recorded.commands.data()) == VK_SUCCESS;
  for (std::size_t index = 0; ok && index < chain.images.size(); ++index) {
    const Image& image = chain.images[index];
    const VkCommandBuffer cmd = recorded.commands[index];
    const VkCommandBufferBeginInfo begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    ok = f.vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
    if (!ok) {
      break;
    }
    const VkImageMemoryBarrier to_copy = ImageBarrier(
        image.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_copy);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {chain.extent.width, chain.extent.height, 1};
    f.vkCmdCopyImageToBuffer(cmd, image.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.buffer,
                             1, &region);
    const VkImageMemoryBarrier to_present = ImageBarrier(
        image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
    VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = image.buffer;
    to_host.size = VK_WHOLE_SIZE;
    f.vkCmdPipelineBarrier(
        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
        nullptr, 1, &to_host, 1, &to_present);
    ok = f.vkEndCommandBuffer(cmd) == VK_SUCCESS;
  }
  if (!ok) {
    f.vkDestroyCommandPool(d.handle, recorded.pool, nullptr);
    return nullptr;
  }
  return &chain.recorded.emplace(family, std::move(recorded)).first->second;
}

void RunWorker(std::shared_ptr<Chain> chain) {
  pthread_setname_np(pthread_self(), "CaptureCopy");
  const Functions& f = chain->device->f;
  const VkDevice device = chain->device->handle;
  std::unique_lock<std::mutex> lock(chain->mutex);
  while (true) {
    chain->changed.wait(lock, [&chain] {
      return chain->stop.load(std::memory_order_acquire) ||
             !chain->presented.empty();
    });
    if (chain->stop.load(std::memory_order_acquire)) {
      return;
    }
    const std::uint32_t index = chain->presented.front();
    Image& image = chain->images[index];
    lock.unlock();
    VkResult waited = VK_TIMEOUT;
    while (waited == VK_TIMEOUT &&
           !chain->stop.load(std::memory_order_acquire)) {
      waited = f.vkWaitForFences(device, 1, &image.fence, VK_TRUE,
                                 kFenceSliceNs);
    }
    if (waited == VK_SUCCESS) {
      if (!chain->coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = image.buffer_memory;
        range.size = VK_WHOLE_SIZE;
        f.vkInvalidateMappedMemoryRanges(device, 1, &range);
      }
      chain->surface.ops->submit(
          chain->surface.window, image.mapped,
          static_cast<std::int32_t>(chain->extent.width * 4), chain->pixels,
          image.present_us);
    } else if (waited != VK_TIMEOUT) {
      chain->surface.ops->submit(chain->surface.window, nullptr, 0,
                                 chain->pixels, image.present_us);
    }
    lock.lock();
    if (waited == VK_TIMEOUT) {
      return;
    }
    chain->presented.pop_front();
    if (waited != VK_SUCCESS) {
      chain->lost = waited;
    }
    image.state = ImageState::kFree;
    chain->free_images.push_back(index);
    chain->changed.notify_all();
  }
}

void Shutdown(Chain& chain) {
  {
    std::lock_guard<std::mutex> lock(chain.mutex);
    chain.stop.store(true, std::memory_order_release);
    chain.changed.notify_all();
  }
  if (chain.worker.joinable()) {
    chain.worker.join();
  }
  DestroyResources(chain);
}

struct FilteredPresentChain {
  VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
  std::vector<VkPresentTimeGOOGLE> entries;
  const void* head = nullptr;
};

void FilterPresentChain(const void* next,
                        const std::vector<std::uint32_t>& positions,
                        FilteredPresentChain* out) {
  static std::atomic<bool> warned{false};
  for (auto* current = static_cast<const VkBaseInStructure*>(next);
       current != nullptr; current = current->pNext) {
    if (current->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) {
      const auto* times =
          reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(current);
      for (const std::uint32_t position : positions) {
        if (times->pTimes != nullptr && position < times->swapchainCount) {
          out->entries.push_back(times->pTimes[position]);
        }
      }
      if (out->entries.size() == positions.size()) {
        out->times.swapchainCount =
            static_cast<std::uint32_t>(out->entries.size());
        out->times.pTimes = out->entries.data();
        out->head = &out->times;
      }
    } else if (!warned.exchange(true, std::memory_order_relaxed)) {
      std::fprintf(stderr,
                   "  [vulkan] present info %d dropped next to a capture "
                   "swapchain\n",
                   static_cast<int>(current->sType));
    }
  }
}

}  // namespace

struct VulkanCaptureSwapchains::Impl {
  std::mutex mutex;
  std::unordered_map<VkDevice, std::shared_ptr<Device>> devices;
  std::unordered_map<VkQueue, std::pair<VkDevice, std::uint32_t>> queues;
  std::unordered_map<VkSurfaceKHR, std::unique_ptr<Surface>> surfaces;
  std::unordered_map<VkSwapchainKHR, std::shared_ptr<Chain>> chains;
  std::unordered_map<VkSemaphore, VkDevice> pending_semaphores;
  std::vector<std::pair<VkDevice, VkFence>> pending_fences;
  std::atomic<std::size_t> surface_count{0};
  std::atomic<std::size_t> chain_count{0};
  std::atomic<std::size_t> pending_count{0};
  std::atomic<Blox7EncoderSurfaceLookup> lookup{nullptr};
  std::atomic<PFN_vkGetPhysicalDeviceQueueFamilyProperties> queue_families{
      nullptr};

  void CountPendingLocked() {
    pending_count.store(pending_semaphores.size() + pending_fences.size(),
                        std::memory_order_release);
  }

  bool FindSurface(VkSurfaceKHR handle, Surface* surface) {
    if (surface_count.load(std::memory_order_acquire) == 0) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = surfaces.find(handle);
    if (found == surfaces.end()) {
      return false;
    }
    if (surface != nullptr) {
      *surface = *found->second;
    }
    return true;
  }

  std::shared_ptr<Chain> FindChain(VkSwapchainKHR handle) {
    if (chain_count.load(std::memory_order_acquire) == 0) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = chains.find(handle);
    return found != chains.end() ? found->second : nullptr;
  }

  VkResult SignalPending(VkQueue queue, const std::vector<VkSemaphore>& waits) {
    std::vector<VkSemaphore> signals;
    std::vector<VkFence> fences;
    std::shared_ptr<Device> device;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto bound = queues.find(queue);
      if (bound == queues.end()) {
        return VK_SUCCESS;
      }
      const VkDevice handle = bound->second.first;
      const auto owner = devices.find(handle);
      if (owner == devices.end()) {
        return VK_SUCCESS;
      }
      device = owner->second;
      for (const VkSemaphore semaphore : waits) {
        const auto found = pending_semaphores.find(semaphore);
        if (found != pending_semaphores.end() && found->second == handle) {
          signals.push_back(semaphore);
          pending_semaphores.erase(found);
        }
      }
      for (auto it = pending_fences.begin(); it != pending_fences.end();) {
        if (it->first == handle) {
          fences.push_back(it->second);
          it = pending_fences.erase(it);
        } else {
          ++it;
        }
      }
      CountPendingLocked();
      if (signals.empty() && fences.empty()) {
        return VK_SUCCESS;
      }
    }
    if (!signals.empty()) {
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.signalSemaphoreCount = static_cast<std::uint32_t>(signals.size());
      submit.pSignalSemaphores = signals.data();
      const VkResult result =
          device->f.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
      if (result != VK_SUCCESS) {
        return result;
      }
    }
    for (const VkFence fence : fences) {
      const VkResult result = device->f.vkQueueSubmit(queue, 0, nullptr, fence);
      if (result != VK_SUCCESS) {
        return result;
      }
    }
    return VK_SUCCESS;
  }
};

VulkanCaptureSwapchains::VulkanCaptureSwapchains() : impl_(new Impl) {}

VulkanCaptureSwapchains::~VulkanCaptureSwapchains() {
  for (auto& [handle, chain] : impl_->chains) {
    chain->stop.store(true, std::memory_order_release);
    chain->changed.notify_all();
    if (chain->worker.joinable()) {
      chain->worker.detach();
    }
  }
}

void VulkanCaptureSwapchains::RegisterDevice(
    VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    PFN_vkGetDeviceProcAddr get_device_proc_addr, VkQueue spare_queue,
    std::mutex* spare_queue_mutex) {
  if (device == VK_NULL_HANDLE || get_instance_proc_addr == nullptr ||
      get_device_proc_addr == nullptr) {
    return;
  }
  auto d = std::make_shared<Device>();
  d->handle = device;
  d->spare_queue = spare_queue;
  d->spare_queue_mutex = spare_queue_mutex;
  bool loaded = true;
#define CAPTURE_LOAD(name)                                                  \
  d->f.name =                                                               \
      reinterpret_cast<PFN_##name>(get_device_proc_addr(device, #name));    \
  loaded = loaded && d->f.name != nullptr;
  CAPTURE_DEVICE_FUNCTIONS(CAPTURE_LOAD)
#undef CAPTURE_LOAD
  const auto memory_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          get_instance_proc_addr(instance,
                                 "vkGetPhysicalDeviceMemoryProperties"));
  if (!loaded || memory_properties == nullptr) {
    std::fprintf(stderr,
                 "  [vulkan] device functions missing, no capture swapchains\n");
    return;
  }
  memory_properties(physical_device, &d->memory);
  impl_->queue_families.store(
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          get_instance_proc_addr(instance,
                                 "vkGetPhysicalDeviceQueueFamilyProperties")),
      std::memory_order_release);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->devices[device] = std::move(d);
}

void VulkanCaptureSwapchains::DestroyDevice(VkDevice device) {
  std::vector<std::shared_ptr<Chain>> doomed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto it = impl_->chains.begin(); it != impl_->chains.end();) {
      if (it->second->device->handle == device) {
        doomed.push_back(std::move(it->second));
        it = impl_->chains.erase(it);
      } else {
        ++it;
      }
    }
    impl_->chain_count.store(impl_->chains.size(), std::memory_order_release);
    for (auto it = impl_->queues.begin(); it != impl_->queues.end();) {
      it = it->second.first == device ? impl_->queues.erase(it) : std::next(it);
    }
    for (auto it = impl_->pending_semaphores.begin();
         it != impl_->pending_semaphores.end();) {
      it = it->second == device ? impl_->pending_semaphores.erase(it)
                                : std::next(it);
    }
    impl_->pending_fences.erase(
        std::remove_if(impl_->pending_fences.begin(),
                       impl_->pending_fences.end(),
                       [device](const std::pair<VkDevice, VkFence>& entry) {
                         return entry.first == device;
                       }),
        impl_->pending_fences.end());
    impl_->CountPendingLocked();
    impl_->devices.erase(device);
  }
  for (const std::shared_ptr<Chain>& chain : doomed) {
    Shutdown(*chain);
  }
}

void VulkanCaptureSwapchains::RegisterQueue(VkDevice device, VkQueue queue,
                                            std::uint32_t family) {
  if (queue == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->queues[queue] = {device, family};
}

bool VulkanCaptureSwapchains::CreateSurface(const void* create_info,
                                            VkSurfaceKHR* surface,
                                            VkResult* result) {
  const auto* info = static_cast<const AndroidSurfaceCreateInfo*>(create_info);
  if (info == nullptr || surface == nullptr ||
      info->sType != kAndroidSurfaceCreateInfo || info->window == nullptr) {
    return false;
  }
  Blox7EncoderSurfaceLookup lookup =
      impl_->lookup.load(std::memory_order_acquire);
  if (lookup == nullptr) {
    lookup = reinterpret_cast<Blox7EncoderSurfaceLookup>(
        dlsym(RTLD_DEFAULT, BLOX7_ENCODER_SURFACE_SYMBOL));
    if (lookup == nullptr) {
      return false;
    }
    impl_->lookup.store(lookup, std::memory_order_release);
  }
  const Blox7EncoderSurface* ops = lookup(info->window);
  if (ops == nullptr) {
    return false;
  }
  auto created = std::make_unique<Surface>();
  created->window = info->window;
  created->ops = ops;
  std::int32_t width = 0;
  std::int32_t height = 0;
  ops->acquire(info->window);
  ops->size(info->window, &width, &height);
  created->width = static_cast<std::uint32_t>(std::max(width, 0));
  created->height = static_cast<std::uint32_t>(std::max(height, 0));
  const auto handle = reinterpret_cast<VkSurfaceKHR>(created.get());
  std::fprintf(stderr, "  [vulkan] encoder surface %ux%u\n", created->width,
               created->height);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->surfaces[handle] = std::move(created);
    impl_->surface_count.store(impl_->surfaces.size(),
                               std::memory_order_release);
  }
  *surface = handle;
  *result = VK_SUCCESS;
  return true;
}

bool VulkanCaptureSwapchains::DestroySurface(VkSurfaceKHR surface) {
  if (impl_->surface_count.load(std::memory_order_acquire) == 0) {
    return false;
  }
  std::unique_ptr<Surface> destroyed;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->surfaces.find(surface);
    if (found == impl_->surfaces.end()) {
      return false;
    }
    destroyed = std::move(found->second);
    impl_->surfaces.erase(found);
    impl_->surface_count.store(impl_->surfaces.size(),
                               std::memory_order_release);
  }
  destroyed->ops->release(destroyed->window);
  return true;
}

bool VulkanCaptureSwapchains::SurfaceSupport(VkPhysicalDevice physical_device,
                                             std::uint32_t family,
                                             VkSurfaceKHR surface,
                                             VkBool32* supported,
                                             VkResult* result) {
  if (!impl_->FindSurface(surface, nullptr)) {
    return false;
  }
  VkBool32 usable = VK_TRUE;
  const PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_families =
      impl_->queue_families.load(std::memory_order_acquire);
  if (queue_families != nullptr) {
    std::uint32_t count = 0;
    queue_families(physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    queue_families(physical_device, &count, properties.data());
    usable = family < count &&
                     (properties[family].queueFlags &
                      (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                       VK_QUEUE_TRANSFER_BIT)) != 0
                 ? VK_TRUE
                 : VK_FALSE;
  }
  if (supported != nullptr) {
    *supported = usable;
  }
  *result = supported != nullptr ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  return true;
}

bool VulkanCaptureSwapchains::SurfaceCapabilities(
    VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities,
    VkResult* result) {
  Surface found;
  if (!impl_->FindSurface(surface, &found)) {
    return false;
  }
  if (capabilities == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
    return true;
  }
  *capabilities = {};
  capabilities->minImageCount = kMinImages;
  capabilities->maxImageCount = kMaxImages;
  capabilities->currentExtent = {found.width, found.height};
  capabilities->minImageExtent = capabilities->currentExtent;
  capabilities->maxImageExtent = capabilities->currentExtent;
  capabilities->maxImageArrayLayers = 1;
  capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  capabilities->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  capabilities->supportedUsageFlags =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  *result = VK_SUCCESS;
  return true;
}

bool VulkanCaptureSwapchains::SurfaceCapabilities2(
    const VkPhysicalDeviceSurfaceInfo2KHR* info,
    VkSurfaceCapabilities2KHR* capabilities, VkResult* result) {
  if (info == nullptr || !impl_->FindSurface(info->surface, nullptr)) {
    return false;
  }
  if (capabilities == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
    return true;
  }
  SurfaceCapabilities(info->surface, &capabilities->surfaceCapabilities,
                      result);
  for (auto* current = static_cast<VkBaseOutStructure*>(capabilities->pNext);
       current != nullptr; current = current->pNext) {
    if (current->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR) {
      reinterpret_cast<VkSurfaceProtectedCapabilitiesKHR*>(current)
          ->supportsProtected = VK_FALSE;
    } else if (current->sType ==
               VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR) {
      reinterpret_cast<VkSharedPresentSurfaceCapabilitiesKHR*>(current)
          ->sharedPresentSupportedUsageFlags = 0;
    }
  }
  return true;
}

bool VulkanCaptureSwapchains::SurfaceFormats(VkSurfaceKHR surface,
                                             std::uint32_t* count,
                                             VkSurfaceFormatKHR* formats,
                                             VkResult* result) {
  if (!impl_->FindSurface(surface, nullptr)) {
    return false;
  }
  if (count == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
  } else if (formats == nullptr) {
    *count = kSurfaceFormatCount;
    *result = VK_SUCCESS;
  } else {
    const std::uint32_t copied = std::min(*count, kSurfaceFormatCount);
    std::copy_n(kSurfaceFormats, copied, formats);
    *count = copied;
    *result = copied < kSurfaceFormatCount ? VK_INCOMPLETE : VK_SUCCESS;
  }
  return true;
}

bool VulkanCaptureSwapchains::SurfaceFormats2(
    const VkPhysicalDeviceSurfaceInfo2KHR* info, std::uint32_t* count,
    VkSurfaceFormat2KHR* formats, VkResult* result) {
  if (info == nullptr || !impl_->FindSurface(info->surface, nullptr)) {
    return false;
  }
  if (count == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
  } else if (formats == nullptr) {
    *count = kSurfaceFormatCount;
    *result = VK_SUCCESS;
  } else {
    const std::uint32_t copied = std::min(*count, kSurfaceFormatCount);
    for (std::uint32_t index = 0; index < copied; ++index) {
      formats[index].surfaceFormat = kSurfaceFormats[index];
    }
    *count = copied;
    *result = copied < kSurfaceFormatCount ? VK_INCOMPLETE : VK_SUCCESS;
  }
  return true;
}

bool VulkanCaptureSwapchains::PresentModes(VkSurfaceKHR surface,
                                           std::uint32_t* count,
                                           VkPresentModeKHR* modes,
                                           VkResult* result) {
  if (!impl_->FindSurface(surface, nullptr)) {
    return false;
  }
  static constexpr VkPresentModeKHR kModes[] = {
      VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
  constexpr std::uint32_t kCount = 4;
  if (count == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
  } else if (modes == nullptr) {
    *count = kCount;
    *result = VK_SUCCESS;
  } else {
    const std::uint32_t copied = std::min(*count, kCount);
    std::copy_n(kModes, copied, modes);
    *count = copied;
    *result = copied < kCount ? VK_INCOMPLETE : VK_SUCCESS;
  }
  return true;
}

bool VulkanCaptureSwapchains::CreateSwapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* info,
    VkSwapchainKHR* swapchain, VkResult* result) {
  if (info == nullptr || swapchain == nullptr ||
      impl_->surface_count.load(std::memory_order_acquire) == 0) {
    return false;
  }
  Surface surface;
  std::shared_ptr<Device> owner;
  std::shared_ptr<Chain> old;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->surfaces.find(info->surface);
    if (found == impl_->surfaces.end()) {
      return false;
    }
    surface = *found->second;
    if (const auto d = impl_->devices.find(device);
        d != impl_->devices.end()) {
      owner = d->second;
    }
    if (const auto o = impl_->chains.find(info->oldSwapchain);
        info->oldSwapchain != VK_NULL_HANDLE && o != impl_->chains.end()) {
      old = o->second;
    }
  }
  *result = VK_ERROR_INITIALIZATION_FAILED;
  if (owner == nullptr) {
    std::fprintf(stderr, "  [vulkan] capture swapchain on an unknown device\n");
    return true;
  }
  if (info->imageExtent.width != surface.width ||
      info->imageExtent.height != surface.height || surface.width == 0 ||
      surface.height == 0) {
    std::fprintf(stderr,
                 "  [vulkan] capture swapchain %ux%u for a %ux%u surface\n",
                 info->imageExtent.width, info->imageExtent.height,
                 surface.width, surface.height);
    return true;
  }
  if (info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
      info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM) {
    std::fprintf(stderr, "  [vulkan] capture swapchain format %d unsupported\n",
                 static_cast<int>(info->imageFormat));
    return true;
  }
  auto chain = std::make_shared<Chain>();
  chain->device = owner;
  chain->surface = surface;
  chain->extent = info->imageExtent;
  chain->pixels = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM
                      ? BLOX7_ENCODER_PIXELS_BGRA
                      : BLOX7_ENCODER_PIXELS_RGBA;
  const std::uint32_t count =
      std::clamp(std::max(info->minImageCount, kMinImages), kMinImages,
                 kMaxImages);
  if (!CreateImages(*chain, *info, count)) {
    DestroyResources(*chain);
    std::fprintf(stderr, "  [vulkan] cannot allocate the capture swapchain\n");
    *result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return true;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    chain->free_images.push_back(index);
  }
  if (old != nullptr) {
    std::lock_guard<std::mutex> lock(old->mutex);
    old->retired = true;
    old->changed.notify_all();
  }
  chain->worker = std::thread(RunWorker, chain);
  const auto handle = reinterpret_cast<VkSwapchainKHR>(chain.get());
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->chains[handle] = chain;
    impl_->chain_count.store(impl_->chains.size(), std::memory_order_release);
  }
  std::fprintf(stderr,
               "  [vulkan] capture swapchain %ux%u, format %d, %u images\n",
               chain->extent.width, chain->extent.height,
               static_cast<int>(info->imageFormat), count);
  *swapchain = handle;
  *result = VK_SUCCESS;
  return true;
}

bool VulkanCaptureSwapchains::DestroySwapchain(VkDevice, VkSwapchainKHR swapchain) {
  if (swapchain == VK_NULL_HANDLE ||
      impl_->chain_count.load(std::memory_order_acquire) == 0) {
    return false;
  }
  std::shared_ptr<Chain> chain;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->chains.find(swapchain);
    if (found == impl_->chains.end()) {
      return false;
    }
    chain = std::move(found->second);
    impl_->chains.erase(found);
    impl_->chain_count.store(impl_->chains.size(), std::memory_order_release);
  }
  Shutdown(*chain);
  return true;
}

bool VulkanCaptureSwapchains::SwapchainImages(VkDevice, VkSwapchainKHR swapchain,
                                              std::uint32_t* count,
                                              VkImage* images,
                                              VkResult* result) {
  const std::shared_ptr<Chain> chain = impl_->FindChain(swapchain);
  if (chain == nullptr) {
    return false;
  }
  const auto total = static_cast<std::uint32_t>(chain->images.size());
  if (count == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
  } else if (images == nullptr) {
    *count = total;
    *result = VK_SUCCESS;
  } else {
    const std::uint32_t copied = std::min(*count, total);
    for (std::uint32_t index = 0; index < copied; ++index) {
      images[index] = chain->images[index].image;
    }
    *count = copied;
    *result = copied < total ? VK_INCOMPLETE : VK_SUCCESS;
  }
  return true;
}

bool VulkanCaptureSwapchains::AcquireImage(VkDevice device,
                                           VkSwapchainKHR swapchain,
                                           std::uint64_t timeout,
                                           VkSemaphore semaphore, VkFence fence,
                                           std::uint32_t* index,
                                           VkResult* result) {
  const std::shared_ptr<Chain> chain = impl_->FindChain(swapchain);
  if (chain == nullptr) {
    return false;
  }
  if (index == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
    return true;
  }
  {
    std::unique_lock<std::mutex> lock(chain->mutex);
    const auto ready = [&chain] {
      return chain->retired || chain->lost != VK_SUCCESS ||
             !chain->free_images.empty();
    };
    if (timeout >= kForeverNs) {
      chain->changed.wait(lock, ready);
    } else if (!chain->changed.wait_for(lock, std::chrono::nanoseconds(timeout),
                                        ready)) {
      *result = timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
      return true;
    }
    if (chain->lost != VK_SUCCESS) {
      *result = chain->lost;
      return true;
    }
    if (chain->retired) {
      *result = VK_ERROR_OUT_OF_DATE_KHR;
      return true;
    }
    *index = chain->free_images.front();
    chain->free_images.pop_front();
    chain->images[*index].state = ImageState::kAcquired;
  }
  const std::shared_ptr<Device>& owner = chain->device;
  bool signaled_fence = false;
  if (fence != VK_NULL_HANDLE && owner->spare_queue != VK_NULL_HANDLE &&
      owner->spare_queue_mutex != nullptr) {
    std::lock_guard<std::mutex> lock(*owner->spare_queue_mutex);
    signaled_fence =
        owner->f.vkQueueSubmit(owner->spare_queue, 0, nullptr, fence) ==
        VK_SUCCESS;
  }
  if (semaphore != VK_NULL_HANDLE ||
      (fence != VK_NULL_HANDLE && !signaled_fence)) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (semaphore != VK_NULL_HANDLE) {
      impl_->pending_semaphores[semaphore] = device;
    }
    if (fence != VK_NULL_HANDLE && !signaled_fence) {
      impl_->pending_fences.emplace_back(device, fence);
    }
    impl_->CountPendingLocked();
  }
  *result = VK_SUCCESS;
  return true;
}

bool VulkanCaptureSwapchains::RefreshCycle(VkDevice, VkSwapchainKHR swapchain,
                                           VkRefreshCycleDurationGOOGLE* duration,
                                           VkResult* result) {
  if (impl_->FindChain(swapchain) == nullptr) {
    return false;
  }
  if (duration != nullptr) {
    duration->refreshDuration = kRefreshNs;
  }
  *result = duration != nullptr ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  return true;
}

bool VulkanCaptureSwapchains::PastPresentationTiming(
    VkDevice, VkSwapchainKHR swapchain, std::uint32_t* count,
    VkPastPresentationTimingGOOGLE*, VkResult* result) {
  if (impl_->FindChain(swapchain) == nullptr) {
    return false;
  }
  if (count != nullptr) {
    *count = 0;
  }
  *result = count != nullptr ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
  return true;
}

VkResult VulkanCaptureSwapchains::BeforeSubmit(VkQueue queue,
                                               std::uint32_t submit_count,
                                               const VkSubmitInfo* submits) {
  if (impl_->pending_count.load(std::memory_order_acquire) == 0) {
    return VK_SUCCESS;
  }
  std::vector<VkSemaphore> waits;
  for (std::uint32_t index = 0; submits != nullptr && index < submit_count;
       ++index) {
    waits.insert(waits.end(), submits[index].pWaitSemaphores,
                 submits[index].pWaitSemaphores +
                     submits[index].waitSemaphoreCount);
  }
  return impl_->SignalPending(queue, waits);
}

VkResult VulkanCaptureSwapchains::BeforeSubmit2(VkQueue queue,
                                                std::uint32_t submit_count,
                                                const VkSubmitInfo2* submits) {
  if (impl_->pending_count.load(std::memory_order_acquire) == 0) {
    return VK_SUCCESS;
  }
  std::vector<VkSemaphore> waits;
  for (std::uint32_t index = 0; submits != nullptr && index < submit_count;
       ++index) {
    for (std::uint32_t wait = 0; wait < submits[index].waitSemaphoreInfoCount;
         ++wait) {
      waits.push_back(submits[index].pWaitSemaphoreInfos[wait].semaphore);
    }
  }
  return impl_->SignalPending(queue, waits);
}

VkResult VulkanCaptureSwapchains::BeforeBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info) {
  if (impl_->pending_count.load(std::memory_order_acquire) == 0) {
    return VK_SUCCESS;
  }
  std::vector<VkSemaphore> waits;
  for (std::uint32_t index = 0; bind_info != nullptr && index < bind_info_count;
       ++index) {
    waits.insert(waits.end(), bind_info[index].pWaitSemaphores,
                 bind_info[index].pWaitSemaphores +
                     bind_info[index].waitSemaphoreCount);
  }
  return impl_->SignalPending(queue, waits);
}

bool VulkanCaptureSwapchains::QueuePresent(VkQueue queue,
                                           const VkPresentInfoKHR* present_info,
                                           PFN_vkQueuePresentKHR next,
                                           VkResult* result) {
  if (present_info == nullptr) {
    return false;
  }
  if (impl_->pending_count.load(std::memory_order_acquire) != 0) {
    const VkResult signaled = impl_->SignalPending(
        queue, std::vector<VkSemaphore>(
                   present_info->pWaitSemaphores,
                   present_info->pWaitSemaphores +
                       present_info->waitSemaphoreCount));
    if (signaled != VK_SUCCESS) {
      *result = signaled;
      return true;
    }
  }
  if (impl_->chain_count.load(std::memory_order_acquire) == 0 ||
      present_info->swapchainCount == 0) {
    return false;
  }
  const std::uint32_t count = present_info->swapchainCount;
  std::vector<std::shared_ptr<Chain>> captured(count);
  std::shared_ptr<Device> device;
  std::uint32_t family = UINT32_MAX;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    bool any = false;
    for (std::uint32_t index = 0; index < count; ++index) {
      if (const auto found = impl_->chains.find(present_info->pSwapchains[index]);
          found != impl_->chains.end()) {
        captured[index] = found->second;
        any = true;
      }
    }
    if (!any) {
      return false;
    }
    if (const auto bound = impl_->queues.find(queue);
        bound != impl_->queues.end()) {
      family = bound->second.second;
      if (const auto d = impl_->devices.find(bound->second.first);
          d != impl_->devices.end()) {
        device = d->second;
      }
    }
  }
  const std::int64_t now = MonotonicUs();
  std::uint32_t host_count = 0;
  std::uint32_t last_capture = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (captured[index] == nullptr) {
      ++host_count;
    } else {
      last_capture = index;
    }
  }
  std::vector<VkResult> results(count, VK_SUCCESS);
  const std::vector<VkPipelineStageFlags> stages(
      present_info->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
  bool holds_signal = present_info->waitSemaphoreCount != 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (captured[index] == nullptr) {
      continue;
    }
    Chain& chain = *captured[index];
    const std::uint32_t image_index = present_info->pImageIndices[index];
    if (device == nullptr || chain.device != device) {
      results[index] = VK_ERROR_INITIALIZATION_FAILED;
      continue;
    }
    std::lock_guard<std::mutex> lock(chain.mutex);
    if (image_index >= chain.images.size() ||
        chain.images[image_index].state != ImageState::kAcquired) {
      results[index] = VK_ERROR_OUT_OF_DATE_KHR;
      continue;
    }
    Image& image = chain.images[image_index];
    const Recorded* recorded = RecordFor(chain, family);
    VkResult submitted = VK_ERROR_OUT_OF_HOST_MEMORY;
    const bool more = host_count != 0 || index < last_capture;
    if (recorded != nullptr) {
      device->f.vkResetFences(device->handle, 1, &image.fence);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.waitSemaphoreCount = present_info->waitSemaphoreCount;
      submit.pWaitSemaphores = present_info->pWaitSemaphores;
      submit.pWaitDstStageMask = stages.data();
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &recorded->commands[image_index];
      submit.signalSemaphoreCount =
          more ? present_info->waitSemaphoreCount : 0;
      submit.pSignalSemaphores = present_info->pWaitSemaphores;
      submitted = device->f.vkQueueSubmit(queue, 1, &submit, image.fence);
    }
    if (submitted != VK_SUCCESS) {
      results[index] = submitted;
      image.state = ImageState::kFree;
      chain.free_images.push_back(image_index);
      chain.changed.notify_all();
      continue;
    }
    holds_signal = more && present_info->waitSemaphoreCount != 0;
    image.state = ImageState::kPresented;
    image.present_us = now;
    chain.surface.ops->queue(chain.surface.window, now);
    chain.presented.push_back(image_index);
    chain.changed.notify_all();
  }

  VkResult overall = VK_SUCCESS;
  if (host_count != 0) {
    std::vector<VkSwapchainKHR> swapchains;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> positions;
    for (std::uint32_t index = 0; index < count; ++index) {
      if (captured[index] == nullptr) {
        swapchains.push_back(present_info->pSwapchains[index]);
        indices.push_back(present_info->pImageIndices[index]);
        positions.push_back(index);
      }
    }
    std::vector<VkResult> host_results(host_count, VK_SUCCESS);
    FilteredPresentChain filtered;
    FilterPresentChain(present_info->pNext, positions, &filtered);
    VkPresentInfoKHR reduced = *present_info;
    reduced.pNext = filtered.head;
    reduced.swapchainCount = host_count;
    reduced.pSwapchains = swapchains.data();
    reduced.pImageIndices = indices.data();
    reduced.pResults = host_results.data();
    overall = next(queue, &reduced);
    for (std::uint32_t index = 0; index < host_count; ++index) {
      results[positions[index]] = host_results[index];
    }
  } else if (holds_signal && device != nullptr) {
    VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    drain.waitSemaphoreCount = present_info->waitSemaphoreCount;
    drain.pWaitSemaphores = present_info->pWaitSemaphores;
    drain.pWaitDstStageMask = stages.data();
    device->f.vkQueueSubmit(queue, 1, &drain, VK_NULL_HANDLE);
  }
  for (const VkResult entry : results) {
    if (entry < 0 && overall >= 0) {
      overall = entry;
    }
  }
  if (present_info->pResults != nullptr) {
    std::copy(results.begin(), results.end(), present_info->pResults);
  }
  *result = overall;
  return true;
}

}  // namespace graphics
}  // namespace blox7
