/**
 * @file        system/auto_updater_posix.cpp
 * @brief       POSIX (Linux) implementation of AutoUpdater::ApplyAndRestart.
 *              See auto_updater.h.
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

#include <unistd.h>

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

  // Same reasoning as the Windows implementation's file-level comment: this
  // process's own executable and .so's live under `install_root` and stay
  // open/mapped for as long as it runs, including a relaunched instance of
  // it -- a detached shell script outside `install_root` waits for this pid
  // to actually exit before touching anything.
  auto temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    REXSYS_WARN("AutoUpdater: no usable temp directory ({})", ec.message());
    return false;
  }
  pid_t pid = getpid();
  auto script_path = temp_dir / ("rex_autoupdate_" + std::to_string(pid) + ".sh");
  auto log_path = temp_dir / "rex_autoupdate.log";
  std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
  if (!script) {
    REXSYS_WARN("AutoUpdater: failed to create update helper script");
    return false;
  }
  script << "#!/bin/sh\n";
  script << "REXLOG=\"" << log_path.string() << "\"\n";
  script << "echo \"==== rex auto-update helper, pid " << pid << " ====\" >>\"$REXLOG\"\n";
  script << "date >>\"$REXLOG\"\n";
  // Wait for the old process to actually go away. `kill -0` alone isn't
  // enough: it also succeeds on a *zombie*, and this helper is not the
  // game's parent, so it can't reap it -- if whatever launched the game is
  // slow to wait() on it, a plain `kill -0` loop would spin forever and the
  // update would silently never apply. Treat a zombie as gone by checking
  // the process state, and cap the wait regardless so we always make
  // progress.
  script << "rexwait=0\n";
  script << "while kill -0 " << pid << " 2>/dev/null; do\n";
  script << "  rexstate=$(ps -o stat= -p " << pid << " 2>/dev/null | head -n1)\n";
  script << "  case \"$rexstate\" in *Z*) break;; esac\n";
  script << "  rexwait=$((rexwait+1))\n";
  script << "  if [ \"$rexwait\" -ge 60 ]; then\n";
  script << "    echo \"[FAILED] pid " << pid << " still alive after ${rexwait}s\" >>\"$REXLOG\"\n";
  script << "    exit 1\n";
  script << "  fi\n";
  script << "  sleep 1\n";
  script << "done\n";
  script << "echo \"pid gone after ${rexwait}s, applying\" >>\"$REXLOG\"\n";
  for (auto& staged_entry : staged_entries) {
    auto dest = install_root / staged_entry.filename();
    script << "rm -rf \"" << dest.string() << "\" >>\"$REXLOG\" 2>&1\n";
    script << "if mv \"" << staged_entry.string() << "\" \"" << dest.string()
           << "\" >>\"$REXLOG\" 2>&1; then\n";
    script << "  echo \"[ok] " << dest.string() << "\" >>\"$REXLOG\"\n";
    script << "else\n";
    script << "  echo \"[FAILED] " << dest.string() << "\" >>\"$REXLOG\"\n";
    script << "fi\n";
  }
  script << "rmdir \"" << staging.string() << "\" 2>/dev/null\n";
  script << "chmod +x \"" << executable_path.string() << "\" 2>/dev/null\n";
  script << "echo relaunching >>\"$REXLOG\"\n";
  // Relaunch from the install root, matching the Windows path's
  // `start /D "<install_root>"` -- without this the new process would just
  // inherit whatever cwd the old one happened to have.
  script << "cd \"" << install_root.string() << "\" || exit 1\n";
  script << "\"" << executable_path.string() << "\" &\n";
  // Safe to unlink while /bin/sh is still executing this file: sh holds an
  // open fd, and the inode outlives the directory entry.
  script << "rm -- \"$0\"\n";
  script.close();
  std::filesystem::permissions(
      script_path,
      std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      ec);

  pid_t child = fork();
  if (child < 0) {
    REXSYS_WARN("AutoUpdater: fork failed for update helper");
    return false;
  }
  if (child == 0) {
    setsid();  // detach into its own session so it survives this process exiting.
    execl("/bin/sh", "sh", script_path.c_str(), static_cast<char*>(nullptr));
    _exit(127);  // execl only returns on failure.
  }
  return true;
}

}  // namespace rex::system
