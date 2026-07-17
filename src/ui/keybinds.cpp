/**
 * @file        ui/keybinds.cpp
 * @brief       Key binding implementation. See keybinds.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/keybinds.h>
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/mod_attribution.h>
#include <mutex>
#include <string>
#include <deque>
#include <unordered_map>

namespace rex::ui {

using rex::ui::VirtualKey;

static const std::unordered_map<std::string, VirtualKey> kKeyNames = {
    // Function keys
    {"F1", VirtualKey::kF1},
    {"F2", VirtualKey::kF2},
    {"F3", VirtualKey::kF3},
    {"F4", VirtualKey::kF4},
    {"F5", VirtualKey::kF5},
    {"F6", VirtualKey::kF6},
    {"F7", VirtualKey::kF7},
    {"F8", VirtualKey::kF8},
    {"F9", VirtualKey::kF9},
    {"F10", VirtualKey::kF10},
    {"F11", VirtualKey::kF11},
    {"F12", VirtualKey::kF12},
    {"F13", VirtualKey::kF13},
    {"F14", VirtualKey::kF14},
    {"F15", VirtualKey::kF15},
    {"F16", VirtualKey::kF16},
    {"F17", VirtualKey::kF17},
    {"F18", VirtualKey::kF18},
    {"F19", VirtualKey::kF19},
    {"F20", VirtualKey::kF20},
    {"F21", VirtualKey::kF21},
    {"F22", VirtualKey::kF22},
    {"F23", VirtualKey::kF23},
    {"F24", VirtualKey::kF24},
    // Letters
    {"A", VirtualKey::kA},
    {"B", VirtualKey::kB},
    {"C", VirtualKey::kC},
    {"D", VirtualKey::kD},
    {"E", VirtualKey::kE},
    {"F", VirtualKey::kF},
    {"G", VirtualKey::kG},
    {"H", VirtualKey::kH},
    {"I", VirtualKey::kI},
    {"J", VirtualKey::kJ},
    {"K", VirtualKey::kK},
    {"L", VirtualKey::kL},
    {"M", VirtualKey::kM},
    {"N", VirtualKey::kN},
    {"O", VirtualKey::kO},
    {"P", VirtualKey::kP},
    {"Q", VirtualKey::kQ},
    {"R", VirtualKey::kR},
    {"S", VirtualKey::kS},
    {"T", VirtualKey::kT},
    {"U", VirtualKey::kU},
    {"V", VirtualKey::kV},
    {"W", VirtualKey::kW},
    {"X", VirtualKey::kX},
    {"Y", VirtualKey::kY},
    {"Z", VirtualKey::kZ},
    // Digits
    {"0", VirtualKey::k0},
    {"1", VirtualKey::k1},
    {"2", VirtualKey::k2},
    {"3", VirtualKey::k3},
    {"4", VirtualKey::k4},
    {"5", VirtualKey::k5},
    {"6", VirtualKey::k6},
    {"7", VirtualKey::k7},
    {"8", VirtualKey::k8},
    {"9", VirtualKey::k9},
    // OEM / special
    {"Backtick", VirtualKey::kOem3},
    {"Minus", VirtualKey::kOemMinus},
    {"Plus", VirtualKey::kOemPlus},
    {"Comma", VirtualKey::kOemComma},
    {"Period", VirtualKey::kOemPeriod},
    {"Semicolon", VirtualKey::kOem1},
    {"Slash", VirtualKey::kOem2},
    {"Backslash", VirtualKey::kOem5},
    {"LBracket", VirtualKey::kOem4},
    {"RBracket", VirtualKey::kOem6},
    {"Quote", VirtualKey::kOem7},
    // Control
    {"Escape", VirtualKey::kEscape},
    {"Return", VirtualKey::kReturn},
    {"Space", VirtualKey::kSpace},
    {"Tab", VirtualKey::kTab},
    {"Backspace", VirtualKey::kBack},
    {"Delete", VirtualKey::kDelete},
    {"Insert", VirtualKey::kInsert},
    {"Home", VirtualKey::kHome},
    {"End", VirtualKey::kEnd},
    {"PageUp", VirtualKey::kPrior},
    {"PageDown", VirtualKey::kNext},
    // Navigation
    {"Left", VirtualKey::kLeft},
    {"Right", VirtualKey::kRight},
    {"Up", VirtualKey::kUp},
    {"Down", VirtualKey::kDown},
    // Modifier
    {"Shift", VirtualKey::kShift},
    {"Control", VirtualKey::kControl},
    {"Alt", VirtualKey::kMenu},
    // Numpad
    {"Numpad0", VirtualKey::kNumpad0},
    {"Numpad1", VirtualKey::kNumpad1},
    {"Numpad2", VirtualKey::kNumpad2},
    {"Numpad3", VirtualKey::kNumpad3},
    {"Numpad4", VirtualKey::kNumpad4},
    {"Numpad5", VirtualKey::kNumpad5},
    {"Numpad6", VirtualKey::kNumpad6},
    {"Numpad7", VirtualKey::kNumpad7},
    {"Numpad8", VirtualKey::kNumpad8},
    {"Numpad9", VirtualKey::kNumpad9},
    {"NumpadEnter", VirtualKey::kReturn},
    {"NumpadPlus", VirtualKey::kAdd},
    {"NumpadMinus", VirtualKey::kSubtract},
    {"NumpadStar", VirtualKey::kMultiply},
    {"NumpadSlash", VirtualKey::kDivide},
    {"PrintScreen", VirtualKey::kSnapshot},
    {"Pause", VirtualKey::kPause},
    {"CapsLock", VirtualKey::kCapital},
    {"NumLock", VirtualKey::kNumLock},
    {"ScrollLock", VirtualKey::kScroll},
    // Mouse buttons
    {"LMB", VirtualKey::kLButton},
    {"RMB", VirtualKey::kRButton},
    {"MMB", VirtualKey::kMButton},
};

VirtualKey ParseVirtualKey(std::string_view name) {
  auto it = kKeyNames.find(std::string(name));
  return (it != kKeyNames.end()) ? it->second : VirtualKey::kNone;
}

std::string VirtualKeyToString(VirtualKey vk) {
  for (const auto& [name, key] : kKeyNames) {
    if (key == vk) {
      return name;
    }
  }
  return {};
}

const std::vector<std::pair<std::string, uint16_t>>& GamepadButtonNames() {
  static const std::vector<std::pair<std::string, uint16_t>> kNames = {
      {"A", rex::input::X_INPUT_GAMEPAD_A},
      {"B", rex::input::X_INPUT_GAMEPAD_B},
      {"X", rex::input::X_INPUT_GAMEPAD_X},
      {"Y", rex::input::X_INPUT_GAMEPAD_Y},
      {"LB", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"RB", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"LThumb", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
      {"RThumb", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"Start", rex::input::X_INPUT_GAMEPAD_START},
      {"Back", rex::input::X_INPUT_GAMEPAD_BACK},
      {"DPadUp", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
      {"DPadDown", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
      {"DPadLeft", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
      {"DPadRight", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  return kNames;
}

uint16_t ParseGamepadButton(std::string_view name) {
  for (const auto& [button_name, mask] : GamepadButtonNames()) {
    if (button_name == name) {
      return mask;
    }
  }
  return 0;
}

std::string GamepadButtonToString(uint16_t button) {
  for (const auto& [button_name, mask] : GamepadButtonNames()) {
    if (mask == button) {
      return button_name;
    }
  }
  return {};
}

/* ---- Bind registry ---- */

