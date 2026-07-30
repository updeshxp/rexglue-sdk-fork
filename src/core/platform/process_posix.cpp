/**
 * @file        core/platform/process_posix.cpp
 * @brief       POSIX implementation of rex::platform::process.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/process.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <rex/logging.h>

#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace rex::platform::process {

bool Relaunch() {
#if REX_PLATFORM_LINUX
  // /proc/self/cmdline holds the original argv, NUL-separated; there's no
  // Linux equivalent of Win32's GetCommandLineW() to re-fetch it otherwise.
  std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
  if (!cmdline_file) {
    REXLOG_ERROR("Relaunch: failed to open /proc/self/cmdline");
    return false;
  }
  std::string raw((std::istreambuf_iterator<char>(cmdline_file)), std::istreambuf_iterator<char>());
  if (raw.empty()) {
    REXLOG_ERROR("Relaunch: /proc/self/cmdline was empty");
    return false;
  }

  std::vector<std::string> args;
  size_t start = 0;
  while (start < raw.size()) {
    size_t nul = raw.find('\0', start);
    if (nul == std::string::npos) {
      nul = raw.size();
    }
    args.emplace_back(raw.substr(start, nul - start));
    start = nul + 1;
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    REXLOG_ERROR("Relaunch: fork failed");
    return false;
  }
  if (pid == 0) {
    // Child: re-exec via /proc/self/exe (the running binary, even if argv[0]
    // was a relative/PATH-resolved name that no longer resolves from here).
    execv("/proc/self/exe", argv.data());
    _exit(127);  // execv only returns on failure.
  }
  return true;
#else
  REXLOG_ERROR("Relaunch: not implemented on this platform");
  return false;
#endif
}

}  // namespace rex::platform::process
