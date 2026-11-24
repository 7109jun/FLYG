// flyG - net module: ICMP echo ("ping") over raw / ICMP datagram sockets
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <random>

#include <flyg/net/socket.hpp>

namespace flyg::net::icmp {

/// Internet checksum (RFC 1071).
[[nodiscard]] inline u16 checksum(std::span<const u8> data) noexcept {
    core::u32 sum = 0;
    usize i = 0;
    for (; i + 1 < data.size(); i += 2) sum += static_cast<core::u32>((data[i] << 8) | data[i + 1]);
    if (i < data.size()) sum += static_cast<core::u32>(data[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

struct PingReply {
    double rtt_ms = 0;
    IpAddress from;
    int ttl = -1;  ///< only known when the OS delivers the IP header (raw IPv4 sockets)
};

/// Builds an echo request packet (type 8 for IPv4, 128 for IPv6).
[[nodiscard]] inline std::vector<u8> make_echo_request(bool v6, u16 id, u16 seq, usize payload_size) {
    std::vector<u8> p(8 + payload_size);
    p[0] = v6 ? 128 : 8;
    p[4] = static_cast<u8>(id >> 8), p[5] = static_cast<u8>(id);
    p[6] = static_cast<u8>(seq >> 8), p[7] = static_cast<u8>(seq);
    for (usize i = 0; i < payload_size; ++i) p[8 + i] = static_cast<u8>('a' + i % 23);
    if (!v6) {  // ICMPv6 checksums need the pseudo header; the OS fills that in for raw ICMPv6 sockets
        const u16 c = checksum(p);
        p[2] = static_cast<u8>(c >> 8), p[3] = static_cast<u8>(c);
    }
    return p;
}

/// Sends one echo request and waits for the matching reply. Returns nullopt on timeout. Throws
/// NetworkException if the process may not use ICMP sockets (needs root/CAP_NET_RAW or, on
/// Linux, a permissive net.ipv4.ping_group_range).
[[nodiscard]] inline std::optional<PingReply> ping(const IpAddress& target, Timeout timeout = Timeout{1000}, usize payload_size = 32, u16 seq = 1) {
    const bool v6 = target.is_v6();
    const Family fam = target.family();
    RawSocket sock;
    bool datagram = false;
#if FLYG_PLATFORM_LINUX
    try {
        sock = RawSocket::open_icmp_datagram(fam);
        datagram = true;
    } catch (const NetworkException&) {
        sock = RawSocket::open(fam, v6 ? static_cast<int>(IPPROTO_ICMPV6) : static_cast<int>(IPPROTO_ICMP));
    }
#else
    sock = RawSocket::open(fam, v6 ? 58 : 1);
#endif
    static std::mt19937 gen{std::random_device{}()};
    const u16 id = static_cast<u16>(gen());
    const auto pkt = make_echo_request(v6, id, seq, payload_size);
    const auto start = std::chrono::steady_clock::now();
    sock.send_to(std::as_bytes(std::span(pkt)), target);
    std::vector<std::byte> buf(2048);
    for (;;) {
        const auto left = std::chrono::duration_cast<Timeout>(timeout - (std::chrono::steady_clock::now() - start));
        if (left.count() <= 0) return std::nullopt;
        const auto r = sock.receive_for(buf, left);
        if (!r) return std::nullopt;
        const u8* d = reinterpret_cast<const u8*>(buf.data());
        usize n = r->first;
        int ttl = -1;
        if (!v6 && !datagram) {  // raw IPv4: skip the IP header
            if (n < 20) continue;
            const usize ihl = static_cast<usize>(d[0] & 15) * 4;
            if (ihl < 20 || n < ihl) continue;
            ttl = d[8];
            d += ihl;
            n -= ihl;
        }
        if (n < 8) continue;
        if (d[0] != (v6 ? 129 : 0) || d[1] != 0) continue;              // not an echo reply
        if (!datagram && static_cast<u16>((d[4] << 8) | d[5]) != id) continue;  // somebody else's ping
        if (static_cast<u16>((d[6] << 8) | d[7]) != seq) continue;
        return PingReply{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), r->second, ttl};
    }
}

}  // namespace flyg::net::icmp
