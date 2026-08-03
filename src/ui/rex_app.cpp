/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation - compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <cstdlib>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/hint_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/gamepad_ui.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/overlay/mod_manager_overlay.h>
#include <rex/ui/overlay/overlay_menu.h>
#include <rex/ui/overlay/shader_debugger_overlay.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/mod_attribution.h>
#include <rex/system/mod_conflict_tracker.h>
#include <rex/system/mod_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <unordered_map>

REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(gpu_backend, "any", "GPU", "Graphics backend: 'any', 'd3d12', or 'vulkan'")
    .allowed({"any", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(settings_manager_enabled, true, "UI",
                    "Use the application's user-facing settings overlay (via "
                    "ReXApp::OnCreateUserSettingsOverlay) instead of the built-in developer "
                    "settings panel when F4 is pressed");

namespace rex {

namespace detail {

// Snapshot-diff helpers for cvar-override conflict detection (see
// mod_conflict_tracker.h). Deliberately doesn't touch the cvar core: takes a
// before/after read of every non-command cvar's current value around a mod's
// lifecycle call and records what changed, attributing it to that mod. This
// only covers changes made synchronously during OnCreateDialogs/
// OnModuleLaunched -- a mod that changes a cvar later (e.g. from a tick
// callback or its own UI) isn't attributed here.
std::unordered_map<std::string, std::string> SnapshotCvarValues() {
  std::unordered_map<std::string, std::string> snapshot;
  for (auto& entry : rex::cvar::GetRegistry()) {
    if (entry.type == rex::cvar::FlagType::Command || !entry.getter) {
      continue;
    }
    snapshot.emplace(entry.name, entry.getter());
  }
  return snapshot;
}

void RecordCvarDiff(rex::Runtime* runtime, const std::string& mod_name,
                    const std::unordered_map<std::string, std::string>& before) {
  auto* tracker = runtime ? runtime->mod_conflict_tracker() : nullptr;
  if (!tracker) {
    return;
  }
  for (auto& entry : rex::cvar::GetRegistry()) {
    if (entry.type == rex::cvar::FlagType::Command || !entry.getter) {
      continue;
    }
    std::string new_value = entry.getter();
    auto it = before.find(entry.name);
    if (it == before.end()) {
      // A cvar registered by this mod during its own lifecycle call.
      tracker->RecordCvarActivity(
          mod_name, {.name = entry.name, .is_new_definition = true, .new_value = new_value});
    } else if (it->second != new_value) {
      tracker->RecordCvarActivity(mod_name, {.name = entry.name,
                                             .is_new_definition = false,
                                             .old_value = it->second,
                                             .new_value = new_value});
    }
  }
}

}  // namespace detail

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements());
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  // A subclass override of SetupEnvironment may have set path cvars after the
  // base class read them (GameDataSelector does exactly this when the user
  // points at an existing game directory). Pick those up now, so the choice
  // takes effect on this run rather than only on the next launch.
  RefreshPathDefaultsIfCvarsChanged();
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

PathConfig ReXApp::ResolvePathDefaults() {
  // Game data: cvar override, or empty (the wizard / CLI must supply one)
  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_root_cvar = REXCVAR_GET(cache_root);
  if (!cache_root_cvar.empty()) {
    cache_dir = cache_root_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  path_cvar_snapshot_ = {game_data_cvar, user_data_cvar, update_data_cvar, cache_root_cvar,
                         metadata_root_cvar};

  PathConfig path_config{game_dir, user_dir, update_dir, cache_dir, metadata_dir, config_path_};
  OnConfigurePaths(path_config);
  return path_config;
}

void ReXApp::RefreshPathDefaultsIfCvarsChanged() {
  const std::vector<std::string> current = {
      REXCVAR_GET(game_data_root), REXCVAR_GET(user_data_root), REXCVAR_GET(update_data_root),
      REXCVAR_GET(cache_root), REXCVAR_GET(metadata_root)};
  if (current == path_cvar_snapshot_) {
    return;
  }

  resolved_defaults_ = ResolvePathDefaults();
  game_data_root_ = resolved_defaults_.game_data_root;
  user_data_root_ = resolved_defaults_.user_data_root;
  update_data_root_ = resolved_defaults_.update_data_root;
  cache_root_ = resolved_defaults_.cache_root;
  metadata_root_ = resolved_defaults_.metadata_root;
  config_path_ = resolved_defaults_.config_path;

  REXLOG_INFO("Path cvars changed during setup; re-resolved paths");
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();
  config_path_ = exe_dir / (std::string(GetName()) + ".toml");

  // Load config FIRST so path and log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  resolved_defaults_ = ResolvePathDefaults();
  game_data_root_ = resolved_defaults_.game_data_root;
  user_data_root_ = resolved_defaults_.user_data_root;
  update_data_root_ = resolved_defaults_.update_data_root;
  cache_root_ = resolved_defaults_.cache_root;
  metadata_root_ = resolved_defaults_.metadata_root;
  config_path_ = resolved_defaults_.config_path;

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_INFO("  Metadata root:  {}", metadata_root_.string());
  }

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;

  runtime_ =
      std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                     paths.update_data_root, paths.cache_root, paths.metadata_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        bool overlay_menu_open =
            overlay_menu_ && static_cast<ui::OverlayMenuDialog*>(overlay_menu_.get())->IsVisible();
        // In UI mode the gamepad_ui_ controller owns the controller entirely
        // (nav cursor, move/resize, etc.) regardless of mouse position, so
        // the game must stay gated off even if the mouse never touches an
        // overlay window.
        bool ui_mode_active =
            gamepad_ui_ && static_cast<ui::GamepadUiController*>(gamepad_ui_.get())->IsUiMode();
        if (!debug_overlay_ && !console_overlay_ && !settings_overlay_ && !user_settings_overlay_ &&
            !achievements_overlay_ && !shader_debugger_overlay_ && !overlay_menu_open &&
            !ui_mode_active)
          return true;
        return !ui_mode_active && !imgui_drawer_->GetIO().WantCaptureMouse;
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  // Load code plugins declared by enabled mods (mod.toml `code = "..."`) now
  // that Runtime exists (EnabledModsInfo needs it) and the ImGui drawer/
  // keybind registry are already live from SetupPresentation. KernelState is
  // not fully live yet here -- mods needing it should do so from
  // OnModuleLaunched() instead.
  {
    // Stored on `this` (not a local) because ctx.mod_root/mod_name below
    // point into these strings, and that pointer must stay valid for the
    // plugin's lifetime (see ModHostContext's contract), not just this call.
    mod_infos_ = runtime_->EnabledModsInfo();
    mod_root_strs_.clear();
    mod_root_strs_.reserve(mod_infos_.size());
    for (const auto& mod : mod_infos_) {
      mod_root_strs_.push_back(mod.mod_root.string());
    }

    rex::system::ModHostContext ctx{};
    ctx.runtime = runtime_.get();
    ctx.app_context = &app_context();
    ctx.window = window_.get();
    ctx.input_system = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    for (size_t i = 0; i < mod_infos_.size(); ++i) {
      const auto& mod = mod_infos_[i];
      if (mod.code.empty()) {
        continue;  // asset-only mod
      }
      ctx.mod_root = mod_root_strs_[i].c_str();
      ctx.mod_name = mod.folder_name.c_str();
      if (auto plugin = rex::system::LoadModPlugin(mod.mod_root, mod.folder_name, mod.code, ctx)) {
        mod_plugins_.push_back(std::move(plugin));
        mod_plugin_owners_.push_back(mod.folder_name);
      }
    }
  }
  if (imgui_drawer_) {
    for (size_t i = 0; i < mod_plugins_.size(); ++i) {
      const std::string& owner = mod_plugin_owners_[i];
      rex::system::ScopedActiveMod active_mod(owner);
      auto cvars_before = detail::SnapshotCvarValues();
      mod_plugins_[i]->OnCreateDialogs(imgui_drawer_.get());
      detail::RecordCvarDiff(runtime_.get(), owner, cvars_before);
    }
  }

  // Same timing NocturneRecomp's own OnPostSetup relies on for
  // FastForward/Achievements input binding ("window(), app_context() and the
  // input system are all live after setup") -- supply the overlay menu's
  // gamepad poll with an InputSystem* fetched at this point, not earlier
  // (SetupOverlays constructs the dialog before input_system() is reliably
  // non-null).
  if (gamepad_ui_) {
    auto* input_sys =
        runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr;
    static_cast<ui::GamepadUiController*>(gamepad_ui_.get())->SetInputSystem(input_sys);
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  config_.gpu_plugin = REXCVAR_GET(gpu_plugin);
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, REXCVAR_GET(gpu_backend));
    if (!config_.graphics) {
      // Fatal by design: no silent headless fallback.
      auto msg =
          fmt::format("Failed to load GPU plugin '{}'. See log for details.", config_.gpu_plugin);
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  // z_order 1: run before input drivers (z_order 0) so system keybinds are
  // consumed before they reach the game as raw key input.
  window_->AddInputListener(this, 1);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  rex::cvar::RegisterChangeCallback("fullscreen", [this](std::string_view, std::string_view value) {
    if (window_) {
      window_->SetFullscreen(value == "true");
    }
  });
  window_->Open();

  auto* graphics_system = config_.graphics.get();
  if (graphics_system && graphics_system->presenter()) {
    // SDK mode: the emulated-Xenos presenter drives the overlays.
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        SetupOverlays(presenter, immediate_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  } else if (!graphics_system) {
    // Detached mode: the app brings its own renderer and drives its own paint
    // loop. ReXApp owns the returned drawer via immediate_drawer_.
    immediate_drawer_ = OnCreateImmediateDrawer();
    if (immediate_drawer_) {
      SetupOverlays(/*presenter=*/nullptr, immediate_drawer_.get());
      // No window_->SetPresenter, no drawer SetPresenter: the app owns the
      // surface and the present cadence.
    }
  }

  return true;
}

void ReXApp::SetupOverlays(rex::ui::Presenter* presenter, rex::ui::ImmediateDrawer* drawer) {
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); },
      [this](ImGuiStyle& style) { OnConfigureStyle(style); });
  // presenter is nullptr in detached mode; ImGuiDrawer tolerates that and the
  // gated eager font upload in SetImmediateDrawer is skipped (font uploads
  // lazily on the first Draw instead).
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);
  rex::ui::RegisterBind(
      "bind_debug_overlay", "F3", "Toggle debug overlay",
      [this] {
        if (debug_overlay_) {
          debug_overlay_.reset();
        } else {
          debug_overlay_ =
              std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(), frame_stats_provider_);
        }
      },
      [this] { return static_cast<bool>(debug_overlay_); }, "Debug##overlay");
  rex::ui::RegisterBind(
      "bind_console", "Backtick", "Toggle console overlay",
      [this] {
        if (console_overlay_) {
          console_overlay_.reset();
        } else {
          console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
        }
      },
      [this] { return static_cast<bool>(console_overlay_); }, "Console##rex");
  rex::ui::RegisterBind(
      "bind_settings", "F4", "Toggle settings overlay",
      [this] {
        if (settings_overlay_ || user_settings_overlay_) {
          settings_overlay_.reset();
          user_settings_overlay_.reset();
        } else if (REXCVAR_GET(settings_manager_enabled)) {
          user_settings_overlay_ = OnCreateUserSettingsOverlay();
          if (!user_settings_overlay_) {
            // App opted in but didn't provide one; fall back to the dev panel.
            auto* input_sys = runtime_
                                  ? static_cast<rex::input::InputSystem*>(runtime_->input_system())
                                  : nullptr;
            settings_overlay_ =
                std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_, input_sys);
          }
        } else {
          auto* input_sys =
              runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr;
          settings_overlay_ =
              std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_, input_sys);
        }
      },
      [this] {
        return static_cast<bool>(settings_overlay_) || static_cast<bool>(user_settings_overlay_);
      },
      "Settings##rex");
  rex::ui::RegisterBind(
      "bind_mod_manager", "F1", "Toggle mod manager overlay",
      [this, drawer] {
        if (mod_manager_overlay_) {
          mod_manager_overlay_.reset();
        } else {
          mod_manager_overlay_ = std::make_unique<ui::ModManagerDialog>(
              imgui_drawer_.get(), drawer, runtime_.get(), window_.get(), config_path_);
        }
      },
      [this] { return static_cast<bool>(mod_manager_overlay_); }, "Mods##overlay");
  rex::ui::RegisterBind(
      "bind_achievements", "F7", "Toggle achievements overlay",
      [this] {
        if (achievements_overlay_) {
          achievements_overlay_.reset();
        } else {
          achievements_overlay_ = CreateAchievementsOverlay();
        }
      },
      [this] { return static_cast<bool>(achievements_overlay_); }, "Achievements##overlay");
  rex::ui::RegisterBind(
      "bind_renderdoc_capture", "F10", "Capture the next guest-rendered frame with RenderDoc",
      [this] {
        if (!runtime_)
          return;
        auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
        if (gs && gs->command_processor()) {
          gs->command_processor()->RequestRenderDocCapture();
        }
      },
      nullptr, "");
  overlay_menu_ = std::make_unique<ui::OverlayMenuDialog>(imgui_drawer_.get(), runtime_.get());
  rex::ui::RegisterBind(
      "bind_shader_debugger", "F2", "Toggle shader debugger overlay",
      [this] {
        if (shader_debugger_overlay_) {
          shader_debugger_overlay_.reset();
        } else {
          auto snapshot_provider = [this]() {
            if (shader_debugger_override_.snapshot_provider) {
              return shader_debugger_override_.snapshot_provider();
            }
            std::vector<ui::ShaderDebuggerEntry> out;
            if (!runtime_)
              return out;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return out;
            auto* cp = gs->command_processor();
            if (!cp)
              return out;
            auto snapshot = cp->GetShaderSnapshot();
            out.reserve(snapshot.size());
            for (const auto& s : snapshot) {
              ui::ShaderDebuggerEntry e;
              e.ucode_hash = s.ucode_hash;
              e.type = static_cast<uint32_t>(s.type);
              e.dword_count = s.dword_count;
              e.disabled = s.disabled;
              e.active = s.active;
              e.profile_total_ns = s.profile_total_ns;
              e.profile_draw_count = s.profile_draw_count;
              out.push_back(e);
            }
            return out;
          };
          auto disable_setter = [this](uint64_t hash, bool disabled) {
            if (shader_debugger_override_.disable_setter) {
              shader_debugger_override_.disable_setter(hash, disabled);
              return;
            }
            if (!runtime_)
              return;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return;
            auto* cp = gs->command_processor();
            if (!cp)
              return;
            // Route through the persistent blacklist so the toggle also applies
            // to future loads of the same shader, not just the one currently
            // resident in the cache. Both AddShaderBlacklist and
            // RemoveShaderBlacklist update already-loaded shaders too.
            if (disabled) {
              cp->AddShaderBlacklist(hash);
            } else {
              cp->RemoveShaderBlacklist(hash);
            }
          };
          auto details_provider = [this](uint64_t hash) {
            if (shader_debugger_override_.details_provider) {
              return shader_debugger_override_.details_provider(hash);
            }
            ui::ShaderDebuggerDetails out;
            if (!runtime_)
              return out;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return out;
            auto* cp = gs->command_processor();
            if (!cp)
              return out;
            auto details = cp->GetShaderDetails(hash);
            out.found = details.found;
            out.info.ucode_hash = details.info.ucode_hash;
            out.info.type = static_cast<uint32_t>(details.info.type);
            out.info.dword_count = details.info.dword_count;
            out.info.disabled = details.info.disabled;
            out.info.active = details.info.active;
            out.ucode_disassembly = std::move(details.ucode_disassembly);
            out.ucode_dwords = std::move(details.ucode_dwords);
            out.translations.reserve(details.translations.size());
            for (auto& t : details.translations) {
              ui::ShaderDebuggerTranslation tt;
              tt.modification = t.modification;
              tt.is_translated = t.is_translated;
              tt.is_valid = t.is_valid;
              tt.host_disassembly = std::move(t.host_disassembly);
              tt.translated_binary = std::move(t.translated_binary);
              out.translations.push_back(std::move(tt));
            }
            return out;
          };
          auto binary_replacer = [this](uint64_t hash, uint64_t modification,
                                        std::vector<uint8_t> binary) {
            if (shader_debugger_override_.binary_replacer) {
              return shader_debugger_override_.binary_replacer(hash, modification,
                                                               std::move(binary));
            }
            if (!runtime_)
              return false;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return false;
            auto* cp = gs->command_processor();
            if (!cp)
              return false;
            return cp->ReplaceShaderTranslationBinary(hash, modification, std::move(binary));
          };
          auto profiling_toggle = [this](bool enabled) {
            if (shader_debugger_override_.profiling_toggle) {
              shader_debugger_override_.profiling_toggle(enabled);
              return;
            }
            if (!runtime_)
              return;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return;
            auto* cp = gs->command_processor();
            if (!cp)
              return;
            cp->SetShaderProfilingEnabled(enabled);
          };
          auto profiling_resetter = [this]() {
            if (shader_debugger_override_.profiling_resetter) {
              shader_debugger_override_.profiling_resetter();
              return;
            }
            if (!runtime_)
              return;
            auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
            if (!gs)
              return;
            auto* cp = gs->command_processor();
            if (!cp)
              return;
            cp->ResetShaderProfiling();
          };
          const std::filesystem::path shaders_toml_path =
              runtime_ ? runtime_->ModDumpRoot() / "shaders.toml"
                       : std::filesystem::path("shaders.toml");
          shader_debugger_overlay_ = std::make_unique<ui::ShaderDebuggerDialog>(
              imgui_drawer_.get(), std::move(snapshot_provider), std::move(disable_setter),
              std::move(details_provider), std::move(binary_replacer), std::move(profiling_toggle),
              std::move(profiling_resetter), shaders_toml_path);
        }
      },
      [this] { return static_cast<bool>(shader_debugger_overlay_); }, "Shader Debugger");

  // Gamepad-driven mode controller (Gameplay <-> UI, see gamepad_ui.h).
  // Constructed alongside overlay_menu_ so it's always live for the whole
  // app lifetime, same rationale as that dialog: it must exist continuously
  // to keep polling the guide button / bind_ui_mode regardless of whether
  // any overlay is currently open.
  gamepad_ui_ = std::make_unique<ui::GamepadUiController>(imgui_drawer_.get(), runtime_.get());
  // Lets the overlay menu's own "Y" bind gate itself to UI mode (see
  // OverlayMenuDialog::SetUiModeQuery) -- wired here, after gamepad_ui_
  // exists, since overlay_menu_ is constructed earlier above.
  static_cast<ui::OverlayMenuDialog*>(overlay_menu_.get())->SetUiModeQuery([this] {
    return static_cast<ui::GamepadUiController*>(gamepad_ui_.get())->IsUiMode();
  });
  // Lets a newly-opened overlay (selected from the overlay menu) grab
  // gamepad focus right away -- see OverlayMenuDialog::SetOverlayShownCallback
  // and GamepadUiController::FocusOverlay.
  static_cast<ui::OverlayMenuDialog*>(overlay_menu_.get())
      ->SetOverlayShownCallback([this](const std::string& bind_name) {
        static_cast<ui::GamepadUiController*>(gamepad_ui_.get())->FocusOverlay(bind_name);
      });

  // Mod code plugins are loaded from ConstructRuntime (not here): Runtime --
  // and therefore EnabledModsInfo()/mod resolution -- doesn't exist until
  // ConstructRuntime runs, which happens *after* SetupPresentation (see the
  // phase order documented on ReXApp). SetupOverlays only registers binds
  // that don't need Runtime (F1/F3/F4/F7/etc.); mod plugin loading mirrors
  // that same constraint by waiting until Runtime is live.

  OnCreateDialogs(imgui_drawer_.get());
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    if (!achievement_notification_) {
      achievement_notification_ =
          std::shared_ptr<ui::AchievementNotificationDialog>(CreateAchievementNotificationDialog());
    }
    if (achievement_notification_ && achievement_notification_listener_ == 0 && runtime_ &&
        runtime_->kernel_state()) {
      std::weak_ptr<ui::AchievementNotificationDialog> notification = achievement_notification_;
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [notification](const rex::system::AchievementEvent& event) {
            if (auto dialog = notification.lock()) {
              dialog->Push(event);
            }
          });
    }

    // Same "construct once, lazily" guard as achievement_notification_ above.
    // imgui_drawer_ is guaranteed live this deep into startup (SetupPresentation
    // already ran), so this is a safe, uniform place for it regardless of
    // which phase order a given app subclass uses.
    if (!hint_toast_ && imgui_drawer_ && runtime_ && !runtime_->startup_hint().empty()) {
      hint_toast_ = std::make_unique<ui::HintToastDialog>(imgui_drawer_.get());
      hint_toast_->Show(runtime_->startup_hint());
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system = runtime_->graphics_system();
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    const std::filesystem::path dump_root = runtime_->ModDumpRoot();
    if (graphics_system) {
      rex::system::AssetReplacementConfig asset_replacement_config;
      asset_replacement_config.texture_mod_roots = runtime_->ModOverlayRoots("textures");
      asset_replacement_config.shader_mod_roots = runtime_->ModOverlayRoots("shaders");
      asset_replacement_config.dump_root = dump_root;
      graphics_system->InitializeAssetReplacement(asset_replacement_config);
    }

    // Apply persistent shader blacklist from shaders.toml so disabled shaders
    // are skipped from the very first draw, without requiring the user to
    // open the F2 shader debugger overlay first.
    if (auto* gs = static_cast<rex::graphics::GraphicsSystem*>(graphics_system)) {
      if (auto* cp = gs->command_processor()) {
        auto blacklist =
            ui::ShaderDebuggerDialog::ReadShaderBlacklistFromToml(dump_root / "shaders.toml");
        for (uint64_t hash : blacklist) {
          cp->AddShaderBlacklist(hash);
        }
      }
    }

    // KernelState/apps are fully live now; notify mod plugins before the
    // project's own OnPostLaunchModule so a mod's own memory/app scans (e.g.
    // ScanFilesystem) are ready by the time the guest starts running.
    for (size_t i = 0; i < mod_plugins_.size(); ++i) {
      const std::string& owner = mod_plugin_owners_[i];
      rex::system::ScopedActiveMod active_mod(owner);
      auto cvars_before = detail::SnapshotCvarValues();
      mod_plugins_[i]->OnModuleLaunched();
      detail::RecordCvarDiff(runtime_.get(), owner, cvars_before);
    }

    OnPostLaunchModule(main_thread.get());
    main_thread->Resume();

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  // Alt+Enter is a hardcoded OS convention, not a user-rebindable keybind.
  if (e.virtual_key() == ui::VirtualKey::kReturn && e.is_alt_pressed()) {
    rex::cvar::SetFlagByName("fullscreen", REXCVAR_GET(fullscreen) ? "false" : "true");
    e.set_handled(true);
    return;
  }
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  // Hard-exit rather than run subsystem teardown, which can deadlock on a host
  // lock still held by a straggler TerminateTitle left running. Flush (not
  // ShutdownLogging, which frees loggers a straggler may still use); the OS
  // reclaims the rest.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
  std::_Exit(0);
}

