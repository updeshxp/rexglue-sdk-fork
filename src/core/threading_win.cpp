/**
 * @file        core/threading_win.cpp
 * @brief       Windows platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <spdlog/spdlog.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>

#define LOG_LASTERROR() \
  { spdlog::error("Win32 Error 0x{:08X} in {}(...)", GetLastError(), __FUNCTION__); }

typedef HANDLE (*SetThreadDescriptionFn)(HANDLE hThread, PCWSTR lpThreadDescription);

namespace rex::thread {

void EnableAffinityConfiguration() {
  HANDLE process_handle = GetCurrentProcess();
  DWORD_PTR process_affinity_mask;
  DWORD_PTR system_affinity_mask;
  GetProcessAffinityMask(process_handle, &process_affinity_mask, &system_affinity_mask);
  SetProcessAffinityMask(process_handle, system_affinity_mask);
}

void EnsureTimerResolutionMillis(uint32_t period_ms) {
  // The Windows default granularity is ~15.6ms; anything at or above that is
  // already honoured and is not worth burning power on.
  if (!period_ms || period_ms >= 15) {
    return;
  }

  typedef UINT(WINAPI * timePeriodFn)(UINT);
  static std::mutex mutex;
  static timePeriodFn begin_period = nullptr;
  static timePeriodFn end_period = nullptr;
  static bool resolved = false;
  static UINT applied = 0;  // 0 = nothing applied yet

  std::lock_guard<std::mutex> lock(mutex);

  // Already fine enough. The rate only ratchets up, so a coarser later request
  // never walks back a finer one still in use.
  if (applied && period_ms >= applied) {
    return;
  }

  if (!resolved) {
    resolved = true;
    // Loaded dynamically so the SDK takes no hard link dependency on winmm.
    // Deliberately never released: the raised resolution must hold for as long
    // as the guest timers do.
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (winmm) {
      begin_period = reinterpret_cast<timePeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
      end_period = reinterpret_cast<timePeriodFn>(GetProcAddress(winmm, "timeEndPeriod"));
    } else {
      LOG_LASTERROR();
    }
  }
  if (!begin_period) {
    spdlog::warn("Unable to raise host timer resolution; timer-driven guest threads may run slow");
    return;
  }

  // Raise before dropping the old request so resolution never dips in between.
  // TIMERR_NOERROR is 0.
  UINT desired = static_cast<UINT>(period_ms);
  if (begin_period(desired) != 0) {
    spdlog::warn("Host rejected a {}ms timer resolution request", desired);
    return;
  }
  if (applied && end_period) {
    end_period(applied);
  }
  applied = desired;
  spdlog::info("Host timer resolution raised to {}ms for guest periodic timers", desired);
}

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(GetCurrentThreadId());
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
struct THREADNAME_INFO {
  DWORD dwType;      // Must be 0x1000.
  LPCSTR szName;     // Pointer to name (in user addr space).
  DWORD dwThreadID;  // Thread ID (-1=caller thread).
  DWORD dwFlags;     // Reserved for future use, must be zero.
};
#pragma pack(pop)

void raise_thread_name_exception(HANDLE thread, const std::string& name) {
  if (!IsDebuggerPresent()) {
    return;
  }
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name.c_str();
  info.dwThreadID = ::GetThreadId(thread);
  info.dwFlags = 0;
  __try {
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<ULONG_PTR*>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
}

static void set_thread_name_impl(HANDLE thread, const std::string_view name) {
  auto kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto func = (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription");
    if (func) {
      // Convert UTF-8 name to UTF-16 using Windows API
      int size_needed =
          MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
      std::wstring u16name(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), u16name.data(),
                          size_needed);
      func(thread, u16name.c_str());
    }
  }
  raise_thread_name_exception(thread, std::string(name));
}

void set_current_thread_name(const std::string_view name) {
  set_thread_name_impl(GetCurrentThread(), name);
}

void MaybeYield() {
  SwitchToThread();
  MemoryBarrier();
}

void SyncMemory() {
  MemoryBarrier();
}

namespace {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Sleep(n) on the system timer lasts up to a tick longer than asked; a high-resolution timer does not.
class SleepTimer {
 public:
  SleepTimer()
      : handle_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS)) {}
  ~SleepTimer() {
    if (handle_) {
      CloseHandle(handle_);
    }
  }
  SleepTimer(const SleepTimer&) = delete;
  SleepTimer& operator=(const SleepTimer&) = delete;

  // Returns false if the timer is unavailable. *alerted is set when an APC ended an alertable sleep.
  bool Sleep(std::chrono::microseconds duration, bool alertable, bool* alerted) {
    if (!handle_) {
      return false;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(duration.count() * 10);
    if (!SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE)) {
      return false;
    }
    if (WaitForSingleObjectEx(handle_, INFINITE, alertable ? TRUE : FALSE) == WAIT_IO_COMPLETION) {
      CancelWaitableTimer(handle_);
      *alerted = true;
    }
    return true;
  }

 private:
  HANDLE handle_;
};

SleepTimer& GetSleepTimer() {
  thread_local SleepTimer timer;
  return timer;
}

}  // namespace

void Sleep(std::chrono::microseconds duration) {
  if (duration.count() < 100) {
    MaybeYield();
    return;
  }
  bool alerted = false;
  if (duration.count() >= 1000 && GetSleepTimer().Sleep(duration, false, &alerted)) {
    return;
  }
  ::Sleep(static_cast<DWORD>(duration.count() / 1000));
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  bool alerted = false;
  if (duration.count() >= 1000 && GetSleepTimer().Sleep(duration, true, &alerted)) {
    return alerted ? SleepResult::kAlerted : SleepResult::kSuccess;
  }
  if (SleepEx(static_cast<DWORD>(duration.count() / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  return TlsAlloc();
}

bool FreeTlsHandle(TlsHandle handle) {
  return TlsFree(handle) ? true : false;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(TlsGetValue(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return TlsSetValue(handle, reinterpret_cast<void*>(value)) ? true : false;
}

template <typename T>
class Win32Handle : public T {
 public:
  explicit Win32Handle(HANDLE handle) : handle_(handle) { assert_not_null(handle); }
  ~Win32Handle() override {
    CloseHandle(handle_);
    handle_ = nullptr;
  }

 protected:
  void* native_handle() const override { return handle_; }

  HANDLE handle_ = nullptr;
};

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle = wait_handle->native_handle();
  DWORD result = WaitForSingleObjectEx(handle, DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle_to_signal = wait_handle_to_signal->native_handle();
  HANDLE handle_to_wait_on = wait_handle_to_wait_on->native_handle();
  DWORD result = SignalObjectAndWait(handle_to_signal, handle_to_wait_on, DWORD(timeout.count()),
                                     is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<HANDLE> handles(wait_handle_count);
  for (size_t i = 0; i < wait_handle_count; ++i) {
    handles[i] = wait_handles[i]->native_handle();
  }
  DWORD result =
      WaitForMultipleObjectsEx(DWORD(handles.size()), handles.data(), wait_all ? TRUE : FALSE,
                               DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kSuccess, result - WAIT_OBJECT_0);
  } else if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kAbandoned, result - WAIT_ABANDONED_0);
  }
  switch (result) {
    case WAIT_IO_COMPLETION:
      return std::pair<WaitResult, size_t>(WaitResult::kUserCallback, 0);
    case WAIT_TIMEOUT:
      return std::pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
    default:
    case WAIT_FAILED:
      return std::pair<WaitResult, size_t>(WaitResult::kFailed, 0);
  }
}

class Win32Event : public Win32Handle<Event> {
 public:
  explicit Win32Event(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Event() override = default;
  void Set() override { SetEvent(handle_); }
  void Reset() override { ResetEvent(handle_); }
  void Pulse() override { PulseEvent(handle_); }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, TRUE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, FALSE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Semaphore : public Win32Handle<Semaphore> {
 public:
  explicit Win32Semaphore(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Semaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    return ReleaseSemaphore(handle_, release_count, reinterpret_cast<LPLONG>(out_previous_count))
               ? true
               : false;
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  HANDLE handle = CreateSemaphore(nullptr, initial_count, maximum_count, nullptr);
  if (handle) {
    return std::make_unique<Win32Semaphore>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Mutant : public Win32Handle<Mutant> {
 public:
  explicit Win32Mutant(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Mutant() = default;
  bool Release() override { return ReleaseMutex(handle_) ? true : false; }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  HANDLE handle = CreateMutex(nullptr, initial_owner ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Mutant>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Timer : public Win32Handle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit Win32Timer(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Timer() = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback) override {
    return SetOnceAt(WClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(GClock_::time_point due_time, std::function<void()> opt_callback) override {
    return SetOnceAt(rex::chrono::clock_cast<WClock_>(due_time), std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time, std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = WClock_::to_file_time(due_time);
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : NULL;
    return SetWaitableTimer(handle_, &due_time_li, 0, completion_routine, this, FALSE) ? true
                                                                                       : false;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(WClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) {
    return SetRepeatingAt(rex::chrono::clock_cast<WClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = WClock_::to_file_time(due_time);
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : NULL;
    return SetWaitableTimer(handle_, &due_time_li, int32_t(period.count()), completion_routine,
                            this, FALSE)
               ? true
               : false;
  }

  bool Cancel() override {
    // Reset the callback immediately so that any completions don't call it.
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    return CancelWaitableTimer(handle_) ? true : false;
  }

 private:
  static void CompletionRoutine(Win32Timer* timer, DWORD timer_low, DWORD timer_high) {
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    callback();
  }

  std::mutex mutex_;
  std::function<void()> callback_;
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, TRUE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, FALSE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Thread : public Win32Handle<Thread> {
 public:
  explicit Win32Thread(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Thread() = default;

  void set_name(std::string name) override {
    set_thread_name_impl(handle_, name);
    Thread::set_name(name);
  }

  int32_t priority() override { return GetThreadPriority(handle_); }
  uint32_t system_id() const override { return GetThreadId(handle_); }

  void set_priority(int32_t new_priority) override { SetThreadPriority(handle_, new_priority); }

  uint64_t affinity_mask() override {
    DWORD_PTR prev = SetThreadAffinityMask(handle_, 1);
    if (prev) {
      SetThreadAffinityMask(handle_, prev);
    }
    return static_cast<uint64_t>(prev);
  }

  void set_affinity_mask(uint64_t new_affinity_mask) override {
    SetThreadAffinityMask(handle_, new_affinity_mask);
  }

  struct ApcData {
    std::function<void()> callback;
  };
  static void NTAPI DispatchApc(ULONG_PTR parameter) {
    auto apc_data = reinterpret_cast<ApcData*>(parameter);
    apc_data->callback();
    delete apc_data;
  }

  void QueueUserCallback(std::function<void()> callback) override {
    auto apc_data = new ApcData({std::move(callback)});
    QueueUserAPC(DispatchApc, handle_, reinterpret_cast<ULONG_PTR>(apc_data));
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = ResumeThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = SuspendThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  void Terminate(int exit_code) override { TerminateThread(handle_, exit_code); }

 private:
  void AssertCallingThread() { assert_true(GetCurrentThreadId() == GetThreadId(handle_)); }
};

thread_local std::unique_ptr<Win32Thread> current_thread_ = nullptr;

struct ThreadStartData {
  std::function<void()> start_routine;
};
DWORD WINAPI ThreadStartRoutine(LPVOID parameter) {
  current_thread_ = std::make_unique<Win32Thread>(::GetCurrentThread());

  auto start_data = reinterpret_cast<ThreadStartData*>(parameter);
  start_data->start_routine();
  delete start_data;
  return 0;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto start_data = new ThreadStartData({std::move(start_routine)});
  HANDLE handle = CreateThread(NULL, params.stack_size, ThreadStartRoutine, start_data,
                               params.create_suspended ? CREATE_SUSPENDED : 0, NULL);
  if (handle) {
    return std::make_unique<Win32Thread>(handle);
  } else {
    LOG_LASTERROR();
    delete start_data;
    return nullptr;
  }
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_.get();
  }

  HANDLE handle = ::GetCurrentThread();
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }

  current_thread_ = std::make_unique<Win32Thread>(handle);
  return current_thread_.get();
}

void Thread::Exit(int exit_code) {
  ExitThread(exit_code);
}

}  // namespace rex::thread
