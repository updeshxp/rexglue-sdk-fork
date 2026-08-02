/**
 * @file        core/platform/process_win.cpp
 * @brief       Windows implementation of rex::platform::process.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/process.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "../platform_win.h"

#include <rex/logging.h>

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <vector>

#include <shellapi.h>

namespace rex::platform::process {

namespace {
// CreateProcessW's default (lpEnvironment = nullptr) has the child inherit a
// copy of this process's environment block, so setting this here before
// spawning is a simple way to hand our own pid to the new instance without
// touching its command line.
constexpr wchar_t kRelaunchFromPidEnvVar[] = L"REX_RELAUNCH_FROM_PID";
}  // namespace

bool Relaunch() {
  // GetCommandLineW() returns the exact command line this process was
  // started with (module path + args), so no argv capture at startup is
  // needed. CreateProcessW requires a mutable buffer for lpCommandLine.
  std::wstring cmdline = GetCommandLineW();
  std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
  buffer.push_back(L'\0');

  wchar_t pid_string[16];
  swprintf_s(pid_string, L"%lu", GetCurrentProcessId());
  SetEnvironmentVariableW(kRelaunchFromPidEnvVar, pid_string);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                      &pi)) {
    REXLOG_ERROR("Relaunch: CreateProcessW failed (GetLastError={})", GetLastError());
    SetEnvironmentVariableW(kRelaunchFromPidEnvVar, nullptr);
    return false;
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void WaitForPreviousInstanceExit() {
  wchar_t pid_string[16];
  DWORD length = GetEnvironmentVariableW(kRelaunchFromPidEnvVar, pid_string, 16);
  if (length == 0 || length >= 16) {
    return;  // not a relaunch (or an implausibly large value) -- nothing to wait on.
  }
  // Consume the handoff so a later, unrelated Relaunch() done by *this*
  // process (which re-sets the var to its own pid) can't be misread as
  // still referring to the original one.
  SetEnvironmentVariableW(kRelaunchFromPidEnvVar, nullptr);

  DWORD pid = wcstoul(pid_string, nullptr, 10);
  if (pid == 0) {
    return;
  }
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) {
    return;  // already exited (or otherwise inaccessible) -- nothing to wait on.
  }
  // Bounded: a hung old instance shouldn't block this one's startup forever.
  WaitForSingleObject(process, 5000);
  CloseHandle(process);
}

bool OpenFolder(const std::filesystem::path& path) {
  // ShellExecuteW's return value is a HINSTANCE by API signature, but per
  // Win32 docs only the numeric value matters: > 32 means success.
  auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    REXLOG_ERROR("OpenFolder: ShellExecuteW failed (result={})", result);
    return false;
  }
  return true;
}

}  // namespace rex::platform::process
