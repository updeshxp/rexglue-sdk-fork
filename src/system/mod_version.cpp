/**
 * @file        system/mod_version.cpp
 * @brief       Shared comma-list/version helpers. See mod_version.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/mod_version.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace rex::system {

std::vector<std::string> SplitCommaList(std::string_view csv) {
  std::vector<std::string> result;
  std::istringstream ss{std::string(csv)};
  std::string token;
  while (std::getline(ss, token, ',')) {
    auto start = token.find_first_not_of(" \t");
    auto end = token.find_last_not_of(" \t");
    if (start == std::string::npos) {
      continue;
    }
    token = token.substr(start, end - start + 1);
    if (!token.empty()) {
      result.push_back(std::move(token));
    }
  }
  return result;
}

ModRequirement ParseRequirement(std::string_view entry) {
  ModRequirement req;
  auto op_pos = entry.find(">=");
  if (op_pos == std::string_view::npos) {
    req.name = std::string(entry);
    return req;
  }
  auto name_part = entry.substr(0, op_pos);
  auto name_end = name_part.find_last_not_of(" \t");
  req.name =
      std::string(name_part.substr(0, name_end == std::string_view::npos ? 0 : name_end + 1));

  auto version_part = entry.substr(op_pos + 2);
  auto version_start = version_part.find_first_not_of(" \t");
  if (version_start != std::string_view::npos) {
    req.min_version = std::string(version_part.substr(version_start));
  }
  return req;
}

std::string ParseGameVersionConstraint(std::string_view value) {
  auto start = value.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return "";
  }
  value = value.substr(start);
  if (value.substr(0, 2) == ">=") {
    value = value.substr(2);
    auto version_start = value.find_first_not_of(" \t");
    value =
        version_start == std::string_view::npos ? std::string_view() : value.substr(version_start);
  }
  auto end = value.find_last_not_of(" \t");
  return end == std::string_view::npos ? "" : std::string(value.substr(0, end + 1));
}

bool ParseVersionComponents(std::string_view version, std::vector<int>& out) {
  out.clear();
  std::string current;
  auto flush = [&]() -> bool {
    if (current.empty() || !std::all_of(current.begin(), current.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
      return false;
    }
    out.push_back(std::stoi(current));
    current.clear();
    return true;
  };
  for (char c : version) {
    if (c == '.') {
      if (!flush()) {
        return false;
      }
    } else {
      current += c;
    }
  }
  return flush();
}

int CompareVersions(const std::vector<int>& have, const std::vector<int>& want) {
  size_t n = std::max(have.size(), want.size());
  for (size_t i = 0; i < n; ++i) {
    int hv = i < have.size() ? have[i] : 0;
    int wv = i < want.size() ? want[i] : 0;
    if (hv != wv) {
      return hv - wv;
    }
  }
  return 0;
}

int CompareVersionStrings(std::string_view have, std::string_view want) {
  std::vector<int> have_parts;
  std::vector<int> want_parts;
  // A version that doesn't parse is treated as all-zero components rather
  // than rejected outright -- callers here (catalog update-available checks)
  // just want "newer/older/equal", not a hard validity gate.
  ParseVersionComponents(have, have_parts);
  ParseVersionComponents(want, want_parts);
  return CompareVersions(have_parts, want_parts);
}

}  // namespace rex::system
