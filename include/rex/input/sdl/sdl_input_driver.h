/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerOpenedAsync(SDL_JoystickID instance_id, SDL_Gamepad* controller);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  // --- Async HID output (rumble / close) worker ---
  // SDL_RumbleGamepad() and SDL_CloseGamepad() can block for a long time on
  // some devices (e.g. a DualSense over Bluetooth), so neither may ever be
  // called on the guest thread or while holding controllers_mutex_ (the
  // latter would also re-introduce a lock-order inversion against SDL's
  // internal joystick lock, see HandleEvent()). Both are instead funneled
  // through a single dedicated worker thread that never holds
  // controllers_mutex_.
  struct RumbleRequest {
    uint16_t low = 0;
    uint16_t high = 0;
    bool pending = false;
  };
  void StartHidWorker();
  void StopHidWorker();
  void HidWorkerMain();
  bool HasHidWorkLocked() const;

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees.
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  // Instance IDs currently being opened on a background thread. SDL_OpenGamepad
  // can block for a long time (e.g. a stalled Bluetooth HID handshake), so it
  // must never run on a thread that also drives game logic. Guarded by
  // controllers_mutex_.
  std::set<SDL_JoystickID> pending_opens_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;

  // Guarded by hid_mutex_. Never held together with controllers_mutex_ except
  // for the controllers_mutex_ -> hid_mutex_ nesting order documented above
  // HidWorkerMain(). hid_mutex_ is always released before any blocking SDL
  // call, so it can never be ordered ahead of SDL's internal joystick lock.
  std::thread hid_worker_;
  std::mutex hid_mutex_;
  std::condition_variable hid_cv_;
  bool hid_worker_stop_ = false;
  // Latest requested rumble per user slot; only the newest value matters.
  std::array<RumbleRequest, HID_SDL_USER_COUNT> rumble_pending_{};
  // Worker-owned instance IDs, independent of controllers_[i].sdl, so the
  // worker can resolve a live SDL_Gamepad* itself instead of being handed a
  // pointer the guest thread might be about to free. 0 = no device in slot.
  std::array<SDL_JoystickID, HID_SDL_USER_COUNT> hid_instance_{};
  // Live devices deferred to the worker to close, so a close never races an
  // in-flight rumble to the same device (both run serialized on the worker).
  std::vector<SDL_Gamepad*> hid_close_queue_;
};

}  // namespace rex::input::sdl
