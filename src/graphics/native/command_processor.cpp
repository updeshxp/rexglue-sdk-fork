/**
 * @file        graphics/native/command_processor.cpp
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "native/command_processor.h"

#include "native/index_expand.h"
#include "native/draw_classify.h"
#include "native/phase_model.h"
#include "native/shader_constants.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>

#include "native/graphics_system.h"
#include "native/native_shared_memory.h"

#if REX_HAS_VULKAN
#include <rex/ui/renderdoc_api.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>
#endif

REXCVAR_DEFINE_BOOL(native_marker, true, "GPU/Native",
                    "Draw a green tab in the top-left corner of every frame produced by the native "
                    "GPU backend, so it is obvious at a glance which renderer is running.");
REXCVAR_DEFINE_BOOL(native_log_draws, false, "GPU/Native",
                    "Log every draw/copy the native renderer receives from the PM4 stream");
REXCVAR_DEFINE_BOOL(native_expand_rects, false, "GPU",
                    "Expand guest rectangle lists into two triangles. Off by default: the\n                    implied fourth corner is reconstructed in the vertex shader and has\n                    not been proven correct yet - see MapPrimitiveTopology.");
REXCVAR_DEFINE_BOOL(native_rect_gs, true, "GPU/Native",
                    "Expand guest rectangle lists with the oracle's geometry shader (the\n                    validated path). Off = draw each rect as a single bare triangle\n                    (loses the half past the diagonal) - kept as an A/B switch for\n                    isolating regressions to the GS path.");
REXCVAR_DEFINE_BOOL(native_marker_empty_frames, false, "GPU/Native",
                    "Clear draw-less frames to a recognisable mid-blue instead of black.\n                    Bring-up aid only: level loads produce long runs of draw-less\n                    frames, so with this on they flash violently blue.");
REXCVAR_DEFINE_BOOL(native_present_frontbuffer, false, "GPU/Native",
                    "Present the resolved frontbuffer image instead of replaying its\n                    draws. The SELECTION is correct and measured - it is what makes\n                    SoulCalibur II report present=true - but the blit that consumes\n                    it still makes vkQueueSubmit fail, so it is off until that is\n                    found with validation layers enabled.");
REXCVAR_DEFINE_BOOL(native_log_phases, false, "GPU/Native",
                    "Log render-target phases, their draw ownership and their resolves.\n                    Cheap (a few lines per frame) and periodic, unlike\n                    native_log_draws, whose per-draw flood rotates these very lines\n                    out of the log file before they can be read.");
REXCVAR_DEFINE_BOOL(native_phase_base_filter, true, "GPU/Native",
                    "Restrict a phase's replay to the draws whose colour render target\n                    matches the resolved EDRAM base. Off = replay every draw in the\n                    phase's range - an A/B switch for titles whose world renders\n                    black, to separate 'draws were filtered out' from 'draws rendered\n                    nothing'.");

// Bump this whenever the clip-disable viewport investigation changes so a fresh
// game log can prove that the staged native plugin is the expected build.
constexpr char kNativeViewportDiagnosticsBuild[] = "clip-vp-diag-2026-09-24.1";

namespace rex::graphics::native {

namespace {

// IEEE half -> float, for reading back kSceneColorFormat images on the host.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  if (exponent == 0) {
    if (mantissa == 0) {
      const uint32_t zero = sign;
      float out;
      std::memcpy(&out, &zero, sizeof(out));
      return out;
    }
    // Subnormal: normalize it.
    exponent = 1;
    while (!(mantissa & 0x400)) {
      mantissa <<= 1;
      --exponent;
    }
    mantissa &= 0x3FF;
  } else if (exponent == 0x1F) {
    exponent = 0xFF;  // Inf / NaN
    const uint32_t bits = sign | (exponent << 23) | (mantissa << 13);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }
  const uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

uint64_t HashUcode(const uint32_t* dwords, uint32_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t i = 0; i < count; ++i) {
    hash ^= dwords[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

#if REX_HAS_VULKAN
// Xenos primitive type -> Vulkan topology. Returns false for types not yet
// supported by the native backend (rect/quad list, line loop, polygon - these
// need geometry-shader-style expansion, a Phase 3 concern).
bool MapPrimitiveTopology(xenos::PrimitiveType prim_type, VkPrimitiveTopology& out) {
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList:
      out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      return true;
    case xenos::PrimitiveType::kLineList:
      out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      return true;
    case xenos::PrimitiveType::kLineStrip:
      out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      return true;
    case xenos::PrimitiveType::kTriangleList:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    case xenos::PrimitiveType::kTriangleStrip:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      return true;
    case xenos::PrimitiveType::kTriangleFan:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      return true;
    case xenos::PrimitiveType::kRectangleList:
      // Approximation: render each 3-vertex rectangle as a single triangle
      // (half the quad). Exact for the oversized-triangle fullscreen pass
      // (Geometry Wars), but a genuine rectangle loses the half beyond the
      // diagonal - visible in Hydro Thunder, whose composite blits are real
      // rectangles. Proper fix: kRectangleListAsTriangleStrip with a
      // triangle-strip topology and primitive restart.
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    case xenos::PrimitiveType::kQuadList:
      // 4 vertices per quad, expanded into two triangles per quad via a
      // generated index buffer in IssueDraw.
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    default:
      return false;
  }
}

// IndexEndianSwaps / ConvertIndex16 / ConvertIndex32 and the quad / rectangle
// expansion live in index_expand.h so they can be unit tested on the host
// without a GPU - see tests/unit/graphics/index_expand_test.cpp.

// Xenos BlendFactor (raw 5-bit value) -> VkBlendFactor. Matches the emulation
// backend's kBlendFactorMap (undefined values 2/3 -> ZERO).
VkBlendFactor MapBlendFactor(xenos::BlendFactor factor) {
  switch (uint32_t(factor)) {
    case 0:  return VK_BLEND_FACTOR_ZERO;
    case 1:  return VK_BLEND_FACTOR_ONE;
    case 4:  return VK_BLEND_FACTOR_SRC_COLOR;
    case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8:  return VK_BLEND_FACTOR_DST_COLOR;
    case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
  }
}

// Xenos BlendOp (comb_fcn) -> VkBlendOp.
VkBlendOp MapBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd:         return VK_BLEND_OP_ADD;
    case xenos::BlendOp::kSubtract:    return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin:         return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax:         return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:                           return VK_BLEND_OP_ADD;
  }
}
#endif  // REX_HAS_VULKAN

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
#if REX_HAS_VULKAN
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(graphics_system_->provider());
  if (!provider) {
    REXLOG_ERROR("rexgpu-native: SetupContext - no Vulkan provider");
    return false;
  }
  vulkan_device_ = provider->vulkan_device();
  if (!vulkan_device_) {
    REXLOG_ERROR("rexgpu-native: SetupContext - provider has no Vulkan device");
    return false;
  }
  if (!CreateClearResources()) {
    REXLOG_ERROR("rexgpu-native: SetupContext - failed to create clear-present resources");
    DestroyClearResources();
    vulkan_device_ = nullptr;
    return false;
  }
  // Phase 2: shared memory, shader translator, descriptor layouts, pools. If any
  // of this fails, keep the Phase 1 clear-present path working (draw_resources_ok_
  // stays false and every draw is skipped, so the frame is at least a clean clear).
  draw_resources_ok_ = CreateDrawResources();
  if (!draw_resources_ok_) {
    REXLOG_WARN("rexgpu-native: SetupContext - draw resources unavailable, clear-only fallback");
    DestroyDrawResources();
  }
  REXLOG_INFO("rexgpu-native: SetupContext ready (build={}, device={}, draw_path={})",
              kNativeViewportDiagnosticsBuild, vulkan_device_->properties().deviceName,
              draw_resources_ok_ ? "geometry" : "clear-only");
  return true;
#else
  REXLOG_ERROR("rexgpu-native: SetupContext - built without Vulkan support");
  return false;
#endif
}

void NativeCommandProcessor::ShutdownContext() {
  REXLOG_INFO("rexgpu-native: ShutdownContext (draws={} deferred={} skipped={} copies={} swaps={})",
              draw_count_, deferred_draw_total_, skipped_draw_total_, copy_count_, swap_count_);
#if REX_HAS_VULKAN
  DestroyDrawResources();
  DestroyClearResources();
  vulkan_device_ = nullptr;
#endif
  shader_map_.clear();
  shader_storage_.clear();
}

#if REX_HAS_VULKAN

bool NativeCommandProcessor::CreateClearResources() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags =
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create command pool");
    return false;
  }

  VkCommandBufferAllocateInfo cb_info = {};
  cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_info.commandPool = command_pool_;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &cb_info, &command_buffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to allocate command buffer");
    return false;
  }

  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (dfn.vkCreateFence(device, &fence_info, nullptr, &clear_fence_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create fence");
    return false;
  }

  // Render pass with a single guest-output-format color attachment. Used both to
  // clear the guest output and (Phase 2) as the render pass for guest draw
  // pipelines and the swap-time replay.
  VkAttachmentDescription attachment = {};
  // Float, not the presenter's format: guest draws render into the HDR scene
  // image and the finished frame is blitted down at present time (see
  // kSceneColorFormat). Every guest pipeline is created against this render
  // pass, so its colour format has to be the one all guest draws target.
  attachment.format = kSceneColorFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_ref = {};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  // Phase 3: transient depth attachment (cleared each frame to far). Gives the
  // 3D scene correct occlusion - the native backend has no EDRAM.
  VkAttachmentDescription depth_attachment = {};
  depth_attachment.format = kDepthFormat;
  depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_ref = {};
  depth_ref.attachment = 1;
  depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  subpass.pDepthStencilAttachment = &depth_ref;

  const VkAttachmentDescription attachments[2] = {attachment, depth_attachment};
  VkRenderPassCreateInfo rp_info = {};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = 2;
  rp_info.pAttachments = attachments;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  if (dfn.vkCreateRenderPass(device, &rp_info, nullptr, &clear_render_pass_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create clear render pass");
    return false;
  }

  // Same attachments, but the colour target is LOADED rather than cleared, for
  // the case where the frame is a presented resolved image that trailing draws
  // composite on top of. Render-pass compatibility depends on formats and
  // sample counts, not load/store ops, so guest pipelines built against
  // clear_render_pass_ are valid here too.
  VkAttachmentDescription load_attachments[2] = {attachment, depth_attachment};
  load_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  rp_info.pAttachments = load_attachments;
  if (dfn.vkCreateRenderPass(device, &rp_info, nullptr, &load_render_pass_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create load render pass");
    return false;
  }

  return true;
}

void NativeCommandProcessor::DestroyClearResources() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (clear_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, clear_framebuffer_, nullptr);
    clear_framebuffer_ = VK_NULL_HANDLE;
  }
  // Depth image/view is referenced by the framebuffer above - destroy after it.
  DestroyDepthResources();
  if (load_render_pass_ != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, load_render_pass_, nullptr);
    load_render_pass_ = VK_NULL_HANDLE;
  }
  if (clear_render_pass_ != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, clear_render_pass_, nullptr);
    clear_render_pass_ = VK_NULL_HANDLE;
  }
  if (clear_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, clear_fence_, nullptr);
    clear_fence_ = VK_NULL_HANDLE;
  }
  if (command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
  }
  clear_framebuffer_view_ = VK_NULL_HANDLE;
  clear_framebuffer_version_ = UINT64_MAX;
  clear_framebuffer_width_ = 0;
  clear_framebuffer_height_ = 0;
}

bool NativeCommandProcessor::EnsureSceneFramebuffer(uint32_t width, uint32_t height) {
  if (scene_framebuffer_ != VK_NULL_HANDLE && scene_width_ == width && scene_height_ == height) {
    return true;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  DestroySceneFramebuffer();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kSceneColorFormat;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_DST as well: the scene image is blitted INTO when a resolved
  // frontbuffer is presented, not only rendered into and blitted out of.
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device, &image_info, nullptr, &scene_color_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: scene image create failed {}x{}", width, height);
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(device, scene_color_, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    DestroySceneFramebuffer();
    return false;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &scene_color_memory_) != VK_SUCCESS ||
      dfn.vkBindImageMemory(device, scene_color_, scene_color_memory_, 0) != VK_SUCCESS) {
    DestroySceneFramebuffer();
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = scene_color_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kSceneColorFormat;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &scene_color_view_) != VK_SUCCESS) {
    DestroySceneFramebuffer();
    return false;
  }
  if (!EnsureDepthResources(width, height)) {
    DestroySceneFramebuffer();
    return false;
  }
  const VkImageView fb_attachments[2] = {scene_color_view_, depth_view_};
  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.renderPass = clear_render_pass_;
  fb_info.attachmentCount = 2;
  fb_info.pAttachments = fb_attachments;
  fb_info.width = width;
  fb_info.height = height;
  fb_info.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_info, nullptr, &scene_framebuffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: scene framebuffer failed {}x{}", width, height);
    DestroySceneFramebuffer();
    return false;
  }
  scene_width_ = width;
  scene_height_ = height;
  return true;
}

void NativeCommandProcessor::DestroySceneFramebuffer() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (scene_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, scene_framebuffer_, nullptr);
    scene_framebuffer_ = VK_NULL_HANDLE;
  }
  if (scene_color_view_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, scene_color_view_, nullptr);
    scene_color_view_ = VK_NULL_HANDLE;
  }
  if (scene_color_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, scene_color_, nullptr);
    scene_color_ = VK_NULL_HANDLE;
  }
  if (scene_color_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, scene_color_memory_, nullptr);
    scene_color_memory_ = VK_NULL_HANDLE;
  }
  scene_width_ = 0;
  scene_height_ = 0;
}

bool NativeCommandProcessor::EnsureClearFramebuffer(VkImageView image_view, uint64_t image_version,
                                                    uint32_t width, uint32_t height) {
  if (clear_framebuffer_ != VK_NULL_HANDLE && clear_framebuffer_view_ == image_view &&
      clear_framebuffer_version_ == image_version && clear_framebuffer_width_ == width &&
      clear_framebuffer_height_ == height) {
    return true;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (clear_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, clear_framebuffer_, nullptr);
    clear_framebuffer_ = VK_NULL_HANDLE;
  }

  if (!EnsureDepthResources(width, height)) {
    return false;
  }

  const VkImageView fb_attachments[2] = {image_view, depth_view_};
  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.renderPass = clear_render_pass_;
  fb_info.attachmentCount = 2;
  fb_info.pAttachments = fb_attachments;
  fb_info.width = width;
  fb_info.height = height;
  fb_info.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_info, nullptr, &clear_framebuffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create clear framebuffer {}x{}", width, height);
    return false;
  }
  clear_framebuffer_view_ = image_view;
  clear_framebuffer_version_ = image_version;
  clear_framebuffer_width_ = width;
  clear_framebuffer_height_ = height;
  return true;
}

bool NativeCommandProcessor::EnsureDepthResources(uint32_t width, uint32_t height) {
  if (depth_image_ != VK_NULL_HANDLE && depth_width_ == width && depth_height_ == height) {
    return true;
  }
  DestroyDepthResources();

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kDepthFormat;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device, &image_info, nullptr, &depth_image_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create depth image {}x{}", width, height);
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(device, depth_image_, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &depth_memory_) != VK_SUCCESS) {
    return false;
  }
  if (dfn.vkBindImageMemory(device, depth_image_, depth_memory_, 0) != VK_SUCCESS) {
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = depth_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kDepthFormat;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &depth_view_) != VK_SUCCESS) {
    return false;
  }
  depth_width_ = width;
  depth_height_ = height;
  return true;
}

void NativeCommandProcessor::DestroyDepthResources() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (depth_view_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, depth_view_, nullptr);
    depth_view_ = VK_NULL_HANDLE;
  }
  if (depth_image_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, depth_image_, nullptr);
    depth_image_ = VK_NULL_HANDLE;
  }
  if (depth_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, depth_memory_, nullptr);
    depth_memory_ = VK_NULL_HANDLE;
  }
  depth_width_ = 0;
  depth_height_ = 0;
}

// ---------------------------------------------------------------------------
// Phase 2 draw resources
// ---------------------------------------------------------------------------

bool NativeCommandProcessor::CreateHostRingBuffer(VkBufferUsageFlags usage, VkDeviceSize size,
                                                  HostRingBuffer& out) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &out.buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetBufferMemoryRequirements(device, out.buffer, &req);
  uint32_t type_index;
  uint32_t candidates =
      req.memoryTypeBits & vulkan_device_->memory_types().host_visible &
      vulkan_device_->memory_types().host_coherent;
  if (!rex::bit_scan_forward(candidates, &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) {
    return false;
  }
  if (dfn.vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return false;
  }
  out.mapping = static_cast<uint8_t*>(mapping);
  out.size = size;
  out.cursor = 0;
  return true;
}

void NativeCommandProcessor::DestroyHostRingBuffer(HostRingBuffer& ring) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (ring.buffer != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, ring.buffer, nullptr);
    ring.buffer = VK_NULL_HANDLE;
  }
  if (ring.memory != VK_NULL_HANDLE) {
    if (ring.mapping) {
      dfn.vkUnmapMemory(device, ring.memory);
      ring.mapping = nullptr;
    }
    dfn.vkFreeMemory(device, ring.memory, nullptr);
    ring.memory = VK_NULL_HANDLE;
  }
  ring.size = 0;
  ring.cursor = 0;
}

uint8_t* NativeCommandProcessor::RingAllocate(HostRingBuffer& ring, VkDeviceSize bytes,
                                              VkDeviceSize alignment, VkBuffer& buffer_out,
                                              VkDeviceSize& offset_out) {
  VkDeviceSize aligned = (ring.cursor + (alignment - 1)) & ~(alignment - 1);
  if (aligned + bytes > ring.size) {
    return nullptr;
  }
  buffer_out = ring.buffer;
  offset_out = aligned;
  ring.cursor = aligned + bytes;
  return ring.mapping + aligned;
}

bool NativeCommandProcessor::CreateDrawResources() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (!memory_) {
    REXLOG_ERROR("rexgpu-native: draw resources - no guest memory");
    return false;
  }

  shared_memory_ = std::make_unique<NativeSharedMemory>(vulkan_device_, *memory_);
  if (!shared_memory_->Initialize()) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory init failed");
    return false;
  }

  // Phase 3: real guest texture cache (untiling + format decode via the shared
  // texture_load_*_cs compute shaders). Non-fatal on failure - the draw path
  // falls back to the dummy white texture so geometry still renders.
  texture_cache_ = NativeTextureCache::Create(vulkan_device_, *register_file_, *shared_memory_);
  if (!texture_cache_) {
    REXLOG_WARN(
        "rexgpu-native: draw resources - texture cache init failed; textures will be white");
  } else {
    REXLOG_INFO("rexgpu-native: texture cache ready (real guest textures)");
  }

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device_),
      /*native_2x_msaa_with_attachments=*/true, /*native_2x_msaa_no_attachments=*/false,
      /*edram_fragment_shader_interlock=*/false);

  shared_memory_binding_count_ =
      1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
               vulkan_device_->properties().maxStorageBufferRange);

  const VkShaderStageFlags guest_stages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  // Set 0: shared memory storage buffer (array of shared_memory_binding_count_).
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_memory_binding_count_;
  shared_binding.stageFlags = guest_stages;
  VkDescriptorSetLayoutCreateInfo shared_layout_info = {};
  shared_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_layout_info.bindingCount = 1;
  shared_layout_info.pBindings = &shared_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &shared_layout_info, nullptr,
                                      &descriptor_set_layout_shared_memory_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory set layout failed");
    return false;
  }

  // Set 1: 5 constant UBOs (system, float vertex, float pixel, bool/loop, fetch).
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  VkDescriptorSetLayoutCreateInfo constants_layout_info = {};
  constants_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  constants_layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  constants_layout_info.pBindings = constant_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &constants_layout_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - constants set layout failed");
    return false;
  }

  // Empty texture set layout (for stages that use no textures - sets 2/3 still
  // exist in the pipeline layout for set-compatibility).
  VkDescriptorSetLayoutCreateInfo empty_layout_info = {};
  empty_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (dfn.vkCreateDescriptorSetLayout(device, &empty_layout_info, nullptr,
                                      &texture_set_layout_empty_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - empty texture set layout failed");
    return false;
  }
  texture_set_layouts_.emplace(0u, texture_set_layout_empty_);

  // Static shared-memory descriptor set (never changes; points at the buffer).
  VkDescriptorPoolSize shared_pool_size = {};
  shared_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_pool_size.descriptorCount = shared_memory_binding_count_;
  VkDescriptorPoolCreateInfo shared_pool_info = {};
  shared_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  shared_pool_info.maxSets = 2;  // shared-memory set + persistent empty texture set
  shared_pool_info.poolSizeCount = 1;
  shared_pool_info.pPoolSizes = &shared_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &shared_pool_info, nullptr,
                                 &shared_memory_descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory pool failed");
    return false;
  }
  VkDescriptorSetAllocateInfo shared_alloc = {};
  shared_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  shared_alloc.descriptorPool = shared_memory_descriptor_pool_;
  shared_alloc.descriptorSetCount = 1;
  shared_alloc.pSetLayouts = &descriptor_set_layout_shared_memory_;
  if (dfn.vkAllocateDescriptorSets(device, &shared_alloc, &shared_memory_descriptor_set_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory set alloc failed");
    return false;
  }
  std::array<VkDescriptorBufferInfo, 4> shared_buffer_infos = {};
  const VkDeviceSize shared_range = SharedMemory::kBufferSize / shared_memory_binding_count_;
  for (uint32_t i = 0; i < shared_memory_binding_count_; ++i) {
    shared_buffer_infos[i].buffer = shared_memory_->buffer();
    shared_buffer_infos[i].offset = shared_range * i;
    shared_buffer_infos[i].range = shared_range;
  }
  VkWriteDescriptorSet shared_write = {};
  shared_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  shared_write.dstSet = shared_memory_descriptor_set_;
  shared_write.dstBinding = 0;
  shared_write.dstArrayElement = 0;
  shared_write.descriptorCount = shared_memory_binding_count_;
  shared_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_write.pBufferInfo = shared_buffer_infos.data();
  dfn.vkUpdateDescriptorSets(device, 1, &shared_write, 0, nullptr);

  // Persistent empty texture descriptor set (bound for stages with no textures).
  VkDescriptorSetAllocateInfo empty_alloc = {};
  empty_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  empty_alloc.descriptorPool = shared_memory_descriptor_pool_;
  empty_alloc.descriptorSetCount = 1;
  empty_alloc.pSetLayouts = &texture_set_layout_empty_;
  if (dfn.vkAllocateDescriptorSets(device, &empty_alloc, &empty_texture_set_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - empty texture set alloc failed");
    return false;
  }

  // Dummy white textures + default sampler for the texture path.
  if (!CreateDummyTextures()) {
    REXLOG_ERROR("rexgpu-native: draw resources - dummy textures failed");
    return false;
  }

  // Per-frame constant descriptor set pool.
  VkDescriptorPoolSize constant_pool_size = {};
  constant_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  constant_pool_size.descriptorCount =
      kMaxDrawsPerFrame * SpirvShaderTranslator::kConstantBufferCount;
  VkDescriptorPoolCreateInfo constant_pool_info = {};
  constant_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  constant_pool_info.maxSets = kMaxDrawsPerFrame;
  constant_pool_info.poolSizeCount = 1;
  constant_pool_info.pPoolSizes = &constant_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &constant_pool_info, nullptr,
                                 &constants_descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - constants pool failed");
    return false;
  }

  // Per-frame texture descriptor set pool (dummy image + sampler bindings).
  // Two sets per draw (vertex + pixel), generous per-set binding budget.
  VkDescriptorPoolSize texture_pool_sizes[2] = {};
  texture_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  texture_pool_sizes[0].descriptorCount = kMaxDrawsPerFrame * 8;
  texture_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  texture_pool_sizes[1].descriptorCount = kMaxDrawsPerFrame * 8;
  VkDescriptorPoolCreateInfo texture_pool_info = {};
  texture_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  texture_pool_info.maxSets = kMaxDrawsPerFrame * 2;
  texture_pool_info.poolSizeCount = 2;
  texture_pool_info.pPoolSizes = texture_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &texture_pool_info, nullptr, &texture_descriptor_pool_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - texture pool failed");
    return false;
  }

  // Per-frame host-visible upload rings (constants + converted indices).
  if (!CreateHostRingBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 32u << 20, uniform_ring_)) {
    REXLOG_ERROR("rexgpu-native: draw resources - uniform ring failed");
    return false;
  }
  if (!CreateHostRingBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 32u << 20, index_ring_)) {
    REXLOG_ERROR("rexgpu-native: draw resources - index ring failed");
    return false;
  }

  return true;
}

