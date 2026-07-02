/**
 * @file        ui/overlay/mod_manager_overlay.cpp
 * @brief       Mod manager overlay implementation. See mod_manager_overlay.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/mod_manager_overlay.h>

#include <fstream>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

namespace {
constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};  // accent blue
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};   // dim grey
constexpr ImVec4 kCodeBadge{1.00f, 0.82f, 0.30f, 1.00f};   // gold, matches gamerscore badges
constexpr float kIconSize = 40.0f;

std::string JoinCommaList(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += names[i];
  }
  return joined;
}
}  // namespace

ModManagerDialog::ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime)
    : ImGuiDialog(imgui_drawer), immediate_drawer_(immediate_drawer), runtime_(runtime) {}

ModManagerDialog::~ModManagerDialog() = default;

ImmediateTexture* ModManagerDialog::GetIcon(const rex::system::ModInfo& mod) {
  if (mod.icon_path.empty()) {
    return nullptr;
  }
  const std::string key = mod.icon_path.string();
  auto it = icon_cache_.find(key);
  if (it != icon_cache_.end()) {
    return it->second.get();
  }

  std::unique_ptr<ImmediateTexture> texture;
  if (immediate_drawer_) {
    std::ifstream file(mod.icon_path, std::ios::binary | std::ios::ate);
    if (file) {
      std::streamsize length = file.tellg();
      if (length > 0) {
        file.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (file.read(reinterpret_cast<char*>(bytes.data()), length)) {
          int width = 0;
          int height = 0;
          std::vector<uint8_t> rgba = DecodeImageRGBA(bytes.data(), bytes.size(), width, height);
          if (!rgba.empty() && width > 0 && height > 0) {
            texture = immediate_drawer_->CreateTexture(
                static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                ImmediateTextureFilter::kLinear, false, rgba.data());
          }
        }
      }
    }
  }

  ImmediateTexture* raw = texture.get();
  icon_cache_.emplace(key, std::move(texture));
  return raw;
}

void ModManagerDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

  if (ImGui::Begin("Mods##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    const auto mods = runtime_ ? runtime_->EnabledModsInfo() : std::vector<rex::system::ModInfo>{};

    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
    ImGui::Text("%zu mod%s enabled", mods.size(), mods.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::TextColored(kMutedText, "Load order: earlier entries win on conflicting files.");
    ImGui::Separator();
    ImGui::Spacing();

    if (mods.empty()) {
      ImGui::TextDisabled("No mods enabled.");
    }

    ImGui::BeginChild("##modlist", ImVec2(0.0f, 0.0f), false);
    int priority = 1;
    for (const auto& mod : mods) {
      ImGui::PushID(mod.folder_name.c_str());

      ImmediateTexture* icon = GetIcon(mod);
      if (icon) {
        ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(kIconSize, kIconSize),
                           ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
      } else {
        ImGui::Dummy(ImVec2(kIconSize, kIconSize));
      }
      ImGui::SameLine();

      ImGui::BeginGroup();
      ImGui::TextColored(kHeaderText, "#%d", priority);
      ImGui::SameLine();
      ImGui::Text("%s", mod.display_name.c_str());
      if (!mod.version.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kMutedText, "v%s", mod.version.c_str());
      }
      if (!mod.code.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kCodeBadge, "[code]");
      }
      ImGui::TextColored(kMutedText, "%s", mod.folder_name.c_str());
      if (!mod.requires_mods.empty()) {
        ImGui::TextColored(kMutedText, "requires: %s", JoinCommaList(mod.requires_mods).c_str());
      }
      if (!mod.conflicts_mods.empty()) {
        ImGui::TextColored(kMutedText, "conflicts: %s", JoinCommaList(mod.conflicts_mods).c_str());
      }
      if (!mod.description.empty()) {
        ImGui::TextWrapped("%s", mod.description.c_str());
      }
      ImGui::EndGroup();

      ImGui::Separator();
      ImGui::PopID();
      ++priority;
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
