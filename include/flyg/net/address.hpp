// flyG - net module: IP addresses and endpoints
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <compare>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <flyg/net/native.hpp>

namespace flyg::net {

using core::u16;
using core::u32;
using core::u8;
using core::usize;

enum class Family : u8 { any, v4, v6 };

/// IPv4 or IPv6 address (value type, totally ordered, hashable via `hash()`).
class IpAddress {
public:
    IpAddress() noexcept { b_.fill(0); }  // 0.0.0.0

    [[nodiscard]] static IpAddress v4(u8 a, u8 b, u8 c, u8 d) noexcept {
        IpAddress r;
        r.b_[0] = a, r.b_[1] = b, r.b_[2] = c, r.b_[3] = d;
        return r;
    }
    [[nodiscard]] static IpAddress v4(u32 host_order) noexcept {
        return v4(static_cast<u8>(host_order >> 24), static_cast<u8>(host_order >> 16), static_cast<u8>(host_order >> 8), static_cast<u8>(host_order));
    }
    [[nodiscard]] static IpAddress v6(const std::array<u8, 16>& bytes, u32 scope_id = 0) noexcept {
        IpAddress r;
        r.v6_ = true;
        r.b_ = bytes;
        r.scope_ = scope_id;
        return r;
    }
    [[nodiscard]] static IpAddress any(Family f = Family::v4) noexcept { return f == Family::v6 ? v6({}) : IpAddress{}; }
    [[nodiscard]] static IpAddress loopback(Family f = Family::v4) noexcept {
        if (f == Family::v6) {
            std::array<u8, 16> b{};
            b[15] = 1;
            return v6(b);
        }
        return v4(127, 0, 0, 1);
    }

    /// Parses dotted-quad IPv4 or RFC 4291/5952 IPv6 text (including "::", embedded IPv4 and "%scope").
    [[nodiscard]] static std::optional<IpAddress> parse(std::string_view s) noexcept {
        if (s.find(':') == std::string_view::npos) return parse_v4(s);
        return parse_v6(s);
    }
    /// Like parse() but throws NetworkException.
    [[nodiscard]] static IpAddress from_string(std::string_view s) {
        if (auto a = parse(s)) return *a;
        throw NetworkException("invalid IP address: '" + std::string(s) + "'");
    }

    [[nodiscard]] bool is_v4() const noexcept { return !v6_; }
    [[nodiscard]] bool is_v6() const noexcept { return v6_; }
    [[nodiscard]] Family family() const noexcept { return v6_ ? Family::v6 : Family::v4; }
    [[nodiscard]] usize size() const noexcept { return v6_ ? 16 : 4; }
    [[nodiscard]] const u8* data() const noexcept { return b_.data(); }
    [[nodiscard]] u32 scope_id() const noexcept { return scope_; }
    /// IPv4 value in host byte order (precondition: is_v4()).
    [[nodiscard]] u32 to_u32() const noexcept {
        return (static_cast<u32>(b_[0]) << 24) | (static_cast<u32>(b_[1]) << 16) | (static_cast<u32>(b_[2]) << 8) | b_[3];
    }

    [[nodiscard]] bool is_unspecified() const noexcept {
        for (usize i = 0; i < size(); ++i)
            if (b_[i]) return false;
        return true;
    }
    [[nodiscard]] bool is_loopback() const noexcept {
        if (!v6_) return b_[0] == 127;
        if (is_v4_mapped()) return b_[12] == 127;
        for (usize i = 0; i < 15; ++i)
            if (b_[i]) return false;
        return b_[15] == 1;
    }
    [[nodiscard]] bool is_v4_mapped() const noexcept {
        if (!v6_) return false;
        for (usize i = 0; i < 10; ++i)
            if (b_[i]) return false;
        return b_[10] == 0xFF && b_[11] == 0xFF;
    }
    [[nodiscard]] bool is_multicast() const noexcept { return v6_ ? b_[0] == 0xFF : (b_[0] & 0xF0) == 0xE0; }
    [[nodiscard]] bool is_link_local() const noexcept {
        return v6_ ? (b_[0] == 0xFE && (b_[1] & 0xC0) == 0x80) : (b_[0] == 169 && b_[1] == 254);
    }
    /// RFC 1918 / unique-local fc00::/7.
    [[nodiscard]] bool is_private() const noexcept {
        if (v6_) return (b_[0] & 0xFE) == 0xFC;
        return b_[0] == 10 || (b_[0] == 172 && (b_[1] & 0xF0) == 16) || (b_[0] == 192 && b_[1] == 168);
    }
    /// For an IPv4-mapped IPv6 address returns the IPv4 address, otherwise *this.
    [[nodiscard]] IpAddress unmapped() const noexcept { return is_v4_mapped() ? v4(b_[12], b_[13], b_[14], b_[15]) : *this; }
    [[nodiscard]] IpAddress mapped() const noexcept {
        if (v6_) return *this;
        std::array<u8, 16> b{};
        b[10] = b[11] = 0xFF;
        for (usize i = 0; i < 4; ++i) b[12 + i] = b_[i];
        return v6(b);
    }

