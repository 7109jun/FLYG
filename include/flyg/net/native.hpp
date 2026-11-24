// flyG - net module: platform socket headers and small helpers (internal)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstring>
#include <string>
#include <string_view>

#include <flyg/core/exception.hpp>
#include <flyg/core/platform.hpp>
#include <flyg/core/types.hpp>

#if FLYG_PLATFORM_WINDOWS
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32.lib")
#    pragma comment(lib, "iphlpapi.lib")
#  endif
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/ip.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace flyg::net::detail {

using core::u16;
using core::u32;
using core::u8;
using core::usize;

#if FLYG_PLATFORM_WINDOWS
using native_socket = SOCKET;
inline constexpr native_socket invalid_socket = INVALID_SOCKET;
using sock_len = int;
[[nodiscard]] inline int last_error() noexcept { return ::WSAGetLastError(); }
inline constexpr int err_would_block = WSAEWOULDBLOCK;
inline constexpr int err_in_progress = WSAEINPROGRESS;
inline constexpr int err_interrupted = WSAEINTR;
inline constexpr int err_conn_refused = WSAECONNREFUSED;
inline constexpr int err_timed_out = WSAETIMEDOUT;
inline constexpr int err_conn_reset = WSAECONNRESET;
inline constexpr int err_addr_in_use = WSAEADDRINUSE;
/// Initializes Winsock exactly once per process (thread-safe, never torn down).
inline void ensure_init() {
    static const int rc = [] {
        WSADATA d;
        return ::WSAStartup(MAKEWORD(2, 2), &d);
    }();
    if (rc != 0) throw NetworkException("WSAStartup failed", rc);
}
inline void close_socket(native_socket s) noexcept { ::closesocket(s); }
#else
using native_socket = int;
inline constexpr native_socket invalid_socket = -1;
using sock_len = socklen_t;
[[nodiscard]] inline int last_error() noexcept { return errno; }
inline constexpr int err_would_block = EWOULDBLOCK;
inline constexpr int err_in_progress = EINPROGRESS;
inline constexpr int err_interrupted = EINTR;
inline constexpr int err_conn_refused = ECONNREFUSED;
inline constexpr int err_timed_out = ETIMEDOUT;
inline constexpr int err_conn_reset = ECONNRESET;
inline constexpr int err_addr_in_use = EADDRINUSE;
inline void ensure_init() {}
inline void close_socket(native_socket s) noexcept { ::close(s); }
#endif

[[nodiscard]] inline bool is_would_block(int e) noexcept {
#if FLYG_PLATFORM_WINDOWS
    return e == WSAEWOULDBLOCK;
#else
    return e == EWOULDBLOCK || e == EAGAIN;
#endif
}

[[nodiscard]] inline std::string error_text(int e) {
#if FLYG_PLATFORM_WINDOWS
    char* buf = nullptr;
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                     static_cast<DWORD>(e), 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string s = n ? std::string(buf, n) : std::string("unknown error");
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
#else
    return std::string(std::strerror(e));
#endif
}

[[noreturn]] inline void throw_net(const std::string& what, int e) {
    throw NetworkException(what + ": " + error_text(e), e);
}
[[noreturn]] inline void throw_net(const std::string& what) { throw_net(what, last_error()); }

}  // namespace flyg::net::detail
