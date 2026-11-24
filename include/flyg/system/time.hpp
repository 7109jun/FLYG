// flyG - clocks, stopwatch, sleeping, timers
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <flyg/system/native.hpp>

namespace flyg::system {

using Clock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;
using Duration = std::chrono::nanoseconds;

/// Monotonic nanoseconds since an unspecified epoch (never goes backwards).
[[nodiscard]] inline u64 monotonic_ns() noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

/// Wall-clock nanoseconds since the Unix epoch (UTC).
[[nodiscard]] inline i64 unix_time_ns() noexcept {
    return static_cast<i64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(WallClock::now().time_since_epoch()).count());
}

[[nodiscard]] inline double unix_time_seconds() noexcept {
    return static_cast<double>(unix_time_ns()) * 1e-9;
}

inline void sleep_for(Duration d) {
    if (d > Duration::zero()) std::this_thread::sleep_for(d);
}

inline void sleep_ms(u32 ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/// Calendar fields of an instant.
struct DateTime {
    int year = 1970;
    int month = 1;   // 1..12
    int day = 1;     // 1..31
    int hour = 0;    // 0..23
    int minute = 0;  // 0..59
    int second = 0;  // 0..60
    int nanosecond = 0;
    bool operator==(const DateTime&) const = default;
};

namespace detail {
// Howard Hinnant's days-from-civil / civil-from-days (proleptic Gregorian calendar).
[[nodiscard]] constexpr i64 days_from_civil(i64 y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}
constexpr void civil_from_days(i64 z, i64& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<i64>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
}  // namespace detail

/// UTC calendar time of a Unix timestamp in nanoseconds.
[[nodiscard]] constexpr DateTime to_datetime(i64 unix_ns) noexcept {
    constexpr i64 ns_per_s = 1'000'000'000;
    i64 secs = unix_ns / ns_per_s;
    i64 ns = unix_ns % ns_per_s;
    if (ns < 0) {
        ns += ns_per_s;
        --secs;
    }
    i64 days = secs / 86400;
    i64 rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    i64 y;
    unsigned m, d;
    detail::civil_from_days(days, y, m, d);
    return DateTime{static_cast<int>(y), static_cast<int>(m), static_cast<int>(d),
                    static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60),
                    static_cast<int>(rem % 60), static_cast<int>(ns)};
}

/// Inverse of to_datetime (UTC). Throws for out-of-range fields.
[[nodiscard]] inline i64 from_datetime(const DateTime& t) {
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour < 0 || t.hour > 23 ||
        t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 60 || t.nanosecond < 0 ||
        t.nanosecond > 999'999'999)
        throw SystemException("from_datetime: field out of range", 0);
    const i64 days = detail::days_from_civil(t.year, static_cast<unsigned>(t.month),
                                             static_cast<unsigned>(t.day));
    // Reject e.g. Feb 30 by round-tripping.
    i64 yy;
    unsigned mm, dd;
    detail::civil_from_days(days, yy, mm, dd);
    if (yy != t.year || static_cast<int>(mm) != t.month || static_cast<int>(dd) != t.day)
        throw SystemException("from_datetime: no such calendar day", 0);
    return ((days * 86400 + t.hour * 3600 + t.minute * 60 + t.second) * 1'000'000'000) + t.nanosecond;
}

/// 0 = Sunday ... 6 = Saturday.
[[nodiscard]] constexpr int day_of_week(const DateTime& t) noexcept {
    const i64 days = detail::days_from_civil(t.year, static_cast<unsigned>(t.month),
                                             static_cast<unsigned>(t.day));
    const i64 w = (days + 4) % 7;  // 1970-01-01 was a Thursday
    return static_cast<int>(w < 0 ? w + 7 : w);
}

[[nodiscard]] constexpr bool is_leap_year(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/// ISO-8601 UTC string, e.g. "2026-11-02T08:47:00.123456789Z" (`fraction_digits` 0..9).
[[nodiscard]] inline std::string format_iso8601(i64 unix_ns, int fraction_digits = 3) {
    if (fraction_digits < 0 || fraction_digits > 9)
        throw SystemException("format_iso8601: fraction_digits must be in 0..9", 0);
    const DateTime t = to_datetime(unix_ns);
    char buf[64];
    int n = std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", t.year, t.month, t.day,
                          t.hour, t.minute, t.second);
    std::string s(buf, static_cast<std::size_t>(n));
    if (fraction_digits > 0) {
        char frac[16];
        std::snprintf(frac, sizeof frac, "%09d", t.nanosecond);
        s += '.';
        s.append(frac, static_cast<std::size_t>(fraction_digits));
    }
    s += 'Z';
    return s;
}

[[nodiscard]] inline std::string now_iso8601(int fraction_digits = 3) {
    return format_iso8601(unix_time_ns(), fraction_digits);
}

/// Parses "YYYY-MM-DD[T| ]hh:mm:ss[.fraction][Z|+hh:mm|-hh:mm]" into Unix nanoseconds (UTC).
[[nodiscard]] inline i64 parse_iso8601(std::string_view s) {
    auto fail = [&]() -> i64 {
        throw SystemException("parse_iso8601: invalid timestamp '" + std::string(s) + "'", 0);
    };
    std::size_t i = 0;
    auto num = [&](int digits, int& out) {
        if (i + static_cast<std::size_t>(digits) > s.size()) return false;
        int v = 0;
        for (int k = 0; k < digits; ++k) {
            const char c = s[i + static_cast<std::size_t>(k)];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        i += static_cast<std::size_t>(digits);
        out = v;
        return true;
    };
    auto lit = [&](char c) {
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    };
    DateTime t;
    if (!num(4, t.year) || !lit('-') || !num(2, t.month) || !lit('-') || !num(2, t.day)) return fail();
    if (i < s.size()) {
        if (!lit('T') && !lit(' ')) return fail();
        if (!num(2, t.hour) || !lit(':') || !num(2, t.minute) || !lit(':') || !num(2, t.second))
            return fail();
        if (lit('.')) {
            int digits = 0;
            int ns = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                if (digits < 9) {
                    ns = ns * 10 + (s[i] - '0');
                    ++digits;
                }
                ++i;
            }
            if (digits == 0) return fail();
            for (; digits < 9; ++digits) ns *= 10;
            t.nanosecond = ns;
        }
    }
    i64 offset_s = 0;
    if (i < s.size()) {
        if (lit('Z')) {
        } else if (s[i] == '+' || s[i] == '-') {
            const int sign = s[i] == '-' ? -1 : 1;
            ++i;
            int oh = 0, om = 0;
            if (!num(2, oh)) return fail();
            lit(':');
            if (i < s.size() && !num(2, om)) return fail();
            offset_s = sign * (oh * 3600 + om * 60);
        } else {
            return fail();
        }
        if (i != s.size()) return fail();
    }
    return from_datetime(t) - offset_s * 1'000'000'000;
}

/// Offset of the local time zone from UTC at the given instant, in seconds (east positive).
[[nodiscard]] inline i64 local_utc_offset_seconds(i64 unix_ns = unix_time_ns()) {
    std::time_t tt = static_cast<std::time_t>(unix_ns / 1'000'000'000);
    std::tm local{};
#if FLYG_PLATFORM_WINDOWS
    if (::localtime_s(&local, &tt) != 0) throw SystemException("localtime_s failed", 0);
#else
    if (!::localtime_r(&tt, &local)) throw SystemException::from_errno("localtime_r failed");
#endif
    // Interpret the local calendar fields as if they were UTC and compare.
    const DateTime d{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                     local.tm_hour,        local.tm_min,     local.tm_sec, 0};
    return (from_datetime(d) / 1'000'000'000) - static_cast<i64>(tt);
}

/// Measures elapsed monotonic time.
class Stopwatch {
public:
    Stopwatch() noexcept : start_(Clock::now()) {}
    void restart() noexcept {
        start_ = Clock::now();
        stopped_ = false;
    }
    void stop() noexcept {
        if (!stopped_) {
            end_ = Clock::now();
            stopped_ = true;
        }
    }
    [[nodiscard]] Duration elapsed() const noexcept {
        return std::chrono::duration_cast<Duration>((stopped_ ? end_ : Clock::now()) - start_);
    }
    [[nodiscard]] double seconds() const noexcept { return static_cast<double>(elapsed().count()) * 1e-9; }
    [[nodiscard]] double milliseconds() const noexcept { return static_cast<double>(elapsed().count()) * 1e-6; }

private:
    Clock::time_point start_;
    Clock::time_point end_{};
    bool stopped_ = false;
};

/// Runs callbacks on one background thread, once or periodically. Callbacks must not block for
/// long; an exception escaping a callback is swallowed and the timer is cancelled if periodic.
class TimerQueue {
public:
    using Callback = std::function<void()>;
    using TimerId = u64;

    TimerQueue() : worker_([this] { run(); }) {}
    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;
    ~TimerQueue() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    /// Calls `cb` once after `delay`.
    TimerId schedule_after(Duration delay, Callback cb) {
        return add(Clock::now() + delay, Duration::zero(), std::move(cb));
    }
    /// Calls `cb` every `period`, first after `period`.
    TimerId schedule_every(Duration period, Callback cb) {
        if (period <= Duration::zero()) throw SystemException("schedule_every: period must be > 0", 0);
        return add(Clock::now() + period, period, std::move(cb));
    }
    /// Cancels a timer; returns true if it was still pending.
    bool cancel(TimerId id) {
        std::lock_guard lock(mutex_);
        if (id != 0 && id == running_) {  // callback in flight: just prevent rescheduling
            cancel_running_ = true;
            return true;
        }
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->second.id == id) {
                queue_.erase(it);
                cv_.notify_all();
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock(mutex_);
        return queue_.size() + (running_ != 0 ? 1 : 0);
    }

private:
    struct Entry {
        TimerId id;
        Duration period;
        Callback cb;
    };

    TimerId add(Clock::time_point when, Duration period, Callback cb) {
        if (!cb) throw SystemException("timer callback is empty", 0);
        std::lock_guard lock(mutex_);
        const TimerId id = ++next_id_;
        queue_.emplace(when, Entry{id, period, std::move(cb)});
        cv_.notify_all();
        return id;
    }

    void run() {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            if (queue_.empty()) {
                cv_.wait(lock);
                continue;
            }
            const auto due = queue_.begin()->first;
            if (Clock::now() < due) {
                cv_.wait_until(lock, due);
                continue;
            }
            auto node = queue_.extract(queue_.begin());
            Entry e = std::move(node.mapped());
            running_ = e.id;
            lock.unlock();
            bool ok = true;
            try {
                e.cb();
            } catch (...) {
                ok = false;
            }
            lock.lock();
            running_ = 0;
            const bool cancelled = cancel_running_;
            cancel_running_ = false;
            if (ok && !cancelled && e.period > Duration::zero() && !stop_) {
                // Schedule relative to the due time to avoid drift, but never into the past.
                auto next = due + e.period;
                if (next < Clock::now()) next = Clock::now();
                queue_.emplace(next, std::move(e));
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::multimap<Clock::time_point, Entry> queue_;
    TimerId next_id_ = 0;
    TimerId running_ = 0;
    bool cancel_running_ = false;
    bool stop_ = false;
    std::thread worker_;  // declared last: starts after the other members exist
};

}  // namespace flyg::system
