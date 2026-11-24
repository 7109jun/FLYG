// flyG - net module: byte-stream abstraction (TCP now, TLS on top) and a buffered reader
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <memory>
#include <string>

#include <flyg/net/socket.hpp>

namespace flyg::net {

/// A reliable, ordered, bidirectional byte stream. Implemented by TCP and TLS connections so that
/// HTTP and WebSocket code does not care what carries it. Not thread-safe except that one thread
/// may read while another writes.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    /// Reads at least one byte; returns 0 at end of stream. Throws TimeoutException on read timeout.
    virtual usize read_some(std::span<std::byte> buf) = 0;
    virtual void write_all(std::span<const std::byte> data) = 0;
    /// Signals that nothing more will be written (TLS: close_notify, TCP: FIN).
    virtual void shutdown_write() = 0;
    virtual void close() noexcept = 0;
    /// Thread-safe: wakes threads blocked in read/write on this stream so they fail promptly. Unlike close() it
    /// never invalidates the handle, so it may be called while another thread is using the stream.
    virtual void interrupt() noexcept { close(); }
    virtual void set_timeouts(Timeout read, Timeout write) = 0;
    [[nodiscard]] virtual Endpoint peer_endpoint() const = 0;
    [[nodiscard]] virtual bool is_secure() const noexcept { return false; }

    void write_all(std::string_view s) { write_all(std::as_bytes(std::span(s.data(), s.size()))); }
};

class TcpByteStream final : public ByteStream {
public:
    explicit TcpByteStream(TcpStream s) : s_(std::move(s)) {}
    usize read_some(std::span<std::byte> buf) override { return s_.read_some(buf); }
    void write_all(std::span<const std::byte> d) override { s_.write_all(d); }
    void shutdown_write() override {
        try {
            s_.shutdown(TcpStream::Shutdown::write);
        } catch (const NetworkException&) {
        }
    }
    void close() noexcept override { s_.close(); }
    void interrupt() noexcept override {
        try {
            s_.shutdown(TcpStream::Shutdown::both);
        } catch (...) {
        }
    }
    void set_timeouts(Timeout r, Timeout w) override {
        s_.set_read_timeout(r);
        s_.set_write_timeout(w);
    }
    [[nodiscard]] Endpoint peer_endpoint() const override { return s_.peer_endpoint(); }
    [[nodiscard]] TcpStream& tcp() noexcept { return s_; }

private:
    TcpStream s_;
};

/// A protocol line exceeded the caller's limit.
class LineTooLong : public NetworkException {
public:
    LineTooLong() : NetworkException("line too long") {}
};

/// Read-side buffer over a ByteStream (line and exact-length reads for protocol parsers).
class BufferedReader {
public:
    explicit BufferedReader(ByteStream& s, usize chunk = 16384) : s_(&s), chunk_(chunk) {}

    /// Reads one line terminated by LF (a preceding CR is removed). Returns false on clean EOF
    /// before any byte. Throws if the line exceeds `max_len` or EOF occurs mid-line.
    bool read_line(std::string& out, usize max_len) {
        out.clear();
        for (;;) {
            for (; pos_ < buf_.size(); ++pos_) {
                const char c = static_cast<char>(buf_[pos_]);
                if (c == '\n') {
                    ++pos_;
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    return true;
                }
                out += c;
                if (out.size() > max_len) throw LineTooLong();
            }
            if (!fill()) {
                if (out.empty()) return false;
                throw NetworkException("connection closed in the middle of a line");
            }
        }
    }
    /// Reads up to `n` bytes (at least one unless EOF).
    usize read_some(std::span<std::byte> out) {
        if (pos_ == buf_.size() && !fill()) return 0;
        const usize k = std::min(out.size(), buf_.size() - pos_);
        std::memcpy(out.data(), buf_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    void read_exact(std::span<std::byte> out) {
        usize done = 0;
        while (done < out.size()) {
            const usize k = read_some(out.subspan(done));
            if (k == 0) throw NetworkException("connection closed before all data was received");
            done += k;
        }
    }
    /// Appends exactly `n` bytes to `dst`.
    void read_exact(std::string& dst, usize n) {
        const usize old = dst.size();
        dst.resize(old + n);
        read_exact(std::as_writable_bytes(std::span(dst.data() + old, n)));
    }
    [[nodiscard]] usize buffered() const noexcept { return buf_.size() - pos_; }
    [[nodiscard]] ByteStream& stream() noexcept { return *s_; }

private:
    bool fill() {
        buf_.resize(chunk_);
        const usize n = s_->read_some(buf_);
        buf_.resize(n);
        pos_ = 0;
        return n != 0;
    }
    ByteStream* s_;
    usize chunk_;
    std::vector<std::byte> buf_;
    usize pos_ = 0;
};

}  // namespace flyg::net
