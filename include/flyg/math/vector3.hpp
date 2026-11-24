// flyG - Vector3
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <compare>
#include <format>
#include <string>

#include <flyg/math/scalar.hpp>

namespace flyg::math {

template <class T>
struct Vector3 {
    using value_type = T;
    static constexpr usize dimension = 3;

    T x{}, y{}, z{};

    constexpr Vector3() noexcept = default;
    constexpr explicit Vector3(T s) noexcept : x(s), y(s), z(s) {}
    constexpr Vector3(T x_, T y_, T z_) noexcept : x(x_), y(y_), z(z_) {}
    template <class U>
    constexpr explicit Vector3(const Vector3<U>& o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)) {}

    static constexpr Vector3 zero() noexcept { return {}; }
    static constexpr Vector3 one() noexcept { return Vector3(T(1)); }
    static constexpr Vector3 unit_x() noexcept { return {T(1), T(0), T(0)}; }
    static constexpr Vector3 unit_y() noexcept { return {T(0), T(1), T(0)}; }
    static constexpr Vector3 unit_z() noexcept { return {T(0), T(0), T(1)}; }

    constexpr T& operator[](usize i) {
        if (i > 2) throw MathException("Vector3 index out of range");
        return i == 0 ? x : (i == 1 ? y : z);
    }
    constexpr const T& operator[](usize i) const {
        if (i > 2) throw MathException("Vector3 index out of range");
        return i == 0 ? x : (i == 1 ? y : z);
    }
    constexpr T* data() noexcept { return &x; }
    constexpr const T* data() const noexcept { return &x; }

