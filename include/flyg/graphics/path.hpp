// flyG - graphics module: 2D geometry (Point, Transform, Rect) and vector Path with SVG path-data parsing
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

#include <flyg/core/core.hpp>

namespace flyg::graphics {

inline constexpr float kPi = 3.14159265358979323846f;

struct Point {
    float x = 0, y = 0;
    friend constexpr bool operator==(const Point&, const Point&) noexcept = default;
    friend constexpr Point operator+(Point a, Point b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Point operator-(Point a, Point b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Point operator*(Point a, float s) noexcept { return {a.x * s, a.y * s}; }
};
[[nodiscard]] inline float length(Point p) noexcept { return std::hypot(p.x, p.y); }
[[nodiscard]] inline float dot(Point a, Point b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] inline float cross(Point a, Point b) noexcept { return a.x * b.y - a.y * b.x; }

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] float right() const noexcept { return x + w; }
    [[nodiscard]] float bottom() const noexcept { return y + h; }
    [[nodiscard]] bool contains(Point p) const noexcept { return p.x >= x && p.y >= y && p.x < right() && p.y < bottom(); }
    [[nodiscard]] bool empty() const noexcept { return w <= 0 || h <= 0; }
};

/// Affine transform: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Transform {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    [[nodiscard]] static Transform translate(float tx, float ty) noexcept { return {1, 0, 0, 1, tx, ty}; }
    [[nodiscard]] static Transform scale(float sx, float sy) noexcept { return {sx, 0, 0, sy, 0, 0}; }
    [[nodiscard]] static Transform rotate(float radians) noexcept {
        const float s = std::sin(radians), c = std::cos(radians);
        return {c, s, -s, c, 0, 0};
    }
    [[nodiscard]] static Transform rotate(float radians, Point center) noexcept {
        return translate(center.x, center.y) * rotate(radians) * translate(-center.x, -center.y);
    }
    [[nodiscard]] static Transform skew(float ax, float ay) noexcept { return {1, std::tan(ay), std::tan(ax), 1, 0, 0}; }

