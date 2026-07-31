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
#include <unordered_set>
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
  // true if the new files were staged (see StagePendingUpdate) rather than
  // written straight to `id`'s live folder, because `id` was already
  // installed and possibly in use (a loaded mod DLL) this session. Staged
  // updates take effect on the next launch (ApplyPendingUpdates), not
  // immediately.
  bool staged = false;
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

  // Deletes <root>/<id> entirely (if it exists) and drops its entry from
  // mods.toml, saving the result. Rejects `id` outright (returns false,
  // touches nothing) if it's empty or contains a path separator/"."/".." --
  // ids only ever come from real, already-reconciled folder names, so this
  // is a defensive check rather than something normal use should ever hit.
  // Deletes immediately -- prefer MarkPendingRemoval for anything driven by
  // the in-game mod manager overlay, where the mod may still be loaded this
  // session (a mod DLL Windows won't let go of) and the player should get a
  // chance to change their mind before a restart makes it permanent. If the
  // folder can't be deleted right now, the mods.toml entry is still dropped
  // immediately and the actual folder deletion is deferred to
  // ApplyPendingRemovals at the next launch (same marker PendingRemovals()
  // reports); still returns true in that case -- from the caller's
  // perspective the mod is gone.
  static bool RemoveMod(const std::filesystem::path& root, const std::string& id);

  // Marks `id` for removal without touching its mods.toml entry or its
  // folder: adds an empty marker file under PendingRemovalsRoot(). The mod
  // keeps loading/showing normally (see PendingRemovals()) until either
  // UnmarkPendingRemoval() undoes this, or the next launch's
  // ApplyPendingRemovals() actually deletes the folder and drops the entry.
  // This is what the mod manager overlay's "Remove" button calls -- it's
  // never blocked by a currently-loaded mod DLL the way an immediate
  // RemoveMod() can be, since nothing is deleted until the process (and
  // whatever it has loaded) has actually exited. Rejects `id` the same way
  // RemoveMod does.
  static bool MarkPendingRemoval(const std::filesystem::path& root, const std::string& id);

  // Undoes a MarkPendingRemoval() ("Restore"): removes `id`'s marker, if any.
  // A no-op (returns true) if `id` wasn't marked. Only meaningful before the
  // next launch's ApplyPendingRemovals() actually runs -- once the folder is
  // gone, there's nothing left to restore.
  static bool UnmarkPendingRemoval(const std::filesystem::path& root, const std::string& id);

  // Every id currently marked for removal (see MarkPendingRemoval), for the
  // mod manager overlay to check per-row without a filesystem call per mod
  // per frame.
  static std::unordered_set<std::string> PendingRemovals(const std::filesystem::path& root);

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

  // Where staged (not-yet-applied) mod updates live, as subfolders named by
  // mod id: <root>/.pending-updates/<id>.
  static std::filesystem::path PendingUpdatesRoot(const std::filesystem::path& root);

  // True if at least one update is staged and waiting for a restart to apply.
  static bool HasPendingUpdates(const std::filesystem::path& root);

  // Moves `content_root` (a freshly extracted mod's content, already owned by
  // the caller -- e.g. a catalog install's unwrapped staging dir) into the
  // pending-updates area under `id`, replacing anything already staged for
  // that id. Used instead of overwriting `root / id` directly when `id` is
  // already installed: on Windows, a mod that's currently loaded (DLL mapped,
  // files open) can't have its files replaced in place, so the new content is
  // written here instead and swapped in at the next launch by
  // ApplyPendingUpdates, before anything has a chance to open it. Never
  // throws; returns false and fills `error` on any filesystem failure.
  static bool StagePendingUpdate(const std::filesystem::path& root, const std::string& id,
                                 const std::filesystem::path& content_root, std::string& error);

  // Moves every staged update (see StagePendingUpdate) onto its real
  // `root / id` folder. Must run before mods are enumerated/loaded (see
  // Runtime::ResolveEnabledMods) -- that's the only point in the process
  // lifetime where nothing yet has the old files open. Best-effort per entry:
  // a failure is logged and left staged for the next launch's attempt rather
  // than aborting the rest. Never throws.
  static void ApplyPendingUpdates(const std::filesystem::path& root);

  // Where MarkPendingRemoval() (and a RemoveMod() that couldn't delete its
  // folder immediately) records a marked id, as an empty marker file:
  // <root>/.pending-removals/<id>.
  static std::filesystem::path PendingRemovalsRoot(const std::filesystem::path& root);

  // True if at least one mod is marked for removal, waiting for a restart to
  // actually delete its folder and drop its mods.toml entry.
  static bool HasPendingRemovals(const std::filesystem::path& root);

  // Deletes every folder marked for removal (see MarkPendingRemoval) and
  // drops its mods.toml entry, then clears its marker. Must run before mods
  // are enumerated/loaded (see Runtime::ResolveEnabledMods) -- same reasoning
  // as ApplyPendingUpdates. Best-effort per entry: a failure is logged and
  // left pending for the next launch's attempt rather than aborting the
  // rest. Never throws.
  static void ApplyPendingRemovals(const std::filesystem::path& root);
};

}  // namespace rex::system
