// flyG - net module: TCP, UDP and raw sockets, polling, network interfaces
// SPDX-License-Identifier: GPL-2.0-only
//
// All sockets are non-blocking internally; the blocking-style calls (read_some, write_all,
// accept, ...) wait with poll() and honour per-socket timeouts, which makes them behave the
// same on Linux and Windows. The try_* calls never block and are what the EventLoop uses.
#pragma once

#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <flyg/net/address.hpp>
#include <flyg/net/resolve.hpp>

namespace flyg::net {

using Timeout = std::chrono::milliseconds;
inline constexpr Timeout no_timeout{-1};

/// A network operation did not complete in time.
class TimeoutException : public NetworkException {
public:
    explicit TimeoutException(const std::string& what) : NetworkException(what + " timed out") {}
};

namespace detail {

#if FLYG_PLATFORM_WINDOWS
using pollfd_t = WSAPOLLFD;
inline int poll_raw(pollfd_t* p, usize n, int ms) { return ::WSAPoll(p, static_cast<ULONG>(n), ms); }
inline constexpr int send_flags = 0;
using io_len_t = int;
#else
using pollfd_t = ::pollfd;
inline int poll_raw(pollfd_t* p, usize n, int ms) { return ::poll(p, static_cast<nfds_t>(n), ms); }
inline constexpr int send_flags = MSG_NOSIGNAL;
using io_len_t = std::size_t;
#endif

/// poll() with EINTR handling. Returns the number of ready descriptors (0 on timeout).
inline int poll_sockets(pollfd_t* fds, usize n, Timeout timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = timeout.count() < 0 ? clock::time_point::max() : clock::now() + timeout;
    for (;;) {
        int ms = -1;
        if (timeout.count() >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            ms = left < 0 ? 0 : static_cast<int>(std::min<long long>(left, 1 << 30));
        }
#if FLYG_PLATFORM_WINDOWS
        if (n == 0) {  // WSAPoll rejects an empty set
            if (ms > 0) ::Sleep(static_cast<DWORD>(ms));
            return 0;
        }
#endif
        const int r = poll_raw(fds, n, ms);
        if (r >= 0) return r;
        if (last_error() == err_interrupted) continue;
        throw_net("poll failed");
    }
}

inline bool wait_socket(native_socket s, short events, Timeout timeout) {
    pollfd_t p{};
    p.fd = s;
    p.events = events;
    return poll_sockets(&p, 1, timeout) > 0;
}

}  // namespace detail

/// Event mask for poll().
enum PollEvent : unsigned { poll_read = 1, poll_write = 2, poll_error = 4, poll_hangup = 8 };

class Socket {
public:
    Socket() noexcept = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : s_(std::exchange(o.s_, detail::invalid_socket)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            s_ = std::exchange(o.s_, detail::invalid_socket);
        }
        return *this;
    }
    ~Socket() { close(); }

    [[nodiscard]] bool valid() const noexcept { return s_ != detail::invalid_socket; }
    [[nodiscard]] detail::native_socket native_handle() const noexcept { return s_; }
    void close() noexcept {
        if (!valid()) return;
        // shutdown() first: it wakes threads blocked in poll()/recv() on this socket (a bare close() does not).
#if FLYG_PLATFORM_WINDOWS
        (void)::shutdown(s_, SD_BOTH);
#else
        (void)::shutdown(s_, SHUT_RDWR);
#endif
        detail::close_socket(std::exchange(s_, detail::invalid_socket));
    }
    detail::native_socket release() noexcept { return std::exchange(s_, detail::invalid_socket); }

