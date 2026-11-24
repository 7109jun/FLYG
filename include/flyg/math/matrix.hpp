// flyG - fixed-size square matrices (Matrix2 / Matrix3 / Matrix4), column-major
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>

#include <flyg/math/scalar.hpp>
#include <flyg/math/vector2.hpp>
#include <flyg/math/vector3.hpp>
#include <flyg/math/vector4.hpp>

namespace flyg::math {

/// N x N matrix stored column-major: element (row r, column c) lives at m[c * N + r].
/// Vectors are column vectors: `M * v`.
template <usize N, Real T>
struct SquareMatrix {
    static_assert(N >= 2 && N <= 4, "flyG fixed matrices are 2x2 .. 4x4 (use math::Matrix for dynamic sizes)");
    using value_type = T;
    static constexpr usize size = N;

    std::array<T, N * N> m{};

    constexpr SquareMatrix() noexcept = default;  // zero matrix

    /// Row-major initialiser list for readability: {{a,b},{c,d}}.
    constexpr SquareMatrix(std::initializer_list<std::initializer_list<T>> rows) {
        if (rows.size() != N) throw MathException("matrix initializer: wrong row count");
        usize r = 0;
        for (const auto& row : rows) {
            if (row.size() != N) throw MathException("matrix initializer: wrong column count");
            usize c = 0;
            for (const T v : row) (*this)(r, c++) = v;
            ++r;
        }
    }

    [[nodiscard]] static constexpr SquareMatrix identity() noexcept {
        SquareMatrix r;
        for (usize i = 0; i < N; ++i) r(i, i) = T(1);
        return r;
    }
    [[nodiscard]] static constexpr SquareMatrix diagonal(T d) noexcept {
        SquareMatrix r;
        for (usize i = 0; i < N; ++i) r(i, i) = d;
        return r;
    }

    constexpr T& operator()(usize row, usize col) noexcept { return m[col * N + row]; }
    constexpr const T& operator()(usize row, usize col) const noexcept { return m[col * N + row]; }

    constexpr T& at(usize row, usize col) {
        if (row >= N || col >= N) throw MathException("matrix index out of range");
        return m[col * N + row];
    }
    constexpr const T& at(usize row, usize col) const {
        if (row >= N || col >= N) throw MathException("matrix index out of range");
        return m[col * N + row];
    }

    constexpr T* data() noexcept { return m.data(); }
    constexpr const T* data() const noexcept { return m.data(); }

    friend constexpr bool operator==(const SquareMatrix&, const SquareMatrix&) noexcept = default;

    constexpr SquareMatrix& operator+=(const SquareMatrix& o) noexcept { for (usize i = 0; i < N * N; ++i) m[i] += o.m[i]; return *this; }
    constexpr SquareMatrix& operator-=(const SquareMatrix& o) noexcept { for (usize i = 0; i < N * N; ++i) m[i] -= o.m[i]; return *this; }
    constexpr SquareMatrix& operator*=(T s) noexcept { for (T& v : m) v *= s; return *this; }
    constexpr SquareMatrix& operator/=(T s) noexcept { for (T& v : m) v /= s; return *this; }
    friend constexpr SquareMatrix operator+(SquareMatrix a, const SquareMatrix& b) noexcept { return a += b; }
    friend constexpr SquareMatrix operator-(SquareMatrix a, const SquareMatrix& b) noexcept { return a -= b; }
    friend constexpr SquareMatrix operator*(SquareMatrix a, T s) noexcept { return a *= s; }
    friend constexpr SquareMatrix operator*(T s, SquareMatrix a) noexcept { return a *= s; }
    friend constexpr SquareMatrix operator/(SquareMatrix a, T s) noexcept { return a /= s; }

