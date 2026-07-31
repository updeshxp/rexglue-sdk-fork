/**
 * @file        system/mod_state.cpp
 * @brief       mods.toml sidecar implementation. See mod_state.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_state.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_set>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform/process.h>
#include <rex/runtime.h>  // REXCVAR_DECLARE(mods_data_root)
#include <rex/system/mod_version.h>

namespace rex::system {

namespace {
constexpr const char* kSidecarName = "mods.toml";

// Sanitizes a candidate mod id the same way the companion desktop launcher's
// build tags are: alnum/-/_/. pass through, everything else collapses to
// '_'; an empty result falls back to "mod" so a pathological zip name never
// produces an unusable/empty folder name.
std::string SanitizeModId(const std::string& candidate) {
  std::string sanitized;
  sanitized.reserve(candidate.size());
  for (char c : candidate) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
      sanitized += c;
    } else {
      sanitized += '_';
    }
  }
  // Trim any leading/trailing whitespace-turned-underscores from a name that
  // started/ended with spaces.
  while (!sanitized.empty() && sanitized.front() == '_') {
    sanitized.erase(sanitized.begin());
  }
  while (!sanitized.empty() && sanitized.back() == '_') {
    sanitized.pop_back();
  }
  return sanitized.empty() ? "mod" : sanitized;
}

// Best-effort read of a mod.toml's `version` key; empty on any failure
// (missing file, parse error, missing key) -- mirrors ParseModInfo's own
// tolerance in runtime.cpp.
std::string ReadModVersion(const std::filesystem::path& manifest_path) {
  if (!std::filesystem::is_regular_file(manifest_path)) {
    return {};
  }
  try {
    auto table = toml::parse_file(manifest_path.string());
    return table["version"].value_or<std::string>("");
  } catch (const toml::parse_error&) {
    return {};
  }
}

bool HasZipExtension(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".zip";
}

}  // namespace

std::filesystem::path ModState::ResolveModsRoot() {
  std::string mods_root_cvar = REXCVAR_GET(mods_data_root);
  if (mods_root_cvar.empty()) {
    return rex::filesystem::GetExecutableFolder() / "mods";
  }
  return std::filesystem::absolute(std::filesystem::path(mods_root_cvar));
}

std::vector<ModStateEntry> ModState::Load(const std::filesystem::path& root) {
  std::vector<ModStateEntry> entries;
  auto path = root / kSidecarName;
  if (!std::filesystem::is_regular_file(path)) {
    return entries;
  }
  try {
    auto table = toml::parse_file(path.string());
    auto mods = table["mods"].as_array();
    if (!mods) {
      return entries;
    }
    for (auto& element : *mods) {
      auto* entry_table = element.as_table();
      if (!entry_table) {
        continue;
      }
      auto id = (*entry_table)["id"].value<std::string>();
      if (!id || id->empty()) {
        continue;
      }
      ModStateEntry entry;
      entry.id = *id;
      entry.enabled = (*entry_table)["enabled"].value_or(true);
      entries.push_back(std::move(entry));
    }
  } catch (const toml::parse_error& error) {
    REXSYS_WARN("Failed to parse {}: {}", path.string(), error.what());
  }
  return entries;
}

bool ModState::Save(const std::filesystem::path& root, const std::vector<ModStateEntry>& entries) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    REXSYS_ERROR("Failed to create mods root {}: {}", root.string(), ec.message());
    return false;
  }

  toml::table doc;
  toml::array mods;
  for (const auto& entry : entries) {
    toml::table entry_table;
    entry_table.insert("id", entry.id);
    entry_table.insert("enabled", entry.enabled);
    mods.push_back(std::move(entry_table));
  }
  doc.insert("mods", std::move(mods));

  // Atomic-ish write: temp file in the same directory (so rename is same-
  // filesystem), then rename over the final path.
  std::random_device rd;
  auto temp_path = root / (std::string(".mods-") + std::to_string(rd()) + ".toml.tmp");
  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXSYS_ERROR("Failed to open {} for writing", temp_path.string());
      return false;
    }
    out << doc;
  }
  auto final_path = root / kSidecarName;
  std::filesystem::rename(temp_path, final_path, ec);
  if (ec) {
    REXSYS_ERROR("Failed to write {}: {}", final_path.string(), ec.message());
    std::filesystem::remove(temp_path);
    return false;
  }
  return true;
}

bool ModState::RemoveMod(const std::filesystem::path& root, const std::string& id) {
  if (id.empty() || id == "." || id == ".." || id.find('/') != std::string::npos ||
      id.find('\\') != std::string::npos) {
    return false;
  }

  auto dir = root / id;
  std::error_code ec;
  if (std::filesystem::exists(dir, ec)) {
    std::filesystem::remove_all(dir, ec);
    if (ec) {
      REXSYS_ERROR("Failed to remove mod folder {}: {}", dir.string(), ec.message());
      return false;
    }
  }

  auto entries = Load(root);
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const ModStateEntry& e) { return e.id == id; }),
                entries.end());
  return Save(root, entries);
}

bool ModState::MarkPendingRemoval(const std::filesystem::path& root, const std::string& id) {
  if (id.empty() || id == "." || id == ".." || id.find('/') != std::string::npos ||
      id.find('\\') != std::string::npos) {
    return false;
  }
  auto pending_root = PendingRemovalsRoot(root);
  std::error_code ec;
  std::filesystem::create_directories(pending_root, ec);
  if (ec) {
    REXSYS_ERROR("Failed to create pending-removals folder: {}", ec.message());
    return false;
  }
  std::ofstream marker(pending_root / id, std::ios::binary);
  return static_cast<bool>(marker);
}

bool ModState::UnmarkPendingRemoval(const std::filesystem::path& root, const std::string& id) {
  std::error_code ec;
  std::filesystem::remove(PendingRemovalsRoot(root) / id, ec);
  return !ec;
}

std::unordered_set<std::string> ModState::PendingRemovals(const std::filesystem::path& root) {
  std::unordered_set<std::string> ids;
  std::error_code ec;
  auto pending_root = PendingRemovalsRoot(root);
  if (!std::filesystem::is_directory(pending_root, ec)) {
    return ids;
  }
  for (auto& entry : std::filesystem::directory_iterator(pending_root, ec)) {
    ids.insert(entry.path().filename().string());
  }
  return ids;
}

std::vector<std::string> ModState::InstalledIds(const std::filesystem::path& root) {
  std::vector<std::string> ids;
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return ids;
  }
  for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    std::string name = entry.path().filename().string();
    // Every in-progress install/update writes its scratch state as a
    // dot-prefixed subfolder directly under root (.pending-updates,
    // .pending-removals, .sideload-staging-<name>, .catalog-staging-<id>)
    // precisely so it's never mistaken for an installed mod -- skip them all
    // here rather than naming each one, so a new staging convention added
    // later doesn't need a matching change here. A mod marked for removal
    // (MarkPendingRemoval) deliberately isn't filtered by anything here: its
    // folder and mods.toml entry are both left untouched until the next
    // launch's ApplyPendingRemovals, so it should keep showing up exactly
    // like any other installed mod until then.
    if (!name.empty() && name.front() == '.') {
      continue;
    }
    ids.push_back(name);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<ModStateEntry> ModState::Reconcile(const std::vector<ModStateEntry>& entries,
                                               const std::vector<std::string>& on_disk_ids) {
  std::unordered_set<std::string> on_disk(on_disk_ids.begin(), on_disk_ids.end());

  std::vector<ModStateEntry> result;
  result.reserve(entries.size() + on_disk_ids.size());
  std::unordered_set<std::string> seen;
  for (const auto& entry : entries) {
    if (on_disk.contains(entry.id)) {
      result.push_back(entry);
      seen.insert(entry.id);
    }
  }
  for (const auto& id : on_disk_ids) {
    if (!seen.contains(id)) {
      result.push_back(ModStateEntry{id, /*enabled=*/true});
    }
  }
  return result;
}

