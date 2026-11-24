// flyG - net module: HTTP/1.1 messages, client and server
// SPDX-License-Identifier: GPL-2.0-only
//
// Implements RFC 9110/9112 essentials: request/response parsing with strict validation
// (request-smuggling safe framing), Content-Length and chunked bodies, keep-alive, redirects,
// timeouts, Expect: 100-continue, HEAD handling, streaming responses and protocol upgrades.
// Not implemented: HTTP/2, content-encoding (gzip is never requested), proxies, cookies.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include <flyg/data/compress.hpp>
#include <flyg/net/stream.hpp>
#include <flyg/net/url.hpp>

namespace flyg::net::http {

// ---- headers ----------------------------------------------------------------------------------------

[[nodiscard]] inline std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return r;
}
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

class Headers {
public:
    using Entry = std::pair<std::string, std::string>;

    /// Appends a header (repeated names are kept separately). Validates name and value.
    void add(std::string name, std::string value) {
        validate(name, value);
        items_.emplace_back(std::move(name), std::move(value));
    }
    /// Replaces all headers of that name with a single one.
    void set(std::string name, std::string value) {
        remove(name);
        add(std::move(name), std::move(value));
    }
    bool remove(std::string_view name) {
        const auto n = std::erase_if(items_, [&](const Entry& e) { return iequals(e.first, name); });
        return n != 0;
    }
    [[nodiscard]] const std::string* find(std::string_view name) const noexcept {
        for (const auto& e : items_)
            if (iequals(e.first, name)) return &e.second;
        return nullptr;
    }
    [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name) != nullptr; }
    [[nodiscard]] std::string get(std::string_view name, std::string_view def = {}) const { return std::string(find(name) ? *find(name) : def); }
    [[nodiscard]] std::vector<std::string> get_all(std::string_view name) const {
        std::vector<std::string> v;
        for (const auto& e : items_)
            if (iequals(e.first, name)) v.push_back(e.second);
        return v;
    }
    /// All values of a list-valued header joined by ", " (RFC 9110 5.3).
    [[nodiscard]] std::string combined(std::string_view name) const {
        std::string r;
        for (const auto& e : items_)
            if (iequals(e.first, name)) r += (r.empty() ? "" : ", ") + e.second;
        return r;
    }
    /// True if a comma-separated header contains `token` (case-insensitive), e.g. "Connection: keep-alive, Upgrade".
    [[nodiscard]] bool has_token(std::string_view name, std::string_view token) const {
        for (const auto& e : items_) {
            if (!iequals(e.first, name)) continue;
            usize i = 0;
            const std::string_view v = e.second;
            while (i <= v.size()) {
                auto j = v.find(',', i);
                std::string_view part = v.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
                while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
                if (iequals(part, token)) return true;
                if (j == std::string_view::npos) break;
                i = j + 1;
            }
        }
        return false;
    }
    [[nodiscard]] auto begin() const noexcept { return items_.begin(); }
    [[nodiscard]] auto end() const noexcept { return items_.end(); }
    [[nodiscard]] usize size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    static void validate(std::string_view name, std::string_view value) {
        if (name.empty()) throw NetworkException("empty HTTP header name");
        for (char c : name) {
            const auto u = static_cast<unsigned char>(c);
            const bool tchar = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                               std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
            if (!tchar) throw NetworkException("invalid character in HTTP header name '" + std::string(name) + "'");
        }
        for (char c : value) {
            const auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && u != '\t') || u == 0x7F) throw NetworkException("invalid character in value of HTTP header '" + std::string(name) + "' (possible header injection)");
        }
    }

private:
    std::vector<Entry> items_;
};

[[nodiscard]] inline std::string_view reason_phrase(int status) noexcept {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "";
    }
}

struct Request {
    std::string method = "GET";
    std::string target = "/";  ///< request-target as received ("/path?query")
    std::string version = "HTTP/1.1";
    Headers headers;
    std::string body;

    /// Path part of the target (percent-decoded) and parsed query.
    [[nodiscard]] std::string path() const {
        const auto q = target.find('?');
        try {
            return data::percent_decode(std::string_view(target).substr(0, q));
        } catch (const std::exception&) {
            return target.substr(0, q);
        }
    }
    [[nodiscard]] QueryParams query() const {
        const auto q = target.find('?');
        return q == std::string::npos ? QueryParams{} : parse_query(std::string_view(target).substr(q + 1));
    }
    [[nodiscard]] std::string query_param(std::string_view name, std::string def = {}) const {
        for (auto& [k, v] : query())
            if (k == name) return v;
        return def;
    }
    Endpoint remote;  ///< filled in by the server
};

struct Response {
    int status = 200;
    std::string reason;  ///< empty: derived from the status code
    std::string version = "HTTP/1.1";
    Headers headers;
    std::string body;

