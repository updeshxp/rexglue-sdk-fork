/**
 * @file        rex/rex_app.h
 * @brief       ReXApp - base class for recompiled windowed applications
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/image_info.h>
#include <rex/runtime.h>
#include <rex/system/mod_plugin.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/shader_debugger_overlay.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app.h>

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex {

class LogCaptureSink;

/// Content path configuration, passed to OnConfigurePaths().
/// All paths start with sensible defaults derived from CLI args and cvars.
/// Subclasses may override any field before Runtime is constructed.
struct PathConfig {
  std::filesystem::path game_data_root;
  std::filesystem::path user_data_root;
  std::filesystem::path update_data_root;
  std::filesystem::path cache_root;
  std::filesystem::path metadata_root;
  std::filesystem::path config_path;
};

namespace ui {
class AchievementNotificationDialog;
class ConsoleDialog;
class SettingsDialog;
class ShaderDebuggerDialog;
}  // namespace ui

/// Base class for recompiled Xbox 360 applications.
///
/// OnInitialize is a thin coordinator that runs four phases in order:
///
///   SetupEnvironment  -> paths, config, logging
///   SetupPresentation -> window, graphics presentation, ImGui drawer
///   OnFinalizePaths   -> hook for wizard-driven path resolution (sync or async)
///   ConstructRuntime  -> Runtime, guest GPU init, XEX load, rexcrt heap
///   LaunchModule      -> shader cache, PrepareModuleLaunch, background wait
///
/// Each phase is a protected virtual; consumers override selectively without
/// re-implementing the whole flow.
///
/// Subclass skeleton:
/// @code
///   // src/my_app_app.h (yours to customize)
///   class MyApp : public rex::ReXApp {
///   public:
///       using rex::ReXApp::ReXApp;
///       static std::unique_ptr<rex::ui::WindowedApp> Create(
///           rex::ui::WindowedAppContext& ctx) {
///         return std::unique_ptr<MyApp>(new MyApp(ctx, "my_app",
///             PPCImageConfig));
///       }
///       // Override hooks: OnPreSetup, OnPostSetup, OnCreateDialogs,
///       // OnConfigureFonts, OnFinalizePaths, etc.
///   };
///
///   // src/main.cpp
///   #include "generated/my_app_init.h"
///   #include "my_app_app.h"
///   REX_DEFINE_APP(my_app, MyApp::Create)
/// @endcode
class ReXApp : public ui::WindowedApp, public ui::WindowListener, public ui::WindowInputListener {
 public:
  ~ReXApp() override;

 protected:
  ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
         std::string_view usage = "");

  // --- Virtual hooks for customization ---

  /// Called before Runtime::Setup(). Override to modify backend config.
  virtual void OnPreSetup(RuntimeConfig& config) {}

  /// Called before Runtime::LoadXexImage(). Override to modify xex image.
  virtual void OnLoadXexImage(std::string& xex_image) {}

  /// Called after runtime is fully initialized, before window creation.
  virtual void OnPostSetup() {}

  /// Called after ImGui drawer is created. Add custom dialogs here.
  virtual void OnCreateDialogs(ui::ImGuiDrawer* drawer) { (void)drawer; }

  /// Called before cleanup begins. Release custom resources here.
  virtual void OnShutdown() {}

  /// Called after path defaults are computed, before Runtime is constructed.
  /// Override to adjust game/user/update data paths programmatically.
  virtual void OnConfigurePaths(PathConfig& paths) { (void)paths; }

  /// Called after SetupPresentation returns (window and ImGui drawer are live)
  /// and before Runtime construction. Override to resolve paths from user
  /// input shown through an ImGui dialog.
  ///
  /// Return a PathConfig to continue initialization synchronously. Return
  /// std::nullopt and invoke `resume(path_config)` later (e.g. from a wizard
  /// completion handler) to continue asynchronously. `resume` must be called
  /// on the UI thread. Calling `resume` after the app has begun shutdown is
  /// a no-op.
  ///
  /// Default implementation returns `defaults` unchanged.
  virtual std::optional<PathConfig> OnFinalizePaths(const PathConfig& defaults,
                                                    std::function<void(PathConfig)> resume) {
    (void)resume;
    return defaults;
  }

  /// Called from the ImGui drawer's Initialize() after the default font is
  /// registered and before the atlas is built. Override to add additional
  /// fonts via AddFontFromMemoryTTF() or similar.
  virtual void OnConfigureFonts(ImFontAtlas* atlas) { (void)atlas; }

  /// Called from the ImGui drawer's Initialize() after the default style
  /// colors are applied. Override to recolor the overlay UI (dialogs,
  /// console, debug/settings overlays) to fit the game being recompiled.
  virtual void OnConfigureStyle(ImGuiStyle& style) { (void)style; }

  /// Called after logging is initialized. Add log sinks here.
  virtual void OnPostInitLogging() {}

  /// Called after Runtime::LoadXexImage() succeeds. The XEX is loaded and
  /// mapped into guest memory but the module has not launched.
  /// Use this for data patches and recomp-specific achievement registration.
  virtual void OnPostLoadXexImage() {}

  /// Called immediately before the main guest thread is created.
  /// Everything is set up -- last chance to patch guest memory/code.
  virtual void OnPreLaunchModule() {}

  /// Called after the main guest thread is created but before it starts
  /// executing. The thread is suspended -- attach debuggers/monitors here.
  virtual void OnPostLaunchModule(system::XThread* thread) { (void)thread; }

  /// Called when the main guest thread exits. The runtime is still alive.
  /// Use for cleanup that depends on runtime resources.
  virtual void OnGuestThreadExit(system::XThread* thread) { (void)thread; }

  /// Detached overlay mode ("bring your own renderer"). Called once from
  /// SetupPresentation when the SDK has no graphics backend
  /// (config.graphics == nullptr, typically cleared in OnPreSetup) and the app
  /// renders the guest itself. Return a unique_ptr to a ui::ImmediateDrawer
  /// subclass that creates textures and submits via your renderer. ReXApp owns
  /// the returned drawer (stored in immediate_drawer_, torn down after
  /// imgui_drawer_).
  ///
  /// Construct the drawer presenter-less. REQUIRED CONTRACT: your CreateTexture
  /// override MUST return nullptr (never crash or assert) when its GPU device
  /// is not yet available, because the SDK uploads the ImGui font atlas lazily
  /// on the first Draw and the device may only come up later (e.g. in the guest
  /// D3D device-creation hook). NOTE: ImmediateDrawer::OnEnterPresenter() /
  /// OnLeavePresenter() are NOT invoked in detached mode (the SDK never calls
  /// SetPresenter with a non-null presenter on your drawer), so perform any
  /// per-renderer GPU init lazily (on first CreateTexture/Begin), not in
  /// OnEnterPresenter. You also own present timing / vsync / letterbox in this
  /// mode.
  ///
  /// See ui::AppUIDrawContext for the per-frame draw-context handoff. Default:
  /// no overlay (SDK presenter mode; this hook is never reached).
  virtual std::unique_ptr<ui::ImmediateDrawer> OnCreateImmediateDrawer() { return nullptr; }

  // --- Window event hooks (delivered on the UI thread) ---

  /// Logical (DPI-independent) client size changed.
  virtual void OnWindowResized(uint32_t logical_width, uint32_t logical_height) {
    (void)logical_width;
    (void)logical_height;
  }

  /// Physical pixel size changed. Use this to resize swap chains.
  virtual void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) {
    (void)pixel_width;
    (void)pixel_height;
  }

  /// The user asked to close the window (close button, Alt+F4). Return false
  /// to veto and close later explicitly (window()->RequestClose()) after
  /// stopping guest threads and draining renderers. Default accepts; the
  /// window then closes and the app quits via the OnClosing path.
  virtual bool OnWindowCloseRequested() { return true; }

  virtual void OnWindowFocusChanged(bool focused) { (void)focused; }

  /// Display scale changed (window moved to a monitor with different DPI).
  /// scale is 1.0 at 96 DPI.
  virtual void OnDpiScaleChanged(float scale) { (void)scale; }

  virtual void OnWindowMinimized() {}
  virtual void OnWindowRestored() {}

  /// Creates the overlay toggled by bind_achievements. Override to replace the
  /// built-in achievement UI. Returning nullptr disables the overlay.
  virtual std::unique_ptr<ui::ImGuiDialog> CreateAchievementsOverlay();

  /// Creates the achievement notification UI. Override to replace the
  /// built-in toast renderer. Returning nullptr disables notifications.
  virtual std::unique_ptr<ui::AchievementNotificationDialog> CreateAchievementNotificationDialog();

  /// Creates the overlay toggled by bind_settings (F4) when the
  /// `settings_manager_enabled` cvar is true. Override to provide a
  /// game-curated settings UI in place of the built-in developer settings
  /// panel (which lists every registered cvar). Returning nullptr falls back
  /// to the developer panel, so this is a no-op by default.
  virtual std::unique_ptr<ui::ImGuiDialog> OnCreateUserSettingsOverlay() { return nullptr; }

  // --- Init phase methods (called in order from OnInitialize) ---

  /// Resolve path defaults, load config TOML, initialize logging.
  /// Populates `resolved_defaults_` with the PathConfig produced by
  /// OnConfigurePaths.
  virtual bool SetupEnvironment();

  /// Re-resolve `resolved_defaults_` if any of the path cvars changed after
  /// SetupEnvironment computed them. A subclass override of SetupEnvironment
  /// (or a wizard it runs, such as GameDataSelector) commonly sets
  /// game_data_root/update_data_root *after* the base class has already
  /// snapshotted them; without this the run would keep using the stale
  /// defaults and only pick up the new ones on the next launch.
  void RefreshPathDefaultsIfCvarsChanged();

  /// Construct Runtime with the given paths, call runtime_->Setup, load the
  /// XEX image, initialize the rexcrt heap. Runs OnPostSetup at the end.
  virtual bool ConstructRuntime(const PathConfig& paths);

  /// Create the window, stand up graphics presentation, create the ImGui
  /// drawer, register overlay keybinds, run OnCreateDialogs.
  virtual bool SetupPresentation();

  /// Kick off the deferred module launch: shader storage init,
  /// PrepareModuleLaunch, main thread resume, background wait.
  virtual void LaunchModule();

  // --- Accessors for subclass use ---
  Runtime* runtime() const { return runtime_.get(); }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }
  ui::ImmediateDrawer* immediate_drawer() const { return immediate_drawer_.get(); }
  system::AchievementManager& achievements() const;

  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  const std::filesystem::path& metadata_root() const { return metadata_root_; }

  /// Path to the app's cvar config TOML (exe_dir / "<name>.toml"), as loaded
  /// in SetupEnvironment and passed to the built-in SettingsDialog. Useful as
  /// a base path for a subclass's own OnCreateUserSettingsOverlay.
  const std::filesystem::path& config_path() const { return config_path_; }

  /// Set a callback that provides guest frame stats to the debug overlay.
  void SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider);

  // Overrides the shader debugger overlay's (F2) data source.
  struct ShaderDebuggerOverride {
    ui::ShaderDebuggerDialog::SnapshotProvider snapshot_provider;
    ui::ShaderDebuggerDialog::DisableSetter disable_setter;
    ui::ShaderDebuggerDialog::DetailsProvider details_provider;
    ui::ShaderDebuggerDialog::BinaryReplacer binary_replacer;
    ui::ShaderDebuggerDialog::ProfilingToggle profiling_toggle;
    ui::ShaderDebuggerDialog::ProfilingResetter profiling_resetter;
  };
  void SetShaderDebuggerOverride(ShaderDebuggerOverride override);

 private:
  std::function<void(PathConfig)> MakeResumeCallback();

  // Stand up the ImGui overlay stack (drawer, F3/Backtick/F4 binds, dialogs)
  // independently of how the presenter/drawer were obtained. `presenter` may be
  // null (detached mode).
  void SetupOverlays(ui::Presenter* presenter, ui::ImmediateDrawer* drawer);

  // WindowedApp overrides
  bool OnInitialize() override;
  void OnDestroy() override;

  // WindowListener overrides
  void OnClosing(ui::UIEvent& e) override;
  bool OnCloseRequested(ui::UIEvent& e) override;
  void OnResize(ui::UISetupEvent& e) override;
  void OnDpiChanged(ui::UISetupEvent& e) override;
  void OnGotFocus(ui::UISetupEvent& e) override;
  void OnLostFocus(ui::UISetupEvent& e) override;
  void OnMinimized(ui::UIEvent& e) override;
  void OnRestored(ui::UIEvent& e) override;

  // WindowInputListener overrides
  void OnKeyDown(ui::KeyEvent& e) override;

  // Resolve the five path cvars into a PathConfig and run OnConfigurePaths.
  PathConfig ResolvePathDefaults();

  PPCImageInfo ppc_info_;
  PathConfig resolved_defaults_;
  // Values the path cvars had when resolved_defaults_ was last computed.
  std::vector<std::string> path_cvar_snapshot_;
  RuntimeConfig config_;
  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path metadata_root_;
  std::unique_ptr<Runtime> runtime_;
  std::unique_ptr<ui::Window> window_;
  std::thread module_thread_;
  std::atomic<bool> shutting_down_{false};
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;

  // Built-in overlays
  std::shared_ptr<LogCaptureSink> log_sink_;
  std::unique_ptr<ui::DebugOverlayDialog> debug_overlay_;
  std::unique_ptr<ui::ConsoleDialog> console_overlay_;
  std::unique_ptr<ui::SettingsDialog> settings_overlay_;
  std::unique_ptr<ui::ImGuiDialog> user_settings_overlay_;
  std::unique_ptr<ui::ImGuiDialog> achievements_overlay_;
  std::shared_ptr<ui::AchievementNotificationDialog> achievement_notification_;
  uint64_t achievement_notification_listener_ = 0;
  std::unique_ptr<ui::ShaderDebuggerDialog> shader_debugger_overlay_;
  ui::DebugOverlayDialog::FrameStatsProvider frame_stats_provider_;
  ShaderDebuggerOverride shader_debugger_override_;
  std::filesystem::path config_path_;

  // Mod code plugins declared by enabled mods (mod.toml `code = "..."`).
  // Loaded once ImGui/keybinds exist (see SetupOverlays), notified again once
  // KernelState is live (see LaunchModule), and shut down before the rest of
  // ReXApp tears down. Never unloaded, mirroring GPU plugin lifetime.
  //
  // mod_infos_ backs the ModHostContext.mod_root/mod_name string pointers
  // handed to each plugin at creation: those pointers are documented to
  // remain valid for the mod's lifetime, so their backing strings must live
  // at least as long as ReXApp itself, not just the SetupOverlays call that
  // builds them.
  std::unique_ptr<ui::ImGuiDialog> mod_manager_overlay_;
  // Gamepad-triggered menu (default Y) listing every overlay -- base app and
  // mod -- that exposes visibility state via RegisterBind's is_visible
  // parameter. Always constructed (never toggled itself off via reset like
  // the others): it must exist continuously for its bind to remain live.
  // See overlay_menu.h.
  std::unique_ptr<ui::ImGuiDialog> overlay_menu_;
  // Two-mode (Gameplay/UI) gamepad controller, toggled by the guide button
  // (see gamepad_ui.h). Always constructed, for the same reason as
  // overlay_menu_ above: it must exist continuously to poll guide/
  // bind_ui_mode and, in Gameplay mode, PollGamepadBinds for every
  // gamepad-keyed bind (a duty this used to belong to overlay_menu_ alone).
  std::unique_ptr<ui::ImGuiDialog> gamepad_ui_;
  std::vector<system::ModInfo> mod_infos_;
  // Parallel to mod_infos_: narrow (UTF-8) form of each mod_root, since
  // ModHostContext.mod_root is a const char* but ModInfo::mod_root is a
  // std::filesystem::path (native-encoded, wchar_t on Windows).
  std::vector<std::string> mod_root_strs_;
  std::vector<std::unique_ptr<system::IModPlugin>> mod_plugins_;
  // Parallel to mod_plugins_ (not mod_infos_: asset-only mods are skipped
  // when building mod_plugins_, so the indices don't otherwise line up).
  // Used to attribute each lifecycle call via ScopedActiveMod -- see
  // mod_attribution.h -- for keybind auto-reassignment and cvar-override
  // conflict detection.
  std::vector<std::string> mod_plugin_owners_;
};

}  // namespace rex
