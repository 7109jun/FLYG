// flyG - net module: URL parsing, resolution and query strings (RFC 3986)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/net/native.hpp>

namespace flyg::net {

using core::u16;
using core::usize;

struct Url {
    std::string scheme;     ///< lower-case, e.g. "http"
    std::string userinfo;   ///< raw "user:pass" or empty
    std::string host;       ///< lower-case; IPv6 literals without brackets
    std::optional<u16> port;
    std::string path;       ///< raw (still percent-encoded); empty for "http://host"
    std::optional<std::string> query;     ///< without '?'
    std::optional<std::string> fragment;  ///< without '#'

    [[nodiscard]] static std::optional<u16> default_port(std::string_view scheme) noexcept {
        if (scheme == "http" || scheme == "ws") return 80;
        if (scheme == "https" || scheme == "wss") return 443;
        if (scheme == "ftp") return 21;
        return std::nullopt;
    }
    [[nodiscard]] u16 effective_port() const noexcept { return port ? *port : default_port(scheme).value_or(0); }
    [[nodiscard]] bool is_secure() const noexcept { return scheme == "https" || scheme == "wss"; }
    [[nodiscard]] bool has_authority() const noexcept { return !host.empty(); }

    /// "path?query" as sent in an HTTP request line (never empty).
    [[nodiscard]] std::string request_target() const {
        std::string t = path.empty() ? "/" : path;
        if (query) t += "?" + *query;
        return t;
    }
    /// "host" or "host:port" for the Host header (port omitted when it is the scheme default).
    [[nodiscard]] std::string host_header() const {
        std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
        if (port && *port != default_port(scheme).value_or(0)) h += ":" + std::to_string(*port);
        return h;
    }
    [[nodiscard]] std::string origin() const { return scheme + "://" + host_header(); }

    [[nodiscard]] std::string to_string() const {
        std::string s = scheme + ":";
        if (has_authority() || scheme == "file") {
            s += "//";
            if (!userinfo.empty()) s += userinfo + "@";
            s += host.find(':') != std::string::npos ? "[" + host + "]" : host;
            if (port) s += ":" + std::to_string(*port);
        }
        s += path;
        if (query) s += "?" + *query;
        if (fragment) s += "#" + *fragment;
        return s;
    }

