/**
 * @file        graphics_plume/plume_graphics_system.cpp
 * @brief       Plume Graphics System implementation
 */

#include "plume_graphics_system.h"
#include "plume_command_processor.h"

#include <rex/logging.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>
#if defined(__ANDROID__)
#include <rex/ui/windowed_app_context_android.h>
#include <rex/ui/window_android.h>
#endif

#include <plume_render_interface.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace rex::graphics_plume {

PlumeGraphicsSystem::PlumeGraphicsSystem() {
  REXLOG_INFO("PlumeGraphicsSystem: instantiated");
}

PlumeGraphicsSystem::~PlumeGraphicsSystem() {
  Shutdown();
}

X_STATUS PlumeGraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  REXLOG_INFO("PlumeGraphicsSystem::SetupPresentation");

  if (!plume_interface_) {
    plume_interface_ = ::plume::CreateVulkanInterface();
    if (!plume_interface_) {
      REXLOG_ERROR("PlumeGraphicsSystem: failed to create VulkanInterface");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("PlumeGraphicsSystem: VulkanInterface created successfully");

    plume_device_ = plume_interface_->createDevice("");
    if (!plume_device_) {
      REXLOG_ERROR("PlumeGraphicsSystem: failed to create Vulkan RenderDevice");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("PlumeGraphicsSystem: Vulkan RenderDevice created successfully (name: '{}')",
                plume_device_->getDescription().name);

#if defined(__ANDROID__)
    if (app_context) {
      auto* android_app = dynamic_cast<ui::AndroidWindowedAppContext*>(app_context);
      if (android_app && android_app->GetWindow()) {
        ANativeWindow* native_win = android_app->GetWindow()->GetNativeWindow();
        if (native_win) {
          ::plume::RenderSwapChainDesc swap_desc(
              native_win,
              ::plume::RenderFormat::R8G8B8A8_UNORM,
              3
          );
          auto q = plume_device_->createCommandQueue(::plume::RenderCommandListType::DIRECT);
          if (q) {
            plume_swapchain_ = q->createSwapChain(swap_desc);
            REXLOG_INFO("PlumeGraphicsSystem: SwapChain created for ANativeWindow ({}x{})",
                        ANativeWindow_getWidth(native_win), ANativeWindow_getHeight(native_win));
          }
        }
      }
    }
#endif
  }

  return X_STATUS_SUCCESS;
}

void PlumeGraphicsSystem::CreateProvider(bool with_presentation) {
  REXLOG_INFO("PlumeGraphicsSystem::CreateProvider (with_presentation={})", with_presentation);
}

std::unique_ptr<rex::graphics::CommandProcessor> PlumeGraphicsSystem::CreateCommandProcessor() {
  REXLOG_INFO("PlumeGraphicsSystem::CreateCommandProcessor");
  return std::make_unique<PlumeCommandProcessor>(this, kernel_state_, plume_device_.get());
}

void PlumeGraphicsSystem::Present() {
  if (plume_swapchain_) {
    uint32_t texture_index = 0;
    if (plume_swapchain_->acquireTexture(nullptr, &texture_index)) {
      plume_swapchain_->present(texture_index, nullptr, 0);
    }
  }
}

void PlumeGraphicsSystem::Shutdown() {
  REXLOG_INFO("PlumeGraphicsSystem::Shutdown");
  plume_swapchain_.reset();
  plume_device_.reset();
  plume_interface_.reset();
  rex::graphics::GraphicsSystem::Shutdown();
}

}  // namespace rex::graphics_plume
