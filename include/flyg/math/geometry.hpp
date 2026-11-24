// flyG - 3D geometry primitives, intersection and collision detection
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <utility>

#include <flyg/math/scalar.hpp>
#include <flyg/math/vector3.hpp>

namespace flyg::math {

template <Real T> using Point3 = Vector3<T>;
using Point3f = Point3<float>;
using Point3d = Point3<double>;

// ============================================================================
// primitives
// ============================================================================

/// Infinite line through `point` along `direction` (unit length).
template <Real T>
struct Line {
    Vector3<T> point{};
    Vector3<T> direction{T(1), T(0), T(0)};

    [[nodiscard]] static Line from_points(const Vector3<T>& a, const Vector3<T>& b) { return {a, normalize(b - a)}; }
    [[nodiscard]] constexpr Vector3<T> at(T t) const noexcept { return point + direction * t; }
    [[nodiscard]] constexpr T project_parameter(const Vector3<T>& p) const noexcept { return dot(p - point, direction); }
    [[nodiscard]] constexpr Vector3<T> closest_point(const Vector3<T>& p) const noexcept { return at(project_parameter(p)); }
    [[nodiscard]] inline T distance_to(const Vector3<T>& p) const noexcept { return distance(p, closest_point(p)); }
};

/// Finite segment from `a` to `b`.
template <Real T>
struct Segment {
    Vector3<T> a{}, b{};
    [[nodiscard]] constexpr Vector3<T> direction_vector() const noexcept { return b - a; }
    [[nodiscard]] inline T length() const noexcept { return math::length(b - a); }
    [[nodiscard]] constexpr Vector3<T> at(T t) const noexcept { return a + (b - a) * t; }
    [[nodiscard]] constexpr Vector3<T> closest_point(const Vector3<T>& p) const noexcept {
        const Vector3<T> ab = b - a;
        const T d = dot(ab, ab);
        if (d == T(0)) return a;
        return at(clamp(dot(p - a, ab) / d, T(0), T(1)));
    }
    [[nodiscard]] inline T distance_to(const Vector3<T>& p) const noexcept { return distance(p, closest_point(p)); }
};

/// Ray: origin plus unit `direction`, t >= 0.
template <Real T>
struct Ray {
    Vector3<T> origin{};
    Vector3<T> direction{T(0), T(0), T(-1)};

    [[nodiscard]] static Ray from_points(const Vector3<T>& from, const Vector3<T>& to) { return {from, normalize(to - from)}; }
    [[nodiscard]] constexpr Vector3<T> at(T t) const noexcept { return origin + direction * t; }
};

/// Plane { p : dot(normal, p) + d == 0 } with unit `normal`.
template <Real T>
struct Plane {
    Vector3<T> normal{T(0), T(1), T(0)};
    T d{};

    [[nodiscard]] static Plane from_point_normal(const Vector3<T>& point, const Vector3<T>& n) {
        const Vector3<T> u = normalize(n);
        return {u, -dot(u, point)};
    }
    /// Counter-clockwise a, b, c defines the front side. Throws for collinear points.
    [[nodiscard]] static Plane from_points(const Vector3<T>& a, const Vector3<T>& b, const Vector3<T>& c) {
        return from_point_normal(a, cross(b - a, c - a));
    }
    [[nodiscard]] constexpr T signed_distance(const Vector3<T>& p) const noexcept { return dot(normal, p) + d; }
    [[nodiscard]] constexpr Vector3<T> closest_point(const Vector3<T>& p) const noexcept { return p - normal * signed_distance(p); }
};

template <Real T>
struct Sphere {
    Vector3<T> center{};
    T radius{T(1)};
    [[nodiscard]] constexpr bool contains(const Vector3<T>& p) const noexcept { return distance_squared(p, center) <= radius * radius; }
    [[nodiscard]] inline T volume() const noexcept { return T(4) / T(3) * pi_v<T> * radius * radius * radius; }
    [[nodiscard]] inline T surface_area() const noexcept { return T(4) * pi_v<T> * radius * radius; }
};

/// Axis-aligned box.
template <Real T>
struct Box {
    Vector3<T> min{T(0)}, max{T(0)};