    /// Streaming body (server side): called repeatedly until it returns false; each call appends
    /// the next chunk to `out`. The response then uses chunked transfer encoding.
    std::function<bool(std::string& out)> producer;
    /// Protocol upgrade (server side): after the 101 response is written, the connection is handed
    /// over to this function (it owns the stream until it returns).
    std::function<void(ByteStream&, BufferedReader&)> upgrade;

    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    void set_body(std::string b, std::string content_type = "text/plain; charset=utf-8") {
        body = std::move(b);
        headers.set("Content-Type", std::move(content_type));
    }
    [[nodiscard]] static Response text(int status, std::string body) {
        Response r;
        r.status = status;
        r.set_body(std::move(body));
        return r;
    }
};

// ---- parsing / serialization ------------------------------------------------------------------------------

struct Limits {
    usize max_start_line = 16 * 1024;
    usize max_header_bytes = 64 * 1024;
    usize max_header_count = 200;
    usize max_body = 64ull * 1024 * 1024;
};

namespace detail {

[[nodiscard]] inline bool is_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        const bool t = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        if (!t) return false;
    }
    return true;
}

/// Thrown for protocol violations; `status` is the response code a server should use.
class ProtocolError : public NetworkException {
public:
    ProtocolError(int status, const std::string& what) : NetworkException(what), status_(status) {}
    [[nodiscard]] int status() const noexcept { return status_; }

private:
    int status_;
};

inline void read_headers(BufferedReader& in, Headers& h, const Limits& lim) {
    usize total = 0;
    std::string line;
    for (;;) {
        try {
            if (!in.read_line(line, lim.max_header_bytes)) throw ProtocolError(400, "connection closed in header block");
        } catch (const LineTooLong&) {
            throw ProtocolError(431, "header line too long");
        }
        if (line.empty()) return;
        total += line.size() + 2;
        if (total > lim.max_header_bytes) throw ProtocolError(431, "header block too large");
        if (h.size() >= lim.max_header_count) throw ProtocolError(431, "too many headers");
        if (line[0] == ' ' || line[0] == '\t') throw ProtocolError(400, "obsolete header line folding is not accepted");
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw ProtocolError(400, "malformed header line");
        std::string name = line.substr(0, colon);
        if (!is_token(name)) throw ProtocolError(400, "invalid header name");  // also rejects "Name : value"
        std::string_view v = std::string_view(line).substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        for (char c : v) {
            const auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && u != '\t') || u == 0x7F) throw ProtocolError(400, "invalid character in header value");
        }
        h.add(std::move(name), std::string(v));
    }
}

[[nodiscard]] inline bool parse_uint(std::string_view s, usize& out, int base = 10) noexcept {
    if (s.empty() || s.size() > 18) return false;
    usize v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * static_cast<usize>(base) + static_cast<usize>(d);
    }
    out = v;
    return true;
}

enum class Framing { none, length, chunked, until_close };

struct BodyPlan {
    Framing kind = Framing::none;
    usize length = 0;
};

