# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    plugin_main.cpp

    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    pipeline/texture/extent.cpp
    pipeline/texture/util.cpp
    pipeline/texture/replacement.cpp
    pm4_plume_transpiler.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/geometry_shader_builder.cpp
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
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
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
    stb_image
    stb_image_write
)

if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexgpu-xenos PRIVATE
        Vulkan::Headers
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
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

#==============================================================================
# rexgpu-native: game-agnostic NATIVE GPU renderer plugin
#
# Selected at runtime with --gpu_plugin native (loads librexgpu-native.so).
# Reuses the SAME backend-agnostic PM4 command-processor + Xenos base that
# rexgpu-xenos uses, and replaces only the backend draw/resolve/present seams
# with native GPU command translation (no EDRAM/tiling/resolve emulation).
#
# This target is fully independent of rexgpu-xenos - it does not modify or
# link it.
#
# Vulkan-only for now (the scaffold reuses the Vulkan provider for windowing).
#==============================================================================
if(REXGLUE_USE_VULKAN)
    set(REXGPU_NATIVE_SOURCES
        # Native renderer plugin entry + backend
        native/plugin_main.cpp
        native/graphics_system.cpp
        native/command_processor.cpp
        native/native_shared_memory.cpp
        native/texture_cache.cpp

        # Geometry-shader SPIR-V construction for Xenos primitive types with no
        # direct Vulkan topology (rectangles today - see command_processor.cpp's
        # GetGeometryShader). Split out of vulkan/pipeline_cache.cpp precisely so
        # it can be compiled here without pulling in the rest of
        # VulkanPipelineCache (VulkanCommandProcessor/VulkanRenderTargetCache/
        # VulkanShader) - keeps this target independent of rexgpu-xenos.
        vulkan/geometry_shader_builder.cpp

        # Shared, game-agnostic PM4 + Xenos base (identical to the rexgpu-xenos
        # backend-agnostic core; only the backend files differ).
        flags.cpp
        register_file.cpp
        registers.cpp
        pipeline/shader/shader.cpp
        format/ucode.cpp
        sampler_info.cpp
        graphics_system.cpp
        command_processor.cpp
        pipeline/texture/extent.cpp
        pipeline/texture/util.cpp
        util/draw_extent_estimator.cpp
        util/draw.cpp
        packet_disassembler.cpp
        primitive_processor.cpp
        pipeline/render_target/cache.cpp
        shared_memory.cpp
        pipeline/texture/cache.cpp
        pipeline/shader/interpreter.cpp
        pipeline/shader/translator.cpp
        pipeline/shader/translator_disasm.cpp

        # SPIR-V shader translation (Xenos microcode -> SPIR-V). Reused verbatim
        # from the shared translator - the native backend translates guest
        # shaders itself instead of emulating the Xenos ALU.
        pipeline/shader/spirv_builder.cpp
        pipeline/shader/spirv.cpp
        pipeline/shader/spirv_translator.cpp
        pipeline/shader/spirv_translator_alu.cpp
        pipeline/shader/spirv_translator_fetch.cpp
        pipeline/shader/spirv_translator_memexport.cpp
        pipeline/shader/spirv_translator_rb.cpp
    )

    add_library(rexgpu-native SHARED ${REXGPU_NATIVE_SOURCES})
    add_library(rex::gpu-native ALIAS rexgpu-native)

    set_target_properties(rexgpu-native PROPERTIES
        EXPORT_NAME gpu-native
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # ${CMAKE_CURRENT_SOURCE_DIR} (src/graphics) lets native/*.cpp resolve their
    # "native/..." internal includes; renderdoc mirrors the xenos target.
    target_include_directories(rexgpu-native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT}
    )

    target_compile_options(rexgpu-native PRIVATE -Wno-unused-variable)

    # Everything PRIVATE: the plugin has no consumers. rexruntime already
    # contains the rexcore/rexui objects (including the Vulkan provider).
    target_link_libraries(rexgpu-native PRIVATE
        rexruntime
        xxHash::xxhash
        stb_image
        stb_image_write
        Vulkan::Headers
        glslang::SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
    )
endif()
