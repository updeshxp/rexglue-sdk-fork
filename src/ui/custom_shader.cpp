/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * See include/rex/ui/custom_shader.h.
 ******************************************************************************
 */

#include <rex/ui/custom_shader.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_hlsl.hpp>

#include <rex/logging.h>

namespace rex::ui {

namespace {

// glslang default built-in resource limits, matching the ones used
// throughout the codebase (graphics/vulkan/command_processor.cpp) for
// runtime GLSL compilation.
constexpr TBuiltInResource kGlslangDefaultResource = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    }};

// Fixed preamble matching the guest output image binding shape used by the
// bilinear pass: a sampled image at binding 0 and a separate sampler at
// binding 1, both in set 0. UV is derived from gl_FragCoord and the
// CustomShaderConstants push constant block (at byte offset 16, after the
// vertex stage's rectangle constants - see BilinearConstants in
// presenter.h), rather than as an interpolated input, since only the fixed
// full-screen-triangle-strip vertex shader is used (no custom vertex stage is
// compiled or run).
//
// RetroArch `.slang`-style aliases (Source, vTexCoord, SourceSize,
// OutputSize) are provided for compatibility with real single-pass RetroArch
// fragment shaders. FrameCount/FrameDirection are fixed (no per-frame counter
// is currently threaded through the paint flow), so animated shaders driven
// purely by FrameCount will not animate - a known limitation.
constexpr char kGlslPreamble[] = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D xe_source_texture;
layout(set = 0, binding = 1) uniform sampler xe_source_sampler;
#define xe_source sampler2D(xe_source_texture, xe_source_sampler)
layout(push_constant) uniform XeCustomShaderConstants {
  layout(offset = 16) ivec2 xe_output_offset;
  layout(offset = 24) vec2 xe_output_size_inv;
  layout(offset = 32) vec2 xe_source_size;
  layout(offset = 40) vec2 xe_source_size_inv;
};
#define xe_uv ((gl_FragCoord.xy - vec2(xe_output_offset)) * xe_output_size_inv)
layout(location = 0) out vec4 xe_frag_color;

// RetroArch-compatible aliases for single-pass `.slang` shaders.
#define Source xe_source
#define vTexCoord xe_uv
#define SourceSize vec4(xe_source_size, xe_source_size_inv)
#define OutputSize vec4(1.0 / xe_output_size_inv, xe_output_size_inv)
#define FrameCount 0u
#define FrameDirection 1.0
#define FragColor xe_frag_color
)";

bool CompileGlslToSpirv(const std::string& source, std::vector<uint32_t>& spirv_out,
                        std::string& error_out) {
  static std::once_flag glslang_initialize_once;
  std::call_once(glslang_initialize_once, []() { glslang::InitializeProcess(); });

  const char* source_c_str = source.c_str();
  glslang::TShader shader(EShLangFragment);
  shader.setStrings(&source_c_str, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultResource, 450, false, messages)) {
    error_out = "glslang shader parse failed";
    if (const char* shader_log = shader.getInfoLog();
        shader_log != nullptr && shader_log[0] != '\0') {
      error_out += ": ";
      error_out += shader_log;
    }
    return false;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error_out = "glslang program link failed";
    if (const char* program_log = program.getInfoLog();
        program_log != nullptr && program_log[0] != '\0') {
      error_out += ": ";
      error_out += program_log;
    }
    return false;
  }

  const glslang::TIntermediate* intermediate = program.getIntermediate(EShLangFragment);
  if (intermediate == nullptr) {
    error_out = "glslang produced no fragment stage intermediate";
    return false;
  }

  glslang::SpvOptions spv_options = {};
  spv_options.disableOptimizer = true;
  spv_options.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &spv_options);
  if (spirv_out.empty()) {
    error_out = "glslang produced empty SPIR-V";
    return false;
  }
  return true;
}

// Cross-compiles fragment-stage SPIR-V to HLSL (shader model 5.1), forcing
// deterministic resource bindings matching the D3D12 custom-shader root
// signature: the source texture at t0, the sampler at s0, and the uniform
// block (CustomShaderConstants, bound as D3D12 32-bit root constants) at b0.
bool CrossCompileSpirvToHlsl(const std::vector<uint32_t>& spirv, std::string& hlsl_out,
                             std::string& error_out) {
  try {
    spirv_cross::CompilerHLSL compiler(spirv);

    spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    for (const spirv_cross::Resource& image : resources.separate_images) {
      compiler.set_decoration(image.id, spv::DecorationDescriptorSet, 0);
      compiler.set_decoration(image.id, spv::DecorationBinding, 0);
    }
    for (const spirv_cross::Resource& sampler : resources.separate_samplers) {
      compiler.set_decoration(sampler.id, spv::DecorationDescriptorSet, 0);
      compiler.set_decoration(sampler.id, spv::DecorationBinding, 0);
    }
    for (const spirv_cross::Resource& push_constant : resources.push_constant_buffers) {
      compiler.set_decoration(push_constant.id, spv::DecorationDescriptorSet, 0);
      compiler.set_decoration(push_constant.id, spv::DecorationBinding, 0);

      // The GLSL preamble declares this block's members at fixed absolute
      // offsets (16, 24, ...) matching Vulkan's single push-constant range
      // shared with the vertex stage (whose rectangle constants occupy bytes
      // 0-16). D3D12 32-bit root constants have no such shared range - the
      // effect-constants root parameter is its own buffer starting at byte 0
      // - so rebase the member offsets to start at 0 here, otherwise
      // SPIRV-Cross preserves the absolute offsets in the emitted cbuffer and
      // every field ends up misaligned with the CustomShaderConstants data
      // uploaded via SetGraphicsRoot32BitConstants.
      const spirv_cross::SPIRType& block_type = compiler.get_type(push_constant.base_type_id);
      uint32_t member_count = uint32_t(block_type.member_types.size());
      uint32_t min_offset = UINT32_MAX;
      for (uint32_t i = 0; i < member_count; ++i) {
        min_offset = std::min(min_offset, compiler.get_member_decoration(push_constant.base_type_id,
                                                                         i, spv::DecorationOffset));
      }
      if (min_offset != 0 && min_offset != UINT32_MAX) {
        for (uint32_t i = 0; i < member_count; ++i) {
          uint32_t offset =
              compiler.get_member_decoration(push_constant.base_type_id, i, spv::DecorationOffset);
          compiler.set_member_decoration(push_constant.base_type_id, i, spv::DecorationOffset,
                                         offset - min_offset);
        }
      }
    }

    spirv_cross::CompilerHLSL::Options hlsl_options;
    hlsl_options.shader_model = 51;
    compiler.set_hlsl_options(hlsl_options);

    hlsl_out = compiler.compile();
    return true;
  } catch (const std::exception& e) {
    error_out = std::string("SPIRV-Cross HLSL cross-compilation failed: ") + e.what();
    return false;
  }
}

