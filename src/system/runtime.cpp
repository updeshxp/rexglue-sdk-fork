/**
 * @file        runtime/runtime.cpp
 * @brief       Runtime subsystem implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>          // PPCFuncMapping
#include <rex/platform/exceptions.h>  // SEH exception support
#include <rex/kernel/crt/heap.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <toml++/toml.hpp>

REXCVAR_DEFINE_STRING(game_data_root, "", "Runtime", "Override game data path");
REXCVAR_DEFINE_STRING(user_data_root, "", "Runtime", "Override user data path");
REXCVAR_DEFINE_STRING(update_data_root, "", "Runtime", "Override update data path");
REXCVAR_DEFINE_STRING(cache_root, "", "Runtime", "Override shader cache path");
REXCVAR_DEFINE_STRING(metadata_root, "", "Runtime", "Override metadata path");
REXCVAR_DEFINE_STRING(mods_data_root, "", "Runtime", "Host directory containing mod folders");
REXCVAR_DEFINE_STRING(enabled_mods, "", "Runtime",
                      "Comma-separated mod folder names to layer over game data");
REXCVAR_DEFINE_STRING(mods_dump_root, "", "Runtime",
                      "Host directory dumped assets (textures, shaders) are written under; "
                      "defaults to <exe folder>/dumps");

namespace rex {

// Static instance for global access
Runtime* Runtime::instance_ = nullptr;

Runtime* Runtime::instance() {
  return instance_;
}

Runtime::Runtime(const std::filesystem::path& game_data_root,
                 const std::filesystem::path& user_data_root,
                 const std::filesystem::path& update_data_root,
                 const std::filesystem::path& cache_root,
                 const std::filesystem::path& metadata_root)
    : game_data_root_(game_data_root),
      user_data_root_(user_data_root.empty() ? game_data_root : user_data_root),
      update_data_root_(update_data_root),
      cache_root_(cache_root),
      metadata_root_(metadata_root) {}

Runtime::~Runtime() {
  Shutdown();
}

std::optional<std::filesystem::path> Runtime::FindMetadataPath(
    const std::filesystem::path& relative_path) const {
  if (!metadata_root_.empty()) {
    std::filesystem::path candidate = metadata_root_ / relative_path;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
    return std::nullopt;
  }

  const std::filesystem::path candidates[] = {
      game_data_root_ / "metadata" / relative_path,
      game_data_root_.parent_path() / "metadata" / relative_path,
      game_data_root_ / relative_path,
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<EmbeddedMetadataAsset> Runtime::FindEmbeddedMetadata(
    const std::filesystem::path& relative_path) const {
  return FindEmbeddedMetadataAsset(relative_path);
}

X_STATUS Runtime::Setup(RuntimeConfig config) {
  if (instance_ != nullptr) {
    REXSYS_ERROR("Runtime::Setup() called but global instance already exists");
    return X_STATUS_UNSUCCESSFUL;
  }
  instance_ = this;

  auto fail = [this](X_STATUS status, std::string_view reason) {
    REXSYS_ERROR("Runtime::Setup failed: {}", reason);
    Shutdown();
    return status;
  };

  // Start profiler (Tracy network threads, counter init)
  rex::perf::Profiler::Startup();

  // Initialize SEH exception support for hardware exception handling
  rex::initialize_seh();

  // Initialize clock
  chrono::Clock::set_guest_tick_frequency(50000000);
  chrono::Clock::set_guest_system_time_base(chrono::Clock::QueryHostSystemTime());
  chrono::Clock::set_guest_time_scalar(1.0);

  // Enable threading affinity configuration
  thread::EnableAffinityConfiguration();

  tool_mode_ = config.tool_mode;
  game_version_ = config.game_version;

  // Create memory system first
  memory_ = std::make_unique<memory::Memory>();
  if (!memory_->Initialize()) {
    return fail(X_STATUS_UNSUCCESSFUL, "memory init failed");
  }

  export_resolver_ = std::make_unique<runtime::ExportResolver>();

  function_dispatcher_ =
      std::make_unique<runtime::FunctionDispatcher>(memory_.get(), export_resolver_.get());
  REXSYS_INFO("FunctionDispatcher initialized");

  // Create virtual file system
  file_system_ = std::make_unique<rex::filesystem::VirtualFileSystem>();

  // Create kernel state - this sets the global singleton
  kernel_state_ = std::make_unique<system::KernelState>(this);

  // Shared address/event registry mods publish into and consume from. The
  // is_patched provider is injected (rather than reached statically) so the
  // registry itself stays testable without a live KernelState.
  mod_registry_ = std::make_unique<system::ModRegistry>([this] {
    auto module = kernel_state_->GetExecutableModule();
    return module && module->is_patched();
  });

  // Initialize input from injected config
  if (config.input_factory) {
    input_system_ = config.input_factory(tool_mode_);
    if (input_system_) {
      X_STATUS input_status = input_system_->Setup();
      if (XFAILED(input_status)) {
        REXSYS_WARN("Failed to initialize input system (status {:08X}) - input disabled",
                    input_status);
        input_system_.reset();
      } else {
        REXSYS_INFO("Input system initialized");
      }
    }
  }

  // HLE kernel modules and apps.
  if (config.kernel_init) {
    config.kernel_init(this, kernel_state_.get());
  }

  // Initialize the APU (Audio Processing Unit) from injected config
  if (config.audio_factory) {
    audio_system_ = config.audio_factory(function_dispatcher_.get());
    if (audio_system_) {
      X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());
      if (XFAILED(audio_status)) {
        REXSYS_WARN("Failed to initialize audio system (status {:08X}) - audio disabled",
                    audio_status);
        audio_system_.reset();
      } else {
        REXSYS_INFO("Audio system initialized");
      }
    }
  }

  // Set up VFS: game_data_root as game:/d:, update_data_root as update:
  if (!SetupVfs()) {
    return fail(X_STATUS_UNSUCCESSFUL, "VFS setup failed");
  }

  // ResolveEnabledMods() (called from SetupVfs()) has already parsed and
  // cached every enabled mod's mod.toml; validate requires/load_after/
  // conflicts now that the full enabled-mods list and order are known.
  if (!ValidateModDependencies()) {
    return fail(X_STATUS_UNSUCCESSFUL, "mod dependency validation failed");
  }

  // Skip GPU initialization in tool mode (for analysis tools like codegen)
  if (tool_mode_) {
    REXSYS_INFO("Runtime initialized in tool mode (no GPU)");
    setup_complete_ = true;
    return X_STATUS_SUCCESS;
  }

  // Initialize GPU from injected config
  if (config.graphics) {
    graphics_system_ = std::move(config.graphics);
    bool with_presentation = (app_context_ != nullptr);
    X_STATUS gpu_status = graphics_system_->Setup(function_dispatcher_.get(), kernel_state_.get(),
                                                  app_context_, with_presentation);
    if (XFAILED(gpu_status)) {
      return fail(gpu_status, "GPU setup failed");
    }
    // Tick the mod registry once per guest frame (on GPU swap). Graphics
    // systems without a swap concept (SetHostSwapCallback default no-op)
    // simply never tick.
    graphics_system_->SetHostSwapCallback([this] { mod_registry_->DispatchTick(); });
    REXSYS_INFO("GPU system initialized (presentation={})", with_presentation);
  } else {
    REXSYS_INFO("Runtime initialized without graphics system (native rendering mode)");
  }

  REXSYS_INFO("Runtime initialized successfully");
  setup_complete_ = true;
  return X_STATUS_SUCCESS;
}

X_STATUS Runtime::Setup(const rex::PPCImageInfo& image_info, RuntimeConfig config) {
  X_STATUS status = Setup(std::move(config));
  if (status != X_STATUS_SUCCESS) {
    return status;
  }

  if (!function_dispatcher_->InitializeFunctionTable(image_info.code_base, image_info.code_size,
                                                     image_info.image_base, image_info.image_size,
                                                     /*is_entrypoint=*/true)) {
    REXSYS_ERROR("Failed to initialize function table");
    Shutdown();
    return X_STATUS_UNSUCCESSFUL;
  }

  if (image_info.func_mappings) {
    int count = 0;
    int duplicates = 0;
    int rejected = 0;
    for (int i = 0; image_info.func_mappings[i].guest != 0; ++i) {
      uint32_t guest = static_cast<uint32_t>(image_info.func_mappings[i].guest);
      auto* host = image_info.func_mappings[i].host;
      if (!host) {
        continue;
      }
      if (function_dispatcher_->GetFunction(guest)) {
        REXSYS_WARN("func_mappings: duplicate guest address {:08X}", guest);
        ++duplicates;
      }
      if (!function_dispatcher_->SetFunction(guest, host)) {
        ++rejected;
      } else {
        ++count;
      }
    }
    REXSYS_DEBUG("Registered {} recompiled functions ({} duplicates, {} rejected)", count,
                 duplicates, rejected);
    if (rejected > 0) {
      REXSYS_ERROR("PPCImageInfo registration: {} func_mappings entries rejected", rejected);
      Shutdown();
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  REXSYS_DEBUG("Runtime setup complete (code: {:08X}-{:08X}, image: {:08X}-{:08X})",
               image_info.code_base, image_info.code_base + image_info.code_size,
               image_info.image_base, image_info.image_base + image_info.image_size);
  return X_STATUS_SUCCESS;
}

void Runtime::Shutdown() {
  if (!instance_ && !setup_complete_ && !memory_) {
    return;
  }

  if (instance_ == this) {
    instance_ = nullptr;
  }

  if (graphics_system_) {
    graphics_system_->Shutdown();
    graphics_system_.reset();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
    audio_system_.reset();
  }
  if (input_system_) {
    input_system_->Shutdown();
    input_system_.reset();
  }
  kernel_state_.reset();
  function_dispatcher_.reset();
  export_resolver_.reset();
  file_system_.reset();
  memory_.reset();

  rex::perf::Profiler::Shutdown();
  setup_complete_ = false;
}

uint8_t* Runtime::virtual_membase() const {
  return memory_ ? memory_->virtual_membase() : nullptr;
}

namespace {

// Splits a comma-separated list, trims whitespace around each entry, and
// drops empty entries. Shared by ResolveEnabledMods() (enabled_mods cvar) and
// ParseModInfo() (mod.toml's requires/load_after/conflicts keys); both use
// the same "comma list of folder names" convention.
std::vector<std::string> SplitCommaList(std::string_view csv) {
  std::vector<std::string> result;
  std::istringstream ss{std::string(csv)};
  std::string token;
  while (std::getline(ss, token, ',')) {
    auto start = token.find_first_not_of(" \t");
    auto end = token.find_last_not_of(" \t");
    if (start == std::string::npos) {
      continue;
    }
    token = token.substr(start, end - start + 1);
    if (!token.empty()) {
      result.push_back(std::move(token));
    }
  }
  return result;
}

// Splits one `requires` entry into a mod name and an optional minimum-version
// constraint: "game_symbols >= 1.0.0" -> {"game_symbols", "1.0.0"}; a bare
// "game_symbols" -> {"game_symbols", ""} (unconstrained). Whitespace around
// the name and version is trimmed; SplitCommaList() has already trimmed the
// entry's outer edges.
system::ModRequirement ParseRequirement(std::string_view entry) {
  system::ModRequirement req;
  auto op_pos = entry.find(">=");
  if (op_pos == std::string_view::npos) {
    req.name = std::string(entry);
    return req;
  }
  auto name_part = entry.substr(0, op_pos);
  auto name_end = name_part.find_last_not_of(" \t");
  req.name =
      std::string(name_part.substr(0, name_end == std::string_view::npos ? 0 : name_end + 1));

  auto version_part = entry.substr(op_pos + 2);
  auto version_start = version_part.find_first_not_of(" \t");
  if (version_start != std::string_view::npos) {
    req.min_version = std::string(version_part.substr(version_start));
  }
  return req;
}

// Parses mod.toml's `game_version` key into a minimum-version constraint.
// Both "1.2.0" and ">= 1.2.0" mean "must be at least 1.2.0" -- no other
// comparison operators are supported. Returns empty for an absent/blank key.
std::string ParseGameVersionConstraint(std::string_view value) {
  auto start = value.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return "";
  }
  value = value.substr(start);
  if (value.substr(0, 2) == ">=") {
    value = value.substr(2);
    auto version_start = value.find_first_not_of(" \t");
    value =
        version_start == std::string_view::npos ? std::string_view() : value.substr(version_start);
  }
  auto end = value.find_last_not_of(" \t");
  return end == std::string_view::npos ? "" : std::string(value.substr(0, end + 1));
}

// Parses a dotted numeric version ("1.0.0", "2.3") into its components.
// Returns false if any '.'-separated part isn't a non-negative integer or the
// string is empty -- callers treat that as "not a usable version" rather than
// trying to compare it.
bool ParseVersionComponents(std::string_view version, std::vector<int>& out) {
  out.clear();
  std::string current;
  auto flush = [&]() -> bool {
    if (current.empty() || !std::all_of(current.begin(), current.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
      return false;
    }
    out.push_back(std::stoi(current));
    current.clear();
    return true;
  };
  for (char c : version) {
    if (c == '.') {
      if (!flush()) {
        return false;
      }
    } else {
      current += c;
    }
  }
  return flush();
}

// Compares two parsed versions component-wise; missing trailing components
// count as 0, so "1.0" == "1.0.0". Returns <0, 0, >0 as `have` compares to
// `want`.
int CompareVersions(const std::vector<int>& have, const std::vector<int>& want) {
  size_t n = std::max(have.size(), want.size());
  for (size_t i = 0; i < n; ++i) {
    int hv = i < have.size() ? have[i] : 0;
    int wv = i < want.size() ? want[i] : 0;
    if (hv != wv) {
      return hv - wv;
    }
  }
  return 0;
}

// mod.toml is optional and purely descriptive except for `code`, which names
// the DLL stem the mod loader should load from <mod_root>/code/, and
// requires/load_after/conflicts, which ValidateModDependencies() enforces.
// Missing fields (or a missing file entirely) just leave ModInfo's members
// empty; this must never hard-fail, since asset-only mods commonly omit it.
system::ModInfo ParseModInfo(const std::filesystem::path& mod_root) {
  system::ModInfo info;
  info.mod_root = mod_root;
  info.folder_name = mod_root.filename().string();
  info.display_name = info.folder_name;

  auto icon_path = mod_root / "icon.png";
  if (std::filesystem::is_regular_file(icon_path)) {
    info.icon_path = icon_path;
  }

  auto manifest_path = mod_root / "mod.toml";
  if (!std::filesystem::is_regular_file(manifest_path)) {
    return info;
  }

  try {
    auto table = toml::parse_file(manifest_path.string());
    info.display_name = table["name"].value_or<std::string>(std::string(info.folder_name));
    info.version = table["version"].value_or<std::string>("");
    info.author = table["author"].value_or<std::string>("");
    info.description = table["description"].value_or<std::string>("");
    info.code = table["code"].value_or<std::string>("");
    for (auto& entry : SplitCommaList(table["requires"].value_or<std::string>(""))) {
      info.requires_mods.push_back(ParseRequirement(entry));
    }
    info.load_after_mods = SplitCommaList(table["load_after"].value_or<std::string>(""));
    info.conflicts_mods = SplitCommaList(table["conflicts"].value_or<std::string>(""));
    info.min_game_version =
        ParseGameVersionConstraint(table["game_version"].value_or<std::string>(""));
  } catch (const toml::parse_error& error) {
    REXSYS_WARN("Failed to parse {}: {}", manifest_path.string(), error.what());
  }
  return info;
}

}  // namespace

void Runtime::ResolveEnabledMods() {
  enabled_mod_roots_.clear();
  enabled_mods_info_.clear();

  std::string mods_root_cvar = REXCVAR_GET(mods_data_root);
  std::string enabled_cvar = REXCVAR_GET(enabled_mods);
  if (enabled_cvar.empty()) {
    return;
  }

  // Default to <exe folder>/mods when unset, so a packaged build with mods
  // enabled in its config works without a launcher passing --mods_data_root
  // explicitly (relative values are otherwise resolved against CWD, not the
  // exe directory).
  auto mods_root = mods_root_cvar.empty()
                       ? rex::filesystem::GetExecutableFolder() / "mods"
                       : std::filesystem::absolute(std::filesystem::path(mods_root_cvar));
  if (!std::filesystem::is_directory(mods_root)) {
    REXSYS_WARN("  mods_data_root does not exist: {}", mods_root.string());
    return;
  }

  // Order is preserved: earlier entries are higher priority (see
  // ModOverlayRoots).
  for (auto& name : SplitCommaList(enabled_cvar)) {
    auto mod_dir = mods_root / name;
    if (std::filesystem::is_directory(mod_dir)) {
      enabled_mod_roots_.push_back(mod_dir);
      REXSYS_INFO("  Mod enabled: {} ({})", name, mod_dir.string());
    } else {
      REXSYS_WARN("  Mod '{}' not found at {}, skipping", name, mod_dir.string());
    }
  }

  // Parse mod.toml once per enabled mod, in priority order, and cache it --
  // both EnabledModsInfo() and ValidateModDependencies() read this cache
  // rather than re-parsing.
  enabled_mods_info_.reserve(enabled_mod_roots_.size());
  for (auto& mod_root : enabled_mod_roots_) {
    enabled_mods_info_.push_back(ParseModInfo(mod_root));
  }
}

std::vector<std::filesystem::path> Runtime::ModOverlayRoots(std::string_view subpath) const {
  std::vector<std::filesystem::path> roots;
  roots.reserve(enabled_mod_roots_.size());
  for (auto& mod_root : enabled_mod_roots_) {
    auto partition_dir = mod_root / rex::to_path(subpath);
    if (std::filesystem::is_directory(partition_dir)) {
      roots.push_back(partition_dir);
    }
  }
  return roots;
}

std::filesystem::path Runtime::ModDumpRoot() const {
  std::string dump_root_cvar = REXCVAR_GET(mods_dump_root);
  if (dump_root_cvar.empty()) {
    return rex::filesystem::GetExecutableFolder() / "dumps";
  }
  return std::filesystem::absolute(std::filesystem::path(dump_root_cvar));
}

std::vector<system::ModInfo> Runtime::EnabledModsInfo() const {
  return enabled_mods_info_;
}

bool Runtime::ValidateModDependencies() const {
  // name -> load-order index (0 = highest priority, matches
  // enabled_mod_roots_/ModOverlayRoots).
  std::unordered_map<std::string, size_t> index_of;
  index_of.reserve(enabled_mods_info_.size());
  for (size_t i = 0; i < enabled_mods_info_.size(); ++i) {
    index_of.emplace(enabled_mods_info_[i].folder_name, i);
  }

  bool ok = true;
  for (size_t i = 0; i < enabled_mods_info_.size(); ++i) {
    const auto& mod = enabled_mods_info_[i];

    for (auto& req : mod.requires_mods) {
      if (req.name == mod.folder_name) {
        REXSYS_ERROR("Mod '{}' lists itself in 'requires'", mod.folder_name);
        ok = false;
        continue;
      }
      auto it = index_of.find(req.name);
      if (it == index_of.end()) {
        REXSYS_ERROR(
            "Mod '{}' requires '{}', which is not enabled. Add '{}' to enabled_mods before '{}'.",
            mod.folder_name, req.name, req.name, mod.folder_name);
        ok = false;
        continue;
      }
      if (it->second > i) {
        REXSYS_ERROR(
            "Mod '{}' requires '{}', but '{}' is listed after '{}' in enabled_mods. Move '{}' "
            "earlier.",
            mod.folder_name, req.name, req.name, mod.folder_name, req.name);
        ok = false;
        continue;
      }
      if (req.min_version.empty()) {
        continue;
      }

      // A version constraint can only be checked if both sides parse as a
      // dotted version. Missing/unparsable data is not itself an error --
      // that would break mods and hosts that predate this feature -- it just
      // means the constraint can't be verified, so it's accepted with a
      // warning rather than failing Setup().
      std::vector<int> want;
      if (!ParseVersionComponents(req.min_version, want)) {
        REXSYS_WARN(
            "Mod '{}' requires '{}' >= '{}', but '{}' isn't a valid dotted version (e.g. "
            "'1.0.0'); skipping the version check",
            mod.folder_name, req.name, req.min_version, req.min_version);
        continue;
      }
      const auto& dep = enabled_mods_info_[it->second];
      std::vector<int> have;
      if (dep.version.empty() || !ParseVersionComponents(dep.version, have)) {
        REXSYS_WARN(
            "Mod '{}' requires '{}' >= {}, but '{}' has no valid 'version' in its mod.toml; "
            "skipping the version check",
            mod.folder_name, req.name, req.min_version, req.name);
        continue;
      }
      if (CompareVersions(have, want) < 0) {
        REXSYS_ERROR("Mod '{}' requires '{}' >= {}, but the enabled '{}' is only version {}",
                     mod.folder_name, req.name, req.min_version, req.name, dep.version);
        ok = false;
      }
    }

    if (!mod.min_game_version.empty()) {
      std::vector<int> want;
      if (!ParseVersionComponents(mod.min_game_version, want)) {
        REXSYS_WARN(
            "Mod '{}' has 'game_version = {}', which isn't a valid dotted version (e.g. "
            "'1.0.0'); skipping the version check",
            mod.folder_name, mod.min_game_version);
      } else {
        std::vector<int> have;
        if (game_version_.empty() || !ParseVersionComponents(game_version_, have)) {
          REXSYS_WARN(
              "Mod '{}' requires game_version >= {}, but this build has no valid version to "
              "check against (RuntimeConfig::game_version was never set); skipping the version "
              "check",
              mod.folder_name, mod.min_game_version);
        } else if (CompareVersions(have, want) < 0) {
          REXSYS_ERROR("Mod '{}' requires game_version >= {}, but this build is only version {}",
                       mod.folder_name, mod.min_game_version, game_version_);
          ok = false;
        }
      }
    }

    for (auto& other : mod.load_after_mods) {
      auto it = index_of.find(other);
      if (it == index_of.end()) {
        REXSYS_WARN("Mod '{}' should load after '{}', which is not enabled", mod.folder_name,
                    other);
      } else if (it->second > i) {
        REXSYS_WARN("Mod '{}' should load after '{}', but is listed before it in enabled_mods",
                    mod.folder_name, other);
      }
    }

    for (auto& other : mod.conflicts_mods) {
      if (other == mod.folder_name) {
        REXSYS_ERROR("Mod '{}' lists itself in 'conflicts'", mod.folder_name);
        ok = false;
        continue;
      }
      if (index_of.contains(other)) {
        REXSYS_ERROR("Mod '{}' conflicts with '{}', but both are enabled", mod.folder_name, other);
        ok = false;
      }
    }
  }
  return ok;
}

bool Runtime::SetupVfs() {
  if (game_data_root_.empty()) {
    REXSYS_WARN("Runtime::SetupVfs: No game_data_root specified, skipping VFS setup");
    return true;
  }

  auto abs_game_root = std::filesystem::absolute(game_data_root_);
  if (!std::filesystem::exists(abs_game_root)) {
    REXSYS_ERROR("Runtime::SetupVfs: game_data_root does not exist: {}", abs_game_root.string());
    return false;
  }

  // Resolve enabled mods once; reused for game, update and (later) DLC overlays.
  ResolveEnabledMods();

  // Mount game_data_root as \Device\Harddisk0\Partition1
  auto mount_path = "\\Device\\Harddisk0\\Partition1";
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      mount_path, abs_game_root, !REXCVAR_GET(allow_game_relative_writes));
  device->set_overlay_roots(ModOverlayRoots("game"));
  if (!device->Initialize()) {
    REXSYS_ERROR("Runtime::SetupVfs: Failed to initialize host path device");
    return false;
  }
  if (!file_system_->RegisterDevice(std::move(device))) {
    REXSYS_ERROR("Runtime::SetupVfs: Failed to register host path device");
    return false;
  }
  REXSYS_INFO("  Mounted {} at {}", abs_game_root.string(), mount_path);

  // Register symbolic links for game: and D:
  file_system_->RegisterSymbolicLink("game:", mount_path);
  file_system_->RegisterSymbolicLink("d:", mount_path);
  REXSYS_DEBUG("  Registered symbolic links: game:, d:");

  // Mount update_data_root as update:\ if provided
  if (!update_data_root_.empty()) {
    auto abs_update_root = std::filesystem::absolute(update_data_root_);
    if (std::filesystem::exists(abs_update_root)) {
      auto update_mount = "\\Device\\Harddisk0\\PartitionUpdate";
      auto update_device =
          std::make_unique<rex::filesystem::HostPathDevice>(update_mount, abs_update_root, true);
      update_device->set_overlay_roots(ModOverlayRoots("update"));
      if (update_device->Initialize() && file_system_->RegisterDevice(std::move(update_device))) {
        file_system_->RegisterSymbolicLink("update:", update_mount);
        REXSYS_INFO("  Mounted {} at update:", abs_update_root.string());
      }
    }
  }

  // Setup NullDevice for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // Using a NullDevice returns success to all IO requests, allowing games
  // to believe cache/raw disk was accessed successfully.
  // NOTE: Must be registered AFTER Partition1 so Partition1 requests don't
  // go to NullDevice (VFS resolves devices in registration order)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"), std::string("\\Cache1")};
  auto null_device =
      std::make_unique<rex::filesystem::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
    REXSYS_DEBUG("  Registered NullDevice for \\Device\\Harddisk0\\{{Partition0,Cache0,Cache1}}");
  }

  // NOTE: Do NOT register a device for cache: paths
  // Games handle "device not found" gracefully but don't handle actual device
  // errors (like NAME_COLLISION) well. Let cache: fail cleanly.

  return true;
}

