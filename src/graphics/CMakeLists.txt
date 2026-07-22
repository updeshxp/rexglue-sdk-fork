# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    registers.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    trace_writer.cpp
    pipeline/texture/extent.cpp
    pipeline/texture/util.cpp
    pipeline/texture/replacement.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

if(REXGLUE_USE_VULKAN)
    # rexgpu-shader-translation: Xenos microcode -> SPIR-V translation, plus
    # the small set of files it depends on (PM4/register decode, cvar/format
    # helpers). Split out of rexgpu-xenos into its own static library so it
    # can be linked directly by a consumer that wants to translate/build
    # Xenos shaders without loading the full runtime-loaded xenos plugin
    # (e.g. a from-scratch native renderer) -- previously this required
    # duplicate-compiling these same files a second time elsewhere.
    set(REXGPU_SHADER_TRANSLATION_SOURCES
        flags.cpp
        register_file.cpp
        packet_disassembler.cpp
        format/ucode.cpp
        pipeline/shader/shader.cpp
        pipeline/shader/translator.cpp
        pipeline/shader/translator_disasm.cpp
        pipeline/shader/spirv_builder.cpp
        pipeline/shader/spirv.cpp
        pipeline/shader/spirv_translator.cpp
        pipeline/shader/spirv_translator_alu.cpp
        pipeline/shader/spirv_translator_fetch.cpp
        pipeline/shader/spirv_translator_memexport.cpp
        pipeline/shader/spirv_translator_rb.cpp
    )

    add_library(rexgpu-shader-translation STATIC ${REXGPU_SHADER_TRANSLATION_SOURCES})
    add_library(rex::gpu-shader-translation ALIAS rexgpu-shader-translation)

    set_target_properties(rexgpu-shader-translation PROPERTIES
        EXPORT_NAME gpu-shader-translation
        POSITION_INDEPENDENT_CODE ON
    )

    # PUBLIC: consumers of this library (rexgpu-xenos, or a downstream native
    # renderer) need rexruntime/rexcore's headers and glslang's SPIRV headers
    # transitively, since spirv_builder.h/spirv_translator.h include them.
    target_link_libraries(rexgpu-shader-translation PUBLIC
        rexruntime
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()

add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)

set_target_properties(rexgpu-xenos PROPERTIES
    EXPORT_NAME gpu-xenos
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(rexgpu-xenos PRIVATE
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
)

target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable)

rex_target_pch(rexgpu-xenos
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>
)

if(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES)
    set_source_files_properties(${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Everything PRIVATE: the plugin has no consumers. Never link the OBJECT
# libraries (rexcore/rexui) here; rexruntime already contains those objects
# and relinking them would duplicate global state.
target_link_libraries(rexgpu-xenos PRIVATE
    rexruntime
    xxHash::xxhash
    snappy
    stb_image
    stb_image_write
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        rexgpu-shader-translation
        volk::volk
        GPUOpen::VulkanMemoryAllocator
    )
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexgpu-xenos PRIVATE
        d3d12
        dxgi
        dxguid
        d3dcompiler
    )
endif()
