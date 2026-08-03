/**
 * @file        ui/overlay/hint_toast.cpp
 * @brief       Hint toast implementation. See hint_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/hint_toast.h>
#include <imgui.h>

#include <algorithm>

namespace rex::ui {

HintToastDialog::HintToastDialog(ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

void HintToastDialog::Show(std::string message) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = std::move(message);
  visible_since_.reset();
  dismissed_ = false;
}

void HintToastDialog::OnDraw(ImGuiIO& io) {
  std::string message;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dismissed_ || message_.empty()) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (!visible_since_) {
      visible_since_ = now;
    }
    age = std::chrono::duration<float>(now - *visible_since_).count();
    if (age >= kDisplaySeconds) {
      dismissed_ = true;
      return;
    }
    message = message_;
  }

  float alpha = 1.0f;
  if (age < 0.3f) {
    alpha = age / 0.3f;
  } else if (age > kDisplaySeconds - 0.6f) {
    alpha = (kDisplaySeconds - age) / 0.6f;
  }
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  const float pad = 16.0f;
  ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.85f * alpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
  if (ImGui::Begin("##hint_toast", nullptr, flags)) {
    ImGui::TextUnformatted(message.c_str());
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

}  // namespace rex::ui
