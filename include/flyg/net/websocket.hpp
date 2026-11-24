// flyG - net module: WebSocket (RFC 6455) client and server
// SPDX-License-Identifier: GPL-2.0-only
//
// Complete framing layer: masking, fragmentation, ping/pong, close handshake with status code
// validation, UTF-8 validation of text messages, message size limit. Not implemented:
// extensions (permessage-deflate needs DEFLATE).
#pragma once

#include <mutex>
#include <random>

#include <flyg/data/encoding.hpp>
#include <flyg/data/hash.hpp>
#include <flyg/net/http.hpp>

namespace flyg::net::ws {

enum class Opcode : core::u8 { continuation = 0, text = 1, binary = 2, close = 8, ping = 9, pong = 10 };

namespace close_code {
inline constexpr u16 normal = 1000, going_away = 1001, protocol_error = 1002, unsupported_data = 1003, no_status = 1005, abnormal = 1006,
                     invalid_payload = 1007, policy_violation = 1008, message_too_big = 1009, mandatory_extension = 1010, internal_error = 1011;
}

struct Message {
    bool binary = false;  ///< false: UTF-8 text
    std::string data;
};

struct Options {
    usize max_message_size = 16 * 1024 * 1024;
    bool auto_pong = true;
};

namespace detail {
inline std::string accept_key(std::string_view client_key) {
    const auto d = data::sha1(std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return data::base64_encode(std::span<const std::byte>(d));
}
inline void random_bytes(std::byte* out, usize n) {
    static std::mutex m;
    static std::mt19937_64 gen{std::random_device{}()};
    std::lock_guard lock(m);
    for (usize i = 0; i < n; ++i) out[i] = static_cast<std::byte>(gen());
}
[[nodiscard]] inline bool valid_close_code(u16 c) noexcept {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1011) || (c >= 3000 && c <= 4999);
}
}  // namespace detail

/// A WebSocket connection (either role). One thread may call receive() while others send.
class Connection {
public:
    /// Takes ownership of the stream (client side).
    Connection(std::unique_ptr<ByteStream> stream, std::unique_ptr<BufferedReader> reader, bool is_client, Options opt = {})
        : owned_stream_(std::move(stream)), owned_reader_(std::move(reader)), stream_(owned_stream_.get()), reader_(owned_reader_.get()), client_(is_client), opt_(opt) {
        if (!reader_) {
            owned_reader_ = std::make_unique<BufferedReader>(*stream_);
            reader_ = owned_reader_.get();
        }
    }
    /// Borrows the stream (server side inside an upgrade callback).
    Connection(ByteStream& stream, BufferedReader& reader, bool is_client, Options opt = {}) : stream_(&stream), reader_(&reader), client_(is_client), opt_(opt) {}
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() {
        if (!closed_sent_ && stream_) {
            try {
                close(close_code::going_away);
            } catch (...) {
            }
        }
    }

    // ---- sending (thread-safe) ---------------------------------------------------------------------------
    void send_text(std::string_view text) {
        if (!data::utf8::is_valid(text)) throw NetworkException("WebSocket text messages must be valid UTF-8");
        send_frame(Opcode::text, std::as_bytes(std::span(text.data(), text.size())));
    }
    void send_binary(std::span<const std::byte> data) { send_frame(Opcode::binary, data); }
    void send_binary(std::string_view data) { send_frame(Opcode::binary, std::as_bytes(std::span(data.data(), data.size()))); }
    void send_ping(std::string_view payload = {}) {
        if (payload.size() > 125) throw NetworkException("ping payload longer than 125 bytes");
        send_frame(Opcode::ping, std::as_bytes(std::span(payload.data(), payload.size())));
    }
    /// Sends a message split into frames of at most `frame_size` bytes (for streaming/testing).
    void send_fragmented(bool binary, std::string_view data, usize frame_size) {
        if (frame_size == 0) throw NetworkException("frame size must be positive");
        std::lock_guard lock(send_m_);
        usize off = 0;
        bool first = true;
        do {
            const usize n = std::min(frame_size, data.size() - off);
            const bool fin = off + n >= data.size();
            write_frame_locked(first ? (binary ? Opcode::binary : Opcode::text) : Opcode::continuation, fin, std::as_bytes(std::span(data.data() + off, n)));
            off += n;
            first = false;
        } while (off < data.size());
    }
    /// Starts the closing handshake. receive() returns nullopt once the peer's close arrives.
    void close(u16 code = close_code::normal, std::string_view reason = {}) {
        std::lock_guard lock(send_m_);
        if (closed_sent_) return;
        closed_sent_ = true;
        std::string payload;
        if (code != close_code::no_status) {
            payload += static_cast<char>(code >> 8);
            payload += static_cast<char>(code & 0xFF);
            payload.append(reason.substr(0, 123));
        }
        write_frame_locked(Opcode::close, true, std::as_bytes(std::span(payload.data(), payload.size())));
    }

