// flyG - Vector2
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <compare>
#include <format>
#include <string>

#include <flyg/math/scalar.hpp>

namespace flyg::math {

template <class T>
struct Vector2 {
    using value_type = T;
    static constexpr usize dimension = 2;

    T x{}, y{};

    constexpr Vector2() noexcept = default;
    constexpr explicit Vector2(T s) noexcept : x(s), y(s) {}
    constexpr Vector2(T x_, T y_) noexcept : x(x_), y(y_) {}
    template <class U>
    constexpr explicit Vector2(const Vector2<U>& o) noexcept : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)) {}

    static constexpr Vector2 zero() noexcept { return {}; }
    static constexpr Vector2 one() noexcept { return Vector2(T(1)); }
    static constexpr Vector2 unit_x() noexcept { return {T(1), T(0)}; }
    static constexpr Vector2 unit_y() noexcept { return {T(0), T(1)}; }

    constexpr T& operator[](usize i) {
        if (i > 1) throw MathException("Vector2 index out of range");
        return i == 0 ? x : y;
    }
    constexpr const T& operator[](usize i) const {
        if (i > 1) throw MathException("Vector2 index out of range");
        return i == 0 ? x : y;
    }
    constexpr T* data() noexcept { return &x; }
    constexpr const T* data() const noexcept { return &x; }

    constexpr Vector2& operator+=(const Vector2& o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr Vector2& operator-=(const Vector2& o) noexcept { x -= o.x; y -= o.y; return *this; }
    constexpr Vector2& operator*=(const Vector2& o) noexcept { x *= o.x; y *= o.y; return *this; }
    constexpr Vector2& operator/=(const Vector2& o) noexcept { x /= o.x; y /= o.y; return *this; }
    constexpr Vector2& operator*=(T s) noexcept { x *= s; y *= s; return *this; }
    constexpr Vector2& operator/=(T s) noexcept { x /= s; y /= s; return *this; }

    friend constexpr Vector2 operator+(Vector2 a, const Vector2& b) noexcept { return a += b; }
    friend constexpr Vector2 operator-(Vector2 a, const Vector2& b) noexcept { return a -= b; }
    friend constexpr Vector2 operator*(Vector2 a, const Vector2& b) noexcept { return a *= b; }
    friend constexpr Vector2 operator/(Vector2 a, const Vector2& b) noexcept { return a /= b; }
    friend constexpr Vector2 operator*(Vector2 a, T s) noexcept { return a *= s; }
    friend constexpr Vector2 operator*(T s, Vector2 a) noexcept { return a *= s; }
    friend constexpr Vector2 operator/(Vector2 a, T s) noexcept { return a /= s; }
    friend constexpr Vector2 operator-(const Vector2& a) noexcept { return {-a.x, -a.y}; }
    friend constexpr bool operator==(const Vector2&, const Vector2&) noexcept = default;
};

using Vector2f = Vector2<float>;
using Vector2d = Vector2<double>;
using Vector2i = Vector2<core::i32>;

template <class T> [[nodiscard]] constexpr T dot(const Vector2<T>& a, const Vector2<T>& b) noexcept { return a.x * b.x + a.y * b.y; }
/// z component of the 3D cross product of (a,0) and (b,0).
template <class T> [[nodiscard]] constexpr T cross(const Vector2<T>& a, const Vector2<T>& b) noexcept { return a.x * b.y - a.y * b.x; }
template <class T> [[nodiscard]] constexpr T length_squared(const Vector2<T>& a) noexcept { return dot(a, a); }
template <Real T> [[nodiscard]] inline T length(const Vector2<T>& a) noexcept { return std::hypot(a.x, a.y); }
template <Real T> [[nodiscard]] inline T distance(const Vector2<T>& a, const Vector2<T>& b) noexcept { return length(b - a); }
template <Real T> [[nodiscard]] constexpr T distance_squared(const Vector2<T>& a, const Vector2<T>& b) noexcept { return length_squared(b - a); }

/// Unit vector; throws MathException for a (near-)zero vector.
template <Real T>
[[nodiscard]] inline Vector2<T> normalize(const Vector2<T>& a) {
    const T l = length(a);
    if (!(l > std::numeric_limits<T>::min())) throw MathException("normalize: zero-length vector");
    return a / l;
}
/// Unit vector, or `fallback` for a (near-)zero vector.
template <Real T>
[[nodiscard]] inline Vector2<T> normalize_or(const Vector2<T>& a, const Vector2<T>& fallback) noexcept {
    const T l = length(a);
    return l > std::numeric_limits<T>::min() ? a / l : fallback;
}

template <class T> [[nodiscard]] constexpr Vector2<T> perpendicular(const Vector2<T>& a) noexcept { return {-a.y, a.x}; }
template <class T> [[nodiscard]] constexpr Vector2<T> min(const Vector2<T>& a, const Vector2<T>& b) noexcept { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}; }
template <class T> [[nodiscard]] constexpr Vector2<T> max(const Vector2<T>& a, const Vector2<T>& b) noexcept { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}; }
template <class T> [[nodiscard]] constexpr Vector2<T> clamp(const Vector2<T>& v, const Vector2<T>& lo, const Vector2<T>& hi) noexcept { return min(max(v, lo), hi); }
template <class T> [[nodiscard]] inline Vector2<T> abs(const Vector2<T>& a) noexcept { return {a.x < T(0) ? -a.x : a.x, a.y < T(0) ? -a.y : a.y}; }
template <Real T> [[nodiscard]] constexpr Vector2<T> lerp(const Vector2<T>& a, const Vector2<T>& b, T t) noexcept { return a + (b - a) * t; }
template <Real T> [[nodiscard]] constexpr Vector2<T> reflect(const Vector2<T>& v, const Vector2<T>& n) noexcept { return v - n * (T(2) * dot(v, n)); }
template <Real T> [[nodiscard]] constexpr Vector2<T> project(const Vector2<T>& v, const Vector2<T>& onto) {
    const T d = dot(onto, onto);
    if (d == T(0)) throw MathException("project: onto vector is zero");
    return onto * (dot(v, onto) / d);
}
/// Angle (radians) of the vector from +x, in (-pi, pi].
template <Real T> [[nodiscard]] inline T angle(const Vector2<T>& a) noexcept { return std::atan2(a.y, a.x); }
/// Signed angle from a to b.
template <Real T> [[nodiscard]] inline T angle_between(const Vector2<T>& a, const Vector2<T>& b) noexcept { return std::atan2(cross(a, b), dot(a, b)); }
template <Real T> [[nodiscard]] inline Vector2<T> rotate(const Vector2<T>& v, T radians_) noexcept {
    const T c = std::cos(radians_), s = std::sin(radians_);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}
template <Real T> [[nodiscard]] inline bool approx_equal(const Vector2<T>& a, const Vector2<T>& b, T rel = default_rel_tol<T>, T abs_ = default_abs_tol<T>) noexcept {
    return approx_equal(a.x, b.x, rel, abs_) && approx_equal(a.y, b.y, rel, abs_);
}
template <class T> [[nodiscard]] inline std::string to_string(const Vector2<T>& v) { return std::format("({}, {})", v.x, v.y); }

}  // namespace flyg::math

template <class T>
struct std::formatter<flyg::math::Vector2<T>> : std::formatter<std::string> {
    auto format(const flyg::math::Vector2<T>& v, std::format_context& ctx) const {
        return std::formatter<std::string>::format(flyg::math::to_string(v), ctx);
    }
};
