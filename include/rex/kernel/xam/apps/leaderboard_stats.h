/**
 * @file        rex/kernel/xam/apps/leaderboard_stats.h
 * @brief       Guest-memory marshalling shared by XamUserCreateStatsEnumerator
 *              (real kernel export, xam_user.cpp) and XGI XUserReadStats /
 *              XSessionWriteStats (msg 0x000B0021 / 0x000B0025).
 *
 * Top-level sizes (XUSER_STATS_RESULTS/VIEW/ROW/COLUMN) are derived from the
 * real xam.xex buffer-sizing arithmetic (see leaderboard_stats.cpp) and
 * should be trustworthy. Sub-field offsets within ROW/COLUMN are still a
 * best-effort reconstruction -- see LEADERBOARD_BACKEND_PLAN.md open item 2.
 * If real leaderboard rows come back malformed in-game, this is the first
 * place to revisit.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/leaderboard_manager.h>
#include <rex/system/xenumerator.h>

namespace rex::kernel::xam::apps {

// XUSER_STATS_RESULTS { be<u32> dwNumViews; be<u32> pViews; }
constexpr uint32_t kStatsResultsHeaderStride = 8;

// XUSER_STATS_SPEC, as read from guest memory (xenumerator.h shape).
struct StatsSpec {
  uint32_t view_id = 0;
  std::vector<uint16_t> column_ids;
};

std::vector<StatsSpec> ReadStatsSpecs(memory::Memory* memory, uint32_t specs_ptr,
                                      uint32_t specs_count);

// Exact byte size WriteStatsView() will need for this row set.
uint32_t ComputeStatsViewSize(const std::vector<rex::system::LeaderboardRow>& rows);

// Builds one XUSER_STATS_VIEW (+ rows + columns) for `spec` at `dest_ptr`,
// consuming up to `remaining_bytes`. Returns the number of bytes written, or
// 0 if it didn't fit.
uint32_t WriteStatsView(memory::Memory* memory, uint32_t dest_ptr, uint32_t remaining_bytes,
                        uint32_t title_id, const StatsSpec& spec,
                        const std::vector<rex::system::LeaderboardRow>& rows);

// One-shot enumerator: a single XamEnumerate() call returns the whole
// XUSER_STATS_VIEW blob (header + rows + columns); the next call reports no
// more items. Shared by XamUserCreateStatsEnumerator (xam_user.cpp).
class XStatsEnumerator : public rex::system::XEnumerator {
 public:
  XStatsEnumerator(rex::system::KernelState* kernel_state, uint32_t title_id, StatsSpec spec,
                   std::vector<rex::system::LeaderboardRow> rows)
      : XEnumerator(kernel_state, 1, kStatsResultsHeaderStride + ComputeStatsViewSize(rows)),
        title_id_(title_id),
        spec_(std::move(spec)),
        rows_(std::move(rows)) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override;

 private:
  uint32_t title_id_;
  StatsSpec spec_;
  std::vector<rex::system::LeaderboardRow> rows_;
  bool done_ = false;
};

}  // namespace rex::kernel::xam::apps
