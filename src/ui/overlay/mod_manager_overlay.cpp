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
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_conflict_tracker.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>

namespace rex::ui {

namespace {
constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};     // accent blue
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};      // dim grey
constexpr ImVec4 kCodeBadge{1.00f, 0.82f, 0.30f, 1.00f};      // gold, matches gamerscore badges
constexpr ImVec4 kConflictBadge{1.00f, 0.55f, 0.35f, 1.00f};  // amber/orange, distinct from gold
constexpr float kIconSize = 40.0f;

// Gamepad face buttons ImGui already tracks (via io.AddKeyEvent, fed by
// whatever gamepad backend is active) paired with the button name
// rex::ui::GamepadButtonNames()/ParseGamepadButton use, so a captured press
// round-trips through the same string the keyboard path uses. Limited to the
// four face buttons deliberately: those are the ones free from ImGui's own
// menu navigation (D-pad/shoulders/sticks drive nav itself), so capturing
// them here doesn't fight the future fully-gamepad-navigable overlay.
constexpr std::pair<ImGuiKey, const char*> kCapturableGamepadButtons[] = {
    {ImGuiKey_GamepadFaceDown, "A"},
    {ImGuiKey_GamepadFaceRight, "B"},
    {ImGuiKey_GamepadFaceLeft, "X"},
    {ImGuiKey_GamepadFaceUp, "Y"},
};

// Scans for the next keyboard key or gamepad face-button press while a bind
// is in "listening" mode. Returns the rex::ui key/button name to apply via
// SetBindKey, or empty if nothing was pressed this frame yet. Escape cancels
// (returns "\x1b" as a sentinel the caller checks for) without changing the
// bind.
constexpr const char* kCancelSentinel = "\x1b";

std::string PollListeningCapture(ImGuiIO& io) {
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    return kCancelSentinel;
  }
  for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
    ImGuiKey k = static_cast<ImGuiKey>(key);
    if (k == ImGuiKey_Escape || k == ImGuiKey_MouseLeft || k == ImGuiKey_MouseRight) {
      continue;
    }
    if (!ImGui::IsKeyPressed(k, false)) {
      continue;
    }
    // Translate via ImGui's own key name, skipping keys that don't map to a
    // rex::ui VirtualKey name -- rebinding is limited to keys the bind system
    // itself recognizes.
    const char* imgui_name = ImGui::GetKeyName(k);
    if (imgui_name && rex::ui::ParseVirtualKey(imgui_name) != rex::ui::VirtualKey::kNone) {
      return imgui_name;
    }
  }
  for (const auto& [imgui_key, button_name] : kCapturableGamepadButtons) {
    if (ImGui::IsKeyPressed(imgui_key, false)) {
      return button_name;
    }
  }
  (void)io;
  return {};
}

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

std::string JoinRequirements(const std::vector<rex::system::ModRequirement>& reqs) {
  std::string joined;
  for (size_t i = 0; i < reqs.size(); ++i) {
    if (i > 0) {
      joined += ", ";
    }
    joined += reqs[i].name;
    if (!reqs[i].min_version.empty()) {
      joined += " >= ";
      joined += reqs[i].min_version;
    }
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
        ImGui::TextColored(kMutedText, "requires: %s", JoinRequirements(mod.requires_mods).c_str());
      }
      if (!mod.min_game_version.empty()) {
        ImGui::TextColored(kMutedText, "needs game version: >= %s", mod.min_game_version.c_str());
      }
      if (!mod.conflicts_mods.empty()) {
        ImGui::TextColored(kMutedText, "conflicts: %s", JoinCommaList(mod.conflicts_mods).c_str());
      }
      if (!mod.description.empty()) {
        ImGui::TextWrapped("%s", mod.description.c_str());
      }
      DrawKeybindsSection(mod);
      DrawCvarsSection(mod);
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

void ModManagerDialog::DrawKeybindsSection(const rex::system::ModInfo& mod) {
  auto binds = rex::ui::SnapshotBinds();
  bool drew_header = false;
  for (const auto& bind : binds) {
    if (bind.owner != mod.folder_name || !bind.active) {
      continue;
    }
    if (!drew_header) {
      ImGui::TextColored(kMutedText, "Keybinds:");
      drew_header = true;
    }

    ImGui::PushID(bind.name.c_str());
    ImGui::Text("%s", bind.description.empty() ? bind.name.c_str() : bind.description.c_str());
    ImGui::SameLine();

    bool listening = listening_bind_ == bind.name;
    std::string label = listening ? "...(press a key)..." : bind.effective_key;
    if (label.empty()) {
      label = "(unbound)";
    }
    if (ImGui::Button(label.c_str())) {
      listening_bind_ = listening ? std::string{} : bind.name;
    }
    if (bind.conflicted) {
      ImGui::SameLine();
      ImGui::TextColored(kConflictBadge, "[unresolved conflict]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Wanted '%s'; no free key was available.", bind.requested_key.c_str());
      }
    } else if (bind.effective_key != bind.requested_key) {
      ImGui::SameLine();
      ImGui::TextColored(kConflictBadge, "[moved]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Wanted '%s', already in use by another mod.",
                          bind.requested_key.c_str());
      }
    }

    if (listening) {
      ImGuiIO& io = ImGui::GetIO();
      std::string captured = PollListeningCapture(io);
      if (captured == kCancelSentinel) {
        listening_bind_.clear();
      } else if (!captured.empty()) {
        rex::ui::SetBindKey(bind.name, captured);
        listening_bind_.clear();
      }
    }
    ImGui::PopID();
  }
}

void ModManagerDialog::DrawCvarsSection(const rex::system::ModInfo& mod) {
  auto* tracker = runtime_ ? runtime_->mod_conflict_tracker() : nullptr;
  if (!tracker) {
    return;
  }
  auto activity = tracker->CvarActivityFor(mod.folder_name);
  if (activity.empty()) {
    return;
  }
  auto divergent = tracker->DivergentOverrides();

  ImGui::TextColored(kMutedText, "Cvars:");
  for (const auto& entry : activity) {
    ImGui::PushID(entry.name.c_str());
    if (entry.is_new_definition) {
      ImGui::TextColored(kMutedText, "defines %s", entry.name.c_str());
    } else {
      ImGui::TextColored(kMutedText, "sets %s: %s -> %s", entry.name.c_str(),
                         entry.old_value.c_str(), entry.new_value.c_str());
    }
    auto it = divergent.find(entry.name);
    if (it != divergent.end()) {
      ImGui::SameLine();
      ImGui::TextColored(kConflictBadge, "[conflict]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Also set by: %s", JoinCommaList(it->second).c_str());
      }
    }
    ImGui::PopID();
  }
}

}  // namespace rex::ui
