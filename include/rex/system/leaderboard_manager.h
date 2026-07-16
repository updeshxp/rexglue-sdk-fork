/**
 * @file        rex/system/leaderboard_manager.h
 * @brief       Host-side leaderboard store, backing the XAM stats API.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::system {

// Mirrors X_USER_DATA (16 bytes, XDK user_data.h).
enum class LeaderboardColumnType : uint8_t {
  kContext = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kWString = 4,
  kFloat = 5,
  kBinary = 6,
  kDateTime = 7,
};

struct LeaderboardColumnValue {
  uint16_t column_id = 0;
  LeaderboardColumnType type = LeaderboardColumnType::kInt32;
  int64_t number = 0;   // used for kContext/kInt32/kInt64/kDateTime
  double real = 0.0;    // used for kDouble/kFloat
  std::u16string text;  // used for kWString
};

struct LeaderboardRow {
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t rank = 0;
  // The board's primary rating value (XUSER_STATS_ROW::i64Rating). This is what
  // the game actually displays as the score/time for a row -- it reads it
  // straight out of the row header, not from the columns array. GetRows()
  // fills it from the ranking column's value.
  int64_t rating = 0;
  std::vector<LeaderboardColumnValue> columns;
};

// Key: (title_id, view_id).
class LeaderboardManager {
 public:
  void SetStorePath(std::filesystem::path path);
  void Load();
  void Save() const;

  // Rows sorted by descending value of `rank_column_id` (or insertion order
  // if the column isn't present). Ties broken by earliest submission.
  std::vector<LeaderboardRow> GetRows(uint32_t title_id, uint32_t view_id, uint32_t rank_column_id,
                                      uint32_t max_rows) const;

  // Inserts or replaces this xuid's row for (title_id, view_id).
  void SubmitRow(uint32_t title_id, uint32_t view_id, uint64_t xuid, const std::string& gamertag,
                 std::vector<LeaderboardColumnValue> columns);

 private:
  struct Board {
    std::vector<LeaderboardRow> rows;  // keyed by xuid, unordered
  };

  static uint64_t MakeKey(uint32_t title_id, uint32_t view_id);

  mutable std::mutex mutex_;
  std::filesystem::path store_path_;
  std::unordered_map<uint64_t, Board> boards_;
};

}  // namespace rex::system
