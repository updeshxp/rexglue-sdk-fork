#include <string>

#include <catch2/catch_test_macros.hpp>

#include "graphics/native/clip_viewport.h"

using rex::graphics::native::ChooseClipDisabledExtent;

TEST_CASE("clip-disabled width is the current surface pitch", "[clip_viewport]") {
  // SF3 log: surface pitch 640, stale resolve of base 0 still 1280x720 or
  // 1536x896, window scissor Y is the 8192 sentinel.
  const auto extent = ChooseClipDisabledExtent(640, 1536, 896, true, 640, false, 8192, 1280, 720);
  CHECK(extent.x == 640);
  CHECK(extent.y == 720);
  CHECK(std::string(extent.source) == "surface_pitch");
}

TEST_CASE("resolve height is used only when its width matches the surface", "[clip_viewport]") {
  const auto extent = ChooseClipDisabledExtent(1536, 1536, 896, false, 8192, false, 8192, 1280, 720);
  CHECK(extent.x == 1536);
  CHECK(extent.y == 896);
  CHECK(std::string(extent.source) == "resolve_matched_width");
}

TEST_CASE("no pitch and no resolve falls back to the video mode", "[clip_viewport]") {
  const auto extent = ChooseClipDisabledExtent(0, 0, 0, false, 8192, false, 8192, 1280, 720);
  CHECK(extent.x == 1280);
  CHECK(extent.y == 720);
}