    [[nodiscard]] Endpoint local_endpoint() const {
        sockaddr_storage ss{};
        detail::sock_len len = sizeof ss;
        if (::getsockname(s_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) detail::throw_net("getsockname failed");
        return Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss));
    }
    [[nodiscard]] Endpoint peer_endpoint() const {
        sockaddr_storage ss{};
        detail::sock_len len = sizeof ss;
        if (::getpeername(s_, reinterpret_cast<sockaddr*>(&ss), &len) != 0) detail::throw_net("getpeername failed");
        return Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss));
    }

    void set_nonblocking(bool on) {
#if FLYG_PLATFORM_WINDOWS
        u_long v = on ? 1 : 0;
        if (::ioctlsocket(s_, FIONBIO, &v) != 0) detail::throw_net("ioctlsocket(FIONBIO) failed");
#else
        const int fl = ::fcntl(s_, F_GETFL, 0);
        if (fl < 0 || ::fcntl(s_, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) != 0) detail::throw_net("fcntl(O_NONBLOCK) failed");
#endif
    }
    void set_reuse_address(bool on) {
#if FLYG_PLATFORM_WINDOWS
        // On Windows SO_REUSEADDR allows hijacking a port; SO_EXCLUSIVEADDRUSE is the safe default.
        (void)on;
#else
        set_int(SOL_SOCKET, SO_REUSEADDR, on);
#endif
    }
    void set_reuse_port(bool on) {
#if defined(SO_REUSEPORT)
        set_int(SOL_SOCKET, SO_REUSEPORT, on);
#else
        (void)on;
        throw NetworkException("SO_REUSEPORT is not supported on this platform");
#endif
    }
    void set_keep_alive(bool on) { set_int(SOL_SOCKET, SO_KEEPALIVE, on); }
    void set_broadcast(bool on) { set_int(SOL_SOCKET, SO_BROADCAST, on); }
    void set_receive_buffer(int bytes) { set_int(SOL_SOCKET, SO_RCVBUF, bytes); }
    void set_send_buffer(int bytes) { set_int(SOL_SOCKET, SO_SNDBUF, bytes); }
    [[nodiscard]] int receive_buffer() const { return get_int(SOL_SOCKET, SO_RCVBUF); }
    [[nodiscard]] int send_buffer() const { return get_int(SOL_SOCKET, SO_SNDBUF); }
    void set_ipv6_only(bool on) { set_int(IPPROTO_IPV6, IPV6_V6ONLY, on); }
    /// Pending asynchronous error (SO_ERROR), 0 if none. Reading clears it.
    [[nodiscard]] int take_error() const { return get_int(SOL_SOCKET, SO_ERROR); }

    /// Waits until readable (or, with `for_write`, writable). Returns false on timeout.
    [[nodiscard]] bool wait_readable(Timeout t) const { return detail::wait_socket(s_, POLLIN, t); }
    [[nodiscard]] bool wait_writable(Timeout t) const { return detail::wait_socket(s_, POLLOUT, t); }

    void set_read_timeout(Timeout t) noexcept { read_timeout_ = t; }
    void set_write_timeout(Timeout t) noexcept { write_timeout_ = t; }
    [[nodiscard]] Timeout read_timeout() const noexcept { return read_timeout_; }
    [[nodiscard]] Timeout write_timeout() const noexcept { return write_timeout_; }

protected:
    Socket(int family, int type, int protocol) {
        detail::ensure_init();
        int t = type;
#if FLYG_PLATFORM_LINUX
        t |= SOCK_CLOEXEC;
#endif
        s_ = ::socket(family, t, protocol);
        if (s_ == detail::invalid_socket) detail::throw_net("socket creation failed");
        try {
            set_nonblocking(true);
        } catch (...) {
            close();
            throw;
        }
    }
    explicit Socket(detail::native_socket s) : s_(s) {
        if (valid()) set_nonblocking(true);
    }

    void set_int(int level, int opt, int v) {
        if (::setsockopt(s_, level, opt, reinterpret_cast<const char*>(&v), sizeof v) != 0) detail::throw_net("setsockopt failed");
    }
    [[nodiscard]] int get_int(int level, int opt) const {
        int v = 0;
        detail::sock_len len = sizeof v;
        if (::getsockopt(s_, level, opt, reinterpret_cast<char*>(&v), &len) != 0) detail::throw_net("getsockopt failed");
        return v;
    }
    void bind_to(const Endpoint& ep) {
        sockaddr_storage ss;
        const auto len = ep.to_sockaddr(ss);
        if (::bind(s_, reinterpret_cast<sockaddr*>(&ss), len) != 0) detail::throw_net("bind to " + ep.to_string() + " failed");
    }
    static int af(const IpAddress& a) noexcept { return a.is_v4() ? AF_INET : AF_INET6; }

    detail::native_socket s_ = detail::invalid_socket;
    Timeout read_timeout_ = no_timeout;
    Timeout write_timeout_ = no_timeout;
};

// ---- TCP -----------------------------------------------------------------------------------------

class TcpStream : public Socket {
public:
    TcpStream() noexcept = default;

