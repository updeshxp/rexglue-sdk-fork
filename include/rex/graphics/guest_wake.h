#pragma once

namespace rex::graphics {

// Wakes host threads blocked on this guest memory word (Windows WaitOnAddress, Linux futex); a no-op elsewhere.
void WakeGuestWordWaiters(const void* host_address);

}  // namespace rex::graphics