/// Decides how the message body is delimited (RFC 9112 section 6.3), rejecting ambiguous framing.
[[nodiscard]] inline BodyPlan plan_body(const Headers& h, bool is_request, int status, bool head_response) {
    if (!is_request && (head_response || status / 100 == 1 || status == 204 || status == 304)) return {};
    const bool has_te = h.contains("Transfer-Encoding");
    const auto cls = h.get_all("Content-Length");
    if (has_te) {
        if (!cls.empty()) throw ProtocolError(400, "both Transfer-Encoding and Content-Length present");
        const std::string te = lower(h.combined("Transfer-Encoding"));
        if (te != "chunked") throw ProtocolError(is_request ? 501 : 502, "unsupported Transfer-Encoding '" + te + "'");
        return {Framing::chunked, 0};
    }
    if (!cls.empty()) {
        usize len = 0;
        bool first = true;
        for (const auto& c : cls) {
            // a single header may be "5, 5" in the wild; accept only if all numbers agree
            usize start = 0;
            while (start <= c.size()) {
                const auto comma = c.find(',', start);
                std::string_view part = std::string_view(c).substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
                usize v;
                if (!parse_uint(part, v)) throw ProtocolError(400, "invalid Content-Length");
                if (!first && v != len) throw ProtocolError(400, "conflicting Content-Length values");
                len = v;
                first = false;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        return {Framing::length, len};
    }
    if (is_request) return {};
    return {Framing::until_close, 0};
}

inline void read_body(BufferedReader& in, const BodyPlan& plan, std::string& body, const Limits& lim, Headers* trailers,
                      const std::function<void(std::span<const std::byte>)>& sink = {}) {
    auto take = [&](usize n) {
        std::byte buf[8192];
        usize left = n;
        while (left) {
            const usize k = std::min(left, sizeof buf);
            in.read_exact(std::span<std::byte>(buf, k));
            if (sink) sink(std::span<const std::byte>(buf, k));
            else body.append(reinterpret_cast<const char*>(buf), k);
            left -= k;
        }
    };
    switch (plan.kind) {
        case Framing::none: return;
        case Framing::length:
            if (plan.length > lim.max_body) throw ProtocolError(413, "body larger than the configured limit");
            take(plan.length);
            return;
        case Framing::chunked: {
            usize total = 0;
            std::string line;
            for (;;) {
                if (!in.read_line(line, 1024)) throw ProtocolError(400, "connection closed inside chunked body");
                const auto semi = line.find(';');  // chunk extensions are ignored
                std::string_view sz = std::string_view(line).substr(0, semi);
                while (!sz.empty() && (sz.back() == ' ' || sz.back() == '\t')) sz.remove_suffix(1);
                usize n;
                if (!parse_uint(sz, n, 16)) throw ProtocolError(400, "invalid chunk size");
                if (n == 0) break;
                total += n;
                if (total > lim.max_body || total < n) throw ProtocolError(413, "body larger than the configured limit");
                take(n);
                std::byte crlf[2];
                in.read_exact(crlf);
                if (crlf[0] != std::byte{'\r'} || crlf[1] != std::byte{'\n'}) throw ProtocolError(400, "missing CRLF after chunk");
            }
            Headers t;
            read_headers(in, t, lim);
            if (trailers) *trailers = std::move(t);
            return;
        }
        case Framing::until_close: {
            std::byte buf[8192];
            usize total = 0;
            for (;;) {
                const usize n = in.read_some(buf);
                if (n == 0) return;
                total += n;
                if (total > lim.max_body) throw ProtocolError(413, "body larger than the configured limit");
                if (sink) sink(std::span<const std::byte>(buf, n));
                else body.append(reinterpret_cast<const char*>(buf), n);
            }
        }
    }
}

[[nodiscard]] inline std::string serialize_head(const Request& r) {
    if (!is_token(r.method)) throw NetworkException("invalid HTTP method");
    for (char c : r.target)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) throw NetworkException("invalid character in request target");
    std::string s = r.method + " " + r.target + " " + r.version + "\r\n";
    for (const auto& [k, v] : r.headers) s += k + ": " + v + "\r\n";
    return s + "\r\n";
}
[[nodiscard]] inline std::string serialize_head(const Response& r, std::string_view reason_override = {}) {
    std::string reason = !reason_override.empty() ? std::string(reason_override) : (r.reason.empty() ? std::string(reason_phrase(r.status)) : r.reason);
    for (char c : reason)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) throw NetworkException("invalid character in reason phrase");
    std::string s = r.version + " " + std::to_string(r.status) + " " + reason + "\r\n";
    for (const auto& [k, v] : r.headers) s += k + ": " + v + "\r\n";
    return s + "\r\n";
}

inline void write_chunk(ByteStream& s, std::string_view data) {
    if (data.empty()) return;
    char hex[24];
    std::snprintf(hex, sizeof hex, "%zx\r\n", data.size());
    std::string out = hex;
    out.append(data);
    out += "\r\n";
    s.write_all(out);
}

}  // namespace detail

/// Reads a request head and body. Returns false on clean EOF before the first byte (idle
/// keep-alive connection closed by the peer). Throws detail::ProtocolError for bad input.
inline bool read_request(BufferedReader& in, Request& req, const Limits& lim = {}, bool read_body_now = true) {
    std::string line;
    try {
        do {
            if (!in.read_line(line, lim.max_start_line)) return false;
        } while (line.empty());
    } catch (const LineTooLong&) {
        throw detail::ProtocolError(414, "request line too long");
    }  // tolerate stray CRLF between pipelined requests (RFC 9112 2.2)
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp1 == sp2) throw detail::ProtocolError(400, "malformed request line");
    req = Request{};
    req.method = line.substr(0, sp1);
    req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = line.substr(sp2 + 1);
    if (!detail::is_token(req.method)) throw detail::ProtocolError(400, "invalid method");
    if (req.target.empty()) throw detail::ProtocolError(400, "empty request target");
    for (char c : req.target)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) throw detail::ProtocolError(400, "invalid character in request target");
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") throw detail::ProtocolError(req.version.rfind("HTTP/", 0) == 0 ? 505 : 400, "unsupported HTTP version");
    detail::read_headers(in, req.headers, lim);
    if (req.version == "HTTP/1.1" && req.headers.get_all("Host").size() != 1) throw detail::ProtocolError(400, "HTTP/1.1 requests need exactly one Host header");
    if (read_body_now) {
        const auto plan = detail::plan_body(req.headers, true, 0, false);
        detail::read_body(in, plan, req.body, lim, nullptr);
    }
    return true;
}

// ---- client ----------------------------------------------------------------------------------------------------

/// Opens a (possibly TLS-secured) connection. The default connector handles plain TCP; set
/// `Client::set_secure_connector` for https/wss.
using Connector = std::function<std::unique_ptr<ByteStream>(const Url& url, Timeout connect_timeout)>;

