/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <rex/ui/imgui_dialog.h>

namespace rex::input {
class InputSystem;
}

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  // input_system: optional; when set, the "Rebind" capture also polls
  // controller button state so gamepad keybinds can be rebound from the UI.
  // window_title: ImGui window title/ID passed to ImGui::Begin. Defaults to
  // the historical "Settings##rex" used by the F4 bind; pass a different
  // ID here when embedding this dialog alongside another ImGui window that
  // also happens to be titled "Settings##rex" (e.g. a game's own curated
  // settings overlay) -- same title means same ImGui window ID, which
  // merges both dialogs' draws into one squeezed window instead of two.
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path,
                 rex::input::InputSystem* input_system = nullptr,
                 std::string window_title = "Settings##rex");
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  rex::input::InputSystem* input_system_ = nullptr;
  std::string window_title_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
};

}  // namespace rex::ui
