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
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_set>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>  // REXCVAR_DECLARE(mods_data_root)
#include <rex/system/mod_version.h>

namespace rex::system {

namespace {
constexpr const char* kSidecarName = "mods.toml";
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

std::vector<std::string> ModState::InstalledIds(const std::filesystem::path& root) {
  std::vector<std::string> ids;
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return ids;
  }
  for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (entry.is_directory()) {
      ids.push_back(entry.path().filename().string());
    }
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
                    "\". Click Auto-sort, or drag it higher.");
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

}  // namespace rex::system
