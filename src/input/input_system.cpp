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

#include <algorithm>
#include <cmath>
#include <functional>

#include <rex/dbg.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
namespace rex::input {

namespace {

// A physical input source (digital button or analog trigger) that can be
// remapped to act as any other physical input, button or trigger alike.
struct RemapEntry {
  const char* name;
  X_INPUT_GAMEPAD_BUTTON button;  // 0 if this entry is a trigger.
  std::function<const std::string&()> get_target;
};

constexpr uint8_t kTriggerRemapThreshold = 30;  // Matches XINPUT_GAMEPAD_TRIGGER_THRESHOLD.

}  // namespace

#define REMAP_ALLOWED_VALUES                                                                      \
  {"dpad_up",     "dpad_down",     "dpad_left",      "dpad_right", "start", "back", "left_thumb", \
   "right_thumb", "left_shoulder", "right_shoulder", "guide",      "a",     "b",    "x",          \
   "y",           "left_trigger",  "right_trigger"}

REXCVAR_DEFINE_STRING(remap_dpad_up, "dpad_up", "Input/Remap/Controller", "D-pad up")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_dpad_down, "dpad_down", "Input/Remap/Controller", "D-pad down")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_dpad_left, "dpad_left", "Input/Remap/Controller", "D-pad left")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_dpad_right, "dpad_right", "Input/Remap/Controller", "D-pad right")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_start, "start", "Input/Remap/Controller", "Start button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_back, "back", "Input/Remap/Controller", "Back button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_left_thumb, "left_thumb", "Input/Remap/Controller", "Left stick press")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_right_thumb, "right_thumb", "Input/Remap/Controller",
                      "Right stick press")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_left_shoulder, "left_shoulder", "Input/Remap/Controller",
                      "Left shoulder")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_right_shoulder, "right_shoulder", "Input/Remap/Controller",
                      "Right shoulder")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_guide, "guide", "Input/Remap/Controller", "Guide button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_a, "a", "Input/Remap/Controller", "A button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_b, "b", "Input/Remap/Controller", "B button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_x, "x", "Input/Remap/Controller", "X button")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_y, "y", "Input/Remap/Controller", "Y button")
    .allowed(REMAP_ALLOWED_VALUES);

REXCVAR_DEFINE_STRING(remap_left_trigger, "left_trigger", "Input/Remap/Controller", "Left trigger")
    .allowed(REMAP_ALLOWED_VALUES);
REXCVAR_DEFINE_STRING(remap_right_trigger, "right_trigger", "Input/Remap/Controller",
                      "Right trigger")
    .allowed(REMAP_ALLOWED_VALUES);

#undef REMAP_ALLOWED_VALUES