struct BindEntry {
  std::string name;
  std::string description;
  std::string owner;          // mod folder name, or empty for the base app
  std::string requested_key;  // what was asked for, before any reassignment
  std::string current_key;    // effective key; backs the CVAR
  bool conflicted = false;    // requested_key was taken, no free key found
  std::function<void()> callback;
};

static std::mutex g_binds_mutex;
static std::deque<BindEntry> g_binds;

// F-keys not claimed by the base app's own binds (F1 mod manager, F2 shader
// debugger, F3 debug overlay, F4 settings). F7 (achievements) is included:
// NocturneRecomp's achievements_menu.cpp clears "bind_achievements" to "" at
// runtime (that overlay is driven by in-game actions, not a direct keypress),
// so F7's effective key is empty and it never appears "taken" below -- no
// special-casing needed here, just don't skip F7 in the pool. F13-F24 are
// overflow for hosts with extended function-key hardware.
static const char* const kCandidateKeyPool[] = {
    "F5",  "F6",  "F7",  "F8",  "F9",  "F10", "F11", "F12", "F13", "F14",
    "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
};

// True if some other *active* bind's effective key is already `key`. Must be
// called with g_binds_mutex held.
bool IsKeyTakenLocked(std::string_view key, const BindEntry* excluding) {
  if (key.empty()) {
    return false;
  }
  for (auto& entry : g_binds) {
    if (&entry == excluding || !entry.callback) {
      continue;
    }
    if (entry.current_key == key) {
      return true;
    }
  }
  return false;
}

// Finds a key from the candidate pool not currently held by any active bind.
// Returns empty if the whole pool is exhausted. Must be called with
// g_binds_mutex held.
std::string FindFreeKeyLocked() {
  for (const char* candidate : kCandidateKeyPool) {
    if (!IsKeyTakenLocked(candidate, nullptr)) {
      return candidate;
    }
  }
  return {};
}

