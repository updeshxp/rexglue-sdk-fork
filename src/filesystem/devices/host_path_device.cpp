/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               bool allow_share_delete)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      allow_share_delete_(allow_share_delete) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (overlay_roots_.empty()) {
    PopulateEntry(root_entry);
  } else {
    // Build the full list of layer dirs: overlays (highest priority first),
    // then the base host_path_.
    std::vector<std::filesystem::path> layers;
    layers.reserve(overlay_roots_.size() + 1);
    for (auto& r : overlay_roots_) {
      layers.push_back(r);
    }
    layers.push_back(host_path_);
    PopulateEntryMerged(root_entry, layers);
    REXFS_INFO("Overlay merge complete; root has {} children", root_entry->children_.size());
  }

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntryMerged(HostPathEntry* parent_entry,
                                         const std::vector<std::filesystem::path>& layer_dirs) {
  // Track which child names we've already added (case-insensitive).
  // For directories that appear in multiple layers, we accumulate the
  // per-layer host paths so we can recurse with a merged source list.
  struct MergedDir {
    HostPathEntry* entry;
    std::vector<std::filesystem::path> source_dirs;
  };
  std::vector<std::pair<std::string, MergedDir>> merged_dirs;  // name -> info

  auto find_merged = [&](const std::string_view name) -> MergedDir* {
    for (auto& [n, md] : merged_dirs) {
      if (rex::string::utf8_equal_case(n, name)) {
        return &md;
      }
    }
    return nullptr;
  };

  auto is_known = [&](const std::string_view name) -> bool {
    for (auto& child : parent_entry->children_) {
      if (rex::string::utf8_equal_case(child->name(), name)) {
        return true;
      }
    }
    return find_merged(name) != nullptr;
  };

  for (const auto& layer_dir : layer_dirs) {
    if (!std::filesystem::exists(layer_dir)) {
      continue;
    }
    auto child_infos = rex::filesystem::ListFiles(layer_dir);
    for (auto& child_info : child_infos) {
      auto child_name = rex::path_to_utf8(child_info.name);
      bool is_dir = child_info.type == rex::filesystem::FileInfo::Type::kDirectory;

      if (is_dir) {
        auto* existing = find_merged(child_name);
        if (existing) {
          // Directory already created by a higher-priority layer; just
          // accumulate this layer's host path for the recursive merge.
          existing->source_dirs.push_back(layer_dir / child_info.name);
          continue;
        }
        // First layer to introduce this directory — create the entry.
        auto child =
            HostPathEntry::Create(this, parent_entry, layer_dir / child_info.name, child_info);
        if (!child)
          continue;
        MergedDir md;
        md.entry = child;
        md.source_dirs.push_back(layer_dir / child_info.name);
        merged_dirs.push_back({child_name, std::move(md)});
        parent_entry->children_.push_back(std::unique_ptr<Entry>(child));
      } else {
        // File: first layer wins, skip if already known.
        if (is_known(child_name))
          continue;
        auto child =
            HostPathEntry::Create(this, parent_entry, layer_dir / child_info.name, child_info);
        if (child) {
          parent_entry->children_.push_back(std::unique_ptr<Entry>(child));
        }
      }
    }
  }

  // Recurse into merged directories.
  for (auto& [name, md] : merged_dirs) {
    PopulateEntryMerged(md.entry, md.source_dirs);
  }
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    auto child = HostPathEntry::Create(this, parent_entry,
                                       parent_entry->host_path() / child_info.name, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

}  // namespace rex::filesystem
