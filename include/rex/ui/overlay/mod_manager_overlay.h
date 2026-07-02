/**
 * @file        rex/ui/overlay/mod_manager_overlay.h
 * @brief       Read-only overlay listing enabled mods in load order.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Generic SDK overlay: shows every enabled mod (asset-only and
 *              code alike) with its icon, in the priority order Runtime
 *              resolves enabled_mods into (index 0 = highest priority, wins
 *              conflicting overlays/replacements). Ships on F1. No
 *              game-specific logic.
 */
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <rex/system/mod_plugin.h>  // ModInfo
#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

class ModManagerDialog : public ImGuiDialog {
 public:
  ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                   rex::Runtime* runtime);
  ~ModManagerDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ImmediateTexture* GetIcon(const rex::system::ModInfo& mod);

  ImmediateDrawer* immediate_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ImmediateTexture>> icon_cache_;
};

}  // namespace rex::ui
