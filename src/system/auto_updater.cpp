/**
 * @file        system/auto_updater.cpp
 * @brief       Self-update client implementation. See auto_updater.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/auto_updater.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/net/http.h>
#include <rex/runtime.h>
#include <rex/system/mod_state.h>
#include <rex/system/mod_version.h>

REXCVAR_DEFINE_BOOL(auto_update_enabled, true, "Updates",
                    "Enables automatic update checks/downloads against the project's configured "
                    "GitHub Releases repo (set false to disable all auto-update network activity)");

namespace rex::system {

namespace {

using nlohmann::json;

std::string ReplaceAll(std::string s, std::string_view from, std::string_view to) {
  if (from.empty()) {
    return s;
  }
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

// Recursively overlays `source` (the user's existing TOML) onto `target`
// (the freshly shipped archive's TOML), so the shipped file's shape/new keys
// win by default but every key the user already had a value for keeps that
// value. Keys the user had that the new file doesn't define at all are
// preserved too, defensively (e.g. a cvar dropped from the default file but
// still meaningful to an already-running config).
void MergeTomlOnto(toml::table& target, const toml::table& source) {
  for (const auto& [key, value] : source) {
    if (value.is_table() && target.contains(key) && target[key].is_table()) {
      MergeTomlOnto(*target[key].as_table(), *value.as_table());
    } else {
      target.insert_or_assign(key, value);
    }
  }
}

// Merges every top-level *.toml the staged content ships with against the
// same-named file already present at `install_root` (if any), so
// user-configured values in the currently-installed TOML survive an update
// that also happens to ship a new default TOML. Best-effort: a parse
// failure on either side just leaves the shipped file untouched.
void MergeShippedToml(const std::filesystem::path& install_root,
                      const std::filesystem::path& content_root) {
  std::error_code ec;
  for (auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".toml") {
      continue;
    }
    auto existing_path = install_root / entry.path().filename();
    if (!std::filesystem::is_regular_file(existing_path, ec)) {
      continue;  // nothing installed yet at this path -- shipped defaults stand as-is.
    }
    try {
      auto shipped = toml::parse_file(entry.path().string());
      auto existing = toml::parse_file(existing_path.string());
      MergeTomlOnto(shipped, existing);
      std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
      out << shipped;
    } catch (const toml::parse_error& e) {
      REXSYS_WARN("AutoUpdater: failed to merge {}: {}", entry.path().string(), e.what());
    }
  }
}

}  // namespace

std::string AutoUpdater::ExpandAssetFormat(const std::string& format, const std::string& tag) {
  std::string result = ReplaceAll(format, "{tag}", tag);
  result = ReplaceAll(result, "{platform}", ModState::HostPlatformId());
  return result;
}

AutoUpdater::~AutoUpdater() {
  if (check_thread_.joinable()) {
    check_thread_.join();
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void AutoUpdater::CheckAsync() {
  bool expected = false;
  if (!check_in_flight_.compare_exchange_strong(expected, true)) {
    return;
  }
  if (check_thread_.joinable()) {
    check_thread_.join();
  }

  auto* runtime = rex::Runtime::instance();
  bool enabled = REXCVAR_GET(auto_update_enabled);
  std::string repo = runtime ? runtime->update_repo() : std::string();
  std::string format = runtime ? runtime->update_asset_format() : std::string();
  std::string current_version = runtime ? runtime->game_version() : std::string();

  if (!enabled || repo.empty() || format.empty() || current_version.empty()) {
    // Disabled by config, or nothing to compare a fetched tag against:
    // settle straight to kFailed without a request, same convention as
    // ModCatalog::Refresh().
    state_.store(UpdateCheckState::kFailed, std::memory_order_release);
    check_in_flight_.store(false, std::memory_order_release);
    return;
  }

  state_.store(UpdateCheckState::kChecking, std::memory_order_release);
  check_thread_ = std::thread([this, repo, format, current_version] {
    CheckWorker(repo, format, current_version);
    check_in_flight_.store(false, std::memory_order_release);
  });
}

void AutoUpdater::CheckWorker(std::string repo, std::string format, std::string current_version) {
  std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
  auto response = rex::net::HttpGet(url);
  if (!response.ok()) {
    REXSYS_WARN("AutoUpdater: release lookup failed: {}",
                response.error.empty() ? std::to_string(response.status) : response.error);
    state_.store(UpdateCheckState::kFailed, std::memory_order_release);
    return;
  }

  try {
    json parsed = json::parse(response.body);
    std::string tag = parsed.value("tag_name", "");
    if (tag.empty() || !parsed.contains("assets") || !parsed["assets"].is_array()) {
      REXSYS_WARN("AutoUpdater: malformed release response (no tag_name/assets)");
      state_.store(UpdateCheckState::kFailed, std::memory_order_release);
      return;
    }

    std::string platform = ModState::HostPlatformId();
    std::string ext = platform.rfind("windows", 0) == 0 ? ".zip" : ".tar.gz";
    std::string expected_name = ExpandAssetFormat(format, tag) + ext;

    UpdateInfo info;
    for (const auto& asset : parsed["assets"]) {
      if (asset.value("name", "") != expected_name) {
        continue;
      }
      info.asset_name = expected_name;
      info.asset_url = asset.value("browser_download_url", "");
      std::string digest = asset.value("digest", "");
      constexpr std::string_view kSha256Prefix = "sha256:";
      if (digest.rfind(kSha256Prefix, 0) == 0) {
        info.sha256 = digest.substr(kSha256Prefix.size());
      }
      break;
    }
    if (info.asset_url.empty()) {
      REXSYS_WARN("AutoUpdater: release '{}' has no asset named '{}'", tag, expected_name);
      state_.store(UpdateCheckState::kFailed, std::memory_order_release);
      return;
    }

    info.tag = tag;
    info.version =
        (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) ? tag.substr(1) : tag;
    info.notes = parsed.value("body", "");

    if (CompareVersionStrings(current_version, info.version) >= 0) {
      state_.store(UpdateCheckState::kUpToDate, std::memory_order_release);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      available_ = info;
    }
    state_.store(UpdateCheckState::kUpdateAvailable, std::memory_order_release);
  } catch (const json::exception& e) {
    REXSYS_WARN("AutoUpdater: failed to parse release response: {}", e.what());
    state_.store(UpdateCheckState::kFailed, std::memory_order_release);
  }
}

std::optional<UpdateInfo> AutoUpdater::Available() const {
  std::lock_guard<std::mutex> lock(info_mutex_);
  return available_;
}

std::filesystem::path AutoUpdater::StagingRoot(const std::filesystem::path& install_root) {
  return install_root / ".pending-self-update";
}

bool AutoUpdater::HasPendingSelfUpdate(const std::filesystem::path& install_root) {
  std::error_code ec;
  auto staging = StagingRoot(install_root);
  if (!std::filesystem::is_directory(staging, ec)) {
    return false;
  }
  for (auto& entry : std::filesystem::directory_iterator(staging, ec)) {
    (void)entry;
    return true;
  }
  return false;
}

// ApplyAndRestart() is platform-specific -- see auto_updater_win.cpp /
// auto_updater_posix.cpp -- since a temp helper script is the only thing
// that can safely swap this process's own running executable/DLLs out from
// under it (see the file-level @remarks above).

void AutoUpdater::InstallAsync(const UpdateInfo& info, const std::filesystem::path& install_root) {
  bool expected = false;
  if (!install_in_flight_.compare_exchange_strong(expected, true)) {
    return;
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_ = UpdateInstallResult{};
    install_result_.in_progress = true;
  }
  install_thread_ = std::thread([this, info, install_root] {
    InstallWorker(info, install_root);
    install_in_flight_.store(false, std::memory_order_release);
  });
}

void AutoUpdater::InstallWorker(UpdateInfo info, std::filesystem::path install_root) {
  auto fail = [this](std::string message) {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.in_progress = false;
    install_result_.done = true;
    install_result_.ok = false;
    install_result_.message = std::move(message);
  };

  std::error_code ec;
  auto temp_root = install_root / ".auto-update-download";
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    fail("failed to create download folder: " + ec.message());
    return;
  }

  auto temp_archive =
      temp_root / ("download" + std::filesystem::path(info.asset_name).extension().string());
  // Handle the ".tar.gz" double extension: path::extension() above only
  // strips ".gz", so re-derive the real suffix straight off the asset name.
  bool is_tar_gz = info.asset_name.size() > 7 &&
                   info.asset_name.compare(info.asset_name.size() - 7, 7, ".tar.gz") == 0;
  temp_archive = temp_root / (is_tar_gz ? "download.tar.gz" : "download.zip");

  std::string download_error;
  auto progress = [this](uint64_t downloaded, uint64_t total) {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.downloaded_bytes = downloaded;
    install_result_.total_bytes = total;
  };
  if (!rex::net::HttpDownloadToFile(info.asset_url, temp_archive, progress, download_error)) {
    std::filesystem::remove_all(temp_root, ec);
    fail("download failed: " + download_error);
    return;
  }

  if (!info.sha256.empty()) {
    std::ifstream file(temp_archive, std::ios::binary);
    std::vector<unsigned char> hash(picosha2::k_digest_size);
    picosha2::hash256(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(),
                      hash.begin(), hash.end());
    std::string actual = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
    file.close();
    std::string expected_lower = info.sha256;
    std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected_lower) {
      std::filesystem::remove_all(temp_root, ec);
      fail("checksum mismatch: expected " + info.sha256 + ", got " + actual +
           " -- the release asset may have changed. Refusing to install.");
      return;
    }
  }

  auto extracted = temp_root / "extracted";
  std::filesystem::remove_all(extracted, ec);
  std::string extract_error;
  bool extracted_ok = is_tar_gz
                          ? rex::filesystem::ExtractTarGz(temp_archive, extracted, extract_error)
                          : rex::filesystem::ExtractZip(temp_archive, extracted, extract_error);
  if (!extracted_ok) {
    std::filesystem::remove_all(temp_root, ec);
    fail("extract failed: " + extract_error);
    return;
  }
  std::filesystem::remove(temp_archive, ec);

  // Unwrap a single top-level directory if the archive was packed that way,
  // same convention as ModCatalog::InstallOneMod.
  std::filesystem::path content_root = extracted;
  {
    std::vector<std::filesystem::directory_entry> top_level;
    for (auto& e : std::filesystem::directory_iterator(extracted, ec)) {
      top_level.push_back(e);
    }
    if (top_level.size() == 1 && top_level[0].is_directory()) {
      content_root = top_level[0].path();
    }
  }

  MergeShippedToml(install_root, content_root);

  auto staging = AutoUpdater::StagingRoot(install_root);
  std::filesystem::remove_all(staging, ec);
  std::string stage_error;
  if (!rex::filesystem::MoveOrCopyDirectory(content_root, staging, stage_error)) {
    std::filesystem::remove_all(temp_root, ec);
    fail("failed to stage extracted update: " + stage_error);
    return;
  }
  std::filesystem::remove_all(temp_root, ec);

  {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.in_progress = false;
    install_result_.done = true;
    install_result_.ok = true;
    install_result_.message = "Downloaded v" + info.version + " -- restart to apply";
  }
}

UpdateInstallResult AutoUpdater::InstallSnapshot() const {
  std::lock_guard<std::mutex> lock(install_mutex_);
  return install_result_;
}

}  // namespace rex::system