void NativeCommandProcessor::DestroyDrawResources() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Resolve resources (framebuffer references clear_render_pass_, still valid
  // here since DestroyClearResources runs after DestroyDrawResources).
  ResetResolvedTargets();
  DestroyResolveRenderTarget();
  if (resolve_staging_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, resolve_staging_buffer_, nullptr);
    resolve_staging_buffer_ = VK_NULL_HANDLE;
  }
  if (resolve_staging_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, resolve_staging_memory_, nullptr);
    resolve_staging_memory_ = VK_NULL_HANDLE;
  }
  resolve_staging_size_ = 0;

  for (auto& kv : pipelines_) {
    if (kv.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, kv.second, nullptr);
    }
  }
  pipelines_.clear();
  for (auto& kv : shader_modules_) {
    if (kv.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, kv.second, nullptr);
    }
  }
  shader_modules_.clear();

  DestroyHostRingBuffer(uniform_ring_);
  DestroyHostRingBuffer(index_ring_);

  for (auto& kv : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(device, kv.second, nullptr);
  }
  pipeline_layouts_.clear();
  for (auto& kv : texture_set_layouts_) {
    dfn.vkDestroyDescriptorSetLayout(device, kv.second, nullptr);
  }
  texture_set_layouts_.clear();
  texture_set_layout_empty_ = VK_NULL_HANDLE;  // owned by the map above

  // Dummy textures.
  VkImage dummy_images[3] = {dummy_image_2d_array_, dummy_image_3d_, dummy_image_cube_};
  VkImageView dummy_views[3] = {dummy_view_2d_array_, dummy_view_3d_, dummy_view_cube_};
  for (int i = 0; i < 3; ++i) {
    if (dummy_views[i] != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, dummy_views[i], nullptr);
    }
    if (dummy_images[i] != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, dummy_images[i], nullptr);
    }
    if (dummy_memory_[i] != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, dummy_memory_[i], nullptr);
      dummy_memory_[i] = VK_NULL_HANDLE;
    }
  }
  dummy_image_2d_array_ = dummy_image_3d_ = dummy_image_cube_ = VK_NULL_HANDLE;
  dummy_view_2d_array_ = dummy_view_3d_ = dummy_view_cube_ = VK_NULL_HANDLE;
  if (dummy_sampler_ != VK_NULL_HANDLE) {
    dfn.vkDestroySampler(device, dummy_sampler_, nullptr);
    dummy_sampler_ = VK_NULL_HANDLE;
  }

  if (texture_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, texture_descriptor_pool_, nullptr);
    texture_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (constants_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, constants_descriptor_pool_, nullptr);
    constants_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (shared_memory_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, shared_memory_descriptor_pool_, nullptr);
    shared_memory_descriptor_pool_ = VK_NULL_HANDLE;
    shared_memory_descriptor_set_ = VK_NULL_HANDLE;
    empty_texture_set_ = VK_NULL_HANDLE;
  }
  if (descriptor_set_layout_constants_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_constants_, nullptr);
    descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  }
  if (descriptor_set_layout_shared_memory_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_shared_memory_, nullptr);
    descriptor_set_layout_shared_memory_ = VK_NULL_HANDLE;
  }

  shader_translator_.reset();
  // Texture cache references the shared memory, so destroy it first.
  texture_cache_.reset();
  if (shared_memory_) {
    shared_memory_->Shutdown();
    shared_memory_.reset();
  }
  deferred_draws_.clear();
  frame_open_ = false;
  draw_resources_ok_ = false;
}

void NativeCommandProcessor::BeginFrameIfNeeded() {
  if (frame_open_) {
    return;
  }
  // Safe to recycle: the previous frame's replay submission was fenced-and-waited
  // in IssueSwap, so nothing references last frame's sets / ring data.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  dfn.vkResetDescriptorPool(device, constants_descriptor_pool_, 0);
  dfn.vkResetDescriptorPool(device, texture_descriptor_pool_, 0);
  uniform_ring_.cursor = 0;
  index_ring_.cursor = 0;
  deferred_draws_.clear();
  // Resolved render-target images and their address aliases are NOT cleared
  // here - both persist across frames, because render-to-texture is often
  // cross-frame (resolved in one frame, sampled in a later one; OutRun's menu
  // does this). Clearing the aliases per frame left such a sample with no view,
  // falling back to guest memory the native backend never wrote - i.e. black.
  // Each resolve overwrites its own entry, so content stays current.
  phases_.clear();
  base_clear_point_.clear();
  base_last_resolve_.clear();
  acquired_dest_keys_this_frame_.clear();
  // Reclaim slots displaced by a same-frame duplicate resolve. The submit that
  // used them completed before this point, so they are safe to destroy now.
  if (!retired_resolved_slots_.empty()) {
    const ui::vulkan::VulkanDevice::Functions& rdfn = vulkan_device_->functions();
    const VkDevice rdevice = vulkan_device_->device();
    for (size_t slot : retired_resolved_slots_) {
      if (slot >= resolved_target_storage_.size()) {
        continue;
      }
      ResolvedTarget& rt = resolved_target_storage_[slot];
      if (rt.view != VK_NULL_HANDLE) rdfn.vkDestroyImageView(rdevice, rt.view, nullptr);
      if (rt.image != VK_NULL_HANDLE) rdfn.vkDestroyImage(rdevice, rt.image, nullptr);
      if (rt.memory != VK_NULL_HANDLE) rdfn.vkFreeMemory(rdevice, rt.memory, nullptr);
      rt = ResolvedTarget{};
    }
    retired_resolved_slots_.clear();
  }
  phase_first_draw_ = 0;
  resolves_this_frame_ = 0;
  // Advance the texture cache to a fresh frame (resets bindings so they are
  // re-resolved from the current fetch constants this frame).
  if (texture_cache_) {
    texture_cache_->BeginNativeFrame();
  }
  frame_open_ = true;
}

VkShaderModule NativeCommandProcessor::GetShaderModule(const Shader::Translation* translation) {
  auto it = shader_modules_.find(translation);
  if (it != shader_modules_.end()) {
    return it->second;
  }
  const std::vector<uint8_t>& spirv = translation->translated_binary();
  VkShaderModule module = VK_NULL_HANDLE;
  if (!spirv.empty() && (spirv.size() % sizeof(uint32_t)) == 0) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size();
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    if (dfn.vkCreateShaderModule(vulkan_device_->device(), &info, nullptr, &module) != VK_SUCCESS) {
      module = VK_NULL_HANDLE;
    }
  }
  shader_modules_.emplace(translation, module);
  return module;
}

VkShaderModule NativeCommandProcessor::GetGeometryShader(
    vulkan::VulkanPipelineCache::GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  VkShaderModule module =
      vulkan::VulkanPipelineCache::BuildGeometryShaderModule(*vulkan_device_, key);
  geometry_shaders_.emplace(key, module);
  return module;
}

VkPipeline NativeCommandProcessor::GetPipeline(VkShaderModule vertex_module,
                                               VkShaderModule pixel_module,
                                               VkPipelineLayout layout,
                                               const GuestPipelineState& state,
                                               VkShaderModule geometry_module) {
  uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(vertex_module));
  key = key * 1099511628211ull ^ uint64_t(reinterpret_cast<uintptr_t>(pixel_module));
  key = key * 1099511628211ull ^ uint64_t(reinterpret_cast<uintptr_t>(geometry_module));
  key = key * 1099511628211ull ^ uint64_t(reinterpret_cast<uintptr_t>(layout));
  key = key * 1099511628211ull ^ state.Hash();
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // A depth-only draw (depth pre-pass, shadow map, or a color pass the guest
  // masked out entirely) is rasterized with NO fragment stage - it contributes
  // depth and nothing else. Same shape the oracle builds by decrementing its
  // stage count when the fragment module is null (vulkan/pipeline_cache.cpp:3154).
  const bool has_fragment = pixel_module != VK_NULL_HANDLE;
  const bool has_geometry = geometry_module != VK_NULL_HANDLE;
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = vertex_module;
  stages[stage_count].pName = "main";
  ++stage_count;
  if (has_geometry) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    stages[stage_count].module = geometry_module;
    stages[stage_count].pName = "main";
    ++stage_count;
  }
  if (has_fragment) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = pixel_module;
    stages[stage_count].pName = "main";
    ++stage_count;
  }

  // Empty vertex input - the translated shaders fetch vertices from shared
  // memory in-shader (vfetch), so there are no classic vertex bindings.
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = state.topology;
  input_assembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  // [DIAG] REX_NATIVE_WIREFRAME=1 renders geometry as a wireframe so the mesh
  // structure is visible even with placeholder (untextured/white) shading -
  // useful for verifying that real guest geometry is being drawn.
  static const bool wireframe =
      getenv("REX_NATIVE_WIREFRAME") && atoi(getenv("REX_NATIVE_WIREFRAME")) != 0 &&
      vulkan_device_->properties().fillModeNonSolid;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  // Wireframe diagnostic must see all triangles, so it disables culling.
  raster.cullMode = wireframe ? VK_CULL_MODE_NONE : state.cull_mode;
  raster.frontFace = state.front_face;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = state.depth_test_enable ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = state.depth_write_enable ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = state.depth_compare_op;
  depth_stencil.depthBoundsTestEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;
  depth_stencil.minDepthBounds = 0.0f;
  depth_stencil.maxDepthBounds = 1.0f;

  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.blendEnable = state.blend_enable ? VK_TRUE : VK_FALSE;
  blend_attachment.srcColorBlendFactor = state.src_color_factor;
  blend_attachment.dstColorBlendFactor = state.dst_color_factor;
  blend_attachment.colorBlendOp = state.color_op;
  blend_attachment.srcAlphaBlendFactor = state.src_alpha_factor;
  blend_attachment.dstAlphaBlendFactor = state.dst_alpha_factor;
  blend_attachment.alphaBlendOp = state.alpha_op;
  blend_attachment.colorWriteMask = state.color_write_mask;
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  // Stays 1 even with no fragment stage: unlike the oracle, whose render-pass
  // key can omit color attachments for a depth-only pass, clear_render_pass_
  // ALWAYS has one - so the attachment must be described, just with a zeroed
  // colorWriteMask (which BuildPipelineState already produced).
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &blend_attachment;

  // Blend constants are dynamic, as in the oracle. Left unset, Vulkan defaults
  // them to zero, which silently turns every kOneMinusConstant* factor into a
  // full-strength 1 - i.e. draws that should modulate instead accumulate.
  VkDynamicState dynamic_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 3;
  dynamic.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = stage_count;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = layout;
  pipeline_info.renderPass = clear_render_pass_;
  pipeline_info.subpass = 0;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

uint64_t NativeCommandProcessor::GuestPipelineState::Hash() const {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(uint64_t(topology));
  mix((uint64_t(blend_enable ? 1 : 0)) | (uint64_t(src_color_factor) << 1) |
      (uint64_t(dst_color_factor) << 9) | (uint64_t(color_op) << 17) |
      (uint64_t(src_alpha_factor) << 24) | (uint64_t(dst_alpha_factor) << 32) |
      (uint64_t(alpha_op) << 40) | (uint64_t(color_write_mask) << 48));
  mix((uint64_t(depth_test_enable ? 1 : 0)) | (uint64_t(depth_write_enable ? 1 : 0) << 1) |
      (uint64_t(depth_compare_op) << 2) | (uint64_t(cull_mode) << 8) |
      (uint64_t(front_face) << 16));
  return h;
}

