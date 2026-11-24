// flyG - scalar math: constants, arithmetic, trigonometry, logarithms, float comparison
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <bit>
#include <cmath>
#include <concepts>
#include <limits>
#include <numbers>
#include <type_traits>
#include <utility>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::math {

using core::i32;
using core::i64;
using core::u32;
using core::u64;
using core::usize;

template <class T>
concept Real = std::floating_point<T>;

// ---- constants ------------------------------------------------------------
template <Real T> inline constexpr T pi_v = std::numbers::pi_v<T>;
template <Real T> inline constexpr T tau_v = std::numbers::pi_v<T> * T(2);
template <Real T> inline constexpr T half_pi_v = std::numbers::pi_v<T> / T(2);
template <Real T> inline constexpr T euler_v = std::numbers::e_v<T>;
template <Real T> inline constexpr T sqrt2_v = std::numbers::sqrt2_v<T>;
template <Real T> inline constexpr T epsilon_v = std::numeric_limits<T>::epsilon();
template <Real T> inline constexpr T infinity_v = std::numeric_limits<T>::infinity();

inline constexpr double pi = std::numbers::pi;
inline constexpr double tau = 2.0 * std::numbers::pi;
inline constexpr double half_pi = std::numbers::pi / 2.0;
inline constexpr double euler = std::numbers::e;

// ---- basic arithmetic -----------------------------------------------------
template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (hi < v ? hi : v);
}

template <Real T>
[[nodiscard]] constexpr T saturate(T v) noexcept { return clamp(v, T(0), T(1)); }

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T square(T v) noexcept { return v * v; }

template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T cube(T v) noexcept { return v * v * v; }

/// -1, 0 or +1.
template <class T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] constexpr T sign(T v) noexcept { return static_cast<T>((T(0) < v) - (v < T(0))); }

template <Real T>
[[nodiscard]] constexpr T lerp(T a, T b, std::type_identity_t<T> t) noexcept { return a + (b - a) * t; }

/// Position of `v` between `a` and `b` (not clamped). Throws when a == b.
template <Real T>
[[nodiscard]] constexpr T inverse_lerp(T a, T b, T v) {
    if (a == b) throw MathException("inverse_lerp: degenerate range");
    return (v - a) / (b - a);
}

template <Real T>
[[nodiscard]] constexpr T remap(T v, T in_lo, T in_hi, T out_lo, T out_hi) {
    return lerp(out_lo, out_hi, inverse_lerp(in_lo, in_hi, v));
}

template <Real T>
[[nodiscard]] constexpr T step(T edge, T x) noexcept { return x < edge ? T(0) : T(1); }

template <Real T>
[[nodiscard]] constexpr T smoothstep(T e0, T e1, T x) {
    const T t = saturate(inverse_lerp(e0, e1, x));
    return t * t * (T(3) - T(2) * t);
}

template <Real T>
[[nodiscard]] constexpr T smootherstep(T e0, T e1, T x) {
    const T t = saturate(inverse_lerp(e0, e1, x));
    return t * t * t * (t * (t * T(6) - T(15)) + T(10));
}

template <Real T>
[[nodiscard]] constexpr T radians(T deg) noexcept { return deg * (pi_v<T> / T(180)); }

template <Real T>
[[nodiscard]] constexpr T degrees(T rad) noexcept { return rad * (T(180) / pi_v<T>); }

// ---- classification -------------------------------------------------------
template <Real T>
[[nodiscard]] constexpr bool is_nan(T v) noexcept { return v != v; }

template <Real T>
[[nodiscard]] constexpr bool is_finite(T v) noexcept {
    return v == v && v != std::numeric_limits<T>::infinity() && v != -std::numeric_limits<T>::infinity();
}

// ---- thin wrappers over <cmath> (keep the argument type) ------------------
template <Real T> [[nodiscard]] inline T abs(T v) noexcept { return std::fabs(v); }
template <Real T> [[nodiscard]] inline T sqrt(T v) noexcept { return std::sqrt(v); }
template <Real T> [[nodiscard]] inline T cbrt(T v) noexcept { return std::cbrt(v); }
template <Real T> [[nodiscard]] inline T hypot(T a, T b) noexcept { return std::hypot(a, b); }
template <Real T> [[nodiscard]] inline T floor(T v) noexcept { return std::floor(v); }
template <Real T> [[nodiscard]] inline T ceil(T v) noexcept { return std::ceil(v); }
template <Real T> [[nodiscard]] inline T round(T v) noexcept { return std::round(v); }
template <Real T> [[nodiscard]] inline T trunc(T v) noexcept { return std::trunc(v); }
template <Real T> [[nodiscard]] inline T fract(T v) noexcept { return v - std::floor(v); }
template <Real T> [[nodiscard]] inline T fmod(T a, T b) noexcept { return std::fmod(a, b); }

/// sqrt that maps tiny negative rounding errors to 0 instead of NaN.
template <Real T>
[[nodiscard]] inline T safe_sqrt(T v) noexcept { return v <= T(0) ? T(0) : std::sqrt(v); }

