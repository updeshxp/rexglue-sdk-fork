/**
 * @file        rex/ui/overlay/overlay_menu.h
 * @brief       Gamepad-triggered menu listing every registered overlay
 *              (base app and mod) with its shown/hidden state.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     First real step of the SDK's overlays becoming fully
 *              gamepad-navigable: it is itself opened by a gamepad button
 *              (default Y, rebindable like any other bind -- see keybinds.h)
 *              or the Insert key, lists every rex::ui::SnapshotBinds() entry
 *              that exposes visibility state, and selecting a row toggles
 *              that overlay via its own bind callback. Since it's built on
 *              the regular bind registry, both vanilla overlays (debug/
 *              console/settings/mod manager/achievements/shader debugger,
 *              once they opt into passing an is_visible getter to
 *              RegisterBind) and mod overlays that do the same show up with
 *              no separate registration mechanism to keep in sync.
 *
 *              The per-frame gamepad poll (rex::ui::PollGamepadBinds) that
 *              makes gamepad-keyed binds fire at all now lives on
 *              rex::ui::GamepadUiController (see gamepad_ui.h), which gates
 *              it to Gameplay mode -- this dialog no longer polls itself.
 *              The Y trigger is reserved for UI mode though (X already
 *              cycles the active overlay and B already closes it there, see
 *              gamepad_ui.h), so this dialog's own Y-bound callback checks
 *              GamepadUiController::IsUiMode() via SetUiModeQuery and no-ops
 *              outside UI mode rather than also firing during gameplay.
 */
#pragma once

#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class OverlayMenuDialog : public ImGuiDialog {
 public:
  OverlayMenuDialog(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime);
  ~OverlayMenuDialog() override;

  bool IsVisible() const { return visible_; }

  // Supplies the check this dialog's gamepad ("Y") bind uses to gate itself
  // to UI mode only -- see GamepadUiController::IsUiMode() in gamepad_ui.h.
  // Called from ReXApp once gamepad_ui_ exists, which is after this dialog
  // is constructed (same ordering constraint as GamepadUiController's own
  // SetInputSystem). The Insert keyboard fallback is not gated: it has no
  // gamepad-mode concept to conflict with.
  void SetUiModeQuery(std::function<bool()> is_ui_mode) { is_ui_mode_ = std::move(is_ui_mode); }

  // Supplies the callback invoked with a bind's name whenever selecting it
  // in this menu causes its overlay to go from hidden to shown -- lets
  // GamepadUiController::FocusOverlay (gamepad_ui.h) give the newly-opened
  // overlay focus right away instead of leaving the overlay menu itself
  // active. Wired from ReXApp after gamepad_ui_ exists, same ordering
  // constraint as SetUiModeQuery above.
  void SetOverlayShownCallback(std::function<void(const std::string&)> on_shown) {
    on_overlay_shown_ = std::move(on_shown);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;
  std::function<bool()> is_ui_mode_;
  std::function<void(const std::string&)> on_overlay_shown_;
};

}  // namespace rex::ui
