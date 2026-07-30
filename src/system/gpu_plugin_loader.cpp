/**
 * @file        system/gpu_plugin_loader.cpp
 * @brief       Host-side loader for GPU emulation plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/gpu_plugin.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

namespace {

// Plugin binaries follow the SDK's per-config postfix convention; this TU is
// part of rexruntime, so REXGLUE_BUILD_CONFIG matches the plugin's config.
std::string_view PluginPostfix() {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  if (kConfig == "Debug") {
    return "d";
  } else if (kConfig == "RelWithDebInfo") {
    return "rd";
  }
  return "";
}

std::string PluginFileName(std::string_view name) {
#if REX_PLATFORM_WIN32
  return fmt::format("rexgpu-{}{}.dll", name, PluginPostfix());
#else
  return fmt::format("librexgpu-{}{}.so", name, PluginPostfix());
#endif
}

#if REX_PLATFORM_WIN32
constexpr std::string_view kPluginPrefix = "rexgpu-";
constexpr std::string_view kPluginExtension = ".dll";
#else
constexpr std::string_view kPluginPrefix = "librexgpu-";
constexpr std::string_view kPluginExtension = ".so";
#endif

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
  auto path = rex::filesystem::GetExecutableFolder() / PluginFileName(name);
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR(
        "GPU plugin '{}' not found at {}. Stage it next to the executable "
        "(GPU_PLUGINS {} in rexglue_configure_target).",
        name, path.string(), name);
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("GPU plugin '{}' failed to load: {}", name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<GpuAbiVersionFn>(kGpuAbiVersionSymbol);
  auto create_fn = library.GetSymbol<GpuCreateFn>(kGpuCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("GPU plugin '{}' is not a rexglue GPU plugin (missing {} / {} exports): {}", name,
                 kGpuAbiVersionSymbol, kGpuCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kGpuPluginAbiVersion, path.string());
    return nullptr;
  }

  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();

  IGraphicsSystem* graphics_system = create_fn(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_INFO("GPU plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
}

std::vector<std::string> EnumerateGpuPlugins() {
  std::vector<std::string> names;

  std::error_code ec;
  auto dir = rex::filesystem::GetExecutableFolder();
  std::string_view postfix = PluginPostfix();

  for (const auto& dir_entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!dir_entry.is_regular_file()) {
      continue;
    }
    std::string filename = dir_entry.path().filename().string();
    std::string_view stem(filename);
    if (!stem.starts_with(kPluginPrefix) || !stem.ends_with(kPluginExtension)) {
      continue;
    }
    stem.remove_prefix(kPluginPrefix.size());
    stem.remove_suffix(kPluginExtension.size());

    // The SDK ships every build variant of a plugin side by side (see
    // build.py's copy_runtime_libs), distinguished only by a bare "d"/"rd"
    // suffix -- same ambiguity/heuristic as there: only accept the variant
    // matching this build's postfix.
    if (!postfix.empty()) {
      if (!stem.ends_with(postfix)) {
        continue;
      }
      stem.remove_suffix(postfix.size());
    } else if (stem.ends_with("rd") || stem.ends_with("d")) {
      continue;
    }

    names.emplace_back(stem);
  }

  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> QuerySupportedBackends(std::string_view name) {
  std::vector<std::string> backends;

  auto path = rex::filesystem::GetExecutableFolder() / PluginFileName(name);
  if (!std::filesystem::exists(path)) {
    return backends;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    return backends;
  }

  auto supported_backends_fn =
      library.GetSymbol<GpuSupportedBackendsFn>(kGpuSupportedBackendsSymbol);
  if (!supported_backends_fn) {
    return backends;
  }

  const char* csv = supported_backends_fn();
  if (!csv) {
    return backends;
  }

  // Parsed into `backends` (an independent copy) before `library` unloads at
  // end of scope, invalidating `csv` (a pointer into the plugin's memory).
  std::string_view view(csv);
  size_t start = 0;
  while (start < view.size()) {
    size_t comma = view.find(',', start);
    if (comma == std::string_view::npos) {
      comma = view.size();
    }
    std::string_view token = view.substr(start, comma - start);
    if (!token.empty()) {
      backends.emplace_back(token);
    }
    start = comma + 1;
  }
  return backends;
}

}  // namespace rex::system
