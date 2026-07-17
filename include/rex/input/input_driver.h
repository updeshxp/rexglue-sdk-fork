#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstddef>
#include <functional>
#include <vector>

#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/kernel.h>
#include <rex/ui/window.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem;

class InputDriver {
 public:
  virtual ~InputDriver() = default;

  virtual X_STATUS Setup() = 0;

  /// Devices this driver has open, in its own arrival order. InputSystem
  /// assigns the cross-driver ordinal, so leave DeviceInfo::ordinal at zero.
  virtual void EnumerateDevices(std::vector<DeviceInfo>& out) = 0;

  virtual X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) = 0;
  virtual X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) = 0;
  virtual X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) = 0;
  virtual X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) = 0;

  virtual void OnWindowAvailable(rex::ui::Window* /*window*/) {}

  // True for drivers backed by a physical gamepad (SDL/XInput), whose button
  // state should pass through the remap_* physical-input remap table. False
  // for drivers like the MnK driver that synthesize a logical button state
  // directly from a keybind_* cvar -- that state already *is* the intended
  // logical button and must not be rerouted by remap_*.
  virtual bool is_physical_device() const { return true; }

  void set_is_active_callback(std::function<bool()> is_active_callback) {
    is_active_callback_ = is_active_callback;
  }

  // Bypasses the is_active_callback_ gate (e.g. an overlay's mouse-capture
  // check) so raw device state can still be read while the callback would
  // otherwise report inactive -- used by the settings overlay's keybind
  // capture, which needs to see gamepad presses even while the mouse hovers
  // its own "Press any key..." button.
  void set_force_active(bool force) { force_active_ = force; }

 protected:
  explicit InputDriver(rex::ui::Window* window, size_t window_z_order)
      : window_(window), window_z_order_(window_z_order) {}

  rex::ui::Window* window() const { return window_; }
  size_t window_z_order() const { return window_z_order_; }

  bool is_active() const { return force_active_ || !is_active_callback_ || is_active_callback_(); }

 private:
  rex::ui::Window* window_;
  size_t window_z_order_;
  std::function<bool()> is_active_callback_ = nullptr;
  bool force_active_ = false;
};

}  // namespace rex::input