std::vector<ModStateEntry> ModState::LoadReconciled(const std::filesystem::path& root) {
  return Reconcile(Load(root), InstalledIds(root));
}

std::vector<std::string> ModState::EnabledIdsInOrder(const std::vector<ModStateEntry>& entries) {
  std::vector<std::string> ids;
  ids.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.enabled) {
      ids.push_back(entry.id);
    }
  }
  return ids;
}

std::vector<ModStateEntry> ModState::AutoSort(
    const std::vector<ModStateEntry>& entries,
    const std::unordered_map<std::string, ModInfo>& manifests) {
  std::vector<std::string> enabled_ids = EnabledIdsInOrder(entries);
  std::unordered_set<std::string> enabled_set(enabled_ids.begin(), enabled_ids.end());

  static const ModInfo kDefaultManifest;
  auto lookup = [&](const std::string& id) -> const ModInfo& {
    auto it = manifests.find(id);
    return it != manifests.end() ? it->second : kDefaultManifest;
  };

  // deps[i] = indices (into enabled_ids) this id must load after.
  std::unordered_map<std::string, std::vector<std::string>> deps;
  deps.reserve(enabled_ids.size());
  for (const auto& id : enabled_ids) {
    const auto& info = lookup(id);
    std::vector<std::string> wants;
    for (const auto& req : info.requires_mods) {
      if (req.name != id && enabled_set.contains(req.name)) {
        wants.push_back(req.name);
      }
    }
    for (const auto& after : info.load_after_mods) {
      if (after != id && enabled_set.contains(after)) {
        wants.push_back(after);
      }
    }
    deps.emplace(id, std::move(wants));
  }

  // Stable Kahn's-algorithm topo sort: repeatedly take the earliest (by
  // original order) node whose dependencies are all already placed; if none
  // is ready (a cycle), take the earliest remaining node anyway so sorting
  // always terminates.
  std::vector<std::string> remaining = enabled_ids;
  std::vector<std::string> placed;
  placed.reserve(remaining.size());
  while (!remaining.empty()) {
    auto it = std::find_if(remaining.begin(), remaining.end(), [&](const std::string& id) {
      const auto& wants = deps[id];
      return std::all_of(wants.begin(), wants.end(), [&](const std::string& dep) {
        return std::find(placed.begin(), placed.end(), dep) != placed.end();
      });
    });
    if (it == remaining.end()) {
      it = remaining.begin();
    }
    placed.push_back(*it);
    remaining.erase(it);
  }

  // Rebuild the full list: enabled slots take the new order from `placed`;
  // disabled entries keep their original absolute position.
  std::vector<ModStateEntry> result;
  result.reserve(entries.size());
  size_t placed_index = 0;
  for (const auto& entry : entries) {
    if (entry.enabled) {
      result.push_back(ModStateEntry{placed.at(placed_index++), true});
    } else {
      result.push_back(entry);
    }
  }
  return result;
}