[[nodiscard]] inline std::unique_ptr<ByteStream> connect_tcp(const Url& u, Timeout t) {
    return std::make_unique<TcpByteStream>(TcpStream::connect(u.host, u.effective_port(), t));
}

struct ClientOptions {
    Timeout connect_timeout{10000};
    Timeout io_timeout{30000};
    int max_redirects = 10;
    bool follow_redirects = true;
    bool keep_alive = true;
    std::string user_agent = "flyG/1.0";
    Limits limits;
    bool decompress = true;                             ///< send Accept-Encoding: gzip, deflate and decode Content-Encoding transparently
    usize max_decoded_body = usize(256) << 20;          ///< decompression-bomb guard
};

/// Connector used by new Clients for https. <flyG/net/https.hpp> registers the built-in TLS connector here.
[[nodiscard]] inline Connector& default_secure_connector() {
    static Connector c;
    return c;
}

class Client {
public:
    Client() : secure_(default_secure_connector()) {}
    explicit Client(ClientOptions o) : opt_(std::move(o)), secure_(default_secure_connector()) {}
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void set_secure_connector(Connector c) { secure_ = std::move(c); }
    [[nodiscard]] ClientOptions& options() noexcept { return opt_; }
    /// Headers added to every request (unless the request sets the same header itself).
    Headers default_headers;

    /// Sends a request to `url` (method/body/headers from `req`; target and Host are set here).
    /// Follows redirects according to the options. When `body_sink` is set, response body bytes
    /// are delivered to it instead of being stored in Response::body.
    Response send(const Url& url, Request req, const std::function<void(std::span<const std::byte>)>& body_sink = {}) {
        Url cur = url;
        for (int hop = 0;; ++hop) {
            Response resp = send_once(cur, req, body_sink);
            const bool redirect = resp.status == 301 || resp.status == 302 || resp.status == 303 || resp.status == 307 || resp.status == 308;
            const std::string* loc = resp.headers.find("Location");
            if (!opt_.follow_redirects || !redirect || !loc) return resp;
            if (hop >= opt_.max_redirects) throw NetworkException("too many redirects (" + std::to_string(opt_.max_redirects) + ")");
            Url next = cur.resolve(*loc);
            if (next.scheme != "http" && next.scheme != "https") throw NetworkException("redirect to unsupported scheme '" + next.scheme + "'");
            if (cur.scheme == "https" && next.scheme == "http") {
                // never silently downgrade; the caller can follow manually if they want to
                return resp;
            }
            const bool to_get = resp.status == 303 || ((resp.status == 301 || resp.status == 302) && req.method != "GET" && req.method != "HEAD");
            if (to_get) {
                if (req.method != "HEAD") req.method = "GET";
                req.body.clear();
                req.headers.remove("Content-Type");
                req.headers.remove("Content-Length");
            }
            if (next.origin() != cur.origin()) {  // do not leak credentials to another origin
                req.headers.remove("Authorization");
                req.headers.remove("Cookie");
            }
            cur = std::move(next);
            cur.fragment.reset();
        }
    }

    Response get(std::string_view url, Headers headers = {}) { return simple("GET", url, {}, std::move(headers)); }
    Response head(std::string_view url, Headers headers = {}) { return simple("HEAD", url, {}, std::move(headers)); }
    Response del(std::string_view url, Headers headers = {}) { return simple("DELETE", url, {}, std::move(headers)); }
    Response post(std::string_view url, std::string body, std::string content_type = "application/octet-stream", Headers headers = {}) {
        headers.set("Content-Type", std::move(content_type));
        return simple("POST", url, std::move(body), std::move(headers));
    }
    Response put(std::string_view url, std::string body, std::string content_type = "application/octet-stream", Headers headers = {}) {
        headers.set("Content-Type", std::move(content_type));
        return simple("PUT", url, std::move(body), std::move(headers));
    }

    /// Closes pooled idle connections.
    void close_idle() {
        std::lock_guard lock(m_);
        idle_.clear();
    }

private:
    struct Pooled {
        std::string key;
        std::unique_ptr<ByteStream> stream;
        std::unique_ptr<BufferedReader> reader;
    };

    Response simple(const char* method, std::string_view url, std::string body, Headers headers) {
        Request r;
        r.method = method;
        r.body = std::move(body);
        r.headers = std::move(headers);
        return send(Url::from_string(url), std::move(r));
    }

    static std::string pool_key(const Url& u) { return u.scheme + "://" + u.host + ":" + std::to_string(u.effective_port()); }