    friend constexpr SquareMatrix operator*(const SquareMatrix& a, const SquareMatrix& b) noexcept {
        SquareMatrix r;
        for (usize c = 0; c < N; ++c)
            for (usize k = 0; k < N; ++k) {
                const T bkc = b(k, c);
                for (usize row = 0; row < N; ++row) r(row, c) += a(row, k) * bkc;
            }
        return r;
    }
    constexpr SquareMatrix& operator*=(const SquareMatrix& o) noexcept { return *this = *this * o; }
};

using Matrix2f = SquareMatrix<2, float>;
using Matrix3f = SquareMatrix<3, float>;
using Matrix4f = SquareMatrix<4, float>;
using Matrix2d = SquareMatrix<2, double>;
using Matrix3d = SquareMatrix<3, double>;
using Matrix4d = SquareMatrix<4, double>;
template <Real T> using Matrix2 = SquareMatrix<2, T>;
template <Real T> using Matrix3 = SquareMatrix<3, T>;
template <Real T> using Matrix4 = SquareMatrix<4, T>;

// ---- matrix * vector ------------------------------------------------------
template <Real T> [[nodiscard]] constexpr Vector2<T> operator*(const SquareMatrix<2, T>& a, const Vector2<T>& v) noexcept {
    return {a(0, 0) * v.x + a(0, 1) * v.y, a(1, 0) * v.x + a(1, 1) * v.y};
}
template <Real T> [[nodiscard]] constexpr Vector3<T> operator*(const SquareMatrix<3, T>& a, const Vector3<T>& v) noexcept {
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z};
}
template <Real T> [[nodiscard]] constexpr Vector4<T> operator*(const SquareMatrix<4, T>& a, const Vector4<T>& v) noexcept {
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z + a(0, 3) * v.w,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z + a(1, 3) * v.w,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z + a(2, 3) * v.w,
            a(3, 0) * v.x + a(3, 1) * v.y + a(3, 2) * v.z + a(3, 3) * v.w};
}

// ---- generic operations ---------------------------------------------------
template <usize N, Real T>
[[nodiscard]] constexpr SquareMatrix<N, T> transpose(const SquareMatrix<N, T>& a) noexcept {
    SquareMatrix<N, T> r;
    for (usize i = 0; i < N; ++i)
        for (usize j = 0; j < N; ++j) r(i, j) = a(j, i);
    return r;
}

template <usize N, Real T>
[[nodiscard]] constexpr T trace(const SquareMatrix<N, T>& a) noexcept {
    T t = T(0);
    for (usize i = 0; i < N; ++i) t += a(i, i);
    return t;
}

