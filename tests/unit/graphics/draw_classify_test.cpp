/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Tests for the native backend's draw disposition table.
 *
 * The regression these exist to prevent: treating a draw that needs no pixel
 * shader as a draw that needs no RASTERIZATION. That dropped 316,001 draws in
 * one Hydro Thunder run, left the depth buffer empty, and rendered the 3D
 * world black while the z-testless HUD drew perfectly.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include "graphics/native/draw_classify.h"

using namespace rex::graphics::native;
using rex::graphics::xenos::EdramMode;

namespace {
// An ordinary textured draw; each test perturbs one fact.
DrawFacts Ordinary() { return DrawFacts{}; }
}  // namespace

TEST_CASE("an ordinary draw keeps its fragment stage", "[draw_classify]") {
  CHECK(ClassifyDraw(Ordinary()) == DrawDisposition::kFull);
}

TEST_CASE("kCopy hands off to the resolve path", "[draw_classify]") {
  auto f = Ordinary();
  f.edram_mode = EdramMode::kCopy;
  CHECK(ClassifyDraw(f) == DrawDisposition::kResolve);
}

// --- The fix ---------------------------------------------------------------

TEST_CASE("kDepthOnly is RENDERED without a fragment stage, not skipped",
          "[draw_classify]") {
  auto f = Ordinary();
  f.edram_mode = EdramMode::kDepthOnly;
  const auto d = ClassifyDraw(f);
  CHECK(d == DrawDisposition::kDepthOnly);
  CHECK_FALSE(IsSkip(d));
}

TEST_CASE("a draw whose pixel shader is unneeded still rasterizes",
          "[draw_classify]") {
  // IsPixelShaderNeededWithRasterization == false means "the FRAGMENT STAGE may
  // be dropped", NOT "drop the draw". This is the exact misreading that caused
  // the black 3D world.
  auto f = Ordinary();
  f.pixel_shader_needed = false;
  const auto d = ClassifyDraw(f);
  CHECK(d == DrawDisposition::kDepthOnly);
  CHECK_FALSE(IsSkip(d));
}

TEST_CASE("a missing pixel shader is depth-only, not a skip", "[draw_classify]") {
  auto f = Ordinary();
  f.has_pixel_shader = false;
  CHECK(ClassifyDraw(f) == DrawDisposition::kDepthOnly);
}

// --- Ordering, which is load-bearing ---------------------------------------

TEST_CASE("vertex memexport is rejected above the rasterization test",
          "[draw_classify]") {
  // The oracle discards on "!rasterization && !vertex_memexport". Rejecting
  // vertex memexport FIRST is what lets the rasterization test below stand in
  // for that combined condition. If the order flipped, a kNoOperation draw kept
  // alive only by its memexport side effect would be silently rendered.
  auto f = Ordinary();
  f.vertex_memexport = true;
  f.rasterization_possible = false;
  CHECK(ClassifyDraw(f) == DrawDisposition::kSkipVertexMemexport);
}

TEST_CASE("pixel memexport is tested below the fragment-stage nulling",
          "[draw_classify]") {
  // A pixel shader that got dropped cannot memexport, so it must NOT be
  // rejected for memexport - the oracle renders this draw.
  auto f = Ordinary();
  f.pixel_shader_needed = false;
  f.pixel_memexport = true;
  CHECK(ClassifyDraw(f) == DrawDisposition::kDepthOnly);
}

TEST_CASE("pixel memexport on a live fragment stage is still skipped",
          "[draw_classify]") {
  auto f = Ordinary();
  f.pixel_memexport = true;
  CHECK(ClassifyDraw(f) == DrawDisposition::kSkipPixelMemexport);
}

TEST_CASE("kDepthOnly ignores pixel memexport entirely", "[draw_classify]") {
  auto f = Ordinary();
  f.edram_mode = EdramMode::kDepthOnly;
  f.pixel_memexport = true;
  CHECK(ClassifyDraw(f) == DrawDisposition::kDepthOnly);
}

// --- Genuine discards ------------------------------------------------------

TEST_CASE("a draw that cannot rasterize is discarded", "[draw_classify]") {
  auto f = Ordinary();
  f.rasterization_possible = false;
  CHECK(ClassifyDraw(f) == DrawDisposition::kSkipNoRasterization);
}

TEST_CASE("kNoOperation cannot rasterize and is discarded", "[draw_classify]") {
  // IsRasterizationPotentiallyDone rejects every mode but kColorDepth and
  // kDepthOnly, so kNoOperation arrives with rasterization_possible false.
  auto f = Ordinary();
  f.edram_mode = EdramMode::kNoOperation;
  f.rasterization_possible = false;
  CHECK(ClassifyDraw(f) == DrawDisposition::kSkipNoRasterization);
}

TEST_CASE("resolve wins over every other condition", "[draw_classify]") {
  auto f = Ordinary();
  f.edram_mode = EdramMode::kCopy;
  f.vertex_memexport = true;
  f.rasterization_possible = false;
  CHECK(ClassifyDraw(f) == DrawDisposition::kResolve);
}
