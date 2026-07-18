/**
 * @file        rex/ui/overlay/gamepad_ui.h
 * @brief       Two-mode gamepad controller: Gameplay (pad drives the guest)
 *              vs UI (pad drives the overlays), toggled by the guide button.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Builds on the overlay menu / bind registry added by
 *              overlay_menu.h: this controller is what actually makes the
 *              gamepad usable to navigate overlays rather than just toggle
 *              them.
 *
 *              - Gameplay (default at boot): the pad drives the game exactly
 *                as before. This controller only edge-detects the guide
 *                button (and the bind_ui_mode keyboard fallback) and polls
 *                rex::ui::PollGamepadBinds so gamepad-keyed overlay-toggle
 *                binds still fire.
 *              - UI: the pad is fully claimed by this controller. Guest input
 *                reads are neutralized (InputSystem::SetGuestInputSuppressed)
 *                so the game can't react underneath the overlays, and
 *                PollGamepadBinds is *not* polled (it would otherwise fire on
 *                the same buttons this controller consumes for navigation).
 *                One overlay is "active": left stick/D-pad and A drive ImGui's
 *                built-in gamepad nav (ImGuiConfigFlags_NavEnableGamepad)
 *                inside it, B closes it, Y opens/activates the overlay menu,
 *                X cycles the active overlay among all currently-shown ones,
 *                right stick moves the active overlay's window, and
 *                left-trigger + right stick resizes it.
 *
 *              A short on-screen flash announces every mode change.
 */
#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace rex::ui {

class GamepadUiController : public ImGuiDialog {
 public:
  // SetupOverlays constructs this dialog before Runtime's input system is
  // guaranteed fully wired -- same timing constraint OverlayMenuDialog has.
  // SetInputSystem is called later, from ReXApp::ConstructRuntime, once
  // window()/app_context()/the input system are all live (see rex_app.cpp).
  GamepadUiController(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime);
  ~GamepadUiController() override;

  bool IsUiMode() const { return mode_ == Mode::kUi; }

  // Supplies (or replaces) the InputSystem this controller reads the pad
  // from and suppresses guest input through. Safe to call more than once.
  void SetInputSystem(rex::input::InputSystem* input_system) { input_system_ = input_system; }

  // Makes the given bind's overlay the "active" one for gamepad navigation
  // (focuses its window; B/X's active-overlay handling then act on it) --
  // used by the overlay menu (see overlay_menu.h) so an overlay it just
  // opened grabs focus immediately instead of leaving the overlay menu
  // itself active underneath it. No-op outside UI mode, where the notion of
  // an "active" overlay doesn't apply.
  void FocusOverlay(const std::string& bind_name) {
    if (mode_ == Mode::kUi) {
      SetActiveBind(bind_name);
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Mode { kGameplay, kUi };

  void ToggleMode();
  void EnterUiMode();
  void EnterGameplayMode();
  // Picks the overlay to focus on entering UI mode: the first already-shown
  // overlay, or the overlay menu itself (opened if necessary) if none are.
  void ActivateInitialOverlay();
  void SetActiveBind(const std::string& bind_name);
  void PollUiNavigation(ImGuiIO& io);
  void DrawModeFlash(ImGuiIO& io);

  rex::Runtime* runtime_ = nullptr;
  rex::input::InputSystem* input_system_ = nullptr;
  Mode mode_ = Mode::kGameplay;
  std::string active_bind_name_;
  std::string active_window_title_;

  // bind_ui_mode's callback runs from inside rex::ui::ProcessKeyEvent, which
  // holds the bind registry's mutex for the duration of the callback.
  // ToggleMode()/EnterUiMode() call back into SnapshotBinds()/InvokeBind(),
  // which would try to re-lock that same (non-recursive) mutex and deadlock.
  // So the keyboard path only sets this flag; OnDraw (running outside any
  // bind-registry lock) does the actual mode switch.
  bool pending_keyboard_toggle_ = false;

  // Edge-detection state for buttons this controller handles directly
  // (outside of ImGui's own nav, which tracks its own edges internally).
  bool guide_was_down_ = false;
  bool a_was_down_ = false;
  bool b_was_down_ = false;
  bool x_was_down_ = false;
  bool y_was_down_ = false;

  // Mode-change flash timer; unset when no flash is showing.
  bool flash_active_ = false;
  std::string flash_text_;
  double flash_started_seconds_ = 0.0;
};

}  // namespace rex::ui
