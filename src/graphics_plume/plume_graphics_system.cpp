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
#include <SDL3/SDL.h>
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
              native_win, ::plume::RenderFormat::R8G8B8A8_UNORM, 3);
          auto q = plume_device_->createCommandQueue(::plume::RenderCommandListType::DIRECT);
          if (q) {
            plume_swapchain_ = q->createSwapChain(swap_desc);
            REXLOG_INFO("PlumeGraphicsSystem: SwapChain created for ANativeWindow ({}x{})",
                        ANativeWindow_getWidth(native_win), ANativeWindow_getHeight(native_win));
          }
        }
      }
    }
#elif defined(_WIN32)
    // ReXGlue uses SDL3 for its desktop window. Plume's Windows backend accepts
    // the native HWND, so bridge SDL's window property to RenderWindow here.
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0 && windows[0]) {
      SDL_PropertiesID props = SDL_GetWindowProperties(windows[0]);
      void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
      if (hwnd) {
        ::plume::RenderSwapChainDesc swap_desc(
            static_cast<HWND>(hwnd), ::plume::RenderFormat::R8G8B8A8_UNORM, 3);
        auto q = plume_device_->createCommandQueue(::plume::RenderCommandListType::DIRECT);
        if (q) {
          plume_swapchain_ = q->createSwapChain(swap_desc);
          if (plume_swapchain_) {
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(windows[0], &width, &height);
            REXLOG_INFO("PlumeGraphicsSystem: Windows SwapChain created ({}x{})", width, height);
          } else {
            REXLOG_ERROR("PlumeGraphicsSystem: Windows SwapChain creation FAILED");
          }
        }
      } else {
        REXLOG_ERROR("PlumeGraphicsSystem: SDL window has no Win32 HWND property");
      }
    } else {
      REXLOG_WARN("PlumeGraphicsSystem: no SDL desktop window available for swapchain");
    }
#elif defined(__linux__)
    // Plume's non-SDL Linux backend expects an X11 Display + Window pair.
    // ReXGlue exposes the SDL3 window, so retrieve the X11 handles from SDL's
    // window properties rather than guessing a native handle layout.
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (windows && window_count > 0 && windows[0]) {
      SDL_PropertiesID props = SDL_GetWindowProperties(windows[0]);
      auto* display = static_cast<Display*>(
          SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
      const auto window = static_cast<Window>(
          SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
      if (display && window != 0) {
        ::plume::RenderWindow render_window{};
        render_window.display = display;
        render_window.window = window;
        ::plume::RenderSwapChainDesc swap_desc(
            render_window, ::plume::RenderFormat::R8G8B8A8_UNORM, 3);
        auto q = plume_device_->createCommandQueue(::plume::RenderCommandListType::DIRECT);
        if (q) {
          plume_swapchain_ = q->createSwapChain(swap_desc);
          if (plume_swapchain_) {
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(windows[0], &width, &height);
            REXLOG_INFO("PlumeGraphicsSystem: X11 SwapChain created ({}x{})", width, height);
          } else {
            REXLOG_ERROR("PlumeGraphicsSystem: X11 SwapChain creation FAILED");
          }
        } else {
          REXLOG_ERROR("PlumeGraphicsSystem: failed to create DIRECT queue for swapchain");
        }
      } else {
        REXLOG_WARN("PlumeGraphicsSystem: SDL window is not exposing X11 handles; "
                   "Wayland is not supported by the current Plume Linux backend");
      }
    } else {
      REXLOG_WARN("PlumeGraphicsSystem: no SDL desktop window available for swapchain");
    }
#endif
  }

  return X_STATUS_SUCCESS;
}

void PlumeGraphicsSystem::CreateProvider(bool with_presentation) {
  REXLOG_INFO("PlumeGraphicsSystem::CreateProvider (with_presentation={})", with_presentation);
  // Plume owns the guest GPU device directly. The host UI presenter is not used
  // by this MVP backend; desktop presentation is bridged to Plume's swapchain in
  // SetupPresentation above. Keep this method side-effect free for headless GPU
  // initialization.
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
