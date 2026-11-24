// flyG - graphics module: anti-aliased scanline polygon rasteriser (16 vertical sub-scanlines, exact horizontal coverage)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <flyg/graphics/path.hpp>
#include <flyg/graphics/stroke.hpp>

namespace flyg::graphics {

enum class FillRule { non_zero, even_odd };

namespace raster {

inline constexpr int kSubScanlines = 16;

struct Edge {
    float x0, y0, x1, y1;  // y0 < y1
    int dir;               // +1 if the original edge went downwards
};

/// Rasterise polygons (implicitly closed) into per-row coverage values in [0,1].
/// `emit(y, x_begin, x_end, coverage)` is called for every row that has coverage; coverage[i] belongs to pixel x_begin+i.
/// Output is clipped to [0,width) x [0,height).
inline void fill(const PolygonSet& polys, FillRule rule, int width, int height,
                 const std::function<void(int y, int xb, int xe, const float* cov)>& emit) {
    if (width <= 0 || height <= 0) return;
    std::vector<Edge> edges;
    float ymin = 1e30f, ymax = -1e30f;
    for (const auto& poly : polys) {
        const usize n = poly.size();
        if (n < 3) continue;
        for (usize i = 0; i < n; ++i) {
            Point a = poly[i], b = poly[(i + 1) % n];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
            if (a.y == b.y) continue;
            int dir = 1;
            if (a.y > b.y) { std::swap(a, b); dir = -1; }
            edges.push_back({a.x, a.y, b.x, b.y, dir});
            ymin = std::min(ymin, a.y);
            ymax = std::max(ymax, b.y);
        }
    }
    if (edges.empty()) return;
    const int row0 = std::max(0, static_cast<int>(std::floor(ymin)));
    const int row1 = std::min(height - 1, static_cast<int>(std::floor(ymax)));
    if (row0 > row1) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });

    std::vector<float> acc(static_cast<usize>(width) + 2), delta(static_cast<usize>(width) + 2), cov(static_cast<usize>(width));
    struct Cross { float x; int dir; };
    std::vector<Cross> xs;
    std::vector<usize> active;
    usize next = 0;
    constexpr float inv = 1.0f / static_cast<float>(kSubScanlines);
    for (int y = row0; y <= row1; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        std::fill(delta.begin(), delta.end(), 0.0f);
        int xmin = width, xmax = -1;
        for (int s = 0; s < kSubScanlines; ++s) {
            const float sy = static_cast<float>(y) + (static_cast<float>(s) + 0.5f) * inv;
            while (next < edges.size() && edges[next].y0 <= sy) active.push_back(next++);
            xs.clear();
            for (usize k = 0; k < active.size();) {
                const Edge& e = edges[active[k]];
                if (e.y1 <= sy) { active[k] = active.back(); active.pop_back(); continue; }
                if (e.y0 <= sy) xs.push_back({e.x0 + (e.x1 - e.x0) * (sy - e.y0) / (e.y1 - e.y0), e.dir});
                ++k;
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.x < b.x; });
            int wind = 0;
            for (usize k = 0; k + 1 < xs.size(); ++k) {
                wind += xs[k].dir;
                const bool inside = rule == FillRule::non_zero ? wind != 0 : (wind & 1) != 0;
                if (!inside) continue;
                float xa = std::max(xs[k].x, 0.0f), xb = std::min(xs[k + 1].x, static_cast<float>(width));
                if (xb <= xa) continue;
                const int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
                xmin = std::min(xmin, ia);
                xmax = std::max(xmax, std::min(ib, width - 1));
                if (ia == ib) {
                    acc[static_cast<usize>(ia)] += (xb - xa) * inv;
                } else {
                    acc[static_cast<usize>(ia)] += (static_cast<float>(ia + 1) - xa) * inv;
                    delta[static_cast<usize>(ia + 1)] += inv;
                    delta[static_cast<usize>(ib)] -= inv;
                    if (ib < width) acc[static_cast<usize>(ib)] += (xb - static_cast<float>(ib)) * inv;
                }
            }
        }
        if (xmax < xmin) continue;
        float run = 0;
        for (int x = 0; x < xmin; ++x) run += delta[static_cast<usize>(x)];
        for (int x = xmin; x <= xmax; ++x) {
            run += delta[static_cast<usize>(x)];
            cov[static_cast<usize>(x - xmin)] = std::clamp(acc[static_cast<usize>(x)] + run, 0.0f, 1.0f);
        }
        emit(y, xmin, xmax + 1, cov.data());
    }
}

}  // namespace raster
}  // namespace flyg::graphics