    Pooled take_connection(const Url& u, bool& reused) {
        const std::string key = pool_key(u);
        {
            std::lock_guard lock(m_);
            for (auto it = idle_.begin(); it != idle_.end(); ++it)
                if (it->key == key) {
                    Pooled p = std::move(*it);
                    idle_.erase(it);
                    reused = true;
                    return p;
                }
        }
        reused = false;
        Pooled p;
        p.key = key;
        if (u.scheme == "https") {
            if (!secure_) throw NetworkException("https requires a TLS connector (include <flyg/net/https.hpp>)");
            p.stream = secure_(u, opt_.connect_timeout);
        } else if (u.scheme == "http") {
            p.stream = connect_tcp(u, opt_.connect_timeout);
        } else {
            throw NetworkException("unsupported URL scheme '" + u.scheme + "'");
        }
        p.stream->set_timeouts(opt_.io_timeout, opt_.io_timeout);
        p.reader = std::make_unique<BufferedReader>(*p.stream);
        return p;
    }

    Response send_once(const Url& u, const Request& tmpl, const std::function<void(std::span<const std::byte>)>& sink) {
        if (!u.has_authority()) throw NetworkException("URL has no host: " + u.to_string());
        Request req = tmpl;
        req.target = u.request_target();
        req.headers.set("Host", u.host_header());
        for (const auto& [k, v] : default_headers)
            if (!req.headers.contains(k)) req.headers.add(k, v);
        if (!req.headers.contains("User-Agent")) req.headers.add("User-Agent", opt_.user_agent);
        if (!req.headers.contains("Accept")) req.headers.add("Accept", "*/*");
        if (!req.headers.contains("Accept-Encoding")) req.headers.add("Accept-Encoding", (opt_.decompress && !sink) ? "gzip, deflate" : "identity");
        if (!u.userinfo.empty() && !req.headers.contains("Authorization"))
            req.headers.add("Authorization", "Basic " + data::base64_encode(data::percent_decode(u.userinfo)));
        req.headers.set("Connection", opt_.keep_alive ? "keep-alive" : "close");
        req.headers.remove("Transfer-Encoding");
        if (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH")
            req.headers.set("Content-Length", std::to_string(req.body.size()));
        else
            req.headers.remove("Content-Length");

        const bool idempotent = req.method == "GET" || req.method == "HEAD" || req.method == "PUT" || req.method == "DELETE" || req.method == "OPTIONS";
        for (int attempt = 0;; ++attempt) {
            bool reused = false;
            Pooled conn = take_connection(u, reused);
            try {
                return exchange(conn, req, u, sink);
            } catch (const std::exception&) {
                // a pooled connection may have been closed by the server while idle: retry once on a fresh one
                if (reused && idempotent && attempt == 0) continue;
                throw;
            }
        }
    }

    /// Undoes Content-Encoding (gzip / x-gzip / deflate, possibly stacked) on a fully buffered response.
    void decode_content(Response& resp) const {
        auto enc = resp.headers.get("Content-Encoding", "");
        if (enc.empty()) return;
        std::vector<std::string> codings;
        usize start = 0;
        while (start <= enc.size()) {
            usize comma = enc.find(',', start);
            if (comma == std::string::npos) comma = enc.size();
            std::string tok = enc.substr(start, comma - start);
            const auto b = tok.find_first_not_of(" \t"), e = tok.find_last_not_of(" \t");
            tok = b == std::string::npos ? "" : tok.substr(b, e - b + 1);
            for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!tok.empty() && tok != "identity") codings.push_back(tok);
            start = comma + 1;
        }
        for (const auto& c : codings)
            if (c != "gzip" && c != "x-gzip" && c != "deflate") return;  // unknown coding: hand the body over untouched
        try {
            std::string body = std::move(resp.body);
            for (auto it = codings.rbegin(); it != codings.rend(); ++it) {
                const auto in = std::as_bytes(std::span(body.data(), body.size()));
                std::vector<std::byte> out;
                if (*it == "deflate") {
                    try {
                        out = data::zlib_decompress(in, opt_.max_decoded_body);
                    } catch (const DataException&) {
                        out = data::inflate(in, opt_.max_decoded_body);  // some servers send a raw deflate stream
                    }
                } else {
                    out = data::gzip_decompress(in, opt_.max_decoded_body);
                }
                body.assign(reinterpret_cast<const char*>(out.data()), out.size());
            }
            resp.body = std::move(body);
        } catch (const DataException& e) {
            throw NetworkException(std::string("invalid Content-Encoding in response: ") + e.what());
        }
        resp.headers.remove("Content-Encoding");
        resp.headers.set("Content-Length", std::to_string(resp.body.size()));
    }

