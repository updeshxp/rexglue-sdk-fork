/**
 * @file        system/leaderboard_manager.cpp
 * @brief       LeaderboardManager implementation.
 */

#include <rex/system/leaderboard_manager.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <toml++/toml.hpp>

namespace rex::system {

uint64_t LeaderboardManager::MakeKey(uint32_t title_id, uint32_t view_id) {
  return (static_cast<uint64_t>(title_id) << 32) | view_id;
}

void LeaderboardManager::SetStorePath(std::filesystem::path path) {
  std::lock_guard lock(mutex_);
  store_path_ = std::move(path);
  boards_.clear();
}

void LeaderboardManager::Load() {
  std::filesystem::path path;
  {
    std::lock_guard lock(mutex_);
    path = store_path_;
  }
  if (path.empty() || !std::filesystem::exists(path)) {
    REXSYS_WARN("Leaderboard store: no file at {} (path empty: {})", path.string(), path.empty());
    return;
  }

  try {
    auto table = toml::parse_file(path.string());
    const auto* boards = table["boards"].as_array();
    if (!boards) {
      return;
    }

    std::lock_guard lock(mutex_);
    boards_.clear();
    for (const auto& node : *boards) {
      const auto* board = node.as_table();
      if (!board) {
        continue;
      }
      uint32_t title_id = static_cast<uint32_t>((*board)["title_id"].value_or<int64_t>(0));
      uint32_t view_id = static_cast<uint32_t>((*board)["view_id"].value_or<int64_t>(0));
      const auto* rows = (*board)["rows"].as_array();
      if (!rows) {
        continue;
      }

      Board& dest = boards_[MakeKey(title_id, view_id)];
      for (const auto& row_node : *rows) {
        const auto* row_table = row_node.as_table();
        if (!row_table) {
          continue;
        }
        LeaderboardRow row;
        // XUIDs routinely exceed INT64_MAX (e.g. 0xB13E...) which TOML's signed
        // 64-bit integers can't represent, so they are written as a quoted hex
        // string. Accept both that and a legacy bare integer (seed files).
        if (auto xuid_str = (*row_table)["xuid"].value<std::string>()) {
          row.xuid = std::strtoull(xuid_str->c_str(), nullptr, 16);
        } else {
          row.xuid = static_cast<uint64_t>((*row_table)["xuid"].value_or<int64_t>(0));
        }
        row.gamertag = (*row_table)["gamertag"].value_or<std::string>("");
        if (const auto* columns = (*row_table)["columns"].as_array()) {
          for (const auto& col_node : *columns) {
            const auto* col_table = col_node.as_table();
            if (!col_table) {
              continue;
            }
            LeaderboardColumnValue column;
            column.column_id = static_cast<uint16_t>((*col_table)["id"].value_or<int64_t>(0));
            column.type =
                static_cast<LeaderboardColumnType>((*col_table)["type"].value_or<int64_t>(1));
            switch (column.type) {
              case LeaderboardColumnType::kDouble:
              case LeaderboardColumnType::kFloat:
                column.real = (*col_table)["value"].value_or<double>(0.0);
                break;
              case LeaderboardColumnType::kWString:
                column.text =
                    rex::string::to_utf16((*col_table)["value"].value_or<std::string>(""));
                break;
              default:
                column.number = (*col_table)["value"].value_or<int64_t>(0);
                break;
            }
            row.columns.push_back(std::move(column));
          }
        }
        dest.rows.push_back(std::move(row));
      }
    }
    REXSYS_INFO("Loaded leaderboard store: {} board(s) from {}", boards_.size(), path.string());
  } catch (const std::exception& error) {
    REXSYS_WARN("Failed to parse leaderboard store {}: {}", path.string(), error.what());
  }
}

void LeaderboardManager::Save() const {
  std::filesystem::path path;
  std::unordered_map<uint64_t, Board> boards;
  {
    std::lock_guard lock(mutex_);
    path = store_path_;
    boards = boards_;
  }
  if (path.empty()) {
    return;
  }

  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  if (ec) {
    REXSYS_WARN("Leaderboard store: cannot create {}: {}", path.parent_path().string(),
                ec.message());
    return;
  }

  std::string content = "# Leaderboard store - managed by ReXGlue runtime\n\n";
  for (const auto& [key, board] : boards) {
    uint32_t title_id = static_cast<uint32_t>(key >> 32);
    uint32_t view_id = static_cast<uint32_t>(key & 0xFFFF'FFFF);
    content += "[[boards]]\n";
    content += fmt::format("title_id = {}\n", title_id);
    content += fmt::format("view_id = {}\n", view_id);
    for (const auto& row : board.rows) {
      content += "  [[boards.rows]]\n";
      // Quoted hex: XUIDs commonly exceed INT64_MAX and a bare decimal makes
      // the whole store unparseable on reload (TOML ints are signed 64-bit).
      content += fmt::format("  xuid = \"{:016x}\"\n", row.xuid);
      content += fmt::format("  gamertag = \"{}\"\n", row.gamertag);
      for (const auto& column : row.columns) {
        content += "    [[boards.rows.columns]]\n";
        content += fmt::format("    id = {}\n", column.column_id);
        content += fmt::format("    type = {}\n", static_cast<uint32_t>(column.type));
        switch (column.type) {
          case LeaderboardColumnType::kDouble:
          case LeaderboardColumnType::kFloat:
            content += fmt::format("    value = {}\n", column.real);
            break;
          case LeaderboardColumnType::kWString:
            content += fmt::format("    value = \"{}\"\n", rex::string::to_utf8(column.text));
            break;
          default:
            content += fmt::format("    value = {}\n", column.number);
            break;
        }
      }
    }
    content += "\n";
  }

  std::filesystem::path temporary_path = path;
  temporary_path += ".tmp";
  {
    std::ofstream file(temporary_path, std::ios::binary);
    if (!file) {
      REXSYS_WARN("Leaderboard store: cannot write {}", temporary_path.string());
      return;
    }
    file << content;
  }

  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(temporary_path, path, ec);
  if (ec) {
    REXSYS_WARN("Leaderboard store: rename failed: {}", ec.message());
  }
}

std::vector<LeaderboardRow> LeaderboardManager::GetRows(uint32_t title_id, uint32_t view_id,
                                                        uint32_t rank_column_id,
                                                        uint32_t max_rows) const {
  std::vector<LeaderboardRow> rows;
  {
    std::lock_guard lock(mutex_);
    auto it = boards_.find(MakeKey(title_id, view_id));
    if (it == boards_.end()) {
      return rows;
    }
    rows = it->second.rows;
  }

  auto rank_value = [rank_column_id](const LeaderboardRow& row) -> int64_t {
    // Prefer the requested rank column, but the read path passes
    // rank_column_id==0 (it asks for no specific columns) while a submitted row
    // may carry only its own property id (e.g. 2). Fall back to the row's first
    // column so single-column written rows still surface their value instead of
    // ranking/displaying as 0.
    const LeaderboardColumnValue* chosen = nullptr;
    for (const auto& column : row.columns) {
      if (column.column_id == rank_column_id) {
        chosen = &column;
        break;
      }
    }
    if (!chosen && !row.columns.empty()) {
      chosen = &row.columns.front();
    }
    if (!chosen) {
      return 0;
    }
    return chosen->type == LeaderboardColumnType::kDouble ||
                   chosen->type == LeaderboardColumnType::kFloat
               ? static_cast<int64_t>(chosen->real)
               : chosen->number;
  };

  std::stable_sort(rows.begin(), rows.end(), [&](const LeaderboardRow& a, const LeaderboardRow& b) {
    return rank_value(a) > rank_value(b);
  });

  if (rows.size() > max_rows) {
    rows.resize(max_rows);
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    rows[i].rank = static_cast<uint32_t>(i + 1);
    // The game displays i64Rating (row+16), not a column, as the score/time.
    // Use the value we ranked by so the displayed number matches the ordering.
    rows[i].rating = rank_value(rows[i]);
  }
  return rows;
}

void LeaderboardManager::SubmitRow(uint32_t title_id, uint32_t view_id, uint64_t xuid,
                                   const std::string& gamertag,
                                   std::vector<LeaderboardColumnValue> columns) {
  {
    std::lock_guard lock(mutex_);
    Board& board = boards_[MakeKey(title_id, view_id)];
    auto it = std::find_if(board.rows.begin(), board.rows.end(),
                           [&](const LeaderboardRow& row) { return row.xuid == xuid; });
    if (it != board.rows.end()) {
      it->gamertag = gamertag;
      it->columns = std::move(columns);
    } else {
      LeaderboardRow row;
      row.xuid = xuid;
      row.gamertag = gamertag;
      row.columns = std::move(columns);
      board.rows.push_back(std::move(row));
    }
  }
  Save();
}

}  // namespace rex::system