    [[nodiscard]] static constexpr Box from_center_extents(const Vector3<T>& c, const Vector3<T>& half) noexcept { return {c - half, c + half}; }
    /// Tightest box around a non-empty point set.
    [[nodiscard]] static Box from_points(std::span<const Vector3<T>> pts) {
        if (pts.empty()) throw MathException("Box::from_points: no points");
        Box b{pts[0], pts[0]};
        for (const auto& p : pts) b.expand(p);
        return b;
    }
    [[nodiscard]] constexpr Vector3<T> center() const noexcept { return (min + max) * T(0.5); }
    [[nodiscard]] constexpr Vector3<T> extents() const noexcept { return (max - min) * T(0.5); }
    [[nodiscard]] constexpr Vector3<T> size() const noexcept { return max - min; }
    [[nodiscard]] constexpr bool valid() const noexcept { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    [[nodiscard]] constexpr T volume() const noexcept { const auto s = size(); return s.x * s.y * s.z; }
    [[nodiscard]] constexpr bool contains(const Vector3<T>& p) const noexcept {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    [[nodiscard]] constexpr bool contains(const Box& o) const noexcept { return contains(o.min) && contains(o.max); }
    constexpr void expand(const Vector3<T>& p) noexcept { min = math::min(min, p); max = math::max(max, p); }
    constexpr void expand(const Box& o) noexcept { expand(o.min); expand(o.max); }
    [[nodiscard]] constexpr Vector3<T> closest_point(const Vector3<T>& p) const noexcept { return clamp(p, min, max); }
};

/// Triangle (counter-clockwise front face).
template <Real T>
struct Triangle {
    Vector3<T> a{}, b{}, c{};
    [[nodiscard]] inline Vector3<T> normal() const { return normalize(cross(b - a, c - a)); }
    [[nodiscard]] inline T area() const noexcept { return length(cross(b - a, c - a)) / T(2); }
};

using Linef = Line<float>;       using Lined = Line<double>;
using Segmentf = Segment<float>; using Segmentd = Segment<double>;
using Rayf = Ray<float>;         using Rayd = Ray<double>;
using Planef = Plane<float>;     using Planed = Plane<double>;
using Spheref = Sphere<float>;   using Sphered = Sphere<double>;
using Boxf = Box<float>;         using Boxd = Box<double>;
using Trianglef = Triangle<float>; using Triangled = Triangle<double>;

// ============================================================================
// distance queries
// ============================================================================

/// Closest points between two infinite lines. Returns {point on a, point on b}; for parallel
/// lines the point on `a` is `a.point`.
template <Real T>
[[nodiscard]] inline std::pair<Vector3<T>, Vector3<T>> closest_points(const Line<T>& a, const Line<T>& b) noexcept {
    const Vector3<T> w = a.point - b.point;
    const T bb = dot(a.direction, b.direction);
    const T d = dot(a.direction, w);
    const T e = dot(b.direction, w);
    const T denom = T(1) - bb * bb;
    T s = T(0), t = e;
    if (denom > T(1e-12)) {
        s = (bb * e - d) / denom;
        t = (e - bb * d) / denom;
    }
    return {a.at(s), b.at(t)};
}

template <Real T> [[nodiscard]] inline T distance(const Line<T>& a, const Line<T>& b) noexcept {
    const auto [p, q] = closest_points(a, b);
    return distance(p, q);
}

/// Closest points between two segments.
template <Real T>
[[nodiscard]] inline std::pair<Vector3<T>, Vector3<T>> closest_points(const Segment<T>& s1, const Segment<T>& s2) noexcept {
    const Vector3<T> d1 = s1.b - s1.a, d2 = s2.b - s2.a, r = s1.a - s2.a;
    const T a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    T s, t;
    if (a <= T(1e-20) && e <= T(1e-20)) return {s1.a, s2.a};
    if (a <= T(1e-20)) {
        s = T(0); t = clamp(f / e, T(0), T(1));
    } else {
        const T c = dot(d1, r);
        if (e <= T(1e-20)) {
            t = T(0); s = clamp(-c / a, T(0), T(1));
        } else {
            const T b = dot(d1, d2), denom = a * e - b * b;
            s = denom > T(1e-20) ? clamp((b * f - c * e) / denom, T(0), T(1)) : T(0);
            t = (b * s + f) / e;
            if (t < T(0)) { t = T(0); s = clamp(-c / a, T(0), T(1)); }
            else if (t > T(1)) { t = T(1); s = clamp((b - c) / a, T(0), T(1)); }
        }
    }
    return {s1.at(s), s2.at(t)};
}

template <Real T> [[nodiscard]] inline T distance(const Segment<T>& a, const Segment<T>& b) noexcept {
    const auto [p, q] = closest_points(a, b);
    return distance(p, q);
}

// ============================================================================
// ray casts. Results are the ray parameter t >= 0 (hit = ray.at(t)).
// ============================================================================

template <Real T>
[[nodiscard]] inline std::optional<T> intersect(const Ray<T>& ray, const Plane<T>& plane) noexcept {
    const T denom = dot(plane.normal, ray.direction);
    if (std::fabs(denom) < T(1e-12)) return std::nullopt;  // parallel
    const T t = -plane.signed_distance(ray.origin) / denom;
    if (t < T(0)) return std::nullopt;
    return t;
}

/// Nearest non-negative hit (0 when the origin is inside the sphere).
template <Real T>
[[nodiscard]] inline std::optional<T> intersect(const Ray<T>& ray, const Sphere<T>& s) noexcept {
    const Vector3<T> oc = ray.origin - s.center;
    const T b = dot(oc, ray.direction);
    const T c = dot(oc, oc) - s.radius * s.radius;
    if (c > T(0) && b > T(0)) return std::nullopt;  // outside and pointing away
    const T disc = b * b - c;
    if (disc < T(0)) return std::nullopt;
    const T t = -b - std::sqrt(disc);
    return t < T(0) ? T(0) : t;
}

/// Slab test. Returns the entry distance (0 when the origin is inside the box).
template <Real T>
[[nodiscard]] inline std::optional<T> intersect(const Ray<T>& ray, const Box<T>& box) noexcept {
    T tmin = T(0), tmax = infinity_v<T>;
    for (usize i = 0; i < 3; ++i) {
        const T o = ray.origin[i], d = ray.direction[i];
        if (std::fabs(d) < T(1e-15)) {
            if (o < box.min[i] || o > box.max[i]) return std::nullopt;
        } else {
            T t1 = (box.min[i] - o) / d, t2 = (box.max[i] - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return std::nullopt;
        }
    }
    return tmin;
}

template <Real T>
struct TriangleHit {
    T t;       ///< ray parameter
    T u, v;    ///< barycentric coordinates of b and c (a has 1-u-v)
};

/// Moeller-Trumbore ray/triangle intersection (double sided).
template <Real T>
[[nodiscard]] inline std::optional<TriangleHit<T>> intersect(const Ray<T>& ray, const Triangle<T>& tri) noexcept {
    const Vector3<T> e1 = tri.b - tri.a, e2 = tri.c - tri.a;
    const Vector3<T> p = cross(ray.direction, e2);
    const T det = dot(e1, p);
    if (std::fabs(det) < T(1e-14)) return std::nullopt;
    const T inv = T(1) / det;
    const Vector3<T> s = ray.origin - tri.a;
    const T u = dot(s, p) * inv;
    if (u < T(0) || u > T(1)) return std::nullopt;
    const Vector3<T> q = cross(s, e1);
    const T v = dot(ray.direction, q) * inv;
    if (v < T(0) || u + v > T(1)) return std::nullopt;
    const T t = dot(e2, q) * inv;
    if (t < T(0)) return std::nullopt;
    return TriangleHit<T>{t, u, v};
}

/// Segment/plane crossing point.
template <Real T>
[[nodiscard]] inline std::optional<Vector3<T>> intersect(const Segment<T>& seg, const Plane<T>& plane) noexcept {
    const T da = plane.signed_distance(seg.a), db = plane.signed_distance(seg.b);
    if ((da > T(0) && db > T(0)) || (da < T(0) && db < T(0))) return std::nullopt;
    if (da == db) return std::nullopt;  // lies in the plane or parallel
    return seg.at(da / (da - db));
}

// ============================================================================
// overlap tests
// ============================================================================

template <Real T> [[nodiscard]] constexpr bool overlaps(const Sphere<T>& a, const Sphere<T>& b) noexcept {
    const T r = a.radius + b.radius;
    return distance_squared(a.center, b.center) <= r * r;
}
template <Real T> [[nodiscard]] constexpr bool overlaps(const Box<T>& a, const Box<T>& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
template <Real T> [[nodiscard]] constexpr bool overlaps(const Sphere<T>& s, const Box<T>& b) noexcept {
    return distance_squared(s.center, b.closest_point(s.center)) <= s.radius * s.radius;
}
template <Real T> [[nodiscard]] constexpr bool overlaps(const Box<T>& b, const Sphere<T>& s) noexcept { return overlaps(s, b); }
template <Real T> [[nodiscard]] constexpr bool overlaps(const Sphere<T>& s, const Plane<T>& p) noexcept {
    const T d = p.signed_distance(s.center);
    return (d < T(0) ? -d : d) <= s.radius;
}
template <Real T> [[nodiscard]] constexpr bool overlaps(const Box<T>& b, const Plane<T>& p) noexcept {
    const Vector3<T> c = b.center(), e = b.extents();
    const T r = e.x * (p.normal.x < T(0) ? -p.normal.x : p.normal.x) + e.y * (p.normal.y < T(0) ? -p.normal.y : p.normal.y) +
                e.z * (p.normal.z < T(0) ? -p.normal.z : p.normal.z);
    const T s = p.signed_distance(c);
    return (s < T(0) ? -s : s) <= r;
}

// ============================================================================
// collision detection with contact information
// ============================================================================

/// `normal` points from shape A towards shape B; `depth` is the penetration distance;
/// `point` is a contact point on the overlap region.
template <Real T>
struct Contact {
    Vector3<T> normal{};
    T depth{};
    Vector3<T> point{};
};

template <Real T>
[[nodiscard]] inline std::optional<Contact<T>> collide(const Sphere<T>& a, const Sphere<T>& b) noexcept {
    const Vector3<T> d = b.center - a.center;
    const T r = a.radius + b.radius;
    const T dist2 = dot(d, d);
    if (dist2 > r * r) return std::nullopt;
    const T dist = std::sqrt(dist2);
    const Vector3<T> n = dist > T(1e-12) ? d / dist : Vector3<T>::unit_y();
    return Contact<T>{n, r - dist, a.center + n * (a.radius - (r - dist) / T(2))};
}

template <Real T>
[[nodiscard]] inline std::optional<Contact<T>> collide(const Box<T>& a, const Box<T>& b) noexcept {
    if (!overlaps(a, b)) return std::nullopt;
    const Vector3<T> ca = a.center(), cb = b.center();
    const Vector3<T> ea = a.extents(), eb = b.extents();
    const Vector3<T> delta = cb - ca;
    T best = infinity_v<T>;
    usize axis = 0;
    for (usize i = 0; i < 3; ++i) {
        const T pen = ea[i] + eb[i] - std::fabs(delta[i]);
        if (pen < best) { best = pen; axis = i; }
    }
    Vector3<T> n{};
    n[axis] = delta[axis] < T(0) ? T(-1) : T(1);
    const Box<T> overlap{max(a.min, b.min), min(a.max, b.max)};
    return Contact<T>{n, best, overlap.center()};
}

template <Real T>
[[nodiscard]] inline std::optional<Contact<T>> collide(const Sphere<T>& s, const Box<T>& b) noexcept {
    const Vector3<T> closest = b.closest_point(s.center);
    const Vector3<T> d = closest - s.center;
    const T dist2 = dot(d, d);
    if (dist2 > s.radius * s.radius) return std::nullopt;
    if (dist2 > T(1e-18)) {  // sphere centre outside the box
        const T dist = std::sqrt(dist2);
        return Contact<T>{d / dist, s.radius - dist, closest};
    }
    // centre inside the box: push out through the nearest face
    const Vector3<T> c = b.center(), e = b.extents();
    T best = infinity_v<T>;
    usize axis = 0;
    for (usize i = 0; i < 3; ++i) {
        const T room = e[i] - std::fabs(s.center[i] - c[i]);
        if (room < best) { best = room; axis = i; }
    }
    Vector3<T> n{};
    n[axis] = s.center[axis] < c[axis] ? T(-1) : T(1);
    return Contact<T>{n, s.radius + best, s.center};
}

template <Real T>
[[nodiscard]] inline std::optional<Contact<T>> collide(const Sphere<T>& s, const Plane<T>& p) noexcept {
    const T d = p.signed_distance(s.center);
    if ((d < T(0) ? -d : d) > s.radius) return std::nullopt;
    // normal from the sphere towards the plane
    const Vector3<T> n = d >= T(0) ? -p.normal : p.normal;
    return Contact<T>{n, s.radius - (d < T(0) ? -d : d), p.closest_point(s.center)};
}

}  // namespace flyg::math
