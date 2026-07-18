/**
 * @file        ui/overlay/overlay_menu.cpp
 * @brief       Overlay menu implementation. See overlay_menu.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/overlay_menu.h>

#include <algorithm>
#include <vector>

#include <imgui.h>

#include <rex/ui/keybinds.h>

namespace rex::ui {

namespace {
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};
constexpr ImVec4 kShownText{0.55f, 0.85f, 0.55f, 1.00f};

// This dialog's own two binds (see the constructor) toggle *this* menu, not
// an entry worth listing inside itself -- selecting "Toggle overlay menu"
// while the menu is open would just close the window you clicked in.
bool IsOwnBind(std::string_view name) {
  return name == "bind_overlay_menu" || name == "bind_overlay_menu_key";
}
}  // namespace

// This dialog's own window title, shared by the RegisterBind call below and
// the ImGui::Begin call in OnDraw so the gamepad UI controller (see
// gamepad_ui.h) can focus/move/resize/close this window like any other
// overlay's.
constexpr const char* kWindowTitle = "Overlays##overlay_menu";

OverlayMenuDialog::OverlayMenuDialog(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime)
    : ImGuiDialog(imgui_drawer), runtime_(runtime) {
  // Default trigger is Y. Gated to UI mode via is_ui_mode_ (set later by
  // SetUiModeQuery): RThumb's stick is now used by the gamepad UI controller
  // (gamepad_ui.h) to move the active overlay around the screen while in UI
  // mode, so Y can no longer double as a plain button press without also
  // considering that mode -- firing during Gameplay would pop this menu open
  // while the guest still has the pad and nothing suppresses its own use of
  // Y, which is exactly the conflict UI mode exists to avoid (X already
  // cycles the active overlay and B already closes it, both scoped to UI
  // mode the same way -- see gamepad_ui.h's PollUiNavigation, which simply
  // never runs outside UI mode).
  rex::ui::RegisterBind(
      "bind_overlay_menu", "Y", "Toggle overlay menu",
      [this] {
        if (is_ui_mode_ && !is_ui_mode_())
          return;
        visible_ = !visible_;
      },
      [this] { return visible_; }, kWindowTitle);
  // Keyboard fallback: gamepad-only triggers are a hard dead end for anyone
  // without a controller plugged in (or hitting a driver quirk), and the
  // bind system only lets one bind own one key/button, so this needs its own
  // separate bind rather than trying to attach two triggers to
  // "bind_overlay_menu". "Insert" isn't claimed by the base app (F1-F4/F7),
  // the mods' F5-F12 convention, or the game's own keybind_* guest-control
  // mappings (Space/RMB/LMB/Shift/M/R/Q/Control/E/Escape/W/A/S/D, see
  // NocturneRecomp's build.py default config).
  rex::ui::RegisterBind(
      "bind_overlay_menu_key", "Insert", "Toggle overlay menu (keyboard)",
      [this] { visible_ = !visible_; }, [this] { return visible_; }, kWindowTitle);
}

OverlayMenuDialog::~OverlayMenuDialog() {
  rex::ui::UnregisterBind("bind_overlay_menu_key");
  rex::ui::UnregisterBind("bind_overlay_menu");
}

void OverlayMenuDialog::OnDraw(ImGuiIO& io) {
  (void)io;
  // The per-frame PollGamepadBinds poll this dialog used to own now lives on
  // rex::ui::GamepadUiController (gamepad_ui.h/.cpp), which also gates it to
  // Gameplay mode only -- in UI mode the controller consumes the gamepad
  // itself for navigation, so firing gamepad-bound overlay toggles off the
  // same buttons would fight it.
  if (!visible_) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(kWindowTitle, &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  auto binds = rex::ui::SnapshotBinds();
  // Group by owner, preserving first-seen (== load/registration) order.
  std::vector<std::string> owners_order;
  for (const auto& bind : binds) {
    if (!bind.has_visibility_state || IsOwnBind(bind.name)) {
      continue;
    }
    if (std::find(owners_order.begin(), owners_order.end(), bind.owner) == owners_order.end()) {
      owners_order.push_back(bind.owner);
    }
  }

  bool any = false;
  for (const auto& owner : owners_order) {
    ImGui::TextColored(kMutedText, "%s", owner.empty() ? "Base app" : owner.c_str());
    for (const auto& bind : binds) {
      if (!bind.has_visibility_state || bind.owner != owner || IsOwnBind(bind.name)) {
        continue;
      }
      any = true;
      ImGui::PushID(bind.name.c_str());
      std::string label = (bind.description.empty() ? bind.name : bind.description) +
                          (bind.visible ? "  [shown]" : "  [hidden]");
      if (bind.visible) {
        ImGui::PushStyleColor(ImGuiCol_Text, kShownText);
      }
      bool selected = ImGui::Selectable(label.c_str());
      if (bind.visible) {
        ImGui::PopStyleColor();
      }
      if (selected) {
        bool was_visible = bind.visible;
        rex::ui::InvokeBind(bind.name);
        if (!was_visible && on_overlay_shown_) {
          on_overlay_shown_(bind.name);
        }
      }
      ImGui::PopID();
    }
  }
  if (!any) {
    ImGui::TextDisabled(
        "No overlays expose visibility state yet (RegisterBind's is_visible parameter).");
  }

  ImGui::End();
}

}  // namespace rex::ui
