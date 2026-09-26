/**
 * @file        graphics_plume/plume_texture_cache.cpp
 * @brief       PlumeTextureCache implementation
 */

#include "plume_texture_cache.h"
#include "plume_command_processor.h"

#include <rex/logging.h>

namespace rex::graphics_plume {

static ::plume::RenderFormat GetPlumeFormat(rex::graphics::xenos::TextureFormat format) {
  switch (format) {
    case rex::graphics::xenos::TextureFormat::k_DXT1: return ::plume::RenderFormat::BC1_UNORM;
    case rex::graphics::xenos::TextureFormat::k_DXT2_3: return ::plume::RenderFormat::BC2_UNORM;
    case rex::graphics::xenos::TextureFormat::k_DXT4_5: return ::plume::RenderFormat::BC3_UNORM;
    case rex::graphics::xenos::TextureFormat::k_8_8_8_8: return ::plume::RenderFormat::R8G8B8A8_UNORM;
    case rex::graphics::xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16: return ::plume::RenderFormat::R16G16B16A16_FLOAT;
    default: return ::plume::RenderFormat::R8G8B8A8_UNORM;
  }
}

PlumeTextureCache::PlumeTexture::PlumeTexture(PlumeTextureCache& texture_cache, TextureKey key, ::plume::RenderDevice* device)
    : Texture(texture_cache, key) {
  ::plume::RenderTextureDesc desc;
  desc.dimension = key.dimension == rex::graphics::xenos::DataDimension::k3D 
                 ? ::plume::RenderTextureDimension::TEXTURE_3D 
                 : ::plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = key.GetWidth();
  desc.height = key.GetHeight();
  desc.depth = key.dimension == rex::graphics::xenos::DataDimension::k3D ? key.GetDepthOrArraySize() : 1;
  desc.arraySize = key.dimension == rex::graphics::xenos::DataDimension::k3D ? 1 : key.GetDepthOrArraySize();
  desc.mipLevels = key.mip_max_level > 0 ? (key.mip_max_level + 1) : 1;

  desc.format = GetPlumeFormat(key.format);

  desc.flags = ::plume::RenderTextureFlag::NONE;
  
  if (desc.width > 0 && desc.height > 0) {
    texture_ = device->createTexture(desc);
    
    if (texture_) {
      ::plume::RenderTextureViewDesc srv_desc;
      srv_desc.dimension = desc.dimension == ::plume::RenderTextureDimension::TEXTURE_3D 
                    ? ::plume::RenderTextureViewDimension::TEXTURE_3D 
                    : ::plume::RenderTextureViewDimension::TEXTURE_2D;
      srv_desc.format = desc.format;
      srv_desc.mipSlice = 0;
      srv_desc.mipLevels = desc.mipLevels;
      srv_desc.arrayIndex = 0;
      srv_desc.arraySize = desc.arraySize;
      srv_ = texture_->createTextureView(srv_desc);
    }
  }
}

PlumeTextureCache::PlumeTexture::~PlumeTexture() {
}

PlumeTextureCache::PlumeTextureCache(const rex::graphics::RegisterFile& register_file,
                                     rex::graphics::SharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y,
                                     PlumeCommandProcessor& command_processor,
                                     ::plume::RenderDevice* device)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      device_(device) {
  texture_uploader_ = std::make_unique<PlumeTextureUploader>(device_);
}

PlumeTextureCache::~PlumeTextureCache() {
  ClearCache();
}

bool PlumeTextureCache::Initialize() {
  if (!texture_uploader_->Initialize()) {
    return false;
  }
  return true;
}

void PlumeTextureCache::BeginSubmission(uint64_t new_submission_index) {
  TextureCache::BeginSubmission(new_submission_index);
}

void PlumeTextureCache::BeginFrame() {
  TextureCache::BeginFrame();
}

void PlumeTextureCache::EndFrame() {
}

void PlumeTextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);
}

