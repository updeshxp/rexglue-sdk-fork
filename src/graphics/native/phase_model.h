/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * The EDRAM phase model for the native GPU backend.
 *
 * The native backend has no EDRAM. A guest frame is a sequence of render-target
 * "phases", each ended by a resolve (IssueCopy) naming a destination address:
 * offscreen phases (imposter atlases, reflections, bloom) resolve to textures
 * later draws sample, and the visible scene resolves to the frontbuffer address
 * that IssueSwap displays.
 *
 * Deciding which deferred draws a phase OWNS, and which phases reach the
 * screen, is the subtle part - and it is pure arithmetic over recorded state,
 * so it lives here where it can be unit tested without a GPU or a running
 * title (tests/unit/graphics/phase_model_test.cpp).
 ******************************************************************************
 */

#ifndef REX_GRAPHICS_NATIVE_PHASE_MODEL_H_
#define REX_GRAPHICS_NATIVE_PHASE_MODEL_H_

#include <cstdint>
#include <vector>

namespace rex {
namespace graphics {
namespace native {

// Guest addresses are matched at 4KB page granularity across the EDRAM-
// addressable range. Both resolve destinations and texture fetch addresses go
// through this, so an alias only hits when the two agree exactly.
inline constexpr uint32_t kEdramAddressMask = 0x1FFFF000u;

constexpr uint32_t ResolvedTargetKey(uint32_t guest_byte_address) {
  return guest_byte_address & kEdramAddressMask;
}

// "Every base" - used by the replay-all fallback and by trailing draws that no
// resolve has claimed, neither of which can name a single EDRAM base.
inline constexpr uint32_t kAnyBase = UINT32_MAX;

// One resolve's claim on the deferred-draw list.
struct PhaseSpan {
  uint32_t src_base = 0;    // EDRAM base the phase rendered into
  uint32_t first_draw = 0;  // inclusive
  uint32_t end_draw = 0;    // exclusive
  uint32_t dest_key = 0;    // ResolvedTargetKey of the resolve destination
};

// A slice of the deferred-draw list to replay into the swap image, filtered to
// one EDRAM base (or kAnyBase for "no filtering").
struct DisplayRange {
  uint32_t first_draw = 0;
  uint32_t end_draw = 0;
  uint32_t src_base = kAnyBase;

