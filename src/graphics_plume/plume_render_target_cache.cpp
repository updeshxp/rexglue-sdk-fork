#include "plume_render_target_cache.h"
#include "plume_command_processor.h"
#include "plume_texture_cache.h"
#include "rex/graphics/register_file.h"
#include "rex/graphics/registers.h"
#include "rex/logging.h"

namespace rex::graphics_plume {

PlumeRenderTargetCache::PlumeRenderTargetCache(
    const rex::graphics::RegisterFile& register_file,
    const rex::memory::Memory& memory,
    ::plume::RenderDevice* device,
    uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y)
    : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
      device_(device) {}

PlumeRenderTargetCache::~PlumeRenderTargetCache() {
  Shutdown();
}

bool PlumeRenderTargetCache::Initialize() {
  InitializeCommon();
  return true;
}

void PlumeRenderTargetCache::Shutdown() {
  ShutdownCommon();
  ClearCache();
}

void PlumeRenderTargetCache::ClearCache() {
  framebuffers_.clear();
  RenderTargetCache::ClearCache();
}

void PlumeRenderTargetCache::EndFrame() {
  // Can cleanup unused textures if needed
}

::plume::RenderFormat PlumeRenderTargetCache::GetPlumeColorFormat(rex::graphics::xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return ::plume::RenderFormat::B8G8R8A8_UNORM;
    default:
      return ::plume::RenderFormat::B8G8R8A8_UNORM;
  }
}

::plume::RenderFormat PlumeRenderTargetCache::GetPlumeDepthFormat(rex::graphics::xenos::DepthRenderTargetFormat format) {
  return ::plume::RenderFormat::D32_FLOAT_S8_UINT;
}

rex::graphics::RenderTargetCache::RenderTarget* PlumeRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {
  uint32_t width = key.GetWidth() * draw_resolution_scale_x();
  // Xenos height is generally dynamic based on EDRAM layout, but typically ~720 scaled
  uint32_t height = 720 * draw_resolution_scale_y(); // Simplified for MVP
  
  auto rt = new PlumeRenderTarget(key, device_, width, height);
  if (!rt->texture) {
    delete rt;
    return nullptr;
  }
  return rt;
}

PlumeRenderTargetCache::PlumeRenderTarget::PlumeRenderTarget(RenderTargetKey key, ::plume::RenderDevice* device, uint32_t width, uint32_t height) 
    : RenderTarget(key) {
  ::plume::RenderTextureDesc desc;
  if (key.is_depth) {
    desc = ::plume::RenderTextureDesc::DepthTarget(width, height, ::plume::RenderFormat::D32_FLOAT_S8_UINT);
  } else {
    desc = ::plume::RenderTextureDesc::ColorTarget(width, height, ::plume::RenderFormat::B8G8R8A8_UNORM);
  }

  texture = device->createTexture(desc);
}

::plume::RenderFramebuffer* PlumeRenderTargetCache::GetCurrentFramebuffer() {
  return nullptr; // Stub
}

#include "rex/graphics/util/draw.h"

bool PlumeRenderTargetCache::Resolve(const rex::memory::Memory& memory, PlumeSharedMemory& shared_memory,
                                     PlumeTextureCache& texture_cache, uint32_t& written_address_out,
                                     uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  rex::graphics::draw_util::ResolveInfo resolve_info;
  if (!rex::graphics::draw_util::GetResolveInfo(register_file(), memory, draw_resolution_scale_x(),
                                                draw_resolution_scale_y(), false, false, resolve_info)) {
    return false;
  }

  if (resolve_info.copy_dest_extent_length > 0) {
    written_address_out = resolve_info.copy_dest_extent_start;
    written_length_out = resolve_info.copy_dest_extent_length;

    texture_cache.ResetTextureBindings();
    shared_memory.RangeWrittenByGpu(written_address_out, written_length_out);
  }

  return true;
}

::plume::RenderFramebuffer* PlumeRenderTargetCache::MVP_GetOrCreateFramebuffer(::plume::RenderDevice* device, uint32_t width, uint32_t height, ::plume::RenderFormat color_fmt, ::plume::RenderFormat depth_fmt) {
  uint64_t key = (static_cast<uint64_t>(width)      << 48) |
                 (static_cast<uint64_t>(height)     << 32) |
                 (static_cast<uint64_t>(color_fmt)  << 16) |
                 (static_cast<uint64_t>(depth_fmt));

  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    mvp_current_key_ = key;
    return it->second.framebuffer.get();
  }

  if (!device) return nullptr;

  FramebufferEntry entry;
  auto color_desc = ::plume::RenderTextureDesc::ColorTarget(width, height, color_fmt);
  entry.color_texture = device->createTexture(color_desc);

  auto depth_desc = ::plume::RenderTextureDesc::DepthTarget(width, height, depth_fmt);
  entry.depth_texture = device->createTexture(depth_desc);

  const ::plume::RenderTexture* color_ptrs[] = { entry.color_texture.get() };
  ::plume::RenderFramebufferDesc fb_desc(color_ptrs, 1, entry.depth_texture.get());
  entry.framebuffer = device->createFramebuffer(fb_desc);

  auto* fb_ptr = entry.framebuffer.get();
  framebuffers_[key] = std::move(entry);
  mvp_current_key_ = key;
  return fb_ptr;
}

::plume::RenderTexture* PlumeRenderTargetCache::MVP_GetColorTexture() {
  auto it = framebuffers_.find(mvp_current_key_);
  if (it != framebuffers_.end()) {
    return it->second.color_texture.get();
  }
  return nullptr;
}

}  // namespace rex::graphics_plume