X_STATUS Runtime::LoadXexImage(const std::string_view module_path) {
  REXSYS_INFO("Loading XEX image: {}", std::string(module_path));

  auto module = system::object_ref<system::UserModule>(new system::UserModule(kernel_state_.get()));
  X_STATUS status = module->LoadFromFile(module_path);
  if (XFAILED(status)) {
    REXSYS_ERROR("Runtime::LoadXexImage: Failed to load module, status {:08X}", status);
    return status;
  }

  kernel_state_->SetExecutableModule(module);
  REXSYS_DEBUG("  XEX image loaded successfully");
  return X_STATUS_SUCCESS;
}

system::object_ref<system::XThread> Runtime::PrepareModuleLaunch() {
  auto executable = kernel_state_->GetExecutableModule();
  if (!executable) {
    REXSYS_ERROR("Runtime::PrepareModuleLaunch: No executable module loaded");
    return nullptr;
  }

  auto thread = kernel_state_->PrepareModuleLaunch(executable);
  if (!thread) {
    REXSYS_ERROR("Runtime::PrepareModuleLaunch: Failed to prepare module");
    return nullptr;
  }

  REXSYS_DEBUG("  Module prepared on thread '{}'", thread->name());
  return thread;
}

system::object_ref<system::XThread> Runtime::LaunchModule() {
  auto thread = PrepareModuleLaunch();
  if (thread) {
    thread->Resume();
    REXSYS_DEBUG("  Module launched on thread '{}'", thread->name());
  }
  return thread;
}

}  // namespace rex
