// flyG - net module: poll-based event loop, timers and asynchronous TCP connections
// SPDX-License-Identifier: GPL-2.0-only
//
// EventLoop::run() processes readiness callbacks, timers and posted tasks on the calling thread.
// add/modify/remove/post/run_after/cancel/stop may be called from any thread (and from inside
// callbacks). Callbacks never run concurrently with each other.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include <flyg/net/socket.hpp>

namespace flyg::net {

using core::u64;

class EventLoop {
public:
    using Callback = std::function<void(unsigned events)>;  ///< PollEvent mask
    using TimerId = u64;

    EventLoop() : wake_(UdpSocket::bind(Endpoint(IpAddress::loopback(), 0))) { wake_ep_ = wake_.local_endpoint(); }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// Starts watching a socket. Replaces an existing registration for the same socket.
    void add(detail::native_socket s, unsigned events, Callback cb) {
        {
            std::lock_guard lock(m_);
            watches_[s] = Watch{events, std::make_shared<Callback>(std::move(cb))};
        }
        wake();
    }
    void modify(detail::native_socket s, unsigned events) {
        {
            std::lock_guard lock(m_);
            if (auto it = watches_.find(s); it != watches_.end()) it->second.events = events;
        }
        wake();
    }
    void remove(detail::native_socket s) {
        {
            std::lock_guard lock(m_);
            watches_.erase(s);
        }
        wake();
    }

    /// Runs `fn` on the loop thread as soon as possible.
    void post(std::function<void()> fn) {
        {
            std::lock_guard lock(m_);
            tasks_.push_back(std::move(fn));
        }
        wake();
    }
    /// Runs `fn` once after `delay` (on the loop thread).
    TimerId run_after(Timeout delay, std::function<void()> fn) {
        TimerId id;
        {
            std::lock_guard lock(m_);
            id = ++next_timer_;
            timers_.emplace(std::chrono::steady_clock::now() + delay, Timer{id, std::move(fn)});
        }
        wake();
        return id;
    }
    /// Cancels a pending timer; returns false if it already fired or never existed.
    bool cancel(TimerId id) {
        std::lock_guard lock(m_);
        for (auto it = timers_.begin(); it != timers_.end(); ++it)
            if (it->second.id == id) {
                timers_.erase(it);
                return true;
            }
        return false;
    }

    /// Runs until stop() is called.
    void run() {
        stop_.store(false);
        while (!stop_.load()) run_once(no_timeout);
    }
    /// One iteration: waits up to `timeout` for activity, then dispatches. Returns true if
    /// anything (event, timer or task) was handled.
    bool run_once(Timeout timeout) {
        bool did = run_tasks();
        std::vector<PollItem> items;
        std::vector<std::shared_ptr<Callback>> cbs;
        Timeout wait = timeout;
        {
            std::lock_guard lock(m_);
            if (!tasks_.empty()) wait = Timeout{0};
            if (!timers_.empty()) {
                auto left = std::chrono::duration_cast<Timeout>(timers_.begin()->first - std::chrono::steady_clock::now());
                if (left.count() < 0) left = Timeout{0};
                if (wait.count() < 0 || left < wait) wait = left;
            }
            items.push_back({wake_.native_handle(), poll_read, 0});
            cbs.push_back(nullptr);
            for (const auto& [s, w] : watches_) {
                items.push_back({s, w.events, 0});
                cbs.push_back(w.cb);
            }
        }
        net::poll(items, wait);
        if (items[0].revents) drain_wake();
        for (usize i = 1; i < items.size(); ++i) {
            if (!items[i].revents) continue;
            {
                // the registration may have been removed or replaced by an earlier callback
                std::lock_guard lock(m_);
                const auto it = watches_.find(items[i].socket);
                if (it == watches_.end() || it->second.cb != cbs[i]) continue;
            }
            (*cbs[i])(items[i].revents);
            did = true;
        }
        did |= run_timers();
        did |= run_tasks();
        return did;
    }
    /// Makes run() return (thread-safe; also callable from callbacks).
    void stop() {
        stop_.store(true);
        wake();
    }

private:
    struct Watch {
        unsigned events = 0;
        std::shared_ptr<Callback> cb;
    };
    struct Timer {
        TimerId id;
        std::function<void()> fn;
    };