    /// Connects to an address (with timeout).
    [[nodiscard]] static TcpStream connect(const Endpoint& ep, Timeout timeout = no_timeout) {
        TcpStream s(ep.address().family());
        sockaddr_storage ss;
        const auto len = ep.to_sockaddr(ss);
        if (::connect(s.s_, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
            const int e = detail::last_error();
            if (e != detail::err_in_progress && !detail::is_would_block(e) && e != detail::err_interrupted)
                detail::throw_net("connect to " + ep.to_string() + " failed", e);
            if (!s.wait_writable(timeout)) throw TimeoutException("connect to " + ep.to_string());
            if (const int se = s.take_error(); se != 0) detail::throw_net("connect to " + ep.to_string() + " failed", se);
        }
        s.set_no_delay(true);
        return s;
    }
    /// Starts a connection without waiting. Wait for writability, then call finish_connect().
    [[nodiscard]] static TcpStream begin_connect(const Endpoint& ep) {
        TcpStream s(ep.address().family());
        sockaddr_storage ss;
        const auto len = ep.to_sockaddr(ss);
        if (::connect(s.s_, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
            const int e = detail::last_error();
            if (e != detail::err_in_progress && !detail::is_would_block(e) && e != detail::err_interrupted)
                detail::throw_net("connect to " + ep.to_string() + " failed", e);
        }
        return s;
    }
    /// Completes begin_connect(): throws if the connection failed.
    void finish_connect() {
        if (const int se = take_error(); se != 0) detail::throw_net("connect failed", se);
        set_no_delay(true);
    }
    /// Resolves `host` and tries each address in turn (the timeout applies per address).
    [[nodiscard]] static TcpStream connect(const std::string& host, u16 port, Timeout timeout = no_timeout, Family family = Family::any) {
        const auto addrs = resolve(host, family);
        std::optional<NetworkException> last;
        for (const auto& a : addrs) {
            try {
                return connect(Endpoint(a, port), timeout);
            } catch (const NetworkException& e) {
                last = e;
            }
        }
        throw *last;
    }

    void set_no_delay(bool on) { set_int(IPPROTO_TCP, TCP_NODELAY, on); }

    /// Never blocks: nullopt = nothing available now, 0 = orderly shutdown by the peer.
    [[nodiscard]] std::optional<usize> try_read(std::span<std::byte> buf) {
        for (;;) {
            const auto n = ::recv(s_, reinterpret_cast<char*>(buf.data()), static_cast<detail::io_len_t>(std::min<usize>(buf.size(), 1u << 30)), 0);
            if (n >= 0) return static_cast<usize>(n);
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) return std::nullopt;
            detail::throw_net("recv failed", e);
        }
    }
    [[nodiscard]] std::optional<usize> try_write(std::span<const std::byte> buf) {
        for (;;) {
            const auto n = ::send(s_, reinterpret_cast<const char*>(buf.data()), static_cast<detail::io_len_t>(std::min<usize>(buf.size(), 1u << 30)), detail::send_flags);
            if (n >= 0) return static_cast<usize>(n);
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) return std::nullopt;
            detail::throw_net("send failed", e);
        }
    }

    /// Reads at least one byte unless the peer closed (returns 0). Honours the read timeout.
    usize read_some(std::span<std::byte> buf) {
        if (buf.empty()) return 0;
        for (;;) {
            if (auto n = try_read(buf)) return *n;
            if (!wait_readable(read_timeout_)) throw TimeoutException("read");
        }
    }
    /// Fills the buffer completely or throws (connection closed early / timeout).
    void read_exact(std::span<std::byte> buf) {
        usize done = 0;
        while (done < buf.size()) {
            const usize n = read_some(buf.subspan(done));
            if (n == 0) throw NetworkException("connection closed before all data was received");
            done += n;
        }
    }
    usize write_some(std::span<const std::byte> buf) {
        for (;;) {
            if (auto n = try_write(buf)) return *n;
            if (!wait_writable(write_timeout_)) throw TimeoutException("write");
        }
    }
    void write_all(std::span<const std::byte> buf) {
        while (!buf.empty()) buf = buf.subspan(write_some(buf));
    }
    void write_all(std::string_view s) { write_all(std::as_bytes(std::span(s.data(), s.size()))); }

