/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Tests for register-derived vertex-shader state on the native backend.
 *
 * All three of these were hardcoded during bring-up. Each is invisible on a 2D
 * pass and destructive on a 3D one, which is why they survived so long: 2D
 * screens rendered pixel-perfect while 3D scenes came out as a screen-spanning
 * white triangle with shattered geometry.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "graphics/native/shader_constants.h"

using namespace rex::graphics::native;

// ---------------------------------------------------------------------------
// User clip planes
// ---------------------------------------------------------------------------

TEST_CASE("clip_disable suppresses every user clip plane", "[shader_constants]") {
  // The 2D case: a blit pass disables clipping, so hardcoding zero happened to
  // be correct here - which is exactly why 2D looked fine.
  const auto cfg = ComputeUserClipPlanes(/*clip_disable=*/true, /*ucp_ena=*/0x3F,
                                         /*ucp_cull_only_ena=*/false, true, true);
  CHECK(cfg == UserClipPlaneConfig{0, 0});
}

TEST_CASE("enabled planes are counted, not passed through", "[shader_constants]") {
  // The shader is told only the TOTAL, tightly packed.
  CHECK(ComputeUserClipPlanes(false, 0b000001, false, true, true).count == 1);
  CHECK(ComputeUserClipPlanes(false, 0b000101, false, true, true).count == 2);
  CHECK(ComputeUserClipPlanes(false, 0b111111, false, true, true).count == 6);
}

TEST_CASE("the 3D case emits clip distances", "[shader_constants]") {
  // This is what was being lost: a 3D pass with clip planes enabled produced
  // count 0, so guest-clipped geometry rasterized unclipped.
  const auto cfg = ComputeUserClipPlanes(/*clip_disable=*/false, /*ucp_ena=*/0b000011,
                                         /*ucp_cull_only_ena=*/false, true, true);
  CHECK(cfg == UserClipPlaneConfig{2, 0});
}

TEST_CASE("cull-only planes become cull distances", "[shader_constants]") {
  const auto cfg = ComputeUserClipPlanes(false, 0b000011, /*ucp_cull_only_ena=*/true, true, true);
  CHECK(cfg == UserClipPlaneConfig{2, 1});
}

TEST_CASE("cull distances substitute when clip distances are unavailable",
          "[shader_constants]") {
  const auto cfg = ComputeUserClipPlanes(false, 0b000011, false,
                                         /*supports_clip=*/false, /*supports_cull=*/true);
  CHECK(cfg == UserClipPlaneConfig{2, 1});
}

TEST_CASE("planes are dropped when the host supports neither built-in",
          "[shader_constants]") {
  const auto cfg = ComputeUserClipPlanes(false, 0b111111, false,
                                         /*supports_clip=*/false, /*supports_cull=*/false);
  CHECK(cfg == UserClipPlaneConfig{0, 0});
}

TEST_CASE("only the low 6 plane bits count", "[shader_constants]") {
  CHECK(ComputeUserClipPlanes(false, 0xFFFFFFFFu, false, true, true).count == 6);
}

// ---------------------------------------------------------------------------
// Vertex kill
// ---------------------------------------------------------------------------

TEST_CASE("vertex kill uses a cull distance when vtx_kill_or is clear",
          "[shader_constants]") {
  // The alternative path kills by writing NaN to position.w. In the rectangle
  // expansion that NaN poisons the edge-length comparison that picks the
  // diagonal, so a single killed vertex misplaces the entire quad.
  CHECK(ComputeVertexKillAnd(true, 0b100, /*vtx_kill_or=*/false));
}

TEST_CASE("vertex kill falls back to the OR path when the guest asks for it",
          "[shader_constants]") {
  CHECK_FALSE(ComputeVertexKillAnd(true, 0b100, /*vtx_kill_or=*/true));
}

TEST_CASE("a shader that does not kill vertices needs no cull distance",
          "[shader_constants]") {
  CHECK_FALSE(ComputeVertexKillAnd(true, 0b001, false));
  CHECK_FALSE(ComputeVertexKillAnd(true, 0b000, false));
}

TEST_CASE("vertex kill needs host cull distance support", "[shader_constants]") {
  CHECK_FALSE(ComputeVertexKillAnd(/*supports_cull_distance=*/false, 0b100, false));
}

// ---------------------------------------------------------------------------
// Colour exponent bias
// ---------------------------------------------------------------------------

TEST_CASE("a zero exponent bias is unity", "[shader_constants]") {
  // The hardcoded value. Correct only when the render target carries no bias.
  CHECK(ColorExpBiasScale(0) == 1.0f);
}

TEST_CASE("exponent bias scales by powers of two", "[shader_constants]") {
  CHECK(ColorExpBiasScale(1) == 2.0f);
  CHECK(ColorExpBiasScale(2) == 4.0f);
  CHECK(ColorExpBiasScale(-1) == 0.5f);
  CHECK(ColorExpBiasScale(-2) == 0.25f);
}

TEST_CASE("fixed 16-bit formats are remapped by five stops", "[shader_constants]") {
  // -32..32 -> -1..1, trading blending correctness for full range.
  CHECK(AdjustColorExpBiasForFormat(0, /*is_fixed_16_format=*/true,
                                    /*truncated_to_minus_1_to_1=*/false) == -5);
  CHECK(ColorExpBiasScale(AdjustColorExpBiasForFormat(0, true, false)) == 1.0f / 32.0f);
}

TEST_CASE("already-truncated formats are not remapped", "[shader_constants]") {
  CHECK(AdjustColorExpBiasForFormat(0, true, /*truncated_to_minus_1_to_1=*/true) == 0);
}

TEST_CASE("non-16-bit formats are never remapped", "[shader_constants]") {
  CHECK(AdjustColorExpBiasForFormat(3, /*is_fixed_16_format=*/false, false) == 3);
}
