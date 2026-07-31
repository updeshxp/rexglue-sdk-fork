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

#include <algorithm>
#include <fstream>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/net/http.h>
#include <rex/platform/process.h>
#include <rex/runtime.h>
#include <rex/system/mod_conflict_tracker.h>
#include <rex/system/mod_version.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

namespace rex::ui {

namespace {
constexpr ImVec4 kHeaderText{0.60f, 0.85f, 1.00f, 1.00f};     // accent blue
constexpr ImVec4 kMutedText{0.60f, 0.62f, 0.66f, 1.00f};      // dim grey
constexpr ImVec4 kCodeBadge{1.00f, 0.82f, 0.30f, 1.00f};      // gold, matches gamerscore badges
constexpr ImVec4 kConflictBadge{1.00f, 0.55f, 0.35f, 1.00f};  // amber/orange, distinct from gold
constexpr ImVec4 kErrorBadge{1.00f, 0.35f, 0.35f, 1.00f};
constexpr ImVec4 kWarnBadge{1.00f, 0.75f, 0.30f, 1.00f};
constexpr ImVec4 kUpdateBadge{0.45f, 0.85f, 0.55f, 1.00f};
constexpr float kIconSize = 40.0f;

constexpr std::pair<ImGuiKey, const char*> kCapturableGamepadButtons[] = {
    {ImGuiKey_GamepadFaceDown, "A"},
    {ImGuiKey_GamepadFaceRight, "B"},
    {ImGuiKey_GamepadFaceLeft, "X"},
    {ImGuiKey_GamepadFaceUp, "Y"},
};

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
    if (i > 0)
      joined += ", ";
    joined += names[i];
  }
  return joined;
}

std::string JoinRequirements(const std::vector<rex::system::ModRequirement>& reqs) {
  std::string joined;
  for (size_t i = 0; i < reqs.size(); ++i) {
    if (i > 0)
      joined += ", ";
    joined += reqs[i].name;
    if (!reqs[i].min_version.empty()) {
      joined += " >= ";
      joined += reqs[i].min_version;
    }
  }
  return joined;
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return {};
  std::streamsize length = file.tellg();
  if (length <= 0)
    return {};
  file.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (!file.read(reinterpret_cast<char*>(bytes.data()), length))
    return {};
  return bytes;
}

std::unique_ptr<ImmediateTexture> UploadRGBA(ImmediateDrawer* drawer,
                                             const std::vector<uint8_t>& bytes) {
  if (!drawer || bytes.empty())
    return nullptr;
  int width = 0, height = 0;
  std::vector<uint8_t> rgba = DecodeImageRGBA(bytes.data(), bytes.size(), width, height);
  if (rgba.empty() || width <= 0 || height <= 0)
    return nullptr;
  return drawer->CreateTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                               ImmediateTextureFilter::kLinear, false, rgba.data());
}

}  // namespace

ModManagerDialog::ModManagerDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime, Window* window,
                                   std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer),
      immediate_drawer_(immediate_drawer),
      runtime_(runtime),
      window_(window),
      config_path_(std::move(config_path)) {}

ModManagerDialog::~ModManagerDialog() {
  for (auto& [url, thread] : icon_downloads_) {
    if (thread.joinable())
      thread.join();
  }
  if (sideload_thread_.joinable()) {
    sideload_thread_.join();
  }
}

void ModManagerDialog::SideloadArchive(std::filesystem::path zip_path) {
  // Resolve the mods root directly rather than going through ReloadFromDisk
  // (which would also mark loaded_ = true and skip OnDraw's first-frame
  // catalog_.Refresh() kickoff -- this can run before OnDraw ever sees this
  // dialog, e.g. when a drop opens the overlay instead of the F1 bind).
  auto root = rex::system::ModState::ResolveModsRoot();

  bool expected = false;
  if (!sideload_in_flight_.compare_exchange_strong(expected, true)) {
    return;  // an install is already running; ignore this drop
  }
  if (sideload_thread_.joinable()) {
    sideload_thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(sideload_mutex_);
    sideload_result_ = SideloadResult{};
    sideload_result_.in_progress = true;
  }

  sideload_thread_ = std::thread([this, root, zip_path = std::move(zip_path)] {
    std::string error;
    auto result = rex::system::ModState::InstallLocalArchive(root, zip_path, error);

    std::lock_guard<std::mutex> lock(sideload_mutex_);
    sideload_result_.in_progress = false;
    sideload_result_.done = true;
    sideload_result_.ok = result.has_value();
    if (result) {
      sideload_result_.message = (result->staged    ? "Downloaded update for \""
                                  : result->updated ? "Updated \""
                                                    : "Sideloaded \"") +
                                 result->id + "\"" +
                                 (result->version.empty() ? "" : " (v" + result->version + ")") +
                                 (result->staged ? " -- restart to apply" : "");
      sideload_result_.focus_id = result->id;
    } else {
      sideload_result_.message = error;
    }
    sideload_in_flight_.store(false, std::memory_order_release);
  });
}