std::string ModState::HostPlatformId() {
#if defined(_WIN32)
  return "windows-x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
}

std::vector<ModIssue> ModState::Validate(const std::vector<ModStateEntry>& entries,
                                         const std::unordered_map<std::string, ModInfo>& manifests,
                                         std::string_view host_version,
                                         std::string_view host_platform) {
  std::vector<ModIssue> issues;
  std::vector<std::string> enabled_ids = EnabledIdsInOrder(entries);
  std::unordered_map<std::string, size_t> index_of;
  for (size_t i = 0; i < enabled_ids.size(); ++i) {
    index_of.emplace(enabled_ids[i], i);
  }

  static const ModInfo kDefaultManifest;
  auto lookup = [&](const std::string& id) -> const ModInfo& {
    auto it = manifests.find(id);
    return it != manifests.end() ? it->second : kDefaultManifest;
  };

  auto err = [&](const std::string& id, std::string message) {
    issues.push_back(ModIssue{id, ModIssue::Kind::kError, std::move(message)});
  };
  auto warn = [&](const std::string& id, std::string message) {
    issues.push_back(ModIssue{id, ModIssue::Kind::kWarning, std::move(message)});
  };

  for (size_t i = 0; i < enabled_ids.size(); ++i) {
    const auto& id = enabled_ids[i];
    const auto& info = lookup(id);

    if (std::any_of(info.requires_mods.begin(), info.requires_mods.end(),
                    [&](const auto& r) { return r.name == id; })) {
      err(id, "\"" + id + "\" lists itself in requires -- remove the self-reference.");
    }
    if (std::find(info.conflicts_mods.begin(), info.conflicts_mods.end(), id) !=
        info.conflicts_mods.end()) {
      err(id, "\"" + id + "\" lists itself in conflicts -- remove the self-reference.");
    }

    bool is_code = !info.code.empty();
    if (is_code) {
      if (info.platforms.empty()) {
        err(id, "\"" + id +
                    "\" is a code mod but declares no platform binaries; it can't load. Update "
                    "or remove it.");
      } else if (std::find(info.platforms.begin(), info.platforms.end(),
                           std::string(host_platform)) == info.platforms.end()) {
        std::string ships;
        for (size_t k = 0; k < info.platforms.size(); ++k) {
          if (k)
            ships += ", ";
          ships += info.platforms[k];
        }
        err(id, "\"" + id + "\" has no binary for this platform (ships: " + ships +
                    "). Update, disable, or remove it.");
      }
    }

    for (const auto& req : info.requires_mods) {
      if (req.name == id) {
        continue;
      }
      auto it = index_of.find(req.name);
      if (it == index_of.end()) {
        err(id, "\"" + id + "\" requires \"" + req.name +
                    "\", which isn't enabled. Enable/install it, or disable \"" + id + "\".");
        continue;
      }
      if (it->second > i) {
        err(id, "\"" + req.name + "\" must load before \"" + id +
                    "\". Click Auto-sort, or move it higher.");
        continue;
      }
      if (req.min_version.empty()) {
        continue;
      }
      const auto& dep_info = lookup(req.name);
      std::vector<int> want;
      std::vector<int> have;
      if (!ParseVersionComponents(req.min_version, want)) {
        warn(id, "\"" + id + "\" requires \"" + req.name + "\" >= " + req.min_version + ", but \"" +
                     req.min_version +
                     "\" isn't a valid version (e.g. \"1.0.0\") "
                     "-- can't verify.");
      } else if (dep_info.version.empty() || !ParseVersionComponents(dep_info.version, have)) {
        warn(id, "\"" + id + "\" requires \"" + req.name + "\" >= " + req.min_version + ", but \"" +
                     req.name +
                     "\" has no valid version in its mod.toml -- can't "
                     "verify.");
      } else if (CompareVersions(have, want) < 0) {
        err(id, "\"" + id + "\" requires \"" + req.name + "\" >= " + req.min_version +
                    ", but the enabled \"" + req.name + "\" is only version " + dep_info.version +
                    ".");
      }
    }

    if (!info.min_game_version.empty()) {
      std::vector<int> want;
      if (!ParseVersionComponents(info.min_game_version, want)) {
        warn(id, "\"" + id + "\" has game_version = \"" + info.min_game_version +
                     "\", which isn't a valid version (e.g. \"1.0.0\") -- can't verify.");
      } else {
        std::vector<int> have;
        if (host_version.empty() || !ParseVersionComponents(host_version, have)) {
          warn(id, "\"" + id + "\" requires game " + info.min_game_version +
                       " or newer, but the installed game version is unknown -- can't verify.");
        } else if (CompareVersions(have, want) < 0) {
          err(id, "\"" + id + "\" requires game " + info.min_game_version +
                      " or newer; the installed game is " + std::string(host_version) +
                      ". Update the game, or disable this mod.");
        }
      }
    }

    for (const auto& after : info.load_after_mods) {
      if (after == id) {
        continue;
      }
      auto it = index_of.find(after);
      if (it == index_of.end()) {
        warn(id,
             "\"" + id + "\" works better loaded after \"" + after + "\", which isn't enabled.");
      } else if (it->second > i) {
        warn(id,
             "\"" + id + "\" works better loaded after \"" + after + "\" (currently loads first).");
      }
    }
  }

  // conflicts: unordered pairs, one issue per side.
  std::unordered_set<std::string> reported;
  for (const auto& id : enabled_ids) {
    const auto& info = lookup(id);
    for (const auto& conflict : info.conflicts_mods) {
      if (conflict == id || !index_of.contains(conflict)) {
        continue;
      }
      std::string a = std::min(id, conflict);
      std::string b = std::max(id, conflict);
      std::string pair_key = a + "\x1f" + b;
      if (reported.contains(pair_key)) {
        continue;
      }
      reported.insert(pair_key);
      err(a, "\"" + a + "\" conflicts with \"" + b + "\". Disable or remove one of them.");
      err(b, "\"" + b + "\" conflicts with \"" + a + "\". Disable or remove one of them.");
    }
  }

  return issues;
}

