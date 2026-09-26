# rexgpu-plume: Plume Modern Graphics GPU plugin
# Namespace: rex::graphics::plume
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this target.

set(REXGPU_PLUME_SOURCES
    plugin_main.cpp
    plume_graphics_system.cpp
    plume_command_processor.cpp
    plume_texture_cache.cpp
    plume_render_target_cache.cpp
    plume_texture_uploader.cpp
    ${REXGLUE_ROOT}/src/graphics/shared_memory.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/texture/cache.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/render_target/cache.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/texture/util.cpp
    ${REXGLUE_ROOT}/src/graphics/command_processor.cpp
    ${REXGLUE_ROOT}/src/graphics/pm4_plume_transpiler.cpp
    ${REXGLUE_ROOT}/src/graphics/graphics_system.cpp
    ${REXGLUE_ROOT}/src/graphics/register_file.cpp
    ${REXGLUE_ROOT}/src/graphics/registers.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/shader.cpp
    ${REXGLUE_ROOT}/src/graphics/format/ucode.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/interpreter.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/translator.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/translator_disasm.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_builder.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_translator.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_translator_alu.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp
    ${REXGLUE_ROOT}/src/graphics/pipeline/shader/spirv_translator_rb.cpp
    ${REXGLUE_ROOT}/src/graphics/util/draw_extent_estimator.cpp
    ${REXGLUE_ROOT}/src/graphics/util/draw.cpp
    ${REXGLUE_ROOT}/src/graphics/flags.cpp
)

add_library(rexgpu-plume SHARED ${REXGPU_PLUME_SOURCES})
add_library(rex::gpu-plume ALIAS rexgpu-plume)

set_target_properties(rexgpu-plume PROPERTIES
    EXPORT_NAME gpu-plume
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

set(PLUME_ROOT "${REXGLUE_ROOT}/thirdparty/plume")
set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUME_SDL_VULKAN_ENABLED OFF CACHE BOOL "" FORCE)

add_subdirectory("${PLUME_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

target_include_directories(rexgpu-plume PRIVATE
    ${PLUME_ROOT}
    ${PLUME_ROOT}/contrib/volk
    ${PLUME_ROOT}/contrib/Vulkan-Headers/include
    ${PLUME_ROOT}/contrib/VulkanMemoryAllocator/include
    ${REXGLUE_ROOT}/thirdparty/renderdoc
)

target_link_libraries(rexgpu-plume PRIVATE
    rexruntime
    plume
    xxHash::xxhash
    Vulkan::Headers
    glslang::SPIRV
    glslang
    MachineIndependent
    GenericCodeGen
    OSDependent
    OGLCompiler
)