void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback) {
  std::lock_guard lock(g_binds_mutex);

  /* Store the bind entry (owns the key string that the CVAR references). */
  auto& entry = g_binds.emplace_back();
  entry.name = std::string(name);
  entry.description = std::string(description);
  entry.owner = std::string(rex::system::CurrentActiveMod());
  entry.requested_key = std::string(default_key);
  entry.current_key = std::string(default_key);
  entry.callback = std::move(callback);

  /* Capture a pointer to the entry's key string for the CVAR getter/setter.
     The entry is stable because g_binds is never compacted while binds are
     alive (UnregisterBind sets the callback to null rather than erasing). */
  std::string* key_ptr = &entry.current_key;

  rex::cvar::RegisterFlag({
      .name = std::string(name),
      .type = rex::cvar::FlagType::String,
      .category = "Input/Keybinds/System",
      .description = std::string(description),
      .setter = [key_ptr](std::string_view v) -> bool {
        *key_ptr = std::string(v);
        return true;
      },
      .getter = [key_ptr]() -> std::string { return *key_ptr; },
      .lifecycle = rex::cvar::Lifecycle::kHotReload,
      .default_value = std::string(default_key),
  });

  // If the user already has a persisted/explicit value for this bind's CVAR
  // (applied synchronously by RegisterFlag from its pending-values table),
  // honor it verbatim -- never auto-move a key the user picked themselves.
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  if (info && info->persist_to_config) {
    return;
  }

  // Otherwise, if the requested default collides with another active bind's
  // effective key, move this one to a free key instead of silently shadowing
  // (or being shadowed by) the other bind.
  if (IsKeyTakenLocked(entry.current_key, &entry)) {
    std::string requested = entry.current_key;
    std::string free_key = FindFreeKeyLocked();
    if (!free_key.empty()) {
      entry.current_key = free_key;
      *key_ptr = free_key;
      REXLOG_WARN("Keybind '{}' (mod '{}') wanted '{}', already in use -- moved to '{}'",
                  entry.name, entry.owner.empty() ? "<base>" : entry.owner, requested, free_key);
    } else {
      entry.conflicted = true;
      REXLOG_WARN(
          "Keybind '{}' (mod '{}') wanted '{}', already in use, and no free key was available",
          entry.name, entry.owner.empty() ? "<base>" : entry.owner, requested);
    }
    if (auto* runtime = rex::Runtime::instance()) {
      if (auto* tracker = runtime->mod_conflict_tracker()) {
        tracker->RecordKeybindReassignment(entry.owner, entry.name, requested, entry.current_key);
      }
    }
  }
}

void UnregisterBind(std::string_view name) {
  std::lock_guard lock(g_binds_mutex);
  for (auto& entry : g_binds) {
    if (entry.name == name) {
      entry.callback = nullptr;
      return;
    }
  }
}

bool ProcessKeyEvent(KeyEvent& e) {
  std::lock_guard lock(g_binds_mutex);
  for (auto& entry : g_binds) {
    if (!entry.callback)
      continue;
    VirtualKey vk = ParseVirtualKey(entry.current_key);
    if (vk != VirtualKey::kNone && e.virtual_key() == vk) {
      entry.callback();
      e.set_handled(true);
      return true;
    }
  }
  return false;
}

std::vector<BindView> SnapshotBinds() {
  std::lock_guard lock(g_binds_mutex);
  std::vector<BindView> result;
  result.reserve(g_binds.size());
  for (auto& entry : g_binds) {
    result.push_back(BindView{
        .name = entry.name,
        .description = entry.description,
        .owner = entry.owner,
        .requested_key = entry.requested_key,
        .effective_key = entry.current_key,
        .active = static_cast<bool>(entry.callback),
        .conflicted = entry.conflicted,
    });
  }
  return result;
}

bool SetBindKey(std::string_view name, std::string_view key) {
  // Accept either a keyboard key or a gamepad button name -- gamepad-bound
  // effective keys aren't dispatched by ProcessKeyEvent yet (that's the
  // future fully-gamepad-navigable overlay work), but the rebind UI and this
  // storage format need to represent one today rather than being retrofitted
  // later.
  if (ParseVirtualKey(key) == VirtualKey::kNone && ParseGamepadButton(key) == 0) {
    return false;
  }

  bool found = false;
  {
    std::lock_guard lock(g_binds_mutex);
    for (auto& entry : g_binds) {
      if (entry.name != name) {
        continue;
      }
      entry.current_key = std::string(key);
      entry.conflicted = false;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  // Outside the lock: the CVAR setter (see RegisterBind above) only touches
  // the entry's own current_key string, but SetFlagByName may invoke
  // registered change callbacks that could re-enter the bind registry.
  rex::cvar::SetFlagByName(name, key, /*persist=*/true);
  return true;
}

}  // namespace rex::ui
