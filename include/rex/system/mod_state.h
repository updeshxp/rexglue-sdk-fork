/**
 * @file        system/mod_state.h
 * @brief       mods.toml sidecar: enable/disable/order state for installed
 *              mods, plus dependency validation as structured data.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Ports the launcher-side `mods.rs` sidecar logic (Goopie
 *              Launcher) into the SDK so the in-game mod manager overlay can
 *              read/write the same file directly, rather than only ever
 *              seeing what a launcher passed via --enabled_mods. Game/
 *              project-agnostic: operates purely on folder ids and
 *              rex::system::ModInfo.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/mod_plugin.h>  // ModInfo

namespace rex::system {

// One entry in the mods.toml sidecar. Order in the containing vector *is*
// load-priority order (index 0 = highest priority); disabled entries keep
// their slot so re-enabling a mod restores its position instead of dropping
// it to the bottom.
struct ModStateEntry {
  std::string id;
  bool enabled = true;

  bool operator==(const ModStateEntry&) const = default;
};

// One problem found by ModState::Validate: "error" blocks a safe launch
// (mirrors Runtime::ValidateModDependencies' hard-fail cases), "warning" is
// purely informational.
struct ModIssue {
  std::string id;
  enum class Kind { kError, kWarning } kind = Kind::kError;
  std::string message;
};

// Outcome of a successful ModState::InstallLocalArchive call.
struct ModInstallResult {
  std::string id;
  std::string version;
  // true if this replaced an already-installed mod of the same id.
  bool updated = false;
};

class ModState {
 public:
  // The same mods-root fallback Runtime::ResolveEnabledMods() uses: the
  // mods_data_root cvar if set, else <exe folder>/mods.
  static std::filesystem::path ResolveModsRoot();

  // Parses <root>/mods.toml ([[mods]] array of {id, enabled}, enabled
  // defaulting to true). Missing or malformed file returns an empty list and
  // only warns -- never throws, since a bare command-line launch (no
  // launcher-managed sidecar yet) is a normal, expected state.
  static std::vector<ModStateEntry> Load(const std::filesystem::path& root);

  // Writes <root>/mods.toml atomically (temp file + rename). Creates `root`
  // if missing. Logs and returns false on failure rather than throwing.
  static bool Save(const std::filesystem::path& root, const std::vector<ModStateEntry>& entries);

  // Enumerates every immediate subdirectory of `root` (installed mod ids),
  // sorted for determinism.
  static std::vector<std::string> InstalledIds(const std::filesystem::path& root);

  // Reconciles `entries` (as loaded from mods.toml) against `on_disk_ids`:
  // keeps recorded entries whose folder still exists (preserving slot and
  // enabled flag), drops entries whose folder is gone, and appends any
  // undiscovered folder at the end as enabled.
  static std::vector<ModStateEntry> Reconcile(const std::vector<ModStateEntry>& entries,
                                              const std::vector<std::string>& on_disk_ids);

  // Convenience: Load() + InstalledIds() + Reconcile() in one call, as used
  // by both Runtime::ResolveEnabledMods() and the overlay's "Installed" tab.
  static std::vector<ModStateEntry> LoadReconciled(const std::filesystem::path& root);

  // Enabled ids, in priority order (disabled entries omitted).
  static std::vector<std::string> EnabledIdsInOrder(const std::vector<ModStateEntry>& entries);

  // Stable Kahn topological sort of the *enabled* subset of `entries` over
  // `requires`/`load_after` edges (both treated as "must load before"), by
  // id -> ModInfo (only enabled ids need be present). Disabled entries are
  // left pinned to their original absolute slot. A cycle (only possible via
  // load_after) is broken by falling back to original relative order for the
  // node(s) involved, so this always terminates and never reorders based on
  // guesswork beyond that.
  static std::vector<ModStateEntry> AutoSort(
      const std::vector<ModStateEntry>& entries,
      const std::unordered_map<std::string, ModInfo>& manifests);

  // Validates the enabled subset of `entries` against `manifests` (id ->
  // ModInfo), following the same rules as Runtime::ValidateModDependencies()
  // plus a platform-binary check for code mods, returned as structured data
  // instead of only logged. `host_version` is compared against each mod's
  // min_game_version (empty = "unknown", same can't-verify-so-warn
  // semantics as an empty RuntimeConfig::game_version). `host_platform` is
  // this host's platform id (e.g. "windows-x64", "linux-x64",
  // "linux-arm64").
  static std::vector<ModIssue> Validate(const std::vector<ModStateEntry>& entries,
                                        const std::unordered_map<std::string, ModInfo>& manifests,
                                        std::string_view host_version,
                                        std::string_view host_platform);

  // This process's platform id in the "platform" mod.toml key convention
  // (e.g. "windows-x64", "linux-x64", "linux-arm64").
  static std::string HostPlatformId();

  // Sideloads a local mod archive (.zip) dropped onto the game window:
  // extracts it into `root`, mirroring the same top-level-directory
  // convention as a catalog install (a single top-level directory becomes the
  // mod and lends its name to the id; otherwise the archive's own content is
  // used flat and named after the zip's file stem). Unlike a catalog install
  // (which trusts the catalog's modId), this is the only signal available to
  // tell an actual mod archive apart from an arbitrary zip a player might
  // drop, so the resolved content root MUST contain a mod.toml or the
  // install is refused. If a mod of the same id is already installed, this
  // replaces it only when the archive's mod.toml `version` is >= the
  // installed one's (CompareVersionStrings), same not-older-clobbers-newer
  // rule as a catalog update; otherwise it's refused. Appends the new id to
  // mods.toml (enabled, at the end) or, when replacing, keeps the existing
  // entry's slot/enabled flag untouched, then saves. Never throws; returns
  // std::nullopt and fills `error` on any failure (not a zip, corrupt
  // archive, no mod.toml, older version, filesystem error).
  static std::optional<ModInstallResult> InstallLocalArchive(const std::filesystem::path& root,
                                                             const std::filesystem::path& zip_path,
                                                             std::string& error);
};

}  // namespace rex::system