namespace {

const std::vector<RemapEntry>& RemapTable() {
  static const std::vector<RemapEntry> table = {
      {"dpad_up", X_INPUT_GAMEPAD_DPAD_UP,
       []() -> const std::string& { return REXCVAR_GET(remap_dpad_up); }},
      {"dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN,
       []() -> const std::string& { return REXCVAR_GET(remap_dpad_down); }},
      {"dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT,
       []() -> const std::string& { return REXCVAR_GET(remap_dpad_left); }},
      {"dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT,
       []() -> const std::string& { return REXCVAR_GET(remap_dpad_right); }},
      {"start", X_INPUT_GAMEPAD_START,
       []() -> const std::string& { return REXCVAR_GET(remap_start); }},
      {"back", X_INPUT_GAMEPAD_BACK,
       []() -> const std::string& { return REXCVAR_GET(remap_back); }},
      {"left_thumb", X_INPUT_GAMEPAD_LEFT_THUMB,
       []() -> const std::string& { return REXCVAR_GET(remap_left_thumb); }},
      {"right_thumb", X_INPUT_GAMEPAD_RIGHT_THUMB,
       []() -> const std::string& { return REXCVAR_GET(remap_right_thumb); }},
      {"left_shoulder", X_INPUT_GAMEPAD_LEFT_SHOULDER,
       []() -> const std::string& { return REXCVAR_GET(remap_left_shoulder); }},
      {"right_shoulder", X_INPUT_GAMEPAD_RIGHT_SHOULDER,
       []() -> const std::string& { return REXCVAR_GET(remap_right_shoulder); }},
      {"guide", X_INPUT_GAMEPAD_GUIDE,
       []() -> const std::string& { return REXCVAR_GET(remap_guide); }},
      {"a", X_INPUT_GAMEPAD_A, []() -> const std::string& { return REXCVAR_GET(remap_a); }},
      {"b", X_INPUT_GAMEPAD_B, []() -> const std::string& { return REXCVAR_GET(remap_b); }},
      {"x", X_INPUT_GAMEPAD_X, []() -> const std::string& { return REXCVAR_GET(remap_x); }},
      {"y", X_INPUT_GAMEPAD_Y, []() -> const std::string& { return REXCVAR_GET(remap_y); }},
      {"left_trigger", static_cast<X_INPUT_GAMEPAD_BUTTON>(0),
       []() -> const std::string& { return REXCVAR_GET(remap_left_trigger); }},
      {"right_trigger", static_cast<X_INPUT_GAMEPAD_BUTTON>(0),
       []() -> const std::string& { return REXCVAR_GET(remap_right_trigger); }},
  };
  return table;
}

const RemapEntry* EntryByName(const std::string& name) {
  for (auto& entry : RemapTable()) {
    if (name == entry.name) {
      return &entry;
    }
  }
  return nullptr;
}

// Applies the physical input remap table, allowing any digital button or
// analog trigger to be reassigned to act as any other button or trigger.
void ApplyRemap(uint16_t orig_buttons, uint8_t orig_left_trigger, uint8_t orig_right_trigger,
                uint16_t& out_buttons, uint8_t& out_left_trigger, uint8_t& out_right_trigger) {
  out_buttons = 0;
  out_left_trigger = 0;
  out_right_trigger = 0;

  for (auto& entry : RemapTable()) {
    bool active;
    uint8_t magnitude;
    if (entry.button) {
      active = (orig_buttons & entry.button) != 0;
      magnitude = 255;
    } else if (std::string(entry.name) == "left_trigger") {
      magnitude = orig_left_trigger;
      active = magnitude > kTriggerRemapThreshold;
    } else {
      magnitude = orig_right_trigger;
      active = magnitude > kTriggerRemapThreshold;
    }
    if (!active) {
      continue;
    }

    const std::string& target = entry.get_target();
    if (target == "left_trigger") {
      out_left_trigger = std::max(out_left_trigger, magnitude);
    } else if (target == "right_trigger") {
      out_right_trigger = std::max(out_right_trigger, magnitude);
    } else if (const RemapEntry* target_entry = EntryByName(target)) {
      out_buttons |= static_cast<uint16_t>(target_entry->button);
    }
  }
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetForceActive(bool force) {
  for (auto& driver : drivers_) {
    driver->set_force_active(force);
  }
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetCapabilities(user_index, flags, out_caps);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  bool first_result = true;
  X_INPUT_STATE merged = {};

  for (auto& driver : drivers_) {
    X_INPUT_STATE state = {};
    X_RESULT result = driver->GetState(user_index, &state);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      // remap_* cvars represent a physical controller's button layout being
      // reassigned, so they only apply to drivers backed by a real gamepad.
      // Drivers like the MnK driver already output the final logical button
      // the user bound a key to, and must be merged in unremapped.
      if (driver->is_physical_device()) {
        uint16_t remapped_buttons;
        uint8_t remapped_left_trigger, remapped_right_trigger;
        ApplyRemap(static_cast<uint16_t>(state.gamepad.buttons), state.gamepad.left_trigger,
                   state.gamepad.right_trigger, remapped_buttons, remapped_left_trigger,
                   remapped_right_trigger);
        state.gamepad.buttons = remapped_buttons;
        state.gamepad.left_trigger = remapped_left_trigger;
        state.gamepad.right_trigger = remapped_right_trigger;
      }

      if (first_result) {
        merged = state;
        first_result = false;
      } else {
        // Merge: OR buttons, max triggers, max-magnitude sticks
        merged.gamepad.buttons = static_cast<uint16_t>(merged.gamepad.buttons) |
                                 static_cast<uint16_t>(state.gamepad.buttons);
        merged.gamepad.left_trigger =
            std::max(merged.gamepad.left_trigger, state.gamepad.left_trigger);
        merged.gamepad.right_trigger =
            std::max(merged.gamepad.right_trigger, state.gamepad.right_trigger);

        auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
          return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
        };
        merged.gamepad.thumb_lx = merge_axis(merged.gamepad.thumb_lx, state.gamepad.thumb_lx);
        merged.gamepad.thumb_ly = merge_axis(merged.gamepad.thumb_ly, state.gamepad.thumb_ly);
        merged.gamepad.thumb_rx = merge_axis(merged.gamepad.thumb_rx, state.gamepad.thumb_rx);
        merged.gamepad.thumb_ry = merge_axis(merged.gamepad.thumb_ry, state.gamepad.thumb_ry);

        if (static_cast<uint32_t>(state.packet_number) >
            static_cast<uint32_t>(merged.packet_number)) {
          merged.packet_number = state.packet_number;
        }
      }
    }
  }

  if (first_result) {
    return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->SetState(user_index, vibration);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetKeystroke(user_index, flags, out_keystroke);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace rex::input
