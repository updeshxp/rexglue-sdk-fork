/**
 * @file        system/gpu_plugin.h
 * @brief       GPU emulation plugin ABI and host-side loader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     One plugin exists today (rexgpu-xenos). The factory carries an
 *              ABI version so this can grow into a general plugin API later.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/interfaces/graphics.h>

#if defined(_WIN32)
#define REX_GPU_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REX_GPU_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace rex::system {

// Bump on any change to GpuCreateInfo or to the IGraphicsSystem interface.
inline constexpr uint32_t kGpuPluginAbiVersion = 1;

inline constexpr const char* kGpuCreateSymbol = "rex_gpu_create";
inline constexpr const char* kGpuAbiVersionSymbol = "rex_gpu_abi_version";

struct GpuCreateInfo {
  uint32_t struct_size = 0;       // sizeof(GpuCreateInfo), set by the host
  const char* backend = nullptr;  // "d3d12", "vulkan", or "any"
};

// extern "C" exports every GPU plugin must provide:
//   uint32_t rex_gpu_abi_version(void);
//   rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
//                                                const GpuCreateInfo* info);
using GpuAbiVersionFn = uint32_t (*)();
using GpuCreateFn = IGraphicsSystem* (*)(uint32_t abi_version, const GpuCreateInfo* info);

// Loads rexgpu-<name> from the executable's directory and constructs its
// graphics system. Returns nullptr after logging a detailed error (missing
// file, missing exports, ABI mismatch, or factory failure). The library
// handle is retained for process lifetime; plugins are never unloaded.
std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name,
                                               std::string_view backend = "any");

// Scans the executable's directory for staged GPU plugin DLLs matching this
// build's naming convention (rexgpu-<name>[postfix].dll / librexgpu-
// <name>[postfix].so) and returns their plugin names (the part LoadGpuPlugin
// takes, e.g. "xenos") -- for UI purposes (e.g. a settings dropdown), since
// `gpu_plugin` itself has no `.allowed(...)` list (the valid set is whatever
// happens to be staged next to the executable, not a fixed enum). Doesn't
// validate ABI/exports; a listed name can still fail to load via
// LoadGpuPlugin if the DLL is malformed.
std::vector<std::string> EnumerateGpuPlugins();

inline constexpr const char* kGpuSupportedBackendsSymbol = "rex_gpu_supported_backends";

// Optional export a GPU plugin may provide:
//   const char* rex_gpu_supported_backends(void);
// Returning a comma-separated list of `gpu_backend` values the plugin
// supports (e.g. "d3d12,vulkan"), reflecting what that specific plugin
// binary was compiled with -- backend support is a property of the plugin,
// not a fixed set every plugin shares (rexgpu-xenos happens to support both
// D3D12 and Vulkan; a future plugin might support only one, or none). A
// plugin without this export is assumed to have no distinct backend choice.
using GpuSupportedBackendsFn = const char* (*)();

// Queries a GPU plugin (by the same name LoadGpuPlugin takes) for the
// `gpu_backend` values it supports, via its optional
// rex_gpu_supported_backends export -- for UI purposes (e.g. only showing a
// backend picker when there's actually more than one option). Loads the
// plugin DLL just long enough to resolve and call that one export, then
// unloads it immediately; LoadGpuPlugin later loads its own, separate,
// permanently-retained instance, so this never leaves an extra copy
// resident. Returns an empty vector if the plugin can't be found, doesn't
// export rex_gpu_supported_backends (no distinct backend choice), or the
// export returns an empty/null string -- callers should treat all of these
// as "nothing to offer a choice between", not an error.
std::vector<std::string> QuerySupportedBackends(std::string_view plugin_name);

}  // namespace rex::system