std::optional<ModInstallResult> ModState::InstallLocalArchive(const std::filesystem::path& root,
                                                              const std::filesystem::path& zip_path,
                                                              std::string& error) {
  error.clear();

  if (!HasZipExtension(zip_path)) {
    error = "not a .zip file";
    return std::nullopt;
  }

  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    error = "failed to create mods folder: " + ec.message();
    return std::nullopt;
  }

  auto staging = root / (".sideload-staging-" + zip_path.stem().string());
  std::filesystem::remove_all(staging, ec);
  std::string extract_error;
  if (!rex::filesystem::ExtractZip(zip_path, staging, extract_error)) {
    std::filesystem::remove_all(staging, ec);
    error = "failed to extract archive: " + extract_error;
    return std::nullopt;
  }

  // Unwrap a single top-level directory if present, same convention as a
  // catalog install (ModCatalog::InstallWorker) and the companion desktop
  // launcher's install_one_archive.
  std::filesystem::path content_root = staging;
  std::string derived_id;
  {
    std::vector<std::filesystem::directory_entry> top_level;
    for (auto& entry : std::filesystem::directory_iterator(staging, ec)) {
      top_level.push_back(entry);
    }
    if (top_level.size() == 1 && top_level[0].is_directory()) {
      content_root = top_level[0].path();
      derived_id = SanitizeModId(top_level[0].path().filename().string());
    } else {
      derived_id = SanitizeModId(zip_path.stem().string());
    }
  }

  // Unlike a catalog install (which trusts the catalog's modId), a dropped
  // zip has no other signal to confirm it's actually a mod archive and not
  // some unrelated file the player dragged in by mistake.
  if (!std::filesystem::is_regular_file(content_root / "mod.toml")) {
    std::filesystem::remove_all(staging, ec);
    error = "archive does not contain a mod.toml -- not a valid mod";
    return std::nullopt;
  }

  std::string new_version = ReadModVersion(content_root / "mod.toml");

  auto dest = root / derived_id;
  bool updated = std::filesystem::exists(dest, ec);
  if (updated) {
    std::string existing_version = ReadModVersion(dest / "mod.toml");
    std::vector<int> want;
    std::vector<int> have;
    bool new_parses = ParseVersionComponents(new_version, want);
    bool existing_parses = ParseVersionComponents(existing_version, have);
    // Same "can't-verify never blocks" posture as the rest of the mod
    // system: only refuse when both sides parse and the drop is strictly
    // older, so an unversioned mod (or one predating this check) is always
    // safe to re-drop over itself.
    if (new_parses && existing_parses && CompareVersions(have, want) > 0) {
      std::filesystem::remove_all(staging, ec);
      error = "a newer version of \"" + derived_id + "\" is already installed (installed v" +
              (existing_version.empty() ? "?" : existing_version) + ", dropped v" +
              (new_version.empty() ? "?" : new_version) + ")";
      return std::nullopt;
    }
  }

  // Clear out the old install first, same as always -- but if that fails,
  // `dest` has a file Windows won't let go of (most likely a mod DLL this
  // very process still has mapped/open), so there's no way to replace it in
  // place. Stage the new content instead; ApplyPendingUpdates swaps it onto
  // `dest` at the next launch, before anything has a chance to open it.
  bool staged = false;
  if (updated) {
    std::filesystem::remove_all(dest, ec);
    if (ec) {
      if (!StagePendingUpdate(root, derived_id, content_root, error)) {
        std::filesystem::remove_all(staging, ec);
        return std::nullopt;
      }
      staged = true;
    }
  }
  if (!staged && !rex::filesystem::MoveOrCopyDirectory(content_root, dest, error)) {
    std::filesystem::remove_all(staging, ec);
    return std::nullopt;
  }
  std::filesystem::remove_all(staging, ec);

  // A staged update hasn't touched mods.toml's on-disk folder yet, but the
  // entry itself (id/enabled/order) is unaffected either way, so this is safe
  // to record now regardless of `staged`.
  auto entries = LoadReconciled(root);
  if (std::none_of(entries.begin(), entries.end(),
                   [&](const ModStateEntry& e) { return e.id == derived_id; })) {
    entries.push_back(ModStateEntry{derived_id, true});
  }
  Save(root, entries);

  return ModInstallResult{derived_id, new_version, updated, staged};
}

