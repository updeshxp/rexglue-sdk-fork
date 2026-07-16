#include <rex/kernel/xam/apps/leaderboard_stats.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace rex::kernel::xam::apps {

using rex::system::LeaderboardColumnType;
using rex::system::LeaderboardColumnValue;
using rex::system::LeaderboardRow;

namespace {
// XUSER_STATS_SPEC::rgwColumnIds is fixed at 0x40 entries in the XDK header.
constexpr uint32_t kMaxColumnIds = 0x40;
// Row/column/view sizes below are not guesses: they're derived from the
// buffer-sizing arithmetic in the real XamUserCreateStatsEnumerator
// (xam.xex `1888PatchedDash` dump, addr 0x818c0150):
//   *pcbBuffer = 8 + NumSpecs*(16 + 48*NumRows) + sum(28*NumColumnIds*NumRows)
// i.e. 8-byte XUSER_STATS_RESULTS header, 16-byte XUSER_STATS_VIEW header,
// 48-byte XUSER_STATS_ROW, 28-byte XUSER_STATS_COLUMN. The *sub-field*
// offsets within those (gamertag placement etc.) are still a best-effort
// reconstruction, not confirmed byte-for-byte.
constexpr uint32_t kColumnStride = 28;  // ColumnId(2) + pad(6) + X_USER_DATA(20)
// XUSER_STATS_ROW, real XDK layout (8-byte aligned):
//   xuid(8)@0, dwRank(4)@8, pad(4)@12, i64Rating(8)@16,
//   szGamertag[16]@24, dwNumColumns(4)@40, pColumns(4)@44  => 48 bytes.
constexpr uint32_t kRowStride = 48;
// XUSER_STATS_VIEW: ViewId(4)@0, TotalViewRows(4)@4, NumRows(4)@8, pRows(4)@12.
constexpr uint32_t kViewHeaderStride = 16;
// The enumerate buffer as a whole is one XUSER_STATS_RESULTS: an 8-byte
// {dwNumViews, pViews} header (kStatsResultsHeaderStride, in the .h) followed
// by the views written here. Confirmed by the real xam.xex sizing arithmetic:
//   *pcbBuffer = 8 + NumSpecs*(16 + 48*NumRows) + 28*columns.
}  // namespace

std::vector<StatsSpec> ReadStatsSpecs(memory::Memory* memory, uint32_t specs_ptr,
                                      uint32_t specs_count) {
  std::vector<StatsSpec> specs;
  // Defensive cap: callers should already validate this, but a
  // parameter-order mismatch turning a garbage register value into
  // "specs_count" must not translate into an unbounded guest-memory walk.
  constexpr uint32_t kMaxSpecs = 32;
  if (!specs_ptr || !specs_count || specs_count > kMaxSpecs) {
    return specs;
  }

  // XUSER_STATS_SPEC { be<u32> ViewId; be<u32> NumColumnIds; be<u16> rgwColumnIds[0x40]; }
  constexpr uint32_t kSpecStride = 4 + 4 + kMaxColumnIds * 2;
  auto base = memory->TranslateVirtual(specs_ptr);
  for (uint32_t i = 0; i < specs_count; ++i) {
    uint8_t* entry = base + i * kSpecStride;
    StatsSpec spec;
    spec.view_id = memory::load_and_swap<uint32_t>(entry + 0);
    uint32_t num_columns = memory::load_and_swap<uint32_t>(entry + 4);
    num_columns = std::min(num_columns, kMaxColumnIds);
    for (uint32_t c = 0; c < num_columns; ++c) {
      spec.column_ids.push_back(memory::load_and_swap<uint16_t>(entry + 8 + c * 2));
    }
    specs.push_back(std::move(spec));
  }
  return specs;
}

uint32_t ComputeStatsViewSize(const std::vector<LeaderboardRow>& rows) {
  uint32_t needed = kViewHeaderStride + static_cast<uint32_t>(rows.size()) * kRowStride;
  for (const auto& row : rows) {
    needed += static_cast<uint32_t>(row.columns.size()) * kColumnStride;
  }
  return needed;
}

