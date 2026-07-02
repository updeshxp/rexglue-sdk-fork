/**
 * @file        system/mod_plugin_loader.cpp
 * @brief       Host-side loader for mod code-plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/mod_plugin.h>

#include <filesystem>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

namespace {

// Mirrors gpu_plugin_loader.cpp's PluginFileName: mod DLLs follow the SDK's
// per-config postfix convention so a mod built alongside a Debug/RelWithDebInfo
// SDK still resolves the matching binary. Unlike GPU plugins, a mod is also
// allowed to ship only a Release DLL (the common case for distributed mods),
// so callers fall back to the bare name when the postfixed one is missing.
std::string ModFileName(std::string_view stem, std::string_view postfix) {
#if REX_PLATFORM_WIN32
  return fmt::format("{}{}.dll", stem, postfix);
#else
  return fmt::format("lib{}{}.so", stem, postfix);
#endif
}

// Mod plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown, same rationale as GPU plugins.
std::vector<platform::DynamicLibrary>& LoadedModPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IModPlugin> LoadModPlugin(const std::filesystem::path& mod_root,
                                          std::string_view mod_name, std::string_view code_stem,
                                          const ModHostContext& ctx) {
  const std::filesystem::path code_dir = mod_root / "code";

  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }

  std::filesystem::path path = code_dir / ModFileName(code_stem, postfix);
  if (!postfix.empty() && !std::filesystem::exists(path)) {
    // Distributed mods commonly ship a single Release build; fall back to it
    // rather than refusing to load into a Debug/RelWithDebInfo host.
    path = code_dir / ModFileName(code_stem, "");
  }
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR("Mod '{}' declares code '{}' but no DLL was found at {}", mod_name, code_stem,
                 path.string());
    return nullptr;
  }

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("Mod '{}' code plugin failed to load: {}", mod_name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<ModAbiVersionFn>(kModAbiVersionSymbol);
  auto create_fn = library.GetSymbol<ModCreateFn>(kModCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("Mod '{}' code plugin is not a rexglue mod plugin (missing {} / {} exports): {}",
                 mod_name, kModAbiVersionSymbol, kModCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kModPluginAbiVersion) {
    REXSYS_ERROR("Mod '{}' code plugin has ABI version {}, host expects {}: {}", mod_name,
                 plugin_abi, kModPluginAbiVersion, path.string());
    return nullptr;
  }

  ModHostContext info = ctx;
  info.struct_size = sizeof(ModHostContext);

  IModPlugin* plugin = create_fn(kModPluginAbiVersion, &info);
  if (!plugin) {
    REXSYS_ERROR("Mod '{}' code plugin factory returned no plugin", mod_name);
    return nullptr;
  }

  LoadedModPlugins().push_back(std::move(library));
  REXSYS_INFO("Mod code plugin '{}' loaded ({})", mod_name, path.filename().string());
  return std::unique_ptr<IModPlugin>(plugin);
}

}  // namespace rex::system
