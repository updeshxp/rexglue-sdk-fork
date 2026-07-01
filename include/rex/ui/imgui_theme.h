/**
 * @file        rex/ui/imgui_theme.h
 * @brief       Derives the full overlay color palette from a single accent color
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
namespace ui {

// Reshades `accent` to a given brightness (HSV value) and alpha, keeping its
// hue and saturation. This is how every role color below is produced, so the
// whole palette stays a single consistent hue family.
inline ImVec4 ShadeAccent(const ImVec4& accent, float value, float alpha) {
  float h, s, v;
  ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, h, s, v);
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(h, s, value, r, g, b);
  return ImVec4(r, g, b, alpha);
}

// Derives ImGui's full overlay palette (dialogs, buttons, scrollbars, tabs,
// etc.) from a single accent color, so a game can retheme every overlay by
// picking one color instead of editing dozens of hardcoded ImVec4s. Call from
// rex::ReXApp::OnConfigureStyle.
inline void ApplyAccentTheme(ImGuiStyle& style, const ImVec4& accent) {
  style.Colors[ImGuiCol_WindowBg] = ShadeAccent(accent, 0.06f, 1.00f);
  style.Colors[ImGuiCol_Border] = ShadeAccent(accent, 0.35f, 1.00f);
  style.Colors[ImGuiCol_TitleBg] = ShadeAccent(accent, 0.40f, 1.00f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ShadeAccent(accent, 0.33f, 1.00f);
  style.Colors[ImGuiCol_TitleBgActive] = ShadeAccent(accent, 0.65f, 1.00f);
  style.Colors[ImGuiCol_MenuBarBg] = ShadeAccent(accent, 0.35f, 1.00f);
  style.Colors[ImGuiCol_ScrollbarBg] = ShadeAccent(accent, 0.40f, 0.59f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ShadeAccent(accent, 0.68f, 0.68f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = ShadeAccent(accent, 1.00f, 0.62f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = ShadeAccent(accent, 0.91f, 0.40f);
  style.Colors[ImGuiCol_CheckMark] = ShadeAccent(accent, 0.95f, 1.00f);
  style.Colors[ImGuiCol_SliderGrabActive] = ShadeAccent(accent, 0.75f, 1.00f);
  style.Colors[ImGuiCol_Button] = ShadeAccent(accent, 0.56f, 0.60f);
  style.Colors[ImGuiCol_ButtonHovered] = ShadeAccent(accent, 0.72f, 1.00f);
  style.Colors[ImGuiCol_ButtonActive] = ShadeAccent(accent, 0.60f, 1.00f);
  style.Colors[ImGuiCol_Header] = ShadeAccent(accent, 0.40f, 0.71f);
  style.Colors[ImGuiCol_HeaderHovered] = ShadeAccent(accent, 0.60f, 0.80f);
  style.Colors[ImGuiCol_HeaderActive] = ShadeAccent(accent, 0.75f, 0.80f);
  style.Colors[ImGuiCol_Separator] = ShadeAccent(accent, 0.35f, 1.00f);
  style.Colors[ImGuiCol_SeparatorHovered] = ShadeAccent(accent, 0.89f, 1.00f);
  style.Colors[ImGuiCol_SeparatorActive] = ShadeAccent(accent, 0.50f, 1.00f);
  style.Colors[ImGuiCol_Tab] = style.Colors[ImGuiCol_Button];
  style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_ButtonHovered];
  style.Colors[ImGuiCol_TabActive] = style.Colors[ImGuiCol_ButtonActive];
  style.Colors[ImGuiCol_TabUnfocused] = style.Colors[ImGuiCol_FrameBg];
  style.Colors[ImGuiCol_TabUnfocusedActive] = style.Colors[ImGuiCol_FrameBgHovered];
  style.Colors[ImGuiCol_TextSelectedBg] = ShadeAccent(accent, 1.00f, 0.21f);
}

}  // namespace ui
}  // namespace rex