    // ---- receiving ---------------------------------------------------------------------------------------------
    /// Returns the next complete message, or nullopt after the connection has been closed
    /// (check close_code()). Control frames are handled internally. Throws NetworkException on
    /// transport errors/timeouts.
    std::optional<Message> receive() {
        if (closed_received_) return std::nullopt;
        std::string assembled;
        bool in_message = false, binary = false;
        for (;;) {
            Frame f;
            try {
                if (!read_frame(f)) {  // transport closed without a close frame
                    mark_closed(close_code::abnormal, "connection lost");
                    return std::nullopt;
                }
            } catch (const ProtocolViolation& v) {
                fail(v.code, v.what());
                return std::nullopt;
            }
            switch (f.opcode) {
                case Opcode::ping:
                    if (opt_.auto_pong) {
                        try {
                            send_frame(Opcode::pong, std::as_bytes(std::span(f.payload.data(), f.payload.size())));
                        } catch (const NetworkException&) {
                        }
                    }
                    last_ping_ = f.payload;
                    continue;
                case Opcode::pong: last_pong_ = f.payload; continue;
                case Opcode::close: {
                    u16 code = close_code::no_status;
                    std::string reason;
                    if (f.payload.size() == 1) {
                        fail(close_code::protocol_error, "close frame with a 1-byte payload");
                        return std::nullopt;
                    }
                    if (f.payload.size() >= 2) {
                        code = static_cast<u16>((static_cast<u8>(f.payload[0]) << 8) | static_cast<u8>(f.payload[1]));
                        reason = f.payload.substr(2);
                        if (!detail::valid_close_code(code)) {
                            fail(close_code::protocol_error, "invalid close code");
                            return std::nullopt;
                        }
                        if (!data::utf8::is_valid(reason)) {
                            fail(close_code::invalid_payload, "close reason is not UTF-8");
                            return std::nullopt;
                        }
                    }
                    mark_closed(code, reason);
                    try {
                        close(code == close_code::no_status ? close_code::normal : code);  // echo (no-op if we closed first)
                    } catch (const NetworkException&) {
                    }
                    return std::nullopt;
                }
                default: break;
            }
            // data frame
            if (f.opcode == Opcode::continuation) {
                if (!in_message) {
                    fail(close_code::protocol_error, "continuation frame without a message");
                    return std::nullopt;
                }
            } else {
                if (in_message) {
                    fail(close_code::protocol_error, "new data frame while a fragmented message is open");
                    return std::nullopt;
                }
                in_message = true;
                binary = f.opcode == Opcode::binary;
            }
            if (assembled.size() + f.payload.size() > opt_.max_message_size) {
                fail(close_code::message_too_big, "message exceeds the size limit");
                return std::nullopt;
            }
            assembled += f.payload;
            if (f.fin) {
                if (!binary && !data::utf8::is_valid(assembled)) {
                    fail(close_code::invalid_payload, "text message is not valid UTF-8");
                    return std::nullopt;
                }
                return Message{binary, std::move(assembled)};
            }
        }
    }

