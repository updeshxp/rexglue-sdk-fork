#ifndef REX_GRAPHICS_PLUME_TEXTURE_UPLOADER_H_
#define REX_GRAPHICS_PLUME_TEXTURE_UPLOADER_H_

#include "rex/graphics/pipeline/texture/cache.h"
#include "plume_render_interface.h"
#include "plume_render_interface_types.h"

#include <memory>
#include <unordered_map>
#include <mutex>

class PlumeTextureUploader {
 public:
  PlumeTextureUploader(::plume::RenderDevice* device);
  ~PlumeTextureUploader();

  // Inicializa todos os pipelines de Compute
  bool Initialize();

  // Limpa recursos
  void Shutdown();

  // Executa o upload (com ou sem untile) de uma textura para a VRAM
  bool UploadTexture(rex::graphics::xenos::DataDimension dimension,
                     uint32_t width,
                     uint32_t height,
                     uint32_t depth_or_array_size,
                     rex::graphics::xenos::TextureFormat format,
                     ::plume::RenderFormat dest_format,
                     bool has_mips,
                     const uint8_t* guest_memory,
                     ::plume::RenderTexture* destination_texture,
                     ::plume::RenderCommandList* command_list);

 private:
  ::plume::RenderDevice* device_;

  // Buffer de Staging para enviar dados da CPU para a GPU
  std::unique_ptr<::plume::RenderBuffer> staging_buffer_;
  size_t current_staging_buffer_size_ = 0;

  // Pipeline layout (Descriptor Sets / Push Constants)
  std::unique_ptr<::plume::RenderPipelineLayout> pipeline_layout_;
  
  // Mapeamento dos Compute Pipelines por formato de textura (DXT1, DXT5, etc)
  std::unordered_map<uint32_t, std::unique_ptr<::plume::RenderPipeline>> pipelines_;

  // Garantir que a Staging Buffer e os pipelines sejam usados de forma segura (Thread-Safe)
  std::mutex upload_mutex_;
};

#endif  // REX_GRAPHICS_PLUME_TEXTURE_UPLOADER_H_
