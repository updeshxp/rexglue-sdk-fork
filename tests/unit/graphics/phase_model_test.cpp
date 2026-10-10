/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Tests for the native backend's EDRAM phase model.
 *
 * Each of these encodes a rule that was established empirically and is easy to
 * regress: get phase ownership wrong and an imposter atlas accumulates the
 * draws of the one before it; get display selection wrong and the frame is
 * blank or double-composited.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "graphics/native/phase_model.h"

using namespace rex::graphics::native;

// ---------------------------------------------------------------------------
// Address keying
// ---------------------------------------------------------------------------

TEST_CASE("resolve keys ignore the low 12 bits", "[phase_model]") {
  CHECK(ResolvedTargetKey(0x16FCF000u) == 0x16FCF000u);
  CHECK(ResolvedTargetKey(0x16FCF123u) == 0x16FCF000u);
}

TEST_CASE("resolve keys drop bits above the EDRAM-addressable range",
          "[phase_model]") {
  // A texture fetch and a resolve destination must agree after masking or the
  // alias silently misses and the draw samples guest memory the backend never
  // wrote - which reads as black.
  CHECK(ResolvedTargetKey(0xF16FCF000u & 0xFFFFFFFFu) == ResolvedTargetKey(0x16FCF000u));
  CHECK(ResolvedTargetKey(0x1F4FE000u) == 0x1F4FE000u);
}

// ---------------------------------------------------------------------------
// Phase ownership
// ---------------------------------------------------------------------------

TEST_CASE("a base's first resolve of the frame owns everything before it",
          "[phase_model]") {
  CHECK(PhaseFirstDraw(false, 0, false, 0) == 0u);
}

TEST_CASE("a re-resolved base owns only draws since its last resolve",
          "[phase_model]") {
  // Base 832 serves four imposter atlases in sequence. Without this, atlas 2
  // would contain atlas 1's draws as well.
  CHECK(PhaseFirstDraw(false, 0, true, 40) == 40u);
}

TEST_CASE("a clear later than the last resolve wins", "[phase_model]") {
  CHECK(PhaseFirstDraw(true, 90, true, 40) == 90u);
}

TEST_CASE("a resolve later than the last clear wins", "[phase_model]") {
  // Resolves do NOT clear EDRAM, so a post-process chain that resolves the same
  // base twice is progressive - but ownership still starts at the later mark.
  CHECK(PhaseFirstDraw(true, 40, true, 119) == 119u);
}

// ---------------------------------------------------------------------------
// Draw membership
// ---------------------------------------------------------------------------

TEST_CASE("an unfiltered range takes every draw", "[phase_model]") {
  CHECK(DrawInRange(/*color_base=*/468, /*depth_only=*/false, kAnyBase));
  CHECK(DrawInRange(/*color_base=*/832, /*depth_only=*/false, kAnyBase));
}

TEST_CASE("a filtered range takes only its own base", "[phase_model]") {
  CHECK(DrawInRange(468, false, 468));
  CHECK_FALSE(DrawInRange(832, false, 468));
}

TEST_CASE("depth-only draws bypass the base filter", "[phase_model]") {
  // Their RB_COLOR_INFO is stale, so filtering on it would drop the depth
  // pre-pass out of the very phase whose depth it establishes - which is what
  // left 3D geometry failing its depth test against a cleared buffer.
  CHECK(DrawInRange(/*color_base=*/0, /*depth_only=*/true, 468));
  CHECK(DrawInRange(/*color_base=*/832, /*depth_only=*/true, 468));
}

// ---------------------------------------------------------------------------
// Display selection
// ---------------------------------------------------------------------------

TEST_CASE("a frame with no resolves replays everything", "[phase_model]") {
  // The Geometry Wars baseline - no render-to-texture at all.
  const auto ranges = SelectDisplayRanges({}, /*fb_key=*/0x1F4FE000u,
                                          /*deferred_count=*/120, /*phase_first_draw=*/0);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 120, kAnyBase});
}

TEST_CASE("only frontbuffer phases reach the screen", "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{
      {/*src_base=*/468, /*first=*/0, /*end=*/122, /*dest=*/0x16FCF000u},  // offscreen scene
      {/*src_base=*/0, /*first=*/122, /*end=*/134, /*dest=*/fb},           // composite
  };
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/134,
                                          /*phase_first_draw=*/134);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{122, 134, 0});
}

TEST_CASE("trailing draws after the last resolve still display",
          "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{{0, 0, 10, fb}};
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/14,
                                          /*phase_first_draw=*/10);
  REQUIRE(ranges.size() == 2);
  CHECK(ranges[0] == DisplayRange{0, 10, 0});
  CHECK(ranges[1] == DisplayRange{10, 14, kAnyBase});
}

