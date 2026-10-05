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
  uint32_t height = key.GetHeight() * draw_resolution_scale_y();
  
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
  auto it = framebuffers_.find(mvp_current_key_);
  return it != framebuffers_.end() ? it->second.framebuffer.get() : nullptr;
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
    REXLOG_ERROR("PlumeRenderTargetCache::Resolve: GetResolveInfo failed");
    return false;
  }

  const uint32_t resolve_width =
      resolve_info.coordinate_info.width_div_8 * rex::graphics::xenos::kResolveAlignmentPixels;
  const uint32_t resolve_height =
      resolve_info.height_div_8 * rex::graphics::xenos::kResolveAlignmentPixels;
  REXLOG_DEBUG(
      "PlumeRenderTargetCache::Resolve: rect={}x{} dest_base=0x{:08X} extent_start=0x{:08X} extent_len=0x{:X} depth={}",
      resolve_width, resolve_height, resolve_info.copy_dest_base,
      resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
      resolve_info.IsCopyingDepth());

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
    // Atualizar o framebuffer primario apenas se for widescreen (ignora shadow maps quadrados como 1600x1600)
    if (width > height && (float(width) / float(height) >= 1.25f)) {
      uint32_t area = width * height;
      if (area > mvp_primary_area_) {
        mvp_primary_area_ = area;
        mvp_primary_key_  = key;
      }
    }
    return it->second.framebuffer.get();
  }

  if (!device) return nullptr;

  FramebufferEntry entry;
  entry.width = width;
  entry.height = height;
  REXLOG_INFO("PlumeRenderTargetCache: creating framebuffer {}x{} color_fmt={} depth_fmt={}",
              width, height, static_cast<uint32_t>(color_fmt), static_cast<uint32_t>(depth_fmt));

  auto color_desc = ::plume::RenderTextureDesc::ColorTarget(width, height, color_fmt);
  REXLOG_DEBUG("PlumeRenderTargetCache: creating color texture");
  entry.color_texture = device->createTexture(color_desc);
  if (!entry.color_texture) {
    REXLOG_ERROR("PlumeRenderTargetCache: color texture creation FAILED ({}x{}, format={})",
                 width, height, static_cast<uint32_t>(color_fmt));
    return nullptr;
  }
  REXLOG_DEBUG("PlumeRenderTargetCache: color texture created");

  // Do not create a depth image when the Xenos draw has depth/stencil disabled.
  // Passing UNKNOWN into DepthTarget can produce an invalid Vulkan image create
  // request on the Intel driver (VMA reports VK_ERROR_INITIALIZATION_FAILED).
  if (depth_fmt != ::plume::RenderFormat::UNKNOWN) {
    auto depth_desc = ::plume::RenderTextureDesc::DepthTarget(width, height, depth_fmt);
    REXLOG_DEBUG("PlumeRenderTargetCache: creating depth texture");
    entry.depth_texture = device->createTexture(depth_desc);
    if (!entry.depth_texture) {
      REXLOG_ERROR("PlumeRenderTargetCache: depth texture creation FAILED ({}x{}, format={})",
                   width, height, static_cast<uint32_t>(depth_fmt));
      framebuffers_.erase(key);
      return nullptr;
    }
    REXLOG_DEBUG("PlumeRenderTargetCache: depth texture created");
  } else {
    REXLOG_DEBUG("PlumeRenderTargetCache: depth disabled; no depth image created");
  }

  const ::plume::RenderTexture* color_ptrs[] = { entry.color_texture.get() };
  ::plume::RenderFramebufferDesc fb_desc(color_ptrs, 1, entry.depth_texture.get());
  REXLOG_DEBUG("PlumeRenderTargetCache: creating framebuffer object");
  entry.framebuffer = device->createFramebuffer(fb_desc);
  if (!entry.framebuffer) {
    REXLOG_ERROR("PlumeRenderTargetCache: framebuffer creation FAILED");
    return nullptr;
  }
  REXLOG_INFO("PlumeRenderTargetCache: framebuffer created successfully");

  auto* fb_ptr = entry.framebuffer.get();
  framebuffers_[key] = std::move(entry);
  mvp_current_key_ = key;

  // Rastrear o maior framebuffer widescreen como primario (cena principal)
  if (width > height && (float(width) / float(height) >= 1.25f)) {
    uint32_t area = width * height;
    if (area > mvp_primary_area_) {
      mvp_primary_area_ = area;
      mvp_primary_key_  = key;
    }
  }

  return fb_ptr;
}

::plume::RenderTexture* PlumeRenderTargetCache::MVP_GetColorTexture() {
  auto it = framebuffers_.find(mvp_current_key_);
  if (it != framebuffers_.end()) {
    return it->second.color_texture.get();
  }
  return nullptr;
}

::plume::RenderTexture* PlumeRenderTargetCache::MVP_GetPrimaryColorTexture(uint32_t frontbuffer_width, uint32_t frontbuffer_height) {
  // Se o jogo forneceu dimensões do frontbuffer (ex: 1024x576), procura correspondência exata de altura
  // e largura suficiente para conter o frontbuffer.
  if (frontbuffer_width > 0 && frontbuffer_height > 0) {
    // Prefer an exact-size render target. The Xenos resolve trace for SF3
    // resolves a 1280x720 rectangle to the frontbuffer, so selecting an
    // arbitrary larger widescreen target can present the wrong surface.
    for (const auto& [k, entry] : framebuffers_) {
      if (entry.width == frontbuffer_width && entry.height == frontbuffer_height) {
        return entry.color_texture.get();
      }
    }

    // Otherwise choose the smallest target that fully contains the requested
    // frontbuffer dimensions. Do not depend on unordered_map iteration order.
    ::plume::RenderTexture* best_tex = nullptr;
    uint64_t best_area = UINT64_MAX;
    for (const auto& [k, entry] : framebuffers_) {
      if (entry.width >= frontbuffer_width && entry.height >= frontbuffer_height) {
        const uint64_t area = uint64_t(entry.width) * entry.height;
        if (area < best_area) {
          best_area = area;
          best_tex = entry.color_texture.get();
        }
      }
    }
    if (best_tex) return best_tex;

    // Last dimension-based fallback: matching height, then closest width.
    uint32_t best_width = UINT32_MAX;
    for (const auto& [k, entry] : framebuffers_) {
      if (entry.height == frontbuffer_height && entry.width >= frontbuffer_width &&
          entry.width < best_width) {
        best_width = entry.width;
        best_tex = entry.color_texture.get();
      }
    }
    if (best_tex) return best_tex;
  }

  // Fallback: procura o maior framebuffer widescreen (aspect ratio >= 1.25, excluindo mapas quadrados)
  ::plume::RenderTexture* best_tex = nullptr;
  uint32_t best_area = 0;
  for (const auto& [k, entry] : framebuffers_) {
    if (entry.height > 0 && entry.width > entry.height) {
      float aspect = float(entry.width) / float(entry.height);
      if (aspect >= 1.25f) {
        uint32_t area = entry.width * entry.height;
        if (area > best_area) {
          best_area = area;
          best_tex = entry.color_texture.get();
        }
      }
    }
  }
  if (best_tex) {
    return best_tex;
  }

  // Fallback para a chave primária rastreada
  auto it = framebuffers_.find(mvp_primary_key_);
  if (it != framebuffers_.end()) {
    return it->second.color_texture.get();
  }
  return MVP_GetColorTexture();
}

}  // namespace rex::graphics_plume
