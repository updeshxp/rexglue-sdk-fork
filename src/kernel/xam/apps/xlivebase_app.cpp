/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <fmt/format.h>

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/leaderboard_stats.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // "CreateStatsEnumeration". Confirmed via IDA (default.xex,
      // sub_82813760): the caller packs 5 dwords -- view id, a constant 0,
      // row count, the guest address of its own local "handle" variable,
      // and a pointer to a single XUSER_STATS_SPEC (a1+296 on the
      // leaderboard-page object) -- via sub_82813718, then dispatches this
      // exact message. The row/column struct sizes in leaderboard_stats.h
      // are separately confirmed against the real xam.xex implementation.
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_rows = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t handle_out_ptr = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t spec_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG(
          "CreateStatsEnumeration(view={}, flags={}, rows={}, handleOut={:08X}, spec={:08X})",
          view_id, flags, num_rows, handle_out_ptr, spec_ptr);

      if (!handle_out_ptr || !num_rows) {
        return X_E_INVALIDARG;
      }

      auto specs = ReadStatsSpecs(memory_, spec_ptr, spec_ptr ? 1 : 0);
      StatsSpec spec;
      if (!specs.empty()) {
        spec = specs.front();
      } else {
        spec.view_id = view_id;
      }

      uint32_t rank_column_id = spec.column_ids.empty() ? 0 : spec.column_ids.front();
      auto rows = kernel_state_->leaderboards().GetRows(kernel_state_->title_id(), spec.view_id,
                                                        rank_column_id, num_rows);

      auto e = object_ref<XStatsEnumerator>(
          new XStatsEnumerator(kernel_state_, kernel_state_->title_id(), spec, std::move(rows)));
      auto result = e->Initialize(0, 0xFC, 0x00058020, 0x00058021, 0);
      if (XFAILED(result)) {
        return result;
      }

      // sub_82584928 (default.xex) doesn't treat the 4th packed dword as a
      // handle out-param: it passes it straight into a retry-allocator
      // (sub_82576950 -> sub_825E1600, an allocate-with-retry loop) as a
      // BYTE SIZE, then allocates that many bytes and later calls
      // XamEnumerate with a handle read back from *(spec_ptr) -- i.e. the
      // guest slot that held the input XUSER_STATS_SPEC pointer doubles as
      // the handle out-param once this call returns. So: write the required
      // buffer size to the 4th slot, and the real handle into *specs_ptr.
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_out_ptr),
                                       static_cast<uint32_t>(e->item_size()));
      if (spec_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(spec_ptr), e->handle());
      }
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);

  // Leaderboard *writes* land here: the game submits scores via an async
  // XLiveBase RPC (XMsgStartIORequest(0xFC, (u16 descriptor)|0x50000, ...)),
  // whose message number is computed at runtime from an RPC descriptor table
  // (sub_82813388 in default.xex) and therefore isn't a fixed constant we can
  // grep for. Dump the request buffer so the exact message + serialized
  // payload can be identified from a single score-save run, then decoded into
  // LeaderboardManager::SubmitRow. buffer_ptr is the packed request produced
  // by sub_82813718/sub_82813598; buffer_length (arg2) is often 0 for async
  // requests, so probe a fixed window.
  if (buffer_ptr) {
    constexpr uint32_t kDumpBytes = 128;
    auto* bytes = memory_->TranslateVirtual(buffer_ptr);
    std::string hex;
    hex.reserve(kDumpBytes * 3);
    for (uint32_t i = 0; i < kDumpBytes; ++i) {
      hex += fmt::format("{:02X} ", bytes[i]);
    }
    REXKRNL_ERROR("XLIVEBASE msg={:08X} request dump [{} bytes]: {}", message, kDumpBytes, hex);
  }
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
