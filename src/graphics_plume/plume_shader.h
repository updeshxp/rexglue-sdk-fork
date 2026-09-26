/**
 * @file        graphics_plume/plume_shader.h
 * @brief       PlumeShader implementation of rex::graphics::Shader
 */

#pragma once

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <plume_render_interface.h>

namespace rex::graphics_plume {

class PlumeShader : public rex::graphics::SpirvShader {
 public:
  PlumeShader(::plume::RenderDevice* device,
              rex::graphics::xenos::ShaderType shader_type,
              uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords,
              size_t ucode_dword_count,
              std::endian ucode_source_endian)
      : rex::graphics::SpirvShader(shader_type, ucode_data_hash, ucode_dwords,
                                  ucode_dword_count, ucode_source_endian),
        plume_device_(device) {}

  ~PlumeShader() override = default;

  ::plume::RenderShader* GetOrCreatePlumeShader(uint64_t modification = 0) {
    auto it = plume_shaders_.find(modification);
    if (it != plume_shaders_.end()) {
      return it->second.get();
    }

    if (!plume_device_) {
      return nullptr;
    }

    auto* translation = GetOrCreateTranslation(modification);
    if (!translation) {
      return nullptr;
    }

    if (!is_ucode_analyzed()) {
      rex::string::StringBuffer disasm_buffer;
      AnalyzeUcode(disasm_buffer);
    }

    if (!translation->is_valid()) {
      rex::graphics::SpirvShaderTranslator::Features features(false);
      features.spirv_version = spv::Spv_1_3;
      features.max_storage_buffer_range = UINT32_MAX;
      features.clip_distance = true;
      features.cull_distance = true;
      features.full_draw_index_uint32 = true;
      rex::graphics::SpirvShaderTranslator translator(features, false, false, false);
      if (!translator.TranslateAnalyzedShader(*translation)) {
        return nullptr;
      }
    }

    const auto& spirv_bytes = translation->translated_binary();
    if (spirv_bytes.empty()) {
      return nullptr;
    }

    auto render_shader = plume_device_->createShader(
        spirv_bytes.data(),
        spirv_bytes.size(),
        "main",
        ::plume::RenderShaderFormat::SPIRV);

    if (!render_shader) {
      return nullptr;
    }

    auto* ptr = render_shader.get();
    plume_shaders_[modification] = std::move(render_shader);
    return ptr;
  }

 private:
  ::plume::RenderDevice* plume_device_ = nullptr;
  std::unordered_map<uint64_t, std::unique_ptr<::plume::RenderShader>> plume_shaders_;
};

}  // namespace rex::graphics_plume