NativeCommandProcessor::GuestPipelineState NativeCommandProcessor::BuildPipelineState(
    VkPrimitiveTopology topology, bool primitive_polygonal,
    const reg::RB_DEPTHCONTROL& depth_control, uint32_t pixel_writes_color_targets) const {
  const RegisterFile& regs = *register_file_;
  GuestPipelineState state;
  state.topology = topology;

  // --- Depth (from the already-normalized RB_DEPTHCONTROL). ---
  xenos::CompareFunction depth_compare;
  bool depth_write;
  if (depth_control.z_enable) {
    depth_compare = depth_control.zfunc;
    depth_write = depth_control.z_write_enable != 0;
  } else {
    depth_compare = xenos::CompareFunction::kAlways;
    depth_write = false;
  }
  state.depth_write_enable = depth_write;
  state.depth_compare_op = VkCompareOp(uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_compare));
  state.depth_test_enable = depth_write || depth_compare != xenos::CompareFunction::kAlways;

  // --- Cull / front face (only meaningful for polygonal primitives). ---
  if (primitive_polygonal) {
    const auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (mode_cntl.cull_front) {
      cull |= VK_CULL_MODE_FRONT_BIT;
    }
    if (mode_cntl.cull_back) {
      cull |= VK_CULL_MODE_BACK_BIT;
    }
    state.cull_mode = cull;
    state.front_face = mode_cntl.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  }

  // --- Blend + color write mask (render target 0). ---
  const uint32_t normalized_color_mask =
      draw_util::GetNormalizedColorMask(regs, pixel_writes_color_targets);
  const uint32_t rt0_write_mask = normalized_color_mask & 0b1111;
  state.color_write_mask = VkColorComponentFlags(rt0_write_mask);
  if (rt0_write_mask) {
    const auto blend_control =
        regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
    const VkBlendFactor src_c = MapBlendFactor(blend_control.color_srcblend);
    const VkBlendFactor dst_c = MapBlendFactor(blend_control.color_destblend);
    const VkBlendOp op_c = MapBlendOp(blend_control.color_comb_fcn);
    const VkBlendFactor src_a = MapBlendFactor(blend_control.alpha_srcblend);
    const VkBlendFactor dst_a = MapBlendFactor(blend_control.alpha_destblend);
    const VkBlendOp op_a = MapBlendOp(blend_control.alpha_comb_fcn);
    const bool identity = src_c == VK_BLEND_FACTOR_ONE && dst_c == VK_BLEND_FACTOR_ZERO &&
                          op_c == VK_BLEND_OP_ADD && src_a == VK_BLEND_FACTOR_ONE &&
                          dst_a == VK_BLEND_FACTOR_ZERO && op_a == VK_BLEND_OP_ADD;
    if (!identity) {
      state.blend_enable = true;
      state.src_color_factor = src_c;
      state.dst_color_factor = dst_c;
      state.color_op = op_c;
      state.src_alpha_factor = src_a;
      state.dst_alpha_factor = dst_a;
      state.alpha_op = op_a;
    }
  }
  return state;
}

bool NativeCommandProcessor::CreateDummyTextures() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  struct Def {
    VkImage* image;
    VkImageView* view;
    VkImageType type;
    VkImageViewType view_type;
    uint32_t layers;
    VkImageCreateFlags flags;
  };
  const Def defs[3] = {
      {&dummy_image_2d_array_, &dummy_view_2d_array_, VK_IMAGE_TYPE_2D,
       VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 0},
      {&dummy_image_3d_, &dummy_view_3d_, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
      {&dummy_image_cube_, &dummy_view_cube_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6,
       VkImageCreateFlags(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)},
  };

  for (int i = 0; i < 3; ++i) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = defs[i].flags;
    image_info.imageType = defs[i].type;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = defs[i].layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn.vkCreateImage(device, &image_info, nullptr, defs[i].image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req;
    dfn.vkGetImageMemoryRequirements(device, *defs[i].image, &req);
    uint32_t type_index;
    if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                               &type_index) &&
        !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
      return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    if (dfn.vkAllocateMemory(device, &alloc, nullptr, &dummy_memory_[i]) != VK_SUCCESS) {
      return false;
    }
    if (dfn.vkBindImageMemory(device, *defs[i].image, dummy_memory_[i], 0) != VK_SUCCESS) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = *defs[i].image;
    view_info.viewType = defs[i].view_type;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, defs[i].layers};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, defs[i].view) != VK_SUCCESS) {
      return false;
    }
  }

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &dummy_sampler_) != VK_SUCCESS) {
    return false;
  }

  // Clear the dummy images to white and move them to SHADER_READ_ONLY_OPTIMAL.
  dfn.vkResetCommandPool(device, command_pool_, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(command_buffer_, &begin_info) != VK_SUCCESS) {
    return false;
  }
  const VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
  for (int i = 0; i < 3; ++i) {
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, defs[i].layers};
    VkImageMemoryBarrier to_dst = {};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = *defs[i].image;
    to_dst.subresourceRange = range;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
    dfn.vkCmdClearColorImage(command_buffer_, *defs[i].image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
  }
  if (dfn.vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
    return false;
  }
  dfn.vkResetFences(device, 1, &clear_fence_);
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_buffer_;
  {
    const ui::vulkan::VulkanDevice::Queue::Acquisition acq =
        vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(acq.queue(), 1, &submit, clear_fence_) != VK_SUCCESS) {
      return false;
    }
  }
  dfn.vkWaitForFences(device, 1, &clear_fence_, VK_TRUE, UINT64_MAX);
  return true;
}

VkImageView NativeCommandProcessor::DummyViewForDimension(xenos::FetchOpDimension dimension) const {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return dummy_view_3d_;
    case xenos::FetchOpDimension::kCube:
      return dummy_view_cube_;
    default:
      return dummy_view_2d_array_;
  }
}

