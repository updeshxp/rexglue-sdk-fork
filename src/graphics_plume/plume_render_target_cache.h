#pragma once

#include "rex/graphics/pipeline/render_target/cache.h"
#include <plume_render_interface.h>
#include <memory>
#include <unordered_map>

namespace rex::graphics_plume {

class PlumeSharedMemory;
class PlumeTextureCache;

class PlumeRenderTargetCache final : public rex::graphics::RenderTargetCache {
 public:
  PlumeRenderTargetCache(const rex::graphics::RegisterFile& register_file,
                         const rex::memory::Memory& memory,
                         ::plume::RenderDevice* device,
                         uint32_t draw_resolution_scale_x,
                         uint32_t draw_resolution_scale_y);
  ~PlumeRenderTargetCache() override;

  bool Initialize();
  void Shutdown();

  void ClearCache() override;

  Path GetPath() const override { return Path::kHostRenderTargets; }

  uint32_t GetMaxRenderTargetWidth() const override { return 8192; }
  uint32_t GetMaxRenderTargetHeight() const override { return 8192; }

  bool IsGammaFormatHostStorageSeparate() const override { return false; }
  bool IsHostDepthEncodingDifferent(rex::graphics::xenos::DepthRenderTargetFormat format) const override { return false; }

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool Resolve(const rex::memory::Memory& memory, PlumeSharedMemory& shared_memory,
               PlumeTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out);

  // Helper to fetch the current active framebuffer based on guest state
  ::plume::RenderFramebuffer* GetCurrentFramebuffer();
  ::plume::RenderTexture* GetCurrentFramebufferColorTexture();

  // MVP
  ::plume::RenderFramebuffer* MVP_GetOrCreateFramebuffer(::plume::RenderDevice* device, uint32_t width, uint32_t height, ::plume::RenderFormat color_fmt, ::plume::RenderFormat depth_fmt);
  ::plume::RenderTexture* MVP_GetColorTexture();
  
  void EndFrame();

 private:
  class PlumeRenderTarget : public RenderTarget {
   public:
    PlumeRenderTarget(RenderTargetKey key, ::plume::RenderDevice* device, uint32_t width, uint32_t height);
    ~PlumeRenderTarget() override = default;

    std::unique_ptr<::plume::RenderTexture> texture;
  };

  ::plume::RenderDevice* device_ = nullptr;
  
  struct FramebufferEntry {
    std::unique_ptr<::plume::RenderTexture> color_texture;
    std::unique_ptr<::plume::RenderTexture> depth_texture;
    std::unique_ptr<::plume::RenderFramebuffer> framebuffer;
  };
  std::unordered_map<uint64_t, FramebufferEntry> framebuffers_;
  uint64_t mvp_current_key_ = 0;

  ::plume::RenderFormat GetPlumeColorFormat(rex::graphics::xenos::ColorRenderTargetFormat format);
  ::plume::RenderFormat GetPlumeDepthFormat(rex::graphics::xenos::DepthRenderTargetFormat format);
};

}  // namespace rex::graphics_plume
