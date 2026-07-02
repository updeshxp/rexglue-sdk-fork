/**
 * @file        rex/ui/overlay/shader_debugger_overlay.h
 *
 * @brief       ImGui shader debugger overlay -- lists the pixel and vertex
 *              shaders currently tracked by the graphics backend and lets the
 *              user toggle individual shaders off and on at runtime.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// Plain-data description of a single shader, mirroring the small subset of
// graphics::CommandProcessor::ShaderInfo the dialog needs. This intentionally
// avoids dragging the graphics module into the public UI header.
struct ShaderDebuggerEntry {
  uint64_t ucode_hash = 0;
  // 0 = Vertex, 1 = Pixel (matches xenos::ShaderType ordering).
  uint32_t type = 0;
  uint32_t dword_count = 0;
  bool disabled = false;
  bool active = false;
  // CPU time accumulated by the backend while this shader was bound, in
  // nanoseconds. Only populated while profiling is enabled.
  uint64_t profile_total_ns = 0;
  uint64_t profile_draw_count = 0;
};

// Per-translation snapshot for the viewer pane.
struct ShaderDebuggerTranslation {
  uint64_t modification = 0;
  bool is_translated = false;
  bool is_valid = false;
  std::string host_disassembly;
  std::vector<uint8_t> translated_binary;
};

// Full per-shader detail surfaced to the viewer/editor pane.
struct ShaderDebuggerDetails {
  bool found = false;
  ShaderDebuggerEntry info;
  std::string ucode_disassembly;
  std::vector<uint32_t> ucode_dwords;
  std::vector<ShaderDebuggerTranslation> translations;
};

class ShaderDebuggerDialog : public ImGuiDialog {
 public:
  using SnapshotProvider = std::function<std::vector<ShaderDebuggerEntry>()>;
  using DisableSetter = std::function<void(uint64_t ucode_hash, bool disabled)>;
  using DetailsProvider = std::function<ShaderDebuggerDetails(uint64_t ucode_hash)>;
  using BinaryReplacer =
      std::function<bool(uint64_t ucode_hash, uint64_t modification, std::vector<uint8_t> binary)>;
  // Toggles backend per-shader timing collection. The dialog enables this in
  // its constructor and disables it in its destructor so there's no overhead
  // while the debugger is closed.
  using ProfilingToggle = std::function<void(bool enabled)>;
  using ProfilingResetter = std::function<void()>;

  ShaderDebuggerDialog(ImGuiDrawer* imgui_drawer, SnapshotProvider snapshot_provider,
                       DisableSetter disable_setter, DetailsProvider details_provider,
                       BinaryReplacer binary_replacer, ProfilingToggle profiling_toggle,
                       ProfilingResetter profiling_resetter,
                       std::filesystem::path shaders_toml_path = {});
  ~ShaderDebuggerDialog();

  // Reads a shaders.toml file and returns the set of shader hashes whose
  // entry has disabled=true. Intended to be called by the application at
  // startup so the persistent blacklist can be installed before any shader
  // is loaded. Returns an empty vector if the file is missing or unreadable.
  static std::vector<uint64_t> ReadShaderBlacklistFromToml(const std::filesystem::path& path);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawShaderTable();
  void DrawDetailsPanel();
  void RefreshSelectedDetails();
  // Returns the user-assigned name for a hash, or empty string if unset.
  std::string LookupName(uint64_t hash) const;
  // Returns the persisted disabled flag for a hash (false if no entry).
  bool LookupDisabled(uint64_t hash) const;
  // Sets/clears a name and persists shaders.toml. Empty name + disabled=false
  // removes the entry entirely.
  void SetName(uint64_t hash, std::string name);
  // Persists the disabled flag for a hash and saves shaders.toml.
  void SetDisabled(uint64_t hash, bool disabled);
  void LoadShadersFromDisk();
  void SaveShadersToDisk() const;

  SnapshotProvider snapshot_provider_;
  DisableSetter disable_setter_;
  DetailsProvider details_provider_;
  BinaryReplacer binary_replacer_;
  ProfilingToggle profiling_toggle_;
  ProfilingResetter profiling_resetter_;

  char filter_buf_[128] = {};
  bool show_vertex_ = true;
  bool show_pixel_ = true;
  bool show_only_active_ = false;
  bool auto_refresh_ = true;
  std::vector<ShaderDebuggerEntry> cached_;

  bool has_selection_ = false;
  uint64_t selected_hash_ = 0;
  ShaderDebuggerDetails selected_details_;
  // Edit buffer for the currently selected translation's host disassembly.
  // We keep this separate from the live translation so the user can iterate
  // without their text being clobbered by background refreshes.
  int selected_translation_idx_ = 0;
  std::vector<char> edit_buffer_;
  // User-supplied path used for "Save binary..." / "Load binary..." buttons.
  char binary_path_buf_[512] = {};
  std::string status_message_;

  // Per-hash settings persisted to disk.
  struct ShaderTomlEntry {
    std::string name;
    bool disabled = false;
  };
  // Persistent per-hash documentation names + disable flags. Loaded from /
  // saved to shaders_toml_path_ if non-empty.
  std::filesystem::path shaders_toml_path_;
  std::unordered_map<uint64_t, ShaderTomlEntry> shader_entries_;
  // Edit buffer for the rename input in the details panel.
  char rename_buf_[128] = {};
};

}  // namespace rex::ui
