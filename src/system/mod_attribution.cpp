/**
 * @file        system/mod_attribution.cpp
 * @brief       Mod attribution implementation. See mod_attribution.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_attribution.h>

namespace rex::system {

namespace {
thread_local std::string g_active_mod;
}  // namespace

std::string_view CurrentActiveMod() {
  return g_active_mod;
}

ScopedActiveMod::ScopedActiveMod(std::string_view mod_name) : previous_(std::move(g_active_mod)) {
  g_active_mod = std::string(mod_name);
}

ScopedActiveMod::~ScopedActiveMod() {
  g_active_mod = std::move(previous_);
}

}  // namespace rex::system
