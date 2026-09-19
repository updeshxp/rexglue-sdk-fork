/**
 * @file        graphics/native/texture_cache.h
 * @brief       Native GPU renderer - real guest texture cache (Phase 3).
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     NativeTextureCache derives from the shared, game-agnostic
 *              rex::graphics::TextureCache. The base class does ALL of the hard,
 *              solved work: fetch-constant -> TextureKey resolution, LRU
 *              eviction, shared-memory residency/watching, and the guest texture
 *              memory layout math. This subclass provides only the Vulkan-native
 *              seams (mirroring rex::graphics::vulkan::VulkanTextureCache) but is
 *              fully SELF-CONTAINED: it owns its own command pool / command
 *              buffer / fence / descriptor pools / scratch buffer, so it does NOT
 *              depend on VulkanCommandProcessor (which VulkanTextureCache does).
 *
 *              Guest textures are untiled + format-decoded on the GPU using the
 *              exact same texture_load_*_cs SPIR-V compute shaders the Vulkan
 *              emulation backend uses, reading from the resident 512 MB
 *              NativeSharedMemory buffer and writing into a linear scratch buffer
 *              that is then copied into a sampled VkImage.
 *
 *              PHASE 3 MVP SIMPLIFICATIONS vs VulkanTextureCache:
 *              - Unsigned host format only (signed-separate formats not split);
 *                is_signed is ignored when selecting a view.
 *              - No resolution scaling (draw_resolution_scale == 1).
 *              - No two-pass float16 conversion fallback (R10G11B11/R11G11B10 to
 *                RGBA16F): those formats are loaded with their direct load shader
 *                where a UNORM/SNORM host format exists, otherwise dropped.
 *              - Per-texture immediate submit-and-wait for the load work, so a
 *                loaded texture is already in SHADER_READ_ONLY_OPTIMAL before any
 *                deferred guest draw replays it.
 *              - Samplers are created on demand and never evicted (the small
 *                per-frame sampler working set makes this fine).
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::native {

class NativeSharedMemory;

class NativeTextureCache final : public TextureCache {
 public:
  // Packed sampler parameters (matches VulkanTextureCache::SamplerParameters).
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    struct Hasher {
      size_t operator()(const SamplerParameters& parameters) const {
        return std::hash<uint32_t>{}(parameters.value);
      }
    };
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };

  static std::unique_ptr<NativeTextureCache> Create(const ui::vulkan::VulkanDevice* vulkan_device,
                                                    const RegisterFile& register_file,
                                                    NativeSharedMemory& shared_memory) {
    std::unique_ptr<NativeTextureCache> texture_cache(
        new NativeTextureCache(vulkan_device, register_file, shared_memory));
    if (!texture_cache->Initialize()) {
      return nullptr;
    }
    return texture_cache;
  }

  ~NativeTextureCache() override;

  // Advances the internal submission counter and resets bindings for retry.
  // Call once per native frame before requesting textures.
  void BeginNativeFrame();

  // Per-binding image view lookup (matches VulkanTextureCache). Returns the
  // resident texture's view, or a null (opaque-black) image view of the right
  // dimension when there's no valid texture. is_signed is accepted for API
  // parity but ignored in the MVP (unsigned view only).
  // out_hit, if non-null, is set to whether a real resident texture was
  // returned (true) vs. the opaque-black null fallback (false) - lets a
  // caller distinguish "this specific binding is unresolved" from "some
  // other cause" without needing to compare VkImageView handles itself.
  VkImageView GetActiveBindingOrNullImageView(uint32_t fetch_constant_index,
                                              xenos::FetchOpDimension dimension, bool is_signed,
                                              bool* out_hit = nullptr);
  VkImageView NullImageViewForDimension(xenos::FetchOpDimension dimension) const;

  // Sampler path (matches VulkanTextureCache). GetSamplerParameters is a pure
  // function of guest state; UseSampler creates-on-demand and caches.
  SamplerParameters GetSamplerParameters(const SpirvShader::SamplerBinding& binding) const;
  VkSampler UseSampler(SamplerParameters parameters);

 protected:
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;
  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;

 private:
  enum LoadDescriptorSetIndex {
    kLoadDescriptorSetIndexDestination,
    kLoadDescriptorSetIndexSource,
    kLoadDescriptorSetCount,
  };

  struct HostFormat {
    LoadShaderIndex load_shader;
    VkFormat format;
    // Whether the format is block-compressed on the host (host block size ==
    // guest format block size, not decompressed on load).
    bool block_compressed;
    // Set up dynamically based on device support.
    bool linear_filterable;
  };

  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Mapping of Xenos swizzle components to Vulkan format components.
    uint32_t swizzle;
    bool unsigned_signed_compatible;
  };

  class NativeTexture final : public Texture {
   public:
    // Takes ownership of the image and its memory. host_format is the VkFormat
    // the image was created with (used when creating views).
    explicit NativeTexture(NativeTextureCache& texture_cache, const TextureKey& key, VkImage image,
                           VkDeviceMemory memory, VkFormat host_format, uint64_t host_memory_usage);
    ~NativeTexture();

    VkImage image() const { return image_; }
    // Whether the image has already been transitioned to SHADER_READ_ONLY_OPTIMAL
    // (i.e. it has valid data). Loads set this true.
    bool loaded() const { return loaded_; }
    void set_loaded(bool loaded) { loaded_ = loaded; }

    // Returns (creating on first use) an image view for this texture with the
    // given host component swizzle. is_array selects a 2D-array vs plain 2D view
    // for 2D/stacked textures.
    VkImageView GetView(uint32_t host_swizzle, bool is_array = true);

   private:
    VkImage image_;
    VkDeviceMemory memory_;
    VkFormat host_format_;
    bool loaded_ = false;
    std::unordered_map<uint32_t, VkImageView> views_;
  };

  struct NativeTextureBinding {
    VkImageView image_view;
    NativeTextureBinding() { Reset(); }
    void Reset() { image_view = VK_NULL_HANDLE; }
  };

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

  explicit NativeTextureCache(const ui::vulkan::VulkanDevice* vulkan_device,
                              const RegisterFile& register_file, NativeSharedMemory& shared_memory);

  bool Initialize();

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  // Grows the scratch buffer to at least the given size if needed. Returns the
  // scratch VkBuffer or VK_NULL_HANDLE on failure.
  VkBuffer EnsureScratchBuffer(VkDeviceSize size);

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  NativeSharedMemory& native_shared_memory_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Host format table (initialized from the static best-format tables + device
  // capability probing in Initialize).
  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;
  HostFormatPair host_formats_[64];

  // Untiling compute load pipelines (unscaled only).
  VkDescriptorSetLayout load_source_dest_set_layout_ = VK_NULL_HANDLE;  // single storage buffer
  VkPipelineLayout load_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, kLoadShaderCount> load_pipelines_{};

  // Null (opaque-black) fallback images.
  std::array<VkDeviceMemory, 2> null_images_memory_{};
  VkImage null_image_2d_array_cube_ = VK_NULL_HANDLE;
  VkImage null_image_3d_ = VK_NULL_HANDLE;
  VkImageView null_image_view_2d_array_ = VK_NULL_HANDLE;
  VkImageView null_image_view_cube_ = VK_NULL_HANDLE;
  VkImageView null_image_view_3d_ = VK_NULL_HANDLE;

  std::array<NativeTextureBinding, xenos::kTextureFetchConstantCount> native_texture_bindings_;

  // Own load command infrastructure (immediate submit-and-wait).
  VkCommandPool load_command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer load_command_buffer_ = VK_NULL_HANDLE;
  VkFence load_fence_ = VK_NULL_HANDLE;
  // Transient descriptor pool for load compute sets (reset per load).
  VkDescriptorPool load_descriptor_pool_ = VK_NULL_HANDLE;

  // Linear scratch destination buffer for untiled data (grown on demand).
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory scratch_memory_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_size_ = 0;

  // Samplers (create-on-demand, no eviction in the MVP).
  std::unordered_map<SamplerParameters, VkSampler, SamplerParameters::Hasher> samplers_;
  xenos::AnisoFilter max_anisotropy_ = xenos::AnisoFilter::kMax_16_1;

  uint64_t submission_index_ = 1;
};

}  // namespace rex::graphics::native
