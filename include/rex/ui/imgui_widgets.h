/**
 * @file        rex/ui/imgui_widgets.h
 * @brief       Small collection of custom ImGui widgets shared by overlays
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <imgui.h>

namespace rex {

namespace cvar {
struct FlagEntry;
}  // namespace cvar

namespace ui {

// A sliding pill-shaped toggle, drawn as a themed alternative to
// ImGui::Checkbox. Behaves like Checkbox: returns true the frame `*v`
// changes, and toggles `*v` on click. Built entirely on ImGui's public API
// (InvisibleButton + draw list) so it stays portable across ImGui versions.
inline bool ToggleSwitch(const char* label, bool* v) {
  ImGuiStyle& style = ImGui::GetStyle();
  const float height = ImGui::GetFrameHeight();
  const float width = height * 1.75f;
  const ImVec2 pos = ImGui::GetCursorScreenPos();

  ImGui::PushID(label);
  bool changed = ImGui::InvisibleButton("##toggle", ImVec2(width, height));
  if (changed) {
    *v = !*v;
  }
  bool hovered = ImGui::IsItemHovered();
  bool held = ImGui::IsItemActive();

  // Ease the knob toward its target position; state lives in ImGui's
  // per-widget storage so it survives across frames without a static.
  ImGuiID anim_id = ImGui::GetID("##anim");
  ImGuiStorage* storage = ImGui::GetStateStorage();
  float t_anim = storage->GetFloat(anim_id, *v ? 1.0f : 0.0f);
  const float target = *v ? 1.0f : 0.0f;
  const float speed = ImGui::GetIO().DeltaTime / 0.08f;
  t_anim += (target - t_anim) * (speed < 1.0f ? speed : 1.0f);
  storage->SetFloat(anim_id, t_anim);

  const ImU32 col_bg = ImGui::GetColorU32(*v ? (held      ? ImGuiCol_ButtonActive
                                                : hovered ? ImGuiCol_ButtonHovered
                                                          : ImGuiCol_Button)
                                             : ImGuiCol_FrameBg);
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), col_bg, height * 0.5f);

  const float knob_radius = height * 0.5f - 2.0f;
  const float knob_x =
      (pos.x + knob_radius + 2.0f) + t_anim * (width - 2.0f * (knob_radius + 2.0f));
  const ImVec2 knob_center(knob_x, pos.y + height * 0.5f);
  draw_list->AddCircleFilled(knob_center, knob_radius, IM_COL32(235, 235, 235, 255));

  ImGui::PopID();

  if (label[0] != '#' || label[1] != '#') {
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
  }

  return changed;
}

// Draws the value editor for a single cvar (toggle for Boolean, combo for a
// String with allowed_values, InputInt/InputDouble clamped to range for
// numeric types, a button for Command, plain text input otherwise). Does not
// draw the entry's name/label - callers place this next to their own label.
// Any edit is applied via cvar::SetFlagByName(entry.name, ..., persist).
// Returns true the frame the value changes. Shared by the SDK's developer
// settings panel and any game-provided settings overlay so there is one
// implementation of "how do I edit a cvar" for all widget layouts.
bool DrawCvarWidget(const cvar::FlagEntry& entry, float item_width = 160.0f, bool persist = true);

}  // namespace ui
}  // namespace rex