    Response exchange(Pooled& conn, const Request& req, const Url& u, const std::function<void(std::span<const std::byte>)>& sink) {
        std::string wire = detail::serialize_head(req);
        const bool expect = req.headers.has_token("Expect", "100-continue");
        if (!expect) wire += req.body;
        conn.stream->write_all(wire);
        Response resp;
        for (;;) {
            read_status_line(*conn.reader, resp);
            resp.headers = Headers{};
            detail::read_headers(*conn.reader, resp.headers, opt_.limits);
            if (resp.status == 100) {
                if (expect) {
                    conn.stream->write_all(req.body);
                    continue;
                }
                continue;  // unsolicited interim response: ignore
            }
            if (resp.status / 100 == 1 && resp.status != 101) continue;
            break;
        }
        if (expect && resp.status != 100 && !req.body.empty()) {
            // server answered without waiting for the body: it will not read it, so drop the connection afterwards
            conn.stream->close();
        }
        const bool head = req.method == "HEAD";
        if (resp.status == 101) return resp;  // caller handles the upgraded stream via upgrade()
        const auto plan = detail::plan_body(resp.headers, false, resp.status, head);
        detail::read_body(*conn.reader, plan, resp.body, opt_.limits, nullptr, sink);
        if (opt_.decompress && !sink && !head && resp.status != 204 && resp.status != 304) decode_content(resp);
        const bool reusable = opt_.keep_alive && plan.kind != detail::Framing::until_close && resp.version == "HTTP/1.1" &&
                              !resp.headers.has_token("Connection", "close") && !(expect && resp.status != 100 && !req.body.empty());
        if (reusable) {
            std::lock_guard lock(m_);
            idle_.push_back(std::move(conn));
            while (idle_.size() > 8) idle_.pop_front();
        }
        (void)u;
        return resp;
    }

    static void read_status_line(BufferedReader& in, Response& r) {
        std::string line;
        if (!in.read_line(line, 16 * 1024)) throw NetworkException("server closed the connection without a response");
        // "HTTP/1.1 200 OK"
        if (line.size() < 12 || line.compare(0, 5, "HTTP/") != 0 || line[8] != ' ') throw NetworkException("malformed HTTP status line: '" + line.substr(0, 60) + "'");
        r.version = line.substr(0, 8);
        if (r.version != "HTTP/1.1" && r.version != "HTTP/1.0") throw NetworkException("unsupported HTTP version in response");
        usize code;
        if (!detail::parse_uint(std::string_view(line).substr(9, 3), code) || (line.size() > 12 && line[12] != ' ')) throw NetworkException("malformed HTTP status code");
        r.status = static_cast<int>(code);
        r.reason = line.size() > 13 ? line.substr(13) : std::string();
        r.body.clear();
    }

    ClientOptions opt_;
    Connector secure_;
    std::mutex m_;
    std::list<Pooled> idle_;

public:
    /// Performs a request expecting a protocol upgrade (101). Returns the response head and, on
    /// success, the still-open stream (with any bytes already buffered handed over via `reader`).
    struct Upgraded {
        Response response;
        std::unique_ptr<ByteStream> stream;
        std::unique_ptr<BufferedReader> reader;
    };
    [[nodiscard]] Upgraded upgrade(const Url& u, Request req) {
        Pooled conn;
        conn.key = pool_key(u);
        if (u.scheme == "https") {
            if (!secure_) throw NetworkException("https requires a TLS connector (include <flyg/net/https.hpp>)");
            conn.stream = secure_(u, opt_.connect_timeout);
        } else {
            conn.stream = connect_tcp(u, opt_.connect_timeout);
        }
        conn.stream->set_timeouts(opt_.io_timeout, opt_.io_timeout);
        conn.reader = std::make_unique<BufferedReader>(*conn.stream);
        req.target = u.request_target();
        req.headers.set("Host", u.host_header());
        if (!req.headers.contains("User-Agent")) req.headers.add("User-Agent", opt_.user_agent);
        conn.stream->write_all(detail::serialize_head(req) + req.body);
        Upgraded up;
        for (;;) {
            read_status_line(*conn.reader, up.response);
            up.response.headers = Headers{};
            detail::read_headers(*conn.reader, up.response.headers, opt_.limits);
            if (up.response.status / 100 == 1 && up.response.status != 101) continue;
            break;
        }
        if (up.response.status != 101) {
            const auto plan = detail::plan_body(up.response.headers, false, up.response.status, false);
            detail::read_body(*conn.reader, plan, up.response.body, opt_.limits, nullptr);
            return up;
        }
        up.stream = std::move(conn.stream);
        up.reader = std::move(conn.reader);
        return up;
    }
};

// ---- server ----------------------------------------------------------------------------------------------------

struct ServerOptions {
    Limits limits;
    Timeout read_timeout{15000};   ///< per read while a request is being received
    Timeout idle_timeout{30000};   ///< keep-alive connections idle longer than this are closed
    Timeout write_timeout{30000};
    usize max_connections = 256;
    usize max_requests_per_connection = 1000;
    std::string server_name = "flyG";
    /// Wraps an accepted TCP connection (e.g. a TLS server handshake). Default: plain TCP.
    std::function<std::unique_ptr<ByteStream>(TcpStream&&)> accept_wrapper;
    /// gzip-compress buffered text-like responses when the client accepts it (off by default).
    bool compress_responses = false;
    usize compress_min_size = 512;
    int compress_level = 5;
};

