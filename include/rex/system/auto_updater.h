/**
 * @file        system/auto_updater.h
 * @brief       Self-update client: checks a downstream project's GitHub
 *              Releases repo (RuntimeConfig::update_repo) for a newer
 *              version, downloads + sha256-verifies + extracts it, and
 *              stages it to replace the current install on the next
 *              restart.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Mirrors rex::system::ModCatalog's fetch/install-worker shape
 *              for the check/download/verify/extract half. Applying the
 *              staged content is *not* ModState's "swap in-process on the
 *              next launch" pattern, though: unlike a mod DLL, the install
 *              root contains this very process's own running executable and
 *              its directly-linked DLLs, which stay locked by the OS loader
 *              for the process's entire lifetime -- including a relaunched
 *              instance, since it's executing from that same locked file the
 *              moment it would try to replace it. ApplyAndRestart() instead
 *              hands the swap to a small detached helper script that outlives
 *              this process, waits for it to actually exit, then does the
 *              swap and relaunches -- see auto_updater_win.cpp/
 *              auto_updater_posix.cpp.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rex::system {

enum class UpdateCheckState { kIdle, kChecking, kUpToDate, kUpdateAvailable, kFailed };

struct UpdateInfo {
  std::string tag;         // GitHub release tag_name, e.g. "v1.4.0".
  std::string version;     // `tag` with a leading 'v'/'V' stripped, e.g. "1.4.0".
  std::string asset_name;  // Matched release asset's file name.
  std::string asset_url;   // Matched release asset's browser_download_url.
  std::string sha256;      // Lowercase hex, from the asset's GitHub-computed digest.
  std::string notes;       // Release body (changelog), for display.
};

struct UpdateInstallResult {
  bool in_progress = false;
  bool done = false;
  bool ok = false;
  std::string message;
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
};

class AutoUpdater {
 public:
  ~AutoUpdater();

  // Kicks off a background check against RuntimeConfig::update_repo's
  // latest GitHub release; a no-op (state left as-is) if one is already in
  // flight. Settles straight to kFailed, no request attempted, if
  // auto_update_enabled is false, update_repo/update_asset_format are
  // unset, or Runtime::game_version() is empty (nothing to compare against).
  void CheckAsync();

  UpdateCheckState state() const { return state_.load(std::memory_order_acquire); }

  // The matched, newer-than-current release, once state() is
  // kUpdateAvailable; std::nullopt otherwise.
  std::optional<UpdateInfo> Available() const;

  // Downloads `info`'s asset, verifies its sha256, extracts it, and stages
  // it under StagingRoot(install_root) for ApplyAndRestart() to pick up. A
  // no-op if an install is already in flight.
  void InstallAsync(const UpdateInfo& info, const std::filesystem::path& install_root);

  UpdateInstallResult InstallSnapshot() const;

  // Where a staged self-update's extracted content lives, as immediate
  // children mirroring the install root's own top-level layout (e.g.
  // <root>/.pending-self-update/nocturnerecomp.exe, .../rexruntime.dll, ...).
  static std::filesystem::path StagingRoot(const std::filesystem::path& install_root);

  // True if a self-update is staged and waiting for ApplyAndRestart().
  static bool HasPendingSelfUpdate(const std::filesystem::path& install_root);

  // Spawns a detached helper (a temp script on both platforms -- see
  // auto_updater_win.cpp/auto_updater_posix.cpp) that waits for this
  // process to exit, moves every top-level entry out of
  // StagingRoot(install_root) onto its matching entry directly under
  // `install_root` (overwriting whatever was there -- an installed mod's own
  // `mods/` folder, save data, etc. are never touched, since they simply
  // aren't present in the staged content), relaunches `executable_path`, and
  // deletes itself. A no-op (returns false, nothing spawned) if no update is
  // staged. Returns true if the helper was spawned successfully, in which
  // case the caller must quit the app right after (e.g.
  // Window::RequestClose()) -- this process is expected to have exited by
  // the time the helper's wait loop moves on.
  static bool ApplyAndRestart(const std::filesystem::path& install_root,
                              const std::filesystem::path& executable_path);

  // Builds the expected release asset's base name (no extension) for `tag`
  // from `format`, substituting the "{tag}" and "{platform}" placeholders
  // (platform via ModState::HostPlatformId()). See
  // RuntimeConfig::update_asset_format for the placeholder contract.
  static std::string ExpandAssetFormat(const std::string& format, const std::string& tag);

 private:
  void CheckWorker(std::string repo, std::string format, std::string current_version);
  void InstallWorker(UpdateInfo info, std::filesystem::path install_root);

  std::atomic<bool> check_in_flight_{false};
  std::thread check_thread_;
  std::atomic<UpdateCheckState> state_{UpdateCheckState::kIdle};
  mutable std::mutex info_mutex_;
  std::optional<UpdateInfo> available_;

  std::atomic<bool> install_in_flight_{false};
  std::thread install_thread_;
  mutable std::mutex install_mutex_;
  UpdateInstallResult install_result_;
};

}  // namespace rex::system
