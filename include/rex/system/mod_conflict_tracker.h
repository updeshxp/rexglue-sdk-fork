/**
 * @file        system/mod_conflict_tracker.h
 * @brief       Records keybind reassignments and cvar-override conflicts
 *              between mods, for display in the mod manager overlay.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Reached via Runtime::mod_conflict_tracker(), the same pattern
 *              as ModRegistry. This is purely a bookkeeping/reporting layer:
 *              keybinds.cpp records reassignments here as they happen;
 *              ReXApp::Setup() records cvar defines/overrides here via a
 *              before/after registry snapshot around each mod's
 *              OnCreateDialogs/OnModuleLaunched call (see rex_app.cpp). It
 *              does not itself change any cvar or keybind behavior.
 */
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::system {

class ModConflictTracker {
 public:
  // One cvar this mod defined (a name that didn't exist before its
  // lifecycle call) or overrode (changed the value of a pre-existing cvar).
  struct CvarActivity {
    std::string name;
    bool is_new_definition = false;  // true = defined, false = overrode
    std::string old_value;           // only meaningful when !is_new_definition
    std::string new_value;
  };

  // Records that `mod_name` defined or changed `cvar_name`'s value from
  // `old_value` to `new_value` during its lifecycle call. If another mod
  // already recorded a different `new_value` for the same cvar, both are
  // flagged as a divergent-override conflict (see DivergentOverrides()).
  void RecordCvarActivity(std::string_view mod_name, CvarActivity activity);

  // Records that `mod_name`'s bind `bind_name` wanted `requested_key` but was
  // moved to `effective_key` because the requested key was already taken.
  // An empty `effective_key` means no free key was available (unresolved).
  void RecordKeybindReassignment(std::string_view mod_name, std::string_view bind_name,
                                 std::string_view requested_key, std::string_view effective_key);

  struct KeybindReassignment {
    std::string bind_name;
    std::string requested_key;
    std::string effective_key;  // empty = unresolved (pool exhausted)
  };

  // All cvar activity recorded for `mod_name`, in recording order.
  std::vector<CvarActivity> CvarActivityFor(std::string_view mod_name) const;

  // All keybind reassignments recorded for `mod_name`, in recording order.
  std::vector<KeybindReassignment> KeybindReassignmentsFor(std::string_view mod_name) const;

  // Names of cvars that two or more mods overrode to different final values,
  // paired with the mod names involved (for a tooltip like "also set by X").
  std::unordered_map<std::string, std::vector<std::string>> DivergentOverrides() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::vector<CvarActivity>> cvar_activity_by_mod_;
  std::unordered_map<std::string, std::vector<KeybindReassignment>> keybind_reassignments_by_mod_;

  // cvar name -> (mod name -> last value that mod set it to), used to detect
  // divergent overrides across mods.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      cvar_last_value_by_mod_;
};

}  // namespace rex::system