    enum class Shutdown { read, write, both };
    void shutdown(Shutdown how = Shutdown::both) {
#if FLYG_PLATFORM_WINDOWS
        const int h = how == Shutdown::read ? SD_RECEIVE : how == Shutdown::write ? SD_SEND : SD_BOTH;
#else
        const int h = how == Shutdown::read ? SHUT_RD : how == Shutdown::write ? SHUT_WR : SHUT_RDWR;
#endif
        if (::shutdown(s_, h) != 0 && detail::last_error() != ENOTCONN_VALUE()) detail::throw_net("shutdown failed");
    }
    /// Closes with RST instead of a graceful FIN.
    void abort() {
        struct linger l {};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        close();
    }

private:
    friend class TcpListener;
    explicit TcpStream(Family f) : Socket(f == Family::v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP) {}
    explicit TcpStream(detail::native_socket s) : Socket(s) {}
    static constexpr int ENOTCONN_VALUE() noexcept {
#if FLYG_PLATFORM_WINDOWS
        return WSAENOTCONN;
#else
        return ENOTCONN;
#endif
    }
};

class TcpListener : public Socket {
public:
    TcpListener() noexcept = default;

    /// Binds and listens. Port 0 picks a free port (see local_endpoint()). An IPv6 wildcard
    /// address accepts IPv4 as well unless `v6_only`.
    [[nodiscard]] static TcpListener bind(const Endpoint& ep, int backlog = 128, bool reuse_address = true, bool v6_only = false) {
        TcpListener l(af(ep.address()));
        if (reuse_address) l.set_reuse_address(true);
        if (ep.address().is_v6()) l.set_ipv6_only(v6_only);
#if FLYG_PLATFORM_WINDOWS
        l.set_int(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#endif
        l.bind_to(ep);
        if (::listen(l.s_, backlog) != 0) detail::throw_net("listen failed");
        return l;
    }

    /// Never blocks: nullopt when no connection is pending.
    [[nodiscard]] std::optional<std::pair<TcpStream, Endpoint>> try_accept() {
        for (;;) {
            sockaddr_storage ss{};
            detail::sock_len len = sizeof ss;
            const auto c = ::accept(s_, reinterpret_cast<sockaddr*>(&ss), &len);
            if (c != detail::invalid_socket) {
                TcpStream s(c);
                s.set_no_delay(true);
                return std::pair<TcpStream, Endpoint>(std::move(s), Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss)));
            }
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) return std::nullopt;
#if FLYG_PLATFORM_LINUX
            if (e == ECONNABORTED || e == EPROTO) continue;  // connection died before we got it
#endif
            detail::throw_net("accept failed", e);
        }
    }
    /// Waits for a connection; nullopt on timeout.
    [[nodiscard]] std::optional<std::pair<TcpStream, Endpoint>> accept(Timeout timeout = no_timeout) {
        using clock = std::chrono::steady_clock;
        const auto deadline = timeout.count() < 0 ? clock::time_point::max() : clock::now() + timeout;
        for (;;) {
            if (auto c = try_accept()) return c;
            Timeout left = no_timeout;
            if (timeout.count() >= 0) {
                left = std::chrono::duration_cast<Timeout>(deadline - clock::now());
                if (left.count() < 0) left = Timeout{0};
            }
            if (!wait_readable(left)) return std::nullopt;
        }
    }

private:
    explicit TcpListener(int family) : Socket(family, SOCK_STREAM, IPPROTO_TCP) {}
};

// ---- UDP -----------------------------------------------------------------------------------------

class UdpSocket : public Socket {
public:
    UdpSocket() noexcept = default;

    /// Creates a socket and binds it (port 0 = ephemeral).
    [[nodiscard]] static UdpSocket bind(const Endpoint& ep, bool reuse_address = false) {
        UdpSocket u(ep.address().family());
        if (reuse_address) u.set_reuse_address(true);
        u.bind_to(ep);
        return u;
    }
    /// Unbound socket of the given family (bound implicitly on first send).
    [[nodiscard]] static UdpSocket open(Family f = Family::v4) { return UdpSocket(f); }

    /// Fixes the default peer for send()/recv() and filters incoming datagrams.
    void connect(const Endpoint& ep) {
        sockaddr_storage ss;
        const auto len = ep.to_sockaddr(ss);
        if (::connect(s_, reinterpret_cast<sockaddr*>(&ss), len) != 0) detail::throw_net("udp connect failed");
    }

