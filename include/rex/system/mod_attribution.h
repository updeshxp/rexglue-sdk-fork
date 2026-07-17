/**
 * @file        system/mod_attribution.h
 * @brief       Tracks which enabled mod is currently executing lifecycle code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Several subsystems (keybind auto-reassignment, cvar-override
 *              conflict detection) need to attribute a registration/change to
 *              the mod that caused it. ReXApp sets this via ScopedActiveMod
 *              while dispatching each mod's IModPlugin::OnCreateDialogs /
 *              OnModuleLaunched (see rex_app.cpp); an empty active mod means
 *              "base app / not inside a mod's lifecycle call."
 */
#pragma once

#include <string>
#include <string_view>

namespace rex::system {

// Returns the folder name of the mod currently executing lifecycle code on
// this thread, or an empty string outside any mod's lifecycle call.
std::string_view CurrentActiveMod();

// RAII scope: sets CurrentActiveMod() to `mod_name` for its lifetime,
// restoring the previous value (supports nested scopes, though mods aren't
// expected to nest today). Thread-local, so concurrent mod dispatch on
// different threads attributes correctly.
class ScopedActiveMod {
 public:
  explicit ScopedActiveMod(std::string_view mod_name);
  ~ScopedActiveMod();

  ScopedActiveMod(const ScopedActiveMod&) = delete;
  ScopedActiveMod& operator=(const ScopedActiveMod&) = delete;

 private:
  std::string previous_;
};

}  // namespace rex::system
