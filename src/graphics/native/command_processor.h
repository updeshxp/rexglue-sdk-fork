/**
 * @file        graphics/native/command_processor.h
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     NativeCommandProcessor derives from the shared, game-agnostic
 *              rex::graphics::CommandProcessor. The base class does ALL PM4
 *              ring-buffer parsing and hands us fully-decoded work through the
 *              virtual seams (SetupContext, LoadShader, IssueDraw, IssueCopy,
 *              IssueSwap).
 *
 *              PHASE 2 STATUS: LoadShader translates Xenos microcode -> SPIR-V
 *              (reusing SpirvShaderTranslator); IssueDraw builds a real Vulkan
 *              graphics pipeline from register-derived state, binds shared
 *              memory + constant buffers (the SPIR-V does in-shader vertex fetch
 *              from shared memory - no classic vertex input), records viewport /
 *              index / draw state into a per-frame deferred draw list; IssueSwap
 *              replays those draws into the presenter's guest-output image and
 *              presents it. Textured draws are skipped for now (Phase 3).
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/ui/vulkan/device.h>
#include "native/texture_cache.h"
#endif

namespace rex::graphics {
class SpirvShaderTranslator;
class SpirvShader;
}  // namespace rex::graphics

namespace rex::graphics::native {

class NativeGraphicsSystem;
class NativeSharedMemory;

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                         system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  // Texture fetch constants are the guest's texture bindings. The texture cache
  // keeps a sticky "in sync" mask per fetch slot and only re-resolves a slot
  // when told the constant changed - so these MUST be forwarded, or the first
  // texture bound to a slot in a frame is reused by every later draw in that
  // frame (an entire title ends up sampling one atlas). The ring path funnels
  // into WriteRegistersFromMem, so overriding these two covers all writes.
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;

  // Trace / save-state seams (no-ops until the native backend owns memory).
  // TracePlaybackWroteMemory/RestoreEdramSnapshot existed for the trace
  // player, which 0.10 does not have.

  // Present seam. Native renderer will hand a host image to the presenter here.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;

  bool IssueCopy() override;

 private:
  // Shader cache keyed by microcode hash. Stores SpirvShader instances (parsed
  // microcode + translated SPIR-V translations).
  std::vector<std::unique_ptr<Shader>> shader_storage_;
  std::unordered_map<uint64_t, Shader*> shader_map_;

  uint64_t draw_count_ = 0;
  uint64_t copy_count_ = 0;
  uint64_t swap_count_ = 0;
  uint64_t deferred_draw_total_ = 0;
  uint64_t skipped_draw_total_ = 0;

#if REX_HAS_VULKAN
  // ----- Phase 1 clear/present state (reused by Phase 2 as the guest-draw
  // render pass and replay submission) -----
  bool CreateClearResources();
  void DestroyClearResources();
  // The guest's own render targets are frequently HDR (Choplifter and PGR3
  // render roughly half their draws to k_2_10_10_10_FLOAT, whose range runs to
  // ~32), and the guest then tone-maps them itself. Rendering into an 8-bit
  // UNORM target clamps everything above 1.0 BEFORE that tone-map pass runs,
  // which is why those titles' bright surfaces come out flat white. So every
  // guest draw lands in a float scene image, and only the finished frame is
  // blitted down to the presenter's swap image.
  static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr uint32_t kSceneColorBytesPerPixel = 8;
  bool EnsureSceneFramebuffer(uint32_t width, uint32_t height);
  void DestroySceneFramebuffer();
  // Colour-LOAD variant of clear_render_pass_, for compositing trailing draws
  // over a presented resolved frontbuffer image.
  VkRenderPass load_render_pass_ = VK_NULL_HANDLE;
  VkImage scene_color_ = VK_NULL_HANDLE;
  VkDeviceMemory scene_color_memory_ = VK_NULL_HANDLE;
  VkImageView scene_color_view_ = VK_NULL_HANDLE;
  VkFramebuffer scene_framebuffer_ = VK_NULL_HANDLE;
  uint32_t scene_width_ = 0;
  uint32_t scene_height_ = 0;

  bool EnsureClearFramebuffer(VkImageView image_view, uint64_t image_version, uint32_t width,
                              uint32_t height);

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  VkRenderPass clear_render_pass_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence clear_fence_ = VK_NULL_HANDLE;

  VkFramebuffer clear_framebuffer_ = VK_NULL_HANDLE;
  VkImageView clear_framebuffer_view_ = VK_NULL_HANDLE;
  uint64_t clear_framebuffer_version_ = UINT64_MAX;
  uint32_t clear_framebuffer_width_ = 0;
  uint32_t clear_framebuffer_height_ = 0;

  uint32_t last_logged_clear_raw_ = 0xFFFFFFFFu;

  // ----- Phase 2 draw state -----
  // A per-frame growing host-visible buffer with a bump allocator, reset each
  // frame. Used for constant UBOs and converted index buffers.
  struct HostRingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize cursor = 0;
  };

  // One deferred draw captured during IssueDraw, replayed at IssueSwap.
  struct DeferredDraw {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    VkDescriptorSet vertex_texture_set = VK_NULL_HANDLE;
    VkDescriptorSet pixel_texture_set = VK_NULL_HANDLE;
    VkViewport viewport = {};
    VkRect2D scissor = {};
    bool indexed = false;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    uint32_t draw_count = 0;
    // RT0's EDRAM base (RB_COLOR_INFO.color_base, in tiles) at defer time -
    // identifies which guest render target this draw wrote, so resolves can
    // select exactly the draws that contributed to their EDRAM region.
    uint32_t color_edram_base = 0;
    // No fragment stage: this draw contributes depth only. Its color_edram_base
    // is whatever RB_COLOR_INFO happened to hold and is therefore meaningless,
    // so base filtering must not be applied to it (see RecordDeferredDrawsForBase).
    bool depth_only = false;
    // RB_DEPTH_INFO.depth_base at defer time. Not used for selection yet -
    // recorded so that grouping depth-only draws by their own EDRAM base stays a
    // small change if admitting them to any containing phase proves too loose.
    uint32_t depth_edram_base = 0;
    // Normalized RB_DEPTHCONTROL at defer time (z_enable / zfunc / z_write).
    // Diagnostic only: the phase's depth attachment is cleared to a fixed 1.0,
    // so a guest using reverse-Z (clear 0.0 + GREATER) would fail every 3D
    // depth test while 2D UI with depth disabled still draws.
    uint32_t depth_control_raw = 0;
    // RB_BLEND_RED/GREEN/BLUE/ALPHA at defer time, replayed as Vulkan's
    // dynamic blend constants.
    float blend_constants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  };

  bool CreateDrawResources();
  void DestroyDrawResources();
  bool CreateHostRingBuffer(VkBufferUsageFlags usage, VkDeviceSize size, HostRingBuffer& out);
  void DestroyHostRingBuffer(HostRingBuffer& ring);
  // Sub-allocates from a host ring buffer. Returns nullptr on overflow.
  uint8_t* RingAllocate(HostRingBuffer& ring, VkDeviceSize bytes, VkDeviceSize alignment,
                        VkBuffer& buffer_out, VkDeviceSize& offset_out);
  void BeginFrameIfNeeded();

  // Register-derived graphics pipeline state (Phase 3: real blend / depth / cull
  // instead of the Phase 2 hardcoded blend-off / depth-off / cull-none).
  struct GuestPipelineState {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Color blend (render target 0).
    bool blend_enable = false;
    VkBlendFactor src_color_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Depth.
    bool depth_test_enable = false;
    bool depth_write_enable = false;
    VkCompareOp depth_compare_op = VK_COMPARE_OP_ALWAYS;
    // Rasterizer.
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    uint64_t Hash() const;
  };

  // Returns the cached VkShaderModule for a translation, creating it on first
  // use. Returns VK_NULL_HANDLE on failure.
  VkShaderModule GetShaderModule(const Shader::Translation* translation);
  // Returns (creating if needed) a graphics pipeline for the given state.
  // geometry_module is optional (VK_NULL_HANDLE = no geometry stage) and is
  // folded into the pipeline cache key alongside the other stage modules.
  VkPipeline GetPipeline(VkShaderModule vertex_module, VkShaderModule pixel_module,
                         VkPipelineLayout layout, const GuestPipelineState& state,
                         VkShaderModule geometry_module = VK_NULL_HANDLE);
  // Xenos kRectangleList gives 3 corners per rect with the 4th implied - same
  // as kPointList/kQuadList, it has no direct Vulkan topology. The mature
  // Vulkan backend (vulkan/pipeline_cache.cpp) expands these with a real
  // geometry shader (triangle in, triangle-strip quad out); this reuses that
  // exact, already-validated SPIR-V builder
  // (VulkanPipelineCache::BuildGeometryShaderModule) instead of the
  // vertex-shader-loop fallback, which was never a shipped/validated Xenia
  // path (see native_expand_rects). Returns VK_NULL_HANDLE on build failure.
  VkShaderModule GetGeometryShader(vulkan::VulkanPipelineCache::GeometryShaderKey key);
  // Fills a GuestPipelineState from the current register file + primitive type.
  GuestPipelineState BuildPipelineState(VkPrimitiveTopology topology, bool primitive_polygonal,
                                        const reg::RB_DEPTHCONTROL& depth_control,
                                        uint32_t pixel_writes_color_targets) const;

  // Texture-path helpers. Textured guest draws are rendered with a dummy white
  // texture bound to every image/sampler binding the shader declares, so the
  // geometry shape appears (flat/vertex-coloured) instead of being skipped.
  bool CreateDummyTextures();
  VkImageView DummyViewForDimension(xenos::FetchOpDimension dimension) const;
  // Texture descriptor set layout for a given image + sampler binding count.
  VkDescriptorSetLayout GetTextureSetLayout(uint32_t texture_count, uint32_t sampler_count);
  // Guest pipeline layout for the four per-stage texture/sampler counts.
  VkPipelineLayout GetGuestPipelineLayout(uint32_t vertex_texture_count,
                                          uint32_t vertex_sampler_count,
                                          uint32_t pixel_texture_count,
                                          uint32_t pixel_sampler_count);
  // Allocates and fills a per-draw texture descriptor set with dummy textures.
  VkDescriptorSet AllocateDummyTextureSet(const SpirvShader* shader, VkDescriptorSetLayout layout);
  // Allocates and fills a per-draw texture descriptor set with REAL guest
  // textures + samplers from the texture cache (Phase 3). Falls back to the dummy
  // white texture / default sampler for any binding with no valid texture.
  VkDescriptorSet AllocateTextureSet(SpirvShader* shader, VkDescriptorSetLayout layout);

  // Guest-shader descriptor layout (matches SpirvShaderTranslator):
  //   set 0 = shared memory storage buffer(s); set 1 = 5 constant UBOs;
  //   set 2 = vertex textures; set 3 = pixel textures.
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout texture_set_layout_empty_ = VK_NULL_HANDLE;

  VkDescriptorPool shared_memory_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_descriptor_set_ = VK_NULL_HANDLE;
  uint32_t shared_memory_binding_count_ = 1;

  // Dummy white textures (one per Xenos image dimension) + a default sampler.
  VkImage dummy_image_2d_array_ = VK_NULL_HANDLE;   // covers 1D/2D (Dim2D arrayed)
  VkImageView dummy_view_2d_array_ = VK_NULL_HANDLE;
  VkImage dummy_image_3d_ = VK_NULL_HANDLE;
  VkImageView dummy_view_3d_ = VK_NULL_HANDLE;
  VkImage dummy_image_cube_ = VK_NULL_HANDLE;
  VkImageView dummy_view_cube_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_memory_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkSampler dummy_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSet empty_texture_set_ = VK_NULL_HANDLE;

  // Cached texture set layouts (key: texture_count<<16 | sampler_count) and
  // guest pipeline layouts (key: packed four per-stage counts).
  std::unordered_map<uint32_t, VkDescriptorSetLayout> texture_set_layouts_;
  std::unordered_map<uint64_t, VkPipelineLayout> pipeline_layouts_;

  // Per-frame constant + texture descriptor sets, reset each frame.
  VkDescriptorPool constants_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorPool texture_descriptor_pool_ = VK_NULL_HANDLE;
  static constexpr uint32_t kMaxDrawsPerFrame = 8192;

  std::unique_ptr<NativeSharedMemory> shared_memory_;
  std::unique_ptr<NativeTextureCache> texture_cache_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  // ----- Phase 3 depth buffer (guest-output sized, transient, cleared each
  // frame). Added so 3D scenes occlude correctly (no EDRAM emulation). -----
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
  bool EnsureDepthResources(uint32_t width, uint32_t height);
  void DestroyDepthResources();
  VkImage depth_image_ = VK_NULL_HANDLE;
  VkImageView depth_view_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
  uint32_t depth_width_ = 0;
  uint32_t depth_height_ = 0;

  // ----- Phase 2 (native): EDRAM resolve -> sampled texture -----
  // The native backend has no EDRAM; deferred draws replay into the swap image
  // at IssueSwap. A guest frame is a sequence of render-target "phases", each
  // ended by a resolve (IssueCopy) that names its destination address: offscreen
  // passes (imposter atlases, reflections, bloom) resolve to textures that later
  // draws sample, and the visible scene resolves to the frontbuffer address that
  // IssueSwap then displays.
  //
  // IssueCopy is CPU-only: it records the phase boundary, creates the phase's
  // "resolved" image/view, and aliases the view at the destination address so
  // later same-frame draws bind it in their descriptor sets. All GPU work stays
  // in IssueSwap's single submit: offscreen phases render into resolve_rt_ and
  // are copied into their resolved images first, then only the draws belonging
  // to frontbuffer phases (plus any trailing unresolved draws) replay into the
  // swap image. A frame with no resolves at all keeps the original replay-all
  // path (Geometry Wars class), as does a frame whose resolves never match the
  // frontbuffer address.
  struct ResolvedTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    // Tracked so a copy into a subrect can PRESERVE the rest of the image
    // (oldLayout must be the real current layout - UNDEFINED discards).
    // Updated CPU-side while recording; the image is UNDEFINED when created,
    // SHADER_READ_ONLY after each frame's phase copy.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  // Resolved targets PERSIST ACROSS FRAMES, keyed by the destination TEXTURE's
  // base address, and are allocated at the guest texture's declared layout size
  // (pitch x height), not the resolve rect's size. Each resolve rect is copied
  // to its position inside the image, so a texture built from several resolves
  // (EDRAM tiling: Hydro Thunder's scene = two 512x576 strips into one
  // 1024x576 texture) assembles correctly and normalized UVs computed against
  // the guest layout sample it correctly.
  // Render-to-texture is frequently cross-frame: a title resolves a target in
  // one frame and samples it in a later one (OutRun's menu does exactly this).
  // Since the native backend never writes resolve results back into guest
  // memory, freeing these per frame left such a sample reading memory that was
  // never written - i.e. black. Reuse the image when the same address is
  // re-resolved at the same size; only reallocate when the size changes.
  size_t AcquireResolvedTarget(uint32_t dest_key, uint32_t width, uint32_t height);
  // One render-target phase: a resolve of EDRAM base src_base to dest_key.
  //
  // A resolve copies EDRAM out without clearing it, so consecutive resolves of
  // the same base are progressive - a post-process chain resolves base B after
  // 119 draws, then again after 1 more draw, and the second resolve still
  // contains all 120. The phase therefore owns every draw that targeted
  // src_base in [first_draw, end_draw), where first_draw is where that base was
  // last cleared, NOT simply the draws since the previous resolve.
  //
  // resolved_index is the phase's image in resolved_target_storage_, or
  // SIZE_MAX if the capture was skipped (cap hit / allocation failure).
  struct RenderPhase {
    uint32_t src_base = 0;
    uint32_t first_draw = 0;
    uint32_t end_draw = 0;
    uint32_t dest_key = 0;
    uint32_t rect_w = 0;
    uint32_t rect_h = 0;
    // Where the resolve rect lands inside the destination texture (and so
    // inside the layout-sized resolved image).
    uint32_t dest_x = 0;
    uint32_t dest_y = 0;
    size_t resolved_index = SIZE_MAX;
    // This phase rendered no new draws; it re-publishes an earlier resolve of
    // the same EDRAM base to a second address (typically the frontbuffer).
    bool republished = false;
    // The guest asked this resolve to clear the EDRAM surface afterwards. A
    // resolve that does NOT clear leaves the surface live, and the draws that
    // follow composite onto what is already there.
    bool clears_color = false;
  };
  // Per EDRAM base, the last resolve that actually captured an image, so a
  // later zero-draw resolve of that base can republish it.
  struct PublishedPhase {
    size_t index = SIZE_MAX;
    uint32_t first_draw = 0;
    uint32_t end_draw = 0;
  };
  std::unordered_map<uint32_t, PublishedPhase> base_last_published_;
  bool EnsureResolveRenderTarget(uint32_t width, uint32_t height);
  void DestroyResolveRenderTarget();
  bool EnsureResolveStaging(VkDeviceSize size);
  void ResetResolvedTargets();
  // Records deferred_draws_[first, first+count) into the currently-bound
  // command buffer / render pass (offscreen resolve RT).
  void RecordDeferredDrawRange(VkCommandBuffer cb, uint32_t first, uint32_t count, uint32_t width,
                               uint32_t height);
  // As above over [first, end), but only the draws that targeted EDRAM base
  // `base` - the draws a resolve of that base actually captures. Returns the
  // number of draws recorded.
  uint32_t RecordDeferredDrawsForBase(VkCommandBuffer cb, uint32_t first, uint32_t end,
                                      uint32_t base, uint32_t width, uint32_t height);
  // Returns the resolved image view aliased at a guest byte address, or null.
  VkImageView ResolvedViewForAddress(uint32_t guest_byte_address) const;
  // Debug: writes each of this frame's resolved render targets to a PPM, so the
  // contents of every offscreen phase can be inspected directly. Enabled with
  // REX_DUMP_RT=<prefix>, at the swap given by REX_DUMP_RT_SWAP.
  void DumpResolvedTargets();

  VkImage resolve_rt_color_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_rt_color_memory_ = VK_NULL_HANDLE;
  VkImageView resolve_rt_color_view_ = VK_NULL_HANDLE;
  VkImage resolve_rt_depth_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_rt_depth_memory_ = VK_NULL_HANDLE;
  VkImageView resolve_rt_depth_view_ = VK_NULL_HANDLE;
  VkFramebuffer resolve_rt_framebuffer_ = VK_NULL_HANDLE;
  uint32_t resolve_rt_width_ = 0;
  uint32_t resolve_rt_height_ = 0;
  VkBuffer resolve_staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_staging_memory_ = VK_NULL_HANDLE;
  VkDeviceSize resolve_staging_size_ = 0;
  // Per-frame resolved images (freed at BeginFrameIfNeeded) and their address
  // aliases (key: guest byte address masked to the physical page range).
  std::vector<ResolvedTarget> resolved_target_storage_;
  std::unordered_map<uint32_t, VkImageView> resolved_target_views_;
  // dest_key -> index into resolved_target_storage_, so a re-resolve of the
  // same address reuses its image instead of leaking a new one every frame.
  std::unordered_map<uint32_t, size_t> resolved_target_index_;
  // Guest layout of each resolved destination texture, so a resolve whose
  // RB_COPY_DEST_BASE lies INSIDE an already-known texture can be attributed
  // to it. D3D9 expresses a rect resolve at (x,y) by pre-adding the tiled
  // offset of the 32-aligned part of (x,y) to the base register (see
  // GetResolveInfo), so an EDRAM tiling strip resolved into the right half of
  // a texture arrives with a base no fetch constant ever samples. The
  // position is recovered by inverting GetTiledOffset2D over the texture's
  // 32x32 granule grid.
  struct ResolvedTextureLayout {
    uint32_t base_raw = 0;       // unmasked RB_COPY_DEST_BASE that established it
    uint32_t pitch_aligned = 0;  // guest layout, 32-aligned pixels
    uint32_t height_aligned = 0;
    uint32_t bpp_log2 = 0;
    uint32_t img_w = 0;  // host image dims used for this texture
    uint32_t img_h = 0;
  };
  std::unordered_map<uint32_t, ResolvedTextureLayout> resolved_texture_layouts_;
  // Native's no-EDRAM path uses resolves as the only authoritative signal for
  // the guest surface extent of each EDRAM base. Clip-disabled fullscreen / blit
  // draws intentionally ask GetHostViewportInfo for the maximum render-target
  // extent to synthesize a host viewport; passing the Vulkan device maximum
  // there makes those draws 8192x8192. Keep the last resolved size for each
  // source base so subsequent draws to that base use the bound guest surface
  // size instead.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> edram_base_surface_extents_;
  // Resolved rect size per alias key, to compare against the size the guest's
  // fetch constant claims the texture at that address is.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> resolved_target_dims_;
  // The frame's completed render-target phases, in guest submission order.
  std::vector<RenderPhase> phases_;
  // Per-EDRAM-base draw index at which that base was last cleared this frame -
  // the start of the draw range a resolve of that base captures.
  std::unordered_map<uint32_t, uint32_t> base_clear_point_;
  // Per-EDRAM-base draw index of the previous resolve of that base. EDRAM is a
  // scratchpad: one base is reused for several unrelated targets in a frame
  // (e.g. four imposter atlases at base 832), so a resolve owns only the draws
  // since that base was last resolved - otherwise every atlas accumulates the
  // ones before it. Progressive post-process chains still work because each
  // pass is a fullscreen quad sampling the previous pass's resolved image.
  std::unordered_map<uint32_t, uint32_t> base_last_resolve_;
  // Destinations already resolved during THIS frame. A second resolve to the
  // same address must get its own image rather than reusing (and destroying)
  // one that earlier draws in this frame already reference.
  std::unordered_set<uint32_t> acquired_dest_keys_this_frame_;
  // Slots displaced by such a second resolve. They stay alive until the next
  // frame boundary, by which point this frame's submit has been fenced.
  std::vector<size_t> retired_resolved_slots_;
  // Index into deferred_draws_ where the current render-target phase began.
  uint32_t phase_first_draw_ = 0;
  // Why draws were dropped in IssueDraw, cumulative. On the object rather than
  // in a function-local static so IssueSwap can dump the whole histogram
  // periodically - the question "were these draws skipped, or did they render
  // nothing?" is the first one to ask about a black world, and a flood-gated
  // per-skip log cannot answer it (it rotates out of the log file).
  std::unordered_map<std::string, uint64_t> skip_reason_counts_;
  // Texture bind outcomes, cumulative. A high null rate means geometry samples
  // opaque black - the other way (besides depth) that a 3D world goes black
  // while 2D UI in the same phase stays correct.
  uint64_t texture_bind_total_ = 0;
  uint64_t texture_miss_total_ = 0;
  uint64_t texture_null_total_ = 0;
  // The guest's own depth clear value (RB_DEPTH_CLEAR), captured from the
  // resolves that clear depth. The native backend has no EDRAM, so it clears a
  // real depth attachment per phase - and clearing it to a hardcoded 1.0 breaks
  // every REVERSE-Z title outright: Choplifter renders 690 of its ~856 world
  // draws with zfunc GREATER_EQUAL, so against a 1.0 clear nothing passes and
  // the whole 3D world vanishes while depth-disabled 2D UI still draws.
  float guest_depth_clear_ = 1.0f;
  // Histogram of RT0's guest colour format over draws (indexed by
  // xenos::ColorRenderTargetFormat). Native renders every target as 8-bit
  // UNORM, so an HDR guest format (3 = 2_10_10_10_FLOAT, 7 = 16_16_16_16_FLOAT)
  // has everything above 1.0 clamped - the difference between "blown out
  // because HDR is being clipped" and "blown out because gamma is missing"
  // (1 = 8_8_8_8_GAMMA, whose conversion flags native never sets).
  uint64_t rt_format_counts_[16] = {};
  // Colour write mask per issued draw. A draw with mask 0 is issued, binds
  // its textures and passes every skip check - and writes no pixels at all.
  uint64_t write_mask_counts_[16] = {};
  uint64_t blend_enable_draws_ = 0;
  uint64_t resolve_count_ = 0;
  uint32_t resolves_this_frame_ = 0;
  // Bounds resolved-image creation per frame (each is a full VkImage); resolves
  // past this are still consumed as phase markers.
  static constexpr uint32_t kMaxResolveCapturesPerFrame = 48;

  HostRingBuffer uniform_ring_;
  HostRingBuffer index_ring_;

  std::unordered_map<const Shader::Translation*, VkShaderModule> shader_modules_;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  // Geometry shaders for Xenos primitive types with no direct Vulkan
  // topology (currently just kRectangleList - see GetGeometryShader).
  // Stores VK_NULL_HANDLE if a build was attempted and failed, matching the
  // oracle's geometry_shaders_ cache.
  std::unordered_map<vulkan::VulkanPipelineCache::GeometryShaderKey, VkShaderModule,
                     vulkan::VulkanPipelineCache::GeometryShaderKey::Hasher>
      geometry_shaders_;

  std::vector<DeferredDraw> deferred_draws_;
  bool frame_open_ = false;
  bool draw_resources_ok_ = false;
#endif  // REX_HAS_VULKAN
};

}  // namespace rex::graphics::native
