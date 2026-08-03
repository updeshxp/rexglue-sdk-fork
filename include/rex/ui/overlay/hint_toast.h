/**
 * @file        rex/ui/overlay/hint_toast.h
 * @brief       Generic single-message toast -- top-left corner,
 *              auto-dismisses after a few seconds.
 *              See RuntimeConfig::startup_hint.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class HintToastDialog : public ImGuiDialog {
 public:
  explicit HintToastDialog(ImGuiDrawer* drawer);

  // Thread-safe: safe to call from any thread. Replaces whatever hint is
  // currently showing/queued
  void Show(std::string message);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kDisplaySeconds = 5.0f;

  std::mutex mutex_;
  std::string message_;
  std::optional<std::chrono::steady_clock::time_point> visible_since_;
  bool dismissed_ = false;
};

}  // namespace rex::ui
