/**
 * @file        system/mod_state_test.cpp
 * @brief       Unit tests for rex::system::ModState (mods.toml sidecar,
 *              reconcile, auto-sort, validate).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/mod_state.h>

using rex::system::ModInfo;
using rex::system::ModIssue;
using rex::system::ModState;
using rex::system::ModStateEntry;

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

ModInfo MakeInfo(std::string folder_name) {
  ModInfo info;
  info.folder_name = std::move(folder_name);
  info.display_name = info.folder_name;
  return info;
}

}  // namespace

TEST_CASE("ModState: Save then Load round-trips entries", "[mod_state]") {
  TempDirectory temp("rex_mod_state_roundtrip");
  std::vector<ModStateEntry> entries = {
      {"mod_a", true},
      {"mod_b", false},
      {"mod_c", true},
  };
  REQUIRE(ModState::Save(temp.path(), entries));

  auto loaded = ModState::Load(temp.path());
  REQUIRE(loaded.size() == 3);
  CHECK(loaded[0].id == "mod_a");
  CHECK(loaded[0].enabled == true);
  CHECK(loaded[1].id == "mod_b");
  CHECK(loaded[1].enabled == false);
  CHECK(loaded[2].id == "mod_c");
  CHECK(loaded[2].enabled == true);
}

TEST_CASE("ModState: Load on a missing file returns empty, not an error", "[mod_state]") {
  TempDirectory temp("rex_mod_state_missing");
  auto loaded = ModState::Load(temp.path());
  CHECK(loaded.empty());
}

TEST_CASE("ModState: Load on a malformed file returns empty, not a throw", "[mod_state]") {
  TempDirectory temp("rex_mod_state_malformed");
  std::ofstream(temp.path() / "mods.toml") << "not valid toml {{{";
  auto loaded = ModState::Load(temp.path());
  CHECK(loaded.empty());
}

TEST_CASE("ModState: Reconcile drops entries whose folder is gone", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"mod_a", true}, {"mod_gone", true}};
  auto reconciled = ModState::Reconcile(entries, {"mod_a"});
  REQUIRE(reconciled.size() == 1);
  CHECK(reconciled[0].id == "mod_a");
}

TEST_CASE("ModState: Reconcile appends newly-discovered folders as enabled, preserving order",
          "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"mod_a", false}};
  auto reconciled = ModState::Reconcile(entries, {"mod_a", "mod_new"});
  REQUIRE(reconciled.size() == 2);
  CHECK(reconciled[0].id == "mod_a");
  CHECK(reconciled[0].enabled == false);  // preserved
  CHECK(reconciled[1].id == "mod_new");
  CHECK(reconciled[1].enabled == true);
}

TEST_CASE("ModState: EnabledIdsInOrder omits disabled entries", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"a", true}, {"b", false}, {"c", true}};
  auto ids = ModState::EnabledIdsInOrder(entries);
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "a");
  CHECK(ids[1] == "c");
}

TEST_CASE("ModState: AutoSort moves a requires-dependency before its dependent", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"ui_color", true}, {"game_symbols", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto ui_color = MakeInfo("ui_color");
  ui_color.requires_mods.push_back({"game_symbols", ""});
  manifests.emplace("ui_color", ui_color);
  manifests.emplace("game_symbols", MakeInfo("game_symbols"));

  auto sorted = ModState::AutoSort(entries, manifests);
  REQUIRE(sorted.size() == 2);
  CHECK(sorted[0].id == "game_symbols");
  CHECK(sorted[1].id == "ui_color");
}

TEST_CASE("ModState: AutoSort pins disabled entries to their original slot", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"a", true}, {"disabled_mod", false}, {"b", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  manifests.emplace("a", MakeInfo("a"));
  manifests.emplace("b", MakeInfo("b"));

  auto sorted = ModState::AutoSort(entries, manifests);
  REQUIRE(sorted.size() == 3);
  CHECK(sorted[1].id == "disabled_mod");
  CHECK(sorted[1].enabled == false);
}

TEST_CASE("ModState: AutoSort breaks a load_after cycle without hanging", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"a", true}, {"b", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto a = MakeInfo("a");
  a.load_after_mods.push_back("b");
  auto b = MakeInfo("b");
  b.load_after_mods.push_back("a");
  manifests.emplace("a", a);
  manifests.emplace("b", b);

  auto sorted = ModState::AutoSort(entries, manifests);
  REQUIRE(sorted.size() == 2);
  // Cycle can't be fully resolved; falls back to original order rather than
  // hanging or crashing.
  CHECK(sorted[0].id == "a");
  CHECK(sorted[1].id == "b");
}

TEST_CASE("ModState: Validate reports an error for a missing requires dependency", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"ui_color", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto ui_color = MakeInfo("ui_color");
  ui_color.requires_mods.push_back({"game_symbols", ""});
  manifests.emplace("ui_color", ui_color);

  auto issues = ModState::Validate(entries, manifests, "", "windows-x64");
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].id == "ui_color");
  CHECK(issues[0].kind == ModIssue::Kind::kError);
}

TEST_CASE("ModState: Validate is clean when requires is satisfied and ordered", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"game_symbols", true}, {"ui_color", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto ui_color = MakeInfo("ui_color");
  ui_color.requires_mods.push_back({"game_symbols", ""});
  manifests.emplace("ui_color", ui_color);
  manifests.emplace("game_symbols", MakeInfo("game_symbols"));

  auto issues = ModState::Validate(entries, manifests, "", "windows-x64");
  CHECK(issues.empty());
}

TEST_CASE("ModState: Validate flags a code mod with no binary for this platform", "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"native_mod", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto native_mod = MakeInfo("native_mod");
  native_mod.code = "native_mod";
  native_mod.platforms = {"linux-x64"};
  manifests.emplace("native_mod", native_mod);

  auto issues = ModState::Validate(entries, manifests, "", "windows-x64");
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].kind == ModIssue::Kind::kError);
}

TEST_CASE("ModState: Validate is clean when the code mod ships this host's platform",
          "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"native_mod", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto native_mod = MakeInfo("native_mod");
  native_mod.code = "native_mod";
  native_mod.platforms = {"windows-x64", "linux-x64"};
  manifests.emplace("native_mod", native_mod);

  auto issues = ModState::Validate(entries, manifests, "", "windows-x64");
  CHECK(issues.empty());
}

TEST_CASE("ModState: Validate reports a conflict for both sides regardless of order",
          "[mod_state]") {
  std::vector<ModStateEntry> entries = {{"a", true}, {"b", true}};
  std::unordered_map<std::string, ModInfo> manifests;
  auto a = MakeInfo("a");
  a.conflicts_mods.push_back("b");
  manifests.emplace("a", a);
  manifests.emplace("b", MakeInfo("b"));

  auto issues = ModState::Validate(entries, manifests, "", "windows-x64");
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].kind == ModIssue::Kind::kError);
  CHECK(issues[1].kind == ModIssue::Kind::kError);
}
