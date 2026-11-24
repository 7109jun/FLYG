// flyG - Vector4
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <compare>
#include <format>
#include <string>

#include <flyg/math/scalar.hpp>
#include <flyg/math/vector3.hpp>

namespace flyg::math {

template <class T>
struct Vector4 {
    using value_type = T;
    static constexpr usize dimension = 4;

    T x{}, y{}, z{}, w{};

    constexpr Vector4() noexcept = default;
    constexpr explicit Vector4(T s) noexcept : x(s), y(s), z(s), w(s) {}
    constexpr Vector4(T x_, T y_, T z_, T w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vector4(const Vector3<T>& v, T w_) noexcept : x(v.x), y(v.y), z(v.z), w(w_) {}
    template <class U>
    constexpr explicit Vector4(const Vector4<U>& o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)), w(static_cast<T>(o.w)) {}

    static constexpr Vector4 zero() noexcept { return {}; }
    static constexpr Vector4 one() noexcept { return Vector4(T(1)); }

    [[nodiscard]] constexpr Vector3<T> xyz() const noexcept { return {x, y, z}; }

    constexpr T& operator[](usize i) {
        if (i > 3) throw MathException("Vector4 index out of range");
        return (&x)[i];
    }
    constexpr const T& operator[](usize i) const {
        if (i > 3) throw MathException("Vector4 index out of range");
        return (&x)[i];
    }
    constexpr T* data() noexcept { return &x; }
    constexpr const T* data() const noexcept { return &x; }

    constexpr Vector4& operator+=(const Vector4& o) noexcept { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
    constexpr Vector4& operator-=(const Vector4& o) noexcept { x -= o.x; y -= o.y; z -= o.z; w -= o.w; return *this; }
    constexpr Vector4& operator*=(const Vector4& o) noexcept { x *= o.x; y *= o.y; z *= o.z; w *= o.w; return *this; }
    constexpr Vector4& operator/=(const Vector4& o) noexcept { x /= o.x; y /= o.y; z /= o.z; w /= o.w; return *this; }
    constexpr Vector4& operator*=(T s) noexcept { x *= s; y *= s; z *= s; w *= s; return *this; }
    constexpr Vector4& operator/=(T s) noexcept { x /= s; y /= s; z /= s; w /= s; return *this; }

    friend constexpr Vector4 operator+(Vector4 a, const Vector4& b) noexcept { return a += b; }
    friend constexpr Vector4 operator-(Vector4 a, const Vector4& b) noexcept { return a -= b; }
    friend constexpr Vector4 operator*(Vector4 a, const Vector4& b) noexcept { return a *= b; }
    friend constexpr Vector4 operator/(Vector4 a, const Vector4& b) noexcept { return a /= b; }
    friend constexpr Vector4 operator*(Vector4 a, T s) noexcept { return a *= s; }
    friend constexpr Vector4 operator*(T s, Vector4 a) noexcept { return a *= s; }
    friend constexpr Vector4 operator/(Vector4 a, T s) noexcept { return a /= s; }
    friend constexpr Vector4 operator-(const Vector4& a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
    friend constexpr bool operator==(const Vector4&, const Vector4&) noexcept = default;
};

using Vector4f = Vector4<float>;
using Vector4d = Vector4<double>;
using Vector4i = Vector4<core::i32>;

template <class T> [[nodiscard]] constexpr T dot(const Vector4<T>& a, const Vector4<T>& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
template <class T> [[nodiscard]] constexpr T length_squared(const Vector4<T>& a) noexcept { return dot(a, a); }
template <Real T> [[nodiscard]] inline T length(const Vector4<T>& a) noexcept { return std::sqrt(dot(a, a)); }
template <Real T> [[nodiscard]] inline T distance(const Vector4<T>& a, const Vector4<T>& b) noexcept { return length(b - a); }

template <Real T>
[[nodiscard]] inline Vector4<T> normalize(const Vector4<T>& a) {
    const T l = length(a);
    if (!(l > std::numeric_limits<T>::min())) throw MathException("normalize: zero-length vector");
    return a / l;
}
template <class T> [[nodiscard]] constexpr Vector4<T> min(const Vector4<T>& a, const Vector4<T>& b) noexcept {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w};
}
template <class T> [[nodiscard]] constexpr Vector4<T> max(const Vector4<T>& a, const Vector4<T>& b) noexcept {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w};
}
template <class T> [[nodiscard]] constexpr Vector4<T> clamp(const Vector4<T>& v, const Vector4<T>& lo, const Vector4<T>& hi) noexcept { return min(max(v, lo), hi); }
template <Real T> [[nodiscard]] constexpr Vector4<T> lerp(const Vector4<T>& a, const Vector4<T>& b, T t) noexcept { return a + (b - a) * t; }

/// Perspective divide: (x/w, y/w, z/w). Throws when w == 0.
template <Real T> [[nodiscard]] inline Vector3<T> homogeneous_divide(const Vector4<T>& v) {
    if (v.w == T(0)) throw MathException("homogeneous_divide: w is zero");
    return {v.x / v.w, v.y / v.w, v.z / v.w};
}
template <Real T> [[nodiscard]] inline bool approx_equal(const Vector4<T>& a, const Vector4<T>& b, T rel = default_rel_tol<T>, T abs_ = default_abs_tol<T>) noexcept {
    return approx_equal(a.x, b.x, rel, abs_) && approx_equal(a.y, b.y, rel, abs_) && approx_equal(a.z, b.z, rel, abs_) && approx_equal(a.w, b.w, rel, abs_);
}
template <class T> [[nodiscard]] inline std::string to_string(const Vector4<T>& v) { return std::format("({}, {}, {}, {})", v.x, v.y, v.z, v.w); }

}  // namespace flyg::math

template <class T>
struct std::formatter<flyg::math::Vector4<T>> : std::formatter<std::string> {
    auto format(const flyg::math::Vector4<T>& v, std::format_context& ctx) const {
        return std::formatter<std::string>::format(flyg::math::to_string(v), ctx);
    }
};
