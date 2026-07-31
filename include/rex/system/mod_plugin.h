/**
 * @file        system/mod_plugin.h
 * @brief       Mod code-plugin ABI and host-side loader
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Generalizes the GPU plugin ABI (see gpu_plugin.h) so any mod
 *              folder can ship a native DLL that hooks the app lifecycle,
 *              not just replace assets. A mod DLL links rexruntime (shared
 *              across the process) so it reaches the same ImGui drawer,
 *              keybind registry, kernel state and memory as the host exe --
 *              this header only carries the handful of pointers that are
 *              NOT reachable through a global/singleton accessor.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define REX_MOD_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REX_MOD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace rex {
class Runtime;
namespace ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace ui
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace rex::system {

// Bump on any change to ModHostContext or to IModPlugin.
inline constexpr uint32_t kModPluginAbiVersion = 1;

inline constexpr const char* kModCreateSymbol = "rex_mod_create";
inline constexpr const char* kModAbiVersionSymbol = "rex_mod_abi_version";

// Borrowed pointers, valid for the mod's lifetime (the plugin is never
// unloaded, so these outlive it). Everything else a mod needs -- cvars,
// rex::ui::RegisterBind, ImGuiDialog registration, runtime->memory(),
// runtime->kernel_state(), runtime->graphics_system() -- is reachable as a
// public SDK API the mod calls directly.
struct ModHostContext {
  uint32_t struct_size = 0;  // sizeof(ModHostContext), set by the host
  Runtime* runtime = nullptr;
  ui::WindowedAppContext* app_context = nullptr;
  ui::Window* window = nullptr;
  input::InputSystem* input_system = nullptr;
  const char* mod_root = nullptr;  // this mod's folder, e.g. for own assets
  const char* mod_name = nullptr;  // folder name, for logging
};

// Lifecycle hooks a mod plugin may implement. All are optional (no-op
// defaults) so a mod only needs to override what it uses.
class IModPlugin {
 public:
  virtual ~IModPlugin() = default;

  // Called once, right after the host's ImGui drawer/overlay stack exists
  // (mirrors ReXApp::OnCreateDialogs). Register overlays/keybinds here.
  virtual void OnCreateDialogs(ui::ImGuiDrawer* drawer) { (void)drawer; }

  // Called once the guest module's main thread has been prepared and
  // KernelState is fully live (mirrors the tail of ReXApp::LaunchModule).
  // Use this for anything that needs kernel apps/memory, e.g. scanning
  // filesystem-backed content.
  virtual void OnModuleLaunched() {}

  // Called before the host begins shutting down. Release resources here.
  virtual void OnShutdown() {}
};

// extern "C" exports every mod code plugin must provide:
//   uint32_t rex_mod_abi_version(void);
//   rex::system::IModPlugin* rex_mod_create(uint32_t abi_version,
//                                           const rex::system::ModHostContext* ctx);
using ModAbiVersionFn = uint32_t (*)();
using ModCreateFn = IModPlugin* (*)(uint32_t abi_version, const ModHostContext* ctx);

// One entry of a `requires` list: a dependency's folder name plus an
// optional "must be at least this version" constraint. `min_version` is
// empty when the requires entry named no constraint (e.g. plain
// "game_symbols" rather than "game_symbols >= 1.0.0"), meaning any enabled,
// correctly-ordered version satisfies it.
struct ModRequirement {
  std::string name;
  std::string min_version;  // dotted numeric version, e.g. "1.0.0"; empty = unconstrained
};

// Describes one enabled mod for display purposes (mod manager overlay etc.),
// parsed from <mod_root>/mod.toml. Purely descriptive -- the SDK does not
// interpret any field besides `code` (the DLL stem to load, if any) and
// `requires` (validated by Runtime::ValidateModDependencies(), see below).
struct ModInfo {
  std::filesystem::path mod_root;
  std::string folder_name;
  std::string display_name;
  std::string version;
  std::string author;
  std::string description;
  std::filesystem::path icon_path;  // empty if no icon.png present
  std::string code;                 // DLL stem under <mod_root>/code/, or empty

  // Dependency/conflict metadata, parsed from mod.toml's `requires`,
  // `load_after` and `conflicts` keys (comma-separated lists, all optional).
  // Validated by Runtime::ValidateModDependencies() at Setup() time.
  std::vector<ModRequirement> requires_mods;  // each must be enabled, ordered
                                              // before this mod, and (if
                                              // min_version is set) at least
                                              // that version, or Setup() fails
  std::vector<std::string> load_after_mods;   // soft ordering hint, warns only
  std::vector<std::string> conflicts_mods;    // hard error if also enabled,
                                              // regardless of order

  // Platform target(s) this code mod's code/ directory currently ships a
  // binary for (e.g. "windows-x64", "linux-x64", "linux-arm64"), parsed from
  // mod.toml's `platform` key (comma-separated). Always empty for asset-only
  // mods (no `code`), and purely descriptive for the SDK itself -- it does
  // not gate LoadModPlugin -- but consumed by rex::system::ModState::Validate
  // to flag a code mod with no binary for the running host.
  std::vector<std::string> platforms;

  // Minimum host application version, parsed from mod.toml's `game_version`
  // key ("1.2.0" or ">= 1.2.0" -- both mean the same thing; no other
  // comparison operators are supported). Empty means unconstrained.
  // Validated against Runtime::game_version() by
  // Runtime::ValidateModDependencies(); a hard error if the host's version is
  // older, or if the host never set RuntimeConfig::game_version at all.
  std::string min_game_version;
};

// Loads <mod_root>/code/<code_stem>[<config-postfix>].dll (falling back to
// <code_stem>.dll) and constructs its plugin. Returns nullptr after logging a
// detailed error (missing file, missing exports, ABI mismatch, or factory
// failure). The library handle is retained for process lifetime; mod
// plugins, like GPU plugins, are never unloaded.
std::unique_ptr<IModPlugin> LoadModPlugin(const std::filesystem::path& mod_root,
                                          std::string_view mod_name, std::string_view code_stem,
                                          const ModHostContext& ctx);

}  // namespace rex::system
