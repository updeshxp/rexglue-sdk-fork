#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <string>
#include <vector>

#include <rex/ui/graphics_provider.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/ui_samplers.h>

namespace rex {
namespace ui {
namespace vulkan {

struct DeviceInfo {
  std::string name;

  // True if an earlier entry in the same EnumerateDevices() call has the
  // identical VkPhysicalDeviceIDProperties::deviceUUID -- a known ICD quirk
  // on some hybrid-graphics Windows laptops that registers one physical
  // adapter twice, not a second distinct GPU. A UI should skip rendering a
  // selectable item for these (selecting the earlier, canonical entry
  // selects the exact same physical device), but this entry's index is
  // still a valid, equivalent choice for `vulkan_device`. Always false if
  // VK_KHR_get_physical_device_properties2 isn't supported (no UUID to
  // compare), so duplicates then show up uncollapsed.
  bool is_duplicate_of_earlier = false;
};

// Lists Vulkan-capable physical devices on this system, in the same order as
// the index a UI would pass to the `vulkan_device` cvar (see
// VulkanProvider::Create) -- i.e. this vector's size and order always
// matches what VulkanProvider::Create itself enumerates, even for entries
// flagged as duplicates. Independent of whether a VulkanProvider has been
// created yet -- for building a device-selection dropdown before graphics
// setup runs. Creates and destroys a throwaway VkInstance; returns an empty
// vector if Vulkan isn't available on this system.
std::vector<DeviceInfo> EnumerateDevices();

class VulkanProvider : public GraphicsProvider {
 public:
  static std::unique_ptr<VulkanProvider> Create(bool with_gpu_emulation, bool with_presentation);

  VulkanInstance* vulkan_instance() const { return vulkan_instance_.get(); }

  VulkanDevice* vulkan_device() const { return vulkan_device_.get(); }

  // nullptr if created without presentation support.
  const UISamplers* ui_samplers() const { return ui_samplers_.get(); }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit VulkanProvider() = default;

  std::unique_ptr<VulkanInstance> vulkan_instance_;

  // Depends on the instance.
  std::unique_ptr<VulkanDevice> vulkan_device_;

  // Depends on the device.
  std::unique_ptr<UISamplers> ui_samplers_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