std::filesystem::path ModState::PendingUpdatesRoot(const std::filesystem::path& root) {
  return root / ".pending-updates";
}

bool ModState::HasPendingUpdates(const std::filesystem::path& root) {
  std::error_code ec;
  auto pending_root = PendingUpdatesRoot(root);
  if (!std::filesystem::is_directory(pending_root, ec)) {
    return false;
  }
  for (auto& entry : std::filesystem::directory_iterator(pending_root, ec)) {
    if (entry.is_directory()) {
      return true;
    }
  }
  return false;
}

bool ModState::StagePendingUpdate(const std::filesystem::path& root, const std::string& id,
                                  const std::filesystem::path& content_root, std::string& error) {
  auto pending_root = PendingUpdatesRoot(root);
  std::error_code ec;
  std::filesystem::create_directories(pending_root, ec);
  if (ec) {
    error = "failed to create pending-updates folder: " + ec.message();
    return false;
  }
  auto staged_dest = pending_root / id;
  std::filesystem::remove_all(staged_dest, ec);
  return rex::filesystem::MoveOrCopyDirectory(content_root, staged_dest, error);
}

void ModState::ApplyPendingUpdates(const std::filesystem::path& root) {
  // If this process was just relaunched (see ModManagerDialog's "Restart &
  // Apply"), the old instance may still be shutting down and, until
  // it fully exits, may still hold the very files a staged update needs to
  // replace (a loaded mod DLL). Give it a bounded window to finish first --
  // without this, applying tends to lose the same race the live in-place
  // update did, silently leaving the update staged again. Unconditional (and
  // cheap/no-op when this wasn't a relaunch) so the wait always happens
  // regardless of whether an update happens to be staged.
  rex::platform::process::WaitForPreviousInstanceExit();

  auto pending_root = PendingUpdatesRoot(root);
  std::error_code ec;
  if (!std::filesystem::is_directory(pending_root, ec)) {
    return;
  }

  std::vector<std::filesystem::path> staged_dirs;
  for (auto& entry : std::filesystem::directory_iterator(pending_root, ec)) {
    if (entry.is_directory()) {
      staged_dirs.push_back(entry.path());
    }
  }

  for (auto& staged_dir : staged_dirs) {
    std::string id = staged_dir.filename().string();
    auto dest = root / id;
    std::filesystem::remove_all(dest, ec);
    std::string move_error;
    if (!rex::filesystem::MoveOrCopyDirectory(staged_dir, dest, move_error)) {
      REXSYS_WARN("Failed to apply pending update for mod '{}': {} (will retry next launch)", id,
                  move_error);
    }
  }
}

