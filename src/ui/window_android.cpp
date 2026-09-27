/**
 * @file        ui/window_android.cpp
 * @brief       Direct Android NDK Window implementation wrapping ANativeWindow
 *
 * @copyright   Copyright (c) 2026 ReXGlue Authors
 * @license     BSD 3-Clause License
 */

#include <rex/ui/window_android.h>

#include <algorithm>
#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <rex/ui/surface_android.h>
#include <rex/ui/windowed_app_context_android.h>

namespace rex::ui {

namespace {

uint32_t ResolveWindowWidth(uint32_t requested_width) {
  if (REXCVAR_GET(window_width) > 0) {
    return uint32_t(REXCVAR_GET(window_width));
  }
  if (!rex::cvar::HasNonDefaultValue("window_width")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_width") && REXCVAR_GET(video_mode_width) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_width), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_width, 1, 8192));
    }
  }
  return requested_width;
}

uint32_t ResolveWindowHeight(uint32_t requested_height) {
  if (REXCVAR_GET(window_height) > 0) {
    return uint32_t(REXCVAR_GET(window_height));
  }
  if (!rex::cvar::HasNonDefaultValue("window_height")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_height") && REXCVAR_GET(video_mode_height) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_height), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_height, 1, 8192));
    }
  }
  return requested_height;
}

}  // namespace

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  desired_logical_width = ResolveWindowWidth(desired_logical_width);
  desired_logical_height = ResolveWindowHeight(desired_logical_height);
  return std::make_unique<WindowAndroid>(app_context, title, desired_logical_width,
                                         desired_logical_height);
}

WindowAndroid::WindowAndroid(WindowedAppContext& app_context, const std::string_view title,
                             uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {
  choreographer_ = AChoreographer_getInstance();
}

WindowAndroid::~WindowAndroid() {
  EnterDestructor();
  if (native_window_) {
    ANativeWindow_release(native_window_);
    native_window_ = nullptr;
  }
}

void* WindowAndroid::GetNativeWindowHandle() const {
  return native_window_;
}

bool WindowAndroid::SetRelativeMouseMode([[maybe_unused]] bool enable) {
  return true;
}

bool WindowAndroid::WarpMouseToCenter([[maybe_unused]] int32_t& x_out,
                                      [[maybe_unused]] int32_t& y_out) {
  return true;
}

void WindowAndroid::SetNativeWindow(ANativeWindow* window) {
  if (native_window_ == window) {
    return;
  }
  if (native_window_) {
    ANativeWindow_release(native_window_);
  }
  native_window_ = window;
  if (native_window_) {
    ANativeWindow_acquire(native_window_);
    int32_t width = ANativeWindow_getWidth(native_window_);
    int32_t height = ANativeWindow_getHeight(native_window_);
    if (width > 0 && height > 0) {
      ANativeWindow_setBuffersGeometry(native_window_, width, height, WINDOW_FORMAT_RGBA_8888);
      WindowDestructionReceiver destruction_receiver(this);
      OnActualSizeUpdate(uint32_t(width), uint32_t(height), destruction_receiver);
    }
  }
  OnSurfaceChanged(native_window_ != nullptr);
}

uint32_t WindowAndroid::GetLatestDpiImpl() const {
  return GetMediumDpi();  // Standard 160 DPI base density, scaled dynamically
}

bool WindowAndroid::OpenImpl() {
  android_app_context().RegisterWindow(this);
  return true;
}

void WindowAndroid::RequestCloseImpl() {
  android_app_context().UnregisterWindow(this);
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  OnAfterClose();
}

void WindowAndroid::ApplyNewFullscreen() {}
void WindowAndroid::ApplyNewTitle() {}
void WindowAndroid::ApplyNewMouseCapture() {}
void WindowAndroid::ApplyNewMouseRelease() {}
void WindowAndroid::ApplyNewCursorVisibility([[maybe_unused]] CursorVisibility old_visibility) {}
void WindowAndroid::ApplyNewTextInputActive() {}
void WindowAndroid::FocusImpl() {}

std::unique_ptr<Surface> WindowAndroid::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!native_window_) {
    return nullptr;
  }
  if (allowed_types & Surface::kTypeFlag_AndroidNativeWindow) {
    return std::make_unique<AndroidNativeWindowSurface>(native_window_);
  }
  return nullptr;
}

void WindowAndroid::RequestPaintImpl() {
  paint_requested_.store(true, std::memory_order_release);

  if (choreographer_ && !choreographer_scheduled_.exchange(true, std::memory_order_acq_rel)) {
    AChoreographer_postFrameCallback(choreographer_, ChoreographerCallback, this);
  }
}

void WindowAndroid::ChoreographerCallback([[maybe_unused]] long frame_time_nanos, void* data) {
  auto* window = static_cast<WindowAndroid*>(data);
  window->choreographer_scheduled_.store(false, std::memory_order_release);
  if (window->paint_requested_.exchange(false, std::memory_order_acq_rel)) {
    window->OnPaint();
  }
}

void WindowAndroid::HandleFrameTick() {
  if (paint_requested_.load(std::memory_order_acquire)) {
    if (!choreographer_) {
      paint_requested_.store(false, std::memory_order_release);
      OnPaint();
    }
  }
}

}  // namespace rex::ui
