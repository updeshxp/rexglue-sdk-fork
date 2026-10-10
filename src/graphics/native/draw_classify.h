/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * What a guest draw becomes on the native backend.
 *
 * The ordering of these decisions is load-bearing and was got wrong once
 * already: reading IsPixelShaderNeededWithRasterization as "skip the draw"
 * rather than "drop the fragment stage" dropped every depth pre-pass, which
 * left 3D geometry failing its depth test against a cleared buffer - a black
 * world with a correctly drawn HUD.
 *
 * Kept free of registers, shaders and Vulkan so the decision table can be
 * unit tested directly (tests/unit/graphics/draw_classify_test.cpp).
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_DRAW_CLASSIFY_H_
#define REX_GRAPHICS_NATIVE_DRAW_CLASSIFY_H_

#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {
namespace native {

// The facts about a draw that decide its fate. Every field is something the
// caller has already resolved from registers or a translated shader.
struct DrawFacts {
  xenos::EdramMode edram_mode = xenos::EdramMode::kColorDepth;
  // draw_util::IsRasterizationPotentiallyDone
  bool rasterization_possible = true;
  bool vertex_memexport = false;
  bool has_pixel_shader = true;
  // draw_util::IsPixelShaderNeededWithRasterization; only meaningful when
  // has_pixel_shader.
  bool pixel_shader_needed = true;
  bool pixel_memexport = false;
};

enum class DrawDisposition {
  kResolve,               // kCopy mode - hand off to IssueCopy
  kSkipVertexMemexport,   // can't execute memexport natively yet
  kSkipNoRasterization,   // nothing would be rasterized
  kSkipPixelMemexport,    // can't execute memexport natively yet
  kDepthOnly,             // rasterize with NO fragment stage
  kFull,                  // rasterize with the pixel shader
};

// Mirrors the emulating backend's prologue
// (vulkan/command_processor.cpp:3741-3800). Two orderings matter:
//
//  - Vertex memexport is rejected ABOVE the rasterization test. The oracle
//    discards on "!rasterization && !vertex_memexport"; since the native
//    backend cannot execute memexport at all, rejecting those first lets the
//    rasterization test stand in for the combined condition.
//  - Pixel memexport is tested BELOW the nulling. A pixel shader that was
//    dropped cannot memexport, so testing it first would discard draws the
//    oracle renders.
constexpr DrawDisposition ClassifyDraw(const DrawFacts& f) {
  if (f.edram_mode == xenos::EdramMode::kCopy) {
    return DrawDisposition::kResolve;
  }
  if (f.vertex_memexport) {
    return DrawDisposition::kSkipVertexMemexport;
  }
  if (!f.rasterization_possible) {
    return DrawDisposition::kSkipNoRasterization;
  }
  // Only kColorDepth can keep a fragment stage. kDepthOnly is the guest asking
  // for depth/stencil with no colour output at all.
  if (f.edram_mode != xenos::EdramMode::kColorDepth || !f.has_pixel_shader ||
      !f.pixel_shader_needed) {
    return DrawDisposition::kDepthOnly;
  }
  if (f.pixel_memexport) {
    return DrawDisposition::kSkipPixelMemexport;
  }
  return DrawDisposition::kFull;
}

constexpr bool IsSkip(DrawDisposition d) {
  return d == DrawDisposition::kSkipVertexMemexport ||
         d == DrawDisposition::kSkipNoRasterization ||
         d == DrawDisposition::kSkipPixelMemexport;
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_DRAW_CLASSIFY_H_
