#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

static unsigned long TranslateWinsockIoctl(uint32_t cmd) {
  // Winsock FIONBIO (0x8004667E) differs from POSIX FIONBIO (0x5421).
  // The guest passes Winsock constants; translate to POSIX equivalents.
  constexpr uint32_t kWinsockFIONBIO = 0x8004667Eu;
  constexpr uint32_t kWinsockFIONREAD = 0x4004667Fu;
  switch (cmd) {
    case kWinsockFIONBIO:
      return FIONBIO;
    case kWinsockFIONREAD:
      return FIONREAD;
    default:
      return cmd;
  }
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  return ioctl(static_cast<int>(handle), TranslateWinsockIoctl(cmd), arg);
}

}  // namespace rex::net
