/**
 * @file        graphics_plume/plume_graphics_system.h
 * @brief       Plume Graphics System implementation of rex::system::IGraphicsSystem
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/logging.h>

namespace plume {
struct RenderInterface;
struct RenderDevice;
struct RenderSwapChain;
}

namespace rex::graphics_plume {

class REX_GPU_PLUGIN_EXPORT PlumeGraphicsSystem final : public ::rex::graphics::GraphicsSystem {
 public:
  PlumeGraphicsSystem();
  ~PlumeGraphicsSystem() override;

  std::string name() const override { return "Plume"; }

  X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override;

  void Shutdown() override;

  void Present();

  ::plume::RenderDevice*    plume_device()    const { return plume_device_.get(); }
  ::plume::RenderSwapChain* plume_swapchain() const { return plume_swapchain_.get(); }

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;

 private:
  std::unique_ptr<::plume::RenderInterface> plume_interface_;
  std::unique_ptr<::plume::RenderDevice> plume_device_;
  std::unique_ptr<::plume::RenderSwapChain> plume_swapchain_;
};

}  // namespace rex::graphics_plume
