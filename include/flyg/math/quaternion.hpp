// flyG - Quaternion
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <format>
#include <string>

#include <flyg/math/matrix.hpp>
#include <flyg/math/scalar.hpp>
#include <flyg/math/vector3.hpp>

namespace flyg::math {

/// Quaternion q = w + xi + yj + zk. Rotation quaternions must be unit length.
template <Real T>
struct Quaternion {
    using value_type = T;
    T x{}, y{}, z{}, w{T(1)};  // default = identity rotation

    constexpr Quaternion() noexcept = default;
    constexpr Quaternion(T x_, T y_, T z_, T w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    [[nodiscard]] static constexpr Quaternion identity() noexcept { return {}; }

    /// Rotation of `radians_` around `axis` (any non-zero length).
    [[nodiscard]] static Quaternion from_axis_angle(const Vector3<T>& axis, T radians_) {
        const Vector3<T> a = normalize(axis);
        const T h = radians_ / T(2);
        const T s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }

    /// Intrinsic Z-Y-X (yaw, pitch, roll) = rotation about Z, then Y, then X, all in radians.
    [[nodiscard]] static Quaternion from_euler(T roll_x, T pitch_y, T yaw_z) noexcept {
        const T cr = std::cos(roll_x / 2), sr = std::sin(roll_x / 2);
        const T cp = std::cos(pitch_y / 2), sp = std::sin(pitch_y / 2);
        const T cy = std::cos(yaw_z / 2), sy = std::sin(yaw_z / 2);
        return {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
                cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
    }

    /// From a proper rotation matrix (orthonormal, det = +1).
    [[nodiscard]] static Quaternion from_matrix(const SquareMatrix<3, T>& r) noexcept {
        const T tr = trace(r);
        Quaternion q;
        if (tr > T(0)) {
            const T s = std::sqrt(tr + T(1)) * T(2);
            q = {(r(2, 1) - r(1, 2)) / s, (r(0, 2) - r(2, 0)) / s, (r(1, 0) - r(0, 1)) / s, s / T(4)};
        } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
            const T s = std::sqrt(T(1) + r(0, 0) - r(1, 1) - r(2, 2)) * T(2);
            q = {s / T(4), (r(0, 1) + r(1, 0)) / s, (r(0, 2) + r(2, 0)) / s, (r(2, 1) - r(1, 2)) / s};
        } else if (r(1, 1) > r(2, 2)) {
            const T s = std::sqrt(T(1) + r(1, 1) - r(0, 0) - r(2, 2)) * T(2);
            q = {(r(0, 1) + r(1, 0)) / s, s / T(4), (r(1, 2) + r(2, 1)) / s, (r(0, 2) - r(2, 0)) / s};
        } else {
            const T s = std::sqrt(T(1) + r(2, 2) - r(0, 0) - r(1, 1)) * T(2);
            q = {(r(0, 2) + r(2, 0)) / s, (r(1, 2) + r(2, 1)) / s, s / T(4), (r(1, 0) - r(0, 1)) / s};
        }
        return q;
    }

    /// Shortest-arc rotation taking unit-ish vector `from` onto `to`.
    [[nodiscard]] static Quaternion from_to(const Vector3<T>& from, const Vector3<T>& to) {
        const Vector3<T> a = normalize(from), b = normalize(to);
        const T d = dot(a, b);
        if (d < T(-1) + T(1e-6)) {  // opposite: rotate pi around any perpendicular axis
            const Vector3<T> p = any_perpendicular(a);
            return {p.x, p.y, p.z, T(0)};
        }
        const Vector3<T> c = cross(a, b);
        Quaternion q{c.x, c.y, c.z, T(1) + d};
        return normalized(q);
    }

    [[nodiscard]] constexpr Quaternion conjugate() const noexcept { return {-x, -y, -z, w}; }

    friend constexpr bool operator==(const Quaternion&, const Quaternion&) noexcept = default;

    friend constexpr Quaternion operator+(const Quaternion& a, const Quaternion& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
    friend constexpr Quaternion operator-(const Quaternion& a, const Quaternion& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
    friend constexpr Quaternion operator*(const Quaternion& a, T s) noexcept { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
    friend constexpr Quaternion operator*(T s, const Quaternion& a) noexcept { return a * s; }
    friend constexpr Quaternion operator-(const Quaternion& a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }

    /// Hamilton product: (a * b) applies b first, then a.
    friend constexpr Quaternion operator*(const Quaternion& a, const Quaternion& b) noexcept {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    constexpr Quaternion& operator*=(const Quaternion& o) noexcept { return *this = *this * o; }
};

using Quaternionf = Quaternion<float>;
using Quaterniond = Quaternion<double>;

template <Real T> [[nodiscard]] constexpr T dot(const Quaternion<T>& a, const Quaternion<T>& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
template <Real T> [[nodiscard]] inline T length(const Quaternion<T>& q) noexcept { return std::sqrt(dot(q, q)); }

template <Real T> [[nodiscard]] inline Quaternion<T> normalized(const Quaternion<T>& q) {
    const T l = length(q);
    if (!(l > std::numeric_limits<T>::min())) throw MathException("Quaternion: cannot normalize zero quaternion");
    return q * (T(1) / l);
}

/// Inverse of any non-zero quaternion.
template <Real T> [[nodiscard]] inline Quaternion<T> inverse(const Quaternion<T>& q) {
    const T n = dot(q, q);
    if (!(n > std::numeric_limits<T>::min())) throw MathException("Quaternion: cannot invert zero quaternion");
    return q.conjugate() * (T(1) / n);
}

/// Rotates `v` by the unit quaternion `q`.
template <Real T> [[nodiscard]] constexpr Vector3<T> rotate(const Quaternion<T>& q, const Vector3<T>& v) noexcept {
    const Vector3<T> u{q.x, q.y, q.z};
    const Vector3<T> t = cross(u, v) * T(2);
    return v + t * q.w + cross(u, t);
}

template <Real T> [[nodiscard]] constexpr SquareMatrix<3, T> to_matrix3(const Quaternion<T>& q) noexcept {
    const T xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const T xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const T wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    SquareMatrix<3, T> r;
    r(0, 0) = T(1) - T(2) * (yy + zz); r(0, 1) = T(2) * (xy - wz);       r(0, 2) = T(2) * (xz + wy);
    r(1, 0) = T(2) * (xy + wz);       r(1, 1) = T(1) - T(2) * (xx + zz); r(1, 2) = T(2) * (yz - wx);
    r(2, 0) = T(2) * (xz - wy);       r(2, 1) = T(2) * (yz + wx);       r(2, 2) = T(1) - T(2) * (xx + yy);
    return r;
}
template <Real T> [[nodiscard]] constexpr SquareMatrix<4, T> to_matrix4(const Quaternion<T>& q) noexcept { return to_matrix4(to_matrix3(q)); }

/// Returns {axis, angle in [0, 2pi]}. Identity yields axis +x and angle 0.
template <Real T> [[nodiscard]] inline std::pair<Vector3<T>, T> to_axis_angle(const Quaternion<T>& q_in) {
    const Quaternion<T> q = normalized(q_in);
    const T angle = T(2) * std::acos(clamp(q.w, T(-1), T(1)));
    const T s = std::sqrt(std::max(T(0), T(1) - q.w * q.w));
    if (s < T(1e-6)) return {Vector3<T>::unit_x(), T(0)};
    return {Vector3<T>{q.x / s, q.y / s, q.z / s}, angle};
}

/// Euler angles {roll(x), pitch(y), yaw(z)} matching from_euler (pitch clamped at +-pi/2).
template <Real T> [[nodiscard]] inline Vector3<T> to_euler(const Quaternion<T>& q) noexcept {
    const T sinr = T(2) * (q.w * q.x + q.y * q.z);
    const T cosr = T(1) - T(2) * (q.x * q.x + q.y * q.y);
    const T sinp = T(2) * (q.w * q.y - q.z * q.x);
    const T pitch = std::fabs(sinp) >= T(1) ? std::copysign(half_pi_v<T>, sinp) : std::asin(sinp);
    const T siny = T(2) * (q.w * q.z + q.x * q.y);
    const T cosy = T(1) - T(2) * (q.y * q.y + q.z * q.z);
    return {std::atan2(sinr, cosr), pitch, std::atan2(siny, cosy)};
}

/// Normalized linear interpolation (cheap, not constant speed).
template <Real T> [[nodiscard]] inline Quaternion<T> nlerp(const Quaternion<T>& a, Quaternion<T> b, T t) {
    if (dot(a, b) < T(0)) b = -b;
    return normalized(a * (T(1) - t) + b * t);
}

/// Spherical linear interpolation along the shortest arc.
template <Real T> [[nodiscard]] inline Quaternion<T> slerp(const Quaternion<T>& a, Quaternion<T> b, T t) {
    T d = dot(a, b);
    if (d < T(0)) { b = -b; d = -d; }
    if (d > T(0.9995)) return nlerp(a, b, t);  // nearly parallel: avoid division by ~0
    const T theta = std::acos(d);
    const T s = std::sin(theta);
    return a * (std::sin((T(1) - t) * theta) / s) + b * (std::sin(t * theta) / s);
}

/// q and -q represent the same rotation.
template <Real T> [[nodiscard]] inline bool rotation_equal(const Quaternion<T>& a, const Quaternion<T>& b, T tol = T(1e-5)) noexcept {
    return std::fabs(dot(a, b)) >= T(1) - tol;
}

template <Real T> [[nodiscard]] inline std::string to_string(const Quaternion<T>& q) { return std::format("({}, {}, {}; w={})", q.x, q.y, q.z, q.w); }

}  // namespace flyg::math
