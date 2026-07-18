/**
 * @file        rex/ui/keybinds.h
 * @brief       Key binding registry with cvar-backed rebindable keys.
 *
 * Provides a global bind registry where components (like overlay dialogs)
 * can register named keybinds with default keys and callbacks. Binds are
 * backed by string CVARs in the "Keybinds" category, so they appear in
 * the settings overlay and can be saved to config files.
 *
 * @section keybinds_usage Usage
 *
 * @code
 * // Register a bind (typically in a constructor):
 * rex::ui::RegisterBind("bind_console", "Backtick",
 *                       "Toggle console overlay", [this]{ ToggleVisible(); });
 *
 * // Dispatch key events (typically in OnKeyDown):
 * rex::ui::ProcessKeyEvent(e);
 *
 * // Unregister (typically in a destructor):
 * rex::ui::UnregisterBind("bind_console");
 * @endcode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::input {
struct X_INPUT_STATE;
}  // namespace rex::input

namespace rex::ui {

/**
 * Parse a human-readable key name to a VirtualKey enum value.
 * @param name  Key name string (e.g. "F3", "Backtick", "A", "Escape").
 * @return      Matching VirtualKey, or VirtualKey::kNone if unrecognized.
 */
VirtualKey ParseVirtualKey(std::string_view name);

/**
 * Convert a VirtualKey to its human-readable string name.
 * @param vk  VirtualKey to convert.
 * @return    Key name string (e.g. "F3", "LMB"), or empty if unrecognized.
 */
std::string VirtualKeyToString(VirtualKey vk);

/**
 * All known gamepad button names paired with their X_INPUT_GAMEPAD_BUTTON
 * bitmask value, e.g. {"A", 0x1000}. Shared table so any UI that lets a user
 * rebind a gamepad button (settings overlay, per-feature binds) names buttons
 * consistently.
 */
const std::vector<std::pair<std::string, uint16_t>>& GamepadButtonNames();

/**
 * Parse a human-readable gamepad button name (e.g. "A", "LThumb", "DPadUp")
 * to its X_INPUT_GAMEPAD_BUTTON bitmask.
 * @return  Matching bitmask, or 0 if unrecognized.
 */
uint16_t ParseGamepadButton(std::string_view name);

/**
 * Convert an X_INPUT_GAMEPAD_BUTTON bitmask (single bit) to its
 * human-readable name.
 * @return  Button name string, or empty if unrecognized.
 */
std::string GamepadButtonToString(uint16_t button);

/**
 * Register a named keybind with a default key and callback.
 *
 * Creates a string CVAR named @p name in the "Keybinds" category so the
 * binding is visible in the settings overlay and persisted to config.
 *
 * If @p default_key is already claimed by another active bind and the
 * caller has no persisted/user-set value for @p name, the bind is
 * auto-reassigned to the next free key from a small candidate pool (mainly
 * intended for the F5-F12 range mods conventionally use) instead of
 * silently shadowing the earlier bind. The reassignment (or, if the pool is
 * exhausted, the unresolved conflict) is recorded on
 * Runtime::mod_conflict_tracker() when called from within a mod's
 * IModPlugin lifecycle call (see mod_attribution.h), and logged.
 *
 * @param name         CVAR name for this bind (e.g. "bind_console").
 * @param default_key  Default key name (e.g. "Backtick", "F3").
 * @param description  Human-readable description for the settings UI.
 * @param callback     Function to invoke when the bound key is pressed.
 * @param is_visible   Optional: returns whether the thing this bind toggles
 *                     (typically an overlay) is currently shown. Powers the
 *                     overlay menu (see overlay_menu.h) and the mod manager's
 *                     per-mod keybind list; omit if the bind doesn't toggle
 *                     visible state, or the state isn't cheaply queryable.
 * @param window_title Optional: the ImGui window title (the string passed to
 *                     ImGui::Begin, including any "##id" suffix) this bind's
 *                     overlay draws into. Lets gamepad UI navigation (see
 *                     gamepad_ui.h) focus/move/resize/close the right window
 *                     when this bind's overlay becomes the "active" one; omit
 *                     if the bind doesn't toggle a single-window overlay.
 */
void RegisterBind(std::string_view name, std::string_view default_key, std::string_view description,
                  std::function<void()> callback, std::function<bool()> is_visible = {},
                  std::string_view window_title = {});

/**
 * Remove a previously registered keybind.
 * @param name  The CVAR name used when registering the bind.
 */
void UnregisterBind(std::string_view name);

/**
 * Describes one registered bind for display/editing (e.g. by the mod manager
 * overlay), without exposing the internal registry directly.
 */
struct BindView {
  std::string name;  // CVAR name, e.g. "bind_sample_overlay"
  std::string description;
  std::string owner;                  // mod folder name that registered this bind, or
                                      // empty if registered by the base app
  std::string requested_key;          // the key the bind originally asked for
  std::string effective_key;          // the key actually in effect (== requested_key
                                      // unless auto-reassigned)
  bool active = false;                // false if UnregisterBind was called
  bool conflicted = false;            // true if requested_key was taken and no free
                                      // key was available (effective_key left as
                                      // requested_key anyway)
  bool has_visibility_state = false;  // true if an is_visible getter was
                                      // supplied to RegisterBind
  bool visible = false;               // only meaningful if has_visibility_state
  std::string window_title;           // the ImGui window title supplied via
                                      // RegisterBind's window_title param, or
                                      // empty if none was given
};

/**
 * Snapshot of every currently-registered bind, in registration order.
 * Safe to call from UI code; does not touch the internal bind registry.
 */
std::vector<BindView> SnapshotBinds();

/**
 * Sets a bind's effective key, validating @p key as either a keyboard key
 * (ParseVirtualKey, dispatched by ProcessKeyEvent) or a gamepad button name
 * (ParseGamepadButton, dispatched by PollGamepadBinds). Persists the change
 * via the bind's backing CVAR
 * (rex::cvar::SetFlagByName(name, key, /*persist=*\/true)) so it survives
 * across restarts and is treated as an explicit user choice, not subject to
 * future auto-reassignment. Clears any recorded conflict for this bind.
 *
 * @return false if @p name is not a registered bind or @p key is not a
 *         recognized key/button name.
 */
bool SetBindKey(std::string_view name, std::string_view key);

/**
 * Invokes a registered bind's callback directly, bypassing key/gamepad
 * dispatch -- used by UI that lets a user select a bind by name (e.g. the
 * overlay menu, see overlay_menu.h) rather than by pressing its key.
 *
 * @return false if @p name is not a registered, active bind.
 */
bool InvokeBind(std::string_view name);

/**
 * Process a key-down event against all registered binds.
 *
 * Looks up each bind's current key from its CVAR, parses it, and compares
 * against the event's virtual key. If a match is found, the bind's callback
 * is invoked and the event is marked as handled.
 *
 * @param e  The key event to process.
 * @return   True if a bind matched and the event was consumed.
 */
bool ProcessKeyEvent(KeyEvent& e);

/**
 * Edge-triggers any active bind whose effective key is a gamepad button name
 * (ParseGamepadButton(effective_key) != 0) against the given controller
 * state, firing its callback once per press (not held). XInput has no
 * button-event callback, so this must be polled once per UI frame with the
 * latest state (mirrors the pattern NocturneRecomp's fast_forward.cpp/
 * achievements_menu.cpp already use for controller-held buttons) -- see
 * overlay_menu.h's dialog for where the SDK does this polling.
 *
 * @param state  Latest controller state (typically player 0).
 */
void PollGamepadBinds(const rex::input::X_INPUT_STATE& state);

}  // namespace rex::ui
