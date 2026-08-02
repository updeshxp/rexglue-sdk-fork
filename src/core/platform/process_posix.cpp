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

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rex::platform::process {

namespace {
// execv() replaces the forked child's image in place, so on the new binary's
// side this is just an inherited environment variable (setenv() before
// fork()+execv() below), same handoff trick as the Windows implementation.
constexpr const char* kRelaunchFromPidEnvVar = "REX_RELAUNCH_FROM_PID";
}  // namespace

bool Relaunch() {
#if REX_PLATFORM_LINUX
  setenv(kRelaunchFromPidEnvVar, std::to_string(getpid()).c_str(), 1);
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

void WaitForPreviousInstanceExit() {
#if REX_PLATFORM_LINUX
  const char* pid_string = getenv(kRelaunchFromPidEnvVar);
  if (!pid_string || !*pid_string) {
    return;  // not a relaunch -- nothing to wait on.
  }
  pid_t pid = static_cast<pid_t>(std::atoi(pid_string));
  // Consume the handoff so a later, unrelated Relaunch() done by *this*
  // process (which re-sets the var to its own pid) can't be misread as
  // still referring to the original one.
  unsetenv(kRelaunchFromPidEnvVar);
  if (pid <= 0) {
    return;
  }

  // The old instance is our parent (fork()+execv() in Relaunch() above), not
  // our child, so waitpid() can't be used here -- poll for its exit via
  // kill(pid, 0) instead. Bounded: a hung old instance shouldn't block this
  // one's startup forever.
  constexpr auto kTimeout = std::chrono::milliseconds(5000);
  constexpr auto kPollInterval = std::chrono::milliseconds(50);
  auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (kill(pid, 0) != 0 && errno == ESRCH) {
      return;  // exited
    }
    std::this_thread::sleep_for(kPollInterval);
  }
#endif
}

bool OpenFolder(const std::filesystem::path& path) {
#if REX_PLATFORM_MAC
  const char* opener = "open";
#else
  const char* opener = "xdg-open";
#endif
  pid_t pid = fork();
  if (pid < 0) {
    REXLOG_ERROR("OpenFolder: fork failed");
    return false;
  }
  if (pid == 0) {
    execlp(opener, opener, path.c_str(), static_cast<char*>(nullptr));
    _exit(127);  // execlp only returns on failure.
  }
  // Reap immediately rather than leaving a zombie -- this is a fire-and-
  // forget launch (the opener re-execs/daemonizes the actual file manager),
  // so there's nothing useful to wait for beyond "did the launch itself
  // succeed."
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace rex::platform::process