    /// True if the first `prefix_len` bits of both addresses agree (same family required).
    [[nodiscard]] bool in_subnet(const IpAddress& net, unsigned prefix_len) const noexcept {
        if (v6_ != net.v6_ || prefix_len > size() * 8) return false;
        const usize full = prefix_len / 8;
        if (std::memcmp(b_.data(), net.b_.data(), full) != 0) return false;
        const unsigned rem = prefix_len % 8;
        if (rem == 0) return true;
        const u8 mask = static_cast<u8>(0xFF << (8 - rem));
        return (b_[full] & mask) == (net.b_[full] & mask);
    }

    /// Canonical text: dotted quad, or RFC 5952 compressed IPv6 (with "%scope" if set).
    [[nodiscard]] std::string to_string() const {
        if (!v6_) return std::to_string(b_[0]) + "." + std::to_string(b_[1]) + "." + std::to_string(b_[2]) + "." + std::to_string(b_[3]);
        u16 g[8];
        for (usize i = 0; i < 8; ++i) g[i] = static_cast<u16>((b_[2 * i] << 8) | b_[2 * i + 1]);
        int best_start = -1, best_len = 0;
        for (int i = 0; i < 8;) {
            if (g[i] != 0) { ++i; continue; }
            int j = i;
            while (j < 8 && g[j] == 0) ++j;
            if (j - i > best_len) best_len = j - i, best_start = i;
            i = j;
        }
        if (best_len < 2) best_start = -1;
        std::string out;
        static constexpr char hex[] = "0123456789abcdef";
        auto put = [&](u16 v) {
            bool started = false;
            for (int sh = 12; sh >= 0; sh -= 4) {
                const unsigned d = (v >> sh) & 15u;
                if (d || started || sh == 0) out += hex[d], started = true;
            }
        };
        for (int i = 0; i < 8; ++i) {
            if (i == best_start) {
                out += "::";
                i += best_len - 1;
                continue;
            }
            if (!out.empty() && out.back() != ':') out += ':';
            put(g[i]);
        }
        if (scope_) out += "%" + std::to_string(scope_);
        return out;
    }

    /// Reverse-DNS name ("4.3.2.1.in-addr.arpa" / nibble form under ip6.arpa).
    [[nodiscard]] std::string reverse_name() const {
        std::string out;
        if (!v6_) {
            for (int i = 3; i >= 0; --i) out += std::to_string(b_[static_cast<usize>(i)]) + ".";
            return out + "in-addr.arpa";
        }
        static constexpr char hex[] = "0123456789abcdef";
        for (int i = 15; i >= 0; --i) {
            out += hex[b_[static_cast<usize>(i)] & 15];
            out += '.';
            out += hex[b_[static_cast<usize>(i)] >> 4];
            out += '.';
        }
        return out + "ip6.arpa";
    }