template <Real T> [[nodiscard]] inline T sin(T v) noexcept { return std::sin(v); }
template <Real T> [[nodiscard]] inline T cos(T v) noexcept { return std::cos(v); }
template <Real T> [[nodiscard]] inline T tan(T v) noexcept { return std::tan(v); }
template <Real T> [[nodiscard]] inline T atan(T v) noexcept { return std::atan(v); }
template <Real T> [[nodiscard]] inline T atan2(T y, T x) noexcept { return std::atan2(y, x); }
template <Real T> [[nodiscard]] inline T sinh(T v) noexcept { return std::sinh(v); }
template <Real T> [[nodiscard]] inline T cosh(T v) noexcept { return std::cosh(v); }
template <Real T> [[nodiscard]] inline T tanh(T v) noexcept { return std::tanh(v); }

/// asin/acos clamp their argument to [-1, 1] so rounding noise never yields NaN.
template <Real T> [[nodiscard]] inline T asin(T v) noexcept { return std::asin(clamp(v, T(-1), T(1))); }
template <Real T> [[nodiscard]] inline T acos(T v) noexcept { return std::acos(clamp(v, T(-1), T(1))); }

template <Real T>
[[nodiscard]] inline std::pair<T, T> sincos(T v) noexcept { return {std::sin(v), std::cos(v)}; }

template <Real T> [[nodiscard]] inline T exp(T v) noexcept { return std::exp(v); }
template <Real T> [[nodiscard]] inline T exp2(T v) noexcept { return std::exp2(v); }
template <Real T> [[nodiscard]] inline T expm1(T v) noexcept { return std::expm1(v); }
template <Real T> [[nodiscard]] inline T log(T v) noexcept { return std::log(v); }
template <Real T> [[nodiscard]] inline T log2(T v) noexcept { return std::log2(v); }
template <Real T> [[nodiscard]] inline T log10(T v) noexcept { return std::log10(v); }
template <Real T> [[nodiscard]] inline T log1p(T v) noexcept { return std::log1p(v); }
template <Real T> [[nodiscard]] inline T pow(T b, T x) noexcept { return std::pow(b, x); }

/// Integer power by repeated squaring (exact for exactly representable intermediate results).
template <Real T>
[[nodiscard]] constexpr T powi(T base, i32 exponent) noexcept {
    u32 n = exponent < 0 ? 0u - static_cast<u32>(exponent) : static_cast<u32>(exponent);
    T result = T(1);
    while (n != 0) {
        if (n & 1u) result *= base;
        base *= base;
        n >>= 1;
    }
    return exponent < 0 ? T(1) / result : result;
}

/// Wraps an angle into (-pi, pi].
template <Real T>
[[nodiscard]] inline T wrap_pi(T a) noexcept {
    a = std::fmod(a + pi_v<T>, tau_v<T>);
    if (a <= T(0)) a += tau_v<T>;
    return a - pi_v<T>;
}

/// Wraps an angle into [0, 2pi).
template <Real T>
[[nodiscard]] inline T wrap_two_pi(T a) noexcept {
    a = std::fmod(a, tau_v<T>);
    return a < T(0) ? a + tau_v<T> : a;
}

// ---- floating point comparison / error management ------------------------
template <Real T> inline constexpr T default_rel_tol = T(64) * std::numeric_limits<T>::epsilon();
template <Real T> inline constexpr T default_abs_tol = T(16) * std::numeric_limits<T>::epsilon();

/// |a-b| <= max(abs_tol, rel_tol * max(|a|,|b|)). NaN is never equal; equal infinities are.
template <Real T>
[[nodiscard]] constexpr bool approx_equal(T a, T b, T rel_tol = default_rel_tol<T>,
                                          T abs_tol = default_abs_tol<T>) noexcept {
    if (a == b) return true;
    if (is_nan(a) || is_nan(b)) return false;
    const T diff = a < b ? b - a : a - b;
    if (!is_finite(diff)) return false;
    const T aa = a < T(0) ? -a : a;
    const T ab = b < T(0) ? -b : b;
    const T scale = aa < ab ? ab : aa;
    const T tol = rel_tol * scale;
    return diff <= (tol > abs_tol ? tol : abs_tol);
}

/// Number of representable values between a and b (0 when equal, max for NaN).
template <Real T>
[[nodiscard]] constexpr u64 ulp_distance(T a, T b) noexcept {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
                  "ulp_distance supports float and double");
    using U = std::conditional_t<std::is_same_v<T, float>, u32, u64>;
    constexpr int bits = static_cast<int>(sizeof(U) * 8);
    constexpr U sign_bit = U{1} << (bits - 1);
    if (is_nan(a) || is_nan(b)) return std::numeric_limits<u64>::max();
    if (a == b) return 0;  // also makes +0 and -0 equal
    auto key = [](T x) constexpr {
        const U u = std::bit_cast<U>(x);
        return (u & sign_bit) ? static_cast<U>(~u) : static_cast<U>(u | sign_bit);
    };
    const U ka = key(a);
    const U kb = key(b);
    return static_cast<u64>(ka > kb ? ka - kb : kb - ka);
}

/// |approx - exact| / |exact| (absolute error when exact == 0).
template <Real T>
[[nodiscard]] inline T relative_error(T approx, T exact) noexcept {
    const T d = std::fabs(approx - exact);
    return exact == T(0) ? d : d / std::fabs(exact);
}

/// Next representable value after `v` towards `toward`.
template <Real T>
[[nodiscard]] inline T next_after(T v, T toward) noexcept { return std::nextafter(v, toward); }

}  // namespace flyg::math
