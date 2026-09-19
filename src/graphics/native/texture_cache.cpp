/**
 * @file        graphics/native/texture_cache.cpp
 * @brief       Native GPU renderer - real guest texture cache (Phase 3).
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     Self-contained port of the untiling + format-decode path from
 *              rex::graphics::vulkan::VulkanTextureCache, with no dependency on
 *              VulkanCommandProcessor. See texture_cache.h for the design notes.
 */

#include "native/texture_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/util.h>

#include "native/native_shared_memory.h"

namespace rex::graphics::native {

// Generated with `xb buildshaders`. Only the unscaled load shaders are used by
// the native backend (no resolution scaling).
namespace shaders {
#include "../shaders/vulkan_spirv/texture_load_128bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_16bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_32bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_64bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_8bpb_cs.h"
#include "../shaders/vulkan_spirv/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_ctx1_cs.h"
#include "../shaders/vulkan_spirv/texture_load_depth_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxn_rg8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3a_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt3aas1111_argb4_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_dxt5a_r8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r16_unorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/vulkan_spirv/texture_load_r5g6b5_b5g6r5_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rg16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rg16_unorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rgba16_snorm_float_cs.h"
#include "../shaders/vulkan_spirv/texture_load_rgba16_unorm_float_cs.h"
}  // namespace shaders

namespace {

constexpr VkFormat kInvalidTextureFetchFallbackFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkComponentSwizzle GetComponentSwizzle(uint32_t texture_swizzle, uint32_t component_index) {
  xenos::XE_GPU_TEXTURE_SWIZZLE texture_component_swizzle =
      xenos::XE_GPU_TEXTURE_SWIZZLE((texture_swizzle >> (3 * component_index)) & 0b111);
  if (texture_component_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE(component_index)) {
    return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
  switch (texture_component_swizzle) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
      return VK_COMPONENT_SWIZZLE_R;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
      return VK_COMPONENT_SWIZZLE_G;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
      return VK_COMPONENT_SWIZZLE_B;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
      return VK_COMPONENT_SWIZZLE_A;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
      return VK_COMPONENT_SWIZZLE_ZERO;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
      return VK_COMPONENT_SWIZZLE_ONE;
    default:
      return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
}

VkImageSubresourceRange WholeColorRange() {
  VkImageSubresourceRange range = {};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.baseMipLevel = 0;
  range.levelCount = VK_REMAINING_MIP_LEVELS;
  range.baseArrayLayer = 0;
  range.layerCount = VK_REMAINING_ARRAY_LAYERS;
  return range;
}

}  // namespace

// ---------------------------------------------------------------------------
// Host format tables (ported verbatim from VulkanTextureCache).
// ---------------------------------------------------------------------------

static_assert(VK_FORMAT_UNDEFINED == VkFormat(0),
              "Assuming that skipping a VkFormat in an initializer results in VK_FORMAT_UNDEFINED");

const NativeTextureCache::HostFormatPair NativeTextureCache::kBestHostFormats[64] = {
    // k_1_REVERSE
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_1_5_5_5
    {{kLoadShaderIndexR5G5B5A1ToB5G5R5A1, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    {{kLoadShaderIndexR5G6B5ToB5G6R5, VK_FORMAT_R5G6B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    {{kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, VK_FORMAT_R5G6B5_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_2_10_10_10
    {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {{kLoadShaderIndex8bpb, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndex8bpb, VK_FORMAT_R8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_8_B
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {{kLoadShaderIndex16bpb, VK_FORMAT_R8G8_UNORM},
     {kLoadShaderIndex16bpb, VK_FORMAT_R8G8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_Cr_Y1_Cb_Y0_REP
    {{kLoadShaderIndex32bpb, VK_FORMAT_G8B8G8R8_422_UNORM, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    {{kLoadShaderIndex32bpb, VK_FORMAT_B8G8R8G8_422_UNORM, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    {{kLoadShaderIndexRGBA4ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_24_8
    {{kLoadShaderIndexDepthUnorm, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndexDepthUnorm, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_24_8_FLOAT
    {{kLoadShaderIndexDepthFloat, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndexDepthFloat, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_UNORM},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_EXPAND
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_EXPAND
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_EXPAND
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_FLOAT
    {{kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     {kLoadShaderIndex16bpb, VK_FORMAT_R16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_FLOAT
    {{kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R16G16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_FLOAT
    {{kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R16G16B16A16_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {{kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
     {kLoadShaderIndex32bpb, VK_FORMAT_R32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_32_32_FLOAT
    {{kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
     {kLoadShaderIndex64bpb, VK_FORMAT_R32G32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_32_32_32_32_FLOAT
    {{kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
     {kLoadShaderIndex128bpb, VK_FORMAT_R32G32B32A32_SFLOAT},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32_AS_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC5_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_UNORM},
     {kLoadShaderIndex32bpb, VK_FORMAT_R8G8B8A8_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_DXT1_AS_16_16_16_16
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC2_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {{kLoadShaderIndex128bpb, VK_FORMAT_BC3_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {{kLoadShaderIndexR11G11B10ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {{kLoadShaderIndexR10G11B11ToRGBA16, VK_FORMAT_R16G16B16A16_UNORM},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, VK_FORMAT_R16G16B16A16_SNORM},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    {{kLoadShaderIndexDXT3A, VK_FORMAT_R8_UNORM},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {{kLoadShaderIndex64bpb, VK_FORMAT_BC4_UNORM_BLOCK, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {{kLoadShaderIndexCTX1, VK_FORMAT_R8G8_UNORM},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {{kLoadShaderIndexDXT3AAs1111ToARGB4, VK_FORMAT_B4G4R4A4_UNORM_PACK16},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatGBGRUnaligned = {
    {kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatBGRGUnaligned = {
    {kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT1Unaligned = {
    {kLoadShaderIndexDXT1ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT2_3Unaligned = {
    {kLoadShaderIndexDXT3ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT4_5Unaligned = {
    {kLoadShaderIndexDXT5ToRGBA8, VK_FORMAT_R8G8B8A8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXNUnaligned = {
    {kLoadShaderIndexDXNToRG8, VK_FORMAT_R8G8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
    false};
const NativeTextureCache::HostFormatPair NativeTextureCache::kHostFormatDXT5AUnaligned = {
    {kLoadShaderIndexDXT5AToR8, VK_FORMAT_R8_UNORM, false, true},
    {kLoadShaderIndexUnknown},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
    false};

// ---------------------------------------------------------------------------
// NativeTexture
// ---------------------------------------------------------------------------

NativeTextureCache::NativeTexture::NativeTexture(NativeTextureCache& texture_cache,
                                                 const TextureKey& key, VkImage image,
                                                 VkDeviceMemory memory, VkFormat host_format,
                                                 uint64_t host_memory_usage)
    : Texture(texture_cache, key), image_(image), memory_(memory), host_format_(host_format) {
  SetHostMemoryUsage(host_memory_usage);
}

NativeTextureCache::NativeTexture::~NativeTexture() {
  const NativeTextureCache& cache = static_cast<const NativeTextureCache&>(texture_cache());
  const ui::vulkan::VulkanDevice* const vulkan_device = cache.vulkan_device_;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (const auto& view_pair : views_) {
    dfn.vkDestroyImageView(device, view_pair.second, nullptr);
  }
  if (image_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, image_, nullptr);
  }
  if (memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, memory_, nullptr);
  }
}

VkImageView NativeTextureCache::NativeTexture::GetView(uint32_t host_swizzle, bool is_array) {
  xenos::DataDimension dimension = key().dimension;
  if (dimension == xenos::DataDimension::k3D || dimension == xenos::DataDimension::kCube) {
    is_array = false;
  }
  if (host_format_ == VK_FORMAT_UNDEFINED) {
    return VK_NULL_HANDLE;
  }

  const NativeTextureCache& cache = static_cast<const NativeTextureCache&>(texture_cache());
  const ui::vulkan::VulkanDevice* const vulkan_device = cache.vulkan_device_;
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }

  uint32_t view_key = (host_swizzle & 0xFFFFu) | (uint32_t(is_array ? 1u : 0u) << 16);
  auto it = views_.find(view_key);
  if (it != views_.end()) {
    return it->second;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkImageViewCreateInfo view_create_info = {};
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.image = image_;
  view_create_info.format = host_format_;
  view_create_info.components.r = GetComponentSwizzle(host_swizzle, 0);
  view_create_info.components.g = GetComponentSwizzle(host_swizzle, 1);
  view_create_info.components.b = GetComponentSwizzle(host_swizzle, 2);
  view_create_info.components.a = GetComponentSwizzle(host_swizzle, 3);
  view_create_info.subresourceRange = WholeColorRange();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      view_create_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
      break;
    case xenos::DataDimension::kCube:
      view_create_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
      break;
    default:
      if (is_array) {
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      } else {
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_create_info.subresourceRange.layerCount = 1;
      }
      break;
  }
  VkImageView view = VK_NULL_HANDLE;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr, &view) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: texture cache failed to create image view (format {}, swizzle {:X})",
                 uint32_t(host_format_), host_swizzle);
    return VK_NULL_HANDLE;
  }
  views_.emplace(view_key, view);
  return view;
}

// ---------------------------------------------------------------------------
// NativeTextureCache
// ---------------------------------------------------------------------------

NativeTextureCache::NativeTextureCache(const ui::vulkan::VulkanDevice* vulkan_device,
                                       const RegisterFile& register_file,
                                       NativeSharedMemory& shared_memory)
    : TextureCache(register_file, shared_memory, 1, 1),
      vulkan_device_(vulkan_device),
      native_shared_memory_(shared_memory),
      guest_shader_pipeline_stages_(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) {}

NativeTextureCache::~NativeTextureCache() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (load_fence_ != VK_NULL_HANDLE) {
    dfn.vkWaitForFences(device, 1, &load_fence_, VK_TRUE, UINT64_MAX);
  }

  // Destroy the textures before the resources they depend on.
  DestroyAllTextures(true);

  for (auto& sampler_pair : samplers_) {
    dfn.vkDestroySampler(device, sampler_pair.second, nullptr);
  }
  samplers_.clear();

  for (VkPipeline pipeline : load_pipelines_) {
    if (pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline, nullptr);
    }
  }
  if (load_pipeline_layout_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineLayout(device, load_pipeline_layout_, nullptr);
  }
  if (load_source_dest_set_layout_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, load_source_dest_set_layout_, nullptr);
  }

  if (null_image_view_3d_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_3d_, nullptr);
  }
  if (null_image_view_cube_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_cube_, nullptr);
  }
  if (null_image_view_2d_array_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, null_image_view_2d_array_, nullptr);
  }
  if (null_image_3d_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, null_image_3d_, nullptr);
  }
  if (null_image_2d_array_cube_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, null_image_2d_array_cube_, nullptr);
  }
  for (VkDeviceMemory memory : null_images_memory_) {
    if (memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, memory, nullptr);
    }
  }

  if (scratch_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
  }
  if (scratch_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, scratch_memory_, nullptr);
  }
  if (load_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, load_descriptor_pool_, nullptr);
  }
  if (load_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, load_fence_, nullptr);
  }
  if (load_command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, load_command_pool_, nullptr);
  }
}

const NativeTextureCache::HostFormatPair& NativeTextureCache::GetHostFormatPair(
    TextureKey key) const {
  const HostFormatPair& host_format_pair = host_formats_[uint32_t(key.format)];
  if (!host_format_pair.format_unsigned.block_compressed &&
      !host_format_pair.format_signed.block_compressed) {
    return host_format_pair;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!(key.GetWidth() & (format_info->block_width - 1)) &&
      !(key.GetHeight() & (format_info->block_height - 1))) {
    return host_format_pair;
  }
  switch (key.format) {
    case xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP:
      return kHostFormatGBGRUnaligned;
    case xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      return kHostFormatBGRGUnaligned;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kHostFormatDXT1Unaligned;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kHostFormatDXT2_3Unaligned;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kHostFormatDXT4_5Unaligned;
    case xenos::TextureFormat::k_DXN:
      return kHostFormatDXNUnaligned;
    case xenos::TextureFormat::k_DXT5A:
      return kHostFormatDXT5AUnaligned;
    default:
      return host_format_pair;
  }
}

uint32_t NativeTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return GetHostFormatPair(key).swizzle;
}

uint32_t NativeTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& props = vulkan_device_->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return props.maxImageDimension2D;
    case xenos::DataDimension::k3D:
      return props.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      return props.maxImageDimensionCube;
    default:
      return 0;
  }
}

uint32_t NativeTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  const ui::vulkan::VulkanDevice::Properties& props = vulkan_device_->properties();
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      return props.maxImageArrayLayers;
    case xenos::DataDimension::k3D:
      return props.maxImageDimension3D;
    case xenos::DataDimension::kCube:
      return 6;
    default:
      return 0;
  }
}

xenos::ClampMode NativeTextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToEdge ||
      clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    return vulkan_device_->properties().samplerMirrorClampToEdge
               ? xenos::ClampMode::kMirrorClampToEdge
               : xenos::ClampMode::kMirroredRepeat;
  }
  return clamp_mode;
}

VkImageView NativeTextureCache::NullImageViewForDimension(xenos::FetchOpDimension dimension) const {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return null_image_view_3d_;
    case xenos::FetchOpDimension::kCube:
      return null_image_view_cube_;
    default:
      return null_image_view_2d_array_;
  }
}

VkImageView NativeTextureCache::GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                                               xenos::FetchOpDimension dimension,
                                                               bool is_signed, bool* out_hit) {
  (void)is_signed;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(dimension, binding->key.dimension)) {
    VkImageView view = native_texture_bindings_[fetch_constant_index].image_view;
    if (view != VK_NULL_HANDLE) {
      if (out_hit) {
        *out_hit = true;
      }
      return view;
    }
  }
  if (out_hit) {
    *out_hit = false;
  }
  return NullImageViewForDimension(dimension);
}

void NativeTextureCache::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    NativeTextureBinding& native_binding = native_texture_bindings_[binding_index];
    native_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding || !binding->texture) {
      continue;
    }
    NativeTexture* texture = static_cast<NativeTexture*>(binding->texture);
    if (!texture->loaded()) {
      // A texture that fails to load leaves this slot on the opaque-black null
      // image, which silently blacks out everything sampling it. Enumerate the
      // failures once each so the unsupported formats are visible in one run.
      static std::unordered_set<uint32_t> reported;
      const uint32_t sig = (uint32_t(binding->key.format) << 8) ^
                           (uint32_t(binding->key.dimension) << 4) ^
                           uint32_t(binding->key.packed_mips);
      if (reported.insert(sig).second) {
        REXLOG_WARN(
            "rexgpu-native: texture NOT LOADED fmt={} ({}) dim={} {}x{} mips={} packed={} slot={}",
            uint32_t(binding->key.format), FormatInfo::Get(binding->key.format)->name,
            uint32_t(binding->key.dimension), binding->key.GetWidth(), binding->key.GetHeight(),
            uint32_t(binding->key.mip_max_level), uint32_t(binding->key.packed_mips),
            binding_index);
      }
      continue;
    }
    texture->MarkAsUsed();
    bool is_array = binding->key.dimension != xenos::DataDimension::k3D;
    native_binding.image_view = texture->GetView(binding->host_swizzle, is_array);
  }
}

std::unique_ptr<TextureCache::Texture> NativeTextureCache::CreateTexture(TextureKey key) {
  const HostFormatPair& host_format = GetHostFormatPair(key);
  VkFormat format = host_format.format_unsigned.format;
  if (format == VK_FORMAT_UNDEFINED) {
    format = host_format.format_signed.format;
  }
  if (format == VK_FORMAT_UNDEFINED) {
    return nullptr;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();

  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  if (key.dimension == xenos::DataDimension::kCube) {
    image_create_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  image_create_info.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  image_create_info.format = format;
  image_create_info.extent.width = key.GetWidth();
  image_create_info.extent.height = key.GetHeight();
  image_create_info.extent.depth = is_3d ? depth_or_array_size : 1;
  image_create_info.mipLevels = key.mip_max_level + 1;
  image_create_info.arrayLayers = is_3d ? 1 : depth_or_array_size;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage image = VK_NULL_HANDLE;
  if (dfn.vkCreateImage(device, &image_create_info, nullptr, &image) != VK_SUCCESS) {
    return nullptr;
  }
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(device, image, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    dfn.vkDestroyImage(device, image, nullptr);
    return nullptr;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
    dfn.vkDestroyImage(device, image, nullptr);
    return nullptr;
  }
  if (dfn.vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
    dfn.vkFreeMemory(device, memory, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    return nullptr;
  }

  return std::unique_ptr<Texture>(
      new NativeTexture(*this, key, image, memory, format, uint64_t(req.size)));
}

VkBuffer NativeTextureCache::EnsureScratchBuffer(VkDeviceSize size) {
  if (scratch_buffer_ != VK_NULL_HANDLE && scratch_size_ >= size) {
    return scratch_buffer_;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // The previous scratch buffer is only referenced by already-completed (fenced)
  // load submissions, so it's safe to destroy here.
  if (scratch_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    scratch_buffer_ = VK_NULL_HANDLE;
  }
  if (scratch_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, scratch_memory_, nullptr);
    scratch_memory_ = VK_NULL_HANDLE;
  }

  VkDeviceSize new_size = std::max<VkDeviceSize>(size, VkDeviceSize(1) << 20);
  // Round up to 1 MB to reduce churn.
  new_size = (new_size + ((VkDeviceSize(1) << 20) - 1)) & ~((VkDeviceSize(1) << 20) - 1);

  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = new_size;
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &scratch_buffer_) != VK_SUCCESS) {
    scratch_buffer_ = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  VkMemoryRequirements req;
  dfn.vkGetBufferMemoryRequirements(device, scratch_buffer_, &req);
  uint32_t type_index;
  if (!rex::bit_scan_forward(req.memoryTypeBits & vulkan_device_->memory_types().device_local,
                             &type_index) &&
      !rex::bit_scan_forward(req.memoryTypeBits, &type_index)) {
    dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    scratch_buffer_ = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &scratch_memory_) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    scratch_buffer_ = VK_NULL_HANDLE;
    scratch_memory_ = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  if (dfn.vkBindBufferMemory(device, scratch_buffer_, scratch_memory_, 0) != VK_SUCCESS) {
    dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    dfn.vkFreeMemory(device, scratch_memory_, nullptr);
    scratch_buffer_ = VK_NULL_HANDLE;
    scratch_memory_ = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  scratch_size_ = new_size;
  return scratch_buffer_;
}

void NativeTextureCache::BeginNativeFrame() {
  // With per-load submit-and-wait, everything up to the previous submission is
  // complete, so previous-frame textures become evictable.
  CompletedSubmissionUpdated(submission_index_);
  ++submission_index_;
  BeginSubmission(submission_index_);
  TextureCache::BeginFrame();
}

bool NativeTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                               bool load_mips) {
  NativeTexture& native_texture = static_cast<NativeTexture&>(texture);
  TextureKey texture_key = native_texture.key();

  const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
  // MVP: unsigned host format only.
  const HostFormat& host_format = host_format_pair.format_unsigned.format != VK_FORMAT_UNDEFINED
                                      ? host_format_pair.format_unsigned
                                      : host_format_pair.format_signed;
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown) {
    const FormatInfo* fi = FormatInfo::Get(texture_key.format);
    REXLOG_WARN("rexgpu-native: texture dropped (no load shader): format={} ({}) {}x{}",
                fi ? fi->name : "?", uint32_t(texture_key.format), texture_key.GetWidth(),
                texture_key.GetHeight());
    return false;
  }
  // MVP: the two-pass float16 conversion fallback is not implemented; drop the
  // affected 10_11_11/11_11_10 textures instead of producing wrong data.
  if (host_format.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
      (load_shader == kLoadShaderIndexR10G11B11ToRGBA16 ||
       load_shader == kLoadShaderIndexR11G11B10ToRGBA16 ||
       load_shader == kLoadShaderIndexR10G11B11ToRGBA16SNorm ||
       load_shader == kLoadShaderIndexR11G11B10ToRGBA16SNorm)) {
    return false;
  }
  VkPipeline pipeline = load_pipelines_[load_shader];
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  const texture_util::TextureGuestLayout& guest_layout = native_texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || native_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  if (level_first > level_last) {
    return false;
  }
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);

  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }
  VkDeviceSize host_buffer_size = 0;
  struct HostLayout {
    VkDeviceSize offset_bytes;
    VkDeviceSize slice_size_bytes;
    uint32_t x_pitch_blocks;
    uint32_t y_pitch_blocks;
  };
  HostLayout host_layout_base = {};
  HostLayout host_layout_mips[xenos::kTextureMaxMips] = {};
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_guest_x_extent_texels;
    uint32_t level_guest_y_extent_texels;
    uint32_t level_guest_z_extent_texels;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_guest_x_extent_texels = guest_layout_packed.x_extent_blocks * block_width;
      level_guest_y_extent_texels = guest_layout_packed.y_extent_blocks * block_height;
      level_guest_z_extent_texels = guest_layout_packed.z_extent;
    } else {
      level_guest_x_extent_texels = std::max(width >> level, UINT32_C(1));
      level_guest_y_extent_texels = std::max(height >> level, UINT32_C(1));
      level_guest_z_extent_texels = std::max(depth >> level, UINT32_C(1));
    }
    level_host_layout.x_pitch_blocks = rex::round_up(
        (level_guest_x_extent_texels + (host_block_width - 1)) / host_block_width,
        host_x_blocks_per_thread);
    level_host_layout.y_pitch_blocks =
        (level_guest_y_extent_texels + (host_block_height - 1)) / host_block_height;
    level_host_layout.slice_size_bytes = VkDeviceSize(load_shader_info.bytes_per_host_block) *
                                         level_host_layout.x_pitch_blocks *
                                         level_host_layout.y_pitch_blocks *
                                         level_guest_z_extent_texels;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }

  VkBuffer scratch_buffer = EnsureScratchBuffer(host_buffer_size);
  if (scratch_buffer == VK_NULL_HANDLE) {
    return false;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  // Allocate the descriptor sets for the load compute shader (dest + source(s)).
  dfn.vkResetDescriptorPool(device, load_descriptor_pool_, 0);
  VkDescriptorSet descriptor_set_dest = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_source_base = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_source_mips = VK_NULL_HANDLE;
  auto allocate_set = [&](VkDescriptorSet& out) -> bool {
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = load_descriptor_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &load_source_dest_set_layout_;
    return dfn.vkAllocateDescriptorSets(device, &info, &out) == VK_SUCCESS;
  };
  if (!allocate_set(descriptor_set_dest)) {
    return false;
  }

  std::array<VkWriteDescriptorSet, 3> write_descriptor_sets = {};
  uint32_t write_descriptor_set_count = 0;
  VkDescriptorBufferInfo dest_buffer_info = {};
  dest_buffer_info.buffer = scratch_buffer;
  dest_buffer_info.offset = 0;
  dest_buffer_info.range = host_buffer_size;
  {
    VkWriteDescriptorSet& w = write_descriptor_sets[write_descriptor_set_count++];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descriptor_set_dest;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &dest_buffer_info;
  }

  uint32_t source_length_alignment = UINT32_C(1) << load_shader_info.source_bpe_log2;
  VkDescriptorBufferInfo source_base_buffer_info = {};
  VkDescriptorBufferInfo source_mips_buffer_info = {};
  if (level_first == 0) {
    if (!allocate_set(descriptor_set_source_base)) {
      return false;
    }
    uint64_t source_base_start = uint64_t(texture_key.base_page) << 12;
    uint64_t source_base_range =
        rex::align(uint64_t(native_texture.GetGuestBaseSize()), uint64_t(source_length_alignment));
    if (source_base_range > uint64_t(SharedMemory::kBufferSize) - source_base_start) {
      return false;
    }
    source_base_buffer_info.buffer = native_shared_memory_.buffer();
    source_base_buffer_info.offset = source_base_start;
    source_base_buffer_info.range = source_base_range;
    VkWriteDescriptorSet& w = write_descriptor_sets[write_descriptor_set_count++];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descriptor_set_source_base;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &source_base_buffer_info;
  }
  if (level_last != 0) {
    if (!allocate_set(descriptor_set_source_mips)) {
      return false;
    }
    uint64_t source_mips_start = uint64_t(texture_key.mip_page) << 12;
    uint64_t source_mips_range =
        rex::align(uint64_t(native_texture.GetGuestMipsSize()), uint64_t(source_length_alignment));
    if (source_mips_range > uint64_t(SharedMemory::kBufferSize) - source_mips_start) {
      return false;
    }
    source_mips_buffer_info.buffer = native_shared_memory_.buffer();
    source_mips_buffer_info.offset = source_mips_start;
    source_mips_buffer_info.range = source_mips_range;
    VkWriteDescriptorSet& w = write_descriptor_sets[write_descriptor_set_count++];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descriptor_set_source_mips;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &source_mips_buffer_info;
  }
  dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets.data(), 0,
                             nullptr);

  // Record the whole load as one immediate command buffer.
  dfn.vkResetCommandPool(device, load_command_pool_, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(load_command_buffer_, &begin_info) != VK_SUCCESS) {
    return false;
  }

  // Make host writes to the shared memory (memcpy'd guest texture bytes) visible
  // to the compute load shader.
  VkMemoryBarrier host_barrier = {};
  host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dfn.vkCmdPipelineBarrier(load_command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &host_barrier, 0, nullptr, 0,
                           nullptr);

  dfn.vkCmdBindPipeline(load_command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  dfn.vkCmdBindDescriptorSets(load_command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                              load_pipeline_layout_, kLoadDescriptorSetIndexDestination, 1,
                              &descriptor_set_dest, 0, nullptr);

  LoadConstants load_constants = {};
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (UINT32_C(1) << 4) | (UINT32_C(1) << 7);

  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  VkDescriptorSet descriptor_set_source_current = VK_NULL_HANDLE;
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;

    VkDescriptorSet descriptor_set_source =
        is_base ? descriptor_set_source_base : descriptor_set_source_mips;
    if (descriptor_set_source_current != descriptor_set_source) {
      descriptor_set_source_current = descriptor_set_source;
      dfn.vkCmdBindDescriptorSets(load_command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  load_pipeline_layout_, kLoadDescriptorSetIndexSource, 1,
                                  &descriptor_set_source, 0, nullptr);
    }

    load_constants.guest_offset = 0;
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level];
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes / bytes_per_block;
    load_constants.guest_pitch_aligned = level_guest_pitch;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, UINT32_C(1));
      level_height = std::max(height >> level, UINT32_C(1));
      level_depth = std::max(depth >> level, UINT32_C(1));
    }
    load_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    load_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    load_constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    load_constants.host_pitch =
        load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;

    dfn.vkCmdPushConstants(load_command_buffer_, load_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(load_constants), &load_constants);

    uint32_t level_array_slice_stride_bytes = level_guest_layout.array_slice_stride_bytes;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (slice != 0) {
        dfn.vkCmdPushConstants(load_command_buffer_, load_pipeline_layout_,
                               VK_SHADER_STAGE_COMPUTE_BIT, offsetof(LoadConstants, guest_offset),
                               sizeof(load_constants.guest_offset), &load_constants.guest_offset);
        dfn.vkCmdPushConstants(load_command_buffer_, load_pipeline_layout_,
                               VK_SHADER_STAGE_COMPUTE_BIT, offsetof(LoadConstants, host_offset),
                               sizeof(load_constants.host_offset), &load_constants.host_offset);
      }
      dfn.vkCmdDispatch(load_command_buffer_, group_count_x, group_count_y,
                        load_constants.size_blocks[2]);
      load_constants.guest_offset += level_array_slice_stride_bytes;
      load_constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  // Scratch buffer (compute write) -> transfer read.
  VkBufferMemoryBarrier scratch_barrier = {};
  scratch_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  scratch_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  scratch_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  scratch_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  scratch_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  scratch_barrier.buffer = scratch_buffer;
  scratch_barrier.offset = 0;
  scratch_barrier.size = VK_WHOLE_SIZE;
  dfn.vkCmdPipelineBarrier(load_command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &scratch_barrier, 0,
                           nullptr);

  // Transition the texture image to TRANSFER_DST.
  VkImageMemoryBarrier to_dst = {};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.srcAccessMask = native_texture.loaded() ? VK_ACCESS_SHADER_READ_BIT : 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_dst.oldLayout = native_texture.loaded() ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = native_texture.image();
  to_dst.subresourceRange = WholeColorRange();
  dfn.vkCmdPipelineBarrier(load_command_buffer_,
                           native_texture.loaded()
                               ? VkPipelineStageFlags(guest_shader_pipeline_stages_)
                               : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

  // Copy the untiled data from the scratch buffer into the image.
  std::vector<VkBufferImageCopy> copy_regions(level_last - level_first + 1);
  for (uint32_t level = level_first; level <= level_last; ++level) {
    VkBufferImageCopy& copy_region = copy_regions[level - level_first];
    copy_region = {};
    const HostLayout& level_host_layout =
        level != 0 ? host_layout_mips[std::min(level, level_packed)] : host_layout_base;
    copy_region.bufferOffset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y, level_offset_z);
      uint32_t level_offset_host_blocks_x = level_offset_blocks_x;
      uint32_t level_offset_host_blocks_y = level_offset_blocks_y;
      if (!host_format.block_compressed) {
        level_offset_host_blocks_x *= block_width;
        level_offset_host_blocks_y *= block_height;
      }
      copy_region.bufferOffset +=
          load_shader_info.bytes_per_host_block *
          (level_offset_host_blocks_x +
           level_host_layout.x_pitch_blocks *
               (level_offset_host_blocks_y +
                level_host_layout.y_pitch_blocks * VkDeviceSize(level_offset_z)));
    }
    copy_region.bufferRowLength = level_host_layout.x_pitch_blocks * host_block_width;
    copy_region.bufferImageHeight = level_host_layout.y_pitch_blocks * host_block_height;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = level;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = array_size;
    copy_region.imageOffset.x = 0;
    copy_region.imageOffset.y = 0;
    copy_region.imageOffset.z = 0;
    copy_region.imageExtent.width = std::max(width >> level, UINT32_C(1));
    copy_region.imageExtent.height = std::max(height >> level, UINT32_C(1));
    copy_region.imageExtent.depth = std::max(depth >> level, UINT32_C(1));
  }
  dfn.vkCmdCopyBufferToImage(load_command_buffer_, scratch_buffer, native_texture.image(),
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copy_regions.size()),
                             copy_regions.data());

  // Transition the image to SHADER_READ_ONLY_OPTIMAL for sampling.
  VkImageMemoryBarrier to_read = {};
  to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_read.image = native_texture.image();
  to_read.subresourceRange = WholeColorRange();
  dfn.vkCmdPipelineBarrier(load_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           guest_shader_pipeline_stages_, 0, 0, nullptr, 0, nullptr, 1, &to_read);

  if (dfn.vkEndCommandBuffer(load_command_buffer_) != VK_SUCCESS) {
    return false;
  }

  dfn.vkResetFences(device, 1, &load_fence_);
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &load_command_buffer_;
  {
    const ui::vulkan::VulkanDevice::Queue::Acquisition acq =
        vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(acq.queue(), 1, &submit, load_fence_) != VK_SUCCESS) {
      return false;
    }
  }
  dfn.vkWaitForFences(device, 1, &load_fence_, VK_TRUE, UINT64_MAX);

  native_texture.MarkAsUsed();
  native_texture.set_loaded(true);
  return true;
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

NativeTextureCache::SamplerParameters NativeTextureCache::GetSamplerParameters(
    const SpirvShader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  aniso_filter = std::min(aniso_filter, max_anisotropy_);
  if (parameters.mag_linear || parameters.min_linear || parameters.mip_linear ||
      aniso_filter != xenos::AnisoFilter::kDisabled) {
    bool linear_filterable = true;
    TextureKey texture_key;
    uint8_t texture_swizzled_signs;
    BindingInfoFromFetchConstant(fetch, texture_key, &texture_swizzled_signs);
    if (texture_key.is_valid) {
      const HostFormatPair& host_format_pair = GetHostFormatPair(texture_key);
      if ((texture_util::IsAnySignNotSigned(texture_swizzled_signs) &&
           !host_format_pair.format_unsigned.linear_filterable) ||
          (texture_util::IsAnySignSigned(texture_swizzled_signs) &&
           !host_format_pair.format_signed.linear_filterable)) {
        linear_filterable = false;
      }
    } else {
      linear_filterable = false;
    }
    if (!linear_filterable) {
      parameters.mag_linear = 0;
      parameters.min_linear = 0;
      parameters.mip_linear = 0;
      aniso_filter = xenos::AnisoFilter::kDisabled;
    }
  }
  parameters.mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  }
  parameters.aniso_filter = aniso_filter;

  return parameters;
}

VkSampler NativeTextureCache::UseSampler(SamplerParameters parameters) {
  auto it = samplers_.find(parameters);
  if (it != samplers_.end()) {
    return it->second;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkSamplerCreateInfo sampler_create_info = {};
  sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_create_info.magFilter = parameters.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_create_info.minFilter = parameters.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_create_info.mipmapMode =
      parameters.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  static const VkSamplerAddressMode kAddressModeMap[] = {
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
  };
  sampler_create_info.addressModeU = kAddressModeMap[uint32_t(parameters.clamp_x)];
  sampler_create_info.addressModeV = kAddressModeMap[uint32_t(parameters.clamp_y)];
  sampler_create_info.addressModeW = kAddressModeMap[uint32_t(parameters.clamp_z)];
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    sampler_create_info.anisotropyEnable = VK_TRUE;
    sampler_create_info.maxAnisotropy = float(
        UINT32_C(1) << (uint32_t(parameters.aniso_filter) - uint32_t(xenos::AnisoFilter::kMax_1_1)));
  }
  sampler_create_info.minLod = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    sampler_create_info.maxLod = sampler_create_info.minLod + 0.25f;
  } else {
    sampler_create_info.maxLod = VK_LOD_CLAMP_NONE;
  }
  sampler_create_info.borderColor = parameters.border_color == xenos::BorderColor::k_ABGR_White
                                        ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                        : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

  VkSampler sampler = VK_NULL_HANDLE;
  if (dfn.vkCreateSampler(device, &sampler_create_info, nullptr, &sampler) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: texture cache failed to create sampler 0x{:08X}", parameters.value);
    return VK_NULL_HANDLE;
  }
  samplers_.emplace(parameters, sampler);
  return sampler;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool NativeTextureCache::Initialize() {
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device_->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device_->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device_->properties();

  // Start from the best formats.
  for (size_t i = 0; i < rex::countof(host_formats_); ++i) {
    host_formats_[i] = kBestHostFormats[i];
  }

  // The YUV 4:2:2 formats are always decoded to RGBA8 in the load shader (no
  // VkSamplerYcbcrConversion is ever created).
  host_formats_[uint32_t(xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP)].format_unsigned = {
      kLoadShaderIndexGBGR8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, false};
  host_formats_[uint32_t(xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP)].unsigned_signed_compatible =
      false;
  host_formats_[uint32_t(xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP)].format_unsigned = {
      kLoadShaderIndexBGRG8ToRGB8, VK_FORMAT_R8G8B8A8_UNORM, false, false};
  host_formats_[uint32_t(xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP)].unsigned_signed_compatible =
      false;

  constexpr VkFormatFeatureFlags kLinearFilterFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  VkFormatProperties format_properties;

  // BC formats are optional; fall back to compute decompression to RGBA8/RG8/R8
  // if unfilterable (matches VulkanTextureCache).
  auto bc_fallback = [&](xenos::TextureFormat format, VkFormat bc_format, LoadShaderIndex load,
                         VkFormat decoded, xenos::TextureFormat as_variant) {
    HostFormatPair& hf = host_formats_[uint32_t(format)];
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device, bc_format, &format_properties);
    if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) != kLinearFilterFeatures) {
      hf.format_unsigned.load_shader = load;
      hf.format_unsigned.format = decoded;
      hf.format_unsigned.block_compressed = false;
      if (as_variant != xenos::TextureFormat(0)) {
        host_formats_[uint32_t(as_variant)] = hf;
      }
    }
  };
  bc_fallback(xenos::TextureFormat::k_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kLoadShaderIndexDXT1ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM, xenos::TextureFormat::k_DXT1_AS_16_16_16_16);
  bc_fallback(xenos::TextureFormat::k_DXT2_3, VK_FORMAT_BC2_UNORM_BLOCK, kLoadShaderIndexDXT3ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM, xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16);
  bc_fallback(xenos::TextureFormat::k_DXT4_5, VK_FORMAT_BC3_UNORM_BLOCK, kLoadShaderIndexDXT5ToRGBA8,
              VK_FORMAT_R8G8B8A8_UNORM, xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16);
  bc_fallback(xenos::TextureFormat::k_DXN, VK_FORMAT_BC5_UNORM_BLOCK, kLoadShaderIndexDXNToRG8,
              VK_FORMAT_R8G8_UNORM, xenos::TextureFormat(0));
  bc_fallback(xenos::TextureFormat::k_DXT5A, VK_FORMAT_BC4_UNORM_BLOCK, kLoadShaderIndexDXT5AToR8,
              VK_FORMAT_R8_UNORM, xenos::TextureFormat(0));

  // Optional normalized 16-bit formats: fall back to SFLOAT if not filterable.
  auto norm16_fallback = [&](xenos::TextureFormat format, VkFormat unorm, LoadShaderIndex unorm_load,
                             VkFormat snorm, LoadShaderIndex snorm_load, VkFormat sfloat) {
    HostFormatPair& hf = host_formats_[uint32_t(format)];
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device, unorm, &format_properties);
    if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) != kLinearFilterFeatures) {
      hf.format_unsigned.load_shader = unorm_load;
      hf.format_unsigned.format = sfloat;
    }
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device, snorm, &format_properties);
    if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) != kLinearFilterFeatures) {
      hf.format_signed.load_shader = snorm_load;
      hf.format_signed.format = sfloat;
    }
  };
  norm16_fallback(xenos::TextureFormat::k_16, VK_FORMAT_R16_UNORM, kLoadShaderIndexR16UNormToFloat,
                  VK_FORMAT_R16_SNORM, kLoadShaderIndexR16SNormToFloat, VK_FORMAT_R16_SFLOAT);
  norm16_fallback(xenos::TextureFormat::k_16_16, VK_FORMAT_R16G16_UNORM,
                  kLoadShaderIndexRG16UNormToFloat, VK_FORMAT_R16G16_SNORM,
                  kLoadShaderIndexRG16SNormToFloat, VK_FORMAT_R16G16_SFLOAT);
  norm16_fallback(xenos::TextureFormat::k_16_16_16_16, VK_FORMAT_R16G16B16A16_UNORM,
                  kLoadShaderIndexRGBA16UNormToFloat, VK_FORMAT_R16G16B16A16_SNORM,
                  kLoadShaderIndexRGBA16SNormToFloat, VK_FORMAT_R16G16B16A16_SFLOAT);

  // Normalize + probe support for all formats.
  for (size_t i = 0; i < rex::countof(host_formats_); ++i) {
    HostFormatPair& host_format = host_formats_[i];
    if (host_format.format_unsigned.format == VK_FORMAT_UNDEFINED) {
      host_format.format_unsigned.load_shader = kLoadShaderIndexUnknown;
    }
    if (host_format.format_unsigned.load_shader == kLoadShaderIndexUnknown) {
      host_format.format_unsigned.format = VK_FORMAT_UNDEFINED;
      host_format.format_unsigned.linear_filterable = false;
    }
    if (host_format.format_signed.format == VK_FORMAT_UNDEFINED) {
      host_format.format_signed.load_shader = kLoadShaderIndexUnknown;
    }
    if (host_format.format_signed.load_shader == kLoadShaderIndexUnknown) {
      host_format.format_signed.format = VK_FORMAT_UNDEFINED;
      host_format.format_signed.linear_filterable = false;
    }
    if (host_format.format_unsigned.format != VK_FORMAT_UNDEFINED) {
      ifn.vkGetPhysicalDeviceFormatProperties(physical_device, host_format.format_unsigned.format,
                                              &format_properties);
      if (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        host_format.format_unsigned.linear_filterable =
            (format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      } else {
        host_format.format_unsigned.format = VK_FORMAT_UNDEFINED;
        host_format.format_unsigned.load_shader = kLoadShaderIndexUnknown;
        host_format.format_unsigned.linear_filterable = false;
      }
    }
    if (host_format.format_signed.format != VK_FORMAT_UNDEFINED) {
      ifn.vkGetPhysicalDeviceFormatProperties(physical_device, host_format.format_signed.format,
                                              &format_properties);
      if (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
        host_format.format_signed.linear_filterable =
            (format_properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      } else {
        host_format.format_signed.format = VK_FORMAT_UNDEFINED;
        host_format.format_signed.load_shader = kLoadShaderIndexUnknown;
        host_format.format_signed.linear_filterable = false;
      }
    }
  }

  // Load descriptor set layout (single storage buffer, compute) + pipeline layout.
  VkDescriptorSetLayoutBinding storage_binding = {};
  storage_binding.binding = 0;
  storage_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  storage_binding.descriptorCount = 1;
  storage_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo storage_layout_info = {};
  storage_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  storage_layout_info.bindingCount = 1;
  storage_layout_info.pBindings = &storage_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &storage_layout_info, nullptr,
                                      &load_source_dest_set_layout_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: texture cache failed to create load set layout");
    return false;
  }

  VkDescriptorSetLayout load_set_layouts[kLoadDescriptorSetCount] = {load_source_dest_set_layout_,
                                                                     load_source_dest_set_layout_};
  VkPushConstantRange push_constant_range = {};
  push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_constant_range.offset = 0;
  push_constant_range.size = sizeof(LoadConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = kLoadDescriptorSetCount;
  pipeline_layout_info.pSetLayouts = load_set_layouts;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &load_pipeline_layout_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: texture cache failed to create load pipeline layout");
    return false;
  }

  // Load shader SPIR-V blobs (unscaled only).
  std::pair<const uint32_t*, size_t> load_shader_code[kLoadShaderCount] = {};
  load_shader_code[kLoadShaderIndex8bpb] =
      std::make_pair(shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs));
  load_shader_code[kLoadShaderIndex16bpb] =
      std::make_pair(shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs));
  load_shader_code[kLoadShaderIndex32bpb] =
      std::make_pair(shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs));
  load_shader_code[kLoadShaderIndex64bpb] =
      std::make_pair(shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs));
  load_shader_code[kLoadShaderIndex128bpb] =
      std::make_pair(shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs));
  load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] = std::make_pair(
      shaders::texture_load_r5g5b5a1_b5g5r5a1_cs, sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs));
  load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] = std::make_pair(
      shaders::texture_load_r5g6b5_b5g6r5_cs, sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs));
  load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
      std::make_pair(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
                     sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs));
  load_shader_code[kLoadShaderIndexRGBA4ToARGB4] = std::make_pair(
      shaders::texture_load_r4g4b4a4_a4r4g4b4_cs, sizeof(shaders::texture_load_r4g4b4a4_a4r4g4b4_cs));
  load_shader_code[kLoadShaderIndexGBGR8ToRGB8] = std::make_pair(
      shaders::texture_load_gbgr8_rgb8_cs, sizeof(shaders::texture_load_gbgr8_rgb8_cs));
  load_shader_code[kLoadShaderIndexBGRG8ToRGB8] = std::make_pair(
      shaders::texture_load_bgrg8_rgb8_cs, sizeof(shaders::texture_load_bgrg8_rgb8_cs));
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] = std::make_pair(
      shaders::texture_load_r10g11b11_rgba16_cs, sizeof(shaders::texture_load_r10g11b11_rgba16_cs));
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
      std::make_pair(shaders::texture_load_r10g11b11_rgba16_snorm_cs,
                     sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs));
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] = std::make_pair(
      shaders::texture_load_r11g11b10_rgba16_cs, sizeof(shaders::texture_load_r11g11b10_rgba16_cs));
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
      std::make_pair(shaders::texture_load_r11g11b10_rgba16_snorm_cs,
                     sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs));
  load_shader_code[kLoadShaderIndexR16UNormToFloat] = std::make_pair(
      shaders::texture_load_r16_unorm_float_cs, sizeof(shaders::texture_load_r16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexR16SNormToFloat] = std::make_pair(
      shaders::texture_load_r16_snorm_float_cs, sizeof(shaders::texture_load_r16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexRG16UNormToFloat] = std::make_pair(
      shaders::texture_load_rg16_unorm_float_cs, sizeof(shaders::texture_load_rg16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexRG16SNormToFloat] = std::make_pair(
      shaders::texture_load_rg16_snorm_float_cs, sizeof(shaders::texture_load_rg16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexRGBA16UNormToFloat] =
      std::make_pair(shaders::texture_load_rgba16_unorm_float_cs,
                     sizeof(shaders::texture_load_rgba16_unorm_float_cs));
  load_shader_code[kLoadShaderIndexRGBA16SNormToFloat] =
      std::make_pair(shaders::texture_load_rgba16_snorm_float_cs,
                     sizeof(shaders::texture_load_rgba16_snorm_float_cs));
  load_shader_code[kLoadShaderIndexDXT1ToRGBA8] = std::make_pair(
      shaders::texture_load_dxt1_rgba8_cs, sizeof(shaders::texture_load_dxt1_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXT3ToRGBA8] = std::make_pair(
      shaders::texture_load_dxt3_rgba8_cs, sizeof(shaders::texture_load_dxt3_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXT5ToRGBA8] = std::make_pair(
      shaders::texture_load_dxt5_rgba8_cs, sizeof(shaders::texture_load_dxt5_rgba8_cs));
  load_shader_code[kLoadShaderIndexDXNToRG8] =
      std::make_pair(shaders::texture_load_dxn_rg8_cs, sizeof(shaders::texture_load_dxn_rg8_cs));
  load_shader_code[kLoadShaderIndexDXT3A] =
      std::make_pair(shaders::texture_load_dxt3a_cs, sizeof(shaders::texture_load_dxt3a_cs));
  load_shader_code[kLoadShaderIndexDXT3AAs1111ToARGB4] = std::make_pair(
      shaders::texture_load_dxt3aas1111_argb4_cs, sizeof(shaders::texture_load_dxt3aas1111_argb4_cs));
  load_shader_code[kLoadShaderIndexDXT5AToR8] =
      std::make_pair(shaders::texture_load_dxt5a_r8_cs, sizeof(shaders::texture_load_dxt5a_r8_cs));
  load_shader_code[kLoadShaderIndexCTX1] =
      std::make_pair(shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs));
  load_shader_code[kLoadShaderIndexDepthUnorm] = std::make_pair(
      shaders::texture_load_depth_unorm_cs, sizeof(shaders::texture_load_depth_unorm_cs));
  load_shader_code[kLoadShaderIndexDepthFloat] = std::make_pair(
      shaders::texture_load_depth_float_cs, sizeof(shaders::texture_load_depth_float_cs));

  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    if (!load_shader_code[i].first) {
      continue;
    }
    load_pipelines_[i] = ui::vulkan::util::CreateComputePipeline(
        vulkan_device_, load_pipeline_layout_, load_shader_code[i].first, load_shader_code[i].second);
    if (load_pipelines_[i] == VK_NULL_HANDLE) {
      REXLOG_ERROR("rexgpu-native: texture cache failed to create load pipeline {}", i);
      return false;
    }
  }

  // Null (opaque-black) fallback images (2D array/cube + 3D).
  VkImageCreateInfo null_image_info = {};
  null_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  null_image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  null_image_info.imageType = VK_IMAGE_TYPE_2D;
  null_image_info.format = kInvalidTextureFetchFallbackFormat;
  null_image_info.extent = {1, 1, 1};
  null_image_info.mipLevels = 1;
  null_image_info.arrayLayers = 6;
  null_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  null_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  null_image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  null_image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  null_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (dfn.vkCreateImage(device, &null_image_info, nullptr, &null_image_2d_array_cube_) !=
      VK_SUCCESS) {
    return false;
  }
  null_image_info.flags = 0;
  null_image_info.imageType = VK_IMAGE_TYPE_3D;
  null_image_info.arrayLayers = 1;
  if (dfn.vkCreateImage(device, &null_image_info, nullptr, &null_image_3d_) != VK_SUCCESS) {
    return false;
  }
  {
    VkImage null_images[2] = {null_image_2d_array_cube_, null_image_3d_};
    for (int i = 0; i < 2; ++i) {
      VkMemoryRequirements req;
      dfn.vkGetImageMemoryRequirements(device, null_images[i], &req);
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
      if (dfn.vkAllocateMemory(device, &alloc, nullptr, &null_images_memory_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn.vkBindImageMemory(device, null_images[i], null_images_memory_[i], 0) != VK_SUCCESS) {
        return false;
      }
    }
  }
  {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = null_image_2d_array_cube_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = kInvalidTextureFetchFallbackFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &null_image_view_2d_array_) !=
        VK_SUCCESS) {
      return false;
    }
    view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view_info.subresourceRange.layerCount = 6;
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &null_image_view_cube_) != VK_SUCCESS) {
      return false;
    }
    view_info.image = null_image_3d_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view_info.subresourceRange.layerCount = 1;
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &null_image_view_3d_) != VK_SUCCESS) {
      return false;
    }
  }

  // Samplers.
  if (device_properties.samplerAnisotropy) {
    max_anisotropy_ = xenos::AnisoFilter(
        uint32_t(xenos::AnisoFilter::kMax_1_1) +
        (31 - rex::lzcnt(uint32_t(
                  std::min(16.0f, std::max(1.0f, device_properties.maxSamplerAnisotropy))))));
  } else {
    max_anisotropy_ = xenos::AnisoFilter::kDisabled;
  }

  // Own load command infrastructure + transient descriptor pool.
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags =
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &load_command_pool_) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo cb_info = {};
  cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_info.commandPool = load_command_pool_;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &cb_info, &load_command_buffer_) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (dfn.vkCreateFence(device, &fence_info, nullptr, &load_fence_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorPoolSize pool_size = {};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = kLoadDescriptorSetCount + 2;
  VkDescriptorPoolCreateInfo descriptor_pool_info = {};
  descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_info.maxSets = kLoadDescriptorSetCount + 2;
  descriptor_pool_info.poolSizeCount = 1;
  descriptor_pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_info, nullptr, &load_descriptor_pool_) !=
      VK_SUCCESS) {
    return false;
  }

  // Clear the null images to opaque-black and transition them for sampling.
  dfn.vkResetCommandPool(device, load_command_pool_, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(load_command_buffer_, &begin_info) != VK_SUCCESS) {
    return false;
  }
  const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkImage null_images[2] = {null_image_2d_array_cube_, null_image_3d_};
  for (int i = 0; i < 2; ++i) {
    VkImageSubresourceRange range = WholeColorRange();
    VkImageMemoryBarrier to_dst = {};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = null_images[i];
    to_dst.subresourceRange = range;
    dfn.vkCmdPipelineBarrier(load_command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
    dfn.vkCmdClearColorImage(load_command_buffer_, null_images[i],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(load_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             guest_shader_pipeline_stages_, 0, 0, nullptr, 0, nullptr, 1, &to_read);
  }
  if (dfn.vkEndCommandBuffer(load_command_buffer_) != VK_SUCCESS) {
    return false;
  }
  dfn.vkResetFences(device, 1, &load_fence_);
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &load_command_buffer_;
  {
    const ui::vulkan::VulkanDevice::Queue::Acquisition acq =
        vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(acq.queue(), 1, &submit, load_fence_) != VK_SUCCESS) {
      return false;
    }
  }
  dfn.vkWaitForFences(device, 1, &load_fence_, VK_TRUE, UINT64_MAX);

  REXLOG_INFO("rexgpu-native: texture cache initialized (max_aniso={})", uint32_t(max_anisotropy_));
  return true;
}

}  // namespace rex::graphics::native
