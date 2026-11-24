// flyG - net module: system name resolution (getaddrinfo)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <vector>

#include <flyg/net/address.hpp>

namespace flyg::net {

/// Resolves a host name or literal address through the operating system's resolver. Literal
/// addresses are returned without any lookup. Results keep the resolver's order (so the OS
/// address-selection policy, e.g. RFC 6724, is honoured) with duplicates removed.
/// Throws NetworkException if the name cannot be resolved.
[[nodiscard]] inline std::vector<IpAddress> resolve(const std::string& host, Family family = Family::any) {
    if (const auto lit = IpAddress::parse(host)) {
        if (family == Family::any || lit->family() == family) return {*lit};
        if (family == Family::v6 && lit->is_v4()) return {lit->mapped()};
        throw NetworkException("address '" + host + "' does not match the requested family");
    }
    detail::ensure_init();
    addrinfo hints{};
    hints.ai_family = family == Family::v4 ? AF_INET : family == Family::v6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0) {
#if FLYG_PLATFORM_WINDOWS
        detail::throw_net("cannot resolve '" + host + "'", rc);
#else
        throw NetworkException("cannot resolve '" + host + "': " + ::gai_strerror(rc), rc);
#endif
    }
    std::vector<IpAddress> out;
    for (const addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
        const IpAddress a = Endpoint::from_sockaddr(p->ai_addr).address();
        if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
    ::freeaddrinfo(res);
    if (out.empty()) throw NetworkException("no addresses found for '" + host + "'");
    return out;
}

/// The local host name.
[[nodiscard]] inline std::string host_name() {
    detail::ensure_init();
    char buf[256] = {};
    if (::gethostname(buf, sizeof buf - 1) != 0) detail::throw_net("gethostname failed");
    return buf;
}

}  // namespace flyg::net
