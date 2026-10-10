/**
 * @file        graphics/native/graphics_system.cpp
 * @brief       Native GPU renderer plugin - IGraphicsSystem implementation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "native/graphics_system.h"

#include <rex/graphics/command_processor.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "native/command_processor.h"

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

namespace rex::graphics::native {

NativeGraphicsSystem::NativeGraphicsSystem() = default;

NativeGraphicsSystem::~NativeGraphicsSystem() = default;

std::string NativeGraphicsSystem::name() const {
  return "Native";
}

void NativeGraphicsSystem::CreateProvider(bool with_presentation) {
#if REX_HAS_VULKAN
  // with_gpu_emulation=true keeps the provider's guest-facing services (shared
  // memory heaps, sparse binding, etc.) available; the native renderer needs
  // the same host device capabilities as the emulated backend.
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
#else
  (void)with_presentation;
  REXLOG_ERROR("rexgpu-native: no Vulkan support compiled in; cannot create provider");
#endif
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::native
