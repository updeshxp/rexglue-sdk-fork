/**
 * @file        graphics/native/native_shared_memory.h
 * @brief       Native GPU renderer - guest-RAM shared memory mirror.
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     Minimal SharedMemory subclass for the native backend. The base
 *              rex::graphics::SharedMemory does all the page-validity tracking;
 *              this subclass only owns the host GPU buffer that mirrors the
 *              512 MB guest physical memory and uploads dirty ranges into it.
 *
 *              Unlike the Vulkan emulation backend (which uses a sparse,
 *              device-local buffer plus a staging upload pool), the native
 *              backend keeps things simple for the first-geometry milestone: a
 *              single non-sparse HOST_VISIBLE | HOST_COHERENT buffer that is
 *              persistently mapped, so UploadRanges is a plain memcpy from guest
 *              memory. Buffer offset N corresponds to guest physical address N
 *              (both masked to 0x1FFFFFFF), matching the in-shader vertex fetch
 *              addressing the SPIR-V translator emits. Host writes performed
 *              before a queue submission are automatically visible to that
 *              submission, so no staging copy or barrier is needed.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/shared_memory.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::native {

class NativeSharedMemory : public SharedMemory {
 public:
  NativeSharedMemory(const ui::vulkan::VulkanDevice* vulkan_device, memory::Memory& memory);
  ~NativeSharedMemory() override;

  bool Initialize();
  void Shutdown();

  VkBuffer buffer() const { return buffer_; }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory_ = VK_NULL_HANDLE;
  uint8_t* mapping_ = nullptr;
};

}  // namespace rex::graphics::native