    constexpr Vector3& operator+=(const Vector3& o) noexcept { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vector3& operator-=(const Vector3& o) noexcept { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vector3& operator*=(const Vector3& o) noexcept { x *= o.x; y *= o.y; z *= o.z; return *this; }
    constexpr Vector3& operator/=(const Vector3& o) noexcept { x /= o.x; y /= o.y; z /= o.z; return *this; }
    constexpr Vector3& operator*=(T s) noexcept { x *= s; y *= s; z *= s; return *this; }
    constexpr Vector3& operator/=(T s) noexcept { x /= s; y /= s; z /= s; return *this; }

    friend constexpr Vector3 operator+(Vector3 a, const Vector3& b) noexcept { return a += b; }
    friend constexpr Vector3 operator-(Vector3 a, const Vector3& b) noexcept { return a -= b; }
    friend constexpr Vector3 operator*(Vector3 a, const Vector3& b) noexcept { return a *= b; }
    friend constexpr Vector3 operator/(Vector3 a, const Vector3& b) noexcept { return a /= b; }
    friend constexpr Vector3 operator*(Vector3 a, T s) noexcept { return a *= s; }
    friend constexpr Vector3 operator*(T s, Vector3 a) noexcept { return a *= s; }
    friend constexpr Vector3 operator/(Vector3 a, T s) noexcept { return a /= s; }
    friend constexpr Vector3 operator-(const Vector3& a) noexcept { return {-a.x, -a.y, -a.z}; }
    friend constexpr bool operator==(const Vector3&, const Vector3&) noexcept = default;
};

using Vector3f = Vector3<float>;
using Vector3d = Vector3<double>;
using Vector3i = Vector3<core::i32>;

template <class T> [[nodiscard]] constexpr T dot(const Vector3<T>& a, const Vector3<T>& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
template <class T> [[nodiscard]] constexpr Vector3<T> cross(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
template <class T> [[nodiscard]] constexpr T length_squared(const Vector3<T>& a) noexcept { return dot(a, a); }
template <Real T> [[nodiscard]] inline T length(const Vector3<T>& a) noexcept { return std::sqrt(dot(a, a)); }
template <Real T> [[nodiscard]] inline T distance(const Vector3<T>& a, const Vector3<T>& b) noexcept { return length(b - a); }
template <Real T> [[nodiscard]] constexpr T distance_squared(const Vector3<T>& a, const Vector3<T>& b) noexcept { return length_squared(b - a); }

template <Real T>
[[nodiscard]] inline Vector3<T> normalize(const Vector3<T>& a) {
    const T l = length(a);
    if (!(l > std::numeric_limits<T>::min())) throw MathException("normalize: zero-length vector");
    return a / l;
}
template <Real T>
[[nodiscard]] inline Vector3<T> normalize_or(const Vector3<T>& a, const Vector3<T>& fallback) noexcept {
    const T l = length(a);
    return l > std::numeric_limits<T>::min() ? a / l : fallback;
}

template <class T> [[nodiscard]] constexpr Vector3<T> min(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
template <class T> [[nodiscard]] constexpr Vector3<T> max(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
template <class T> [[nodiscard]] constexpr Vector3<T> clamp(const Vector3<T>& v, const Vector3<T>& lo, const Vector3<T>& hi) noexcept { return min(max(v, lo), hi); }
template <class T> [[nodiscard]] inline Vector3<T> abs(const Vector3<T>& a) noexcept {
    return {a.x < T(0) ? -a.x : a.x, a.y < T(0) ? -a.y : a.y, a.z < T(0) ? -a.z : a.z};
}
template <class T> [[nodiscard]] constexpr T max_component(const Vector3<T>& a) noexcept { return a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z); }
template <class T> [[nodiscard]] constexpr T min_component(const Vector3<T>& a) noexcept { return a.x < a.y ? (a.x < a.z ? a.x : a.z) : (a.y < a.z ? a.y : a.z); }
template <Real T> [[nodiscard]] constexpr Vector3<T> lerp(const Vector3<T>& a, const Vector3<T>& b, T t) noexcept { return a + (b - a) * t; }
template <Real T> [[nodiscard]] constexpr Vector3<T> reflect(const Vector3<T>& v, const Vector3<T>& n) noexcept { return v - n * (T(2) * dot(v, n)); }
/// Refraction of unit vector `v` through a surface with unit normal `n` and index ratio `eta`.
/// Returns the zero vector on total internal reflection.
template <Real T> [[nodiscard]] inline Vector3<T> refract(const Vector3<T>& v, const Vector3<T>& n, T eta) noexcept {
    const T d = dot(n, v);
    const T k = T(1) - eta * eta * (T(1) - d * d);
    if (k < T(0)) return {};
    return v * eta - n * (eta * d + std::sqrt(k));
}
template <Real T> [[nodiscard]] constexpr Vector3<T> project(const Vector3<T>& v, const Vector3<T>& onto) {
    const T d = dot(onto, onto);
    if (d == T(0)) throw MathException("project: onto vector is zero");
    return onto * (dot(v, onto) / d);
}
/// Unsigned angle between two vectors in [0, pi].
template <Real T> [[nodiscard]] inline T angle_between(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return std::atan2(length(cross(a, b)), dot(a, b));
}
/// Any unit vector perpendicular to the unit vector `n`.
template <Real T> [[nodiscard]] inline Vector3<T> any_perpendicular(const Vector3<T>& n) noexcept {
    const Vector3<T> axis = std::fabs(n.x) < T(0.9) ? Vector3<T>::unit_x() : Vector3<T>::unit_y();
    return normalize_or(cross(n, axis), Vector3<T>::unit_z());
}
template <Real T> [[nodiscard]] inline bool approx_equal(const Vector3<T>& a, const Vector3<T>& b, T rel = default_rel_tol<T>, T abs_ = default_abs_tol<T>) noexcept {
    return approx_equal(a.x, b.x, rel, abs_) && approx_equal(a.y, b.y, rel, abs_) && approx_equal(a.z, b.z, rel, abs_);
}
template <class T> [[nodiscard]] inline std::string to_string(const Vector3<T>& v) { return std::format("({}, {}, {})", v.x, v.y, v.z); }

}  // namespace flyg::math

template <class T>
struct std::formatter<flyg::math::Vector3<T>> : std::formatter<std::string> {
    auto format(const flyg::math::Vector3<T>& v, std::format_context& ctx) const {
        return std::formatter<std::string>::format(flyg::math::to_string(v), ctx);
    }
};
