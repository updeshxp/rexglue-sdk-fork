/**
 * @file        graphics_plume/plume_texture_cache.h
 * @brief       PlumeTextureCache implementation based on rex::graphics::TextureCache
 */

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <plume_render_interface.h>

#include "plume_texture_uploader.h"

namespace rex::graphics_plume {

class PlumeCommandProcessor;

class PlumeTextureCache final : public rex::graphics::TextureCache {
 public:
  using rex::graphics::TextureCache::ResetTextureBindings;

  union SamplerParameters {
    uint32_t value;
    struct {
      rex::graphics::xenos::ClampMode clamp_x : 3;
      rex::graphics::xenos::ClampMode clamp_y : 3;
      rex::graphics::xenos::ClampMode clamp_z : 3;
      rex::graphics::xenos::BorderColor border_color : 2;
      uint32_t mag_linear : 1;
      uint32_t min_linear : 1;
      uint32_t mip_linear : 1;
      rex::graphics::xenos::AnisoFilter aniso_filter : 3;
      uint32_t mip_min_level : 4;
      uint32_t mip_base_map : 1;
    };
    SamplerParameters() : value(0) {}
    bool operator==(const SamplerParameters& p) const { return value == p.value; }
  };

  struct SamplerHash {
    size_t operator()(const SamplerParameters& p) const { return std::hash<uint32_t>{}(p.value); }
  };

  class PlumeTexture final : public Texture {
   public:
    PlumeTexture(PlumeTextureCache& texture_cache, TextureKey key, ::plume::RenderDevice* device);
    ~PlumeTexture() override;
    
    ::plume::RenderTexture* plume_texture() const { return texture_.get(); }
    ::plume::RenderTextureView* plume_srv() const { return srv_.get(); }
    
    std::unique_ptr<::plume::RenderTexture> texture_;
    std::unique_ptr<::plume::RenderTextureView> srv_;
  };

  PlumeTextureCache(const rex::graphics::RegisterFile& register_file,
                    rex::graphics::SharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y,
                    PlumeCommandProcessor& command_processor,
                    ::plume::RenderDevice* device);
  ~PlumeTextureCache() override;

  bool Initialize();

  void BeginSubmission(uint64_t new_submission_index) override;
  void BeginFrame() override;
  void EndFrame();
  
  void RequestTextures(uint32_t used_texture_mask) override;

  ::plume::RenderSampler* UseSampler(SamplerParameters params);
  ::plume::RenderTextureView* GetActiveBindingTextureView(uint32_t fetch_constant_index);
  PlumeTexture* GetActiveBindingPlumeTexture(uint32_t fetch_constant_index);
  SamplerParameters GetSamplerParameters(const rex::graphics::SpirvShader::SamplerBinding& binding) const;

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override { return false; }
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override { return false; }
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  
  uint32_t GetMaxHostTextureWidthHeight(rex::graphics::xenos::DataDimension dimension) const override { return 8192; }
  uint32_t GetMaxHostTextureDepthOrArraySize(rex::graphics::xenos::DataDimension dimension) const override { return 2048; }

  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;

  bool EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled, uint32_t length_unscaled,
                                          uint32_t length_scaled_alignment_log2 = 0) override { return false; }

  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  PlumeCommandProcessor& command_processor_;
  ::plume::RenderDevice* device_;
  
  std::unique_ptr<PlumeTextureUploader> texture_uploader_;

  std::unordered_map<SamplerParameters, std::unique_ptr<::plume::RenderSampler>, SamplerHash> samplers_;
};

}  // namespace rex::graphics_plume
