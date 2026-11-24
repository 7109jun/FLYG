// flyG - graphics module: path stroker (caps, joins, miter limit, dashing) producing fill polygons
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <vector>

#include <flyg/graphics/path.hpp>

namespace flyg::graphics {

enum class LineCap { butt, round, square };
enum class LineJoin { miter, round, bevel };

struct StrokeStyle {
    float width = 1;
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;
    float miter_limit = 4;
    std::vector<float> dash;   ///< alternating on/off lengths; empty = solid
    float dash_offset = 0;
};

/// Result of stroking: positively oriented polygons whose non-zero union is the stroke.
using PolygonSet = std::vector<std::vector<Point>>;

namespace stroke_detail {

inline void orient(std::vector<Point>& poly) {
    double a = 0;
    for (usize i = 0; i < poly.size(); ++i) {
        const Point p = poly[i], q = poly[(i + 1) % poly.size()];
        a += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    if (a < 0) std::reverse(poly.begin(), poly.end());
}

inline void add(PolygonSet& out, std::vector<Point> poly) {
    if (poly.size() < 3) return;
    orient(poly);
    out.push_back(std::move(poly));
}

inline int arc_steps(float radius, float angle, float tol) {
    if (radius <= tol) return std::max(1, static_cast<int>(std::ceil(std::fabs(angle) / (kPi / 2))));
    const float step = 2 * std::acos(std::clamp(1 - tol / radius, -1.0f, 1.0f));
    return std::clamp(static_cast<int>(std::ceil(std::fabs(angle) / std::max(step, 0.05f))), 1, 400);
}

inline std::vector<Polyline> dashed(const std::vector<Polyline>& in, const std::vector<float>& pattern_in, float offset) {
    std::vector<float> pat = pattern_in;
    float total = 0;
    for (float v : pat) {
        if (v < 0 || !std::isfinite(v)) throw GraphicsException("dash lengths must be non-negative");
        total += v;
    }
    if (pat.empty() || total <= 0) return in;
    if (pat.size() % 2) pat.insert(pat.end(), pattern_in.begin(), pattern_in.end());
    total = 0;
    for (float v : pat) total += v;
    std::vector<Polyline> out;
    for (const Polyline& pl : in) {
        std::vector<Point> pts = pl.pts;
        if (pl.closed && !pts.empty()) pts.push_back(pts.front());
        if (pts.size() < 2) continue;
        float o = std::fmod(offset, total);
        if (o < 0) o += total;
        usize idx = 0;
        while (o >= pat[idx]) { o -= pat[idx]; idx = (idx + 1) % pat.size(); if (pat[idx] == 0 && o == 0) break; }
        float remain = pat[idx] - o;
        bool on = idx % 2 == 0;
        const usize first_new = out.size();
        Polyline cur;
        if (on) cur.pts.push_back(pts[0]);
        bool starts_on = on;
        for (usize i = 0; i + 1 < pts.size(); ++i) {
            Point a = pts[i];
            const Point b = pts[i + 1];
            float seg = length(b - a);
            while (seg > remain) {
                const Point m = a + (b - a) * (remain / seg);
                if (on) { cur.pts.push_back(m); out.push_back(std::move(cur)); cur = {}; }
                else cur.pts = {m};
                seg -= remain;
                a = m;
                idx = (idx + 1) % pat.size();
                remain = pat[idx];
                on = !on;
                if (out.size() - first_new > 200000) throw GraphicsException("dash pattern produces too many segments");
            }
            remain -= seg;
            if (on) cur.pts.push_back(b);
        }
        if (on && cur.pts.size() >= 2) {
            if (pl.closed && starts_on && out.size() > first_new) {  // merge the tail with the first dash
                auto& head = out[first_new].pts;
                cur.pts.insert(cur.pts.end(), head.begin() + 1, head.end());
                head = std::move(cur.pts);
            } else {
                out.push_back(std::move(cur));
            }
        }
    }
    return out;
}

}  // namespace stroke_detail

/// Stroke `path` in its own coordinate space. `tolerance` bounds the error of round caps/joins in path units.
[[nodiscard]] inline PolygonSet stroke(const Path& path, const StrokeStyle& st, float tolerance = 0.1f) {
    using namespace stroke_detail;
    if (!(st.width > 0) || !std::isfinite(st.width)) throw GraphicsException("stroke width must be positive");
    const float hw = st.width / 2;
    PolygonSet out;
    std::vector<Polyline> lines = path.flatten(tolerance);
    if (!st.dash.empty()) lines = dashed(lines, st.dash, st.dash_offset);

    auto disc_wedge = [&](Point c, float a0, float sweep) {  // pie slice (sweep may be a full circle)
        const int n = arc_steps(hw, sweep, tolerance);
        std::vector<Point> poly;
        if (std::fabs(sweep) < 2 * kPi - 1e-4f) poly.push_back(c);
        for (int i = 0; i <= n; ++i) {
            const float a = a0 + sweep * static_cast<float>(i) / static_cast<float>(n);
            poly.push_back({c.x + hw * std::cos(a), c.y + hw * std::sin(a)});
        }
        add(out, std::move(poly));
    };

    for (const Polyline& src : lines) {
        std::vector<Point> p;
        for (const Point& q : src.pts)
            if (p.empty() || length(q - p.back()) > 1e-6f) p.push_back(q);
        bool closed = src.closed;
        if (closed && p.size() > 1 && length(p.front() - p.back()) <= 1e-6f) p.pop_back();
        if (p.size() == 1 || (closed && p.size() == 2 && false)) {
            // zero-length subpath: dot for round caps, square for square caps
            if (st.cap == LineCap::round) disc_wedge(p[0], 0, 2 * kPi);
            else if (st.cap == LineCap::square) add(out, {{p[0].x - hw, p[0].y - hw}, {p[0].x + hw, p[0].y - hw}, {p[0].x + hw, p[0].y + hw}, {p[0].x - hw, p[0].y + hw}});
            continue;
        }
        if (p.size() < 2) continue;
        if (closed && p.size() < 3) closed = false;
        const usize n = p.size();
        const usize segs = closed ? n : n - 1;
        std::vector<Point> dir(segs), nor(segs);
        for (usize i = 0; i < segs; ++i) {
            const Point d = p[(i + 1) % n] - p[i];
            const float l = length(d);
            dir[i] = {d.x / l, d.y / l};
            nor[i] = {-dir[i].y, dir[i].x};
        }
        for (usize i = 0; i < segs; ++i) {
            const Point a = p[i], b = p[(i + 1) % n], o = nor[i] * hw;
            add(out, {a + o, b + o, b - o, a - o});
        }
        auto join = [&](usize i0, usize i1, Point v) {
            const Point d0 = dir[i0], d1 = dir[i1];
            const float cr = cross(d0, d1), dt = dot(d0, d1);
            if (std::fabs(cr) < 1e-6f && dt > 0) return;
            const float s = cr > 0 ? -1.0f : 1.0f;  // side of the outer corner
            const Point n0 = nor[i0] * s, n1 = nor[i1] * s;
            const Point e0 = v + n0 * hw, e1 = v + n1 * hw;
            switch (st.join) {
                case LineJoin::bevel: add(out, {v, e0, e1}); break;
                case LineJoin::miter: {
                    const float cos_half = std::sqrt(std::max(0.0f, (1 + dt) / 2));
                    if (cos_half > 1e-4f && 1 / cos_half <= st.miter_limit) {
                        const Point tip = v + (n0 + n1) * (hw / (1 + dt));
                        add(out, {v, e0, tip, e1});
                    } else {
                        add(out, {v, e0, e1});
                    }
                    break;
                }
                case LineJoin::round: {
                    const float a0 = std::atan2(n0.y, n0.x);
                    float sweep = std::atan2(cross(n0, n1), dot(n0, n1));
                    if (std::fabs(cr) < 1e-6f) sweep = s > 0 ? kPi : -kPi;  // 180 degree turn
                    disc_wedge(v, a0, sweep);
                    break;
                }
            }
        };
        for (usize i = 1; i < segs; ++i) join(i - 1, i, p[i]);
        if (closed) join(segs - 1, 0, p[0]);
        else {
            auto cap = [&](Point v, Point d) {  // d points out of the line end
                const Point nn{-d.y, d.x};
                if (st.cap == LineCap::square) add(out, {v + nn * hw, v + nn * hw + d * hw, v - nn * hw + d * hw, v - nn * hw});
                else if (st.cap == LineCap::round) disc_wedge(v, std::atan2(nn.y, nn.x), -kPi);
            };
            cap(p[0], dir[0] * -1.0f);
            cap(p[n - 1], dir[segs - 1]);
        }
    }
    return out;
}

}  // namespace flyg::graphics