    friend bool operator==(const IpAddress& a, const IpAddress& b) noexcept { return a.v6_ == b.v6_ && a.b_ == b.b_ && a.scope_ == b.scope_; }
    friend std::strong_ordering operator<=>(const IpAddress& a, const IpAddress& b) noexcept {
        if (a.v6_ != b.v6_) return a.v6_ <=> b.v6_;
        if (const int c = std::memcmp(a.b_.data(), b.b_.data(), a.size()); c != 0) return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        return a.scope_ <=> b.scope_;
    }
    [[nodiscard]] usize hash() const noexcept {
        usize h = v6_ ? 0x9E3779B97F4A7C15ull : 0;
        for (usize i = 0; i < size(); ++i) h = (h ^ b_[i]) * 1099511628211ull;
        return h ^ scope_;
    }

private:
    [[nodiscard]] static std::optional<IpAddress> parse_v4(std::string_view s) noexcept {
        IpAddress r;
        usize pos = 0;
        for (int part = 0; part < 4; ++part) {
            if (pos >= s.size()) return std::nullopt;
            unsigned v = 0;
            usize digits = 0;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                v = v * 10 + static_cast<unsigned>(s[pos] - '0');
                if (++digits > 3 || v > 255) return std::nullopt;
                ++pos;
            }
            if (digits == 0 || (digits > 1 && s[pos - digits] == '0')) return std::nullopt;  // no leading zeros
            r.b_[static_cast<usize>(part)] = static_cast<u8>(v);
            if (part < 3) {
                if (pos >= s.size() || s[pos] != '.') return std::nullopt;
                ++pos;
            }
        }
        if (pos != s.size()) return std::nullopt;
        return r;
    }
    [[nodiscard]] static std::optional<IpAddress> parse_v6(std::string_view s) noexcept {
        u32 scope = 0;
        if (const auto pct = s.find('%'); pct != std::string_view::npos) {
            const auto z = s.substr(pct + 1);
            s = s.substr(0, pct);
            if (z.empty()) return std::nullopt;
            for (char c : z) {
                if (c < '0' || c > '9') return std::nullopt;  // numeric scope ids only (interface names need the OS)
                scope = scope * 10 + static_cast<u32>(c - '0');
            }
        }
        u16 head[8], tail[8];
        int nh = 0, nt = 0;
        bool gap = false;
        usize pos = 0;
        if (s.size() >= 2 && s[0] == ':' && s[1] == ':') {
            gap = true;
            pos = 2;
        } else if (!s.empty() && s[0] == ':') {
            return std::nullopt;
        }
        while (pos < s.size()) {
            auto& arr = gap ? tail : head;
            int& n = gap ? nt : nh;
            if (nh + nt >= 8) return std::nullopt;
            // embedded IPv4 as the final 32 bits
            const auto rest = s.substr(pos);
            if (rest.find('.') != std::string_view::npos && rest.find(':') == std::string_view::npos) {
                const auto v4a = parse_v4(rest);
                if (!v4a || nh + nt > 6) return std::nullopt;
                arr[n++] = static_cast<u16>((v4a->b_[0] << 8) | v4a->b_[1]);
                arr[n++] = static_cast<u16>((v4a->b_[2] << 8) | v4a->b_[3]);
                pos = s.size();
                break;
            }
            unsigned v = 0;
            usize digits = 0;
            while (pos < s.size()) {
                const char c = s[pos];
                unsigned d;
                if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
                else break;
                v = v * 16 + d;
                if (++digits > 4) return std::nullopt;
                ++pos;
            }
            if (digits == 0) return std::nullopt;
            arr[n++] = static_cast<u16>(v);
            if (pos == s.size()) break;
            if (s[pos] != ':') return std::nullopt;
            ++pos;
            if (pos < s.size() && s[pos] == ':') {
                if (gap) return std::nullopt;
                gap = true;
                ++pos;
            } else if (pos == s.size()) {
                return std::nullopt;  // trailing single colon
            }
        }
        const int total = nh + nt;
        if (gap ? total > 7 : total != 8) return std::nullopt;
        u16 all[8] = {};
        for (int i = 0; i < nh; ++i) all[i] = head[i];
        for (int i = 0; i < nt; ++i) all[8 - nt + i] = tail[i];
        std::array<u8, 16> b{};
        for (int i = 0; i < 8; ++i) b[static_cast<usize>(2 * i)] = static_cast<u8>(all[i] >> 8), b[static_cast<usize>(2 * i + 1)] = static_cast<u8>(all[i]);
        return v6(b, scope);
    }

