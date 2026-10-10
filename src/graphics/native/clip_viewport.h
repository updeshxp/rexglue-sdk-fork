/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Host viewport extent for guest draws with clipping disabled.
 *
 * With PA_CL_CLIP_CNTL.clip_disable the vertex shader emits positions in
 * pixels, and GetHostViewportInfo turns `x_max`/`y_max` into the host
 * viewport those pixels are measured against. The extent has to be the
 * surface being drawn *now*.
 *
 * The native backend has no render-target cache, so an earlier attempt used
 * the last resolve's destination texture size for the EDRAM base. That size
 * belongs to the previous surface: SF3 reuses EDRAM base 0 for 384x224,
 * 768x448, 1536x896 and 1280x720 targets in one frame, and the logs show
 * clip-disabled draws (surface pitch 640) being given 1280x720 or 1536x896.
 * A pixel coordinate then lands at the wrong host pixel.
 *
 * Width comes from RB_SURFACE_INFO.surface_pitch, which is the current
 * surface. Height comes from a resolve of that same width when one exists,
 * otherwise from a valid window scissor, otherwise from the guest video mode.
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_
#define REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_

#include <cstdint>

#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {
namespace native {

struct ClipDisabledExtent {
  uint32_t x = 1;
  uint32_t y = 1;
  // Diagnostic label, stable for the viewport log.
  const char* source = "video_mode";
};

// `resolve_w/h` is the EDRAM rect of the last resolve of this base, or 0 if
// this base has not been resolved yet. `surface_pitch` is
// RB_SURFACE_INFO.surface_pitch for the draw. Scissor edges are valid when
// they are non-zero and below the 8192 sentinel Direct3D 9 writes.
//
// Priority: resolve_w/resolve_h first, then surface_pitch, then scissor, then
// video mode as a last resort.
//
// `surface_pitch` was tried as the top-priority source (ahead of the resolve
// lookup) to guard against a stale resolve for a reused EDRAM base - but
// RB_SURFACE_INFO is sticky GPU state, and a clip-disabled fullscreen-triangle
// draw does not necessarily rewrite it itself before drawing, so it can (and
// in production logs, consistently does) hold a leftover value from whatever
// other pass last touched that register, not this draw's actual target. A
// full session capture with surface_pitch as top priority produced vp=640x720
// (half the real 1280x720 target) on every single clip-disabled draw. The
// resolve-extent pair, by contrast, is internally self-consistent - both
// values come from the same recorded resolve - and was verified correct
// (matching the working emulated-backend reference, zero anomalies across a
// full session) when given top priority. Keep surface_pitch as a fallback for
// when no resolve exists yet, not an override of one that does.
inline ClipDisabledExtent ChooseClipDisabledExtent(uint32_t surface_pitch, uint32_t resolve_w,
                                                   uint32_t resolve_h, bool scissor_x_valid,
                                                   uint32_t scissor_right, bool scissor_y_valid,
                                                   uint32_t scissor_bottom, uint32_t video_w,
                                                   uint32_t video_h) {
  constexpr uint32_t kSentinel = xenos::kTexture2DCubeMaxWidthHeight;
  auto bounded = [](uint32_t v) { return v >= 1 && v < kSentinel; };

  ClipDisabledExtent out;
  out.x = bounded(video_w) ? video_w : 1;
  out.y = bounded(video_h) ? video_h : 1;
  out.source = "video_mode";

  if (bounded(resolve_w)) {
    out.x = resolve_w;
    out.source = "resolve_width";
  } else if (bounded(surface_pitch)) {
    out.x = surface_pitch;
    out.source = "surface_pitch";
  } else if (scissor_x_valid && bounded(scissor_right)) {
    out.x = scissor_right;
    out.source = "scissor_x";
  }

  // The resolve height is only meaningful paired with its own resolve width -
  // only trust it when that's what we actually picked for X above.
  if (bounded(resolve_w) && bounded(resolve_h) && out.source == "resolve_width") {
    out.y = resolve_h;
    out.source = "resolve_matched_width";
  } else if (scissor_y_valid && bounded(scissor_bottom)) {
    out.y = scissor_bottom;
    if (out.source == "video_mode" || out.source == "surface_pitch") {
      out.source = "scissor_y";
    }
  }
  return out;
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_