    void wake() noexcept {
        try {
            wake_.send_to(std::string_view("w", 1), wake_ep_);
        } catch (...) {
        }
    }
    void drain_wake() {
        std::byte buf[64];
        while (wake_.try_receive(buf)) {
        }
    }
    bool run_tasks() {
        std::deque<std::function<void()>> t;
        {
            std::lock_guard lock(m_);
            t.swap(tasks_);
        }
        for (auto& f : t) f();
        return !t.empty();
    }
    bool run_timers() {
        bool any = false;
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard lock(m_);
                if (timers_.empty() || timers_.begin()->first > std::chrono::steady_clock::now()) break;
                fn = std::move(timers_.begin()->second.fn);
                timers_.erase(timers_.begin());
            }
            fn();
            any = true;
        }
        return any;
    }

    UdpSocket wake_;
    Endpoint wake_ep_;
    std::mutex m_;
    std::map<detail::native_socket, Watch> watches_;
    std::multimap<std::chrono::steady_clock::time_point, Timer> timers_;
    std::deque<std::function<void()>> tasks_;
    TimerId next_timer_ = 0;
    std::atomic<bool> stop_{false};
};

/// A TCP connection driven by an EventLoop: data arrives through a callback, writes are queued
/// and flushed when the socket is writable. All callbacks run on the loop thread.
class AsyncConnection : public std::enable_shared_from_this<AsyncConnection> {
public:
    using DataHandler = std::function<void(std::span<const std::byte>)>;
    using CloseHandler = std::function<void(const std::string& error)>;  ///< empty error = orderly close

    [[nodiscard]] static std::shared_ptr<AsyncConnection> create(EventLoop& loop, TcpStream stream) {
        return std::shared_ptr<AsyncConnection>(new AsyncConnection(loop, std::move(stream)));
    }

    /// Sets the handlers and starts reading. Call once.
    void start(DataHandler on_data, CloseHandler on_close = {}) {
        on_data_ = std::move(on_data);
        on_close_ = std::move(on_close);
        auto self = shared_from_this();
        loop_.add(stream_.native_handle(), poll_read, [self](unsigned ev) { self->on_events(ev); });
    }

    /// Queues data for sending (thread-safe).
    void write(std::string_view data) { write(std::as_bytes(std::span(data.data(), data.size()))); }
    void write(std::span<const std::byte> data) {
        {
            std::lock_guard lock(m_);
            if (closed_) return;
            out_.insert(out_.end(), data.begin(), data.end());
        }
        auto self = shared_from_this();
        loop_.post([self] { self->flush(); });
    }
    /// Flushes queued data, then closes (thread-safe).
    void close_after_flush() {
        auto self = shared_from_this();
        loop_.post([self] {
            self->closing_ = true;
            self->flush();
        });
    }
    /// Closes immediately (thread-safe).
    void close() {
        auto self = shared_from_this();
        loop_.post([self] { self->finish(""); });
    }
    [[nodiscard]] bool is_open() const {
        std::lock_guard lock(m_);
        return !closed_;
    }
    [[nodiscard]] Endpoint peer() const { return peer_; }

private:
    AsyncConnection(EventLoop& loop, TcpStream s) : loop_(loop), stream_(std::move(s)) {
        try {
            peer_ = stream_.peer_endpoint();
        } catch (const NetworkException&) {
        }
    }