VkDescriptorSetLayout NativeCommandProcessor::GetTextureSetLayout(uint32_t texture_count,
                                                                 uint32_t sampler_count) {
  const uint32_t key = (texture_count << 16) | (sampler_count & 0xFFFF);
  auto it = texture_set_layouts_.find(key);
  if (it != texture_set_layouts_.end()) {
    return it->second;
  }
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  bindings.reserve(texture_count + sampler_count);
  for (uint32_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding b = {};
    b.binding = i;
    b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    bindings.push_back(b);
  }
  for (uint32_t j = 0; j < sampler_count; ++j) {
    VkDescriptorSetLayoutBinding b = {};
    b.binding = texture_count + j;
    b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    bindings.push_back(b);
  }
  VkDescriptorSetLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (vulkan_device_->functions().vkCreateDescriptorSetLayout(vulkan_device_->device(), &info,
                                                              nullptr, &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture_set_layouts_.emplace(key, layout);
  return layout;
}

VkPipelineLayout NativeCommandProcessor::GetGuestPipelineLayout(uint32_t vertex_texture_count,
                                                               uint32_t vertex_sampler_count,
                                                               uint32_t pixel_texture_count,
                                                               uint32_t pixel_sampler_count) {
  const uint64_t key = (uint64_t(vertex_texture_count) << 48) |
                       (uint64_t(vertex_sampler_count) << 32) |
                       (uint64_t(pixel_texture_count) << 16) | uint64_t(pixel_sampler_count);
  auto it = pipeline_layouts_.find(key);
  if (it != pipeline_layouts_.end()) {
    return it->second;
  }
  VkDescriptorSetLayout vertex_tex = GetTextureSetLayout(vertex_texture_count, vertex_sampler_count);
  VkDescriptorSetLayout pixel_tex = GetTextureSetLayout(pixel_texture_count, pixel_sampler_count);
  if (vertex_tex == VK_NULL_HANDLE || pixel_tex == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  VkDescriptorSetLayout sets[4] = {descriptor_set_layout_shared_memory_,
                                   descriptor_set_layout_constants_, vertex_tex, pixel_tex};
  VkPipelineLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = 4;
  info.pSetLayouts = sets;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (vulkan_device_->functions().vkCreatePipelineLayout(vulkan_device_->device(), &info, nullptr,
                                                         &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  pipeline_layouts_.emplace(key, layout);
  return layout;
}

VkDescriptorSet NativeCommandProcessor::AllocateDummyTextureSet(const SpirvShader* shader,
                                                               VkDescriptorSetLayout layout) {
  const std::vector<SpirvShader::TextureBinding>& textures =
      shader->GetTextureBindingsAfterTranslation();
  const std::vector<SpirvShader::SamplerBinding>& samplers =
      shader->GetSamplerBindingsAfterTranslation();
  const uint32_t texture_count = uint32_t(textures.size());
  const uint32_t sampler_count = uint32_t(samplers.size());

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  VkDescriptorSetAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = texture_descriptor_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(vulkan_device_->device(), &alloc, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  std::vector<VkDescriptorImageInfo> image_infos(texture_count);
  std::vector<VkDescriptorImageInfo> sampler_infos(sampler_count);
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(texture_count + sampler_count);
  for (uint32_t i = 0; i < texture_count; ++i) {
    image_infos[i].imageView = DummyViewForDimension(
        static_cast<xenos::FetchOpDimension>(textures[i].dimension));
    image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = i;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &image_infos[i];
    writes.push_back(w);
  }
  for (uint32_t j = 0; j < sampler_count; ++j) {
    sampler_infos[j].sampler = dummy_sampler_;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = texture_count + j;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &sampler_infos[j];
    writes.push_back(w);
  }
  if (!writes.empty()) {
    dfn.vkUpdateDescriptorSets(vulkan_device_->device(), uint32_t(writes.size()), writes.data(), 0,
                               nullptr);
  }
  return set;
}

VkDescriptorSet NativeCommandProcessor::AllocateTextureSet(SpirvShader* shader,
                                                           VkDescriptorSetLayout layout) {
  const std::vector<SpirvShader::TextureBinding>& textures =
      shader->GetTextureBindingsAfterTranslation();
  const std::vector<SpirvShader::SamplerBinding>& samplers =
      shader->GetSamplerBindingsAfterTranslation();
  const uint32_t texture_count = uint32_t(textures.size());
  const uint32_t sampler_count = uint32_t(samplers.size());

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  VkDescriptorSetAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = texture_descriptor_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(vulkan_device_->device(), &alloc, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  // image_infos / sampler_infos are sized up-front so the &element pointers
  // stored in the writes remain valid until vkUpdateDescriptorSets.
  std::vector<VkDescriptorImageInfo> image_infos(texture_count);
  std::vector<VkDescriptorImageInfo> sampler_infos(sampler_count);
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(texture_count + sampler_count);
  for (uint32_t i = 0; i < texture_count; ++i) {
    const SpirvShader::TextureBinding& tb = textures[i];
    const auto dimension = static_cast<xenos::FetchOpDimension>(tb.dimension);
    VkImageView view = VK_NULL_HANDLE;
    // Prefer a resolved render-target alias (render-to-texture: reflections,
    // HUD/mirror RTs, bloom). Only 2D bindings match the resolved image, which
    // is a plain 2D color image.
    if (!resolved_target_views_.empty() &&
        (dimension == xenos::FetchOpDimension::k1D || dimension == xenos::FetchOpDimension::k2D)) {
      const xenos::xe_gpu_texture_fetch_t fetch =
          register_file_->GetTextureFetch(tb.fetch_constant);
      view = ResolvedViewForAddress(fetch.base_address << 12);
    }
    // TEMP-DIAG: bounded log of every texture binding that hits a resolved
    // alias - what addresses draws sample, guest-declared vs actual resolved
    // size, and the live map size. Gated on OCCURRENCE COUNT of alias HITS
    // (not swap_count_, which races ahead during load and can't be guessed in
    // advance - see the rexgpu-native swap_count_ trap), so a short run is
    // guaranteed to capture real in-game composite/render-to-texture draws
    // wherever they land, without knowing the frame number up front.
    // REX_ALIAS_SWAP, if set, additionally restricts to that exact swap (old
    // behavior, kept for a targeted single-frame re-run once the swap of
    // interest is known).
    {
      static const uint64_t alias_swap =
          getenv("REX_ALIAS_SWAP") ? uint64_t(atoll(getenv("REX_ALIAS_SWAP"))) : UINT64_MAX;
      static uint64_t alias_log_count = 0;
      const bool swap_ok = alias_swap == UINT64_MAX || swap_count_ == alias_swap;
      // Log every 1D/2D binding attempted while ANY resolved alias exists,
      // hit or miss - a MISS here (view stayed null despite a plausible
      // alias existing) is a different bug class (stale/wrong fetch address)
      // than a hit with mismatched guest_tex vs resolved size.
      const bool alias_plausible = !resolved_target_views_.empty() &&
                                   (dimension == xenos::FetchOpDimension::k1D ||
                                    dimension == xenos::FetchOpDimension::k2D);
      if (swap_ok && alias_plausible && alias_log_count < 60) {
        ++alias_log_count;
        if (alias_log_count == 1) {
          std::string keys;
          for (const auto& kv : resolved_target_views_) {
            keys += fmt::format("0x{:08X} ", kv.first);
          }
          REXLOG_INFO("rexgpu-native: ALIASMAP [{}]", keys);
        }
        const xenos::xe_gpu_texture_fetch_t fetch =
            register_file_->GetTextureFetch(tb.fetch_constant);
        const uint32_t key = (uint32_t(fetch.base_address) << 12) & 0x1FFFF000u;
        const auto dim_it = resolved_target_dims_.find(key);
        REXLOG_INFO(
            "rexgpu-native: ALIAS{} base=0x{:08X} key=0x{:08X} draw#{} rt_base={} dim={} "
            "guest_tex={}x{} resolved={}x{} pitch={} map={}",
            view != VK_NULL_HANDLE ? "hit" : "MISS", uint32_t(fetch.base_address) << 12, key,
            draw_count_, uint32_t(register_file_->Get<reg::RB_COLOR_INFO>().color_base),
            uint32_t(dimension), uint32_t(fetch.size_2d.width) + 1,
            uint32_t(fetch.size_2d.height) + 1,
            dim_it != resolved_target_dims_.end() ? dim_it->second.first : 0,
            dim_it != resolved_target_dims_.end() ? dim_it->second.second : 0,
            uint32_t(fetch.pitch) << 5, resolved_target_views_.size());
      }
    }
    bool cache_hit = true;  // Stays true when a resolved-target alias satisfied the bind.
    if (view == VK_NULL_HANDLE) {
      cache_hit = false;
      view = texture_cache_->GetActiveBindingOrNullImageView(tb.fetch_constant, dimension,
                                                             bool(tb.is_signed), &cache_hit);
      // TEMP-DIAG: bounded log of texture-cache MISSES (opaque-black
      // fallback, NativeTextureCache::NullImageViewForDimension) - a miss
      // here renders black, not white, so this is here to positively rule
      // the cache-miss path in or out as the cause of a solid-white surface
      // (e.g. PGR3's road) rather than assuming it.
      if (REXCVAR_GET(native_log_draws)) {
        static uint64_t miss_log_count = 0;
        if (!cache_hit && miss_log_count < 60) {
          ++miss_log_count;
          const xenos::xe_gpu_texture_fetch_t fetch =
              register_file_->GetTextureFetch(tb.fetch_constant);
          REXLOG_INFO(
              "rexgpu-native: TEXMISS draw#{} fetch_slot={} dim={} guest_tex={}x{} "
              "base=0x{:08X} rt_base={}",
              draw_count_, tb.fetch_constant, uint32_t(dimension),
              uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
              uint32_t(fetch.base_address) << 12,
              uint32_t(register_file_->Get<reg::RB_COLOR_INFO>().color_base));
        }
      }
    }
    // Cumulative bind outcomes, read from the periodic SKIPS line.
    // tex_miss counts CACHE MISSES (the cache's own opaque-black null view),
    // which is the number that matters. Counting `view == VK_NULL_HANDLE`
    // instead reads 0 even when every texture misses, because the cache
    // returns its null VIEW rather than a null HANDLE - that mistake made an
    // earlier run look like "textures are fine" on no evidence.
    ++texture_bind_total_;
    if (!cache_hit) {
      ++texture_miss_total_;
    }
    if (view == VK_NULL_HANDLE) {
      ++texture_null_total_;
      // No cache view (e.g. cache disabled) - fall back to the dummy white one.
      view = DummyViewForDimension(dimension);
    }
    image_infos[i].imageView = view;
    image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = i;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &image_infos[i];
    writes.push_back(w);
  }
  for (uint32_t j = 0; j < sampler_count; ++j) {
    VkSampler sampler =
        texture_cache_->UseSampler(texture_cache_->GetSamplerParameters(samplers[j]));
    if (sampler == VK_NULL_HANDLE) {
      sampler = dummy_sampler_;
    }
    sampler_infos[j].sampler = sampler;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = texture_count + j;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &sampler_infos[j];
    writes.push_back(w);
  }
  if (!writes.empty()) {
    dfn.vkUpdateDescriptorSets(vulkan_device_->device(), uint32_t(writes.size()), writes.data(), 0,
                               nullptr);
  }
  return set;
}

#endif  // REX_HAS_VULKAN

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t hash = HashUcode(host_address, dword_count);
  auto it = shader_map_.find(hash);
  if (it != shader_map_.end()) {
    return it->second;
  }

  // A SpirvShader keeps the parsed microcode and its SPIR-V translations.
  auto shader = std::make_unique<SpirvShader>(shader_type, hash, host_address, dword_count);
  Shader* ptr = shader.get();
  shader_storage_.push_back(std::move(shader));
  shader_map_.emplace(hash, ptr);

  REXLOG_TRACE("rexgpu-native: LoadShader type={} guest=0x{:08X} dwords={} hash=0x{:016X}",
               shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", guest_address, dword_count,
               hash);
  return ptr;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)major_mode_explicit;
  ++draw_count_;

  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: IssueDraw #{} prim={} indices={} indexed={} vs={} ps={}",
                draw_count_, static_cast<uint32_t>(prim_type), index_count,
                index_buffer_info != nullptr, static_cast<void*>(active_vertex_shader()),
                static_cast<void*>(active_pixel_shader()));
  }

#if REX_HAS_VULKAN
  if (!draw_resources_ok_) {
    return true;
  }
  const RegisterFile& regs = *register_file_;

  auto skip = [&](const char* reason) {
    ++skipped_draw_total_;
    // Counts live on the object (not in a static) so IssueSwap can dump the
    // whole histogram periodically. An early-only or flood-gated log is
    // useless here: the per-draw flood rotates it out of the log file long
    // before a real in-game frame arrives.
    ++skip_reason_counts_[reason];
    uint64_t& n = skip_reason_counts_[reason];
    if (REXCVAR_GET(native_log_draws) && (n == 1 || (n % 2000) == 0)) {
      REXLOG_INFO("rexgpu-native: skip draw #{} reason={} count={} prim={}", draw_count_, reason, n,
                  uint32_t(prim_type));
    }
    return true;  // Draw is consumed; just not rendered natively yet.
  };

  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }

  VkPrimitiveTopology topology;
  if (!MapPrimitiveTopology(prim_type, topology)) {
    return skip("primitive_type");
  }

  auto* vertex_shader = static_cast<SpirvShader*>(active_vertex_shader());
  if (!vertex_shader) {
    return skip("missing_vertex_shader");
  }

  rex::string::StringBuffer ucode_buffer;
  vertex_shader->AnalyzeUcode(ucode_buffer);

  auto* pixel_shader = static_cast<SpirvShader*>(active_pixel_shader());
  if (pixel_shader) {
    pixel_shader->AnalyzeUcode(ucode_buffer);
  }

  // The decision table (including the two orderings that matter) lives in
  // draw_classify.h so it is unit tested rather than re-derived here; see
  // tests/unit/graphics/draw_classify_test.cpp.
  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  DrawFacts facts;
  facts.edram_mode = edram_mode;
  facts.rasterization_possible =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  facts.vertex_memexport = vertex_shader->memexport_eM_written();
  facts.has_pixel_shader = pixel_shader != nullptr;
  facts.pixel_shader_needed =
      pixel_shader && draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs);
  facts.pixel_memexport = pixel_shader && pixel_shader->memexport_eM_written();

  switch (ClassifyDraw(facts)) {
    case DrawDisposition::kResolve:
      return IssueCopy();
    case DrawDisposition::kSkipVertexMemexport:
      return skip("memexport_vertex");
    case DrawDisposition::kSkipNoRasterization:
      return skip("no_rasterization");
    case DrawDisposition::kSkipPixelMemexport:
      return skip("memexport_pixel");
    case DrawDisposition::kDepthOnly:
      // Rasterize with NO fragment stage - this draw contributes depth and
      // nothing else. Dropping these instead left every depth pre-pass out of
      // the depth buffer, so 3D geometry failed its depth test against a
      // buffer still cleared to 1.0 and the world rendered black, while
      // z-testless HUD/2D drew fine.
      pixel_shader = nullptr;
      break;
    case DrawDisposition::kFull:
      break;
  }
  const bool depth_only = pixel_shader == nullptr;

  const reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  // Nothing consumes the interpolators without a fragment stage, so the vertex
  // shader is translated to write none (oracle: vulkan/command_processor.cpp:3813).
  const uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  // --- Shader modifications (host-render-target path; no tessellation, UCP or
  // memexport in this milestone). ---
  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  // Rectangle lists give 3 corners and leave the 4th implied. Without expansion
  // only the first triangle is drawn, which is exact for the oversized-triangle
  // fullscreen trick but loses everything past the diagonal on a genuine
  // rectangle - the diagonal seam through Hydro Thunder's composite blits.
  //
  // Still off, but the reason is now pinned down rather than guessed.
  //
  // The index side is NOT the problem. The encoding (primitive << 2 | corner,
  // strip 0,1,2,3 emitted as list 0,1,2 / 2,1,3) is covered by unit tests in
  // tests/unit/graphics/index_expand_test.cpp, and measurement shows every
  // rectangle draw Hydro Thunder issues is auto-indexed (RECTDRAW: count=3
  // indexed=false) - so the "expansion never reads the guest index buffer"
  // theory does not explain the regression either.
  //
  // Turning it on (2026-07-19) puts a huge white triangle across Hydro
  // Thunder's frame and mangles the HUD, which is corner 3 - the IMPLIED fourth
  // corner - coming out as garbage. That reconstruction happens in the vertex
  // shader under HostVertexShaderType::kRectangleListAsTriangleStrip (see the
  // vmod below), so the remaining work is on the shader side, not here.
  const bool expand_rects = REXCVAR_GET(native_expand_rects);
  // TEMP-DIAG: characterise every rectangle-list draw - are they auto-indexed
  // or DMA-indexed? Decides whether the reverted expansion's missing guest
  // index-buffer read explains the Geometry Wars regression.
  if (prim_type == xenos::PrimitiveType::kRectangleList) {
    static uint64_t rect_log = 0;
    if (REXCVAR_GET(native_log_draws) && rect_log < 48) {
      ++rect_log;
      REXLOG_INFO("rexgpu-native: RECTDRAW #{} count={} indexed={} fmt={} endian={} swap={}",
                  draw_count_, index_count, index_buffer_info != nullptr,
                  index_buffer_info ? uint32_t(index_buffer_info->format) : 99u,
                  index_buffer_info ? uint32_t(index_buffer_info->endianness) : 99u, swap_count_);
    }
  }
  SpirvShaderTranslator::Modification vmod(shader_translator_->GetDefaultVertexShaderModification(
      vertex_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
      expand_rects ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
                   : Shader::HostVertexShaderType::kVertex));
  vmod.vertex.interpolator_mask = interpolator_mask;
  vmod.vertex.output_point_parameters =
      uint32_t((vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               prim_type == xenos::PrimitiveType::kPointList);
  // Register-derived, not hardcoded. Forcing these to 0 is invisible on 2D
  // passes (which set clip_disable and don't kill vertices) and destroys 3D
  // ones: guest-clipped geometry rasterizes unclipped, and a kill-vertex
  // shader falls into the NaN-position.w path whose NaN poisons the rectangle
  // diagonal search. Derivations are unit tested in shader_constants.h.
  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto& dev_props = vulkan_device_->properties();
  const UserClipPlaneConfig ucp = ComputeUserClipPlanes(
      pa_cl_clip_cntl.clip_disable != 0, pa_cl_clip_cntl.ucp_ena,
      pa_cl_clip_cntl.ucp_cull_only_ena != 0, dev_props.shaderClipDistance != 0,
      dev_props.shaderCullDistance != 0);
  vmod.vertex.user_clip_plane_count = ucp.count;
  vmod.vertex.user_clip_plane_cull = ucp.cull;
  vmod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  vmod.vertex.vertex_kill_and =
      uint32_t(ComputeVertexKillAnd(dev_props.shaderCullDistance != 0,
                                    vertex_shader->writes_point_size_edge_flag_kill_vertex(),
                                    pa_cl_clip_cntl.vtx_kill_or != 0));

  // --- Translate. A depth-only draw has no pixel shader to translate. ---
  Shader::Translation* vtrans = vertex_shader->GetOrCreateTranslation(vmod.value);
  if (!vtrans->is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(*vtrans)) {
      return skip("vs_translate_failed");
    }
  }
  if (!vtrans->is_valid()) {
    return skip("translation_invalid");
  }
  VkShaderModule vs_module = GetShaderModule(vtrans);
  if (vs_module == VK_NULL_HANDLE) {
    return skip("shader_module");
  }

  VkShaderModule ps_module = VK_NULL_HANDLE;
  // Declared outside the if() - needed below to build the geometry-shader key
  // even when there IS a pixel shader (kRectangleList composite blits are
  // textured quads, not depth-only draws). Stays value-0 (matching the
  // oracle's PipelineDescription::pixel_shader_modification for a draw with
  // no pixel shader) when pixel_shader is null.
  SpirvShaderTranslator::Modification pmod;
  if (pixel_shader) {
    pmod = SpirvShaderTranslator::Modification(shader_translator_->GetDefaultPixelShaderModification(
        pixel_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
    pmod.pixel.interpolator_mask = interpolator_mask;
    pmod.pixel.interpolators_centroid = 0;
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      pmod.pixel.param_gen_enable = 1;
      pmod.pixel.param_gen_interpolator = ps_param_gen_pos;
      pmod.pixel.param_gen_point =
          uint32_t(prim_type == xenos::PrimitiveType::kPointList);
    }
    pmod.pixel.depth_stencil_mode =
        SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;

    Shader::Translation* ptrans = pixel_shader->GetOrCreateTranslation(pmod.value);
    if (!ptrans->is_translated()) {
      if (!shader_translator_->TranslateAnalyzedShader(*ptrans)) {
        return skip("ps_translate_failed");
      }
    }
    if (!ptrans->is_valid()) {
      return skip("translation_invalid");
    }
    ps_module = GetShaderModule(ptrans);
    if (ps_module == VK_NULL_HANDLE) {
      return skip("shader_module");
    }
  }

  // Texture/sampler binding counts are known after translation. Textured draws
  // are rendered with a dummy white texture bound to every binding (Phase 2.5),
  // so the geometry appears instead of being skipped.
  const uint32_t vtex = uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  const uint32_t vsamp = uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  // A depth-only draw samples nothing in the fragment stage; 0/0 resolves to a
  // valid layout and the shared empty_texture_set_.
  const uint32_t ptex =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  const uint32_t psamp =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;
  VkPipelineLayout pipeline_layout = GetGuestPipelineLayout(vtex, vsamp, ptex, psamp);
  if (pipeline_layout == VK_NULL_HANDLE) {
    return skip("pipeline_layout");
  }
  // Phase 3: real register-derived blend / depth / cull instead of the Phase 2
  // hardcoded blend-off / depth-off / cull-none.
  // pixel_writes_color_targets = 0 makes GetNormalizedColorMask return 0, which
  // in turn leaves color_write_mask 0 and blending off - exactly what a
  // stage-less depth-only pipeline needs, with no special-casing here.
  const GuestPipelineState pipeline_state =
      BuildPipelineState(topology, primitive_polygonal, normalized_depth_control,
                         pixel_shader ? pixel_shader->writes_color_targets() : 0);
  // A stage-less pipeline with a live write mask would write undefined color
  // into the target. GuestPipelineState needs no "depth only" field - ps_module
  // is already mixed into the pipeline cache key and color_write_mask is already
  // in Hash() - so this is the guardrail that replaces one.
  if (depth_only && pipeline_state.color_write_mask != 0) {
    return skip("depth_only_color_mask");
  }
  // kRectangleList's 3 guest vertices per rect leave the 4th corner implied.
  // MapPrimitiveTopology already feeds this a bare TRIANGLE_LIST (one
  // primitive = one rect, no index expansion) - exactly the input the
  // oracle's kRectangleList geometry shader expects - so attach that same,
  // already-validated GS instead of drawing half the rect (see
  // MapPrimitiveTopology's comment). Mutually exclusive with expand_rects:
  // that path already expands the index buffer to 2 full triangles per rect
  // and reconstructs the 4th corner in the vertex shader instead
  // (unvalidated - see its cvar comment), so vmod's host_vertex_shader_type
  // is kRectangleListAsTriangleStrip there, not kVertex, and
  // GetGeometryShaderKey correctly declines it.
  VkShaderModule geometry_module = VK_NULL_HANDLE;
  if (prim_type == xenos::PrimitiveType::kRectangleList && !expand_rects &&
      REXCVAR_GET(native_rect_gs)) {
    vulkan::VulkanPipelineCache::GeometryShaderKey gs_key;
    if (vulkan::VulkanPipelineCache::GetGeometryShaderKey(
            vulkan::VulkanPipelineCache::PipelineGeometryShader::kRectangleList, vmod, pmod,
            gs_key)) {
      geometry_module = GetGeometryShader(gs_key);
      if (geometry_module == VK_NULL_HANDLE) {
        return skip("geometry_shader_unavailable");
      }
    }
  } else if (prim_type == xenos::PrimitiveType::kPointList) {
    // Point sprites: the oracle NEVER draws guest point lists as bare 1-pixel
    // points - it always attaches the kPointList geometry shader, which
    // expands each point into a screen-facing quad sized by the point system
    // constants (set below) and gives the pixel shader its sprite UVs via
    // param_gen. Topology stays POINT_LIST - exactly the GS's input.
    vulkan::VulkanPipelineCache::GeometryShaderKey gs_key;
    if (vulkan::VulkanPipelineCache::GetGeometryShaderKey(
            vulkan::VulkanPipelineCache::PipelineGeometryShader::kPointList, vmod, pmod, gs_key)) {
      geometry_module = GetGeometryShader(gs_key);
      if (geometry_module == VK_NULL_HANDLE) {
        return skip("geometry_shader_unavailable");
      }
    }
  }
  VkPipeline pipeline =
      GetPipeline(vs_module, ps_module, pipeline_layout, pipeline_state, geometry_module);
  if (pipeline == VK_NULL_HANDLE) {
    return skip("pipeline");
  }

  BeginFrameIfNeeded();
  if (deferred_draws_.size() >= kMaxDrawsPerFrame) {
    return skip("draw_budget");
  }

  // Phase 3: request + untile the real guest textures this draw's shaders use.
  // Loads are immediate (submit-and-wait) so the textures are resident before
  // the deferred draw replays at swap time.
  if (texture_cache_) {
    const uint32_t used_texture_mask =
        vertex_shader->GetUsedTextureMaskAfterTranslation() |
        (pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
    texture_cache_->RequestTextures(used_texture_mask);
  }

  // --- Viewport / NDC. ---
  uint32_t viewport_max_x = vulkan_device_->properties().maxViewportDimensions[0];
  uint32_t viewport_max_y = vulkan_device_->properties().maxViewportDimensions[1];
  const uint32_t color_edram_base = regs.Get<reg::RB_COLOR_INFO>().color_base;
  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    const char* viewport_source = "device_max_fallthrough";
    const auto surface_extent_it = edram_base_surface_extents_.find(color_edram_base);
    const bool surface_extent_hit =
        surface_extent_it != edram_base_surface_extents_.end() && surface_extent_it->second.first &&
        surface_extent_it->second.second;
    const uint32_t resolve_extent_x =
        surface_extent_it != edram_base_surface_extents_.end() ? surface_extent_it->second.first : 0;
    const uint32_t resolve_extent_y =
        surface_extent_it != edram_base_surface_extents_.end() ? surface_extent_it->second.second : 0;
    draw_util::Scissor scissor;
    draw_util::GetScissor(regs, scissor);
    const uint32_t scissor_right = scissor.offset[0] + scissor.extent[0];
    const uint32_t scissor_bottom = scissor.offset[1] + scissor.extent[1];
    const bool scissor_x_valid =
        scissor_right && scissor_right < xenos::kTexture2DCubeMaxWidthHeight;
    const bool scissor_y_valid =
        scissor_bottom && scissor_bottom < xenos::kTexture2DCubeMaxWidthHeight;
    if (surface_extent_hit) {
      viewport_max_x = surface_extent_it->second.first;
      viewport_max_y = surface_extent_it->second.second;
      viewport_source = "resolve_extent_hit";
    } else if (scissor_x_valid || scissor_y_valid) {
      if (scissor_x_valid) {
        viewport_max_x = scissor_right;
      }
      if (scissor_y_valid) {
        viewport_max_y = scissor_bottom;
      }
      viewport_source = "scissor_fallback";
    }
    if (REXCVAR_GET(native_log_draws)) {
      const auto scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      const auto scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      REXLOG_INFO(
          "rexgpu-native: viewport_clip_disable draw=#{} base=0x{:X} source={} "
          "resolve_extent={}x{} raw_scissor=tl=0x{:08X}[{},{} off_disable={}] "
          "br=0x{:08X}[{},{}] decoded_scissor=off={}+{} extent={}x{} right_bottom={}x{} "
          "valid={}x{} device_max={}x{} final_max={}x{}",
          draw_count_, color_edram_base, viewport_source, resolve_extent_x, resolve_extent_y,
          scissor_tl.value, scissor_tl.tl_x, scissor_tl.tl_y, scissor_tl.window_offset_disable,
          scissor_br.value, scissor_br.br_x, scissor_br.br_y, scissor.offset[0], scissor.offset[1],
          scissor.extent[0], scissor.extent[1], scissor_right, scissor_bottom, scissor_x_valid,
          scissor_y_valid, vulkan_device_->properties().maxViewportDimensions[0],
          vulkan_device_->properties().maxViewportDimensions[1], viewport_max_x, viewport_max_y);
    }
  }
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, false, viewport_max_x, viewport_max_y, true,
                                 normalized_depth_control, false, false,
                                 pixel_shader && pixel_shader->writes_depth(), viewport_info);

  // --- System constants. ---
  SpirvShaderTranslator::SystemConstants system_constants;
  std::memset(&system_constants, 0, sizeof(system_constants));
  uint32_t flags = 0;
  const auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // NOTE: `flags` is committed to system_constants AFTER the per-render-target
  // loop below, which contributes the gamma bits. Assigning it here instead
  // silently discards them.
  system_constants.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  system_constants.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  system_constants.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  system_constants.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  for (uint32_t i = 0; i < 3; ++i) {
    system_constants.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  // Point sprites: the kPointList geometry shader expands each 1-vertex point
  // into a screen-facing quad sized by these (register-derived, mirroring
  // vulkan/command_processor.cpp:6285-6318 with draw resolution scale 1).
  // Left zeroed, every sprite expands to nothing - invisible sprites with a
  // perfectly clean log.
  if (prim_type == xenos::PrimitiveType::kPointList) {
    const auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    system_constants.point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    system_constants.point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    system_constants.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    system_constants.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    // 2 because 1 in NDC is half the viewport axis, 0.5 for diameter->radius -
    // they cancel (see the oracle's comment).
    system_constants.point_screen_diameter_to_ndc_radius[0] =
        1.0f / std::max(viewport_info.xy_extent[0], uint32_t(1));
    system_constants.point_screen_diameter_to_ndc_radius[1] =
        1.0f / std::max(viewport_info.xy_extent[1], uint32_t(1));
  }
  // Colour exponent bias lives in RB_COLOR_INFO bits 20:25 and the shader
  // multiplies output by 2^bias. Hardcoding 1.0f blows out any render target
  // carrying a bias - a prime suspect for washed-out 3D scenes.
  //
  // k_16_16/k_16_16_16_16 need the oracle's -32..32 -> -1..1 remap
  // (AdjustColorExpBiasForFormat, vulkan/command_processor.cpp:6385-6398)
  // whenever the host render target ISN'T a true SNORM16 that already
  // represents that range - the oracle checks device SNORM16 support via
  // VulkanRenderTargetCache::IsFixed{RG16,RGBA16}TruncatedToMinus1To1(),
  // but native has no per-guest-format render target selection at all: every
  // native render target is VK_FORMAT_R8G8B8A8_UNORM (see e.g.
  // EnsureResolveRenderTarget/clear_framebuffer_ image creation), so it is
  // ALWAYS the untruncated/"fallback to float" case for these two formats -
  // unconditionally true here, not read from any device capability. Left
  // unapplied, a k_16_16_16_16 render target's colour comes out ~32x too
  // bright (PGR3's road went flat white; the HUD/UI, on ordinary 8888
  // targets, was unaffected - exactly the split Jon reported).
  for (uint32_t i = 0; i < 4; ++i) {
    const auto rt_color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    const bool is_fixed_16_format =
        rt_color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        rt_color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
    const int32_t adjusted_bias = AdjustColorExpBiasForFormat(
        rt_color_info.color_exp_bias, is_fixed_16_format, /*truncated_to_minus_1_to_1=*/false);
    system_constants.color_exp_bias[i] = ColorExpBiasScale(adjusted_bias);
    // Gamma targets: the Xenos converts linear->gamma on write, which the
    // oracle emulates with explicit PWL gamma logic in the shader
    // (vulkan/command_processor.cpp:6165-6175). Native never set this, so a
    // guest gamma target received raw linear colour.
    if (rt_color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    // TEMP-DIAG: bounded log of every NONZERO exponent bias seen, with the
    // render target format and the raw vs adjusted value - ground truth for
    // whether a bias is actually in play here (and on which format) rather
    // than assuming it.
    if (REXCVAR_GET(native_log_draws) && rt_color_info.color_exp_bias != 0) {
      static uint64_t bias_log_count = 0;
      if (bias_log_count < 80) {
        ++bias_log_count;
        REXLOG_INFO(
            "rexgpu-native: EXPBIAS draw#{} rt={} fmt={} raw_bias={} fixed16={} adjusted={} "
            "scale={}",
            draw_count_, i, uint32_t(rt_color_info.color_format), int32_t(rt_color_info.color_exp_bias),
            is_fixed_16_format, adjusted_bias, system_constants.color_exp_bias[i]);
      }
    }
  }
  // Committed here, after the loop above has contributed the gamma bits.
  system_constants.flags = flags;
  // The shader reads only the planes that are enabled, tightly packed.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* ucp_write = system_constants.user_clip_planes[0];
    uint32_t ucp_remaining = pa_cl_clip_cntl.ucp_ena & 0x3Fu;
    uint32_t ucp_index;
    while (rex::bit_scan_forward(ucp_remaining, &ucp_index)) {
      ucp_remaining &= ~(uint32_t(1) << ucp_index);
      std::memcpy(ucp_write, &regs[XE_GPU_REG_PA_CL_UCP_0_X + ucp_index * 4],
                  4 * sizeof(float));
      ucp_write += 4;
    }
  }

  // --- Upload constant UBOs and build the constants descriptor set. ---
  const VkDeviceSize ubo_align =
      VkDeviceSize(vulkan_device_->properties().minUniformBufferOffsetAlignment);
  std::array<VkDescriptorBufferInfo, SpirvShaderTranslator::kConstantBufferCount> ubo_infos = {};
  auto upload_ubo = [&](uint32_t binding, const void* src, size_t size) -> bool {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, size, ubo_align, buffer, offset);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, src, size);
    ubo_infos[binding].buffer = buffer;
    ubo_infos[binding].offset = offset;
    ubo_infos[binding].range = size;
    return true;
  };

  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferSystem, &system_constants,
                  sizeof(system_constants))) {
    return skip("ubo_overflow_system");
  }

  // Vertex float constants (tightly packed per the shader's bitmap; base 000).
  const Shader::ConstantRegisterMap& vmap = vertex_shader->constant_register_map();
  {
    size_t float_size = sizeof(float) * 4 * std::max(vmap.float_count, UINT32_C(1));
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, float_size, ubo_align, buffer, offset);
    if (!mapping) {
      return skip("ubo_overflow_vfloat");
    }
    uint8_t* write = mapping;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = vmap.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(1ull << index);
        std::memcpy(write, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (index << 2)],
                    sizeof(float) * 4);
        write += sizeof(float) * 4;
      }
    }
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].buffer = buffer;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].offset = offset;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].range = float_size;
  }

  // Pixel float constants (base 256). A depth-only draw has no pixel shader and
  // so no constant map - but the slot is still MANDATORY: the descriptor write
  // below fills all kConstantBufferCount bindings unconditionally, and leaving
  // this one's buffer VK_NULL_HANDLE is an invalid write. Bind a zeroed vec4.
  const Shader::ConstantRegisterMap* pmap =
      pixel_shader ? &pixel_shader->constant_register_map() : nullptr;
  {
    size_t float_size = sizeof(float) * 4 * std::max(pmap ? pmap->float_count : 0u, UINT32_C(1));
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, float_size, ubo_align, buffer, offset);
    if (!mapping) {
      return skip("ubo_overflow_pfloat");
    }
    uint8_t* write = mapping;
    if (pmap) {
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = pmap->float_bitmap[i];
        uint32_t index;
        while (rex::bit_scan_forward(bits, &index)) {
          bits &= ~(1ull << index);
          std::memcpy(write, &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (index << 2)],
                      sizeof(float) * 4);
          write += sizeof(float) * 4;
        }
      }
    } else {
      std::memset(write, 0, float_size);
    }
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].buffer = buffer;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].offset = offset;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].range = float_size;
  }

  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferBoolLoop,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], sizeof(uint32_t) * (8 + 32))) {
    return skip("ubo_overflow_bool");
  }
  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferFetch,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(uint32_t) * 6 * 32)) {
    return skip("ubo_overflow_fetch");
  }

  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    VkDescriptorSetAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = constants_descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &descriptor_set_layout_constants_;
    if (dfn.vkAllocateDescriptorSets(vulkan_device_->device(), &alloc, &constants_set) !=
        VK_SUCCESS) {
      return skip("descriptor_alloc");
    }
    VkWriteDescriptorSet writes[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = constants_set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[i].pBufferInfo = &ubo_infos[i];
    }
    dfn.vkUpdateDescriptorSets(vulkan_device_->device(),
                               SpirvShaderTranslator::kConstantBufferCount, writes, 0, nullptr);
  }

  // --- Make vertex buffers resident in shared memory (in-shader vfetch). ---
  for (uint32_t i = 0; i < rex::countof(vmap.vertex_fetch_bitmap); ++i) {
    uint32_t bits = vmap.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(bits, &j)) {
      bits &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      if (vfetch.type != xenos::FetchConstantType::kVertex &&
          vfetch.type != xenos::FetchConstantType::kInvalidVertex) {
        continue;
      }
      if (vfetch.size) {
        shared_memory_->RequestRange(vfetch.address << 2, vfetch.size << 2);
      }
    }
  }

  // --- Index buffer conversion (guest big-endian -> host little-endian). ---
  // Quad lists (4 verts/quad) are expanded into a triangle list here, since the
  // native draw path has no geometry-shader stage. Each quad's two triangles are
  // emitted in the same 0,1,3 / 3,1,2 decomposition the Vulkan backend's quad
  // geometry shader uses (GL_QUAD_STRIP order 0,1,3,2), so winding matches and
  // face culling stays consistent with the emulated backend.
  const bool expand_quads = (prim_type == xenos::PrimitiveType::kQuadList);
  static const uint32_t kQuadTri[6] = {0, 1, 3, 3, 1, 2};

  bool indexed = false;
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  uint32_t draw_index_count = index_count;

  if (expand_rects) {
    // Two triangles per rectangle. The index VALUE is what the shader decodes,
    // not a vertex offset: guest primitive index in the upper bits, host vertex
    // within the rectangle (0-3) in the low 2 bits - the same encoding the
    // shared primitive processor's builtin two-triangle-strip buffer uses. The
    // strip (0,1,2,3) is emitted as a list (0,1,2, 2,1,3) so no primitive
    // restart state is needed and the winding is unchanged.
    const uint32_t rect_index_count = RectangleListExpandedCount(index_count);
    if (!rect_index_count) {
      return skip("rect_count_zero");
    }
    uint8_t* dst = RingAllocate(index_ring_, size_t(rect_index_count) * sizeof(uint32_t),
                                sizeof(uint32_t), index_buffer, index_offset);
    if (!dst) {
      return skip("index_overflow");
    }
    GuestIndexSource rect_src;
    if (index_buffer_info != nullptr) {
      rect_src.data = memory_->TranslatePhysical(index_buffer_info->guest_base);
      rect_src.is_32bit = index_buffer_info->format == xenos::IndexFormat::kInt32;
      rect_src.endianness = index_buffer_info->endianness;
    }
    ExpandRectangleList(index_count, rect_src, reinterpret_cast<uint32_t*>(dst));
    index_type = VK_INDEX_TYPE_UINT32;
    indexed = true;
    draw_index_count = rect_index_count;
  } else if (expand_quads) {
    const uint32_t tri_index_count = QuadListExpandedCount(index_count);
    if (!tri_index_count) {
      return skip("quad_count_zero");
    }
    GuestIndexSource quad_src;
    if (index_buffer_info != nullptr) {
      quad_src.data = memory_->TranslatePhysical(index_buffer_info->guest_base);
      quad_src.is_32bit = index_buffer_info->format == xenos::IndexFormat::kInt32;
      quad_src.endianness = index_buffer_info->endianness;
    }
    const bool out32 = QuadListNeeds32Bit(index_count, quad_src);
    const size_t elem = out32 ? sizeof(uint32_t) : sizeof(uint16_t);
    uint8_t* dst =
        RingAllocate(index_ring_, size_t(tri_index_count) * elem, elem, index_buffer, index_offset);
    if (!dst) {
      return skip("index_overflow");
    }
    if (out32) {
      ExpandQuadList(index_count, quad_src, reinterpret_cast<uint32_t*>(dst));
      index_type = VK_INDEX_TYPE_UINT32;
    } else {
      ExpandQuadList(index_count, quad_src, reinterpret_cast<uint16_t*>(dst));
      index_type = VK_INDEX_TYPE_UINT16;
    }
    indexed = true;
    draw_index_count = tri_index_count;
  } else if (index_buffer_info != nullptr && index_count) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const size_t elem = is32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const size_t dst_size = size_t(index_count) * elem;
    uint8_t* dst = RingAllocate(index_ring_, dst_size, elem, index_buffer, index_offset);
    if (!dst) {
      return skip("index_overflow");
    }
    const uint8_t* src = memory_->TranslatePhysical(index_buffer_info->guest_base);
    if (is32) {
      auto* out = reinterpret_cast<uint32_t*>(dst);
      for (uint32_t k = 0; k < index_count; ++k) {
        out[k] = ConvertIndex32(src + size_t(k) * 4, index_buffer_info->endianness);
      }
      index_type = VK_INDEX_TYPE_UINT32;
    } else {
      auto* out = reinterpret_cast<uint16_t*>(dst);
      for (uint32_t k = 0; k < index_count; ++k) {
        out[k] = ConvertIndex16(src + size_t(k) * 2, index_buffer_info->endianness);
      }
      index_type = VK_INDEX_TYPE_UINT16;
    }
    indexed = true;
  }

  // --- Allocate per-draw texture descriptor sets. Phase 3 binds the REAL guest
  // textures + samplers from the cache; falls back to dummy white if the cache
  // failed to initialize. ---
  auto allocate_texture_set = [&](SpirvShader* shader, uint32_t tex, uint32_t samp) -> VkDescriptorSet {
    if (!(tex + samp)) {
      return empty_texture_set_;
    }
    VkDescriptorSetLayout tex_layout = GetTextureSetLayout(tex, samp);
    return texture_cache_ ? AllocateTextureSet(shader, tex_layout)
                          : AllocateDummyTextureSet(shader, tex_layout);
  };
  VkDescriptorSet vertex_texture_set = allocate_texture_set(vertex_shader, vtex, vsamp);
  VkDescriptorSet pixel_texture_set = allocate_texture_set(pixel_shader, ptex, psamp);
  if (vertex_texture_set == VK_NULL_HANDLE || pixel_texture_set == VK_NULL_HANDLE) {
    return skip("texture_descriptor");
  }

  // --- Capture the deferred draw. ---
  DeferredDraw draw = {};
  draw.pipeline = pipeline;
  draw.pipeline_layout = pipeline_layout;
  draw.constants_set = constants_set;
  draw.vertex_texture_set = vertex_texture_set;
  draw.pixel_texture_set = pixel_texture_set;
  draw.viewport.x = float(viewport_info.xy_offset[0]);
  draw.viewport.y = float(viewport_info.xy_offset[1]);
  draw.viewport.width = float(std::max(viewport_info.xy_extent[0], UINT32_C(1)));
  draw.viewport.height = float(std::max(viewport_info.xy_extent[1], UINT32_C(1)));
  draw.viewport.minDepth = viewport_info.z_min;
  draw.viewport.maxDepth = viewport_info.z_max;
  draw.scissor.offset = {int32_t(viewport_info.xy_offset[0]), int32_t(viewport_info.xy_offset[1])};
  draw.scissor.extent = {std::max(viewport_info.xy_extent[0], UINT32_C(1)),
                         std::max(viewport_info.xy_extent[1], UINT32_C(1))};
  draw.indexed = indexed;
  draw.index_buffer = index_buffer;
  draw.index_offset = index_offset;
  draw.index_type = index_type;
  draw.draw_count = draw_index_count;
  draw.color_edram_base = color_edram_base;
  draw.depth_only = depth_only;
  {
    const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    draw.depth_edram_base =
        rb_depth_info.depth_base | (uint32_t(rb_depth_info.depth_base_bit_11) << 11);
  }
  draw.depth_control_raw = normalized_depth_control.value;
  draw.blend_constants[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  draw.blend_constants[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  draw.blend_constants[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  draw.blend_constants[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  ++rt_format_counts_[uint32_t(regs.Get<reg::RB_COLOR_INFO>().color_format) & 15];
  // TEMP-DIAG: the frontbuffer-phase draws (base 0) are where the composite goes
  // wrong - log their geometry and viewport.
  if (draw.color_edram_base == 0 && swap_count_ > 3000) {
    static uint64_t fb_draw_log = 0;
    if (REXCVAR_GET(native_log_draws) && fb_draw_log < 64) {
      ++fb_draw_log;
      REXLOG_INFO(
          "rexgpu-native: FBDRAW #{} prim={} count={} vp=({},{} {}x{}) ndc_scale=({},{},{}) "
          "ndc_off=({},{},{}) vtex={} ptex={}",
          draw_count_, uint32_t(prim_type), draw.draw_count, draw.viewport.x, draw.viewport.y,
          draw.viewport.width, draw.viewport.height, viewport_info.ndc_scale[0],
          viewport_info.ndc_scale[1], viewport_info.ndc_scale[2], viewport_info.ndc_offset[0],
          viewport_info.ndc_offset[1], viewport_info.ndc_offset[2], vtex, ptex);
    }
  }
  deferred_draws_.push_back(draw);
  ++deferred_draw_total_;

  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: draw #{} queued prim={} verts={} indexed={} vp={}x{}+{}+{}",
                draw_count_, uint32_t(prim_type), index_count, indexed,
                viewport_info.xy_extent[0], viewport_info.xy_extent[1], viewport_info.xy_offset[0],
                viewport_info.xy_offset[1]);
  }
  return true;
#else
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  return true;
#endif  // REX_HAS_VULKAN
}

#if REX_HAS_VULKAN
bool NativeCommandProcessor::EnsureResolveRenderTarget(uint32_t width, uint32_t height) {
  width = std::max(width, 1u);
  height = std::max(height, 1u);
  if (resolve_rt_color_ != VK_NULL_HANDLE && resolve_rt_width_ >= width &&
      resolve_rt_height_ >= height) {
    return true;
  }
  // Grow to cover the requested size (never shrink) so one RT serves every phase.
  width = std::max(width, resolve_rt_width_);
  height = std::max(height, resolve_rt_height_);
  DestroyResolveRenderTarget();

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  auto make_image = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage& image_out, VkDeviceMemory& memory_out,
                        VkImageView& view_out) -> bool {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn.vkCreateImage(device, &image_info, nullptr, &image_out) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req;
    dfn.vkGetImageMemoryRequirements(device, image_out, &req);
    uint32_t type_index;
    if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                               &type_index) &&
        !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
      return false;
    }
    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type_index;
    if (dfn.vkAllocateMemory(device, &alloc, nullptr, &memory_out) != VK_SUCCESS ||
        dfn.vkBindImageMemory(device, image_out, memory_out, 0) != VK_SUCCESS) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_out;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, 1, 0, 1};
    return dfn.vkCreateImageView(device, &view_info, nullptr, &view_out) == VK_SUCCESS;
  };

  if (!make_image(kSceneColorFormat,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, resolve_rt_color_, resolve_rt_color_memory_,
                  resolve_rt_color_view_)) {
    REXLOG_ERROR("rexgpu-native: resolve RT color alloc failed {}x{}", width, height);
    DestroyResolveRenderTarget();
    return false;
  }
  if (!make_image(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, resolve_rt_depth_, resolve_rt_depth_memory_,
                  resolve_rt_depth_view_)) {
    REXLOG_ERROR("rexgpu-native: resolve RT depth alloc failed {}x{}", width, height);
    DestroyResolveRenderTarget();
    return false;
  }
  const VkImageView attachments[2] = {resolve_rt_color_view_, resolve_rt_depth_view_};
  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.renderPass = clear_render_pass_;
  fb_info.attachmentCount = 2;
  fb_info.pAttachments = attachments;
  fb_info.width = width;
  fb_info.height = height;
  fb_info.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_info, nullptr, &resolve_rt_framebuffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: resolve RT framebuffer failed");
    DestroyResolveRenderTarget();
    return false;
  }
  resolve_rt_width_ = width;
  resolve_rt_height_ = height;
  return true;
}

