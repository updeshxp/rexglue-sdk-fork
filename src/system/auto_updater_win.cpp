/**
 * @file        system/auto_updater_win.cpp
 * @brief       Windows implementation of AutoUpdater::ApplyAndRestart. See
 *              auto_updater.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/auto_updater.h>

#include <fstream>
#include <string>
#include <vector>

#include <Windows.h>

#include <rex/logging.h>

namespace rex::system {

bool AutoUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& executable_path) {
  if (!HasPendingSelfUpdate(install_root)) {
    return false;
  }
  auto staging = StagingRoot(install_root);

  std::error_code ec;
  std::vector<std::filesystem::path> staged_entries;
  for (auto& entry : std::filesystem::directory_iterator(staging, ec)) {
    staged_entries.push_back(entry.path());
  }
  if (staged_entries.empty()) {
    return false;
  }

  // This process's own executable and directly-linked DLLs live under
  // `install_root` and stay locked by the OS loader for as long as this
  // process runs -- including a relaunched instance of it, since it would
  // still be executing from that same locked file at the moment it tried to
  // replace it (see auto_updater.h's @remarks). A detached batch script
  // outside `install_root` has no such problem: it waits for this pid to
  // actually exit before touching anything.
  wchar_t temp_dir[MAX_PATH];
  DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
  if (temp_len == 0 || temp_len >= MAX_PATH) {
    REXSYS_WARN("AutoUpdater: GetTempPathW failed");
    return false;
  }
  DWORD pid = GetCurrentProcessId();
  std::filesystem::path script_path =
      std::filesystem::path(temp_dir) / (L"rex_autoupdate_" + std::to_wstring(pid) + L".bat");
  std::filesystem::path log_path = std::filesystem::path(temp_dir) / L"rex_autoupdate.log";

  std::wofstream script(script_path, std::ios::binary | std::ios::trunc);
  if (!script) {
    REXSYS_WARN("AutoUpdater: failed to create update helper script");
    return false;
  }
  script << L"@echo off\r\n";
  script << L"set \"REXLOG=" << log_path.wstring() << L"\"\r\n";
  // The subroutine has to live above the main flow so that the self-delete
  // can stay the physically last line of the file (see its comment below).
  script << L"goto rexmain\r\n";
  script << L"\r\n";
  // :rexapply <staged> <dest> -- clear whatever is at <dest> and move
  // <staged> onto it, retrying for a while. Even after the game's pid is
  // gone, its files can stay transiently locked (delayed handle release,
  // antivirus real-time scan re-opening the exe the instant it stops being
  // mapped), which is exactly what bit `nocturnerecomp.exe` while the
  // smaller data files moved fine. This mirrors the retry/backoff that
  // `MoveOrCopyDirectory` already does in-process.
  script << L":rexapply\r\n";
  script << L"set _rextry=0\r\n";
  script << L":rexapply_loop\r\n";
  script << L"rmdir /s /q %~2 >>\"%REXLOG%\" 2>&1\r\n";
  script << L"del /f /q %~2 >>\"%REXLOG%\" 2>&1\r\n";
  script << L"move /y %~1 %~2 >>\"%REXLOG%\" 2>&1\r\n";
  script << L"if not exist %~1 (\r\n";
  script << L"  echo [ok] %~2 >>\"%REXLOG%\"\r\n";
  script << L"  exit /b 0\r\n";
  script << L")\r\n";
  script << L"set /a _rextry+=1\r\n";
  script << L"echo [retry %_rextry%] %~2 >>\"%REXLOG%\"\r\n";
  script << L"if %_rextry% GEQ 20 (\r\n";
  script << L"  echo [FAILED] %~2 >>\"%REXLOG%\"\r\n";
  script << L"  exit /b 1\r\n";
  script << L")\r\n";
  script << L"timeout /t 1 /nobreak >NUL\r\n";
  script << L"goto rexapply_loop\r\n";
  script << L"\r\n";
  script << L":rexmain\r\n";
  script << L"echo ==== rex auto-update helper, pid " << pid << L" ==== >>\"%REXLOG%\"\r\n";
  script << L"echo %DATE% %TIME% waiting for pid " << pid << L" >>\"%REXLOG%\"\r\n";
  script << L":wait\r\n";
  script << L"tasklist /fi \"PID eq " << pid << L"\" 2>NUL | find \"" << pid << L"\" >NUL\r\n";
  script << L"if not errorlevel 1 (\r\n";
  script << L"  timeout /t 1 /nobreak >NUL\r\n";
  script << L"  goto wait\r\n";
  script << L")\r\n";
  script << L"echo %DATE% %TIME% pid gone, applying >>\"%REXLOG%\"\r\n";
  for (auto& staged_entry : staged_entries) {
    auto dest = install_root / staged_entry.filename();
    script << L"call :rexapply \"" << staged_entry.wstring() << L"\" \"" << dest.wstring()
           << L"\"\r\n";
  }
  script << L"rmdir /s /q \"" << staging.wstring() << L"\" >>\"%REXLOG%\" 2>&1\r\n";
  script << L"echo %DATE% %TIME% relaunching >>\"%REXLOG%\"\r\n";
  script << L"start \"\" /D \"" << install_root.wstring() << L"\" \"" << executable_path.wstring()
         << L"\"\r\n";
  // Self-delete via its own detached `start`, rather than a plain trailing
  // `del "%~f0"`: chaining a self-delete directly after the `start` above
  // reads flaky in practice under CREATE_NO_WINDOW (cmd.exe sometimes loses
  // track of the batch file's read position right after `start` returns,
  // logging "Impossibile trovare il file batch"/"Cannot find batch file" and
  // never running the delete). Routing it through one more `start` sidesteps
  // that entirely; the temp file merely being left behind on the rare
  // failure is harmless. Keep this the last line of the file so cmd.exe
  // never needs to seek past it.
  script << L"start \"\" /B cmd /c del \"" << script_path.wstring() << L"\"\r\n";
  script.close();

  std::wstring cmdline = L"cmd.exe /c \"" + script_path.wstring() + L"\"";
  std::vector<wchar_t> buffer(cmdline.begin(), cmdline.end());
  buffer.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // CREATE_NO_WINDOW (a real, just-hidden console) rather than
  // DETACHED_PROCESS (no console at all) -- the script uses `timeout` and
  // `start`, both of which are console-aware and behave inconsistently
  // without one.
  if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    REXSYS_WARN("AutoUpdater: failed to spawn update helper (GetLastError={})", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace rex::system