  bool operator==(const DisplayRange& o) const {
    return first_draw == o.first_draw && end_draw == o.end_draw && src_base == o.src_base;
  }
};

// Where a phase's ownership starts.
//
// A resolve copies EDRAM out WITHOUT clearing it, so consecutive resolves of
// the same base are progressive: a post-process chain that resolves base B
// after 119 draws and again after 1 more still has all 120 in EDRAM the second
// time. But EDRAM is also a scratchpad REUSED within a frame - base 832 serves
// four different 1024x1024 imposter atlases in sequence - so a phase owns only
// the draws since that base was last resolved, otherwise each atlas
// accumulates the ones before it.
//
// Ownership therefore begins at whichever came later: the base's last resolve,
// or the last clear of that base.
constexpr uint32_t PhaseFirstDraw(bool has_clear_point, uint32_t clear_point,
                                  bool has_last_resolve, uint32_t last_resolve) {
  const uint32_t a = has_clear_point ? clear_point : 0u;
  const uint32_t b = has_last_resolve ? last_resolve : 0u;
  return a > b ? a : b;
}

// Whether a deferred draw participates in a range.
//
// Depth-only draws bypass the base filter: they carry no meaningful colour base
// (RB_COLOR_INFO is whatever it happened to hold), and the backend keeps one
// depth buffer per render pass rather than one per EDRAM base, so within a pass
// "depth is shared scratch" is the honest model.
constexpr bool DrawInRange(uint32_t draw_color_base, bool draw_depth_only, uint32_t range_base) {
  return range_base == kAnyBase || draw_depth_only || draw_color_base == range_base;
}

// Chooses what reaches the screen.
//
// - Phases whose destination is the frontbuffer replay, filtered to their base.
// - Draws after the last resolve have no phase, so they replay unfiltered.
// - If NO phase resolved to the frontbuffer, the whole frame replays
//   unfiltered - the baseline behaviour for titles that never resolve
//   (Geometry Wars class).
inline std::vector<DisplayRange> SelectDisplayRanges(const std::vector<PhaseSpan>& phases,
                                                     uint32_t fb_key, uint32_t deferred_count,
                                                     uint32_t phase_first_draw) {
  std::vector<DisplayRange> ranges;
  bool any_fb_phase = false;
  for (const PhaseSpan& p : phases) {
    if (p.dest_key == fb_key && p.end_draw > p.first_draw) {
      any_fb_phase = true;
      ranges.push_back({p.first_draw, p.end_draw, p.src_base});
    }
  }
  if (deferred_count > phase_first_draw) {
    ranges.push_back({phase_first_draw, deferred_count, kAnyBase});
  }
  if (!any_fb_phase) {
    ranges.assign(1, {0u, deferred_count, kAnyBase});
  }
  return ranges;
}

// What the swap image should be built from.
//
// The frontbuffer is an ADDRESS holding a finished image, not a list of draws.
// When a phase has resolved to the address IssueSwap names, the guest has
// already produced the frame and the faithful thing to do is present that
// image - which is what the hardware, and the emulating backend, do.
//
// Replaying the draws instead is only an approximation, and it is a poor one:
// the draws that produced the frame may target an EDRAM base the replay filter
// rejects, or be a composite that cannot be re-run against the swap image. It
// is measurably wrong - SoulCalibur II, Ridge Racer 6, OutRun, Bionic Commando
// 1 and 2, Rainbow Islands and SoulCalibur IV all replay to a BLACK frame
// (mean 0.000333, byte-identical across all seven) while resolving perfectly
// good images to their frontbuffer addresses.
//
// Trailing draws - those issued after the last resolve - still replay on top,
// since a HUD drawn after the final resolve is not in the resolved image.
struct DisplaySource {
  // Present the resolved image aliased at resolved_key, then replay `ranges`.
  bool present_resolved = false;
  uint32_t resolved_key = 0;
  std::vector<DisplayRange> ranges;
};

inline DisplaySource ChooseDisplaySource(const std::vector<PhaseSpan>& phases, uint32_t fb_key,
                                         uint32_t deferred_count, uint32_t phase_first_draw) {
  DisplaySource out;
  for (const PhaseSpan& p : phases) {
    if (p.dest_key == fb_key && p.end_draw > p.first_draw) {
      out.present_resolved = true;
      out.resolved_key = fb_key;
    }
  }
  if (out.present_resolved) {
    // Only what came after the last resolve; everything before it is already
    // baked into the image being presented.
    if (deferred_count > phase_first_draw) {
      out.ranges.push_back({phase_first_draw, deferred_count, kAnyBase});
    }
    return out;
  }
  // No resolve reached the frontbuffer: fall back to the replay model.
  out.ranges = SelectDisplayRanges(phases, fb_key, deferred_count, phase_first_draw);
  return out;
}

// Two phases in ONE frame that resolve to the same destination address.
//
// Resolved images are keyed by destination address and reused, so colliding
// phases share a single image: the later one overwrites the earlier, and any
// draw that sampled the earlier result gets the wrong content (or, when the
// sizes differ, an image that was reallocated out from under it). Hydro
// Thunder does exactly this - a 512x576 phase and a 1024x576 phase both resolve
// to 0x1690F000 in the same frame.
//
// Returns the indices of phases that are NOT the last writer of their
// destination, i.e. the ones whose content a shared image would lose.
inline std::vector<size_t> FindOverwrittenPhases(const std::vector<PhaseSpan>& phases) {
  std::vector<size_t> overwritten;
  for (size_t i = 0; i < phases.size(); ++i) {
    if (phases[i].end_draw <= phases[i].first_draw) {
      continue;  // empty phase, nothing to lose
    }
    for (size_t j = i + 1; j < phases.size(); ++j) {
      if (phases[j].dest_key == phases[i].dest_key &&
          phases[j].end_draw > phases[j].first_draw) {
        overwritten.push_back(i);
        break;
      }
    }
  }
  return overwritten;
}

}  // namespace native
}  // namespace graphics
}  // namespace rex

#endif  // REX_GRAPHICS_NATIVE_PHASE_MODEL_H_
