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

  // 1. Criar RenderPipelineLayout para os Compute Shaders de Textura do Xenia
  // O Xenia usa:
  // - Binding 0: Buffer de Origem (Memória Raw do Xbox / Staging Buffer)
  // - Binding 1: Imagem de Destino (UAV / Storage Image)
  // E Push Constants para LoadConstants.

  ::plume::RenderPushConstantRange push_range;
  push_range.offset = 0;
  push_range.size = 40; // sizeof(TextureCache::LoadConstants), is protected

  ::plume::RenderDescriptorRange ranges[2];
  
  // Staging Buffer (Storage Buffer de Leitura)
  ranges[0] = ::plume::RenderDescriptorRange(::plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  
  // Destination Texture (UAV / Storage Texture de Escrita)
  ranges[1] = ::plume::RenderDescriptorRange(::plume::RenderDescriptorRangeType::READ_WRITE_TEXTURE, 1, 1);

  ::plume::RenderDescriptorSetDesc set_desc(ranges, 2);

  ::plume::RenderPipelineLayoutDesc layout_desc(&push_range, 1, &set_desc, 1, false, false);
  pipeline_layout_ = device_->createPipelineLayout(layout_desc);

  if (!pipeline_layout_) {
    REXLOG_ERROR("Falha ao criar o Pipeline Layout para o PlumeTextureUploader.");
    return false;
  }

  // Helper para compilar shader e pipeline
  auto create_pipeline = [&](const uint32_t* spirv_code, size_t spirv_size) -> std::unique_ptr<::plume::RenderPipeline> {
    auto shader = device_->createShader(spirv_code, spirv_size, "main", ::plume::RenderShaderFormat::SPIRV);
    if (!shader) return nullptr;

    ::plume::RenderComputePipelineDesc compute_desc(pipeline_layout_.get(), shader.get(), 16, 16, 1);
    return device_->createComputePipeline(compute_desc);
  };

  // 2. Criar as Pipelines de Compute para os Formatos Principais (DXT1, DXT5, RGBA8)
  pipelines_[3] = // kLoadShaderIndex64bpb
      create_pipeline(shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs));
      
  pipelines_[4] = // kLoadShaderIndex128bpb
      create_pipeline(shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs));
      
  pipelines_[2] = // kLoadShaderIndex32bpb
      create_pipeline(shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs));

  // Staging buffer inicial (16MB para aguentar quase qualquer textura)
  current_staging_buffer_size_ = 16 * 1024 * 1024;
  staging_buffer_ = device_->createBuffer(::plume::RenderBufferDesc::UploadBuffer(current_staging_buffer_size_));

  REXLOG_INFO("PlumeTextureUploader inicializado (Compute Pipelines: 3 prontas, Staging: 16MB)");
  return true;
}

void PlumeTextureUploader::Shutdown() {
  std::lock_guard<std::mutex> lock(upload_mutex_);
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

  size_t required_size = guest_layout.base.level_data_extent_bytes; // Tamanho máximo estimado

  // Aumentar o Staging Buffer se necessário
  if (required_size > current_staging_buffer_size_) {
    current_staging_buffer_size_ = required_size + (4 * 1024 * 1024); // Cresce com folga
    staging_buffer_ = device_->createBuffer(::plume::RenderBufferDesc::UploadBuffer(current_staging_buffer_size_));
  }

  // Map Memory
  void* host_ptr = staging_buffer_->map();
  if (!host_ptr) {
    REXLOG_ERROR("Falha ao mapear Plume Staging Buffer!");
    return false;
  }

  // Por agora, para garantir que TUDO renderiza perfeitamente no teste do Android sem falta de shader:
  // Fazemos Untile via CPU (Rápido) e usamos a via nativa de Upload do Plume (CopyBufferToTexture)
  // TODO: Habilitar o path GPU quando validarmos o binding de descriptors do Plume.
  
  texture_conversion::UntileInfo untile_info;
  untile_info.input_format_info = format_info;
  untile_info.output_format_info = format_info;
  untile_info.width = width;
  untile_info.height = height;
  untile_info.offset_x = 0;
  untile_info.offset_y = 0;
  untile_info.input_pitch = guest_layout.base.x_extent_blocks;
  untile_info.output_pitch = guest_layout.base.x_extent_blocks; // Manter pitch
  untile_info.copy_callback = [](void* dest, const void* src, size_t size) {
    std::memcpy(dest, src, size);
  };

  texture_conversion::Untile(static_cast<uint8_t*>(host_ptr), guest_memory, &untile_info);

  staging_buffer_->unmap();

  // Enviar comando para GPU copiar o staging buffer descompactado para a Textura real.
  ::plume::RenderTextureCopyLocation src_loc = ::plume::RenderTextureCopyLocation::PlacedFootprint(
      staging_buffer_.get(), dest_format, width, height, 1, untile_info.output_pitch * format_info->bytes_per_block());
      
  ::plume::RenderTextureCopyLocation dst_loc = ::plume::RenderTextureCopyLocation::Subresource(destination_texture, 0, 0);

  // Barreiras de memoria
  ::plume::RenderTextureBarrier tex_barrier(destination_texture, ::plume::RenderTextureLayout::COPY_DEST);
  command_list->barriers(::plume::RenderBarrierStage::NONE, tex_barrier);

  command_list->copyTextureRegion(dst_loc, src_loc);

  tex_barrier.layout = ::plume::RenderTextureLayout::SHADER_READ;
  command_list->barriers(::plume::RenderBarrierStage::NONE, tex_barrier);

  return true;
}
