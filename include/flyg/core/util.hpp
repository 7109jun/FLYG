// flyG - common utilities
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <bit>
#include <concepts>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::core {

/// Inherit to make a type non-copyable but movable.
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&) = default;
    NonCopyable& operator=(NonCopyable&&) = default;
};

/// Runs a callable when leaving scope (RAII). Can be dismissed.
template <std::invocable F>
class ScopeExit {
public:
    explicit ScopeExit(F f) noexcept(std::is_nothrow_move_constructible_v<F>) : f_(std::move(f)) {}
    ScopeExit(ScopeExit&& o) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(o.f_)), active_(std::exchange(o.active_, false)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;
    ~ScopeExit() {
        if (active_) f_();
    }
    void dismiss() noexcept { active_ = false; }

private:
    F f_;
    bool active_ = true;
};

template <std::invocable F>
[[nodiscard]] ScopeExit<F> scope_exit(F f) {
    return ScopeExit<F>(std::move(f));
}

/// Throws `Exception` carrying `code` when `cond` is false.
inline void ensure(bool cond, std::string_view message,
                   ErrorCode code = ErrorCode::invalid_state,
                   std::source_location loc = std::source_location::current()) {
    if (!cond) FLYG_UNLIKELY {
        throw Exception(std::string(message), code, loc);
    }
}

/// Checked integral conversion; throws Exception(overflow) when the value does not fit.
template <std::integral To, std::integral From>
[[nodiscard]] constexpr To checked_cast(From v) {
    if (!std::in_range<To>(v)) throw Exception("integer conversion overflow", ErrorCode::overflow);
    return static_cast<To>(v);
}

/// Checked unsigned addition / multiplication (throws on overflow).
[[nodiscard]] constexpr usize checked_add(usize a, usize b) {
    if (a > std::numeric_limits<usize>::max() - b)
        throw Exception("size addition overflow", ErrorCode::overflow);
    return a + b;
}
[[nodiscard]] constexpr usize checked_mul(usize a, usize b) {
    if (a != 0 && b > std::numeric_limits<usize>::max() / a)
        throw Exception("size multiplication overflow", ErrorCode::overflow);
    return a * b;
}

/// boost-style hash combination.
constexpr void hash_combine(usize& seed, usize h) noexcept {
    seed ^= h + usize{0x9e3779b97f4a7c15ULL} + (seed << 6) + (seed >> 2);
}

template <class T>
constexpr void hash_append(usize& seed, const T& v) {
    hash_combine(seed, std::hash<T>{}(v));
}

/// Hexadecimal formatting of a byte range, e.g. "de ad be ef".
[[nodiscard]] inline std::string to_hex(ByteSpan bytes, char sep = ' ') {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (usize i = 0; i < bytes.size(); ++i) {
        if (i != 0 && sep != '\0') out.push_back(sep);
        const auto b = std::to_integer<unsigned>(bytes[i]);
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

/// "1.50 MiB" style size formatting.
[[nodiscard]] inline std::string format_bytes(u64 n) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(n);
    usize u = 0;
    while (v >= 1024.0 && u + 1 < std::size(units)) {
        v /= 1024.0;
        ++u;
    }
    return u == 0 ? std::format("{} B", n) : std::format("{:.2f} {}", v, units[u]);
}

}  // namespace flyg::core