void NativeCommandProcessor::DestroyResolveRenderTarget() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (resolve_rt_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, resolve_rt_framebuffer_, nullptr);
    resolve_rt_framebuffer_ = VK_NULL_HANDLE;
  }
  VkImageView views[2] = {resolve_rt_color_view_, resolve_rt_depth_view_};
  VkImage images[2] = {resolve_rt_color_, resolve_rt_depth_};
  VkDeviceMemory mems[2] = {resolve_rt_color_memory_, resolve_rt_depth_memory_};
  for (int i = 0; i < 2; ++i) {
    if (views[i] != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, views[i], nullptr);
    if (images[i] != VK_NULL_HANDLE) dfn.vkDestroyImage(device, images[i], nullptr);
    if (mems[i] != VK_NULL_HANDLE) dfn.vkFreeMemory(device, mems[i], nullptr);
  }
  resolve_rt_color_view_ = resolve_rt_depth_view_ = VK_NULL_HANDLE;
  resolve_rt_color_ = resolve_rt_depth_ = VK_NULL_HANDLE;
  resolve_rt_color_memory_ = resolve_rt_depth_memory_ = VK_NULL_HANDLE;
  resolve_rt_width_ = resolve_rt_height_ = 0;
}

bool NativeCommandProcessor::EnsureResolveStaging(VkDeviceSize size) {
  if (resolve_staging_buffer_ != VK_NULL_HANDLE && resolve_staging_size_ >= size) {
    return true;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (resolve_staging_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, resolve_staging_buffer_, nullptr);
    resolve_staging_buffer_ = VK_NULL_HANDLE;
  }
  if (resolve_staging_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, resolve_staging_memory_, nullptr);
    resolve_staging_memory_ = VK_NULL_HANDLE;
  }
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &resolve_staging_buffer_) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetBufferMemoryRequirements(device, resolve_staging_buffer_, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &resolve_staging_memory_) != VK_SUCCESS) {
    return false;
  }
  if (dfn.vkBindBufferMemory(device, resolve_staging_buffer_, resolve_staging_memory_, 0) !=
      VK_SUCCESS) {
    return false;
  }
  resolve_staging_size_ = size;
  return true;
}

