/**
 * @file        system/mod_conflict_tracker.cpp
 * @brief       Mod conflict tracker implementation. See mod_conflict_tracker.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_conflict_tracker.h>

namespace rex::system {

void ModConflictTracker::RecordCvarActivity(std::string_view mod_name, CvarActivity activity) {
  std::lock_guard lock(mutex_);
  std::string mod(mod_name);
  std::string cvar_name = activity.name;
  std::string new_value = activity.new_value;
  cvar_activity_by_mod_[mod].push_back(std::move(activity));
  cvar_last_value_by_mod_[cvar_name][mod] = std::move(new_value);
}

void ModConflictTracker::RecordKeybindReassignment(std::string_view mod_name,
                                                   std::string_view bind_name,
                                                   std::string_view requested_key,
                                                   std::string_view effective_key) {
  std::lock_guard lock(mutex_);
  auto& list = keybind_reassignments_by_mod_[std::string(mod_name)];
  list.push_back({std::string(bind_name), std::string(requested_key), std::string(effective_key)});
}

std::vector<ModConflictTracker::CvarActivity> ModConflictTracker::CvarActivityFor(
    std::string_view mod_name) const {
  std::lock_guard lock(mutex_);
  auto it = cvar_activity_by_mod_.find(std::string(mod_name));
  if (it == cvar_activity_by_mod_.end()) {
    return {};
  }
  return it->second;
}

std::vector<ModConflictTracker::KeybindReassignment> ModConflictTracker::KeybindReassignmentsFor(
    std::string_view mod_name) const {
  std::lock_guard lock(mutex_);
  auto it = keybind_reassignments_by_mod_.find(std::string(mod_name));
  if (it == keybind_reassignments_by_mod_.end()) {
    return {};
  }
  return it->second;
}

std::unordered_map<std::string, std::vector<std::string>> ModConflictTracker::DivergentOverrides()
    const {
  std::lock_guard lock(mutex_);
  std::unordered_map<std::string, std::vector<std::string>> result;
  for (const auto& [cvar_name, mod_values] : cvar_last_value_by_mod_) {
    if (mod_values.size() < 2) {
      continue;
    }
    // Divergent only if the set of distinct values is > 1 (two mods setting
    // the same cvar to the *same* value isn't a real conflict).
    const std::string* first_value = nullptr;
    bool divergent = false;
    for (const auto& [mod, value] : mod_values) {
      if (!first_value) {
        first_value = &value;
      } else if (value != *first_value) {
        divergent = true;
        break;
      }
    }
    if (!divergent) {
      continue;
    }
    std::vector<std::string> mods;
    mods.reserve(mod_values.size());
    for (const auto& [mod, value] : mod_values) {
      mods.push_back(mod);
    }
    result.emplace(cvar_name, std::move(mods));
  }
  return result;
}

}  // namespace rex::system