    void send_to(std::span<const std::byte> data, const Endpoint& to) {
        sockaddr_storage ss;
        const auto len = to.to_sockaddr(ss);
        for (;;) {
            const auto n = ::sendto(s_, reinterpret_cast<const char*>(data.data()), static_cast<detail::io_len_t>(data.size()), detail::send_flags, reinterpret_cast<sockaddr*>(&ss), len);
            if (n >= 0) return;
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) {
                if (!wait_writable(write_timeout_)) throw TimeoutException("udp send");
                continue;
            }
            detail::throw_net("sendto " + to.to_string() + " failed", e);
        }
    }
    void send_to(std::string_view data, const Endpoint& to) { send_to(std::as_bytes(std::span(data.data(), data.size())), to); }
    void send(std::span<const std::byte> data) {
        for (;;) {
            const auto n = ::send(s_, reinterpret_cast<const char*>(data.data()), static_cast<detail::io_len_t>(data.size()), detail::send_flags);
            if (n >= 0) return;
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) {
                if (!wait_writable(write_timeout_)) throw TimeoutException("udp send");
                continue;
            }
            detail::throw_net("send failed", e);
        }
    }

    struct Datagram {
        usize size = 0;
        Endpoint from;
    };
    /// Never blocks. A datagram larger than `buf` is truncated.
    [[nodiscard]] std::optional<Datagram> try_receive(std::span<std::byte> buf) {
        for (;;) {
            sockaddr_storage ss{};
            detail::sock_len len = sizeof ss;
            const auto n = ::recvfrom(s_, reinterpret_cast<char*>(buf.data()), static_cast<detail::io_len_t>(buf.size()), 0, reinterpret_cast<sockaddr*>(&ss), &len);
            if (n >= 0) return Datagram{static_cast<usize>(n), Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss))};
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) return std::nullopt;
#if FLYG_PLATFORM_WINDOWS
            if (e == WSAEMSGSIZE) return Datagram{buf.size(), Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss))};
            if (e == WSAECONNRESET) continue;  // ICMP port unreachable from an earlier send
#endif
            detail::throw_net("recvfrom failed", e);
        }
    }
    /// Waits for a datagram; throws TimeoutException when the read timeout passes.
    Datagram receive(std::span<std::byte> buf) {
        for (;;) {
            if (auto d = try_receive(buf)) return *d;
            if (!wait_readable(read_timeout_)) throw TimeoutException("udp receive");
        }
    }
    /// Like receive() but returns nullopt on timeout.
    [[nodiscard]] std::optional<Datagram> receive_for(std::span<std::byte> buf, Timeout t) {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + t;
        for (;;) {
            if (auto d = try_receive(buf)) return d;
            auto left = std::chrono::duration_cast<Timeout>(deadline - clock::now());
            if (left.count() < 0) left = Timeout{0};
            if (!wait_readable(left)) return std::nullopt;
        }
    }

    void join_multicast(const IpAddress& group, u32 interface_index = 0) {
        if (group.is_v4()) {
            ip_mreq m{};
            std::memcpy(&m.imr_multiaddr, group.data(), 4);
            m.imr_interface.s_addr = htonl(INADDR_ANY);
            if (::setsockopt(s_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof m) != 0) detail::throw_net("join multicast group failed");
        } else {
            ipv6_mreq m{};
            std::memcpy(&m.ipv6mr_multiaddr, group.data(), 16);
            m.ipv6mr_interface = interface_index;
            if (::setsockopt(s_, IPPROTO_IPV6, IPV6_JOIN_GROUP, reinterpret_cast<const char*>(&m), sizeof m) != 0) detail::throw_net("join multicast group failed");
        }
    }
    void set_multicast_loop(bool on, Family f = Family::v4) {
        if (f == Family::v6) set_int(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, on);
        else set_int(IPPROTO_IP, IP_MULTICAST_LOOP, on);
    }
    void set_multicast_ttl(int ttl, Family f = Family::v4) {
        if (f == Family::v6) set_int(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, ttl);
        else set_int(IPPROTO_IP, IP_MULTICAST_TTL, ttl);
    }
    void set_ttl(int ttl, Family f = Family::v4) {
        if (f == Family::v6) set_int(IPPROTO_IPV6, IPV6_UNICAST_HOPS, ttl);
        else set_int(IPPROTO_IP, IP_TTL, ttl);
    }

private:
    explicit UdpSocket(Family f) : Socket(f == Family::v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP) {}
};

// ---- raw sockets ---------------------------------------------------------------------------------

