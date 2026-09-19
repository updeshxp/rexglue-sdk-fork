/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Index-buffer expansion for the native GPU backend.
 *
 * The native draw path has no geometry-shader stage, so guest primitive types
 * the host cannot draw directly (quad lists, rectangle lists) are turned into
 * plain triangle lists by generating an index buffer here.
 *
 * This header is deliberately free of Vulkan and of any guest-memory plumbing:
 * the source indices arrive through GuestIndexSource and the results are
 * written into a caller-owned buffer. That keeps the tricky part - winding
 * order, corner encoding, endian handling, index width - testable on the host
 * without a GPU or a running title (tests/unit/graphics/index_expand_test.cpp).
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_INDEX_EXPAND_H_
#define REX_GRAPHICS_NATIVE_INDEX_EXPAND_H_

#include <cstddef>
#include <cstdint>

#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {
namespace native {

// Endian conversion for guest index data. Only the three swapping modes
// actually reorder bytes; everything else is passed through.
inline bool IndexEndianSwaps(xenos::Endian e) {
  return e == xenos::Endian::k8in16 || e == xenos::Endian::k8in32 || e == xenos::Endian::k16in32;
}

inline uint16_t ConvertIndex16(const uint8_t* p, xenos::Endian e) {
  uint16_t v = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
  if (IndexEndianSwaps(e)) {
    v = uint16_t((v >> 8) | (v << 8));
  }
  return v;
}

inline uint32_t ConvertIndex32(const uint8_t* p, xenos::Endian e) {
  uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
  switch (e) {
    case xenos::Endian::k8in16:
      return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case xenos::Endian::k8in32:
      return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
    case xenos::Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}

// Where a draw's vertex indices come from.
//
// `data == nullptr` means the draw is AUTO-INDEXED: there is no guest index
// buffer and vertex N's index is simply N. Otherwise `data` points at the
// guest index buffer (already translated to a host pointer) and each element
// is decoded according to `is_32bit` / `endianness`.
struct GuestIndexSource {
  const uint8_t* data = nullptr;
  bool is_32bit = false;
  xenos::Endian endianness = xenos::Endian::kNone;

  uint32_t Read(uint32_t vertex) const {
    if (!data) {
      return vertex;
    }
    return is_32bit ? ConvertIndex32(data + size_t(vertex) * 4, endianness)
                    : uint32_t(ConvertIndex16(data + size_t(vertex) * 2, endianness));
  }
};

// ---------------------------------------------------------------------------
// Quad lists: 4 vertices per quad -> 2 triangles (6 indices).
// ---------------------------------------------------------------------------
//
// The 0,1,3 / 3,1,2 decomposition is the one the emulating backend's quad
// geometry shader uses (GL_QUAD_STRIP order 0,1,3,2), so winding - and
// therefore face culling - matches that backend exactly.
inline constexpr uint32_t kQuadTriangleCorners[6] = {0, 1, 3, 3, 1, 2};

constexpr uint32_t QuadListQuadCount(uint32_t index_count) { return index_count / 4; }
constexpr uint32_t QuadListExpandedCount(uint32_t index_count) {
  return QuadListQuadCount(index_count) * 6;
}

// Writes QuadListExpandedCount(index_count) indices to `out`. OutT is uint16_t
// or uint32_t; the caller picks the width (see QuadListNeeds32Bit).
template <typename OutT>
void ExpandQuadList(uint32_t index_count, const GuestIndexSource& src, OutT* out) {
  const uint32_t quad_count = QuadListQuadCount(index_count);
  for (uint32_t q = 0; q < quad_count; ++q) {
    const uint32_t base = q * 4;
    for (uint32_t p = 0; p < 6; ++p) {
      *out++ = OutT(src.Read(base + kQuadTriangleCorners[p]));
    }
  }
}

// 32-bit output is needed when the source is 32-bit, or when an auto-indexed
// draw is large enough that vertex indices exceed 16 bits.
inline bool QuadListNeeds32Bit(uint32_t index_count, const GuestIndexSource& src) {
  return src.is_32bit || index_count > 0x10000u;
}

// ---------------------------------------------------------------------------
// Rectangle lists: 3 guest vertices per rectangle -> 2 triangles (6 indices).
// ---------------------------------------------------------------------------
//
// A Xenos rectangle list supplies only THREE corners; the fourth is implied.
// The host vertex shader reconstructs it, which is why the emitted value is
// not a plain vertex index but an ENCODED one:
//
//     (guest primitive index << 2) | corner
//
// with corner in 0..3. The shader (translated with
// HostVertexShaderType::kRectangleListAsTriangleStrip) decodes those low two
// bits to decide which corner it is computing. The strip order 0,1,2,3 is
// emitted as the list 0,1,2, 2,1,3 so no primitive-restart state is needed and
// the winding is identical to the strip's.
//
// Drawing only the first triangle - which is what the backend did before this
// expansion existed - is exact ONLY for the oversized-triangle fullscreen
// trick. A genuine rectangle loses everything beyond the diagonal, which is
// what put a diagonal seam through Hydro Thunder's composite blits.
inline constexpr uint32_t kRectTriangleCorners[6] = {0, 1, 2, 2, 1, 3};

constexpr uint32_t RectangleListRectCount(uint32_t index_count) { return index_count / 3; }
constexpr uint32_t RectangleListExpandedCount(uint32_t index_count) {
  return RectangleListRectCount(index_count) * 6;
}

// Writes RectangleListExpandedCount(index_count) encoded indices to `out`.
//
// NOTE the asymmetry with ExpandQuadList: the emitted value identifies a
// PRIMITIVE and a CORNER, not a vertex, so it is derived from the primitive
// ordinal and never from the guest index buffer. `src` is accepted so DMA-
// indexed rectangle draws can be handled once the shader side consumes real
// vertex indices; today every observed rectangle draw is auto-indexed.
template <typename OutT>
void ExpandRectangleList(uint32_t index_count, const GuestIndexSource& src, OutT* out) {
  (void)src;
  const uint32_t rect_count = RectangleListRectCount(index_count);
  for (uint32_t r = 0; r < rect_count; ++r) {
    for (uint32_t p = 0; p < 6; ++p) {
      *out++ = OutT((r << 2) | kRectTriangleCorners[p]);
    }
  }
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_INDEX_EXPAND_H_
