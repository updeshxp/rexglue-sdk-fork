/**
 * @file        graphics/native/native_shared_memory.cpp
 * @brief       Native GPU renderer - guest-RAM shared memory mirror.
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "native/native_shared_memory.h"

#include <algorithm>
#include <cstring>

#include <rex/bit.h>
#include <rex/logging.h>
#include <rex/memory.h>

namespace rex::graphics::native {

NativeSharedMemory::NativeSharedMemory(const ui::vulkan::VulkanDevice* vulkan_device,
                                       memory::Memory& memory)
    : SharedMemory(memory), vulkan_device_(vulkan_device) {}

NativeSharedMemory::~NativeSharedMemory() {
  Shutdown();
}

bool NativeSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // A single non-sparse buffer covering all 512 MB of guest physical memory,
  // usable as a shader storage buffer (in-shader vertex fetch) and an index
  // buffer (guest DMA indices). No sparse residency (InitializeSparseHostGpu
  // Memory is intentionally not called), so the whole buffer is resident and
  // EnsureHostGpuMemoryAllocated is a no-op.
  VkBufferCreateInfo buffer_create_info = {};
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: shared memory - failed to create the {} MB buffer",
                 kBufferSize >> 20);
    return false;
  }

  VkMemoryRequirements memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer_, &memory_requirements);

  // Prefer HOST_VISIBLE | HOST_COHERENT so uploads are a direct memcpy and are
  // implicitly visible to the device on submit.
  uint32_t memory_type_index;
  uint32_t host_visible_coherent = memory_requirements.memoryTypeBits &
                                   vulkan_device_->memory_types().host_visible &
                                   vulkan_device_->memory_types().host_coherent;
  if (!rex::bit_scan_forward(host_visible_coherent, &memory_type_index)) {
    REXLOG_ERROR("rexgpu-native: shared memory - no HOST_VISIBLE|HOST_COHERENT memory type");
    Shutdown();
    return false;
  }

  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = memory_requirements.size;
  allocate_info.memoryTypeIndex = memory_type_index;
  if (dfn.vkAllocateMemory(device, &allocate_info, nullptr, &buffer_memory_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: shared memory - failed to allocate {} MB", kBufferSize >> 20);
    Shutdown();
    return false;
  }
  if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory_, 0) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: shared memory - failed to bind buffer memory");
    Shutdown();
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, buffer_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: shared memory - failed to map buffer memory");
    Shutdown();
    return false;
  }
  mapping_ = static_cast<uint8_t*>(mapping);

  REXLOG_INFO("rexgpu-native: shared memory ready ({} MB host-visible buffer)", kBufferSize >> 20);
  return true;
}

void NativeSharedMemory::Shutdown() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, buffer_, nullptr);
    buffer_ = VK_NULL_HANDLE;
  }
  if (buffer_memory_ != VK_NULL_HANDLE) {
    if (mapping_) {
      dfn.vkUnmapMemory(device, buffer_memory_);
      mapping_ = nullptr;
    }
    dfn.vkFreeMemory(device, buffer_memory_, nullptr);
    buffer_memory_ = VK_NULL_HANDLE;
  }
  ShutdownCommon();
}

bool NativeSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  const uint32_t page_bytes_log2 = page_size_log2();
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    uint32_t start_bytes = upload_range.first << page_bytes_log2;
    uint32_t length_bytes = upload_range.second << page_bytes_log2;
    length_bytes = std::min(length_bytes, kBufferSize - start_bytes);
    // Mark valid before the copy so a concurrent CPU write during the memcpy
    // isn't lost (the base class re-invalidates on the callback).
    MakeRangeValid(start_bytes, length_bytes, false);
    std::memcpy(mapping_ + start_bytes, memory().TranslatePhysical(start_bytes), length_bytes);
  }
  return true;
}

}  // namespace rex::graphics::native
