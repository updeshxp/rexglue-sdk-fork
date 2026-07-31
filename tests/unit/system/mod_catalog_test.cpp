/**
 * @file        system/mod_catalog_test.cpp
 * @brief       Unit tests for rex::system::ParseModsResponse /
 *              ParseGameIdResponse against canned Firestore runQuery
 *              fixtures (docs/MODS_API.md's response shape).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/mod_catalog.h>

using rex::system::ParseGameIdResponse;
using rex::system::ParseModsResponse;

TEST_CASE("ModCatalog: ParseGameIdResponse extracts the last path segment of document.name",
          "[mod_catalog]") {
  std::string body = R"([{"document": {
    "name": "projects/p/databases/(default)/documents/games/abc-123-def",
    "fields": {"title": {"stringValue": "Some Game"}}}}])";
  CHECK(ParseGameIdResponse(body) == "abc-123-def");
}

TEST_CASE("ModCatalog: ParseGameIdResponse handles the no-match shape (readTime only)",
          "[mod_catalog]") {
  std::string body = R"([{"readTime": "2024-01-01T00:00:00Z"}])";
  CHECK(ParseGameIdResponse(body).empty());
}

TEST_CASE("ModCatalog: ParseGameIdResponse handles malformed JSON without throwing",
          "[mod_catalog]") {
  CHECK(ParseGameIdResponse("not json at all").empty());
}

TEST_CASE("ModCatalog: ParseModsResponse unwraps typed fields", "[mod_catalog]") {
  std::string body = R"([{"document": {
    "name": ".../mods/e86f637e-...__blackboard",
    "fields": {
      "modId":     {"stringValue": "blackboard"},
      "name":      {"stringValue": "Blackboard"},
      "author":    {"stringValue": "someone"},
      "version":   {"stringValue": "1.0.1"},
      "status":    {"stringValue": "approved"},
      "assetUrl":  {"stringValue": "https://example.com/blackboard.zip"},
      "checksum":  {"stringValue": "deadbeef"},
      "gameVersion": {"stringValue": "1.2.0"},
      "requires":  {"arrayValue": {}},
      "platform":  {"arrayValue": {"values": [
        {"stringValue": "windows-x64"}, {"stringValue": "linux-x64"}]}}
    }}}])";

  auto mods = ParseModsResponse(body);
  REQUIRE(mods.size() == 1);
  const auto& mod = mods[0];
  CHECK(mod.mod_id == "blackboard");
  CHECK(mod.name == "Blackboard");
  CHECK(mod.author == "someone");
  CHECK(mod.version == "1.0.1");
  CHECK(mod.status == "approved");
  CHECK(mod.asset_url == "https://example.com/blackboard.zip");
  CHECK(mod.checksum == "deadbeef");
  CHECK(mod.game_version == "1.2.0");
  REQUIRE(mod.platforms.size() == 2);
  CHECK(mod.platforms[0] == "windows-x64");
  CHECK(mod.requires_mods.empty());
}

TEST_CASE("ModCatalog: ParseModsResponse filters on presence of 'document', not array length",
          "[mod_catalog]") {
  // A no-match query still returns a one-element array -- must yield zero
  // parsed mods, not one bogus entry.
  std::string body = R"([{"readTime": "2024-01-01T00:00:00Z"}])";
  auto mods = ParseModsResponse(body);
  CHECK(mods.empty());
}

TEST_CASE("ModCatalog: ParseModsResponse sorts featured mods first, then alphabetically",
          "[mod_catalog]") {
  std::string body = R"([
    {"document": {"name": ".../mods/a", "fields": {
      "modId": {"stringValue": "zeta"}, "name": {"stringValue": "Zeta"},
      "status": {"stringValue": "approved"}}}},
    {"document": {"name": ".../mods/b", "fields": {
      "modId": {"stringValue": "alpha"}, "name": {"stringValue": "Alpha"},
      "status": {"stringValue": "featured"}}}},
    {"document": {"name": ".../mods/c", "fields": {
      "modId": {"stringValue": "beta"}, "name": {"stringValue": "Beta"},
      "status": {"stringValue": "approved"}}}}
  ])";

  auto mods = ParseModsResponse(body);
  REQUIRE(mods.size() == 3);
  CHECK(mods[0].mod_id == "alpha");  // featured, sorts first
  CHECK(mods[1].mod_id == "beta");   // approved, alphabetical
  CHECK(mods[2].mod_id == "zeta");
}

TEST_CASE("ModCatalog: ParseModsResponse never throws on malformed JSON", "[mod_catalog]") {
  CHECK(ParseModsResponse("{{{not json").empty());
}
