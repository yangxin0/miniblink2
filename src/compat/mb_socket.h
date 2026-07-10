// mb_socket.h — INTERNAL cross-platform TCP socket compat for the library.
//
// This is engine-internal platform glue (src/compat/), NOT part of the public
// mb* SDK: the socket types never cross the C ABI. It abstracts the handful of
// differences between BSD sockets (POSIX/macOS) and Winsock2 (Windows) —
// handle type, invalid value, close/shutdown, recv/send length, one-time
// library init — so feature code (the in-engine DevTools endpoint,
// mb_devtools_server.cc) reads the same on both platforms.

#ifndef MB_COMPAT_MB_SOCKET_H_
#define MB_COMPAT_MB_SOCKET_H_

#include <cstddef>

#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif BUILDFLAG(IS_POSIX)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mb {
namespace compat {

#if BUILDFLAG(IS_WIN)

using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline constexpr int kShutdownBoth = SD_BOTH;
inline bool SocketValid(socket_t s) { return s != INVALID_SOCKET; }
inline void SocketClose(socket_t s) { ::closesocket(s); }
inline long SocketRecv(socket_t s, char* b, std::size_t n) {
  return ::recv(s, b, static_cast<int>(n), 0);
}
inline long SocketSend(socket_t s, const char* b, std::size_t n) {
  return ::send(s, b, static_cast<int>(n), 0);
}
// Winsock must be initialized once per process before any socket call.
inline bool SocketPlatformInit() {
  static bool ok = [] {
    WSADATA wsa;
    return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  return ok;
}

#elif BUILDFLAG(IS_POSIX)

using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline constexpr int kShutdownBoth = SHUT_RDWR;
inline bool SocketValid(socket_t s) { return s >= 0; }
inline void SocketClose(socket_t s) { ::close(s); }
inline long SocketRecv(socket_t s, char* b, std::size_t n) {
  return ::recv(s, b, n, 0);
}
inline long SocketSend(socket_t s, const char* b, std::size_t n) {
  return ::send(s, b, n, 0);
}
inline bool SocketPlatformInit() { return true; }  // no per-process init needed

#endif

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_POSIX)
// setsockopt with an int value (the option ptr is const char* on Windows,
// const void* on POSIX — const char* converts on both).
inline void SocketSetOptInt(socket_t s, int level, int opt, int val) {
  ::setsockopt(s, level, opt, reinterpret_cast<const char*>(&val), sizeof(val));
}
#endif

}  // namespace compat
}  // namespace mb

#endif  // MB_COMPAT_MB_SOCKET_H_
