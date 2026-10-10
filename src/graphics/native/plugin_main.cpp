/**
 * @file        graphics/native/plugin_main.cpp
 * @brief       rexgpu-native plugin entry points
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     Game-agnostic native GPU renderer plugin. Selected at runtime
 *              with --gpu_plugin native (loads librexgpu-native.so). Exposes
 *              the same ABI as rexgpu-xenos so the host loader is unchanged.
 */

#include <string_view>

#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "native/graphics_system.h"

#ifndef REX_GPU_PLUGIN_NAME
#define REX_GPU_PLUGIN_NAME "native"
#endif

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-{}: host requested ABI {}, plugin is ABI {}", REX_GPU_PLUGIN_NAME,
                 abi_version, rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-{}: invalid GpuCreateInfo", REX_GPU_PLUGIN_NAME);
    return nullptr;
  }

  std::string_view backend = info->backend ? info->backend : "any";
#if REX_HAS_VULKAN
  if (backend == "any" || backend == "vulkan" || backend == REX_GPU_PLUGIN_NAME) {
    return new rex::graphics::native::NativeGraphicsSystem();
  }
#endif
  REXLOG_ERROR("rexgpu-{}: requested backend '{}' is not compiled into this plugin",
               REX_GPU_PLUGIN_NAME, backend);
  return nullptr;
}