void ModManagerDialog::ReloadFromDisk() {
  mods_root_ = rex::system::ModState::ResolveModsRoot();
  entries_ = rex::system::ModState::LoadReconciled(mods_root_);
  manifests_.clear();
  if (runtime_) {
    for (const auto& info : runtime_->InstalledModsInfo()) {
      manifests_.emplace(info.folder_name, info);
    }
  }
  loaded_ = true;
  pending_removal_ids_ = rex::system::ModState::PendingRemovals(mods_root_);
  has_pending_updates_ =
      rex::system::ModState::HasPendingUpdates(mods_root_) || !pending_removal_ids_.empty();
  issues_ = rex::system::ModState::Validate(entries_, manifests_,
                                            runtime_ ? runtime_->game_version() : "",
                                            rex::system::ModState::HostPlatformId());
}

void ModManagerDialog::PersistAndRevalidate() {
  rex::system::ModState::Save(mods_root_, entries_);
  issues_ = rex::system::ModState::Validate(entries_, manifests_,
                                            runtime_ ? runtime_->game_version() : "",
                                            rex::system::ModState::HostPlatformId());
}

bool ModManagerDialog::StateDiffersFromStartup() const {
  if (!runtime_)
    return false;
  return entries_ != runtime_->ModStateAtStartup();
}

ImmediateTexture* ModManagerDialog::GetLocalIcon(const rex::system::ModInfo& mod) {
  if (mod.icon_path.empty())
    return nullptr;
  const std::string key = mod.icon_path.string();
  auto it = icon_cache_.find(key);
  if (it != icon_cache_.end())
    return it->second.get();

  auto texture = UploadRGBA(immediate_drawer_, ReadFileBytes(mod.icon_path));
  ImmediateTexture* raw = texture.get();
  icon_cache_.emplace(key, std::move(texture));
  return raw;
}

ImmediateTexture* ModManagerDialog::GetRemoteIcon(const std::string& url) {
  if (url.empty())
    return nullptr;
  auto cache_it = icon_cache_.find(url);
  if (cache_it != icon_cache_.end())
    return cache_it->second.get();

  std::vector<uint8_t> bytes;
  {
    std::lock_guard<std::mutex> lock(remote_icon_mutex_);
    auto bytes_it = remote_icon_bytes_.find(url);
    if (bytes_it != remote_icon_bytes_.end()) {
      bytes = bytes_it->second;
      remote_icon_bytes_.erase(bytes_it);
    } else if (!icon_downloads_.contains(url)) {
      icon_downloads_.emplace(url, std::thread([this, url] {
                                auto response = rex::net::HttpGet(url);
                                if (response.ok()) {
                                  std::lock_guard<std::mutex> lock2(remote_icon_mutex_);
                                  remote_icon_bytes_[url] = std::vector<uint8_t>(
                                      response.body.begin(), response.body.end());
                                }
                              }));
      return nullptr;
    } else {
      return nullptr;
    }
  }

  auto texture = UploadRGBA(immediate_drawer_, bytes);
  ImmediateTexture* raw = texture.get();
  icon_cache_.emplace(url, std::move(texture));
  return raw;
}

