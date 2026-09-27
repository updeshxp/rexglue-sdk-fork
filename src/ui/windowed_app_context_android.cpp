/**
 * @file        ui/windowed_app_context_android.cpp
 * @brief       Direct Android NDK native UI loop context
 *
 * @copyright   Copyright (c) 2026 ReXGlue Authors
 * @license     BSD 3-Clause License
 */

#include <rex/ui/windowed_app_context_android.h>
#include <rex/ui/window_android.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <rex/logging.h>

namespace rex::ui {

AndroidWindowedAppContext::AndroidWindowedAppContext() = default;

AndroidWindowedAppContext::~AndroidWindowedAppContext() {
  if (wakeup_event_fd_ >= 0) {
    if (looper_) {
      ALooper_removeFd(looper_, wakeup_event_fd_);
    }
    close(wakeup_event_fd_);
    wakeup_event_fd_ = -1;
  }
  ExecutePendingFunctionsFromUIThread();
}

bool AndroidWindowedAppContext::Initialize() {
  looper_ = ALooper_forThread();
  if (!looper_) {
    looper_ = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  }

  wakeup_event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeup_event_fd_ < 0) {
    REXLOG_ERROR("AndroidWindowedAppContext: eventfd creation failed");
    return false;
  }

  if (looper_) {
    ALooper_addFd(looper_, wakeup_event_fd_, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                  LooperCallback, this);
  }

  return true;
}

int AndroidWindowedAppContext::LooperCallback(int fd, int /*events*/, void* data) {
  uint64_t counter = 0;
  read(fd, &counter, sizeof(counter));
  auto* context = static_cast<AndroidWindowedAppContext*>(data);
  context->ExecutePendingFunctionsFromUIThread();
  return 1;  // Keep callback active
}

void AndroidWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  if (wakeup_event_fd_ >= 0) {
    uint64_t increment = 1;
    write(wakeup_event_fd_, &increment, sizeof(increment));
  }
}

void AndroidWindowedAppContext::PlatformQuitFromUIThread() {
  quit_requested_.store(true, std::memory_order_release);
  NotifyUILoopOfPendingFunctions();
}

void AndroidWindowedAppContext::QuitMessageLoop() {
  PlatformQuitFromUIThread();
}

int AndroidWindowedAppContext::RunMainMessageLoop() {
  while (!HasQuitFromUIThread() && !quit_requested_.load(std::memory_order_acquire)) {
    // Process looper events with a short timeout to handle paint/vsync
    int result = ALooper_pollOnce(16, nullptr, nullptr, nullptr);
    if (result == ALOOPER_POLL_ERROR) {
      REXLOG_ERROR("AndroidWindowedAppContext: ALooper_pollOnce returned error");
      break;
    }

    ExecutePendingFunctionsFromUIThread();

    if (window_) {
      window_->HandleFrameTick();
    }
  }

  return 0;
}

void AndroidWindowedAppContext::RegisterWindow(WindowAndroid* window) {
  window_ = window;
}

void AndroidWindowedAppContext::UnregisterWindow(WindowAndroid* window) {
  if (window_ == window) {
    window_ = nullptr;
  }
}

}  // namespace rex::ui