::plume::RenderSampler* PlumeTextureCache::UseSampler(SamplerParameters params) {
  auto it = samplers_.find(params);
  if (it != samplers_.end()) {
    return it->second.get();
  }

  ::plume::RenderSamplerDesc desc;
  
  // TODO: Mapeamento detalhado dos filtros (Linear/Point) e Address modes (Clamp/Wrap)
  desc.magFilter = params.mag_linear ? ::plume::RenderFilter::LINEAR : ::plume::RenderFilter::NEAREST;
  desc.minFilter = params.min_linear ? ::plume::RenderFilter::LINEAR : ::plume::RenderFilter::NEAREST;
  desc.mipmapMode = params.mip_linear ? ::plume::RenderMipmapMode::LINEAR : ::plume::RenderMipmapMode::NEAREST;
  
  auto convert_clamp = [](rex::graphics::xenos::ClampMode clamp) {
    switch (clamp) {
      case rex::graphics::xenos::ClampMode::kRepeat: return ::plume::RenderTextureAddressMode::WRAP;
      case rex::graphics::xenos::ClampMode::kMirroredRepeat: return ::plume::RenderTextureAddressMode::MIRROR;
      case rex::graphics::xenos::ClampMode::kClampToEdge: return ::plume::RenderTextureAddressMode::CLAMP;
      case rex::graphics::xenos::ClampMode::kClampToHalfway: return ::plume::RenderTextureAddressMode::CLAMP; // Plume não tem halfway
      case rex::graphics::xenos::ClampMode::kClampToBorder: return ::plume::RenderTextureAddressMode::BORDER;
      default: return ::plume::RenderTextureAddressMode::WRAP;
    }
  };

  desc.addressU = convert_clamp(params.clamp_x);
  desc.addressV = convert_clamp(params.clamp_y);
  desc.addressW = convert_clamp(params.clamp_z);
  desc.maxAnisotropy = 1.0f;
  if (params.aniso_filter > rex::graphics::xenos::AnisoFilter::kDisabled) {
    desc.maxAnisotropy = static_cast<float>(1 << static_cast<uint32_t>(params.aniso_filter));
  }

  switch (static_cast<rex::graphics::xenos::BorderColor>(params.border_color)) {
    case rex::graphics::xenos::BorderColor::k_ABGR_Black:
      desc.borderColor = ::plume::RenderBorderColor::OPAQUE_BLACK;
      break;
    case rex::graphics::xenos::BorderColor::k_ABGR_White:
      desc.borderColor = ::plume::RenderBorderColor::OPAQUE_WHITE;
      break;
    default:
      desc.borderColor = ::plume::RenderBorderColor::TRANSPARENT_BLACK;
      break;
  }
  
  auto sampler = device_->createSampler(desc);
  auto* ptr = sampler.get();
  samplers_[params] = std::move(sampler);
  return ptr;
}

::plume::RenderTextureView* PlumeTextureCache::GetActiveBindingTextureView(uint32_t fetch_constant_index) {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) return nullptr;
  
  auto* plume_tex = static_cast<PlumeTexture*>(binding->texture);
  if (!plume_tex) return nullptr;
  
  return plume_tex->plume_srv();
}

PlumeTextureCache::PlumeTexture* PlumeTextureCache::GetActiveBindingPlumeTexture(uint32_t fetch_constant_index) {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) return nullptr;
  return static_cast<PlumeTexture*>(binding->texture);
}

PlumeTextureCache::SamplerParameters PlumeTextureCache::GetSamplerParameters(
    const rex::graphics::SpirvShader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;
  parameters.clamp_x = fetch.clamp_x;
  parameters.clamp_y = fetch.clamp_y;
  parameters.clamp_z = fetch.clamp_z;
  parameters.border_color = fetch.border_color;

  auto mag_filter = binding.mag_filter == rex::graphics::xenos::TextureFilter::kUseFetchConst
                        ? fetch.mag_filter
                        : binding.mag_filter;
  parameters.mag_linear = (mag_filter == rex::graphics::xenos::TextureFilter::kLinear);

  auto min_filter = binding.min_filter == rex::graphics::xenos::TextureFilter::kUseFetchConst
                        ? fetch.min_filter
                        : binding.min_filter;
  parameters.min_linear = (min_filter == rex::graphics::xenos::TextureFilter::kLinear);

  auto mip_filter = binding.mip_filter == rex::graphics::xenos::TextureFilter::kUseFetchConst
                        ? fetch.mip_filter
                        : binding.mip_filter;
  parameters.mip_linear = (mip_filter == rex::graphics::xenos::TextureFilter::kLinear);

  parameters.aniso_filter = binding.aniso_filter == rex::graphics::xenos::AnisoFilter::kUseFetchConst
                                ? fetch.aniso_filter
                                : binding.aniso_filter;
  parameters.mip_min_level = fetch.mip_min_level;
  parameters.mip_base_map = 0;
  return parameters;
}

uint32_t PlumeTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  // Retornamos R, G, B, A padrão por enquanto
  return (0) | (1 << 3) | (2 << 6) | (3 << 9);
}

std::unique_ptr<rex::graphics::TextureCache::Texture> PlumeTextureCache::CreateTexture(TextureKey key) {
  return std::make_unique<PlumeTexture>(*this, key, device_);
}

bool PlumeTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base, bool load_mips) {
  auto& plume_tex = static_cast<PlumeTexture&>(texture);
  if (!plume_tex.plume_texture()) return false;

  ::plume::RenderCommandList* command_list = command_processor_.GetActiveCommandList();
  if (!command_list) return false;

  const auto& key = texture.key();
  const uint8_t* guest_memory = static_cast<PlumeSharedMemory&>(shared_memory()).GetPointer(key.base_page << 12);
  bool has_mips = key.mip_max_level > 0;
  
  ::plume::RenderFormat dest_format = GetPlumeFormat(key.format);
  return texture_uploader_->UploadTexture(key.dimension, key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(), key.format, dest_format, has_mips, guest_memory, plume_tex.plume_texture(), command_list);
}

}  // namespace rex::graphics_plume