uint32_t WriteStatsView(memory::Memory* memory, uint32_t dest_ptr, uint32_t remaining_bytes,
                        uint32_t title_id, const StatsSpec& spec,
                        const std::vector<LeaderboardRow>& rows) {
  uint32_t needed = ComputeStatsViewSize(rows);
  if (needed > remaining_bytes) {
    return 0;
  }

  // Writes a single XUSER_STATS_VIEW (+ its rows/columns) at dest_ptr. The
  // enclosing XUSER_STATS_RESULTS header is written by the caller (the
  // enumerator's WriteItems or the XGI XUserReadStats handler), because that
  // header spans potentially multiple views and its pViews pointer must point
  // at the first of them.
  uint32_t rows_ptr = dest_ptr + kViewHeaderStride;

  // XUSER_STATS_VIEW: ViewId, TotalViewRows, NumRows, pRows (Xenia/XDK order).
  // The real consumer sub_82584B88 reads pViews->NumRows (view+8), so view+8
  // must be a count and view+12 the row pointer.
  uint8_t* view = memory->TranslateVirtual(dest_ptr);
  memory::store_and_swap<uint32_t>(view + 0, spec.view_id);
  memory::store_and_swap<uint32_t>(view + 4, static_cast<uint32_t>(rows.size()));
  memory::store_and_swap<uint32_t>(view + 8, static_cast<uint32_t>(rows.size()));
  memory::store_and_swap<uint32_t>(view + 12, rows.empty() ? 0 : rows_ptr);

  uint32_t columns_ptr = rows_ptr + static_cast<uint32_t>(rows.size()) * kRowStride;
  for (const auto& row : rows) {
    uint8_t* row_out = memory->TranslateVirtual(rows_ptr);
    std::memset(row_out, 0, kRowStride);
    memory::store_and_swap<uint64_t>(row_out + 0, row.xuid);
    memory::store_and_swap<uint32_t>(row_out + 8, row.rank);
    // +12: pad. +16: i64Rating -- the score/time the game reads and displays
    // for this row (default.xex sub_825C1E98 reads *(row+16), never a column).
    memory::store_and_swap<int64_t>(row_out + 16, row.rating);
    {
      std::string tag = row.gamertag.substr(0, 15);
      std::memcpy(row_out + 24, tag.data(), tag.size());
    }
    memory::store_and_swap<uint32_t>(row_out + 40, static_cast<uint32_t>(row.columns.size()));
    memory::store_and_swap<uint32_t>(row_out + 44, row.columns.empty() ? 0 : columns_ptr);

    uint8_t* column_out = memory->TranslateVirtual(columns_ptr);
    for (const auto& column : row.columns) {
      memory::store_and_swap<uint16_t>(column_out + 0, column.column_id);
      std::memset(column_out + 2, 0, 6);
      // X_USER_DATA at column_out+8: type byte @+0, value @+12 (20 bytes total).
      uint8_t* user_data = column_out + 8;
      std::memset(user_data, 0, 20);
      user_data[0] = static_cast<uint8_t>(column.type);
      uint8_t* value = user_data + 4;
      switch (column.type) {
        case LeaderboardColumnType::kDouble:
          memory::store_and_swap<double>(value, column.real);
          break;
        case LeaderboardColumnType::kFloat:
          memory::store_and_swap<float>(value, static_cast<float>(column.real));
          break;
        case LeaderboardColumnType::kInt64:
        case LeaderboardColumnType::kDateTime:
          memory::store_and_swap<int64_t>(value, column.number);
          break;
        case LeaderboardColumnType::kContext:
        case LeaderboardColumnType::kInt32:
        default:
          memory::store_and_swap<int32_t>(value, static_cast<int32_t>(column.number));
          break;
      }
      column_out += kColumnStride;
      columns_ptr += kColumnStride;
    }
    rows_ptr += kRowStride;
  }

  (void)title_id;
  return needed;
}

uint32_t XStatsEnumerator::WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                                      uint32_t* written_count) {
  if (done_) {
    return X_ERROR_NO_MORE_FILES;
  }
  uint32_t size = static_cast<uint32_t>(item_size());

  // The enumerate buffer as a whole is one XUSER_STATS_RESULTS: an 8-byte
  // {dwNumViews, pViews} header followed by our single XUSER_STATS_VIEW. The
  // real consumer (default.xex sub_82584B88) walks *(*(buffer+4)+8), i.e.
  // pViews->NumRows, so pViews must be a real guest pointer to the view.
  if (size < kStatsResultsHeaderStride) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  uint32_t view_ptr = buffer_ptr + kStatsResultsHeaderStride;
  uint8_t* results = memory()->TranslateVirtual(buffer_ptr);
  memory::store_and_swap<uint32_t>(results + 0, 1);         // dwNumViews
  memory::store_and_swap<uint32_t>(results + 4, view_ptr);  // pViews

  uint32_t written =
      WriteStatsView(memory(), view_ptr, size - kStatsResultsHeaderStride, title_id_, spec_, rows_);
  if (!written && (size - kStatsResultsHeaderStride)) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  done_ = true;
  if (written_count) {
    *written_count = 1;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xam::apps
