#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <optional>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file),
        memory_(memory),
        shader_interpreter_(register_file, memory) {}

  // The shader must have its ucode analyzed.
  // min_y_out, if given, receives a row no lower than the first one the draw can touch (0 if
  // that isn't known).
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader, uint32_t* min_y_out = nullptr);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader,
                        uint32_t* min_y_out = nullptr);

  // The positions (x, y, z, w) exported for vertices 0... of an auto-indexed draw, by running the
  // vertex shader on the CPU. False if they can't be known exactly: the shader can't be
  // interpreted, or a vertex is killed or doesn't export a whole position.
  bool GetAutoIndexedVertexPositions(const Shader& vertex_shader, uint32_t vertex_count,
                                     float (*positions_out)[4]);

 private:
  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_mask = 0;
      vertex_kill = 0;
    }

    float position[4] = {};
    uint32_t position_mask = 0;
    uint32_t vertex_kill = 0;
  };

  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  const RegisterFile& register_file_;
  const memory::Memory& memory_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace rex::graphics
