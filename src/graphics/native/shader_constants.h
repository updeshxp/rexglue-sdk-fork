/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Register-derived vertex-shader state for the native GPU backend.
 *
 * These were hardcoded to zero/one during bring-up, which is invisible on 2D
 * passes (they disable clipping and use no exponent bias) and destroys 3D
 * ones. Pure functions of register values, so they are unit tested rather than
 * eyeballed - see tests/unit/graphics/shader_constants_test.cpp.
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_SHADER_CONSTANTS_H_
#define REX_GRAPHICS_NATIVE_SHADER_CONSTANTS_H_

#include <cstdint>

namespace rex {
namespace graphics {
namespace native {

// How the guest's user clip planes map onto host clip/cull distances.
struct UserClipPlaneConfig {
  uint32_t count = 0;  // number of planes the shader emits, tightly packed
  uint32_t cull = 0;   // 1 = emit as cull distances rather than clip distances

  bool operator==(const UserClipPlaneConfig& o) const {
    return count == o.count && cull == o.cull;
  }
};

// Mirrors vulkan/pipeline_cache.cpp:926-946.
//
// A guest 3D pass typically enables PA_CL_CLIP_CNTL.ucp_ena; a 2D blit pass
// typically sets clip_disable. Forcing count to 0 therefore looks perfectly
// fine on 2D while leaving 3D geometry rasterized unclipped - which is exactly
// how a screen-spanning untextured triangle appears.
constexpr UserClipPlaneConfig ComputeUserClipPlanes(bool clip_disable, uint32_t ucp_ena,
                                                    bool ucp_cull_only_ena,
                                                    bool supports_clip_distance,
                                                    bool supports_cull_distance) {
  uint32_t planes = clip_disable ? 0u : ucp_ena;
  UserClipPlaneConfig cfg;
  if (planes) {
    if (ucp_cull_only_ena && supports_cull_distance) {
      cfg.cull = 1;
    } else if (!supports_clip_distance && supports_cull_distance) {
      cfg.cull = 1;  // fallback when clip distances are unavailable
    } else if (!supports_clip_distance) {
      planes = 0;  // no supported built-in at all
      cfg.cull = 0;
    } else {
      cfg.cull = 0;
    }
  }
  // Popcount of the enabled-plane mask - the shader knows only the total.
  uint32_t count = 0;
  for (uint32_t m = planes & 0x3Fu; m; m &= m - 1) {
    ++count;
  }
  cfg.count = count;
  return cfg;
}

// Mirrors vulkan/pipeline_cache.cpp:956-958.
//
// When the guest shader kills vertices and PA_CL_CLIP_CNTL.vtx_kill_or is
// clear, the kill must go through a cull distance. Forcing this to 0 sends it
// down the OR path instead, which kills by writing NaN to position.w - and in
// the rectangle path one NaN corner poisons the edge-length comparison that
// picks the diagonal, misplacing the whole quad.
constexpr bool ComputeVertexKillAnd(bool supports_cull_distance,
                                    uint32_t writes_point_size_edge_flag_kill_vertex,
                                    bool vtx_kill_or) {
  return supports_cull_distance && (writes_point_size_edge_flag_kill_vertex & 0b100) &&
         !vtx_kill_or;
}

// Mirrors vulkan/command_processor.cpp:6388-6403.
//
// RB_COLOR_INFO bits 20:25 hold a signed exponent bias; the shader multiplies
// colour output by 2^bias. Hardcoding 1.0f blows out any render target that
// carries a nonzero bias.
inline float ColorExpBiasScale(int32_t color_exp_bias) {
  float scale;
  const uint32_t bits = uint32_t(0x3F800000) + (uint32_t(color_exp_bias) << 23);
  __builtin_memcpy(&scale, &bits, sizeof(scale));
  return scale;
}

// The -32..32 -> -1..1 remap the host-render-target path applies to fixed-point
// 16-bit formats, to get the full range at the cost of blending correctness.
constexpr int32_t AdjustColorExpBiasForFormat(int32_t color_exp_bias, bool is_fixed_16_format,
                                              bool truncated_to_minus_1_to_1) {
  return (is_fixed_16_format && !truncated_to_minus_1_to_1) ? color_exp_bias - 5 : color_exp_bias;
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_SHADER_CONSTANTS_H_