    std::array<u8, 16> b_{};
    bool v6_ = false;
    u32 scope_ = 0;
};

/// IP address + port.
class Endpoint {
public:
    Endpoint() = default;
    Endpoint(IpAddress a, u16 port) noexcept : addr_(a), port_(port) {}

    [[nodiscard]] const IpAddress& address() const noexcept { return addr_; }
    [[nodiscard]] u16 port() const noexcept { return port_; }
    void set_port(u16 p) noexcept { port_ = p; }

    /// "1.2.3.4:80", "[::1]:80". A missing port is an error.
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view s) noexcept {
        std::string_view host, port;
        if (!s.empty() && s[0] == '[') {
            const auto close = s.find(']');
            if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != ':') return std::nullopt;
            host = s.substr(1, close - 1);
            port = s.substr(close + 2);
        } else {
            const auto colon = s.rfind(':');
            if (colon == std::string_view::npos || s.find(':') != colon) return std::nullopt;
            host = s.substr(0, colon);
            port = s.substr(colon + 1);
        }
        const auto a = IpAddress::parse(host);
        if (!a || port.empty() || port.size() > 5) return std::nullopt;
        u32 p = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return std::nullopt;
            p = p * 10 + static_cast<u32>(c - '0');
        }
        if (p > 65535) return std::nullopt;
        return Endpoint(*a, static_cast<u16>(p));
    }
    [[nodiscard]] static Endpoint from_string(std::string_view s) {
        if (auto e = parse(s)) return *e;
        throw NetworkException("invalid endpoint: '" + std::string(s) + "'");
    }
    [[nodiscard]] std::string to_string() const {
        return addr_.is_v6() ? "[" + addr_.to_string() + "]:" + std::to_string(port_) : addr_.to_string() + ":" + std::to_string(port_);
    }

    // ---- sockaddr conversion ----------------------------------------------------------------------
    [[nodiscard]] static Endpoint from_sockaddr(const sockaddr* sa) {
        if (sa->sa_family == AF_INET) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
            u8 b[4];
            std::memcpy(b, &in->sin_addr, 4);
            return Endpoint(IpAddress::v4(b[0], b[1], b[2], b[3]), ntohs(in->sin_port));
        }
        if (sa->sa_family == AF_INET6) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
            std::array<u8, 16> b{};
            std::memcpy(b.data(), &in6->sin6_addr, 16);
            return Endpoint(IpAddress::v6(b, in6->sin6_scope_id), ntohs(in6->sin6_port));
        }
        throw NetworkException("unsupported address family");
    }
    /// Fills `ss` and returns its used length.
    detail::sock_len to_sockaddr(sockaddr_storage& ss) const noexcept {
        std::memset(&ss, 0, sizeof ss);
        if (addr_.is_v4()) {
            auto* in = reinterpret_cast<sockaddr_in*>(&ss);
            in->sin_family = AF_INET;
            in->sin_port = htons(port_);
            std::memcpy(&in->sin_addr, addr_.data(), 4);
            return sizeof(sockaddr_in);
        }
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&ss);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port_);
        std::memcpy(&in6->sin6_addr, addr_.data(), 16);
        in6->sin6_scope_id = addr_.scope_id();
        return sizeof(sockaddr_in6);
    }

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
    friend std::strong_ordering operator<=>(const Endpoint& a, const Endpoint& b) noexcept {
        if (auto c = a.addr_ <=> b.addr_; c != 0) return c;
        return a.port_ <=> b.port_;
    }

private:
    IpAddress addr_;
    u16 port_ = 0;
};

}  // namespace flyg::net

template <>
struct std::hash<flyg::net::IpAddress> {
    std::size_t operator()(const flyg::net::IpAddress& a) const noexcept { return a.hash(); }
};
template <>
struct std::hash<flyg::net::Endpoint> {
    std::size_t operator()(const flyg::net::Endpoint& e) const noexcept { return e.address().hash() ^ (static_cast<std::size_t>(e.port()) << 1); }
};