class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;

    Server(const Endpoint& ep, Handler handler, ServerOptions opt = {}) : opt_(std::move(opt)), handler_(std::move(handler)), listener_(TcpListener::bind(ep)) {
        if (!handler_) throw NetworkException("HTTP server needs a handler");
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server() { stop(); }

    [[nodiscard]] Endpoint local_endpoint() const { return listener_.local_endpoint(); }
    [[nodiscard]] u16 port() const { return local_endpoint().port(); }

    /// Starts accepting connections on a background thread.
    void start() {
        if (running_.exchange(true)) return;
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    /// Stops accepting, closes all connections and waits for handler threads.
    void stop() {
        if (!running_.exchange(false)) {
            if (acceptor_.joinable()) acceptor_.join();
            return;
        }
        if (acceptor_.joinable()) acceptor_.join();
        std::vector<std::shared_ptr<Conn>> conns;
        {
            std::lock_guard lock(m_);
            conns = conns_;
        }
        for (auto& c : conns) {
            std::lock_guard lock(c->m);
            if (c->stream) c->stream->interrupt();
        }
        for (auto& c : conns)
            if (c->thread.joinable()) c->thread.join();
        std::lock_guard lock(m_);
        conns_.clear();
    }
    [[nodiscard]] usize active_connections() const {
        std::lock_guard lock(m_);
        usize n = 0;
        for (const auto& c : conns_)
            if (!c->done.load()) ++n;
        return n;
    }

private:
    struct Conn {
        std::mutex m;
        ByteStream* stream = nullptr;
        std::thread thread;
        std::atomic<bool> done{false};
    };

    void accept_loop() {
        while (running_.load()) {
            std::optional<std::pair<TcpStream, Endpoint>> c;
            try {
                c = listener_.accept(Timeout{50});
            } catch (const NetworkException&) {
                continue;
            }
            reap();
            if (!c) continue;
            if (active_connections() >= opt_.max_connections) {
                try {
                    c->first.write_all("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                } catch (const NetworkException&) {
                }
                continue;
            }
            auto conn = std::make_shared<Conn>();
            auto stream = std::make_shared<TcpStream>(std::move(c->first));
            const Endpoint remote = c->second;
            conn->thread = std::thread([this, conn, stream, remote]() mutable {
                try {
                    serve(*conn, std::move(*stream), remote);
                } catch (...) {
                }
                conn->done = true;
            });
            std::lock_guard lock(m_);
            conns_.push_back(std::move(conn));
        }
    }
    void reap() {
        std::lock_guard lock(m_);
        for (auto it = conns_.begin(); it != conns_.end();) {
            if ((*it)->done.load()) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void serve(Conn& conn, TcpStream tcp, const Endpoint& remote) {
        std::unique_ptr<ByteStream> stream;
        try {
            stream = opt_.accept_wrapper ? opt_.accept_wrapper(std::move(tcp)) : std::make_unique<TcpByteStream>(std::move(tcp));
        } catch (const std::exception&) {
            return;  // failed TLS handshake etc.
        }
        {
            std::lock_guard lock(conn.m);
            conn.stream = stream.get();
        }
        struct Clear {
            Conn& c;
            ~Clear() {
                std::lock_guard lock(c.m);
                c.stream = nullptr;
            }
        } clear{conn};
        BufferedReader in(*stream);
        for (usize served = 0; running_.load() && served < opt_.max_requests_per_connection; ++served) {
            Request req;
            Response resp;
            bool close_after = false;
            stream->set_timeouts(served == 0 ? opt_.read_timeout : opt_.idle_timeout, opt_.write_timeout);
            try {
                // head first (idle timeout applies to the wait for it), then body with the normal timeout
                if (!read_request(in, req, opt_.limits, false)) return;
                stream->set_timeouts(opt_.read_timeout, opt_.write_timeout);
                req.remote = remote;
                if (req.headers.has_token("Expect", "100-continue") && req.version == "HTTP/1.1") stream->write_all("HTTP/1.1 100 Continue\r\n\r\n");
                const auto plan = detail::plan_body(req.headers, true, 0, false);
                detail::read_body(in, plan, req.body, opt_.limits, nullptr);
            } catch (const detail::ProtocolError& e) {
                send_error(*stream, e.status(), e.what());
                return;
            } catch (const TimeoutException&) {
                if (served == 0 || in.buffered() != 0) send_error(*stream, 408, "request timed out");
                return;
            } catch (const NetworkException&) {
                return;  // peer vanished
            }
            close_after = req.version == "HTTP/1.0" ? !req.headers.has_token("Connection", "keep-alive") : req.headers.has_token("Connection", "close");
            try {
                handler_(req, resp);
            } catch (const std::exception& e) {
                resp = Response::text(500, "Internal Server Error");
                (void)e;
            } catch (...) {
                resp = Response::text(500, "Internal Server Error");
            }
            try {
                if (!write_response(*stream, in, req, resp, close_after)) return;
            } catch (const NetworkException&) {
                return;
            }
            if (close_after) {
                stream->shutdown_write();
                // drain a little so the peer sees our response before the RST of an abortive close
                try {
                    stream->set_timeouts(Timeout{200}, Timeout{200});
                    std::byte sink[1024];
                    while (stream->read_some(sink) != 0) {
                    }
                } catch (const NetworkException&) {
                }
                return;
            }
        }
    }

    void send_error(ByteStream& s, int status, const std::string& msg) {
        Response r = Response::text(status, msg + "\n");
        r.headers.set("Connection", "close");
        r.headers.set("Content-Length", std::to_string(r.body.size()));
        r.headers.set("Server", opt_.server_name);
        try {
            s.write_all(detail::serialize_head(r) + r.body);
            s.shutdown_write();
        } catch (const NetworkException&) {
        }
    }

    /// Writes the response. Returns false if the connection must not be used any more.
    bool write_response(ByteStream& s, BufferedReader& in, const Request& req, Response& resp, bool& close_after) {
        const bool head = req.method == "HEAD";
        resp.headers.set("Server", opt_.server_name);
        if (!resp.headers.contains("Date")) resp.headers.add("Date", http_date());
        if (resp.upgrade) {
            resp.status = resp.status == 200 ? 101 : resp.status;
            s.write_all(detail::serialize_head(resp));
            resp.upgrade(s, in);
            return false;
        }
        const bool no_body = head || resp.status / 100 == 1 || resp.status == 204 || resp.status == 304;
        if (close_after) resp.headers.set("Connection", "close");
        else if (req.version == "HTTP/1.0") resp.headers.set("Connection", "keep-alive");
        resp.headers.remove("Transfer-Encoding");
        if (resp.producer && !no_body) {
            if (req.version == "HTTP/1.0") {  // no chunking in 1.0: stream until close
                resp.headers.remove("Content-Length");
                resp.headers.set("Connection", "close");
                close_after = true;
                s.write_all(detail::serialize_head(resp));
                std::string chunk;
                while (true) {
                    chunk.clear();
                    const bool more = resp.producer(chunk);
                    if (!chunk.empty()) s.write_all(chunk);
                    if (!more) break;
                }
                return true;
            }
            resp.headers.remove("Content-Length");
            resp.headers.set("Transfer-Encoding", "chunked");
            s.write_all(detail::serialize_head(resp));
            std::string chunk;
            for (;;) {
                chunk.clear();
                const bool more = resp.producer(chunk);
                detail::write_chunk(s, chunk);
                if (!more) break;
            }
            s.write_all("0\r\n\r\n");
            return true;
        }
        if (opt_.compress_responses && !no_body) maybe_compress(req, resp);
        if (!no_body || head) resp.headers.set("Content-Length", std::to_string(resp.body.size()));
        if (resp.status == 204 || resp.status == 304 || resp.status / 100 == 1) resp.headers.remove("Content-Length");
        std::string out = detail::serialize_head(resp);
        if (!no_body) out += resp.body;
        s.write_all(out);
        return true;
    }

    static bool accepts_gzip(const Request& req) {
        std::string ae = req.headers.get("Accept-Encoding", "");
        for (auto& c : ae) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        usize pos = ae.find("gzip");
        if (pos == std::string::npos) return false;
        const usize end = ae.find(',', pos);
        const std::string item = ae.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        return item.find("q=0") == std::string::npos || item.find("q=0.") != std::string::npos;
    }
    void maybe_compress(const Request& req, Response& resp) const {
        if (resp.body.size() < opt_.compress_min_size || resp.headers.contains("Content-Encoding") || !accepts_gzip(req)) return;
        if (resp.status < 200 || resp.status == 206) return;
        std::string ct = resp.headers.get("Content-Type", "");
        for (auto& c : ct) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool texty = ct.rfind("text/", 0) == 0 || ct.find("json") != std::string::npos || ct.find("xml") != std::string::npos ||
                           ct.find("javascript") != std::string::npos || ct.find("svg") != std::string::npos;
        if (!texty) return;
        const auto z = data::gzip_compress(std::as_bytes(std::span(resp.body.data(), resp.body.size())), opt_.compress_level);
        if (z.size() >= resp.body.size()) return;
        resp.body.assign(reinterpret_cast<const char*>(z.data()), z.size());
        resp.headers.set("Content-Encoding", "gzip");
        resp.headers.add("Vary", "Accept-Encoding");
    }

    [[nodiscard]] static std::string http_date() {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if FLYG_PLATFORM_WINDOWS
        ::gmtime_s(&tm, &t);
#else
        ::gmtime_r(&t, &tm);
#endif
        char buf[64];
        static constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return buf;
    }

    ServerOptions opt_;
    Handler handler_;
    TcpListener listener_;
    std::atomic<bool> running_{false};
    std::thread acceptor_;
    mutable std::mutex m_;
    std::vector<std::shared_ptr<Conn>> conns_;
};

}  // namespace flyg::net::http
