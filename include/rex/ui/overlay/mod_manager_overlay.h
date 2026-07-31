/**
 * @file        rex/ui/overlay/mod_manager_overlay.h
 * @brief       Read/write mod manager overlay: enable/disable, reorder,
 *              auto-sort installed mods (mods.toml), and browse/install from
 *              the public mod catalog.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Generic SDK overlay: no game-specific logic. "Installed" tab
 *              is always present and reads/writes <mods_root>/mods.toml
 *              directly (see rex::system::ModState); "All" tab only renders
 *              once rex::system::ModCatalog reaches kReady, and is silently
 *              omitted on any failure/disabled config. Ships on F1.
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/system/mod_catalog.h>  // CatalogMod, ModCatalog
#include <rex/system/mod_plugin.h>   // ModInfo
#include <rex/system/mod_state.h>    // ModStateEntry, ModIssue
#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;
class Window;

class ModManagerDialog : public ImGuiDialog {
 public:
  ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                   rex::Runtime* runtime, Window* window);
  ~ModManagerDialog() override;

  // Sideloads a mod archive dropped onto the game window (see ReXApp::
  // OnFileDrop): installs it via rex::system::ModState::InstallLocalArchive
  // on a background thread, then, on success, focuses this dialog's
  // Installed tab on the newly (re)installed mod. Safe to call while another
  // sideload/install is already in flight -- a second drop while one is
  // running is ignored rather than queued.
  void SideloadArchive(std::filesystem::path zip_path);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Reloads entries_/manifests_/issues_ from disk (mods.toml + each
  // installed mod's mod.toml). Called once lazily and whenever "Refresh from
  // disk" is clicked.
  void ReloadFromDisk();
  // Persists entries_ to mods.toml and re-validates. Called after every
  // mutation (toggle/move/auto-sort/install).
  void PersistAndRevalidate();
  bool StateDiffersFromStartup() const;

  void DrawInstalledTab();
  void DrawCatalogTab();
  void DrawRestartBanner();

  ImmediateTexture* GetLocalIcon(const rex::system::ModInfo& mod);
  // Kicks a background download for `url` if not already cached/in-flight;
  // returns the texture if it's ready yet, else nullptr (rendered as no
  // icon this frame -- next frame picks it up once the download lands).
  ImmediateTexture* GetRemoteIcon(const std::string& url);

  void DrawKeybindsSection(const rex::system::ModInfo& mod);
  void DrawCvarsSection(const rex::system::ModInfo& mod);

  std::string listening_bind_;

  ImmediateDrawer* immediate_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  Window* window_ = nullptr;

  std::filesystem::path mods_root_;
  std::vector<rex::system::ModStateEntry> entries_;
  std::unordered_map<std::string, rex::system::ModInfo> manifests_;
  std::vector<rex::system::ModIssue> issues_;
  bool loaded_ = false;

  rex::system::ModCatalog catalog_;
  bool catalog_refresh_requested_ = false;

  std::unordered_map<std::string, std::unique_ptr<ImmediateTexture>> icon_cache_;

  // Remote icon downloads: URL -> raw bytes once landed. A background
  // std::thread per unique URL (bounded by icon_downloads_ membership, so
  // each URL is only ever fetched once per dialog lifetime).
  std::mutex remote_icon_mutex_;
  std::unordered_map<std::string, std::vector<uint8_t>> remote_icon_bytes_;
  std::unordered_map<std::string, std::thread> icon_downloads_;

  // Sideload (drag-and-drop zip install) state -- see SideloadArchive.
  struct SideloadResult {
    bool in_progress = false;
    bool done = false;
    bool ok = false;
    std::string message;
    // Id to focus in the Installed tab once this result is consumed; empty
    // on failure (nothing to focus).
    std::string focus_id;
  };
  std::atomic<bool> sideload_in_flight_{false};
  std::thread sideload_thread_;
  std::mutex sideload_mutex_;
  SideloadResult sideload_result_;
  // Id of the mod to auto-scroll to and highlight in the Installed tab, on
  // the next draw after a successful sideload. Cleared once applied.
  std::string focus_mod_id_;
};

}  // namespace rex::ui
