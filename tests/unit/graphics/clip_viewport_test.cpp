#include <catch2/catch_test_macros.hpp>

#include "graphics/native/clip_viewport.h"

using rex::graphics::native::HostClipDisabledViewportMax;
using rex::graphics::native::IsHostClipRangeExtent;

TEST_CASE("clip-disabled viewport max is the device limit, not the surface pitch",
          "[clip_viewport]") {
  // SF3 log: pitch 640, stale or following resolve 1536x896 / 1280x720, device
  // maxViewportDimensions 32768. The host range must stay the device limit so
  // GetHostViewportInfo can cap it at 8192. Pitch 640 clips the 1280 resolve.
  const auto extent = HostClipDisabledViewportMax(32768, 32768, 640, 1536, 896);
  CHECK(extent.x == 32768);
  CHECK(extent.y == 32768);
  CHECK_FALSE(IsHostClipRangeExtent(640));
  CHECK(IsHostClipRangeExtent(8192));
}

TEST_CASE("clip-disabled viewport max ignores a matching resolve", "[clip_viewport]") {
  const auto extent = HostClipDisabledViewportMax(32768, 32768, 1536, 1536, 896);
  CHECK(extent.x == 32768);
  CHECK(extent.y == 32768);
}

TEST_CASE("clip-disabled viewport max is at least 1", "[clip_viewport]") {
  const auto extent = HostClipDisabledViewportMax(0, 0, 0, 0, 0);
  CHECK(extent.x == 1);
  CHECK(extent.y == 1);
}