void NativeCommandProcessor::ResetResolvedTargets() {
  if (resolved_target_storage_.empty() && resolved_target_views_.empty()) {
    resolved_target_views_.clear();
    resolved_target_dims_.clear();
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  for (ResolvedTarget& rt : resolved_target_storage_) {
    if (rt.view != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, rt.view, nullptr);
    if (rt.image != VK_NULL_HANDLE) dfn.vkDestroyImage(device, rt.image, nullptr);
    if (rt.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, rt.memory, nullptr);
  }
  resolved_target_storage_.clear();
  resolved_target_views_.clear();
  resolved_target_dims_.clear();
  resolved_texture_layouts_.clear();
}

void NativeCommandProcessor::DumpResolvedTargets() {
  static const char* dump_prefix = getenv("REX_DUMP_RT");
  if (!dump_prefix || phases_.empty() || !vulkan_device_) {
    return;
  }
  // Two gates: REX_DUMP_RT_SWAP=<n> (exact swap number, for a targeted
  // re-run) or REX_DUMP_RT_MINDRAWS=<n> (first frame with at least n draws -
  // catches "a real in-game frame" without knowing its swap number up front,
  // since swap_count_ races ahead during load and can't be guessed).
  static const uint64_t dump_swap =
      getenv("REX_DUMP_RT_SWAP") ? uint64_t(atoll(getenv("REX_DUMP_RT_SWAP"))) : 0;
  static const uint32_t dump_min_draws =
      getenv("REX_DUMP_RT_MINDRAWS") ? uint32_t(atoi(getenv("REX_DUMP_RT_MINDRAWS"))) : 0;
  if (dump_min_draws) {
    static bool fired = false;
    if (fired || deferred_draws_.size() < dump_min_draws) {
      return;
    }
    fired = true;
  } else if (swap_count_ != dump_swap) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  for (size_t pi = 0; pi < phases_.size(); ++pi) {
    const RenderPhase& p = phases_[pi];
    if (p.resolved_index == SIZE_MAX) {
      continue;
    }
    const ResolvedTarget& rt = resolved_target_storage_[p.resolved_index];
    // Dump the WHOLE layout-sized image, not just this phase's rect - a
    // texture assembled from several tiling-strip resolves is only inspectable
    // as a whole.
    const VkDeviceSize size = VkDeviceSize(rt.width) * rt.height * kSceneColorBytesPerPixel;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_size = 0;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory, nullptr, &memory_size)) {
      continue;
    }

    dfn.vkResetCommandPool(device, command_pool_, 0);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn.vkBeginCommandBuffer(command_buffer_, &begin);
    VkImageMemoryBarrier to_src = {};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = rt.image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
    VkBufferImageCopy region = {};
    region.bufferRowLength = rt.width;
    region.bufferImageHeight = rt.height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {rt.width, rt.height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer_, rt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &region);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &back);
    dfn.vkEndCommandBuffer(command_buffer_);
    dfn.vkResetFences(device, 1, &clear_fence_);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer_;
    {
      const ui::vulkan::VulkanDevice::Queue::Acquisition acq =
          vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(acq.queue(), 1, &submit, clear_fence_) != VK_SUCCESS) {
        dfn.vkDestroyBuffer(device, buffer, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        continue;
      }
    }
    dfn.vkWaitForFences(device, 1, &clear_fence_, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
      ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, memory, 0, 0, memory_size);
      char path[512];
      snprintf(path, sizeof(path), "%s_p%02zu_%08X_base%u_%ux%u_r%ux%u+%u+%u.ppm", dump_prefix, pi,
               p.dest_key, p.src_base, rt.width, rt.height, p.rect_w, p.rect_h, p.dest_x, p.dest_y);
      FILE* f = fopen(path, "wb");
      if (f) {
        fprintf(f, "P6\n%u %u\n255\n", rt.width, rt.height);
        // kSceneColorFormat is R16G16B16A16_SFLOAT - 4 halves per pixel. HDR
        // values above 1.0 are clamped for display only; the stored image
        // keeps them.
        const uint16_t* src = static_cast<const uint16_t*>(mapped);
        for (uint32_t i = 0; i < rt.width * rt.height; ++i) {
          uint8_t rgb[3];
          for (uint32_t c = 0; c < 3; ++c) {
            const float v = HalfToFloat(src[i * 4 + c]);
            rgb[c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
          }
          fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        REXLOG_WARN("rexgpu-native: [DUMP-RT] {}", path);
      }
      dfn.vkUnmapMemory(device, memory);
    }
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
  }
}

size_t NativeCommandProcessor::AcquireResolvedTarget(uint32_t dest_key, uint32_t width,
                                                     uint32_t height) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Images are sized to the destination texture's layout, so several resolves
  // of the same key in one frame (EDRAM tiling strips) COMPOSE into one image:
  // matching dimensions always reuse the slot, and the subrect copies preserve
  // each other via the tracked image layout. Only a SIZE CHANGE on a second
  // same-frame resolve must not rebuild in place - draws earlier in THIS frame
  // already baked the existing view into their descriptor sets and only the
  // PREVIOUS frame's submit has been fenced, so destroying it would be a
  // use-after-free. Give the new size its own slot and retire the old one
  // until the next frame's fence has passed.
  const bool second_resolve_this_frame = acquired_dest_keys_this_frame_.count(dest_key) != 0;
  acquired_dest_keys_this_frame_.insert(dest_key);

  auto it = resolved_target_index_.find(dest_key);
  if (it != resolved_target_index_.end()) {
    ResolvedTarget& existing = resolved_target_storage_[it->second];
    if (existing.width == width && existing.height == height) {
      return it->second;  // Reuse - this frame's copies write into it.
    }
    if (second_resolve_this_frame) {
      retired_resolved_slots_.push_back(it->second);
    } else {
      // Size changed across frames: destroy and rebuild in place. Safe because
      // the previous frame's submit was fenced and waited before this point.
      if (existing.view != VK_NULL_HANDLE) dfn.vkDestroyImageView(device, existing.view, nullptr);
      if (existing.image != VK_NULL_HANDLE) dfn.vkDestroyImage(device, existing.image, nullptr);
      if (existing.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, existing.memory, nullptr);
      existing = ResolvedTarget{};
    }
  }

  ResolvedTarget rt;
  rt.width = width;
  rt.height = height;
  // Float: a resolved target is frequently an HDR intermediate the guest
  // samples back and tone-maps, so it must not clamp at 1.0 either.
  const VkFormat resolved_format = kSceneColorFormat;
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = resolved_format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC as well as DST: a resolved image is not only written and
  // sampled, it is also BLITTED to the scene image when it is the frontbuffer
  // the guest publishes (see ChooseDisplaySource), and read back by the RT
  // dump. Without it those reads are invalid and presentation fails outright.
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device, &image_info, nullptr, &rt.image) != VK_SUCCESS) {
    return SIZE_MAX;
  }
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(device, rt.image, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    dfn.vkDestroyImage(device, rt.image, nullptr);
    return SIZE_MAX;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &rt.memory) != VK_SUCCESS ||
      dfn.vkBindImageMemory(device, rt.image, rt.memory, 0) != VK_SUCCESS) {
    if (rt.memory != VK_NULL_HANDLE) dfn.vkFreeMemory(device, rt.memory, nullptr);
    dfn.vkDestroyImage(device, rt.image, nullptr);
    return SIZE_MAX;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = rt.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = resolved_format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (dfn.vkCreateImageView(device, &view_info, nullptr, &rt.view) != VK_SUCCESS) {
    dfn.vkFreeMemory(device, rt.memory, nullptr);
    dfn.vkDestroyImage(device, rt.image, nullptr);
    return SIZE_MAX;
  }

  size_t index;
  if (it != resolved_target_index_.end() && !second_resolve_this_frame) {
    index = it->second;
    resolved_target_storage_[index] = rt;
  } else {
    // Fresh slot. For a second same-frame resolve this also repoints the alias
    // at the newest image, which is what later draws should sample.
    index = resolved_target_storage_.size();
    resolved_target_storage_.push_back(rt);
    resolved_target_index_[dest_key] = index;
  }
  return index;
}

VkImageView NativeCommandProcessor::ResolvedViewForAddress(uint32_t guest_byte_address) const {
  if (resolved_target_views_.empty()) {
    return VK_NULL_HANDLE;
  }
  auto it = resolved_target_views_.find(guest_byte_address & 0x1FFFF000u);
  return it != resolved_target_views_.end() ? it->second : VK_NULL_HANDLE;
}

uint32_t NativeCommandProcessor::RecordDeferredDrawsForBase(VkCommandBuffer cb, uint32_t first,
                                                            uint32_t end, uint32_t base,
                                                            uint32_t width, uint32_t height) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  uint32_t recorded = 0;
  const bool base_filter = REXCVAR_GET(native_phase_base_filter);
  end = std::min(end, uint32_t(deferred_draws_.size()));
  for (uint32_t i = first; i < end; ++i) {
    const DeferredDraw& draw = deferred_draws_[i];
    // Depth-only draws bypass the base filter: they carry no meaningful color
    // base, and the native backend has one depth buffer per render pass rather
    // than one per EDRAM base - so within a pass, "depth is shared scratch" is
    // the honest model. A depth pre-pass therefore belongs to whichever phase
    // range contains it. Ordering is preserved because this is a monotonic scan.
    if (base_filter && !draw.depth_only && draw.color_edram_base != base) {
      continue;
    }
    ++recorded;
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
    dfn.vkCmdSetBlendConstants(cb, draw.blend_constants);
    VkDescriptorSet sets[4] = {shared_memory_descriptor_set_, draw.constants_set,
                               draw.vertex_texture_set, draw.pixel_texture_set};
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline_layout, 0, 4,
                                sets, 0, nullptr);
    VkViewport viewport = draw.viewport;
    dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = draw.scissor;
    if (uint32_t(scissor.offset.x) >= width || uint32_t(scissor.offset.y) >= height) {
      scissor.offset = {0, 0};
      scissor.extent = {width, height};
    } else {
      scissor.extent.width = std::min(scissor.extent.width, width - uint32_t(scissor.offset.x));
      scissor.extent.height = std::min(scissor.extent.height, height - uint32_t(scissor.offset.y));
    }
    dfn.vkCmdSetScissor(cb, 0, 1, &scissor);
    if (draw.indexed) {
      dfn.vkCmdBindIndexBuffer(cb, draw.index_buffer, draw.index_offset, draw.index_type);
      dfn.vkCmdDrawIndexed(cb, draw.draw_count, 1, 0, 0, 0);
    } else {
      dfn.vkCmdDraw(cb, draw.draw_count, 1, 0, 0);
    }
  }
  return recorded;
}

void NativeCommandProcessor::RecordDeferredDrawRange(VkCommandBuffer cb, uint32_t first,
                                                     uint32_t count, uint32_t width,
                                                     uint32_t height) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const uint32_t end = std::min(first + count, uint32_t(deferred_draws_.size()));
  for (uint32_t i = first; i < end; ++i) {
    const DeferredDraw& draw = deferred_draws_[i];
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
    dfn.vkCmdSetBlendConstants(cb, draw.blend_constants);
    VkDescriptorSet sets[4] = {shared_memory_descriptor_set_, draw.constants_set,
                               draw.vertex_texture_set, draw.pixel_texture_set};
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline_layout, 0, 4,
                                sets, 0, nullptr);
    VkViewport viewport = draw.viewport;
    dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = draw.scissor;
    if (uint32_t(scissor.offset.x) >= width || uint32_t(scissor.offset.y) >= height) {
      scissor.offset = {0, 0};
      scissor.extent = {width, height};
    } else {
      scissor.extent.width = std::min(scissor.extent.width, width - uint32_t(scissor.offset.x));
      scissor.extent.height = std::min(scissor.extent.height, height - uint32_t(scissor.offset.y));
    }
    dfn.vkCmdSetScissor(cb, 0, 1, &scissor);
    if (draw.indexed) {
      dfn.vkCmdBindIndexBuffer(cb, draw.index_buffer, draw.index_offset, draw.index_type);
      dfn.vkCmdDrawIndexed(cb, draw.draw_count, 1, 0, 0, 0);
    } else {
      dfn.vkCmdDraw(cb, draw.draw_count, 1, 0, 0);
    }
  }
}
#endif  // REX_HAS_VULKAN

bool NativeCommandProcessor::IssueCopy() {
  ++copy_count_;
#if REX_HAS_VULKAN
  // Mark the end of the current render-target phase regardless of whether we
  // capture it, so a later phase renders only its own draws.
  const uint32_t phase_first = phase_first_draw_;
  const uint32_t phase_count = uint32_t(deferred_draws_.size()) - phase_first;
  phase_first_draw_ = uint32_t(deferred_draws_.size());

  // Every exit from IssueCopy logs the destination it was for, so a resolve
  // that never becomes a phase can be traced to the exact guard that ate it.
  // Predicated-tiling titles issue several strip resolves back to back, and a
  // resolve dropped here is invisible downstream - it simply never appears in
  // the phase list. Gated on the cheap phase cvar, not the per-draw flood.
  const uint32_t diag_dest = register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE];
  const bool diag_copy = REXCVAR_GET(native_log_phases) || REXCVAR_GET(native_log_draws);
  auto copy_exit = [&](const char* why) {
    static std::unordered_map<std::string, uint64_t> n;
    const uint64_t k = ++n[why];
    if (diag_copy && (k <= 3 || (k % 250) == 0)) {
      REXLOG_INFO("rexgpu-native: COPYEXIT #{} dest=0x{:08X} why={} n={}", copy_count_, diag_dest,
                  why, k);
    }
  };
  const bool diag = REXCVAR_GET(native_log_draws);
  // NOT gated on phase_count: a resolve copies EDRAM out WITHOUT clearing it,
  // so a second resolve with no draws in between is re-publishing the same
  // content to another address - which is exactly how a title copies its
  // finished frame to the frontbuffer. Dropping those rendered SoulCalibur II,
  // OutRun, Ridge Racer 6, SoulCalibur IV, both Bionic Commandos and Rainbow
  // Islands black (all seven byte-identical at mean 0.000333).
  if (!draw_resources_ok_ || !frame_open_) {
    if (diag) {
      REXLOG_INFO("rexgpu-native: IssueCopy #{} skip res_ok={} frame_open={} phase_count={}",
                  copy_count_, draw_resources_ok_, frame_open_, phase_count);
    }
    copy_exit(!phase_count ? "no_draws_since_last_resolve" : "resources_or_frame");
    return true;
  }
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false,
                                 resolve_info)) {
    if (diag) {
      REXLOG_INFO("rexgpu-native: IssueCopy #{} skip GetResolveInfo failed", copy_count_);
    }
    copy_exit("get_resolve_info_failed");
    return true;
  }
  if (resolve_info.IsCopyingDepth()) {
    if (diag) {
      REXLOG_INFO("rexgpu-native: IssueCopy #{} skip depth resolve -> 0x{:08X} rect={}x{}",
                  copy_count_, resolve_info.copy_dest_texture_base,
                  uint32_t(resolve_info.coordinate_info.width_div_8) * 8,
                  uint32_t(resolve_info.height_div_8) * 8);
    }
    copy_exit("depth_resolve");
    return true;  // Depth resolves are not sampled as color; skip for now.
  }
  const uint32_t rect_w = resolve_info.coordinate_info.width_div_8 * 8;
  const uint32_t rect_h = resolve_info.height_div_8 * 8;
  if (!rect_w || !rect_h || !resolve_info.copy_dest_extent_length) {
    if (diag) {
      REXLOG_INFO("rexgpu-native: IssueCopy #{} skip empty rect {}x{} extent={}", copy_count_,
                  rect_w, rect_h, resolve_info.copy_dest_extent_length);
    }
    copy_exit("empty_rect");
    return true;  // Empty / broken resolve rect - silent no-op.
  }
  // Alias by the destination TEXTURE's base, not the per-rect tile-adjusted
  // address: D3D9 expresses a rect resolve by pre-adding the tiled offset of
  // the rect's 32-aligned origin to RB_COPY_DEST_BASE, so a game resolving
  // one texture in several rects (EDRAM tiling - Hydro Thunder's scene is two
  // 512x576 strips into the halves of one 1024x576 texture) presents each
  // strip under a base no fetch constant ever samples. A base that lies
  // INSIDE an already-known resolved texture is therefore attributed to that
  // texture, its position recovered by inverting GetTiledOffset2D over the
  // texture's 32x32 granule grid.
  const uint32_t dest_base = resolve_info.copy_dest_texture_base;
  uint32_t dest_key = dest_base & 0x1FFFF000u;
  uint32_t dest_x = resolve_info.copy_dest_x0;
  uint32_t dest_y = resolve_info.copy_dest_y0;
  // The resolved image gets the guest texture's declared layout size, with
  // this rect copied to its position inside it. The sampling draw's UVs are
  // normalized against that declared size, so a rect-sized image would show
  // one strip stretched across the whole quad (the "scene offset to the
  // right" bug). max() covers a bogus/zero pitch register; the cap covers a
  // bogus huge one.
  uint32_t img_w = std::min(std::max(resolve_info.copy_dest_pitch_px, dest_x + rect_w), 8192u);
  uint32_t img_h = std::min(std::max(resolve_info.copy_dest_height_px, dest_y + rect_h), 8192u);
  bool attributed = false;
  for (const auto& [owner_key, layout] : resolved_texture_layouts_) {
    if (dest_base <= layout.base_raw) {
      continue;
    }
    const uint32_t delta = dest_base - layout.base_raw;
    const uint32_t layout_bytes =
        (layout.pitch_aligned * layout.height_aligned) << layout.bpp_log2;
    if (delta >= layout_bytes) {
      continue;
    }
    // Find the 32x32 granule whose tiled offset in the owner's layout equals
    // the base delta. Granule grids are small (<= pitch/32 * height/32).
    for (uint32_t gy = 0; !attributed && gy < layout.height_aligned; gy += 32) {
      for (uint32_t gx = 0; gx < layout.pitch_aligned; gx += 32) {
        if (uint32_t(texture_util::GetTiledOffset2D(int32_t(gx), int32_t(gy),
                                                    layout.pitch_aligned, layout.bpp_log2)) !=
            delta) {
          continue;
        }
        const uint32_t cand_x = gx + resolve_info.copy_dest_x0;
        const uint32_t cand_y = gy + resolve_info.copy_dest_y0;
        if (cand_x + rect_w <= layout.img_w && cand_y + rect_h <= layout.img_h) {
          dest_key = owner_key;
          dest_x = cand_x;
          dest_y = cand_y;
          img_w = layout.img_w;
          img_h = layout.img_h;
          attributed = true;
        }
        break;
      }
    }
    if (attributed) {
      break;
    }
  }
  if (!attributed) {
    ResolvedTextureLayout& layout = resolved_texture_layouts_[dest_key];
    layout.base_raw = dest_base;
    layout.pitch_aligned = uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32)
                           << 5;
    layout.height_aligned = uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32)
                            << 5;
    layout.bpp_log2 = resolve_info.copy_dest_bpp_log2;
    layout.img_w = img_w;
    layout.img_h = img_h;
  }

  // From here on the phase is recorded even if its image capture is skipped -
  // the swap needs the draw-range/destination mapping either way.
  //
  // The phase captures every draw that targeted this EDRAM base since the base
  // was last cleared - not just the draws since the previous resolve, because
  // resolving does not clear EDRAM (see RenderPhase).
  const uint32_t src_base = resolve_info.color_edram_info.base_tiles;
  edram_base_surface_extents_[src_base] = {img_w, img_h};
  const auto clear_it = base_clear_point_.find(src_base);
  const auto last_it = base_last_resolve_.find(src_base);
  RenderPhase phase;
  phase.src_base = src_base;
  phase.first_draw = std::max(clear_it != base_clear_point_.end() ? clear_it->second : 0u,
                              last_it != base_last_resolve_.end() ? last_it->second : 0u);
  phase.end_draw = uint32_t(deferred_draws_.size());
  phase.dest_key = dest_key;
  phase.rect_w = rect_w;
  phase.rect_h = rect_h;
  phase.dest_x = dest_x;
  phase.dest_y = dest_y;
  base_last_resolve_[src_base] = phase.end_draw;
  // A resolve that also clears starts a fresh accumulation for this base.
  if (resolve_info.IsClearingColor()) {
    base_clear_point_[src_base] = phase.end_draw;
  }

  // Re-publication: this resolve owns no new draws, so it is copying EDRAM
  // content some earlier resolve of the same base already captured. Point the
  // new destination at that image instead of rendering nothing.
  if (phase.end_draw <= phase.first_draw) {
    auto prev = base_last_published_.find(src_base);
    if (prev != base_last_published_.end() &&
        prev->second.index < resolved_target_storage_.size()) {
      const ResolvedTarget& img = resolved_target_storage_[prev->second.index];
      if (img.view != VK_NULL_HANDLE) {
        resolved_target_views_[dest_key] = img.view;
        resolved_target_index_[dest_key] = prev->second.index;
        resolved_target_dims_[dest_key] = {img.width, img.height};
        phase.first_draw = prev->second.first_draw;
        phase.end_draw = prev->second.end_draw;
        phase.resolved_index = prev->second.index;
        phase.republished = true;
        phases_.push_back(phase);
        copy_exit("REPUBLISHED");
        return true;
      }
    }
    copy_exit("empty_no_prior_publish");
    return true;
  }

  if (resolves_this_frame_ >= kMaxResolveCapturesPerFrame) {
    phases_.push_back(phase);
    if (diag) {
      REXLOG_INFO("rexgpu-native: IssueCopy #{} phase-only (resolve cap {})", copy_count_,
                  resolves_this_frame_);
    }
    copy_exit("resolve_cap");
    return true;  // Bound the per-frame image-allocation cost.
  }

  // The offscreen RT must cover the resolve rect and every phase draw's viewport.
  uint32_t need_w = rect_w;
  uint32_t need_h = rect_h;
  const uint32_t phase_end = uint32_t(deferred_draws_.size());
  for (uint32_t i = phase_first; i < phase_end; ++i) {
    const DeferredDraw& d = deferred_draws_[i];
    need_w = std::max(need_w, uint32_t(d.viewport.x + d.viewport.width));
    need_h = std::max(need_h, uint32_t(d.viewport.y + d.viewport.height));
  }
  if (!EnsureResolveRenderTarget(need_w, need_h) ||
      !EnsureResolveStaging(VkDeviceSize(rect_w) * rect_h * kSceneColorBytesPerPixel)) {
    phases_.push_back(phase);
    return true;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Acquire (reuse or create) the persistent resolved image for this address,
  // sized to the destination texture's layout.
  const size_t resolved_index = AcquireResolvedTarget(dest_key, img_w, img_h);
  if (resolved_index == SIZE_MAX) {
    phases_.push_back(phase);
    return true;
  }

  // CPU-only: alias the (not yet rendered) resolved image at its destination
  // address so later draws bind it in their descriptor sets. The GPU work -
  // rendering the phase into resolve_rt_ and copying the rect into the resolved
  // image - happens in IssueSwap's single submit.
  resolved_target_views_[dest_key] = resolved_target_storage_[resolved_index].view;
  resolved_target_dims_[dest_key] = {img_w, img_h};
  phase.resolved_index = resolved_index;
  phases_.push_back(phase);
  ++resolve_count_;
  ++resolves_this_frame_;
  copy_exit("BECAME_PHASE");
  base_last_published_[src_base] = {resolved_index, phase.first_draw, phase.end_draw};
  if (REXCVAR_GET(native_log_draws)) {
    // TEMP-DIAG: EDRAM geometry + the RT bases of the phase's draws, to ground
    // the draw-range <-> resolve mapping in real data.
    char bases[96];
    size_t bp = 0;
    uint32_t last_base = UINT32_MAX;
    for (uint32_t i = phase_first; i < uint32_t(deferred_draws_.size()) && bp < sizeof(bases) - 12;
         ++i) {
      const uint32_t b = deferred_draws_[i].color_edram_base;
      if (b != last_base) {
        bp += snprintf(bases + bp, sizeof(bases) - bp, "%s%u", bp ? "," : "", b);
        last_base = b;
      }
    }
    bases[bp] = 0;
    REXLOG_INFO(
        "rexgpu-native: IssueCopy #{} resolve {}x{} phase_draws={} -> 0x{:08X} key=0x{:08X}"
        "+({},{}) tex={}x{} attr={} src_tiles={} eoff={},{} clearC={} clearD={} draw_bases=[{}]",
        copy_count_, rect_w, rect_h, phase_count, dest_base, dest_key, dest_x, dest_y, img_w,
        img_h, attributed, uint32_t(resolve_info.color_edram_info.base_tiles),
        uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) * 8,
        uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) * 8,
        resolve_info.IsClearingColor(), resolve_info.IsClearingDepth(), bases);
  }