    /// (*this * o)(p) == (*this)(o(p)): o is applied first.
    [[nodiscard]] Transform operator*(const Transform& o) const noexcept {
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    [[nodiscard]] Point operator()(Point p) const noexcept { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    [[nodiscard]] Point apply_vector(Point p) const noexcept { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
    [[nodiscard]] float determinant() const noexcept { return a * d - b * c; }
    [[nodiscard]] std::optional<Transform> inverse() const noexcept {
        const float det = determinant();
        if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) return std::nullopt;
        const float id = 1.0f / det;
        return Transform{d * id, -b * id, -c * id, a * id, (c * f - d * e) * id, (b * e - a * f) * id};
    }
    /// Geometric-mean scale factor (used to choose flattening tolerances).
    [[nodiscard]] float mean_scale() const noexcept { return std::sqrt(std::fabs(determinant())); }
    [[nodiscard]] float max_scale() const noexcept { return std::max(std::hypot(a, b), std::hypot(c, d)); }
    friend bool operator==(const Transform&, const Transform&) noexcept = default;
};

struct Polyline {
    std::vector<Point> pts;
    bool closed = false;
};

enum class Verb : u8 { move, line, quad, cubic, close };

class Path {
public:
    Path& move_to(float x, float y) { verbs_.push_back(Verb::move); pts_.push_back({x, y}); start_ = cur_ = {x, y}; have_ = true; return *this; }
    Path& line_to(float x, float y) { ensure(); verbs_.push_back(Verb::line); pts_.push_back({x, y}); cur_ = {x, y}; return *this; }
    Path& quad_to(float cx, float cy, float x, float y) {
        ensure();
        verbs_.push_back(Verb::quad);
        pts_.push_back({cx, cy}), pts_.push_back({x, y});
        cur_ = {x, y};
        return *this;
    }
    Path& cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        ensure();
        verbs_.push_back(Verb::cubic);
        pts_.push_back({c1x, c1y}), pts_.push_back({c2x, c2y}), pts_.push_back({x, y});
        cur_ = {x, y};
        return *this;
    }
    Path& close() {
        if (have_ && !verbs_.empty() && verbs_.back() != Verb::close) { verbs_.push_back(Verb::close); cur_ = start_; }
        return *this;
    }

    /// Circular arc around (cx,cy) from a0 to a1 (radians, y axis pointing down: positive angles go clockwise on screen).
    Path& arc(float cx, float cy, float r, float a0, float a1, bool counter_clockwise = false) {
        if (r < 0) throw GraphicsException("negative arc radius");
        float sweep = a1 - a0;
        if (!counter_clockwise) {
            if (sweep >= 2 * kPi) sweep = 2 * kPi;
            else while (sweep < 0) sweep += 2 * kPi;
        } else {
            if (sweep <= -2 * kPi) sweep = -2 * kPi;
            else while (sweep > 0) sweep -= 2 * kPi;
        }
        const Point s{cx + r * std::cos(a0), cy + r * std::sin(a0)};
        if (have_ && !(verbs_.empty())) line_to(s.x, s.y); else move_to(s.x, s.y);
        arc_segments(cx, cy, r, r, 0, a0, sweep);
        return *this;
    }
    Path& ellipse(float cx, float cy, float rx, float ry, float rotation = 0) {
        const float c = std::cos(rotation), s = std::sin(rotation);
        auto P = [&](float t) { return Point{cx + rx * std::cos(t) * c - ry * std::sin(t) * s, cy + rx * std::cos(t) * s + ry * std::sin(t) * c}; };
        const Point p0 = P(0);
        move_to(p0.x, p0.y);
        arc_segments(cx, cy, rx, ry, rotation, 0, 2 * kPi);
        return close();
    }
    Path& circle(float cx, float cy, float r) { return ellipse(cx, cy, r, r); }
    Path& rect(float x, float y, float w, float h) {
        move_to(x, y), line_to(x + w, y), line_to(x + w, y + h), line_to(x, y + h);
        return close();
    }
    Path& rounded_rect(float x, float y, float w, float h, float rx, float ry = -1) {
        if (ry < 0) ry = rx;
        rx = std::min(std::fabs(rx), std::fabs(w) / 2), ry = std::min(std::fabs(ry), std::fabs(h) / 2);
        if (rx <= 0 || ry <= 0) return rect(x, y, w, h);
        move_to(x + rx, y);
        line_to(x + w - rx, y);
        arc_segments(x + w - rx, y + ry, rx, ry, 0, -kPi / 2, kPi / 2);
        line_to(x + w, y + h - ry);
        arc_segments(x + w - rx, y + h - ry, rx, ry, 0, 0, kPi / 2);
        line_to(x + rx, y + h);
        arc_segments(x + rx, y + h - ry, rx, ry, 0, kPi / 2, kPi / 2);
        line_to(x, y + ry);
        arc_segments(x + rx, y + ry, rx, ry, 0, kPi, kPi / 2);
        return close();
    }
    Path& polygon(const std::vector<Point>& p, bool closed = true) {
        for (usize i = 0; i < p.size(); ++i) i == 0 ? move_to(p[i].x, p[i].y) : line_to(p[i].x, p[i].y);
        return closed ? close() : *this;
    }
    /// SVG elliptical arc (endpoint parameterisation) from the current point.
    Path& arc_to(float rx, float ry, float x_axis_rotation_deg, bool large_arc, bool sweep, float x, float y) {
        ensure();
        const Point p0 = cur_;
        rx = std::fabs(rx), ry = std::fabs(ry);
        if (p0.x == x && p0.y == y) return *this;
        if (rx == 0 || ry == 0) return line_to(x, y);
        const float phi = x_axis_rotation_deg * kPi / 180.0f, cp = std::cos(phi), sp = std::sin(phi);
        const float dx2 = (p0.x - x) / 2, dy2 = (p0.y - y) / 2;
        const float x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
        const float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lam > 1) { rx *= std::sqrt(lam), ry *= std::sqrt(lam); }
        const float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        const float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        float co = den == 0 ? 0 : std::sqrt(std::max(0.0f, num / den));
        if (large_arc == sweep) co = -co;
        const float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
        const float cx = cp * cxp - sp * cyp + (p0.x + x) / 2, cy = sp * cxp + cp * cyp + (p0.y + y) / 2;
        auto ang = [](float ux, float uy, float vx, float vy) {
            const float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return a;
        };
        const float th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        float dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * kPi;
        else if (sweep && dth < 0) dth += 2 * kPi;
        arc_segments(cx, cy, rx, ry, phi, th1, dth);
        cur_ = {x, y};
        pts_.back() = {x, y};  // remove drift on the last control point
        return *this;
    }