    /// Parses an absolute URL. Returns nullopt if it is malformed.
    [[nodiscard]] static std::optional<Url> parse(std::string_view in) {
        for (char c : in)
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return std::nullopt;
        Url u;
        const auto colon = in.find(':');
        if (colon == std::string_view::npos || colon == 0) return std::nullopt;
        for (usize i = 0; i < colon; ++i) {
            const char c = in[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
            if (!ok) return std::nullopt;
            u.scheme += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        std::string_view rest = in.substr(colon + 1);
        if (const auto h = rest.find('#'); h != std::string_view::npos) {
            u.fragment = std::string(rest.substr(h + 1));
            rest = rest.substr(0, h);
        }
        if (const auto q = rest.find('?'); q != std::string_view::npos) {
            u.query = std::string(rest.substr(q + 1));
            rest = rest.substr(0, q);
        }
        if (rest.substr(0, 2) == "//") {
            rest.remove_prefix(2);
            const auto slash = rest.find('/');
            std::string_view auth = rest.substr(0, slash);
            u.path = slash == std::string_view::npos ? "" : std::string(rest.substr(slash));
            if (const auto at = auth.rfind('@'); at != std::string_view::npos) {
                u.userinfo = std::string(auth.substr(0, at));
                auth.remove_prefix(at + 1);
            }
            std::string_view host = auth, port;
            if (!auth.empty() && auth[0] == '[') {
                const auto close = auth.find(']');
                if (close == std::string_view::npos) return std::nullopt;
                host = auth.substr(1, close - 1);
                const auto after = auth.substr(close + 1);
                if (!after.empty()) {
                    if (after[0] != ':') return std::nullopt;
                    port = after.substr(1);
                }
                if (host.find(':') == std::string_view::npos) return std::nullopt;
            } else {
                const auto pc = auth.rfind(':');
                if (pc != std::string_view::npos) {
                    host = auth.substr(0, pc);
                    port = auth.substr(pc + 1);
                    if (host.find(':') != std::string_view::npos) return std::nullopt;
                }
            }
            for (char c : host) u.host += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
            if (!port.empty()) {
                if (port.size() > 5) return std::nullopt;
                unsigned p = 0;
                for (char c : port) {
                    if (c < '0' || c > '9') return std::nullopt;
                    p = p * 10 + static_cast<unsigned>(c - '0');
                }
                if (p > 65535) return std::nullopt;
                u.port = static_cast<u16>(p);
            }
            if (u.host.empty() && u.scheme != "file") return std::nullopt;
            for (char c : u.host)
                if (c == '/' || c == '\\' || c == '?' || c == '#' || c == '@') return std::nullopt;
        } else {
            u.path = std::string(rest);
        }
        return u;
    }
    [[nodiscard]] static Url from_string(std::string_view s) {
        if (auto u = parse(s)) return *u;
        throw NetworkException("invalid URL: '" + std::string(s) + "'");
    }

    /// Resolves a (possibly relative) reference against this URL (RFC 3986 section 5.2).
    [[nodiscard]] Url resolve(std::string_view ref) const {
        if (auto abs = parse(ref)) return *abs;  // has a scheme
        Url t;
        t.scheme = scheme;
        std::string_view r = ref;
        for (char c : r)
            if (static_cast<unsigned char>(c) <= 0x20) throw NetworkException("invalid URL reference");
        if (const auto h = r.find('#'); h != std::string_view::npos) {
            t.fragment = std::string(r.substr(h + 1));
            r = r.substr(0, h);
        }
        std::optional<std::string> rq;
        if (const auto q = r.find('?'); q != std::string_view::npos) {
            rq = std::string(r.substr(q + 1));
            r = r.substr(0, q);
        }
        if (r.substr(0, 2) == "//") {  // network-path reference
            Url n = from_string(scheme + ":" + std::string(r) + (rq ? "?" + *rq : ""));
            n.fragment = t.fragment;
            return n;
        }
        t.userinfo = userinfo;
        t.host = host;
        t.port = port;
        if (r.empty()) {
            t.path = path;
            t.query = rq ? rq : query;
        } else {
            if (r[0] == '/') t.path = remove_dot_segments(std::string(r));
            else t.path = remove_dot_segments(merge(std::string(r)));
            t.query = rq;
        }
        return t;
    }

    [[nodiscard]] static std::string remove_dot_segments(std::string in) {
        std::string out;
        while (!in.empty()) {
            if (in.rfind("../", 0) == 0) in.erase(0, 3);
            else if (in.rfind("./", 0) == 0) in.erase(0, 2);
            else if (in.rfind("/./", 0) == 0) in.erase(0, 2);
            else if (in == "/.") in = "/";
            else if (in.rfind("/../", 0) == 0 || in == "/..") {
                in = in == "/.." ? "/" : in.substr(3);
                if (const auto p = out.rfind('/'); p != std::string::npos) out.erase(p);
                else out.clear();
            } else if (in == "." || in == "..") in.clear();
            else {
                const auto next = in.find('/', in[0] == '/' ? 1 : 0);
                out += in.substr(0, next);
                in.erase(0, next);
            }
        }
        return out;
    }

private:
    [[nodiscard]] std::string merge(const std::string& r) const {
        if (has_authority() && path.empty()) return "/" + r;
        const auto slash = path.rfind('/');
        return (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + r;
    }
};

using QueryParams = std::vector<std::pair<std::string, std::string>>;

/// Parses "a=1&b=two%20words" (application/x-www-form-urlencoded: '+' means space).
[[nodiscard]] inline QueryParams parse_query(std::string_view q) {
    QueryParams out;
    usize i = 0;
    while (i <= q.size()) {
        const auto amp = q.find('&', i);
        const auto part = q.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        if (!part.empty()) {
            const auto eq = part.find('=');
            try {
                out.emplace_back(data::percent_decode(part.substr(0, eq), true), eq == std::string_view::npos ? std::string() : data::percent_decode(part.substr(eq + 1), true));
            } catch (const std::exception&) {
                out.emplace_back(std::string(part.substr(0, eq)), eq == std::string_view::npos ? std::string() : std::string(part.substr(eq + 1)));
            }
        }
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return out;
}
[[nodiscard]] inline std::string build_query(const QueryParams& p) {
    std::string out;
    for (const auto& [k, v] : p) {
        if (!out.empty()) out += '&';
        out += data::percent_encode(k, true) + "=" + data::percent_encode(v, true);
    }
    return out;
}

}  // namespace flyg::net