void ModManagerDialog::OnDraw(ImGuiIO& io) {
  if (!loaded_) {
    ReloadFromDisk();
    if (runtime_ && !runtime_->catalog_name().empty()) {
      catalog_.Refresh();
    }
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

  if (ImGui::Begin("Mods##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    DrawRestartBanner();

    if (ImGui::BeginTabBar("##modtabs")) {
      if (ImGui::BeginTabItem("Installed")) {
        DrawInstalledTab();
        ImGui::EndTabItem();
      }
      if (catalog_.state() == rex::system::CatalogState::kReady) {
        if (ImGui::BeginTabItem("All")) {
          DrawCatalogTab();
          ImGui::EndTabItem();
        }
      } else if (catalog_.state() == rex::system::CatalogState::kLoading) {
        if (ImGui::BeginTabItem("All")) {
          ImGui::TextDisabled("Loading catalog...");
          ImGui::EndTabItem();
        }
      }
      // kFailed / kIdle (disabled config): no "All" tab at all.
      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
}

void ModManagerDialog::DrawRestartBanner() {
  if (!StateDiffersFromStartup() && !has_pending_updates_)
    return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
  ImGui::TextWrapped("Mod changes require a restart to take effect.");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::SmallButton(has_pending_updates_ ? "Restart & Apply" : "Restart Now")) {
    if (rex::platform::process::Relaunch() && window_) {
      window_->RequestClose();
    }
  }
  ImGui::Separator();
}

void ModManagerDialog::DrawInstalledTab() {
  {
    SideloadResult sideload_snapshot;
    {
      std::lock_guard<std::mutex> lock(sideload_mutex_);
      sideload_snapshot = sideload_result_;
      // Consume the focus request exactly once; the message itself persists
      // (same "shows forever until the next install" convention the catalog
      // tab's install status already uses).
      sideload_result_.focus_id.clear();
    }
    if (sideload_snapshot.in_progress) {
      ImGui::TextColored(kMutedText, "Installing dropped mod...");
      ImGui::Separator();
    } else if (sideload_snapshot.done) {
      ImGui::TextColored(sideload_snapshot.ok ? kUpdateBadge : kErrorBadge, "%s",
                         sideload_snapshot.message.c_str());
      ImGui::Separator();
    }
    if (sideload_snapshot.ok && !sideload_snapshot.focus_id.empty()) {
      ReloadFromDisk();
      focus_mod_id_ = sideload_snapshot.focus_id;
    }
  }

  if (ImGui::SmallButton("Auto-sort")) {
    entries_ = rex::system::ModState::AutoSort(entries_, manifests_);
    PersistAndRevalidate();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Refresh from disk")) {
    ReloadFromDisk();
  }
  ImGui::SameLine();
  ImGui::TextColored(kMutedText, "%zu mod%s installed", entries_.size(),
                     entries_.size() == 1 ? "" : "s");
  ImGui::TextColored(kMutedText, "Load order: earlier entries win on conflicting files.");
  ImGui::Separator();

  if (entries_.empty()) {
    ImGui::TextDisabled("No mods installed.");
  }

  auto issues_for = [&](const std::string& id) {
    std::vector<const rex::system::ModIssue*> out;
    for (const auto& issue : issues_) {
      if (issue.id == id)
        out.push_back(&issue);
    }
    return out;
  };

  auto catalog_snapshot = catalog_.state() == rex::system::CatalogState::kReady
                              ? catalog_.Snapshot()
                              : std::vector<rex::system::CatalogMod>{};
  auto find_catalog_entry = [&](const std::string& id) -> const rex::system::CatalogMod* {
    for (const auto& mod : catalog_snapshot) {
      if (mod.mod_id == id)
        return &mod;
    }
    return nullptr;
  };

  ImGui::BeginChild("##modlist", ImVec2(0.0f, 0.0f), false);
  if (pending_scroll_restore_ >= 0.0f) {
    ImGui::SetScrollY(pending_scroll_restore_);
    pending_scroll_restore_ = -1.0f;
  }
  int priority = 1;
  for (size_t i = 0; i < entries_.size(); ++i) {
    auto& entry = entries_[i];
    auto manifest_it = manifests_.find(entry.id);
    static const rex::system::ModInfo kEmptyInfo;
    const rex::system::ModInfo& mod =
        manifest_it != manifests_.end() ? manifest_it->second : kEmptyInfo;

    ImGui::PushID(entry.id.c_str());
    bool is_focused = entry.id == focus_mod_id_;
    if (is_focused) {
      // One-shot: consume so this only scrolls/highlights on the frame right
      // after a sideload lands, not on every subsequent draw.
      focus_mod_id_.clear();
      ImGui::SetScrollHereY(0.2f);
    }
    bool pending_removal = pending_removal_ids_.contains(entry.id);
    if (!entry.enabled || pending_removal) {
      ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
    }

    // Row controls stacked in their own vertical column (checkbox, reorder
    // arrows, remove/restore) so they don't crowd the title/icon on one line.
    bool list_changed = false;
    ImGui::BeginGroup();
    bool enabled = entry.enabled;
    ImGui::BeginDisabled(pending_removal);
    if (ImGui::Checkbox("##enabled", &enabled)) {
      entry.enabled = enabled;
      PersistAndRevalidate();
    }
    ImGui::BeginDisabled(i == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
      pending_scroll_restore_ = ImGui::GetScrollY();
      std::swap(entries_[i], entries_[i - 1]);
      PersistAndRevalidate();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(i + 1 >= entries_.size());
    if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < entries_.size()) {
      pending_scroll_restore_ = ImGui::GetScrollY();
      std::swap(entries_[i], entries_[i + 1]);
      PersistAndRevalidate();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();  // pending_removal
    if (pending_removal) {
      if (ImGui::SmallButton("Restore")) {
        pending_scroll_restore_ = ImGui::GetScrollY();
        rex::system::ModState::UnmarkPendingRemoval(mods_root_, entry.id);
        ReloadFromDisk();
        list_changed = true;
      }
    } else {
      // MarkPendingRemoval only touches its own marker file -- unlike a
      // straight ModState::RemoveMod, this can't be blocked by a currently-
      // loaded mod DLL, since nothing is deleted until the next launch
      // (ApplyPendingRemovals), by which point this process (and whatever it
      // had loaded) has exited.
      if (ImGui::SmallButton("Remove")) {
        pending_scroll_restore_ = ImGui::GetScrollY();
        rex::system::ModState::MarkPendingRemoval(mods_root_, entry.id);
        ReloadFromDisk();
        list_changed = true;
      }
    }
    ImGui::EndGroup();
    ImGui::SameLine();

    if (list_changed) {
      if (!enabled || pending_removal) {
        ImGui::PopStyleColor();  // balance the push above before bailing out
      }
      ImGui::PopID();
      // ReloadFromDisk() just replaced entries_/pending_removal_ids_
      // wholesale; bail out of this frame's iteration rather than continuing
      // to index into them with a stale i, or re-touching a row that's now a
      // different mod. The next frame redraws the updated list from scratch.
      break;
    }

    ImmediateTexture* icon = GetLocalIcon(mod);
    if (icon) {
      ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(kIconSize, kIconSize),
                         ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
    } else {
      ImGui::Dummy(ImVec2(kIconSize, kIconSize));
    }
    ImGui::SameLine();

    ImGui::BeginGroup();
    if (entry.enabled) {
      ImGui::TextColored(kHeaderText, "#%d", priority++);
      ImGui::SameLine();
    }
    if (is_focused) {
      ImGui::TextColored(kUpdateBadge, "%s",
                         mod.display_name.empty() ? entry.id.c_str() : mod.display_name.c_str());
    } else {
      ImGui::Text("%s", mod.display_name.empty() ? entry.id.c_str() : mod.display_name.c_str());
    }
    if (pending_removal) {
      ImGui::SameLine();
      ImGui::TextColored(kErrorBadge, "[pending removal]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Will be deleted on restart. Click Restore to keep it.");
      }
    }
    if (!mod.version.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kMutedText, "v%s", mod.version.c_str());
    }
    if (!mod.code.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kCodeBadge, "[code]");
    }

    const auto* catalog_entry = find_catalog_entry(entry.id);
    if (catalog_.state() == rex::system::CatalogState::kReady && !catalog_entry) {
      ImGui::SameLine();
      ImGui::TextColored(kMutedText, "[Sideloaded]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Installed locally (drag-and-drop or manually copied) -- not from the "
            "mod catalog, so it won't get automatic update checks.");
      }
    }

    if (catalog_entry) {
      if (!mod.version.empty() &&
          rex::system::CompareVersionStrings(catalog_entry->version, mod.version) > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kUpdateBadge, "Update available (v%s)", catalog_entry->version.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Update")) {
          catalog_.InstallAsync(*catalog_entry, mods_root_);
        }
      }
    }

    for (const auto* issue : issues_for(entry.id)) {
      ImGui::SameLine();
      bool is_error = issue->kind == rex::system::ModIssue::Kind::kError;
      ImGui::TextColored(is_error ? kErrorBadge : kWarnBadge, is_error ? "[error]" : "[warning]");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", issue->message.c_str());
      }
    }

    ImGui::TextColored(kMutedText, "%s", entry.id.c_str());
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

    if (!entry.enabled || pending_removal) {
      ImGui::PopStyleColor();
    }
    ImGui::Separator();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void ModManagerDialog::DrawCatalogTab() {
  if (ImGui::SmallButton("Refresh")) {
    catalog_.Refresh();
  }
  ImGui::Separator();

  auto mods = catalog_.Snapshot();
  auto install_status = catalog_.InstallSnapshot();
  if (install_status.in_progress) {
    ImGui::TextColored(kMutedText, "Installing... (%llu / %llu bytes)",
                       static_cast<unsigned long long>(install_status.downloaded_bytes),
                       static_cast<unsigned long long>(install_status.total_bytes));
  } else if (install_status.done) {
    ImGui::TextColored(install_status.ok ? kUpdateBadge : kErrorBadge, "%s",
                       install_status.message.c_str());
    if (install_status.ok) {
      // A successful install may have changed what's on disk; pick it up so
      // the Installed tab and restart banner reflect it immediately.
      ReloadFromDisk();
    }
  }
  ImGui::Separator();

  std::string host_platform = rex::system::ModState::HostPlatformId();
  std::string host_version = runtime_ ? runtime_->game_version() : "";

  ImGui::BeginChild("##cataloglist", ImVec2(0.0f, 0.0f), false);
  for (const auto& mod : mods) {
    ImGui::PushID(mod.mod_id.c_str());

    ImmediateTexture* icon = GetRemoteIcon(mod.icon_url);
    if (icon) {
      ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(kIconSize, kIconSize),
                         ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));
    } else {
      ImGui::Dummy(ImVec2(kIconSize, kIconSize));
    }
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("%s", mod.name.empty() ? mod.mod_id.c_str() : mod.name.c_str());
    if (!mod.version.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kMutedText, "v%s", mod.version.c_str());
    }
    if (!mod.author.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(kMutedText, "by %s", mod.author.c_str());
    }
    if (!mod.description.empty()) {
      ImGui::TextWrapped("%s", mod.description.c_str());
    }

    auto installed_it = manifests_.find(mod.mod_id);
    bool already_installed = installed_it != manifests_.end();

    std::string incompatible_reason;
    if (!mod.game_version.empty() &&
        rex::system::CompareVersionStrings(host_version, mod.game_version) < 0) {
      incompatible_reason = "Requires game version >= " + mod.game_version + " (running " +
                            (host_version.empty() ? "unknown" : host_version) + ")";
    } else if (!mod.platforms.empty() && std::find(mod.platforms.begin(), mod.platforms.end(),
                                                   host_platform) == mod.platforms.end()) {
      incompatible_reason =
          "No binary for this platform (ships: " + JoinCommaList(mod.platforms) + ")";
    }

    if (!incompatible_reason.empty()) {
      ImGui::BeginDisabled();
      ImGui::SmallButton("Install");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", incompatible_reason.c_str());
      }
    } else if (already_installed) {
      if (!installed_it->second.version.empty() &&
          rex::system::CompareVersionStrings(mod.version, installed_it->second.version) > 0) {
        if (ImGui::SmallButton("Update")) {
          catalog_.InstallAsync(mod, mods_root_);
        }
      } else {
        ImGui::TextColored(kMutedText, "Installed");
      }
    } else {
      if (ImGui::SmallButton("Install")) {
        catalog_.InstallAsync(mod, mods_root_);
      }
    }
    ImGui::EndGroup();

    ImGui::Separator();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void ModManagerDialog::DrawKeybindsSection(const rex::system::ModInfo& mod) {
  auto binds = rex::ui::SnapshotBinds();
  bool drew_header = false;
  for (const auto& bind : binds) {
    if (bind.owner != mod.folder_name || !bind.active)
      continue;
    if (!drew_header) {
      ImGui::TextColored(kMutedText, "Keybinds:");
      drew_header = true;
    }

    ImGui::PushID(bind.name.c_str());
    ImGui::Text("%s", bind.description.empty() ? bind.name.c_str() : bind.description.c_str());
    ImGui::SameLine();

    bool listening = listening_bind_ == bind.name;
    std::string label = listening ? "...(press a key)..." : bind.effective_key;
    if (label.empty())
      label = "(unbound)";
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
        // SetBindKey only marks the underlying cvar persist-eligible
        // (rex::cvar::SetFlagByName's persist=true) -- it doesn't write
        // anything to disk itself. Without this SaveConfig call, the new
        // key takes effect immediately (SetBindKey updates the live
        // BindEntry) but is lost the moment the process exits, since
        // nothing else was going to flush it to config_path_.
        if (rex::ui::SetBindKey(bind.name, captured) && !config_path_.empty()) {
          rex::cvar::SaveConfig(config_path_);
        }
        listening_bind_.clear();
      }
    }
    ImGui::PopID();
  }
}

void ModManagerDialog::DrawCvarsSection(const rex::system::ModInfo& mod) {
  auto* tracker = runtime_ ? runtime_->mod_conflict_tracker() : nullptr;
  if (!tracker)
    return;
  auto activity = tracker->CvarActivityFor(mod.folder_name);
  if (activity.empty())
    return;
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