#endif  // REX_HAS_VULKAN
  return true;
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  ++swap_count_;
#if REX_HAS_VULKAN
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    // Headless (no presentation) - drop the deferred frame and keep counting.
    deferred_draws_.clear();
    frame_open_ = false;
    return;
  }
  if (!vulkan_device_ || clear_render_pass_ == VK_NULL_HANDLE) {
    static bool no_device_logged = false;
    if (!no_device_logged) {
      no_device_logged = true;
      REXLOG_ERROR("rexgpu-native: IssueSwap has a presenter but no device state");
    }
    return;
  }

  const uint32_t width = frontbuffer_width ? frontbuffer_width : 1280u;
  const uint32_t height = frontbuffer_height ? frontbuffer_height : 720u;

  // The guest's depth clear value, read straight from the register rather than
  // captured from a clearing resolve - a depth-clear resolve does not reliably
  // reach IssueCopy's body (it is filtered by the phase-count and depth-copy
  // guards), and missing it leaves the fixed 1.0 that blacks out reverse-Z
  // titles. RB_DEPTH_CLEAR packs 24-bit depth above an 8-bit stencil.
  {
    const uint32_t rb_depth_clear = register_file_->values[XE_GPU_REG_RB_DEPTH_CLEAR];
    guest_depth_clear_ =
        std::clamp(float(rb_depth_clear >> 8) / float(0xFFFFFF), 0.0f, 1.0f);
  }

  // [TEMP DIAG] RenderDoc capture without the overlay/keyboard - ported from
  // vulkan/command_processor.cpp's IssueSwap so the same forensics workflow
  // (used successfully on PGR3's glass-confetti bug) works on this backend
  // too. Run with ENABLE_VULKAN_RENDERDOC_CAPTURE=1 (implicit layer) and
  // either REX_RENDERDOC_CAPTURE_FRAME=<n> (capture at guest swap <n>) or
  // REX_RENDERDOC_CAPTURE_DRAWS=<min>:<k> (capture at the k-th swap whose
  // frame issued at least <min> draws - robust when the exact frame number
  // isn't known). REX_RENDERDOC_CAPTURE_PATH sets the .rdc path template.
  {
    static const char* rd_frame_env = getenv("REX_RENDERDOC_CAPTURE_FRAME");
    static const char* rd_draws_env = getenv("REX_RENDERDOC_CAPTURE_DRAWS");
    const uint32_t frame_draws = uint32_t(deferred_draws_.size());
    if (rd_frame_env || rd_draws_env) {
      static auto renderdoc = ui::RenderDocAPI::CreateIfConnected();
      bool rd_trigger = false;
      if (rd_frame_env && swap_count_ == uint64_t(atoll(rd_frame_env))) {
        rd_trigger = true;
      }
      if (rd_draws_env) {
        static uint32_t rd_min_draws = 0, rd_draws_k = 1;
        static bool rd_draws_parsed = sscanf(rd_draws_env, "%u:%u", &rd_min_draws, &rd_draws_k) >= 1;
        static uint32_t rd_draws_hits = 0;
        static bool rd_draws_fired = false;
        if (rd_draws_parsed && !rd_draws_fired && frame_draws >= rd_min_draws &&
            ++rd_draws_hits == rd_draws_k) {
          rd_draws_fired = true;
          rd_trigger = true;
        }
      }
      if (renderdoc && rd_trigger) {
        if (const char* rd_path = getenv("REX_RENDERDOC_CAPTURE_PATH")) {
          renderdoc->api_1_0_0()->SetLogFilePathTemplate(rd_path);
        }
        renderdoc->api_1_0_0()->TriggerCapture();
        REXLOG_WARN("rexgpu-native: [RENDERDOC] triggered capture at guest swap {} ({}x{}, {} draws)",
                    swap_count_, width, height, frame_draws);
      }
    }
  }

  const uint32_t clear_raw = register_file_->values[XE_GPU_REG_RB_COLOR_CLEAR];
  const bool used_guest = clear_raw != 0;
  std::array<float, 4> clear_rgba;
  if (used_guest) {
    clear_rgba[0] = float((clear_raw >> 16) & 0xFF) / 255.0f;
    clear_rgba[1] = float((clear_raw >> 8) & 0xFF) / 255.0f;
    clear_rgba[2] = float(clear_raw & 0xFF) / 255.0f;
    clear_rgba[3] = float((clear_raw >> 24) & 0xFF) / 255.0f;
  } else {
    // Black, NOT the recognisable mid-blue this used to use for empty frames.
    // Loading screens produce long runs of draw-less frames interleaved with
    // drawn ones, so that debug colour showed up as violent blue flashing
    // through every level load. Keep it available for bring-up, but behind the
    // same switch as the other renderer-identification aids.
    clear_rgba = (deferred_draws_.empty() && REXCVAR_GET(native_marker_empty_frames))
                     ? std::array<float, 4>{0.16f, 0.36f, 0.72f, 1.0f}
                     : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
  }

  // Phase-aware display: draws belonging to phases that resolved to the
  // frontbuffer address (plus any trailing unresolved draws) replay into the
  // swap image; other phases render offscreen only. A frame with no
  // frontbuffer-matching resolve keeps the replay-all baseline.
  const uint32_t fb_key = ResolvedTargetKey(frontbuffer_ptr);
  // Selection lives in phase_model.h so the rules are unit tested rather than
  // re-derived here; see tests/unit/graphics/phase_model_test.cpp.
  std::vector<PhaseSpan> phase_spans;
  phase_spans.reserve(phases_.size());
  for (const RenderPhase& p : phases_) {
    phase_spans.push_back({p.src_base, p.first_draw, p.end_draw, p.dest_key});
  }
  DisplaySource display_source = ChooseDisplaySource(
      phase_spans, fb_key, uint32_t(deferred_draws_.size()), phase_first_draw_);
  // Presenting requires the resolved image to actually exist; if it does not,
  // fall back to the replay model rather than showing nothing.
  size_t present_index = SIZE_MAX;
  if (display_source.present_resolved && !REXCVAR_GET(native_present_frontbuffer)) {
    // Selection found a finished frame, but consuming it is not safe yet.
    display_source = DisplaySource{};
    display_source.ranges = SelectDisplayRanges(phase_spans, fb_key,
                                                uint32_t(deferred_draws_.size()),
                                                phase_first_draw_);
  } else if (display_source.present_resolved) {
    auto it = resolved_target_index_.find(display_source.resolved_key);
    if (it != resolved_target_index_.end() &&
        resolved_target_storage_[it->second].image != VK_NULL_HANDLE) {
      present_index = it->second;
    } else {
      display_source = DisplaySource{};
      display_source.ranges = SelectDisplayRanges(phase_spans, fb_key,
                                                  uint32_t(deferred_draws_.size()),
                                                  phase_first_draw_);
    }
  }
  const std::vector<DisplayRange>& display_ranges = display_source.ranges;

  const size_t draw_replay_count = deferred_draws_.size();
  if (REXCVAR_GET(native_log_draws) && clear_raw != last_logged_clear_raw_) {
    last_logged_clear_raw_ = clear_raw;
    if (phases_.empty()) {
      REXLOG_INFO("rexgpu-native: IssueSwap #{} {}x{} draws={} clear=0x{:08X}", swap_count_, width,
                  height, draw_replay_count, clear_raw);
    } else {
      uint32_t display_total = 0;
      for (const auto& range : display_ranges) {
        for (uint32_t i = range.first_draw; i < range.end_draw && i < deferred_draws_.size();
             ++i) {
          if (DrawInRange(deferred_draws_[i].color_edram_base, deferred_draws_[i].depth_only,
                          range.src_base)) {
            ++display_total;
          }
        }
      }
      const uint32_t trailing = uint32_t(deferred_draws_.size()) - phase_first_draw_;
      REXLOG_INFO(
          "rexgpu-native: IssueSwap #{} {}x{} draws={} clear=0x{:08X} phases={} fb_key=0x{:08X} "
          "fb_match={} display={} trail={}",
          swap_count_, width, height, draw_replay_count, clear_raw, phases_.size(), fb_key,
          // fb_match: false means selection fell back to replaying the whole
          // frame because no phase resolved to the frontbuffer.
          !(display_ranges.size() == 1 && display_ranges[0].src_base == kAnyBase &&
            display_ranges[0].first_draw == 0),
          display_total, trailing);
    }
  }

  // DIAG (REX_PHASES_SWAP): logged OUTSIDE the presenter callback, so a frame
  // where the callback never runs is distinguishable from one where it runs
  // and finds nothing to render. Those two look identical from inside.
  {
    // Gated on how many times we have REACHED HERE, not on swap_count_:
    // swap_count_ increments before the !presenter early return, so it races
    // far ahead during loading and an absolute gate never fires once rendering
    // actually starts.
    static uint64_t entry_n = 0;
    ++entry_n;
    if ((REXCVAR_GET(native_log_phases) || REXCVAR_GET(native_log_draws)) &&
        (entry_n <= 4 || (entry_n % 137) == 0)) {
      REXLOG_INFO("rexgpu-native: SWAPENTRY #{} swap={} phases={} deferred={} fb=0x{:08X}",
                  entry_n, swap_count_, phases_.size(), deferred_draws_.size(), frontbuffer_ptr);
      // The display decision itself: the frontbuffer key the guest named, what
      // every phase actually resolved to, and whether a finished image was
      // found to present. Without this the two failure modes - "no phase
      // matched the frontbuffer" and "matched, but the image was empty" - are
      // indistinguishable from a black frame.
      std::string dests;
      for (const RenderPhase& p : phases_) {
        dests += fmt::format("{:08X}{} ", p.dest_key,
                             p.end_draw > p.first_draw ? "" : "(empty)");
      }
      REXLOG_INFO("rexgpu-native: PRESENT fb_key=0x{:08X} present={} ranges={} dests=[{}]", fb_key,
                  present_index != SIZE_MAX, display_ranges.size(), dests);
      // The skip histogram: "were the missing draws rejected, and why?" - the
      // first question for a black world, answerable only from a log the
      // per-draw flood has not rotated away.
      std::string hist;
      for (const auto& [reason, n] : skip_reason_counts_) {
        hist += fmt::format("{}={} ", reason, n);
      }
      std::string rtf;
      for (uint32_t i = 0; i < 16; ++i) {
        if (rt_format_counts_[i]) {
          rtf += fmt::format("{}={} ", i, rt_format_counts_[i]);
        }
      }
      REXLOG_INFO(
          "rexgpu-native: SKIPS issued={} skipped={} [{}] tex_binds={} tex_miss={} tex_null={} "
          "zclear={} rtfmt=[{}]",
          draw_count_, skipped_draw_total_, hist, texture_bind_total_, texture_miss_total_,
          texture_null_total_, guest_depth_clear_, rtf);
    }
  }

  const bool presented = presenter->RefreshGuestOutput(
      width, height, width, height,
      [this, width, height, clear_rgba, &display_ranges, present_index](
          ui::Presenter::GuestOutputRefreshContext& context) mutable -> bool {
        auto& vk_ctx =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const VkImage image = vk_ctx.image();
        const VkImageView image_view = vk_ctx.image_view();
        const bool ever_written = vk_ctx.image_ever_written_previously();
        context.SetIs8bpc(deferred_draws_.empty());

        (void)image_view;
        if (!EnsureSceneFramebuffer(width, height)) {
          REXLOG_ERROR("rexgpu-native: PRESENTFAIL EnsureSceneFramebuffer {}x{}", width, height);
          return false;
        }

        const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
        const VkDevice device = vulkan_device_->device();

        dfn.vkResetCommandPool(device, command_pool_, 0);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dfn.vkBeginCommandBuffer(command_buffer_, &begin_info) != VK_SUCCESS) {
          REXLOG_ERROR("rexgpu-native: PRESENTFAIL vkBeginCommandBuffer");
          return false;
        }

        VkImageSubresourceRange subresource_range = {};
        subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource_range.levelCount = 1;
        subresource_range.layerCount = 1;

        // Before the guest draws execute, make all host writes to shared memory /
        // uniform / index buffers (memcpy'd during IssueDraw) visible to the GPU.
        VkMemoryBarrier host_barrier = {};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                     VK_ACCESS_UNIFORM_READ_BIT;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &host_barrier, 0, nullptr, 0, nullptr);

        // Render each captured phase into the offscreen resolve RT and copy its
        // resolve rect into the phase's per-frame resolved image, so the display
        // draws below (and later offscreen phases) sample this frame's content
        // through the address aliases registered at IssueCopy time.
        // DIAG: report EVERY phase and why it was or wasn't rendered. The
        // per-phase "recorded" log below only fires for phases that render, so
        // a phase dropped by this guard was invisible - which is exactly the
        // state Choplifter and OutRun are in (offscreen phase empty on screen,
        // no PHASE line in the log). Gated on one swap so it costs nothing.
        {
          // Same occurrence-count gating as SWAPENTRY above, and for the same
          // reason - see the comment there.
          static uint64_t phases_n = 0;
          ++phases_n;
          if (REXCVAR_GET(native_log_draws) && (phases_n <= 4 || (phases_n % 400) == 0)) {
            REXLOG_INFO("rexgpu-native: PHASES swap={} count={} deferred={} phase_first={}",
                        swap_count_, phases_.size(), deferred_draws_.size(), phase_first_draw_);
            for (size_t pi = 0; pi < phases_.size(); ++pi) {
              const RenderPhase& p = phases_[pi];
              const char* why = p.resolved_index == SIZE_MAX ? "no_image"
                                : (p.end_draw <= p.first_draw ? "EMPTY_RANGE" : "renders");
              REXLOG_INFO(
                  "rexgpu-native: PHASES [{}] dest=0x{:08X} base={} range=[{},{}) rect={}x{} -> {}",
                  pi, p.dest_key, p.src_base, p.first_draw, p.end_draw, p.rect_w, p.rect_h, why);
            }
          }
        }
        for (const RenderPhase& p : phases_) {
          if (p.resolved_index == SIZE_MAX || p.end_draw <= p.first_draw) {
            continue;
          }
          ResolvedTarget& resolved = resolved_target_storage_[p.resolved_index];

          VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkImageMemoryBarrier rt_to_color = {};
          rt_to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          rt_to_color.srcAccessMask = 0;
          rt_to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          rt_to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          rt_to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          rt_to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          rt_to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          rt_to_color.image = resolve_rt_color_;
          rt_to_color.subresourceRange = color_range;
          dfn.vkCmdPipelineBarrier(
              command_buffer_,
              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
              &rt_to_color);

          VkClearValue rt_clears[2] = {};
          rt_clears[1].depthStencil.depth = guest_depth_clear_;
          VkRenderPassBeginInfo rt_rp_begin = {};
          rt_rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
          rt_rp_begin.renderPass = clear_render_pass_;
          rt_rp_begin.framebuffer = resolve_rt_framebuffer_;
          rt_rp_begin.renderArea.extent = {resolve_rt_width_, resolve_rt_height_};
          rt_rp_begin.clearValueCount = 2;
          rt_rp_begin.pClearValues = rt_clears;
          dfn.vkCmdBeginRenderPass(command_buffer_, &rt_rp_begin, VK_SUBPASS_CONTENTS_INLINE);
          const uint32_t recorded = RecordDeferredDrawsForBase(
              command_buffer_, p.first_draw, p.end_draw, p.src_base, resolve_rt_width_,
              resolve_rt_height_);
          // TEMP-DIAG: what each resolved image is actually built from. Gated on
          // an OCCURRENCE COUNT, and on its own cheap cvar rather than
          // native_log_draws - an early-only bound plus the per-draw flood meant
          // these lines were always rotated out of the log before they could be
          // read (cost a session on Choplifter).
          static uint64_t phase_log = 0;
          ++phase_log;
          if ((REXCVAR_GET(native_log_phases) || REXCVAR_GET(native_log_draws)) &&
              (phase_log <= 8 || (phase_log % 200) == 0)) {
            // How many draws the base filter rejected, and which bases they
            // carried - the difference between "filtered out" and "rendered
            // nothing" for a black world.
            uint32_t range_size = 0, dropped = 0;
            uint32_t other_base = UINT32_MAX;
            for (uint32_t i = p.first_draw;
                 i < p.end_draw && i < uint32_t(deferred_draws_.size()); ++i) {
              ++range_size;
              const DeferredDraw& d = deferred_draws_[i];
              if (!d.depth_only && d.color_edram_base != p.src_base) {
                ++dropped;
                other_base = d.color_edram_base;
              }
            }
            // Depth state of the phase's draws. The attachment is cleared to a
            // fixed 1.0, so a guest using reverse-Z (GREATER, zfunc 4/5/6)
            // would fail EVERY 3D depth test while 2D UI (z_enable 0) still
            // draws - exactly the "world black, HUD correct" signature.
            uint32_t zfunc_hist[8] = {};
            uint32_t z_disabled = 0;
            for (uint32_t i = p.first_draw;
                 i < p.end_draw && i < uint32_t(deferred_draws_.size()); ++i) {
              const reg::RB_DEPTHCONTROL dc{deferred_draws_[i].depth_control_raw};
              if (!dc.z_enable) {
                ++z_disabled;
              } else {
                ++zfunc_hist[uint32_t(dc.zfunc) & 7];
              }
            }
            REXLOG_INFO(
                "rexgpu-native: PHASE dest=0x{:08X} base={} range=[{},{}) size={} recorded={} "
                "dropped={} other_base={} rect={}x{} zoff={} zfunc=[{},{},{},{},{},{},{},{}]",
                p.dest_key, p.src_base, p.first_draw, p.end_draw, range_size, recorded, dropped,
                other_base, p.rect_w, p.rect_h, z_disabled, zfunc_hist[0], zfunc_hist[1],
                zfunc_hist[2], zfunc_hist[3], zfunc_hist[4], zfunc_hist[5], zfunc_hist[6],
                zfunc_hist[7]);
          }
          dfn.vkCmdEndRenderPass(command_buffer_);

          VkImageMemoryBarrier to_src = rt_to_color;
          to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          to_src.image = resolve_rt_color_;
          VkImageMemoryBarrier to_dst = to_src;
          // The copy writes one rect of a layout-sized image; the rest of the
          // image (other tiling strips, this frame or earlier) must survive,
          // so transition from the image's REAL current layout - UNDEFINED
          // here would let the driver discard it.
          to_dst.srcAccessMask =
              resolved.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                  ? VkAccessFlags(VK_ACCESS_SHADER_READ_BIT)
                  : VkAccessFlags(0);
          to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          to_dst.oldLayout = resolved.layout;
          to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          to_dst.image = resolved.image;
          const bool first_image_write = resolved.layout == VK_IMAGE_LAYOUT_UNDEFINED;
          VkImageMemoryBarrier pre_blit[2] = {to_src, to_dst};
          dfn.vkCmdPipelineBarrier(command_buffer_,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_TRANSFER_BIT |
                                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                                   pre_blit);
          if (first_image_write) {
            // A fresh layout-sized image can be larger than any rect written
            // this frame; clear it so never-resolved regions sample as black
            // rather than uninitialized memory (visible as speckling).
            VkClearColorValue zero = {};
            dfn.vkCmdClearColorImage(command_buffer_, resolved.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &color_range);
            VkImageMemoryBarrier clear_to_copy = to_dst;
            clear_to_copy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            clear_to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            clear_to_copy.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            clear_to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &clear_to_copy);
          }

          // The staging buffer is reused serially across phases; fence off the
          // previous phase's buffer->image read before overwriting it.
          VkBufferMemoryBarrier staging_reuse = {};
          staging_reuse.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
          staging_reuse.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
          staging_reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          staging_reuse.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          staging_reuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          staging_reuse.buffer = resolve_staging_buffer_;
          staging_reuse.offset = 0;
          staging_reuse.size = VK_WHOLE_SIZE;
          dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &staging_reuse,
                                   0, nullptr);

          // The phase's draws render at the resolve RT's origin (per-draw
          // viewports are guest-origin), so the rect is read from (0,0) there
          // and written at its destination-texture position in the resolved
          // image.
          VkBufferImageCopy src_region = {};
          src_region.bufferOffset = 0;
          src_region.bufferRowLength = p.rect_w;
          src_region.bufferImageHeight = p.rect_h;
          src_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          src_region.imageOffset = {0, 0, 0};
          src_region.imageExtent = {p.rect_w, p.rect_h, 1};
          dfn.vkCmdCopyImageToBuffer(command_buffer_, resolve_rt_color_,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolve_staging_buffer_,
                                     1, &src_region);
          VkBufferMemoryBarrier buf_barrier = staging_reuse;
          buf_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          buf_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &buf_barrier,
                                   0, nullptr);
          VkBufferImageCopy dst_region = src_region;
          dst_region.imageOffset = {int32_t(p.dest_x), int32_t(p.dest_y), 0};
          dfn.vkCmdCopyBufferToImage(command_buffer_, resolve_staging_buffer_, resolved.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dst_region);

          VkImageMemoryBarrier to_read = to_dst;
          to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          to_read.image = resolved.image;
          dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &to_read);
          resolved.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // The swap image is now a BLIT DESTINATION, not the render target - the
        // guest draws go into the float scene image and are tone-scaled down by
        // the blit below.
        VkImageMemoryBarrier acquire_barrier = {};
        acquire_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barrier.srcAccessMask =
            ever_written ? VkAccessFlags(VK_ACCESS_SHADER_READ_BIT) : VkAccessFlags(0);
        acquire_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire_barrier.oldLayout = ever_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        acquire_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.image = image;
        acquire_barrier.subresourceRange = subresource_range;
        // The scene image is written fresh every frame, so its previous
        // contents are discardable (UNDEFINED).
        VkImageMemoryBarrier scene_to_color = acquire_barrier;
        scene_to_color.srcAccessMask = 0;
        scene_to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        scene_to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        scene_to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        scene_to_color.image = scene_color_;
        const VkImageMemoryBarrier pre_scene[2] = {acquire_barrier, scene_to_color};
        dfn.vkCmdPipelineBarrier(
            command_buffer_,
            ever_written ? VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                         : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
            nullptr, 0, nullptr, 2, pre_scene);

        VkClearValue clear_values[2] = {};
        clear_values[0].color.float32[0] = clear_rgba[0];
        clear_values[0].color.float32[1] = clear_rgba[1];
        clear_values[0].color.float32[2] = clear_rgba[2];
        clear_values[0].color.float32[3] = clear_rgba[3];
        // Depth cleared to the GUEST's clear value each frame - the native
        // backend keeps no persistent depth across frames (no EDRAM). A fixed
        // 1.0 here silently breaks reverse-Z titles (see guest_depth_clear_).
        clear_values[1].depthStencil.depth = guest_depth_clear_;
        clear_values[1].depthStencil.stencil = 0;

        // The guest already resolved this frame to the frontbuffer address, so
        // PRESENT that image rather than trying to re-render the draws that
        // made it (see ChooseDisplaySource). The blit scales the resolved
        // image - which is sized to the guest's texture layout - onto the
        // scene image.
        if (present_index != SIZE_MAX &&
            resolved_target_storage_[present_index].layout !=
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
          // Republication aliases two destination keys onto one image, so a
          // key's tracked layout can lag what the other key did to it. Only
          // blit from an image we know is in the layout we are about to claim -
          // declaring the wrong oldLayout is VUID-VkImageMemoryBarrier-01213
          // and makes the whole submit invalid.
          present_index = SIZE_MAX;
        }
        if (present_index != SIZE_MAX) {
          ResolvedTarget& src = resolved_target_storage_[present_index];
          VkImageMemoryBarrier to_transfer[2] = {};
          for (uint32_t i = 0; i < 2; ++i) {
            to_transfer[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_transfer[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          }
          to_transfer[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
          to_transfer[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          to_transfer[0].oldLayout = src.layout;
          to_transfer[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          to_transfer[0].image = src.image;
          to_transfer[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          to_transfer[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          to_transfer[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          to_transfer[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          to_transfer[1].image = scene_color_;
          dfn.vkCmdPipelineBarrier(command_buffer_,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                                   to_transfer);

          VkImageBlit present_blit = {};
          present_blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          present_blit.srcOffsets[0] = {0, 0, 0};
          present_blit.srcOffsets[1] = {int32_t(src.width), int32_t(src.height), 1};
          present_blit.dstSubresource = present_blit.srcSubresource;
          present_blit.dstOffsets[0] = {0, 0, 0};
          present_blit.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
          dfn.vkCmdBlitImage(command_buffer_, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             scene_color_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &present_blit,
                             VK_FILTER_LINEAR);

          VkImageMemoryBarrier back[2] = {to_transfer[0], to_transfer[1]};
          back[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          back[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          back[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          back[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          back[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          back[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          back[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          back[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   0, 0, nullptr, 0, nullptr, 2, back);
          src.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        // Presenting keeps the blitted image (LOAD); replaying starts from the
        // clear colour.
        rp_begin.renderPass =
            present_index != SIZE_MAX ? load_render_pass_ : clear_render_pass_;
        rp_begin.framebuffer = scene_framebuffer_;
        rp_begin.renderArea.extent = {width, height};
        rp_begin.clearValueCount = 2;
        rp_begin.pClearValues = clear_values;
        dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        // Replay the frame's display draws (frontbuffer phases + trailing, or
        // everything when no phase matched the frontbuffer) into the guest
        // output image.
        for (const auto& range : display_ranges) {
          if (range.src_base == kAnyBase) {
            RecordDeferredDrawRange(command_buffer_, range.first_draw,
                                    range.end_draw - range.first_draw, width, height);
          } else {
            RecordDeferredDrawsForBase(command_buffer_, range.first_draw, range.end_draw,
                                       range.src_base, width,
                                       height);
          }
        }

        // Renderer identifier: a green tab in the top-left corner marks frames
        // produced by THIS (native) backend. The xenos backend draws nothing
        // here, so "green tab = new engine, no tab = old engine" is readable at
        // a glance in a screenshot or on a live window.
        if (REXCVAR_GET(native_marker)) {
          VkClearAttachment marker = {};
          marker.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          marker.colorAttachment = 0;
          marker.clearValue.color.float32[0] = 0.0f;
          marker.clearValue.color.float32[1] = 1.0f;
          marker.clearValue.color.float32[2] = 0.2f;
          marker.clearValue.color.float32[3] = 1.0f;
          const uint32_t mw = std::min(uint32_t(48), width);
          const uint32_t mh = std::min(uint32_t(16), height);
          VkClearRect marker_rect = {};
          marker_rect.rect.offset = {0, 0};
          marker_rect.rect.extent = {mw, mh};
          marker_rect.baseArrayLayer = 0;
          marker_rect.layerCount = 1;
          dfn.vkCmdClearAttachments(command_buffer_, 1, &marker, 1, &marker_rect);
        }

        dfn.vkCmdEndRenderPass(command_buffer_);

        // Scene (float, HDR) -> swap image (presenter's format). The blit does
        // the format conversion and clamps to the destination range, which is
        // correct AFTER the guest's own tone-map pass has run in the scene
        // image - the point of rendering float in the first place.
        VkImageMemoryBarrier scene_to_src = scene_to_color;
        scene_to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        scene_to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        scene_to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        scene_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &scene_to_src);

        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = blit.srcOffsets[0];
        blit.dstOffsets[1] = blit.srcOffsets[1];
        dfn.vkCmdBlitImage(command_buffer_, scene_color_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

        VkImageMemoryBarrier release_barrier = acquire_barrier;
        release_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        release_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        release_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &release_barrier);

        if (dfn.vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
          REXLOG_ERROR("rexgpu-native: PRESENTFAIL vkEndCommandBuffer");
          return false;
        }

        dfn.vkResetFences(device, 1, &clear_fence_);
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        {
          const ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
              vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
          if (dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, clear_fence_) !=
              VK_SUCCESS) {
            REXLOG_ERROR("rexgpu-native: vkQueueSubmit failed for guest draw present");
            return false;
          }
        }
        dfn.vkWaitForFences(device, 1, &clear_fence_, VK_TRUE, UINT64_MAX);
        return true;
      });

  // Debug readback of every offscreen phase, before the frame's resources go.
  DumpResolvedTargets();

  // The frame's transient resources are now safe to recycle on the next draw.
  deferred_draws_.clear();
  frame_open_ = false;

  if (!presented) {
    static bool present_fail_logged = false;
    if (!present_fail_logged) {
      present_fail_logged = true;
      REXLOG_WARN("rexgpu-native: RefreshGuestOutput returned false ({}x{})", width, height);
    }
    return;
  }

  // [TEMP DIAG] Dump the presented frame as a PPM (same env knobs as the Vulkan
  // backend): REX_DUMP_FRAME=<prefix>.
  {
    static const char* dump_prefix = getenv("REX_DUMP_FRAME");
    if (dump_prefix) {
      static const uint32_t dump_every =
          getenv("REX_DUMP_FRAME_EVERY") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_EVERY"))) : 150u;
      static const uint32_t dump_start =
          getenv("REX_DUMP_FRAME_START") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_START"))) : 0u;
      static const uint32_t dump_max =
          getenv("REX_DUMP_FRAME_MAX") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_MAX"))) : 12u;
      // Optional: only dump frames that recorded at least this many guest draws
      // (so a busy geometry frame is captured instead of an idle clear frame).
      static const uint32_t dump_min_draws =
          getenv("REX_DUMP_FRAME_MINDRAWS") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_MINDRAWS"))) : 0u;
      static uint32_t dumped = 0;
      static uint32_t qualifying = 0;
      const uint32_t frame_counter = uint32_t(swap_count_);
      bool want_dump;
      if (dump_min_draws) {
        // Dump every dump_every-th frame whose draw count meets the threshold.
        want_dump = draw_replay_count >= dump_min_draws &&
                    (dump_every ? (qualifying % dump_every) == 0 : true);
        if (draw_replay_count >= dump_min_draws) {
          ++qualifying;
        }
      } else {
        want_dump = dump_every && frame_counter >= dump_start && (frame_counter % dump_every) == 0;
      }
      if (dumped < dump_max && want_dump) {
        ui::RawImage raw_image;
        if (presenter->CaptureGuestOutput(raw_image)) {
          char path[512];
          snprintf(path, sizeof(path), "%s_%04u.ppm", dump_prefix, frame_counter);
          FILE* f = fopen(path, "wb");
          if (f) {
            fprintf(f, "P6\n%u %u\n255\n", raw_image.width, raw_image.height);
            const uint8_t* src = raw_image.data.data();
            for (uint32_t y = 0; y < raw_image.height; ++y) {
              const uint8_t* row = src + size_t(y) * raw_image.stride;
              for (uint32_t x = 0; x < raw_image.width; ++x) {
                fwrite(row + size_t(x) * 4, 1, 3, f);
              }
            }
            fclose(f);
            ++dumped;
            REXLOG_WARN("rexgpu-native: [DUMP] swap {} -> {}", frame_counter, path);
          }
        }
      }
    }
  }
#else
  (void)frontbuffer_ptr;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
#endif  // REX_HAS_VULKAN
}

void NativeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  if (texture_cache_ && index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
  }
}

void NativeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  if (!texture_cache_ || !num_registers) {
    return;
  }
  const uint32_t end_index = start_index + num_registers - 1;
  if (end_index < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 ||
      start_index > XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    return;
  }
  // Clamp to the fetch-constant window - a batch may straddle its edges.
  const uint32_t first = std::max(start_index, uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0));
  const uint32_t last = std::min(end_index, uint32_t(XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5));
  texture_cache_->TextureFetchConstantsWritten(
      (first - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6,
      (last - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
}

}  // namespace rex::graphics::native