/// Raw IP socket for a given protocol number (e.g. 1 = ICMP). Usually needs administrator /
/// CAP_NET_RAW rights; open() throws NetworkException otherwise. IPv4 receive buffers include the
/// IP header; IPv6 ones do not (OS behaviour).
class RawSocket : public Socket {
public:
    RawSocket() noexcept = default;
    [[nodiscard]] static RawSocket open(Family f, int protocol) {
        RawSocket r(f == Family::v6 ? AF_INET6 : AF_INET, SOCK_RAW, protocol);
        return r;
    }
    /// ICMP "ping socket" (SOCK_DGRAM, no privileges needed on Linux when net.ipv4.ping_group_range
    /// allows it; the kernel manages the echo id and strips the IP header).
    [[nodiscard]] static RawSocket open_icmp_datagram(Family f) {
        return RawSocket(f == Family::v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, f == Family::v6 ? static_cast<int>(IPPROTO_ICMPV6) : static_cast<int>(IPPROTO_ICMP));
    }
    void send_to(std::span<const std::byte> data, const IpAddress& to) {
        sockaddr_storage ss;
        const auto len = Endpoint(to, 0).to_sockaddr(ss);
        for (;;) {
            const auto n = ::sendto(s_, reinterpret_cast<const char*>(data.data()), static_cast<detail::io_len_t>(data.size()), detail::send_flags, reinterpret_cast<sockaddr*>(&ss), len);
            if (n >= 0) return;
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (detail::is_would_block(e)) {
                if (!wait_writable(write_timeout_)) throw TimeoutException("raw send");
                continue;
            }
            detail::throw_net("raw sendto failed", e);
        }
    }
    [[nodiscard]] std::optional<std::pair<usize, IpAddress>> receive_for(std::span<std::byte> buf, Timeout t) {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + t;
        for (;;) {
            sockaddr_storage ss{};
            detail::sock_len len = sizeof ss;
            const auto n = ::recvfrom(s_, reinterpret_cast<char*>(buf.data()), static_cast<detail::io_len_t>(buf.size()), 0, reinterpret_cast<sockaddr*>(&ss), &len);
            if (n >= 0) return std::pair<usize, IpAddress>(static_cast<usize>(n), Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss)).address());
            const int e = detail::last_error();
            if (e == detail::err_interrupted) continue;
            if (!detail::is_would_block(e)) detail::throw_net("raw recvfrom failed", e);
            auto left = std::chrono::duration_cast<Timeout>(deadline - clock::now());
            if (left.count() < 0) left = Timeout{0};
            if (!wait_readable(left)) return std::nullopt;
        }
    }

protected:
    RawSocket(int family, int type, int protocol) : Socket(family, type, protocol) {}
};

// ---- readiness polling ---------------------------------------------------------------------------

struct PollItem {
    detail::native_socket socket = detail::invalid_socket;
    unsigned events = 0;   ///< requested PollEvent mask
    unsigned revents = 0;  ///< filled by poll()
};

/// Waits for readiness on many sockets. Returns the number of items with non-zero revents.
inline int poll(std::vector<PollItem>& items, Timeout timeout = no_timeout) {
    std::vector<detail::pollfd_t> fds(items.size());
    for (usize i = 0; i < items.size(); ++i) {
        fds[i] = {};
        fds[i].fd = items[i].socket;
        fds[i].events = static_cast<short>(((items[i].events & poll_read) ? POLLIN : 0) | ((items[i].events & poll_write) ? POLLOUT : 0));
    }
    const int r = detail::poll_sockets(fds.data(), fds.size(), timeout);
    int ready = 0;
    for (usize i = 0; i < items.size(); ++i) {
        const auto re = fds[i].revents;
        unsigned m = 0;
        if (re & POLLIN) m |= poll_read;
        if (re & POLLOUT) m |= poll_write;
        if (re & POLLERR) m |= poll_error;
        if (re & POLLHUP) m |= poll_hangup;
        if (re & POLLNVAL) m |= poll_error;
        items[i].revents = m;
        if (m) ++ready;
    }
    (void)r;
    return ready;
}

// ---- network interfaces --------------------------------------------------------------------------

struct InterfaceAddress {
    IpAddress address;
    unsigned prefix_length = 0;
};

struct Interface {
    std::string name;
    u32 index = 0;
    std::optional<std::array<u8, 6>> mac;
    std::vector<InterfaceAddress> addresses;
    bool up = false;
    bool loopback = false;
    bool multicast = false;
    bool broadcast = false;

