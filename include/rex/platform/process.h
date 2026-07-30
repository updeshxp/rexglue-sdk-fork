/**
 * @file        rex/platform/process.h
 * @brief       Platform-agnostic process control.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::platform::process {

// Spawns a new instance of the currently running executable with the same
// command line (including any --cvar arguments), independent of the current
// process. Intended for "restart to apply" flows (e.g. a settings overlay
// changing a kRequiresRestart cvar): callers still need to shut the current
// process down themselves afterward (e.g. WindowedApp::window()->
// RequestClose()) -- this only starts the replacement. Returns false if the
// new process could not be spawned (current process is left running).
bool Relaunch();

}  // namespace rex::platform::process
