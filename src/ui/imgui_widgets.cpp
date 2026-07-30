/**
 * @file        ui/imgui_widgets.cpp
 *
 * @brief       Cvar-aware widgets. See imgui_widgets.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/imgui_widgets.h>
#include <rex/cvar.h>
#include <rex/string.h>
#include <imgui.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>

namespace rex {
namespace ui {

bool DrawCvarWidget(const cvar::FlagEntry& entry, float item_width, bool persist) {
  std::string current_val = entry.getter();
  bool changed = false;

  ImGui::SetNextItemWidth(item_width);
  if (entry.type == cvar::FlagType::Boolean) {
    bool v = (current_val == "true");
    if (rex::ui::ToggleSwitch("##v", &v)) {
      cvar::SetFlagByName(entry.name, v ? "true" : "false", persist);
      changed = true;
    }
  } else if (entry.type == cvar::FlagType::String && !entry.constraints.allowed_values.empty()) {
    const auto& opts = entry.constraints.allowed_values;
    int cur_idx = 0;
    for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
      if (opts[i] == current_val) {
        cur_idx = i;
        break;
      }
    }
    if (ImGui::BeginCombo("##v", opts[cur_idx].c_str())) {
      for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        bool sel = (i == cur_idx);
        if (ImGui::Selectable(opts[i].c_str(), sel)) {
          cvar::SetFlagByName(entry.name, opts[i], persist);
          changed = true;
        }
        if (sel) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  } else if (entry.type == cvar::FlagType::Int32 || entry.type == cvar::FlagType::Int64 ||
             entry.type == cvar::FlagType::Uint32 || entry.type == cvar::FlagType::Uint64) {
    int v = std::atoi(current_val.c_str());
    int vmin =
        entry.constraints.min.has_value() ? static_cast<int>(*entry.constraints.min) : INT_MIN;
    int vmax =
        entry.constraints.max.has_value() ? static_cast<int>(*entry.constraints.max) : INT_MAX;
    if (ImGui::InputInt("##v", &v)) {
      v = std::clamp(v, vmin, vmax);
      cvar::SetFlagByName(entry.name, std::to_string(v), persist);
      changed = true;
    }
  } else if (entry.type == cvar::FlagType::Double) {
    double v = std::atof(current_val.c_str());
    if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
      if (entry.constraints.min) {
        v = std::max(v, *entry.constraints.min);
      }
      if (entry.constraints.max) {
        v = std::min(v, *entry.constraints.max);
      }
      cvar::SetFlagByName(entry.name, std::to_string(v), persist);
      changed = true;
    }
  } else if (entry.type == cvar::FlagType::Command) {
    if (ImGui::Button(std::string(entry.name + "##v").c_str())) {
      cvar::InvokeCommand(entry.name, "");
      changed = true;
    }
  } else {
    char buf[256];
    rex::string::copy_truncating(buf, current_val, sizeof(buf));
    ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      cvar::SetFlagByName(entry.name, buf, persist);
      changed = true;
    }
  }

  return changed;
}

}  // namespace ui
}  // namespace rex
