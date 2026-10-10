/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Host viewport for guest draws with PA_CL_CLIP_CNTL.clip_disable.
 *
 * The working Vulkan backend passes VkPhysicalDeviceLimits::maxViewportDimensions
 * into GetHostViewportInfo. That function then caps each axis at
 * xenos::kTexture2DCubeMaxWidthHeight (8192). Guest pixel positions stay 1:1
 * with host pixels for any extent, until the extent itself clips them.
 *
 * SF3's clip-disabled rectangle draws report RB_SURFACE_INFO.surface_pitch 640
 * while the following resolve of that EDRAM base is 1280x720 (native log:
 * draw #27 at startup, draw #177757 during a fight, resolve 1280x720). Using
 * the pitch as the viewport clips that resolve to its left 640 pixels. The
 * last resolve of the EDRAM base is also the wrong size: base 0 is reused for
 * 384x224, 768x448, 1536x896 and 1280x720 in one frame.
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_
#define REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_

#include <cstdint>

#include "rex/graphics/xenos.h"

namespace rex {
namespace graphics {
namespace native {

struct ClipDisabledViewportMax {
  uint32_t x = 1;
  uint32_t y = 1;
};

// Device viewport limits. `surface_pitch` and the last resolve size are
// accepted so callers cannot quietly start using them again; they do not
// affect the result. GetHostViewportInfo caps these at 8192.
inline ClipDisabledViewportMax HostClipDisabledViewportMax(uint32_t device_max_x,
                                                          uint32_t device_max_y,
                                                          uint32_t surface_pitch,
                                                          uint32_t resolve_w,
                                                          uint32_t resolve_h) {
  (void)surface_pitch;
  (void)resolve_w;
  (void)resolve_h;
  ClipDisabledViewportMax out;
  out.x = device_max_x ? device_max_x : 1;
  out.y = device_max_y ? device_max_y : 1;
  return out;
}

// GetHostViewportInfo's clip-disabled extent. A resolve target must not be
// grown to this range.
inline bool IsHostClipRangeExtent(uint32_t extent) {
  return extent >= xenos::kTexture2DCubeMaxWidthHeight;
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_CLIP_VIEWPORT_H_
