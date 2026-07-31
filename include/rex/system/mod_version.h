/**
 * @file        system/mod_version.h
 * @brief       Shared comma-list/version helpers for mod.toml parsing and
 *              validation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Extracted from runtime.cpp so rex::system::ModState and
 *              rex::system::ModCatalog can share them without depending on
 *              Runtime itself.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/system/mod_plugin.h>  // ModRequirement

namespace rex::system {

// Splits a comma-separated list, trims whitespace around each entry, and
// drops empty entries. Shared by enabled_mods/mods.toml resolution and
// mod.toml's requires/load_after/conflicts/platform keys -- all use the same
// "comma list of names" convention.
std::vector<std::string> SplitCommaList(std::string_view csv);

// Splits one `requires` entry into a mod name and an optional minimum-version
// constraint: "game_symbols >= 1.0.0" -> {"game_symbols", "1.0.0"}; a bare
// "game_symbols" -> {"game_symbols", ""} (unconstrained).
ModRequirement ParseRequirement(std::string_view entry);

// Parses mod.toml's `game_version` key into a minimum-version constraint.
// Both "1.2.0" and ">= 1.2.0" mean "must be at least 1.2.0" -- no other
// comparison operator is supported. Returns empty for an absent/blank key.
std::string ParseGameVersionConstraint(std::string_view value);

// Parses a dotted numeric version ("1.0.0", "2.3") into its components.
// Returns false if any '.'-separated part isn't a non-negative integer or the
// string is empty -- callers treat that as "not a usable version" rather than
// trying to compare it.
bool ParseVersionComponents(std::string_view version, std::vector<int>& out);

// Compares two parsed versions component-wise; missing trailing components
// count as 0, so "1.0" == "1.0.0". Returns <0, 0, >0 as `have` compares to
// `want`.
int CompareVersions(const std::vector<int>& have, const std::vector<int>& want);

// Lenient string-form version compare: parses both sides with
// ParseVersionComponents (missing/unparsable segments count as 0) and
// compares. Used where a version can't be assumed to already be a validated
// dotted number (e.g. catalog data).
int CompareVersionStrings(std::string_view have, std::string_view want);

}  // namespace rex::system
