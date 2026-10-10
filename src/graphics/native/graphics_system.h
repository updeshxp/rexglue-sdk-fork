/**
 * @file        graphics/native/graphics_system.h
 * @brief       Native GPU renderer plugin - IGraphicsSystem implementation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License (matches the rest of the SDK).
 *
 * @remarks     rexgpu-native is a GAME-AGNOSTIC native renderer plugin. It
 *              reuses the base rex::graphics::GraphicsSystem plumbing (ring
 *              buffer, MMIO register file, vsync worker, interrupts) and the
 *              base rex::graphics::CommandProcessor PM4 parser, and replaces
 *              only the backend draw/resolve/present seams with native GPU
 *              command translation. This is the same seam the emulated
 *              rexgpu-xenos plugin attaches to, so any title selects it with
 *              --gpu_plugin native without game-specific hooks.
 *
 *              SCAFFOLD STATUS: the backend seams currently log the command
 *              stream and do not yet render. See command_processor.h.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>

namespace rex::graphics::native {

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override;

 protected:
  // Reuse the existing Vulkan provider so window + presenter infrastructure
  // stands up unchanged. The native renderer only replaces what happens
  // *inside* the command processor, not how frames reach the screen.
  void CreateProvider(bool with_presentation) override;

  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::native