TEST_CASE("no frontbuffer match falls back to replaying everything",
          "[phase_model]") {
  // Note this DISCARDS the trailing range too - the fallback is whole-frame.
  std::vector<PhaseSpan> phases{{468, 0, 122, 0x16FCF000u}};
  const auto ranges = SelectDisplayRanges(phases, /*fb_key=*/0x1F4FE000u,
                                          /*deferred_count=*/130, /*phase_first_draw=*/122);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 130, kAnyBase});
}

TEST_CASE("an empty frontbuffer phase does not count as a match",
          "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{{0, 5, 5, fb}};
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/5,
                                          /*phase_first_draw=*/5);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 5, kAnyBase});
}

// ---------------------------------------------------------------------------
// Destination collisions
// ---------------------------------------------------------------------------

TEST_CASE("distinct destinations never collide", "[phase_model]") {
  std::vector<PhaseSpan> phases{
      {468, 0, 122, 0x16FCF000u},
      {468, 122, 123, 0x16D8F000u},
  };
  CHECK(FindOverwrittenPhases(phases).empty());
}

TEST_CASE("two phases resolving to one address in a frame collide",
          "[phase_model]") {
  // Measured in Hydro Thunder: a 512x576 phase at base 936 and a 1024x576 phase
  // at base 468 both resolve to 0x1690F000 in the same frame. Resolved images
  // are keyed by address alone, so they share one image - the second overwrites
  // the first, and a differing size reallocates it outright.
  std::vector<PhaseSpan> phases{
      {/*src_base=*/936, 0, 3, 0x1690F000u},
      {/*src_base=*/468, 3, 4, 0x1690F000u},
  };
  const auto overwritten = FindOverwrittenPhases(phases);
  REQUIRE(overwritten.size() == 1);
  CHECK(overwritten[0] == 0u);  // the base-936 phase loses its content
}

TEST_CASE("an empty phase is not reported as overwritten", "[phase_model]") {
  std::vector<PhaseSpan> phases{
      {936, 3, 3, 0x1690F000u},
      {468, 3, 4, 0x1690F000u},
  };
  CHECK(FindOverwrittenPhases(phases).empty());
}

// ---------------------------------------------------------------------------
// Shapes measured from real titles
// ---------------------------------------------------------------------------

TEST_CASE("Choplifter shape: one base resolved offscreen then to the frontbuffer",
          "[phase_model]") {
  // Measured at swap 1600: the whole frame is two phases, BOTH at EDRAM base
  // 720. The world renders and resolves to a texture at 0x13380000; the HUD
  // then renders at the same base and resolves to the frontbuffer.
  //
  // Same base, different destinations - so this is NOT the duplicate-resolve
  // collision, and ownership must still split the two cleanly: the world phase
  // owns draws [0,90), the HUD phase only [90,104).
  const uint32_t fb = 0x1F5F8000u;
  const uint32_t world_end = 90;

  CHECK(PhaseFirstDraw(false, 0, false, 0) == 0u);            // world: first resolve of base 720
  CHECK(PhaseFirstDraw(false, 0, true, world_end) == 90u);    // HUD: since that base last resolved

  std::vector<PhaseSpan> phases{
      {/*src_base=*/720, 0, world_end, 0x13380000u},
      {/*src_base=*/720, world_end, 104, fb},
  };
  CHECK(FindOverwrittenPhases(phases).empty());

  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/104,
                                          /*phase_first_draw=*/104);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{world_end, 104, 720});
}

TEST_CASE("Choplifter shape: the world phase must not be starved by base filtering",
          "[phase_model]") {
  // The world phase came out EMPTY on the device. Ownership above is correct, so
  // if the world draws are dropped it is the per-draw filter: any world draw
  // carrying a colour base other than the phase's own is excluded. This pins the
  // contract the offscreen replay relies on.
  const uint32_t world_base = 720;
  CHECK(DrawInRange(world_base, /*depth_only=*/false, world_base));
  // A draw that targeted a different colour base genuinely does not belong.
  CHECK_FALSE(DrawInRange(/*color_base=*/468, false, world_base));
  // ...but a depth-only draw must never be filtered out by a stale colour base.
  CHECK(DrawInRange(/*color_base=*/468, /*depth_only=*/true, world_base));
}

TEST_CASE("OutRun shape: a frame with no resolves still replays its draws",
          "[phase_model]") {
  // Measured: OutRun issues ZERO resolves in the window sampled, so phases_ is
  // empty. Selection must fall back to replaying everything - a frame that
  // renders nothing here would be a selection bug rather than a draw bug.
  const auto ranges = SelectDisplayRanges({}, /*fb_key=*/0x1F4FE000u,
                                          /*deferred_count=*/57, /*phase_first_draw=*/0);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 57, kAnyBase});
}

