/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Backend-agnostic front-end for the post_process_shader_path custom post-process
 * effect (GuestOutputPaintEffect::kCustomShader), shared by the Vulkan and
 * D3D12 presenters.
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::ui {

// Compiles a user-provided post-process fragment shader (either the original
// bare-fragment format, or a single-pass RetroArch-style `.slang` shader
// using `#pragma stage vertex`/`#pragma stage fragment`) into SPIR-V for the
// Vulkan backend, and (optionally) cross-compiles that SPIR-V into HLSL for
// the D3D12 backend.
//
// Only the fragment stage is executed - both formats are run as a
// single-pass effect on top of the existing fixed guest-output-painting
// vertex shader (a full-screen triangle strip), so any custom vertex stage in
// a `.slang` shader is not compiled or run. This matches the vast majority of
// real single-pass RetroArch shaders, whose vertex stage is a plain
// pass-through. See presenter.h `CustomShaderConstants` for the fixed
// uniform layout supplied to the shader.
class CustomShader {
 public:
  struct CompileResult {
    bool success = false;
    // Populated when success is false.
    std::string error;

    // SPIR-V for the fragment stage, consumed directly by the Vulkan
    // backend. Valid only when success is true.
    std::vector<uint32_t> spirv;

    // HLSL (shader model 5.1) source for the fragment stage, produced by
    // cross-compiling `spirv` via SPIRV-Cross. Only populated when
    // `compile_hlsl` was requested and compilation succeeded; the entry
    // point is "main". Resource bindings are forced to Texture2D t0,
    // SamplerState s0, cbuffer b0, matching the D3D12 custom-shader root
    // signature.
    std::string hlsl_ps;
  };

  // Loads glsl_path (a bare fragment shader or a RetroArch-style `.slang`
  // shader) from disk, wraps it with the fixed preamble declaring the
  // xe_source/xe_uv (and, for `.slang` shaders, RetroArch-compatible
  // Source/vTexCoord/SourceSize/OutputSize/FrameCount) aliases over
  // CustomShaderConstants, and compiles it to SPIR-V. If compile_hlsl is
  // true, also cross-compiles the result to HLSL for D3D12.
  static CompileResult Compile(const std::string& glsl_path, bool compile_hlsl);
};

}  // namespace rex::ui