std::filesystem::path ModState::PendingRemovalsRoot(const std::filesystem::path& root) {
  return root / ".pending-removals";
}

bool ModState::HasPendingRemovals(const std::filesystem::path& root) {
  std::error_code ec;
  auto pending_root = PendingRemovalsRoot(root);
  if (!std::filesystem::is_directory(pending_root, ec)) {
    return false;
  }
  for (auto& entry : std::filesystem::directory_iterator(pending_root, ec)) {
    (void)entry;
    return true;
  }
  return false;
}

void ModState::ApplyPendingRemovals(const std::filesystem::path& root) {
  // Same reasoning as ApplyPendingUpdates: a relaunched instance may still
  // need to wait for the old one to actually let go of a locked mod folder.
  rex::platform::process::WaitForPreviousInstanceExit();

  auto pending_root = PendingRemovalsRoot(root);
  std::error_code ec;
  if (!std::filesystem::is_directory(pending_root, ec)) {
    return;
  }

  std::vector<std::filesystem::path> markers;
  for (auto& entry : std::filesystem::directory_iterator(pending_root, ec)) {
    markers.push_back(entry.path());
  }

  std::vector<std::string> removed_ids;
  for (auto& marker : markers) {
    std::string id = marker.filename().string();
    std::filesystem::remove_all(root / id, ec);
    if (ec) {
      REXSYS_WARN("Failed to apply pending removal for mod '{}': {} (will retry next launch)", id,
                  ec.message());
      continue;
    }
    std::filesystem::remove(marker, ec);
    removed_ids.push_back(std::move(id));
  }

  // The mods.toml entry is only ever dropped here, once the folder is
  // actually gone -- MarkPendingRemoval deliberately leaves it in place so
  // the mod keeps showing/loading normally until this point.
  if (!removed_ids.empty()) {
    auto entries = Load(root);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const ModStateEntry& e) {
                                   return std::find(removed_ids.begin(), removed_ids.end(), e.id) !=
                                          removed_ids.end();
                                 }),
                  entries.end());
    Save(root, entries);
  }
}

}  // namespace rex::system