TEST_CASE("a frame with no resolves AND no draws selects an empty range",
          "[phase_model]") {
  // OutRun measured mean 0.0003 - essentially only the marker tab. If the
  // deferred list is empty the selection is vacuous, which distinguishes
  // "nothing was drawn" from "draws were dropped at selection".
  const auto ranges = SelectDisplayRanges({}, 0x1F4FE000u, /*deferred_count=*/0,
                                          /*phase_first_draw=*/0);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 0, kAnyBase});
}

// --- Presenting the resolved frontbuffer, rather than replaying its draws ---

TEST_CASE("SoulCalibur II shape: the finished frame is presented, not replayed",
          "[phase_model]") {
  // Measured on the native backend: the guest asks to display 0x1D2D0000 and
  // 0x1CF38000 alternately (double buffered), and its phases resolve to an
  // intermediate at 0x1E4C8000 AND to both of those frontbuffer addresses. So
  // the finished frame already exists at the address IssueSwap names.
  //
  // Replaying its draws produced a BLACK frame (mean 0.000333, byte-identical
  // across seven titles). Presenting the resolved image is what the hardware
  // does, so that is what must be selected.
  const std::vector<PhaseSpan> phases = {
      {/*src_base=*/0, /*first=*/0, /*end=*/120, /*dest_key=*/0x1E4C8000u},
      {/*src_base=*/0, /*first=*/120, /*end=*/180, /*dest_key=*/0x1D2D0000u},
  };
  const auto src = ChooseDisplaySource(phases, /*fb_key=*/0x1D2D0000u,
                                       /*deferred_count=*/180, /*phase_first_draw=*/180);
  CHECK(src.present_resolved);
  CHECK(src.resolved_key == 0x1D2D0000u);
  // Nothing trailing, so nothing replays on top.
  CHECK(src.ranges.empty());
}

TEST_CASE("a HUD drawn after the last resolve still replays on top", "[phase_model]") {
  // The resolved image cannot contain draws issued after it was resolved, so
  // trailing draws must survive - otherwise presenting the image would silently
  // drop the HUD.
  const std::vector<PhaseSpan> phases = {
      {/*src_base=*/720, /*first=*/0, /*end=*/90, /*dest_key=*/0x1F5F8000u},
  };
  const auto src = ChooseDisplaySource(phases, /*fb_key=*/0x1F5F8000u,
                                       /*deferred_count=*/105, /*phase_first_draw=*/90);
  CHECK(src.present_resolved);
  REQUIRE(src.ranges.size() == 1);
  CHECK(src.ranges[0] == DisplayRange{90, 105, kAnyBase});
}

TEST_CASE("no resolve reaching the frontbuffer keeps the replay model",
          "[phase_model]") {
  // OutRun's measured shape: one phase, resolving to 0x16818000, which is NOT
  // the frontbuffer. There is no finished image to present, so selection must
  // fall back to replaying - never to presenting an address nothing wrote.
  const std::vector<PhaseSpan> phases = {
      {/*src_base=*/0, /*first=*/0, /*end=*/57, /*dest_key=*/0x16818000u},
  };
  const auto src = ChooseDisplaySource(phases, /*fb_key=*/0x1F4FE000u,
                                       /*deferred_count=*/57, /*phase_first_draw=*/57);
  CHECK_FALSE(src.present_resolved);
  REQUIRE(src.ranges.size() == 1);
  CHECK(src.ranges[0] == DisplayRange{0, 57, kAnyBase});
}

TEST_CASE("Geometry Wars shape: no resolves at all still replays everything",
          "[phase_model]") {
  // The title never resolves; it must keep the replay-all baseline and must
  // not be diverted into presenting a resolved image that does not exist.
  const auto src = ChooseDisplaySource({}, /*fb_key=*/0x1F4FE000u,
                                       /*deferred_count=*/300, /*phase_first_draw=*/0);
  CHECK_FALSE(src.present_resolved);
  REQUIRE(src.ranges.size() == 1);
  CHECK(src.ranges[0] == DisplayRange{0, 300, kAnyBase});
}

TEST_CASE("an EMPTY frontbuffer phase does not count as a finished frame",
          "[phase_model]") {
  // A phase that owns no draws resolved nothing, so presenting its address
  // would show a stale or blank image. Selection must fall through to replay.
  const std::vector<PhaseSpan> phases = {
      {/*src_base=*/0, /*first=*/40, /*end=*/40, /*dest_key=*/0x1D2D0000u},
  };
  const auto src = ChooseDisplaySource(phases, /*fb_key=*/0x1D2D0000u,
                                       /*deferred_count=*/40, /*phase_first_draw=*/40);
  CHECK_FALSE(src.present_resolved);
}