    [[nodiscard]] bool is_closed() const noexcept { return closed_received_; }
    [[nodiscard]] u16 close_code() const noexcept { return close_code_; }
    [[nodiscard]] const std::string& close_reason() const noexcept { return close_reason_; }
    [[nodiscard]] const std::string& last_pong() const noexcept { return last_pong_; }
    [[nodiscard]] const std::string& last_ping() const noexcept { return last_ping_; }
    [[nodiscard]] const std::string& subprotocol() const noexcept { return subprotocol_; }
    void set_subprotocol(std::string s) { subprotocol_ = std::move(s); }
    void set_timeouts(Timeout r, Timeout w) { stream_->set_timeouts(r, w); }
    [[nodiscard]] Endpoint peer_endpoint() const { return stream_->peer_endpoint(); }

private:
    struct Frame {
        bool fin = false;
        Opcode opcode = Opcode::continuation;
        std::string payload;
    };
    struct ProtocolViolation : std::runtime_error {
        u16 code;
        ProtocolViolation(u16 c, const char* w) : std::runtime_error(w), code(c) {}
    };

    /// Reads one frame. Returns false on EOF at a frame boundary.
    bool read_frame(Frame& f) {
        std::byte h[2];
        if (reader_->read_some(std::span<std::byte>(h, 1)) == 0) return false;
        reader_->read_exact(std::span<std::byte>(h + 1, 1));
        const u8 b0 = static_cast<u8>(h[0]), b1 = static_cast<u8>(h[1]);
        f.fin = b0 & 0x80;
        if (b0 & 0x70) throw ProtocolViolation(close_code::protocol_error, "reserved bits set (no extension negotiated)");
        const u8 op = b0 & 0x0F;
        if (!(op <= 2 || (op >= 8 && op <= 10))) throw ProtocolViolation(close_code::protocol_error, "unknown opcode");
        f.opcode = static_cast<Opcode>(op);
        const bool masked = b1 & 0x80;
        if (masked == client_) throw ProtocolViolation(close_code::protocol_error, client_ ? "server frames must not be masked" : "client frames must be masked");
        core::u64 len = b1 & 0x7F;
        if (len == 126) {
            std::byte e[2];
            reader_->read_exact(e);
            len = (static_cast<core::u64>(e[0]) << 8) | static_cast<core::u64>(e[1]);
            if (len < 126) throw ProtocolViolation(close_code::protocol_error, "non-minimal length encoding");
        } else if (len == 127) {
            std::byte e[8];
            reader_->read_exact(e);
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<core::u64>(e[i]);
            if (len >> 63) throw ProtocolViolation(close_code::protocol_error, "invalid 64-bit length");
            if (len < 65536) throw ProtocolViolation(close_code::protocol_error, "non-minimal length encoding");
        }
        if (op >= 8) {
            if (!f.fin) throw ProtocolViolation(close_code::protocol_error, "fragmented control frame");
            if (len > 125) throw ProtocolViolation(close_code::protocol_error, "control frame payload too large");
        }
        if (len > opt_.max_message_size) throw ProtocolViolation(close_code::message_too_big, "frame exceeds the size limit");
        std::byte mask[4] = {};
        if (masked) reader_->read_exact(mask);
        f.payload.clear();
        f.payload.resize(static_cast<usize>(len));
        reader_->read_exact(std::as_writable_bytes(std::span(f.payload.data(), f.payload.size())));
        if (masked)
            for (usize i = 0; i < f.payload.size(); ++i) f.payload[i] = static_cast<char>(static_cast<u8>(f.payload[i]) ^ static_cast<u8>(mask[i & 3]));
        return true;
    }

