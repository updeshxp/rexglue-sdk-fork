/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Tests for the native backend's index-buffer expansion.
 *
 * These cover the parts that are easy to get subtly wrong and expensive to
 * diagnose from a running title: winding order (a flipped triangle disappears
 * into backface culling), the rectangle corner encoding the vertex shader
 * decodes, endian handling, and index width selection.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "graphics/native/index_expand.h"

using namespace rex::graphics::native;
using rex::graphics::xenos::Endian;

namespace {

// Builds a guest index buffer in the layout the GPU would see.
std::vector<uint8_t> MakeGuestIndices16(const std::vector<uint16_t>& values) {
  std::vector<uint8_t> bytes(values.size() * 2);
  for (size_t i = 0; i < values.size(); ++i) {
    bytes[i * 2 + 0] = uint8_t(values[i] & 0xFF);
    bytes[i * 2 + 1] = uint8_t(values[i] >> 8);
  }
  return bytes;
}

std::vector<uint8_t> MakeGuestIndices32(const std::vector<uint32_t>& values) {
  std::vector<uint8_t> bytes(values.size() * 4);
  for (size_t i = 0; i < values.size(); ++i) {
    bytes[i * 4 + 0] = uint8_t(values[i] & 0xFF);
    bytes[i * 4 + 1] = uint8_t((values[i] >> 8) & 0xFF);
    bytes[i * 4 + 2] = uint8_t((values[i] >> 16) & 0xFF);
    bytes[i * 4 + 3] = uint8_t(values[i] >> 24);
  }
  return bytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// GuestIndexSource
// ---------------------------------------------------------------------------

TEST_CASE("auto-indexed source returns the vertex ordinal", "[index_expand]") {
  GuestIndexSource src;  // data == nullptr
  CHECK(src.Read(0) == 0);
  CHECK(src.Read(7) == 7);
  CHECK(src.Read(65536) == 65536);
}

TEST_CASE("16-bit guest indices decode without an endian swap", "[index_expand]") {
  const auto bytes = MakeGuestIndices16({0x0102, 0x0304});
  GuestIndexSource src{bytes.data(), /*is_32bit=*/false, Endian::kNone};
  CHECK(src.Read(0) == 0x0102);
  CHECK(src.Read(1) == 0x0304);
}

TEST_CASE("16-bit guest indices byte-swap under 8in16", "[index_expand]") {
  const auto bytes = MakeGuestIndices16({0x0102});
  GuestIndexSource src{bytes.data(), /*is_32bit=*/false, Endian::k8in16};
  CHECK(src.Read(0) == 0x0201);
}

TEST_CASE("32-bit guest indices honour each endian mode", "[index_expand]") {
  const auto bytes = MakeGuestIndices32({0x11223344u});

  GuestIndexSource none{bytes.data(), true, Endian::kNone};
  CHECK(none.Read(0) == 0x11223344u);

  GuestIndexSource b8in32{bytes.data(), true, Endian::k8in32};
  CHECK(b8in32.Read(0) == 0x44332211u);

  GuestIndexSource b16in32{bytes.data(), true, Endian::k16in32};
  CHECK(b16in32.Read(0) == 0x33441122u);

  GuestIndexSource b8in16{bytes.data(), true, Endian::k8in16};
  CHECK(b8in16.Read(0) == 0x22114433u);
}

// ---------------------------------------------------------------------------
// Quad lists
// ---------------------------------------------------------------------------

TEST_CASE("quad list expands 4 vertices into 6 indices", "[index_expand]") {
  CHECK(QuadListQuadCount(4) == 1);
  CHECK(QuadListExpandedCount(4) == 6);
  CHECK(QuadListQuadCount(8) == 2);
  CHECK(QuadListExpandedCount(8) == 12);
  // A partial quad contributes nothing rather than reading past the end.
  CHECK(QuadListQuadCount(3) == 0);
  CHECK(QuadListExpandedCount(3) == 0);
}

TEST_CASE("auto-indexed quad uses the 0,1,3/3,1,2 decomposition", "[index_expand]") {
  GuestIndexSource src;
  std::vector<uint16_t> out(QuadListExpandedCount(4));
  ExpandQuadList(4, src, out.data());
  // Both triangles share edge 1-3, which is what keeps the winding consistent
  // with the emulating backend's quad geometry shader.
  CHECK(out == std::vector<uint16_t>{0, 1, 3, 3, 1, 2});
}

TEST_CASE("second quad is offset by 4 vertices", "[index_expand]") {
  GuestIndexSource src;
  std::vector<uint16_t> out(QuadListExpandedCount(8));
  ExpandQuadList(8, src, out.data());
  const std::vector<uint16_t> expected{0, 1, 3, 3, 1, 2, 4, 5, 7, 7, 5, 6};
  CHECK(out == expected);
}

TEST_CASE("DMA-indexed quad reads the guest index buffer", "[index_expand]") {
  // A quad whose guest indices are not sequential - if the expansion ignored
  // the index buffer it would emit 0,1,3,3,1,2 instead.
  const auto bytes = MakeGuestIndices16({100, 101, 102, 103});
  GuestIndexSource src{bytes.data(), /*is_32bit=*/false, Endian::kNone};
  std::vector<uint16_t> out(QuadListExpandedCount(4));
  ExpandQuadList(4, src, out.data());
  const std::vector<uint16_t> expected{100, 101, 103, 103, 101, 102};
  CHECK(out == expected);
}

TEST_CASE("quad expansion selects 32-bit output when it must", "[index_expand]") {
  GuestIndexSource auto_indexed;
  CHECK_FALSE(QuadListNeeds32Bit(1024, auto_indexed));
  // Auto-indexed draws large enough to exceed 16-bit vertex indices.
  CHECK(QuadListNeeds32Bit(0x10001u, auto_indexed));

  GuestIndexSource wide{nullptr, /*is_32bit=*/true, Endian::kNone};
  CHECK(QuadListNeeds32Bit(4, wide));
}

// ---------------------------------------------------------------------------
// Rectangle lists
// ---------------------------------------------------------------------------

TEST_CASE("rectangle list expands 3 vertices into 6 indices", "[index_expand]") {
  CHECK(RectangleListRectCount(3) == 1);
  CHECK(RectangleListExpandedCount(3) == 6);
  CHECK(RectangleListRectCount(6) == 2);
  CHECK(RectangleListExpandedCount(6) == 12);
  CHECK(RectangleListRectCount(2) == 0);
}

TEST_CASE("rectangle emits both triangles, not just the first", "[index_expand]") {
  // The regression this guards: drawing only corners 0,1,2 renders half the
  // rectangle and leaves a diagonal seam across composite blits.
  GuestIndexSource src;
  std::vector<uint32_t> out(RectangleListExpandedCount(3));
  ExpandRectangleList(3, src, out.data());
  REQUIRE(out.size() == 6);

  // Corner is the low 2 bits; all four corners must appear.
  std::vector<uint32_t> corners;
  for (uint32_t v : out) {
    corners.push_back(v & 3u);
  }
  CHECK(corners == std::vector<uint32_t>{0, 1, 2, 2, 1, 3});
  CHECK(std::count(corners.begin(), corners.end(), 3u) == 1);
}

TEST_CASE("rectangle indices encode the primitive ordinal above the corner",
          "[index_expand]") {
  GuestIndexSource src;
  std::vector<uint32_t> out(RectangleListExpandedCount(6));
  ExpandRectangleList(6, src, out.data());
  REQUIRE(out.size() == 12);

  // First rectangle: primitive 0. Second: primitive 1.
  for (size_t i = 0; i < 6; ++i) {
    CHECK((out[i] >> 2) == 0u);
    CHECK((out[i + 6] >> 2) == 1u);
  }
  // The corner pattern repeats per rectangle.
  CHECK((out[6] & 3u) == 0u);
  CHECK((out[11] & 3u) == 3u);
}

TEST_CASE("rectangle list form is equivalent to the backend's strip form",
          "[index_expand]") {
  // The emulating backend expands rectangles as a triangle STRIP with primitive
  // restart, over a builtin buffer of (primitive << 2 | corner) with UINT32_MAX
  // separators (primitive_processor.cpp:188-207). This backend uses a triangle
  // LIST instead. This asserts the two describe the SAME triangles with the
  // SAME winding, so the list form is not the reason rectangle expansion
  // corrupts 3D scenes - don't re-litigate the index side.
  const uint32_t kRestart = UINT32_MAX;
  const uint32_t rect_count = 3;

  // Build the reference strip exactly as the builtin buffer does.
  std::vector<uint32_t> strip;
  for (uint32_t i = 0; i < rect_count; ++i) {
    if (i) {
      strip.push_back(kRestart);
    }
    for (uint32_t j = 0; j < 4; ++j) {
      strip.push_back((i << 2) + j);
    }
  }

  // Decompose that strip into triangles the way the rasterizer would, flipping
  // winding on odd triangles as a strip does.
  std::vector<std::array<uint32_t, 3>> from_strip;
  for (size_t i = 0; i + 2 < strip.size(); ++i) {
    if (strip[i] == kRestart || strip[i + 1] == kRestart || strip[i + 2] == kRestart) {
      continue;
    }
    size_t run_start = 0;
    for (size_t k = 0; k <= i; ++k) {
      if (strip[k] == kRestart) {
        run_start = k + 1;
      }
    }
    const bool odd = ((i - run_start) & 1) != 0;
    from_strip.push_back(odd ? std::array<uint32_t, 3>{strip[i + 1], strip[i], strip[i + 2]}
                             : std::array<uint32_t, 3>{strip[i], strip[i + 1], strip[i + 2]});
  }

  // And the list form this backend actually emits.
  GuestIndexSource src;
  std::vector<uint32_t> list(RectangleListExpandedCount(rect_count * 3));
  ExpandRectangleList(rect_count * 3, src, list.data());
  std::vector<std::array<uint32_t, 3>> from_list;
  for (size_t i = 0; i + 2 < list.size(); i += 3) {
    from_list.push_back({list[i], list[i + 1], list[i + 2]});
  }

  CHECK(from_strip == from_list);
}

TEST_CASE("both rectangle triangles share the 1-2 edge", "[index_expand]") {
  // Strip 0,1,2,3 emitted as list 0,1,2 / 2,1,3. Sharing edge 1-2 is what makes
  // the two triangles a rectangle rather than a bowtie, and keeps both windings
  // the same so neither is culled.
  GuestIndexSource src;
  std::vector<uint32_t> out(RectangleListExpandedCount(3));
  ExpandRectangleList(3, src, out.data());
  const uint32_t a[3] = {out[0] & 3u, out[1] & 3u, out[2] & 3u};
  const uint32_t b[3] = {out[3] & 3u, out[4] & 3u, out[5] & 3u};
  CHECK(a[1] == 1u);
  CHECK(a[2] == 2u);
  CHECK(b[0] == 2u);
  CHECK(b[1] == 1u);
}
