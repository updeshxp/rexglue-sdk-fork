/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

void SetMouseLookActive(bool active);
bool IsMouseLookActive();

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  bool is_physical_device() const override { return false; }

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  // Returns raw, un-decayed relative mouse motion accumulated since the last
  // call, then hard-resets it to zero (drain semantics). For hooks that write
  // straight into a processed "final" value (bypassing the guest's own
  // deadzone/response-curve/ramp entirely) rather than emulating an XINPUT
  // stick that still has to pass through that pipeline. out_dx/out_dy are in
  // the same raw relative-motion units as MouseEvent::rel_x/rel_y (not
  // pre-scaled to stick range) -- callers apply their own sensitivity/scale.
  // Returns false (out params untouched) if MnK mode is off, the window
  // doesn't have focus, or the mouse isn't captured.
  bool TryGetLookDelta(int32_t* out_dx, int32_t* out_dy);

 private:
  bool IsEnabled() const;
  void UpdateMouseCapture();
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  // Decays mouse_dx_/mouse_dy_ toward zero based on time elapsed since the
  // last decay, then updates last_decay_time_. Called from both OnMouseMove
  // and GetState's drain so the decay is applied at whatever granularity
  // either is invoked, rather than jumping in large, timing-dependent steps.
  void DecayMouseAccumulator();

  // Called from the guest thread. The rest of the capture path stays on the UI
  // thread, since every Window call in it reaches SDL.
  void QueueMouseCaptureUpdate(bool should_capture);
  void ApplyMouseCaptureFromUIThread();
  void ReleaseMouseCaptureFromUIThread(rex::ui::Window* window);
  void RecenterCursorFromUIThread(int32_t x, int32_t y);
  // Safe to call from any thread.
  void DetachFromWindow();

  // Only the UI thread writes it, so only guest thread access needs the lock.
  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking: an exponentially-decaying accumulator (not a
  // per-poll delta that gets hard-reset to zero) of motion while the mouse is
  // captured (see MouseEvent::dx/dy, or an absolute-position diff when the
  // platform can't lock the pointer). A hard reset makes a single mouse-move
  // event read as full stick deflection for exactly one poll and then snap to
  // zero the instant no new event has arrived by the next poll -- unlike a
  // physical stick, which stays deflected for as long as it's held. That
  // starves any turn-rate ramp/acceleration in the guest's camera code of the
  // sustained "stick held" input it needs to reach full speed, even though
  // the instantaneous stick value is already maxed. Decaying instead keeps
  // the reported stick "held" for a short tail after each flick, closer to
  // how a gamepad stick behaves.
  double mouse_dx_ = 0.0;
  double mouse_dy_ = 0.0;
  std::chrono::steady_clock::time_point last_decay_time_{};
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // True while the window has the pointer locked and is reporting relative
  // motion, in which case MouseEvent::dx/dy are used directly instead of
  // differencing absolute positions.
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;

  // Guest thread to UI thread. The queued flag coalesces the posts.
  std::atomic<bool> mouse_capture_requested_{false};
  std::atomic<bool> mouse_capture_update_queued_{false};

  std::atomic<bool> has_focus_{true};

  // GetState() is polled independently by both the guest (via
  // XamInputGetState) and the host's own gamepad-UI overlay every render
  // frame (see GamepadUiController::OnDraw), so a naive "read and zero"
  // drain of mouse_dx_/mouse_dy_ lets whichever caller happens to poll
  // first each frame steal the analog stick before the other ever sees it.
  // Coalesce polls within a few ms into a single drain so every caller in
  // that window observes the same rx/ry.
  std::chrono::steady_clock::time_point last_drain_time_{};
  int16_t cached_rx_ = 0;
  int16_t cached_ry_ = 0;
  bool have_cached_stick_ = false;

  // Raw, un-decayed relative motion accumulated since the last
  // TryGetLookDelta() drain. Separate from mouse_dx_/mouse_dy_ (the decaying
  // accumulator) so a bypass hook's drain doesn't fight GetState()'s decay
  // state or vice versa. Coalesced the same way as cached_rx_/cached_ry_ so
  // multiple callers within a frame see (and drain) the same delta once.
  double raw_delta_x_ = 0.0;
  double raw_delta_y_ = 0.0;
  std::chrono::steady_clock::time_point last_raw_drain_time_{};
  int32_t cached_raw_dx_ = 0;
  int32_t cached_raw_dy_ = 0;
  bool have_cached_raw_delta_ = false;

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
