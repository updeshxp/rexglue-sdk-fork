/**
 * @file        ui/overlay/shader_debugger_overlay.cpp
 *
 * @brief       Shader debugger overlay implementation. Lists all known
 *              vertex/pixel shaders, lets the user toggle them on/off, and
 *              opens a viewer/editor for the ucode disassembly and the host
 *              translations of any selected shader.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/shader_debugger_overlay.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

#include <imgui.h>

namespace rex::ui {

namespace {

const char* TypeLabel(uint32_t type) {
  switch (type) {
    case 0:
      return "VS";
    case 1:
      return "PS";
    default:
      return "??";
  }
}

bool MatchesFilter(const ShaderDebuggerEntry& entry, const char* filter) {
  if (!filter || !filter[0]) {
    return true;
  }
  char hash_str[32];
  std::snprintf(hash_str, sizeof(hash_str), "%016llx",
                static_cast<unsigned long long>(entry.ucode_hash));
  std::string needle(filter);
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string hay(hash_str);
  std::transform(hay.begin(), hay.end(), hay.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return hay.find(needle) != std::string::npos;
}

// ImGui's multi-line input wants a null-terminated, mutable buffer big enough
// to hold the contents plus growth headroom.
void LoadEditBuffer(std::vector<char>& buffer, const std::string& source) {
  size_t needed = source.size() + 4096;  // Headroom for typing.
  if (needed < 8192)
    needed = 8192;
  buffer.assign(needed, '\0');
  std::memcpy(buffer.data(), source.data(), source.size());
}

}  // namespace

ShaderDebuggerDialog::ShaderDebuggerDialog(
    ImGuiDrawer* imgui_drawer, SnapshotProvider snapshot_provider, DisableSetter disable_setter,
    DetailsProvider details_provider, BinaryReplacer binary_replacer,
    ProfilingToggle profiling_toggle, ProfilingResetter profiling_resetter,
    std::filesystem::path shaders_toml_path)
    : ImGuiDialog(imgui_drawer),
      snapshot_provider_(std::move(snapshot_provider)),
      disable_setter_(std::move(disable_setter)),
      details_provider_(std::move(details_provider)),
      binary_replacer_(std::move(binary_replacer)),
      profiling_toggle_(std::move(profiling_toggle)),
      profiling_resetter_(std::move(profiling_resetter)),
      shaders_toml_path_(std::move(shaders_toml_path)) {
  // Turn on per-shader timing for the lifetime of the dialog.
  if (profiling_toggle_)
    profiling_toggle_(true);
  if (profiling_resetter_)
    profiling_resetter_();
  LoadShadersFromDisk();
}

ShaderDebuggerDialog::~ShaderDebuggerDialog() {
  if (profiling_toggle_)
    profiling_toggle_(false);
}

std::string ShaderDebuggerDialog::LookupName(uint64_t hash) const {
  auto it = shader_entries_.find(hash);
  return it == shader_entries_.end() ? std::string{} : it->second.name;
}

bool ShaderDebuggerDialog::LookupDisabled(uint64_t hash) const {
  auto it = shader_entries_.find(hash);
  return it != shader_entries_.end() && it->second.disabled;
}

void ShaderDebuggerDialog::SetName(uint64_t hash, std::string name) {
  // Trim trailing whitespace; treat empty as a removal so we don't write
  // pointless entries to the toml.
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' ||
                           name.back() == '\n')) {
    name.pop_back();
  }
  auto it = shader_entries_.find(hash);
  if (name.empty()) {
    if (it != shader_entries_.end()) {
      it->second.name.clear();
      // Drop the entry entirely if there's no other reason to keep it.
      if (!it->second.disabled) {
        shader_entries_.erase(it);
      }
    }
  } else {
    if (it == shader_entries_.end()) {
      shader_entries_[hash] = ShaderTomlEntry{std::move(name), false};
    } else {
      it->second.name = std::move(name);
    }
  }
  SaveShadersToDisk();
}

void ShaderDebuggerDialog::SetDisabled(uint64_t hash, bool disabled) {
  auto it = shader_entries_.find(hash);
  if (it == shader_entries_.end()) {
    if (disabled) {
      shader_entries_[hash] = ShaderTomlEntry{std::string{}, true};
      SaveShadersToDisk();
    }
    return;
  }
  if (it->second.disabled == disabled)
    return;
  it->second.disabled = disabled;
  // If the entry now has no name and no disable flag, drop it entirely.
  if (!disabled && it->second.name.empty()) {
    shader_entries_.erase(it);
  }
  SaveShadersToDisk();
}

namespace {

// Minimal hand-rolled escape for TOML basic-string values. Names are short
// human-typed labels, so a comprehensive parser isn't needed.
std::string EscapeTomlString(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

// Inverse: handle the small subset of escapes EscapeTomlString produces.
std::string UnescapeTomlString(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      char n = in[++i];
      switch (n) {
        case '\\':
          out += '\\';
          break;
        case '"':
          out += '"';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u':
          if (i + 4 < in.size()) {
            unsigned int code = 0;
            std::sscanf(in.c_str() + i + 1, "%4x", &code);
            if (code < 0x80) {
              out += static_cast<char>(code);
            }
            i += 4;
          }
          break;
        default:
          out += n;
          break;
      }
    } else {
      out += c;
    }
  }
  return out;
}

// Strips leading/trailing ASCII whitespace from a string view.
std::string_view TrimView(std::string_view s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string_view::npos)
    return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Strips an inline TOML comment (an unquoted '#' through end of line) from
// the value portion of a line. We do a naive scan that respects double-quoted
// regions, which is enough for the inline-table values we emit.
std::string StripCommentRespectingQuotes(std::string_view in) {
  bool in_quote = false;
  bool escape = false;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_quote) {
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        in_quote = false;
      }
    } else {
      if (c == '"') {
        in_quote = true;
      } else if (c == '#') {
        return std::string(in.substr(0, i));
      }
    }
  }
  return std::string(in);
}

// Parses a single inline-table value of the form `{ name = "x", disabled = true }`.
// Missing keys default to empty/false. Returns false on a clearly malformed line.
bool ParseInlineEntry(std::string_view value, std::string& out_name, bool& out_disabled) {
  out_name.clear();
  out_disabled = false;
  std::string_view v = TrimView(value);
  if (v.size() < 2 || v.front() != '{' || v.back() != '}')
    return false;
  v = TrimView(v.substr(1, v.size() - 2));
  // Walk comma-separated key=value pairs, respecting quoted strings.
  size_t i = 0;
  while (i < v.size()) {
    // Find next '=' for the key.
    size_t eq = v.find('=', i);
    if (eq == std::string_view::npos)
      break;
    std::string key(TrimView(v.substr(i, eq - i)));
    // Find end of value: comma at top level (not inside quotes).
    size_t j = eq + 1;
    bool in_quote = false;
    bool escape = false;
    size_t end = v.size();
    for (; j < v.size(); ++j) {
      char c = v[j];
      if (escape) {
        escape = false;
        continue;
      }
      if (in_quote) {
        if (c == '\\') {
          escape = true;
          continue;
        }
        if (c == '"')
          in_quote = false;
      } else {
        if (c == '"')
          in_quote = true;
        else if (c == ',') {
          end = j;
          break;
        }
      }
    }
    std::string_view raw_value = TrimView(v.substr(eq + 1, end - eq - 1));
    if (key == "name") {
      if (raw_value.size() >= 2 && raw_value.front() == '"' && raw_value.back() == '"') {
        out_name = UnescapeTomlString(std::string(raw_value.substr(1, raw_value.size() - 2)));
      }
    } else if (key == "disabled") {
      out_disabled = (raw_value == "true");
    }
    if (end >= v.size())
      break;
    i = end + 1;
  }
  return true;
}

// Parses one shaders.toml line into (hash, name, disabled). Returns false if
// the line is blank/comment/malformed. Accepts both the new inline-table
// format and the legacy `"hash" = "name"` format from the previous revision.
bool ParseShaderTomlLine(const std::string& line, uint64_t& out_hash, std::string& out_name,
                         bool& out_disabled) {
  out_name.clear();
  out_disabled = false;
  std::string_view sv = TrimView(line);
  if (sv.empty() || sv.front() == '#')
    return false;
  size_t eq = sv.find('=');
  if (eq == std::string_view::npos)
    return false;
  std::string key(TrimView(sv.substr(0, eq)));
  // Accept bare or quoted hex hash key.
  if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
    key = key.substr(1, key.size() - 2);
  }
  if (key.empty())
    return false;
  uint64_t hash = 0;
  if (std::sscanf(key.c_str(), "%llx", reinterpret_cast<unsigned long long*>(&hash)) != 1) {
    return false;
  }
  std::string value_str = StripCommentRespectingQuotes(sv.substr(eq + 1));
  std::string_view value = TrimView(value_str);
  if (value.empty())
    return false;
  if (value.front() == '{') {
    if (!ParseInlineEntry(value, out_name, out_disabled))
      return false;
  } else if (value.front() == '"') {
    // Legacy format: just a quoted name, no disabled flag.
    size_t close = value.find_last_of('"');
    if (close == 0)
      return false;
    out_name = UnescapeTomlString(std::string(value.substr(1, close - 1)));
  } else {
    return false;
  }
  out_hash = hash;
  return true;
}

}  // namespace

void ShaderDebuggerDialog::LoadShadersFromDisk() {
  shader_entries_.clear();
  if (shaders_toml_path_.empty())
    return;
  std::ifstream in(shaders_toml_path_);
  if (!in)
    return;
  std::string line;
  while (std::getline(in, line)) {
    uint64_t hash = 0;
    std::string name;
    bool disabled = false;
    if (!ParseShaderTomlLine(line, hash, name, disabled))
      continue;
    if (name.empty() && !disabled)
      continue;
    shader_entries_[hash] = ShaderTomlEntry{std::move(name), disabled};
  }
}

void ShaderDebuggerDialog::SaveShadersToDisk() const {
  if (shaders_toml_path_.empty())
    return;
  // Make sure the parent directory exists.
  std::error_code ec;
  if (shaders_toml_path_.has_parent_path()) {
    std::filesystem::create_directories(shaders_toml_path_.parent_path(), ec);
  }
  std::ofstream out(shaders_toml_path_, std::ios::trunc);
  if (!out)
    return;
  out << "# Per-shader settings.\n";
  out << "# Format: <ucode_hash_hex> = { name = \"<name>\", disabled = <bool> }\n";
  // Sort by hash for stable diffs.
  std::vector<std::pair<uint64_t, ShaderTomlEntry>> ordered(shader_entries_.begin(),
                                                            shader_entries_.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& kv : ordered) {
    if (kv.second.name.empty() && !kv.second.disabled)
      continue;
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "%016llx", static_cast<unsigned long long>(kv.first));
    out << hash_buf << " = { name = \"" << EscapeTomlString(kv.second.name)
        << "\", disabled = " << (kv.second.disabled ? "true" : "false") << " }\n";
  }
}

std::vector<uint64_t> ShaderDebuggerDialog::ReadShaderBlacklistFromToml(
    const std::filesystem::path& path) {
  std::vector<uint64_t> out;
  if (path.empty())
    return out;
  std::ifstream in(path);
  if (!in)
    return out;
  std::string line;
  while (std::getline(in, line)) {
    uint64_t hash = 0;
    std::string name;
    bool disabled = false;
    if (!ParseShaderTomlLine(line, hash, name, disabled))
      continue;
    if (disabled)
      out.push_back(hash);
  }
  return out;
}

void ShaderDebuggerDialog::RefreshSelectedDetails() {
  if (!has_selection_ || !details_provider_) {
    selected_details_ = {};
    edit_buffer_.clear();
    return;
  }
  selected_details_ = details_provider_(selected_hash_);
  if (!selected_details_.found) {
    edit_buffer_.clear();
    return;
  }
  if (selected_translation_idx_ < 0 ||
      selected_translation_idx_ >= static_cast<int>(selected_details_.translations.size())) {
    selected_translation_idx_ = 0;
  }
  if (!selected_details_.translations.empty()) {
    LoadEditBuffer(edit_buffer_,
                   selected_details_.translations[selected_translation_idx_].host_disassembly);
  } else {
    edit_buffer_.clear();
  }
}

void ShaderDebuggerDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Shader Debugger", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    if (!open) {
      Close();
    }
    return;
  }

  if (auto_refresh_ || cached_.empty()) {
    if (snapshot_provider_) {
      cached_ = snapshot_provider_();
    }
  }

  // Split: left = list, right = viewer/editor.
  float right_pane_width = has_selection_ ? 460.0f : 0.0f;
  float avail = ImGui::GetContentRegionAvail().x;
  float left_pane_width = avail - right_pane_width - (has_selection_ ? 8.0f : 0.0f);
  if (left_pane_width < 200.0f)
    left_pane_width = 200.0f;

  ImGui::BeginChild("##left", ImVec2(left_pane_width, 0), false);
  DrawShaderTable();
  ImGui::EndChild();

  if (has_selection_) {
    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, 0), true);
    DrawDetailsPanel();
    ImGui::EndChild();
  }

  ImGui::End();

  if (!open) {
    Close();
  }
}

void ShaderDebuggerDialog::DrawShaderTable() {
  size_t total = cached_.size();
  size_t vs_count = 0, ps_count = 0, disabled_count = 0;
  for (const auto& e : cached_) {
    if (e.type == 0) {
      ++vs_count;
    } else if (e.type == 1) {
      ++ps_count;
    }
    if (e.disabled) {
      ++disabled_count;
    }
  }
  ImGui::Text("Shaders: %zu (VS: %zu, PS: %zu)  Disabled: %zu", total, vs_count, ps_count,
              disabled_count);

  ImGui::Checkbox("VS", &show_vertex_);
  ImGui::SameLine();
  ImGui::Checkbox("PS", &show_pixel_);
  ImGui::SameLine();
  ImGui::Checkbox("Only active", &show_only_active_);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-refresh", &auto_refresh_);
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) {
    if (snapshot_provider_) {
      cached_ = snapshot_provider_();
    }
    if (has_selection_) {
      RefreshSelectedDetails();
    }
  }

  if (ImGui::Button("Enable all")) {
    for (const auto& e : cached_) {
      if (e.disabled) {
        if (disable_setter_)
          disable_setter_(e.ucode_hash, false);
        SetDisabled(e.ucode_hash, false);
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Disable all visible")) {
    for (const auto& e : cached_) {
      bool show_type = (e.type == 0 && show_vertex_) || (e.type == 1 && show_pixel_);
      if (!show_type)
        continue;
      if (show_only_active_ && !e.active)
        continue;
      if (!MatchesFilter(e, filter_buf_))
        continue;
      if (!e.disabled) {
        if (disable_setter_)
          disable_setter_(e.ucode_hash, true);
        SetDisabled(e.ucode_hash, true);
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset timings")) {
    if (profiling_resetter_)
      profiling_resetter_();
  }
  if (has_selection_) {
    ImGui::SameLine();
    if (ImGui::Button("Close viewer")) {
      has_selection_ = false;
      selected_hash_ = 0;
      selected_details_ = {};
      edit_buffer_.clear();
    }
  }

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##filter", "Filter by hash (hex substring)", filter_buf_,
                           sizeof(filter_buf_));

  ImGui::Separator();

  if (ImGui::BeginTable("##shaders", 9,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_NoSort, 70.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort, 50.0f);
    ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_None, 170.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 140.0f);
    ImGui::TableSetupColumn("Dwords", ImGuiTableColumnFlags_None, 70.0f);
    ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_None, 50.0f);
    ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_None, 80.0f);
    ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_None, 70.0f);
    ImGui::TableSetupColumn("Avg us", ImGuiTableColumnFlags_None, 70.0f);
    ImGui::TableHeadersRow();

    std::vector<size_t> indices;
    indices.reserve(cached_.size());
    for (size_t i = 0; i < cached_.size(); ++i) {
      const auto& e = cached_[i];
      bool show_type = (e.type == 0 && show_vertex_) || (e.type == 1 && show_pixel_);
      if (!show_type)
        continue;
      if (show_only_active_ && !e.active)
        continue;
      if (!MatchesFilter(e, filter_buf_))
        continue;
      indices.push_back(i);
    }

    if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
      if (sort_specs->SpecsCount > 0) {
        const auto& spec = sort_specs->Specs[0];
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
          const auto& ea = cached_[a];
          const auto& eb = cached_[b];
          int cmp = 0;
          switch (spec.ColumnIndex) {
            case 1:
              cmp = (ea.type < eb.type) ? -1 : (ea.type > eb.type ? 1 : 0);
              break;
            case 2:
              cmp = (ea.ucode_hash < eb.ucode_hash) ? -1 : (ea.ucode_hash > eb.ucode_hash ? 1 : 0);
              break;
            case 3: {
              const std::string& na = LookupName(ea.ucode_hash);
              const std::string& nb = LookupName(eb.ucode_hash);
              cmp = na.compare(nb);
              break;
            }
            case 4:
              cmp = (ea.dword_count < eb.dword_count) ? -1
                                                      : (ea.dword_count > eb.dword_count ? 1 : 0);
              break;
            case 5:
              cmp = (ea.active ? 1 : 0) - (eb.active ? 1 : 0);
              break;
            case 6:
              cmp = (ea.profile_total_ns < eb.profile_total_ns)
                        ? -1
                        : (ea.profile_total_ns > eb.profile_total_ns ? 1 : 0);
              break;
            case 7:
              cmp = (ea.profile_draw_count < eb.profile_draw_count)
                        ? -1
                        : (ea.profile_draw_count > eb.profile_draw_count ? 1 : 0);
              break;
            case 8: {
              uint64_t avg_a =
                  ea.profile_draw_count ? ea.profile_total_ns / ea.profile_draw_count : 0;
              uint64_t avg_b =
                  eb.profile_draw_count ? eb.profile_total_ns / eb.profile_draw_count : 0;
              cmp = (avg_a < avg_b) ? -1 : (avg_a > avg_b ? 1 : 0);
              break;
            }
            default:
              break;
          }
          return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
        });
      }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(indices.size()));
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const auto& entry = cached_[indices[static_cast<size_t>(row)]];
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(entry.ucode_hash));

        ImGui::TableSetColumnIndex(0);
        bool enabled = !entry.disabled;
        if (ImGui::Checkbox("##en", &enabled)) {
          if (disable_setter_) {
            disable_setter_(entry.ucode_hash, !enabled);
          }
          SetDisabled(entry.ucode_hash, !enabled);
        }

        ImGui::TableSetColumnIndex(1);
        if (entry.type == 1) {
          ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", TypeLabel(entry.type));
        } else {
          ImGui::TextColored(ImVec4(0.85f, 1.0f, 0.6f, 1.0f), "%s", TypeLabel(entry.type));
        }

        ImGui::TableSetColumnIndex(2);
        char hash_str[24];
        std::snprintf(hash_str, sizeof(hash_str), "%016llx",
                      static_cast<unsigned long long>(entry.ucode_hash));
        bool row_selected = has_selection_ && entry.ucode_hash == selected_hash_;
        if (ImGui::Selectable(
                hash_str, row_selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
          has_selection_ = true;
          selected_hash_ = entry.ucode_hash;
          selected_translation_idx_ = 0;
          status_message_.clear();
          // Seed the rename buffer with the current name (if any).
          const std::string& nm = LookupName(entry.ucode_hash);
          std::memset(rename_buf_, 0, sizeof(rename_buf_));
          std::strncpy(rename_buf_, nm.c_str(), sizeof(rename_buf_) - 1);
          RefreshSelectedDetails();
        }

        ImGui::TableSetColumnIndex(3);
        const std::string& name = LookupName(entry.ucode_hash);
        if (!name.empty()) {
          ImGui::TextUnformatted(name.c_str());
        } else {
          ImGui::TextDisabled("-");
        }

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u", entry.dword_count);

        ImGui::TableSetColumnIndex(5);
        if (entry.active) {
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "*");
        }

        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%.3f", entry.profile_total_ns / 1'000'000.0);

        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%llu", static_cast<unsigned long long>(entry.profile_draw_count));

        ImGui::TableSetColumnIndex(8);
        if (entry.profile_draw_count) {
          ImGui::Text("%.2f", (entry.profile_total_ns / 1'000.0) /
                                  static_cast<double>(entry.profile_draw_count));
        } else {
          ImGui::TextDisabled("-");
        }

        ImGui::PopID();
      }
    }
    clipper.End();
    ImGui::EndTable();
  }
}

void ShaderDebuggerDialog::DrawDetailsPanel() {
  if (!selected_details_.found) {
    ImGui::TextDisabled("Shader %016llx not found (may have been evicted).",
                        static_cast<unsigned long long>(selected_hash_));
    if (ImGui::Button("Refresh")) {
      RefreshSelectedDetails();
    }
    return;
  }

  const auto& info = selected_details_.info;
  ImGui::Text("%s  %016llx", TypeLabel(info.type),
              static_cast<unsigned long long>(info.ucode_hash));
  ImGui::SameLine();
  if (ImGui::SmallButton("Copy")) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(info.ucode_hash));
    ImGui::SetClipboardText(buf);
  }
  ImGui::Text("Dwords: %u   Translations: %zu   Disabled: %s", info.dword_count,
              selected_details_.translations.size(), info.disabled ? "yes" : "no");

  // Rename / documentation label. Hitting Enter or pressing the Save button
  // writes the new name into shader_names_ and persists shaders.toml.
  ImGui::SetNextItemWidth(260.0f);
  bool committed =
      ImGui::InputTextWithHint("##rename", "Documentation name", rename_buf_, sizeof(rename_buf_),
                               ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (ImGui::Button("Save name") || committed) {
    SetName(selected_hash_, std::string(rename_buf_));
    status_message_ = rename_buf_[0] ? "Saved name." : "Cleared name.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear name")) {
    rename_buf_[0] = '\0';
    SetName(selected_hash_, std::string{});
    status_message_ = "Cleared name.";
  }

  if (ImGui::Button("Refresh details")) {
    RefreshSelectedDetails();
  }

  if (!status_message_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", status_message_.c_str());
  }

  ImGui::Separator();

  if (ImGui::CollapsingHeader("Microcode disassembly", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Read-only multiline view of the Xenos disassembly.
    ImVec2 size(-1.0f, 180.0f);
    ImGui::InputTextMultiline(
        "##ucode_disasm", const_cast<char*>(selected_details_.ucode_disassembly.c_str()),
        selected_details_.ucode_disassembly.size() + 1, size,
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
  }

  if (selected_details_.translations.empty()) {
    ImGui::TextDisabled("No host translations have been built yet.");
    return;
  }

  if (ImGui::CollapsingHeader("Host translation editor", ImGuiTreeNodeFlags_DefaultOpen)) {
    // Translation selector combo.
    char preview[80];
    std::snprintf(preview, sizeof(preview), "Modification %d (mod=0x%016llx)",
                  selected_translation_idx_,
                  static_cast<unsigned long long>(
                      selected_details_.translations[selected_translation_idx_].modification));
    if (ImGui::BeginCombo("##translation", preview)) {
      for (int i = 0; i < static_cast<int>(selected_details_.translations.size()); ++i) {
        const auto& tr = selected_details_.translations[i];
        char label[96];
        std::snprintf(label, sizeof(label), "#%d  mod=0x%016llx  %s%s", i,
                      static_cast<unsigned long long>(tr.modification),
                      tr.is_translated ? "translated" : "pending", tr.is_valid ? "" : ", invalid");
        bool sel = (i == selected_translation_idx_);
        if (ImGui::Selectable(label, sel)) {
          selected_translation_idx_ = i;
          LoadEditBuffer(edit_buffer_, tr.host_disassembly);
          status_message_.clear();
        }
        if (sel)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    const auto& tr = selected_details_.translations[selected_translation_idx_];
    ImGui::Text("Translated binary: %zu bytes  (%s, %s)", tr.translated_binary.size(),
                tr.is_translated ? "translated" : "pending", tr.is_valid ? "valid" : "INVALID");

    // Editor for the host disassembly text. This text is informational --
    // editing it does not retranslate the shader. To make changes effective,
    // use the binary replace controls below.
    ImGui::TextDisabled("Host disassembly (editable note buffer):");
    if (edit_buffer_.empty()) {
      LoadEditBuffer(edit_buffer_, tr.host_disassembly);
    }
    ImGui::InputTextMultiline("##host_disasm", edit_buffer_.data(), edit_buffer_.size(),
                              ImVec2(-1.0f, 200.0f));

    ImGui::Separator();
    ImGui::TextDisabled(
        "Runtime replacement: dump the binary, edit/recompile externally, then load it back.");

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::InputTextWithHint("##binpath", "Path to .bin (DXBC/DXIL/SPIR-V/...)", binary_path_buf_,
                             sizeof(binary_path_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Save binary...")) {
      const char* path = binary_path_buf_;
      if (!path[0]) {
        status_message_ = "Enter a target file path first.";
      } else {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
          status_message_ = std::string("Failed to open ") + path + " for writing.";
        } else {
          if (!tr.translated_binary.empty()) {
            out.write(reinterpret_cast<const char*>(tr.translated_binary.data()),
                      static_cast<std::streamsize>(tr.translated_binary.size()));
          }
          status_message_ =
              "Wrote " + std::to_string(tr.translated_binary.size()) + " bytes to " + path;
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load binary...")) {
      const char* path = binary_path_buf_;
      if (!path[0]) {
        status_message_ = "Enter a source file path first.";
      } else if (!binary_replacer_) {
        status_message_ = "Backend does not support binary replacement.";
      } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
          status_message_ = std::string("Failed to open ") + path + " for reading.";
        } else {
          std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
          size_t n = bytes.size();
          bool ok = binary_replacer_(selected_hash_, tr.modification, std::move(bytes));
          if (ok) {
            status_message_ = "Queued replacement of " + std::to_string(n) +
                              " bytes; pipelines will rebuild on next draw.";
            // Re-query so the new size is reflected promptly.
            RefreshSelectedDetails();
          } else {
            status_message_ = "Replacement failed (shader/translation no longer present).";
          }
        }
      }
    }
  }
}

}  // namespace rex::ui
