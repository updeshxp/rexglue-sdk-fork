#include <atomic>

#include <rex/graphics/guest_wake.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#elif REX_PLATFORM_LINUX
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rex::graphics {

void WakeGuestWordWaiters(const void* host_address) {
#if REX_PLATFORM_WIN32
  // The store being announced must be ordered before the wake looks for waiters.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  WakeByAddressAll(const_cast<void*>(host_address));
#elif REX_PLATFORM_LINUX
  std::atomic_thread_fence(std::memory_order_seq_cst);
  syscall(SYS_futex, host_address, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#else
  (void)host_address;
#endif
}

}  // namespace rex::graphics