template <Real T> [[nodiscard]] constexpr T determinant(const SquareMatrix<2, T>& a) noexcept {
    return a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0);
}
template <Real T> [[nodiscard]] constexpr T determinant(const SquareMatrix<3, T>& a) noexcept {
    return a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
           a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
           a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
}
template <Real T> [[nodiscard]] constexpr T determinant(const SquareMatrix<4, T>& a) noexcept {
    const T s0 = a(0, 0) * a(1, 1) - a(1, 0) * a(0, 1);
    const T s1 = a(0, 0) * a(1, 2) - a(1, 0) * a(0, 2);
    const T s2 = a(0, 0) * a(1, 3) - a(1, 0) * a(0, 3);
    const T s3 = a(0, 1) * a(1, 2) - a(1, 1) * a(0, 2);
    const T s4 = a(0, 1) * a(1, 3) - a(1, 1) * a(0, 3);
    const T s5 = a(0, 2) * a(1, 3) - a(1, 2) * a(0, 3);
    const T c5 = a(2, 2) * a(3, 3) - a(3, 2) * a(2, 3);
    const T c4 = a(2, 1) * a(3, 3) - a(3, 1) * a(2, 3);
    const T c3 = a(2, 1) * a(3, 2) - a(3, 1) * a(2, 2);
    const T c2 = a(2, 0) * a(3, 3) - a(3, 0) * a(2, 3);
    const T c1 = a(2, 0) * a(3, 2) - a(3, 0) * a(2, 2);
    const T c0 = a(2, 0) * a(3, 1) - a(3, 0) * a(2, 1);
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

/// Inverse via Gauss-Jordan elimination with full row pivoting. Throws MathException when the
/// matrix is singular (pivot below `tolerance` relative to the largest entry).
template <usize N, Real T>
[[nodiscard]] SquareMatrix<N, T> inverse(const SquareMatrix<N, T>& a, T tolerance = T(64) * std::numeric_limits<T>::epsilon()) {
    SquareMatrix<N, T> work = a;
    SquareMatrix<N, T> inv = SquareMatrix<N, T>::identity();
    T max_abs = T(0);
    for (const T v : a.m) max_abs = std::max(max_abs, std::fabs(v));
    if (max_abs == T(0)) throw MathException("inverse: zero matrix is singular");
    for (usize col = 0; col < N; ++col) {
        usize pivot = col;
        T best = std::fabs(work(col, col));
        for (usize r = col + 1; r < N; ++r) {
            const T v = std::fabs(work(r, col));
            if (v > best) { best = v; pivot = r; }
        }
        if (!(best > tolerance * max_abs)) throw MathException("inverse: matrix is singular");
        if (pivot != col)
            for (usize c = 0; c < N; ++c) {
                std::swap(work(col, c), work(pivot, c));
                std::swap(inv(col, c), inv(pivot, c));
            }
        const T inv_p = T(1) / work(col, col);
        for (usize c = 0; c < N; ++c) { work(col, c) *= inv_p; inv(col, c) *= inv_p; }
        for (usize r = 0; r < N; ++r) {
            if (r == col) continue;
            const T f = work(r, col);
            if (f == T(0)) continue;
            for (usize c = 0; c < N; ++c) { work(r, c) -= f * work(col, c); inv(r, c) -= f * inv(col, c); }
        }
    }
    return inv;
}

template <usize N, Real T>
[[nodiscard]] bool approx_equal(const SquareMatrix<N, T>& a, const SquareMatrix<N, T>& b, T rel = default_rel_tol<T>, T abs_ = default_abs_tol<T>) noexcept {
    for (usize i = 0; i < N * N; ++i)
        if (!approx_equal(a.m[i], b.m[i], rel, abs_)) return false;
    return true;
}

template <usize N, Real T>
[[nodiscard]] std::string to_string(const SquareMatrix<N, T>& a) {
    std::string s;
    for (usize r = 0; r < N; ++r) {
        s += r == 0 ? "[" : " ";
        for (usize c = 0; c < N; ++c) {
            if (c != 0) s += ", ";
            s += std::format("{}", a(r, c));
        }
        s += r + 1 == N ? "]" : "\n";
    }
    return s;
}

// ---- 2D (Matrix3) and 3D (Matrix3 / Matrix4) transformations -------------
template <Real T = float> [[nodiscard]] inline SquareMatrix<2, T> rotation2(T radians_) noexcept {
    const T c = std::cos(radians_), s = std::sin(radians_);
    SquareMatrix<2, T> r;
    r(0, 0) = c; r(0, 1) = -s; r(1, 0) = s; r(1, 1) = c;
    return r;
}
template <Real T> [[nodiscard]] constexpr SquareMatrix<3, T> translation2(const Vector2<T>& t) noexcept {
    auto r = SquareMatrix<3, T>::identity();
    r(0, 2) = t.x; r(1, 2) = t.y;
    return r;
}
template <Real T> [[nodiscard]] constexpr SquareMatrix<3, T> scaling2(const Vector2<T>& s) noexcept {
    auto r = SquareMatrix<3, T>::identity();
    r(0, 0) = s.x; r(1, 1) = s.y;
    return r;
}
template <Real T> [[nodiscard]] inline SquareMatrix<3, T> rotation2_h(T radians_) noexcept {
    const T c = std::cos(radians_), s = std::sin(radians_);
    auto r = SquareMatrix<3, T>::identity();
    r(0, 0) = c; r(0, 1) = -s; r(1, 0) = s; r(1, 1) = c;
    return r;
}

template <Real T> [[nodiscard]] constexpr SquareMatrix<4, T> translation(const Vector3<T>& t) noexcept {
    auto r = SquareMatrix<4, T>::identity();
    r(0, 3) = t.x; r(1, 3) = t.y; r(2, 3) = t.z;
    return r;
}
template <Real T> [[nodiscard]] constexpr SquareMatrix<4, T> scaling(const Vector3<T>& s) noexcept {
    auto r = SquareMatrix<4, T>::identity();
    r(0, 0) = s.x; r(1, 1) = s.y; r(2, 2) = s.z;
    return r;
}
/// Rotation of `radians_` around the (not necessarily unit) `axis`. Throws for a zero axis.
template <Real T> [[nodiscard]] inline SquareMatrix<3, T> rotation3(const Vector3<T>& axis, T radians_) {
    const Vector3<T> a = normalize(axis);
    const T c = std::cos(radians_), s = std::sin(radians_), t = T(1) - c;
    SquareMatrix<3, T> r;
    r(0, 0) = t * a.x * a.x + c;       r(0, 1) = t * a.x * a.y - s * a.z; r(0, 2) = t * a.x * a.z + s * a.y;
    r(1, 0) = t * a.x * a.y + s * a.z; r(1, 1) = t * a.y * a.y + c;       r(1, 2) = t * a.y * a.z - s * a.x;
    r(2, 0) = t * a.x * a.z - s * a.y; r(2, 1) = t * a.y * a.z + s * a.x; r(2, 2) = t * a.z * a.z + c;
    return r;
}
template <Real T> [[nodiscard]] inline SquareMatrix<4, T> rotation(const Vector3<T>& axis, T radians_) {
    const auto r3 = rotation3(axis, radians_);
    auto r = SquareMatrix<4, T>::identity();
    for (usize i = 0; i < 3; ++i)
        for (usize j = 0; j < 3; ++j) r(i, j) = r3(i, j);
    return r;
}
template <Real T> [[nodiscard]] constexpr SquareMatrix<4, T> to_matrix4(const SquareMatrix<3, T>& r3) noexcept {
    auto r = SquareMatrix<4, T>::identity();
    for (usize i = 0; i < 3; ++i)
        for (usize j = 0; j < 3; ++j) r(i, j) = r3(i, j);
    return r;
}

template <Real T> [[nodiscard]] constexpr Vector3<T> transform_point(const SquareMatrix<4, T>& a, const Vector3<T>& p) noexcept {
    const Vector4<T> r = a * Vector4<T>(p, T(1));
    return {r.x, r.y, r.z};
}
template <Real T> [[nodiscard]] constexpr Vector3<T> transform_direction(const SquareMatrix<4, T>& a, const Vector3<T>& d) noexcept {
    const Vector4<T> r = a * Vector4<T>(d, T(0));
    return {r.x, r.y, r.z};
}

/// Right-handed view matrix looking from `eye` towards `target` (OpenGL convention, camera looks down -z).
template <Real T> [[nodiscard]] inline SquareMatrix<4, T> look_at(const Vector3<T>& eye, const Vector3<T>& target, const Vector3<T>& up) {
    const Vector3<T> f = normalize(target - eye);
    const Vector3<T> s = normalize(cross(f, up));
    const Vector3<T> u = cross(s, f);
    auto r = SquareMatrix<4, T>::identity();
    r(0, 0) = s.x; r(0, 1) = s.y; r(0, 2) = s.z; r(0, 3) = -dot(s, eye);
    r(1, 0) = u.x; r(1, 1) = u.y; r(1, 2) = u.z; r(1, 3) = -dot(u, eye);
    r(2, 0) = -f.x; r(2, 1) = -f.y; r(2, 2) = -f.z; r(2, 3) = dot(f, eye);
    return r;
}

/// Right-handed perspective projection, clip-space z in [-1, 1] (OpenGL).
template <Real T> [[nodiscard]] inline SquareMatrix<4, T> perspective(T fov_y_radians, T aspect, T z_near, T z_far) {
    if (!(fov_y_radians > T(0) && fov_y_radians < pi_v<T>) || !(aspect > T(0)) || !(z_near > T(0)) || !(z_far > z_near))
        throw MathException("perspective: invalid parameters");
    const T f = T(1) / std::tan(fov_y_radians / T(2));
    SquareMatrix<4, T> r;
    r(0, 0) = f / aspect;
    r(1, 1) = f;
    r(2, 2) = (z_far + z_near) / (z_near - z_far);
    r(2, 3) = (T(2) * z_far * z_near) / (z_near - z_far);
    r(3, 2) = T(-1);
    return r;
}
/// Right-handed orthographic projection, clip-space z in [-1, 1] (OpenGL).
template <Real T> [[nodiscard]] inline SquareMatrix<4, T> orthographic(T left, T right, T bottom, T top, T z_near, T z_far) {
    if (left == right || bottom == top || z_near == z_far) throw MathException("orthographic: degenerate volume");
    auto r = SquareMatrix<4, T>::identity();
    r(0, 0) = T(2) / (right - left);
    r(1, 1) = T(2) / (top - bottom);
    r(2, 2) = T(-2) / (z_far - z_near);
    r(0, 3) = -(right + left) / (right - left);
    r(1, 3) = -(top + bottom) / (top - bottom);
    r(2, 3) = -(z_far + z_near) / (z_far - z_near);
    return r;
}

}  // namespace flyg::math
