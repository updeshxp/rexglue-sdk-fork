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
//
// The new process starts concurrently with the old one's (caller-driven)
// shutdown -- there's no guarantee the old process has actually exited, or
// released any files it had open, by the time the new process's own startup
// runs. Records this process's id (via an environment variable the new
// process inherits) so the new process's WaitForPreviousInstanceExit() can
// wait on it.
bool Relaunch();

// If this process was started by another instance's Relaunch(), blocks
// (up to a few seconds) until that instance has fully exited; a no-op
// otherwise, or once already called (the handoff is consumed on first use).
// Call this once, as early in startup as practical, before anything that
// might need a file/handle the old instance could still be holding open
// during its own shutdown (e.g. applying a staged mod update onto a folder
// the old process had a mod DLL loaded from).
void WaitForPreviousInstanceExit();

}  // namespace rex::platform::process