    [[nodiscard]] std::string mac_string() const {
        if (!mac) return {};
        static constexpr char hex[] = "0123456789abcdef";
        std::string s;
        for (usize i = 0; i < 6; ++i) {
            if (i) s += ':';
            s += hex[(*mac)[i] >> 4];
            s += hex[(*mac)[i] & 15];
        }
        return s;
    }
};

namespace detail {
[[nodiscard]] inline unsigned prefix_of(const u8* mask, usize n) noexcept {
    unsigned bits = 0;
    for (usize i = 0; i < n; ++i) {
        for (int b = 7; b >= 0; --b) {
            if (mask[i] & (1u << b)) ++bits;
            else return bits;
        }
    }
    return bits;
}
}  // namespace detail

/// Lists the host's network interfaces with their addresses.
[[nodiscard]] inline std::vector<Interface> interfaces() {
    std::vector<Interface> out;
#if FLYG_PLATFORM_WINDOWS
    detail::ensure_init();
    ULONG size = 16384;
    std::vector<unsigned char> buf;
    ULONG rc;
    for (int tries = 0; tries < 4; ++tries) {
        buf.resize(size);
        rc = ::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (rc != NO_ERROR) throw NetworkException("GetAdaptersAddresses failed", static_cast<int>(rc));
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        Interface i;
        i.name = a->AdapterName;
        i.index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
        i.up = a->OperStatus == IfOperStatusUp;
        i.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        i.multicast = !(a->Flags & IP_ADAPTER_NO_MULTICAST);
        i.broadcast = a->IfType != IF_TYPE_SOFTWARE_LOOPBACK;
        if (a->PhysicalAddressLength == 6) {
            std::array<u8, 6> m{};
            std::memcpy(m.data(), a->PhysicalAddress, 6);
            i.mac = m;
        }
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            const auto fam = u->Address.lpSockaddr->sa_family;
            if (fam != AF_INET && fam != AF_INET6) continue;
            i.addresses.push_back({Endpoint::from_sockaddr(u->Address.lpSockaddr).address(), u->OnLinkPrefixLength});
        }
        out.push_back(std::move(i));
    }
#else
    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0) detail::throw_net("getifaddrs failed");
    struct Guard {
        ifaddrs* p;
        ~Guard() { ::freeifaddrs(p); }
    } guard{head};
    auto find_or_add = [&](const char* name) -> Interface& {
        for (auto& i : out)
            if (i.name == name) return i;
        Interface i;
        i.name = name;
        i.index = ::if_nametoindex(name);
        out.push_back(std::move(i));
        return out.back();
    };
    for (const ifaddrs* p = head; p; p = p->ifa_next) {
        if (!p->ifa_name) continue;
        Interface& i = find_or_add(p->ifa_name);
        i.up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
        i.loopback = p->ifa_flags & IFF_LOOPBACK;
        i.multicast = p->ifa_flags & IFF_MULTICAST;
        i.broadcast = p->ifa_flags & IFF_BROADCAST;
        if (!p->ifa_addr) continue;
        if (p->ifa_addr->sa_family == AF_INET || p->ifa_addr->sa_family == AF_INET6) {
            InterfaceAddress ia;
            ia.address = Endpoint::from_sockaddr(p->ifa_addr).address();
            if (p->ifa_netmask) {
                if (p->ifa_netmask->sa_family == AF_INET) {
                    ia.prefix_length = detail::prefix_of(reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr), 4);
                } else if (p->ifa_netmask->sa_family == AF_INET6) {
                    ia.prefix_length = detail::prefix_of(reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in6*>(p->ifa_netmask)->sin6_addr), 16);
                }
            }
            i.addresses.push_back(ia);
        }
    }
    // hardware addresses via a SIOCGIFHWADDR query on a throw-away socket
    const int tmp = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (tmp >= 0) {
        for (auto& i : out) {
            ifreq req{};
            std::strncpy(req.ifr_name, i.name.c_str(), IFNAMSIZ - 1);
            if (::ioctl(tmp, SIOCGIFHWADDR, &req) == 0) {
                std::array<u8, 6> m{};
                std::memcpy(m.data(), req.ifr_hwaddr.sa_data, 6);
                bool zero = true;
                for (u8 b : m) zero = zero && b == 0;
                if (!zero || !i.loopback) i.mac = m;
                if (zero && i.loopback) i.mac = m;
            }
        }
        ::close(tmp);
    }
#endif
    return out;
}

}  // namespace flyg::net