    void append(const Path& o) {
        verbs_.insert(verbs_.end(), o.verbs_.begin(), o.verbs_.end());
        pts_.insert(pts_.end(), o.pts_.begin(), o.pts_.end());
        if (o.have_) { cur_ = o.cur_; start_ = o.start_; have_ = true; }
    }
    [[nodiscard]] Path transformed(const Transform& t) const {
        Path p = *this;
        for (auto& q : p.pts_) q = t(q);
        p.cur_ = t(cur_), p.start_ = t(start_);
        return p;
    }
    [[nodiscard]] bool empty() const noexcept { return verbs_.empty(); }
    [[nodiscard]] const std::vector<Verb>& verbs() const noexcept { return verbs_; }
    [[nodiscard]] const std::vector<Point>& points() const noexcept { return pts_; }
    void clear() { verbs_.clear(); pts_.clear(); have_ = false; }

    /// Convert to polylines with maximum deviation `tolerance` (in path units).
    [[nodiscard]] std::vector<Polyline> flatten(float tolerance = 0.1f) const {
        std::vector<Polyline> out;
        usize pi = 0;
        Point cur{}, start{};
        auto cur_line = [&]() -> Polyline& {
            if (out.empty() || out.back().closed) { out.push_back({}); out.back().pts.push_back(cur); }
            return out.back();
        };
        const float tol = std::max(tolerance, 1e-4f);
        for (Verb v : verbs_) {
            switch (v) {
                case Verb::move:
                    cur = start = pts_[pi++];
                    out.push_back({});
                    out.back().pts.push_back(cur);
                    break;
                case Verb::line: {
                    Polyline& pl = cur_line();
                    cur = pts_[pi++];
                    pl.pts.push_back(cur);
                    break;
                }
                case Verb::quad: {
                    Polyline& pl = cur_line();
                    const Point c = pts_[pi], e = pts_[pi + 1];
                    pi += 2;
                    const float dd = length(cur - c * 2.0f + e);
                    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd / (4 * tol)))), 1, 500);
                    for (int i = 1; i <= n; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
                        pl.pts.push_back(cur * (u * u) + c * (2 * u * t) + e * (t * t));
                    }
                    cur = e;
                    break;
                }
                case Verb::cubic: {
                    Polyline& pl = cur_line();
                    const Point c1 = pts_[pi], c2 = pts_[pi + 1], e = pts_[pi + 2];
                    pi += 3;
                    const float dd = std::max(length(cur - c1 * 2.0f + c2), length(c1 - c2 * 2.0f + e));
                    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(0.75f * dd / tol))), 1, 500);
                    for (int i = 1; i <= n; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(n), u = 1 - t;
                        pl.pts.push_back(cur * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + e * (t * t * t));
                    }
                    cur = e;
                    break;
                }
                case Verb::close:
                    if (!out.empty() && !out.back().closed) out.back().closed = true;
                    cur = start;
                    break;
            }
        }
        // drop lone moves
        out.erase(std::remove_if(out.begin(), out.end(), [](const Polyline& p) { return p.pts.size() < 2 && !p.closed; }), out.end());
        return out;
    }

    /// Axis-aligned bounds of the control polygon (conservative for curves).
    [[nodiscard]] Rect bounds() const {
        if (pts_.empty()) return {};
        float x0 = pts_[0].x, y0 = pts_[0].y, x1 = x0, y1 = y0;
        for (const Point& p : pts_) { x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y); }
        return {x0, y0, x1 - x0, y1 - y0};
    }

    /// Parse SVG path data ("M10 10 L20 20 C... Z"). Throws GraphicsException on malformed input.
    [[nodiscard]] static Path parse_svg(std::string_view d) {
        Path path;
        usize i = 0;
        char cmd = 0;
        Point cur{}, start{}, last_c{};
        char prev = 0;
        auto skip = [&] { while (i < d.size() && (d[i] == ' ' || d[i] == ',' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r')) ++i; };
        auto num = [&]() -> float {
            skip();
            if (i >= d.size()) throw GraphicsException("svg path: unexpected end of data");
            const std::string s(d.substr(i, std::min<usize>(40, d.size() - i)));
            char* end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str()) throw GraphicsException("svg path: expected a number");
            i += static_cast<usize>(end - s.c_str());
            return v;
        };
        auto flag = [&]() -> bool {
            skip();
            if (i >= d.size() || (d[i] != '0' && d[i] != '1')) throw GraphicsException("svg path: expected an arc flag");
            return d[i++] == '1';
        };
        for (;;) {
            skip();
            if (i >= d.size()) break;
            const char c = d[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; ++i; }
            else if (cmd == 0) throw GraphicsException("svg path: data must start with a command");
            else if (cmd == 'M') cmd = 'L';
            else if (cmd == 'm') cmd = 'l';
            else if (cmd == 'Z' || cmd == 'z') throw GraphicsException("svg path: unexpected number after close");
            if (path.empty() && cmd != 'M' && cmd != 'm') throw GraphicsException("svg path: data must start with a moveto");
            const bool rel = cmd >= 'a';
            const char up = static_cast<char>(rel ? cmd - 32 : cmd);
            const float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            switch (up) {
                case 'M': { const float x = num() + ox, y = num() + oy; path.move_to(x, y); cur = start = {x, y}; break; }
                case 'L': { const float x = num() + ox, y = num() + oy; path.line_to(x, y); cur = {x, y}; break; }
                case 'H': { const float x = num() + ox; path.line_to(x, cur.y); cur.x = x; break; }
                case 'V': { const float y = num() + oy; path.line_to(cur.x, y); cur.y = y; break; }
                case 'C': {
                    const float x1 = num() + ox, y1 = num() + oy, x2 = num() + ox, y2 = num() + oy, x = num() + ox, y = num() + oy;
                    path.cubic_to(x1, y1, x2, y2, x, y);
                    last_c = {x2, y2};
                    cur = {x, y};
                    break;
                }
                case 'S': {
                    const Point r = (prev == 'C' || prev == 'S') ? Point{2 * cur.x - last_c.x, 2 * cur.y - last_c.y} : cur;
                    const float x2 = num() + ox, y2 = num() + oy, x = num() + ox, y = num() + oy;
                    path.cubic_to(r.x, r.y, x2, y2, x, y);
                    last_c = {x2, y2};
                    cur = {x, y};
                    break;
                }
                case 'Q': {
                    const float x1 = num() + ox, y1 = num() + oy, x = num() + ox, y = num() + oy;
                    path.quad_to(x1, y1, x, y);
                    last_c = {x1, y1};
                    cur = {x, y};
                    break;
                }
                case 'T': {
                    const Point r = (prev == 'Q' || prev == 'T') ? Point{2 * cur.x - last_c.x, 2 * cur.y - last_c.y} : cur;
                    const float x = num() + ox, y = num() + oy;
                    path.quad_to(r.x, r.y, x, y);
                    last_c = r;
                    cur = {x, y};
                    break;
                }
                case 'A': {
                    const float rx = num(), ry = num(), rot = num();
                    const bool la = flag(), sw = flag();
                    const float x = num() + ox, y = num() + oy;
                    path.arc_to(rx, ry, rot, la, sw, x, y);
                    cur = {x, y};
                    break;
                }
                case 'Z': path.close(); cur = start; break;
                default: throw GraphicsException(std::string("svg path: unknown command '") + cmd + "'");
            }
            prev = up;
        }
        return path;
    }

private:
    void ensure() { if (!have_) move_to(0, 0); }
    /// Append cubic Bezier approximations of an elliptical arc (current point must be its start).
    void arc_segments(float cx, float cy, float rx, float ry, float rot, float a0, float sweep) {
        const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 2) - 1e-4f)));
        const float da = sweep / static_cast<float>(n);
        const float k = 4.0f / 3.0f * std::tan(da / 4);
        const float cr = std::cos(rot), sr = std::sin(rot);
        auto map = [&](float ex, float ey) { return Point{cx + ex * cr - ey * sr, cy + ex * sr + ey * cr}; };
        for (int i = 0; i < n; ++i) {
            const float t0 = a0 + da * static_cast<float>(i), t1 = t0 + da;
            const float c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
            const Point p1 = map(rx * (c0 - k * s0), ry * (s0 + k * c0));
            const Point p2 = map(rx * (c1 + k * s1), ry * (s1 - k * c1));
            const Point p3 = map(rx * c1, ry * s1);
            cubic_to(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        }
    }
    std::vector<Verb> verbs_;
    std::vector<Point> pts_;
    Point cur_{}, start_{};
    bool have_ = false;
};

}  // namespace flyg::graphics