bool ReXApp::OnCloseRequested(ui::UIEvent& e) {
  (void)e;
  return OnWindowCloseRequested();
}

void ReXApp::OnResize(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnWindowPixelSizeChanged(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  OnWindowResized(window_->GetActualLogicalWidth(), window_->GetActualLogicalHeight());
}

void ReXApp::OnDpiChanged(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnDpiScaleChanged(float(window_->GetDpi()) / float(window_->GetMediumDpi()));
}

void ReXApp::OnGotFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(true);
}

void ReXApp::OnLostFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(false);
}

void ReXApp::OnMinimized(ui::UIEvent& e) {
  (void)e;
  OnWindowMinimized();
}

void ReXApp::OnRestored(ui::UIEvent& e) {
  (void)e;
  OnWindowRestored();
}

void ReXApp::OnFileDrop(ui::FileDropEvent& e) {
  std::string ext = e.filename().extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext != ".zip") {
    return;  // not a mod archive; ignore silently
  }
  if (!imgui_drawer_) {
    return;  // dropped before overlays exist (e.g. mid-setup); nothing to show progress in
  }

  if (!mod_manager_overlay_) {
    mod_manager_overlay_ = std::make_unique<ui::ModManagerDialog>(
        imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), window_.get(), config_path_);
  }
  static_cast<ui::ModManagerDialog*>(mod_manager_overlay_.get())->SideloadArchive(e.filename());
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // Notify mod plugins before their overlays/dialogs are torn down. Library
  // handles are intentionally kept loaded (see LoadModPlugin) -- guest
  // threads may still be executing plugin code pages at this point.
  for (auto& plugin : mod_plugins_) {
    plugin->OnShutdown();
  }
  mod_plugins_.clear();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_mod_manager");
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::UnregisterBind("bind_shader_debugger");

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  overlay_menu_.reset();  // its own destructor unregisters "bind_overlay_menu"
  mod_manager_overlay_.reset();
  settings_overlay_.reset();
  user_settings_overlay_.reset();
  console_overlay_.reset();
  shader_debugger_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // immediate_drawer_ was already unlinked from imgui_drawer_ above. Detach it
  // from its presenter so SDK mode runs OnLeavePresenter() before disposal; in
  // detached mode the drawer never had a presenter, so SetPresenter(nullptr) is
  // a no-op.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

void ReXApp::SetShaderDebuggerOverride(ShaderDebuggerOverride override) {
  shader_debugger_override_ = std::move(override);
}

}  // namespace rex