// Finds the start of the next line whose first non-whitespace characters
// match `marker`, starting the search at or after `from`. This only matches
// pragma directives that actually begin a line - unlike a raw substring
// search, it will not be fooled by the marker text appearing inside a `//`
// comment or elsewhere mid-line. Returns std::string::npos if not found.
size_t FindPragmaLine(const std::string& source, const char* marker, size_t from) {
  size_t line_start = from;
  while (line_start <= source.size()) {
    // Skip leading whitespace on this line before comparing.
    size_t content_start = source.find_first_not_of(" \t", line_start);
    if (content_start != std::string::npos &&
        source.compare(content_start, std::strlen(marker), marker) == 0 &&
        // Only a match at the very start of the source, or immediately after
        // a newline, counts as "starting a line".
        (line_start == 0 || (line_start > 0 && source[line_start - 1] == '\n'))) {
      return line_start;
    }
    size_t next_newline = source.find('\n', line_start);
    if (next_newline == std::string::npos) {
      break;
    }
    line_start = next_newline + 1;
  }
  return std::string::npos;
}

// Splits a RetroArch-style `.slang` shader into its fragment stage body
// (everything between the first `#pragma stage fragment` line and the next
// `#pragma stage` line, or end of file). Code preceding the first
// `#pragma stage` (helper declarations, `#pragma parameter`/`#pragma name`,
// etc.) is treated as shared preamble and included verbatim before the
// fragment body; the vertex stage, if present, is not extracted or compiled
// (see CustomShader class comment).
//
// Returns false if no `#pragma stage fragment` marker is found (as its own
// line, not merely appearing as text e.g. inside a comment).
bool ExtractSlangFragmentStage(const std::string& source, std::string& fragment_source_out) {
  constexpr char kStagePragma[] = "#pragma stage";
  constexpr char kFragmentStagePragma[] = "#pragma stage fragment";

  size_t fragment_stage_pos = FindPragmaLine(source, kFragmentStagePragma, 0);
  if (fragment_stage_pos == std::string::npos) {
    return false;
  }

  // Shared preamble: everything before the first `#pragma stage` marker of
  // any kind.
  size_t first_stage_pos = FindPragmaLine(source, kStagePragma, 0);
  std::string shared_preamble = source.substr(0, first_stage_pos);

  // Body: from just after the `#pragma stage fragment` line to the next
  // `#pragma stage` marker (the vertex stage, if it follows), or EOF.
  size_t body_start = source.find('\n', fragment_stage_pos);
  body_start = (body_start == std::string::npos) ? source.size() : body_start + 1;
  size_t next_stage_pos = FindPragmaLine(source, kStagePragma, body_start);
  std::string body = source.substr(body_start, (next_stage_pos == std::string::npos)
                                                   ? std::string::npos
                                                   : next_stage_pos - body_start);

  fragment_source_out = shared_preamble + body;
  return true;
}

}  // namespace

CustomShader::CompileResult CustomShader::Compile(const std::string& glsl_path, bool compile_hlsl) {
  CompileResult result;

  std::ifstream file(glsl_path, std::ios::in | std::ios::binary);
  if (!file) {
    result.error = "Failed to open custom shader file '" + glsl_path + "'";
    return result;
  }
  std::ostringstream file_stream;
  file_stream << file.rdbuf();
  std::string source = file_stream.str();

  std::string fragment_body;
  if (!ExtractSlangFragmentStage(source, fragment_body)) {
    // Not a RetroArch `.slang` shader (no `#pragma stage` markers) - treat
    // the whole file as the legacy bare-fragment format.
    fragment_body = std::move(source);
  }

  std::string full_source = kGlslPreamble;
  full_source += fragment_body;

  if (!CompileGlslToSpirv(full_source, result.spirv, result.error)) {
    REXLOG_ERROR("CustomShader: Failed to compile '{}': {}", glsl_path, result.error);
    return result;
  }

  if (compile_hlsl) {
    if (!CrossCompileSpirvToHlsl(result.spirv, result.hlsl_ps, result.error)) {
      REXLOG_ERROR("CustomShader: Failed to cross-compile '{}' to HLSL: {}", glsl_path,
                   result.error);
      return result;
    }
  }

  result.success = true;
  return result;
}

}  // namespace rex::ui
