/**
 * @file        ui/overlay/achievements_overlay.cpp
 * @brief       Achievements overlay implementation. See achievements_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievements_overlay.h>

#include <algorithm>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

AchievementsOverlayDialog::AchievementsOverlayDialog(ImGuiDrawer* imgui_drawer,
                                                     ImmediateDrawer* immediate_drawer,
                                                     rex::Runtime* runtime,
                                                     rex::system::AchievementManager* achievements)
    : ImGuiDialog(imgui_drawer),
      achievements_(achievements),
      icon_cache_(immediate_drawer, runtime) {}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {}

namespace {
// Palette — kept ASCII-only; the bundled overlay font has no em dash / check glyphs.
constexpr ImVec4 kLockedTitle{0.78f, 0.80f, 0.84f, 1.00f};  // light grey
constexpr ImVec4 kLockedDesc{0.50f, 0.52f, 0.56f, 1.00f};   // dim grey
constexpr ImVec4 kBadgeGS{1.00f, 0.82f, 0.30f, 1.00f};      // gamerscore gold
constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};   // accent blue
constexpr float kIconSize = 44.0f;

// "Unlocked" accent (title text, description tint, row band, progress bar)
// derives from the active theme's CheckMark color rather than a hardcoded
// green, so a project's OnConfigureStyle recolor (see rex::ReXApp) applies
// here too instead of clashing with it.
ImVec4 UnlockedTitleColor() {
  ImVec4 c = ImGui::GetStyle().Colors[ImGuiCol_CheckMark];
  c.w = 1.0f;
  return c;
}

ImVec4 UnlockedDescColor() {
  const ImVec4 accent = UnlockedTitleColor();
  return ImVec4(accent.x * 0.6f + 0.35f, accent.y * 0.6f + 0.35f, accent.z * 0.6f + 0.35f, 1.0f);
}

ImVec4 RowUnlockedBgColor() {
  const ImVec4 accent = UnlockedTitleColor();
  return ImVec4(accent.x * 0.5f, accent.y * 0.5f, accent.z * 0.5f, 0.35f);
}

// Drawn in place of the icon for a still-hidden secret achievement
void DrawLockGlyph(ImDrawList* draw_list, ImVec2 top_left, float size, ImU32 color) {
  const float body_w = size * 0.56f;
  const float body_h = size * 0.42f;
  const float shackle_r = body_w * 0.34f;
  const float cx = top_left.x + size * 0.5f;

  const float glyph_height = shackle_r + body_h;
  const float body_top = top_left.y + (size - glyph_height) * 0.5f + shackle_r;
  const ImVec2 body_min(cx - body_w * 0.5f, body_top);
  const ImVec2 body_max(cx + body_w * 0.5f, body_top + body_h);

  const float thickness = size * 0.06f;
  draw_list->PathArcTo(ImVec2(cx, body_top), shackle_r, IM_PI, IM_PI * 2.0f, 16);
  draw_list->PathStroke(color, 0, thickness);

  draw_list->AddRectFilled(body_min, body_max, color, 2.0f);

  const float keyhole_r = size * 0.05f;
  const ImVec2 keyhole_center(cx, body_top + body_h * 0.42f);
  draw_list->AddCircleFilled(keyhole_center, keyhole_r, ImGui::GetColorU32(ImGuiCol_WindowBg));
}

// Fake bold font by drawing the glyphs a second time offset by a pixel
void TextBoldColored(ImDrawList* draw_list, const ImVec4& col, const std::string& text) {
  ImGui::TextColored(col, "%s", text.c_str());
  const ImVec2 min = ImGui::GetItemRectMin();
  draw_list->AddText(ImVec2(min.x + 1.0f, min.y), ImGui::GetColorU32(col), text.c_str());
}

}  // namespace

ImmediateTexture* AchievementsOverlayDialog::GetIcon(
    const rex::system::AchievementInfo& achievement) {
  return icon_cache_.GetIcon(achievement);
}

void AchievementsOverlayDialog::OnDraw(ImGuiIO& io) {
  const AchievementsStyle& style = imgui_drawer()->style().achievements;

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.window_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, style.item_spacing);

  if (ImGui::Begin("Achievements##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    const auto achievements = achievements_->ListAchievements();

    int unlocked_count = 0;
    int total_gs = 0;
    int earned_gs = 0;
    for (const auto& a : achievements) {
      total_gs += static_cast<int>(a.gamerscore);
      if (achievements_->IsUnlocked(a.id)) {
        ++unlocked_count;
        earned_gs += static_cast<int>(a.gamerscore);
      }
    }
    const int total_count = static_cast<int>(achievements.size());

    // ---- Header: summary line + progress bar -------------------------------
    ImDrawList* header_draw_list = ImGui::GetWindowDrawList();
    const float header_row_width = ImGui::GetContentRegionAvail().x;
    const std::string gs_text = fmt::format("{}G / {}G", earned_gs, total_gs);
    const float gs_text_width = ImGui::CalcTextSize(gs_text.c_str()).x;

    TextBoldColored(header_draw_list, kHeaderText,
                    fmt::format("{} / {} unlocked", unlocked_count, total_count));
    ImGui::SameLine(header_row_width - gs_text_width);
    TextBoldColored(header_draw_list, kBadgeGS, gs_text);

    float frac = total_count > 0 ? static_cast<float>(unlocked_count) / total_count : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UnlockedTitleColor());
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 6.0f), "");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ---- List --------------------------------------------------------------
    ImGui::BeginChild("##achlist", ImVec2(0.0f, 0.0f), false);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (const auto& a : achievements) {
      const bool is_unlocked = achievements_->IsUnlocked(a.id);
      const bool is_secret =
          !is_unlocked && !(a.flags & rex::system::kAchievementFlagShowUnachieved);
      const bool revealed = revealed_secrets_.contains(a.id);
      const bool show_hidden = is_secret && !revealed;
      const bool hint_empty = a.unachieved_description.empty();
      const std::string& desc =
          (is_unlocked || hint_empty) ? a.description : a.unachieved_description;

      ImGui::PushID(static_cast<int>(a.id));

      // Split the draw list so the row band (channel 0) renders behind the
      // text (channel 1); we paint the rect after measuring, then merge.
      draw_list->ChannelsSplit(2);
      draw_list->ChannelsSetCurrent(1);

      const float row_start_y = ImGui::GetCursorScreenPos().y;
      const float pad = 4.0f;
      ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));

      // Leave a visible gap between the row band's left edge and the icon
      const float icon_left_inset = 6.0f;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + icon_left_inset);

      const float row_content_avail_x = ImGui::GetContentRegionAvail().x;

      // Reserve the icon column now, but draw the icon itself after the text
      // block so it can be vertically centered against the text's height.
      const ImVec2 icon_screen_pos = ImGui::GetCursorScreenPos();
      ImmediateTexture* icon = show_hidden ? nullptr : GetIcon(a);
      ImGui::Dummy(ImVec2(kIconSize, kIconSize));
      ImGui::SameLine();

      // Text block to the right of the icon.
      const ImVec4 unlocked_title = UnlockedTitleColor();
      const float reveal_column_width = 90.0f;
      ImGui::BeginGroup();
      ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x +
                             (is_secret ? -reveal_column_width : 0.0f));
      TextBoldColored(draw_list, kBadgeGS, fmt::format("{}G", a.gamerscore));
      ImGui::SameLine();
      if (show_hidden) {
        TextBoldColored(draw_list, kLockedTitle, "Hidden Achievement");
      } else {
        TextBoldColored(draw_list, is_unlocked ? unlocked_title : kLockedTitle, a.label);
      }

      if (!show_hidden) {
        ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? UnlockedDescColor() : kLockedDesc);
        ImGui::TextWrapped("%s", desc.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      const float text_block_height = ImGui::GetItemRectSize().y;

      // Reveal toggle on the right side of the row
      if (is_secret) {
        const float row_content_height = std::max(kIconSize, text_block_height);
        const float checkbox_offset_y = (row_content_height - ImGui::GetFrameHeight()) * 0.5f;

        ImGui::SameLine(row_content_avail_x - reveal_column_width);
        if (checkbox_offset_y > 0.0f) {
          ImGui::SetCursorPosY(ImGui::GetCursorPosY() + checkbox_offset_y);
        }
        ImGui::BeginGroup();
        bool checked = revealed;
        if (ImGui::Checkbox("Reveal", &checked)) {
          if (checked) {
            revealed_secrets_.insert(a.id);
          } else {
            revealed_secrets_.erase(a.id);
          }
        }
        ImGui::EndGroup();
      }

      ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
      const float row_end_y = ImGui::GetCursorScreenPos().y;

      // Draw the icon (or lock glyph) now that the row's bounds are known
      const float icon_draw_y = row_start_y + (row_end_y - row_start_y - kIconSize) * 0.5f;
      const ImVec2 icon_draw_pos(icon_screen_pos.x, icon_draw_y);
      if (show_hidden) {
        DrawLockGlyph(draw_list, icon_draw_pos, kIconSize,
                      ImGui::GetColorU32(UnlockedTitleColor()));
      } else if (icon) {
        ImVec4 tint = is_unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.45f, 0.45f, 0.8f);
        draw_list->AddImage(reinterpret_cast<ImTextureID>(icon), icon_draw_pos,
                            ImVec2(icon_draw_pos.x + kIconSize, icon_draw_pos.y + kIconSize),
                            ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(tint));
      }

      // Paint the band behind unlocked rows on the lower channel.
      if (is_unlocked) {
        draw_list->ChannelsSetCurrent(0);
        const float x0 = ImGui::GetWindowPos().x + 2.0f;
        const float x1 = x0 + ImGui::GetWindowSize().x - 4.0f;
        draw_list->AddRectFilled(ImVec2(x0, row_start_y), ImVec2(x1, row_end_y),
                                 ImGui::GetColorU32(RowUnlockedBgColor()), 3.0f);
      }
      draw_list->ChannelsMerge();

      ImGui::Separator();
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