    void on_events(unsigned ev) {
        try {
            if (ev & poll_write) flush();
            if (closed_) return;
            if (ev & (poll_read | poll_hangup | poll_error)) {
                std::byte buf[16384];
                for (int i = 0; i < 16; ++i) {  // bounded so one busy connection cannot starve the loop
                    const auto n = stream_.try_read(buf);
                    if (!n) break;
                    if (*n == 0) {
                        finish("");
                        return;
                    }
                    if (on_data_) on_data_(std::span<const std::byte>(buf, *n));
                    if (closed_) return;
                }
            }
        } catch (const std::exception& e) {
            finish(e.what());
        }
    }
    void flush() {
        if (closed_) return;
        try {
            for (;;) {
                std::vector<std::byte> chunk;
                {
                    std::lock_guard lock(m_);
                    if (out_.empty()) break;
                    chunk.assign(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(std::min<usize>(out_.size(), 1u << 16)));
                }
                const auto n = stream_.try_write(chunk);
                if (!n) {
                    loop_.modify(stream_.native_handle(), poll_read | poll_write);
                    return;
                }
                std::lock_guard lock(m_);
                out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(*n));
            }
            loop_.modify(stream_.native_handle(), poll_read);
            if (closing_) finish("");
        } catch (const std::exception& e) {
            finish(e.what());
        }
    }
    void finish(const std::string& err) {
        {
            std::lock_guard lock(m_);
            if (closed_) return;
            closed_ = true;
        }
        loop_.remove(stream_.native_handle());
        stream_.close();
        auto cb = std::move(on_close_);
        on_close_ = nullptr;
        on_data_ = nullptr;  // breaks reference cycles through captured shared_ptrs
        if (cb) cb(err);
    }

    EventLoop& loop_;
    TcpStream stream_;
    Endpoint peer_;
    DataHandler on_data_;
    CloseHandler on_close_;
    mutable std::mutex m_;
    std::vector<std::byte> out_;
    bool closed_ = false;
    bool closing_ = false;
};

/// Accepts connections on the loop and hands each one over as an AsyncConnection.
class AsyncServer {
public:
    using AcceptHandler = std::function<void(std::shared_ptr<AsyncConnection>)>;

    AsyncServer(EventLoop& loop, const Endpoint& ep, AcceptHandler on_accept) : loop_(loop), listener_(TcpListener::bind(ep)), on_accept_(std::move(on_accept)) {
        loop_.add(listener_.native_handle(), poll_read, [this](unsigned) {
            while (auto c = listener_.try_accept()) on_accept_(AsyncConnection::create(loop_, std::move(c->first)));
        });
    }
    AsyncServer(const AsyncServer&) = delete;
    AsyncServer& operator=(const AsyncServer&) = delete;
    ~AsyncServer() { loop_.remove(listener_.native_handle()); }
    [[nodiscard]] Endpoint local_endpoint() const { return listener_.local_endpoint(); }

private:
    EventLoop& loop_;
    TcpListener listener_;
    AcceptHandler on_accept_;
};

/// Connects without blocking; `done` is invoked on the loop thread with the connected stream or
/// an error message (and an invalid stream).
inline void async_connect(EventLoop& loop, const Endpoint& ep, std::function<void(TcpStream, const std::string& error)> done,
                          Timeout timeout = no_timeout) {
    struct State {
        TcpStream stream;
        std::function<void(TcpStream, const std::string&)> done;
        EventLoop::TimerId timer = 0;
        bool finished = false;
    };
    auto st = std::make_shared<State>();
    st->done = std::move(done);
    try {
        st->stream = TcpStream::begin_connect(ep);
    } catch (const std::exception& e) {
        loop.post([st, msg = std::string(e.what())] { st->done(TcpStream{}, msg); });
        return;
    }
    const auto h = st->stream.native_handle();
    auto finish = [&loop, st, h](const std::string& err) {
        if (st->finished) return;
        st->finished = true;
        loop.remove(h);
        if (st->timer) loop.cancel(st->timer);
        auto d = std::move(st->done);
        if (err.empty()) d(std::move(st->stream), "");
        else d(TcpStream{}, err);
    };
    loop.add(h, poll_write, [st, finish](unsigned) {
        try {
            st->stream.finish_connect();
            finish("");
        } catch (const std::exception& e) {
            finish(e.what());
        }
    });
    if (timeout.count() >= 0) st->timer = loop.run_after(timeout, [finish] { finish("connect timed out"); });
}

}  // namespace flyg::net