    void send_frame(Opcode op, std::span<const std::byte> payload) {
        std::lock_guard lock(send_m_);
        if (closed_sent_) throw NetworkException("WebSocket is closing; cannot send");
        write_frame_locked(op, true, payload);
    }
    void write_frame_locked(Opcode op, bool fin, std::span<const std::byte> payload) {
        std::vector<std::byte> out;
        out.reserve(payload.size() + 14);
        out.push_back(static_cast<std::byte>((fin ? 0x80 : 0) | static_cast<u8>(op)));
        const u8 maskbit = client_ ? 0x80 : 0;
        if (payload.size() < 126) {
            out.push_back(static_cast<std::byte>(maskbit | payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            out.push_back(static_cast<std::byte>(maskbit | 126));
            out.push_back(static_cast<std::byte>(payload.size() >> 8));
            out.push_back(static_cast<std::byte>(payload.size()));
        } else {
            out.push_back(static_cast<std::byte>(maskbit | 127));
            for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::byte>(static_cast<core::u64>(payload.size()) >> (8 * i)));
        }
        if (client_) {
            std::byte mask[4];
            detail::random_bytes(mask, 4);
            out.insert(out.end(), mask, mask + 4);
            for (usize i = 0; i < payload.size(); ++i) out.push_back(payload[i] ^ mask[i & 3]);
        } else {
            out.insert(out.end(), payload.begin(), payload.end());
        }
        stream_->write_all(out);
    }

    void mark_closed(u16 code, const std::string& reason) {
        closed_received_ = true;
        close_code_ = code;
        close_reason_ = reason;
    }
    void fail(u16 code, const char* why) {
        try {
            close(code, why);
        } catch (const NetworkException&) {
        }
        mark_closed(code, why);
    }

    std::unique_ptr<ByteStream> owned_stream_;
    std::unique_ptr<BufferedReader> owned_reader_;
    ByteStream* stream_;
    BufferedReader* reader_;
    bool client_;
    Options opt_;
    std::mutex send_m_;
    bool closed_sent_ = false;
    bool closed_received_ = false;
    u16 close_code_ = 0;
    std::string close_reason_, last_pong_, last_ping_, subprotocol_;
};

// ---- client handshake ---------------------------------------------------------------------------------------------

struct ConnectOptions {
    std::vector<std::string> subprotocols;
    std::string origin;
    http::Headers headers;
    Options ws;
};

/// Opens a WebSocket to a ws:// or wss:// URL using `client` for the underlying connection (so
/// its timeouts and TLS connector apply).
[[nodiscard]] inline std::unique_ptr<Connection> connect(http::Client& client, std::string_view url, const ConnectOptions& opt = {}) {
    Url u = Url::from_string(url);
    if (u.scheme == "ws") u.scheme = "http";
    else if (u.scheme == "wss") u.scheme = "https";
    else throw NetworkException("WebSocket URL must use ws:// or wss://");
    std::byte keyraw[16];
    detail::random_bytes(keyraw, 16);
    const std::string key = data::base64_encode(std::span<const std::byte>(keyraw));
    http::Request req;
    req.method = "GET";
    for (const auto& [k, v] : opt.headers) req.headers.add(k, v);
    req.headers.set("Upgrade", "websocket");
    req.headers.set("Connection", "Upgrade");
    req.headers.set("Sec-WebSocket-Key", key);
    req.headers.set("Sec-WebSocket-Version", "13");
    if (!opt.origin.empty()) req.headers.set("Origin", opt.origin);
    if (!opt.subprotocols.empty()) {
        std::string list;
        for (const auto& s : opt.subprotocols) list += (list.empty() ? "" : ", ") + s;
        req.headers.set("Sec-WebSocket-Protocol", list);
    }
    auto up = client.upgrade(u, std::move(req));
    const auto& r = up.response;
    if (r.status != 101) throw NetworkException("WebSocket handshake failed: server answered " + std::to_string(r.status) + " " + r.reason);
    if (!http::iequals(r.headers.get("Upgrade"), "websocket") || !r.headers.has_token("Connection", "upgrade"))
        throw NetworkException("WebSocket handshake failed: missing Upgrade/Connection headers");
    if (r.headers.get("Sec-WebSocket-Accept") != detail::accept_key(key)) throw NetworkException("WebSocket handshake failed: bad Sec-WebSocket-Accept");
    if (r.headers.contains("Sec-WebSocket-Extensions")) throw NetworkException("WebSocket handshake failed: server selected an extension we did not offer");
    std::string proto = r.headers.get("Sec-WebSocket-Protocol");
    if (!proto.empty() && std::find(opt.subprotocols.begin(), opt.subprotocols.end(), proto) == opt.subprotocols.end())
        throw NetworkException("WebSocket handshake failed: server selected an unoffered subprotocol");
    up.stream->set_timeouts(no_timeout, client.options().io_timeout);
    auto c = std::make_unique<Connection>(std::move(up.stream), std::move(up.reader), true, opt.ws);
    c->set_subprotocol(proto);
    return c;
}
[[nodiscard]] inline std::unique_ptr<Connection> connect(std::string_view url, const ConnectOptions& opt = {}) {
    // The client must outlive nothing: the upgraded stream is independent of it.
    http::Client client;
    return connect(client, url, opt);
}

// ---- server side -----------------------------------------------------------------------------------------------------

struct AcceptOptions {
    std::vector<std::string> subprotocols;  ///< supported, in server preference order
    Options ws;
    std::function<bool(const http::Request&)> check_origin;  ///< optional CSRF guard; return false to refuse
};

[[nodiscard]] inline bool is_upgrade_request(const http::Request& req) {
    return req.headers.has_token("Connection", "upgrade") && http::iequals(req.headers.get("Upgrade"), "websocket");
}

/// Prepares `resp` for a WebSocket upgrade and arranges for `handler` to run on the connection
/// thread once the 101 response has been sent. Returns false (with `resp` set to an error) if the
/// request is not a valid WebSocket handshake or was refused.
inline bool accept(const http::Request& req, http::Response& resp, std::function<void(Connection&)> handler, const AcceptOptions& opt = {}) {
    auto refuse = [&](int status, const std::string& why) {
        resp = http::Response::text(status, why + "\n");
        return false;
    };
    if (req.method != "GET") return refuse(405, "WebSocket handshake requires GET");
    if (req.version != "HTTP/1.1") return refuse(400, "WebSocket handshake requires HTTP/1.1");
    if (!is_upgrade_request(req)) return refuse(400, "not a WebSocket upgrade request");
    if (req.headers.get("Sec-WebSocket-Version") != "13") {
        refuse(426, "unsupported WebSocket version");
        resp.headers.set("Sec-WebSocket-Version", "13");
        return false;
    }
    const std::string key = req.headers.get("Sec-WebSocket-Key");
    try {
        if (data::base64_decode(key).size() != 16) return refuse(400, "invalid Sec-WebSocket-Key");
    } catch (const std::exception&) {
        return refuse(400, "invalid Sec-WebSocket-Key");
    }
    if (opt.check_origin && !opt.check_origin(req)) return refuse(403, "origin not allowed");
    std::string chosen;
    if (!opt.subprotocols.empty() && req.headers.contains("Sec-WebSocket-Protocol")) {
        for (const auto& mine : opt.subprotocols)
            if (req.headers.has_token("Sec-WebSocket-Protocol", mine)) {
                chosen = mine;
                break;
            }
    }
    resp = http::Response{};
    resp.status = 101;
    resp.headers.set("Upgrade", "websocket");
    resp.headers.set("Connection", "Upgrade");
    resp.headers.set("Sec-WebSocket-Accept", detail::accept_key(key));
    if (!chosen.empty()) resp.headers.set("Sec-WebSocket-Protocol", chosen);
    const Options wso = opt.ws;
    resp.upgrade = [handler = std::move(handler), wso, chosen](ByteStream& s, BufferedReader& in) {
        s.set_timeouts(no_timeout, Timeout{30000});
        Connection c(s, in, false, wso);
        c.set_subprotocol(chosen);
        handler(c);
        try {
            c.close(close_code::normal);
            // give the peer a moment to complete the closing handshake
            s.set_timeouts(Timeout{1000}, Timeout{1000});
            while (c.receive()) {
            }
        } catch (const NetworkException&) {
        }
        // Lingering close: FIN our side, then discard what the peer still has in flight (bounded) so that
        // closing with unread data does not turn into a TCP reset that destroys our close frame.
        try {
            s.shutdown_write();
            s.set_timeouts(Timeout{250}, Timeout{250});
            std::byte sink[8192];
            usize drained = 0;
            while (drained < (4u << 20)) {
                const usize n = s.read_some(sink);
                if (n == 0) break;
                drained += n;
            }
        } catch (const NetworkException&) {
        }
    };
    return true;
}

}  // namespace flyg::net::ws
