/**
 * @file        mod_dependency_test.cpp
 * @brief       Unit tests for mod.toml requires/load_after/conflicts
 *              validation (Runtime::ValidateModDependencies).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/runtime.h>

using rex::X_STATUS;

namespace {

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "_" + std::to_string(suffix) + "_" + std::to_string(next_id++));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << content;
}

// Writes <mods_root>/<name>/mod.toml with the given optional dependency
// fields (empty string omits the key entirely, matching mod.toml's "absence
// means no constraint" convention).
void WriteMod(const std::filesystem::path& mods_root, std::string_view name,
              std::string_view requires_list = "", std::string_view load_after_list = "",
              std::string_view conflicts_list = "", std::string_view version = "",
              std::string_view game_version_constraint = "") {
  std::string toml = "name = \"" + std::string(name) + "\"\n";
  if (!version.empty()) {
    toml += "version = \"" + std::string(version) + "\"\n";
  }
  if (!requires_list.empty()) {
    toml += "requires = \"" + std::string(requires_list) + "\"\n";
  }
  if (!load_after_list.empty()) {
    toml += "load_after = \"" + std::string(load_after_list) + "\"\n";
  }
  if (!conflicts_list.empty()) {
    toml += "conflicts = \"" + std::string(conflicts_list) + "\"\n";
  }
  if (!game_version_constraint.empty()) {
    toml += "game_version = \"" + std::string(game_version_constraint) + "\"\n";
  }
  WriteFile(mods_root / name / "mod.toml", toml);
}

// Runs Runtime::Setup() with the given enabled_mods order against mods laid
// out under mods_root, in tool mode (skips GPU) so the test stays cheap.
// `host_game_version` feeds RuntimeConfig::game_version (empty = never set,
// same as a project that doesn't opt into this feature).
// Returns whether Setup() succeeded.
bool SetupWithMods(const std::filesystem::path& game_root, const std::filesystem::path& mods_root,
                   std::string_view enabled_mods_csv, std::string_view host_game_version = "") {
  REXCVAR_SET(mods_data_root, mods_root.string());
  REXCVAR_SET(enabled_mods, std::string(enabled_mods_csv));

  rex::Runtime runtime(game_root);
  rex::RuntimeConfig config;
  config.tool_mode = true;
  config.game_version = std::string(host_game_version);
  X_STATUS status = runtime.Setup(std::move(config));
  runtime.Shutdown();

  REXCVAR_SET(enabled_mods, std::string());
  REXCVAR_SET(mods_data_root, std::string());

  return status == X_STATUS_SUCCESS;
}

}  // namespace

TEST_CASE("mod dependency validation: requires satisfied in correct order", "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_ok");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols");

  CHECK(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE("mod dependency validation: requires fails when the dependency is missing",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_missing");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols");
  // game_symbols is never enabled.

  CHECK_FALSE(SetupWithMods(game_root, mods_root, "ui_color"));
}

TEST_CASE("mod dependency validation: requires fails when the dependency loads after",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_misorder");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols");

  // ui_color listed before game_symbols: wrong order.
  CHECK_FALSE(SetupWithMods(game_root, mods_root, "ui_color,game_symbols"));
}

TEST_CASE("mod dependency validation: conflicts fails regardless of order", "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_conflict");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", "", "", /*conflicts_list=*/"legacy_hack");
  WriteMod(mods_root, "legacy_hack");

  CHECK_FALSE(SetupWithMods(game_root, mods_root, "ui_color,legacy_hack"));
  CHECK_FALSE(SetupWithMods(game_root, mods_root, "legacy_hack,ui_color"));
}

TEST_CASE("mod dependency validation: load_after violation warns but still succeeds",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_load_after");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols");
  // ui_color should load after game_symbols, but is listed first: a warning,
  // not a hard failure.
  WriteMod(mods_root, "ui_color", "", /*load_after_list=*/"game_symbols");

  CHECK(SetupWithMods(game_root, mods_root, "ui_color,game_symbols"));
}

TEST_CASE("mod dependency validation: requires >= version satisfied by an exact match",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_version_exact");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols", "", "", "", /*version=*/"1.0.0");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols >= 1.0.0");

  CHECK(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE("mod dependency validation: requires >= version satisfied by a newer version",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_version_newer");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols", "", "", "", /*version=*/"1.2.0");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols >= 1.0.0");

  CHECK(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE("mod dependency validation: requires >= version fails when the dependency is older",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_version_older");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols", "", "", "", /*version=*/"0.9.0");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols >= 1.0.0");

  CHECK_FALSE(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE(
    "mod dependency validation: requires >= version is accepted when the dependency has no "
    "version to check (can't-verify, not a failure)",
    "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_version_missing");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols");  // no version key
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols >= 1.0.0");

  CHECK(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE(
    "mod dependency validation: requires >= a malformed constraint is accepted (can't-verify, "
    "not a failure)",
    "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_version_malformed");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "game_symbols", "", "", "", /*version=*/"1.0.0");
  WriteMod(mods_root, "ui_color", /*requires_list=*/"game_symbols >= not-a-version");

  CHECK(SetupWithMods(game_root, mods_root, "game_symbols,ui_color"));
}

TEST_CASE("mod dependency validation: game_version satisfied by an exact host match",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_game_version_exact");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", "", "", "", "", /*game_version_constraint=*/"1.2.0");

  CHECK(SetupWithMods(game_root, mods_root, "ui_color", /*host_game_version=*/"1.2.0"));
}

TEST_CASE("mod dependency validation: game_version satisfied by a newer host, '>=' form accepted",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_game_version_ge_form");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", "", "", "", "", /*game_version_constraint=*/">= 1.2.0");

  CHECK(SetupWithMods(game_root, mods_root, "ui_color", /*host_game_version=*/"1.3.0"));
}

TEST_CASE("mod dependency validation: game_version fails when the host is older",
          "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_game_version_older");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", "", "", "", "", /*game_version_constraint=*/"1.2.0");

  CHECK_FALSE(SetupWithMods(game_root, mods_root, "ui_color", /*host_game_version=*/"1.1.0"));
}

TEST_CASE(
    "mod dependency validation: game_version is accepted when the host never set a version "
    "(can't-verify, not a failure)",
    "[mod_dependency]") {
  TempDirectory temp("rex_mod_deps_game_version_unset");
  auto game_root = temp.path() / "game";
  std::filesystem::create_directories(game_root);
  auto mods_root = temp.path() / "mods";

  WriteMod(mods_root, "ui_color", "", "", "", "", /*game_version_constraint=*/"1.2.0");

  CHECK(SetupWithMods(game_root, mods_root, "ui_color", /*host_game_version=*/""));
}
