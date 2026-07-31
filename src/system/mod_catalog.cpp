/**
 * @file        system/mod_catalog.cpp
 * @brief       Public mod catalog client implementation. See mod_catalog.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_catalog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

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

REXCVAR_DEFINE_STRING(mod_catalog_project, "goopie-f3ef6", "Mods",
                      "Firestore project id serving the mod catalog (blank + mod_catalog_url "
                      "blank disables the catalog entirely)");
REXCVAR_DEFINE_STRING(mod_catalog_api_key, "AIzaSyCUbUOOC-Jkb51XmLFGJIzbHxaw-EUgZm0", "Mods",
                      "Firestore public web API key for the mod catalog");
REXCVAR_DEFINE_STRING(mod_catalog_url, "", "Mods",
                      "Full runQuery URL override; wins over mod_catalog_project/api_key when set "
                      "-- escape hatch for a non-Firestore/self-hosted backend");
REXCVAR_DEFINE_STRING(mod_catalog_games_collection, "games", "Mods",
                      "Firestore collection queried to resolve catalog_name -> gameId");
REXCVAR_DEFINE_STRING(mod_catalog_mods_collection, "mods", "Mods",
                      "Firestore collection queried for a resolved gameId's mods");

namespace rex::system {

namespace {

using nlohmann::json;

// Unwraps one Firestore typed value ({"stringValue": ...}, {"arrayValue":
// ...}, ...) into a plain string (best-effort; only the shapes CatalogMod
// needs are handled). Returns empty for anything unrecognized/absent.
std::string UnwrapString(const json& value) {
  if (value.contains("stringValue") && value["stringValue"].is_string()) {
    return value["stringValue"].get<std::string>();
  }
  return {};
}

std::vector<std::string> UnwrapStringArray(const json& value) {
  std::vector<std::string> out;
  if (!value.contains("arrayValue")) {
    return out;
  }
  const auto& array_value = value["arrayValue"];
  if (!array_value.contains("values") || !array_value["values"].is_array()) {
    return out;
  }
  for (const auto& element : array_value["values"]) {
    std::string s = UnwrapString(element);
    if (!s.empty()) {
      out.push_back(std::move(s));
    }
  }
  return out;
}

std::string FieldString(const json& fields, const char* key) {
  if (!fields.contains(key)) {
    return {};
  }
  return UnwrapString(fields[key]);
}

std::vector<std::string> FieldStringArray(const json& fields, const char* key) {
  if (!fields.contains(key)) {
    return {};
  }
  return UnwrapStringArray(fields[key]);
}

std::string LastPathSegment(const std::string& name) {
  auto pos = name.find_last_of('/');
  return pos == std::string::npos ? name : name.substr(pos + 1);
}

std::string StructuredQueryEqualsBody(std::string_view collection, std::string_view field,
                                      std::string_view value) {
  json query;
  query["structuredQuery"]["from"] = json::array({{{"collectionId", std::string(collection)}}});
  query["structuredQuery"]["where"]["fieldFilter"]["field"]["fieldPath"] = std::string(field);
  query["structuredQuery"]["where"]["fieldFilter"]["op"] = "EQUAL";
  query["structuredQuery"]["where"]["fieldFilter"]["value"]["stringValue"] = std::string(value);
  query["structuredQuery"]["limit"] = 1;
  return query.dump();
}

std::string ModsForGameQueryBody(std::string_view collection, std::string_view game_id) {
  json query;
  json game_filter;
  game_filter["fieldFilter"]["field"]["fieldPath"] = "gameId";
  game_filter["fieldFilter"]["op"] = "EQUAL";
  game_filter["fieldFilter"]["value"]["stringValue"] = std::string(game_id);

  json status_filter;
  status_filter["fieldFilter"]["field"]["fieldPath"] = "status";
  status_filter["fieldFilter"]["op"] = "IN";
  status_filter["fieldFilter"]["value"]["arrayValue"]["values"] =
      json::array({{{"stringValue", "approved"}}, {{"stringValue", "featured"}}});

  query["structuredQuery"]["from"] = json::array({{{"collectionId", std::string(collection)}}});
  query["structuredQuery"]["where"]["compositeFilter"]["op"] = "AND";
  query["structuredQuery"]["where"]["compositeFilter"]["filters"] =
      json::array({game_filter, status_filter});
  return query.dump();
}

// Best-effort read of an installed mod's `version` key, for comparing
// against a `requires` entry's minimum-version pin; empty on any failure
// (not installed, missing/malformed mod.toml, no version key). Mirrors
// ModState's own tolerance for an optional/missing manifest.
std::string ReadInstalledModVersion(const std::filesystem::path& mods_root, const std::string& id) {
  auto manifest_path = mods_root / id / "mod.toml";
  if (!std::filesystem::is_regular_file(manifest_path)) {
    return {};
  }
  try {
    auto table = toml::parse_file(manifest_path.string());
    return table["version"].value_or<std::string>("");
  } catch (const toml::parse_error&) {
    return {};
  }
}

}  // namespace

std::string ParseGameIdResponse(const std::string& json_body) {
  try {
    json parsed = json::parse(json_body);
    if (!parsed.is_array()) {
      return {};
    }
    for (const auto& row : parsed) {
      if (!row.contains("document")) {
        continue;  // no-match shape: [{"readTime": ...}]
      }
      std::string name = row["document"].value("name", "");
      if (!name.empty()) {
        return LastPathSegment(name);
      }
    }
  } catch (const json::exception&) {
    return {};
  }
  return {};
}

std::vector<CatalogMod> ParseModsResponse(const std::string& json_body) {
  std::vector<CatalogMod> mods;
  try {
    json parsed = json::parse(json_body);
    if (!parsed.is_array()) {
      return mods;
    }
    for (const auto& row : parsed) {
      if (!row.contains("document")) {
        continue;
      }
      const auto& doc = row["document"];
      if (!doc.contains("fields")) {
        continue;
      }
      const auto& fields = doc["fields"];

      CatalogMod mod;
      mod.mod_id = FieldString(fields, "modId");
      if (mod.mod_id.empty()) {
        std::string name = doc.value("name", "");
        mod.mod_id = LastPathSegment(name);
      }
      mod.name = FieldString(fields, "name");
      mod.author = FieldString(fields, "author");
      mod.description = FieldString(fields, "description");
      mod.version = FieldString(fields, "version");
      mod.asset_url = FieldString(fields, "assetUrl");
      mod.checksum = FieldString(fields, "checksum");
      mod.platforms = FieldStringArray(fields, "platform");
      mod.requires_mods = FieldStringArray(fields, "requires");
      mod.game_version = FieldString(fields, "gameVersion");
      mod.icon_url = FieldString(fields, "iconUrl");
      mod.status = FieldString(fields, "status");

      if (!mod.mod_id.empty()) {
        mods.push_back(std::move(mod));
      }
    }
  } catch (const json::exception& e) {
    REXSYS_WARN("ModCatalog: failed to parse catalog response: {}", e.what());
    return {};
  }

  // Featured first, then approved, then alphabetical -- see mod_manager
  // overlay's "All" tab ordering.
  std::stable_sort(mods.begin(), mods.end(), [](const CatalogMod& a, const CatalogMod& b) {
    bool a_featured = a.status == "featured";
    bool b_featured = b.status == "featured";
    if (a_featured != b_featured) {
      return a_featured;
    }
    return a.name < b.name;
  });
  return mods;
}

std::string ModCatalog::EffectiveQueryUrl() {
  std::string override_url = REXCVAR_GET(mod_catalog_url);
  if (!override_url.empty()) {
    return override_url;
  }
  std::string project = REXCVAR_GET(mod_catalog_project);
  std::string api_key = REXCVAR_GET(mod_catalog_api_key);
  if (project.empty()) {
    return {};
  }
  return "https://firestore.googleapis.com/v1/projects/" + project +
         "/databases/(default)/documents:runQuery?key=" + api_key;
}

ModCatalog::~ModCatalog() {
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
}

void ModCatalog::Refresh() {
  bool expected = false;
  if (!fetch_in_flight_.compare_exchange_strong(expected, true)) {
    return;  // already loading
  }
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();
  }

  auto* runtime = rex::Runtime::instance();
  std::string catalog_name = runtime ? runtime->catalog_name() : std::string();
  std::string query_url = EffectiveQueryUrl();
  std::string games_collection = REXCVAR_GET(mod_catalog_games_collection);
  std::string mods_collection = REXCVAR_GET(mod_catalog_mods_collection);

  if (catalog_name.empty() || query_url.empty()) {
    // Disabled by config: settle straight to kFailed, no request attempted.
    // Indistinguishable from a network failure to the overlay, by design.
    state_.store(CatalogState::kFailed, std::memory_order_release);
    fetch_in_flight_.store(false, std::memory_order_release);
    return;
  }

  state_.store(CatalogState::kLoading, std::memory_order_release);
  fetch_thread_ = std::thread([this, catalog_name, query_url, games_collection, mods_collection] {
    FetchWorker(catalog_name, query_url, games_collection, mods_collection);
    fetch_in_flight_.store(false, std::memory_order_release);
  });
}

void ModCatalog::FetchWorker(std::string catalog_name, std::string query_url,
                             std::string games_collection, std::string mods_collection) {
  auto game_query = StructuredQueryEqualsBody(games_collection, "recompName", catalog_name);
  auto game_response = rex::net::HttpPostJson(query_url, game_query);
  if (!game_response.ok()) {
    REXSYS_WARN("ModCatalog: game lookup failed: {}", game_response.error.empty()
                                                          ? std::to_string(game_response.status)
                                                          : game_response.error);
    state_.store(CatalogState::kFailed, std::memory_order_release);
    return;
  }

  std::string game_id = ParseGameIdResponse(game_response.body);
  if (game_id.empty()) {
    REXSYS_WARN("ModCatalog: no catalog game found for catalog_name '{}'", catalog_name);
    state_.store(CatalogState::kFailed, std::memory_order_release);
    return;
  }

  auto mods_query = ModsForGameQueryBody(mods_collection, game_id);
  auto mods_response = rex::net::HttpPostJson(query_url, mods_query);
  if (!mods_response.ok()) {
    REXSYS_WARN("ModCatalog: mods query failed: {}", mods_response.error.empty()
                                                         ? std::to_string(mods_response.status)
                                                         : mods_response.error);
    state_.store(CatalogState::kFailed, std::memory_order_release);
    return;
  }

  auto mods = ParseModsResponse(mods_response.body);
  {
    std::lock_guard<std::mutex> lock(mods_mutex_);
    mods_ = std::move(mods);
  }
  state_.store(CatalogState::kReady, std::memory_order_release);
}

std::vector<CatalogMod> ModCatalog::Snapshot() const {
  std::lock_guard<std::mutex> lock(mods_mutex_);
  return mods_;
}

void ModCatalog::InstallAsync(const CatalogMod& entry, const std::filesystem::path& mods_root) {
  bool expected = false;
  if (!install_in_flight_.compare_exchange_strong(expected, true)) {
    return;
  }
  if (install_thread_.joinable()) {
    install_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_ = CatalogInstallResult{};
    install_result_.in_progress = true;
  }
  install_thread_ = std::thread([this, entry, mods_root] {
    InstallWorker(entry, mods_root);
    install_in_flight_.store(false, std::memory_order_release);
  });
}

bool ModCatalog::InstallOneMod(const CatalogMod& entry, const std::filesystem::path& mods_root,
                               std::string& out_error, bool& out_staged) {
  out_staged = false;
  std::error_code ec;
  std::filesystem::create_directories(mods_root, ec);
  if (ec) {
    out_error = "failed to create mods folder: " + ec.message();
    return false;
  }

  auto temp_zip = mods_root / (".catalog-download-" + entry.mod_id + ".zip.tmp");
  std::string download_error;
  auto progress = [this](uint64_t downloaded, uint64_t total) {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.downloaded_bytes = downloaded;
    install_result_.total_bytes = total;
  };
  if (!rex::net::HttpDownloadToFile(entry.asset_url, temp_zip, progress, download_error)) {
    std::filesystem::remove(temp_zip, ec);
    out_error = "download failed: " + download_error;
    return false;
  }

  // Checksum verification: hard-refuse on mismatch, never extract.
  if (!entry.checksum.empty()) {
    std::ifstream file(temp_zip, std::ios::binary);
    std::vector<unsigned char> hash(picosha2::k_digest_size);
    picosha2::hash256(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(),
                      hash.begin(), hash.end());
    std::string actual = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
    file.close();
    std::string expected_lower = entry.checksum;
    std::transform(expected_lower.begin(), expected_lower.end(), expected_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected_lower) {
      std::filesystem::remove(temp_zip, ec);
      out_error = "checksum mismatch: expected " + entry.checksum + ", got " + actual +
                  " -- the release asset may have changed since this mod was approved. Refusing "
                  "to install.";
      return false;
    }
  }

  auto staging = mods_root / (".catalog-staging-" + entry.mod_id);
  std::filesystem::remove_all(staging, ec);
  std::string extract_error;
  if (!rex::filesystem::ExtractZip(temp_zip, staging, extract_error)) {
    std::filesystem::remove(temp_zip, ec);
    std::filesystem::remove_all(staging, ec);
    out_error = "extract failed: " + extract_error;
    return false;
  }
  std::filesystem::remove(temp_zip, ec);

  // Unwrap a single top-level directory if present, same convention as the
  // launcher's install_one_archive.
  std::filesystem::path content_root = staging;
  {
    std::vector<std::filesystem::directory_entry> top_level;
    for (auto& e : std::filesystem::directory_iterator(staging, ec)) {
      top_level.push_back(e);
    }
    if (top_level.size() == 1 && top_level[0].is_directory()) {
      content_root = top_level[0].path();
    }
  }

  auto dest = mods_root / entry.mod_id;
  bool updated = std::filesystem::exists(dest, ec);
  // Clear out the old install first, same as always -- but if that fails,
  // `dest` has a file Windows won't let go of (most likely a mod DLL this
  // very process still has mapped/open), so there's no way to replace it in
  // place. Stage the new content instead; ModState::ApplyPendingUpdates swaps
  // it onto `dest` at the next launch, before anything has a chance to open
  // it. A fresh install has nothing to clear, so it always writes straight
  // to dest.
  if (updated) {
    std::filesystem::remove_all(dest, ec);
    if (ec) {
      if (!ModState::StagePendingUpdate(mods_root, entry.mod_id, content_root, out_error)) {
        std::filesystem::remove_all(staging, ec);
        return false;
      }
      out_staged = true;
    }
  }
  if (!out_staged && !rex::filesystem::MoveOrCopyDirectory(content_root, dest, out_error)) {
    std::filesystem::remove_all(staging, ec);
    return false;
  }
  std::filesystem::remove_all(staging, ec);

  auto entries = ModState::LoadReconciled(mods_root);
  if (std::none_of(entries.begin(), entries.end(),
                   [&](const ModStateEntry& e) { return e.id == entry.mod_id; })) {
    entries.push_back(ModStateEntry{entry.mod_id, true});
  }
  ModState::Save(mods_root, entries);
  return true;
}

void ModCatalog::InstallWorker(CatalogMod entry, std::filesystem::path mods_root) {
  auto fail = [this](std::string message) {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.in_progress = false;
    install_result_.done = true;
    install_result_.ok = false;
    install_result_.message = std::move(message);
  };

  // Pull in the latest published version of each unmet `requires` dependency
  // first, mirroring the companion desktop launcher's install flow (see
  // GameMods.tsx's handleInstall) -- a player shouldn't have to hunt down and
  // install a mod's dependencies by hand. Only one level deep (a
  // dependency's own dependencies aren't resolved), matching the launcher.
  // Already-installed dependencies are skipped unless the requirement pins a
  // minimum version the installed copy doesn't meet.
  std::vector<std::string> installed_deps;
  {
    auto catalog_snapshot = Snapshot();
    auto find_catalog_entry = [&](const std::string& id) -> const CatalogMod* {
      for (const auto& mod : catalog_snapshot) {
        if (mod.mod_id == id) {
          return &mod;
        }
      }
      return nullptr;
    };

    auto installed_entries = ModState::LoadReconciled(mods_root);
    std::unordered_map<std::string, std::string> installed_versions;
    for (const auto& installed_entry : installed_entries) {
      installed_versions.emplace(installed_entry.id,
                                 ReadInstalledModVersion(mods_root, installed_entry.id));
    }

    for (const auto& req_string : entry.requires_mods) {
      ModRequirement req = ParseRequirement(req_string);
      if (req.name.empty() || req.name == entry.mod_id) {
        continue;
      }
      auto installed_it = installed_versions.find(req.name);
      if (installed_it != installed_versions.end() &&
          (req.min_version.empty() ||
           CompareVersionStrings(installed_it->second, req.min_version) >= 0)) {
        continue;  // already installed and satisfies the pin (if any)
      }
      const CatalogMod* req_mod = find_catalog_entry(req.name);
      if (!req_mod || req_mod->asset_url.empty()) {
        continue;  // nothing we can auto-install for this requirement
      }
      std::string dep_error;
      bool dep_staged = false;
      if (!InstallOneMod(*req_mod, mods_root, dep_error, dep_staged)) {
        fail("failed to install dependency \"" + req.name + "\": " + dep_error);
        return;
      }
      installed_deps.push_back(req.name);
    }
  }

  std::string error;
  bool staged = false;
  if (!InstallOneMod(entry, mods_root, error, staged)) {
    fail(std::move(error));
    return;
  }

  std::string message = (staged ? "Downloaded update for \"" : "Installed \"") + entry.mod_id +
                        "\"" + (entry.version.empty() ? "" : " (v" + entry.version + ")");
  if (staged) {
    message += " -- restart to apply";
  }
  if (!installed_deps.empty()) {
    message += " (+" + std::to_string(installed_deps.size()) +
               (installed_deps.size() == 1 ? " dependency" : " dependencies") + ")";
  }
  {
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.in_progress = false;
    install_result_.done = true;
    install_result_.ok = true;
    install_result_.staged = staged;
    install_result_.message = std::move(message);
  }
}

CatalogInstallResult ModCatalog::InstallSnapshot() const {
  std::lock_guard<std::mutex> lock(install_mutex_);
  return install_result_;
}

}  // namespace rex::system
