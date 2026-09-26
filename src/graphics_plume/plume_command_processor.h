/**
 * @file        graphics_plume/plume_command_processor.h
 * @brief       PlumeCommandProcessor using Plume RenderInterface & Pm4PlumeTranspiler
 */

#pragma once

#include <memory>
#include <vector>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pm4_plume_transpiler.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <plume_render_interface.h>

#include "plume_texture_cache.h"
#include "plume_render_target_cache.h"
namespace rex::graphics_plume {

class PlumeGraphicsSystem;

class PlumeSharedMemory final : public rex::graphics::SharedMemory {
 public:
  PlumeSharedMemory(rex::memory::Memory& memory)
      : rex::graphics::SharedMemory(memory) {}
  
  void SetSharedMemoryBuffer(::plume::RenderBuffer* buffer, size_t buffer_size) {
    gpu_buffer_ = buffer;
    gpu_buffer_size_ = buffer_size;
  }

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override {
    if (gpu_buffer_) {
      void* mapped = gpu_buffer_->map();
      if (mapped) {
        for (const auto& range : upload_page_ranges) {
          uint32_t offset = range.first << 12; // 4096 bytes por pagina
          uint32_t length = range.second << 12;
          if (static_cast<size_t>(offset) + length <= gpu_buffer_size_) {
            const uint8_t* src = memory().TranslatePhysical<const uint8_t*>(offset);
            if (src) {
              std::memcpy(static_cast<uint8_t*>(mapped) + offset, src, length);
            }
          }
          MakeRangeValid(range.first, range.second, false);
        }
        gpu_buffer_->unmap();
        return true;
      }
    }

    for (const auto& range : upload_page_ranges) {
      MakeRangeValid(range.first, range.second, false);
    }
    return true;
  }

  uint8_t* GetPointer(uint32_t address) {
    return memory().TranslatePhysical(address);
  }

 private:
  ::plume::RenderBuffer* gpu_buffer_ = nullptr;
  size_t gpu_buffer_size_ = 0;
};

class PlumeCommandProcessor final : public rex::graphics::CommandProcessor {
 public:
  PlumeCommandProcessor(PlumeGraphicsSystem* graphics_system,
                         rex::system::KernelState* kernel_state,
                         ::plume::RenderDevice* device);
  ~PlumeCommandProcessor() override;

  bool Initialize() override;
  void Shutdown() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  ::plume::RenderCommandList* GetActiveCommandList() const { return frames_[current_frame_index_].cmd_list.get(); }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address,
                                    const uint32_t* host_address,
                                    uint32_t dword_count) override;

  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;

  bool IssueCopy() override;

 private:
  PlumeGraphicsSystem* plume_graphics_system_ = nullptr;
  ::plume::RenderDevice* plume_device_ = nullptr;
  std::unique_ptr<::plume::RenderCommandQueue> plume_queue_;

  struct PlumePipeline {
    ::plume::RenderPipeline* pipeline = nullptr;
    ::plume::RenderPipelineLayout* layout = nullptr;
  };

  struct PipelineEntry {
    std::unique_ptr<::plume::RenderPipeline> pipeline;
    ::plume::RenderPipelineLayout* layout = nullptr;
  };

  // Synchronization and Multiple Frames in Flight
  static constexpr uint32_t kMaxFramesInFlight = 2;
  struct PlumeFrameContext {
    std::unique_ptr<::plume::RenderCommandList> cmd_list;
    std::unique_ptr<::plume::RenderCommandFence> fence;
    bool in_flight = false;
    
    struct FrameGarbage {
      std::unique_ptr<::plume::RenderDescriptorSet> descriptor_set;     // Set 1 (constants)
      std::unique_ptr<::plume::RenderDescriptorSet> vs_descriptor_set;  // Set 2 (vs textures & samplers)
      std::unique_ptr<::plume::RenderDescriptorSet> ps_descriptor_set;  // Set 3 (ps textures & samplers)
      std::unique_ptr<::plume::RenderBuffer> sys_buf;
      std::unique_ptr<::plume::RenderBuffer> vs_float_buf;
      std::unique_ptr<::plume::RenderBuffer> ps_float_buf;
      std::unique_ptr<::plume::RenderBuffer> bool_buf;
      std::unique_ptr<::plume::RenderBuffer> fetch_buf;
    };
    std::vector<FrameGarbage> garbage;
  };
  PlumeFrameContext frames_[kMaxFramesInFlight];
  uint32_t current_frame_index_ = 0;
  rex::graphics::Pm4PlumeTranspiler transpiler_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> loaded_shaders_;
  std::unordered_map<uint64_t, PipelineEntry> graphics_pipelines_;
  std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderPipelineLayout>> pipeline_layouts_;

  PlumePipeline GetOrCreateGraphicsPipeline(::plume::RenderPrimitiveTopology topology, ::plume::RenderFormat color_format);
  ::plume::RenderPipelineLayout* GetOrCreatePipelineLayout(uint32_t t_vs, uint32_t s_vs, uint32_t t_ps, uint32_t s_ps);
  bool cmd_list_open_ = false;  // true entre begin() e end() do command list

  // Set 0: Shared Memory
  std::unique_ptr<::plume::RenderBuffer> shared_memory_buf_;
  std::unique_ptr<::plume::RenderDescriptorSet> shared_memory_descriptor_set_;

  // Fallbacks for unbound textures / samplers
  std::unique_ptr<::plume::RenderTexture> dummy_texture_;
  std::unique_ptr<::plume::RenderTextureView> dummy_texture_view_;
  std::unique_ptr<::plume::RenderSampler> default_sampler_;

  // Uniform buffer management for Phase F
  rex::graphics::SpirvShaderTranslator::SystemConstants system_constants_ = {};
  
  std::unique_ptr<PlumeSharedMemory> shared_memory_;
  std::unique_ptr<PlumeTextureCache> texture_cache_;
  std::unique_ptr<PlumeRenderTargetCache> render_target_cache_;
};

}  // namespace rex::graphics_plume
