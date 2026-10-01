#include "plume_texture_uploader.h"
#include "plume_texture_cache.h"
#include "rex/graphics/pipeline/texture/info.h"
#include "rex/graphics/pipeline/texture/conversion.h"
#include "rex/graphics/pipeline/texture/util.h"
#include "rex/graphics/shared_memory.h"
#include "rex/logging.h"

// SPIR-V Headers para os Shaders de Descompressão
namespace shaders {
#include "../graphics/shaders/vulkan_spirv/texture_load_64bpb_cs.h"
#include "../graphics/shaders/vulkan_spirv/texture_load_128bpb_cs.h"
#include "../graphics/shaders/vulkan_spirv/texture_load_32bpb_cs.h"
}

using namespace rex::graphics;

PlumeTextureUploader::PlumeTextureUploader(::plume::RenderDevice* device) : device_(device) {
}

PlumeTextureUploader::~PlumeTextureUploader() {
  Shutdown();
}

bool PlumeTextureUploader::Initialize() {
  std::lock_guard<std::mutex> lock(upload_mutex_);

  REXLOG_INFO("PlumeTextureUploader: Initialize ENTER (lazy upload path)");

  // The current uploader path performs CPU untile + GPU copyTextureRegion.
  // The old implementation also created a compute pipeline layout here, but
  // never used those compute pipelines. That extra pipeline creation can block
  // on some Vulkan drivers during CommandProcessor initialization. Xerenge's
  // Plume path likewise keeps its upload resources to staging/copy resources.
  current_staging_buffer_size_ = 16 * 1024 * 1024;
  REXLOG_INFO("PlumeTextureUploader: creating 16 MiB staging buffer");
  staging_buffer_ = device_->createBuffer(
      ::plume::RenderBufferDesc::UploadBuffer(current_staging_buffer_size_));
  if (!staging_buffer_) {
    REXLOG_ERROR("PlumeTextureUploader: failed to create 16 MiB staging buffer");
    return false;
  }

  REXLOG_INFO("PlumeTextureUploader: staging buffer created; mapping buffer");
  staging_mapped_ptr_ = staging_buffer_->map();
  if (!staging_mapped_ptr_) {
    REXLOG_ERROR("PlumeTextureUploader: failed to map staging buffer");
    staging_buffer_.reset();
    return false;
  }

  REXLOG_INFO("PlumeTextureUploader: staging buffer mapped successfully");
  REXLOG_INFO("PlumeTextureUploader initialized (CPU untile + GPU copy, 16 MiB staging)");
  return true;
}
void PlumeTextureUploader::Shutdown() {
  std::lock_guard<std::mutex> lock(upload_mutex_);
  if (staging_mapped_ptr_ && staging_buffer_) {
    staging_buffer_->unmap();
    staging_mapped_ptr_ = nullptr;
  }
  staging_buffer_.reset();
  pipelines_.clear();
  pipeline_layout_.reset();
}

bool PlumeTextureUploader::UploadTexture(rex::graphics::xenos::DataDimension dimension,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t depth_or_array_size,
                                         rex::graphics::xenos::TextureFormat format,
                                         ::plume::RenderFormat dest_format,
                                         bool has_mips,
                                         const uint8_t* guest_memory,
                                         ::plume::RenderTexture* destination_texture,
                                         ::plume::RenderCommandList* command_list) {
  std::lock_guard<std::mutex> lock(upload_mutex_);
  
  if (!guest_memory || !destination_texture || !command_list) {
    return false;
  }

  // Determinar o formato e tamanho
  const texture_util::TextureGuestLayout guest_layout = texture_util::GetGuestTextureLayout(
      dimension, (width + 31) / 32, width, height, depth_or_array_size, true, format, false, true, has_mips ? 1 : 0);
      
  const FormatInfo* format_info = FormatInfo::Get(format);
  if (!format_info) return false;

  bool is_ctx1 = (format == rex::graphics::xenos::TextureFormat::k_CTX1);
  const FormatInfo* output_format_info = is_ctx1 ? FormatInfo::Get(rex::graphics::xenos::TextureFormat::k_8_8) : format_info;

  uint32_t block_w = format_info->block_width;
  uint32_t block_h = format_info->block_height;
  uint32_t blocks_x = (width + block_w - 1) / block_w;
  uint32_t blocks_y = (height + block_h - 1) / block_h;

  size_t required_size = is_ctx1 ? (static_cast<size_t>(width) * height * 2) : guest_layout.base.level_data_extent_bytes;
  if (required_size == 0) {
    required_size = static_cast<size_t>(blocks_x) * blocks_y * format_info->bytes_per_block();
  }

  // Aumentar o Staging Buffer se necessário
  if (required_size > current_staging_buffer_size_) {
    if (staging_mapped_ptr_ && staging_buffer_) {
      staging_buffer_->unmap();
      staging_mapped_ptr_ = nullptr;
    }
    current_staging_buffer_size_ = required_size + (4 * 1024 * 1024); // Cresce com folga
    staging_buffer_ = device_->createBuffer(::plume::RenderBufferDesc::UploadBuffer(current_staging_buffer_size_));
    if (staging_buffer_) {
      staging_mapped_ptr_ = staging_buffer_->map();
    }
  }

  void* host_ptr = staging_mapped_ptr_;
  if (!host_ptr && staging_buffer_) {
    staging_mapped_ptr_ = staging_buffer_->map();
    host_ptr = staging_mapped_ptr_;
  }
  if (!host_ptr) {
    REXLOG_ERROR("Falha ao mapear Plume Staging Buffer!");
    return false;
  }

  // Executa o Untile acelerado diretamente para o buffer persistente mapeado
  texture_conversion::UntileInfo untile_info;
  untile_info.input_format_info = format_info;
  untile_info.output_format_info = output_format_info;
  untile_info.width = blocks_x;
  untile_info.height = blocks_y;
  untile_info.offset_x = 0;
  untile_info.offset_y = 0;
  untile_info.input_pitch = guest_layout.base.x_extent_blocks;
  untile_info.output_pitch = is_ctx1 ? width : blocks_x;
  untile_info.copy_callback = [](void* dest, const void* src, size_t size) {
    std::memcpy(dest, src, size);
  };

  texture_conversion::Untile(static_cast<uint8_t*>(host_ptr), guest_memory, &untile_info);

  // Enviar comando para GPU copiar o staging buffer descompactado para a Textura real.
  ::plume::RenderTextureCopyLocation src_loc = ::plume::RenderTextureCopyLocation::PlacedFootprint(
      staging_buffer_.get(), dest_format, width, height, 1, width);
      
  ::plume::RenderTextureCopyLocation dst_loc = ::plume::RenderTextureCopyLocation::Subresource(destination_texture, 0, 0);

  // Barreiras de memoria
  ::plume::RenderTextureBarrier tex_barrier(destination_texture, ::plume::RenderTextureLayout::COPY_DEST);
  command_list->barriers(::plume::RenderBarrierStage::NONE, tex_barrier);

  command_list->copyTextureRegion(dst_loc, src_loc);

  tex_barrier.layout = ::plume::RenderTextureLayout::SHADER_READ;
  command_list->barriers(::plume::RenderBarrierStage::NONE, tex_barrier);

  return true;
}
