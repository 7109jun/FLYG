// flyG - graphics module: software 3D rasteriser (programmable vertex/fragment shaders, homogeneous clipping,
// perspective-correct interpolation, z-buffer, culling, blending, optional supersampling anti-aliasing)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <concepts>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

#include <flyg/graphics/image.hpp>
#include <flyg/graphics/mesh.hpp>

namespace flyg::graphics {

enum class CullMode { none, back, front };
enum class DepthFunc { less, lequal, greater, gequal, equal, not_equal, always, never };
enum class PolygonMode { fill, line, point };

struct DrawState {
    CullMode cull = CullMode::back;
    bool front_ccw = true;
    bool depth_test = true;
    bool depth_write = true;
    DepthFunc depth_func = DepthFunc::less;
    bool blend = false;                  ///< source-over blending with the fragment alpha
    PolygonMode polygon = PolygonMode::fill;
    bool linear_output = true;           ///< fragment colours are linear light and get sRGB-encoded when written
    bool scissor = false;                ///< restrict drawing to the rectangle below (output pixels, origin top-left)
    long scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;
};

struct RenderStats {
    usize triangles_submitted = 0, triangles_rasterised = 0, fragments_shaded = 0;
};

/// A shader supplies `Varying` (needs operator+ and operator*(float)), `vertex()` and `fragment()`.
/// fragment() may take (varying) or (varying, ddx, ddy) when the shader declares `static constexpr bool needs_derivatives = true`.
template <class S>
concept VertexShader = requires(S s, const Vertex& v, typename S::Varying& out) {
    typename S::Varying;
    { s.vertex(v, out) } -> std::convertible_to<Vector4f>;
};

class Renderer {
public:
    /// `supersample` (1..4) renders at supersample^2 samples per pixel and averages them in resolve().
    Renderer(usize width, usize height, unsigned supersample = 1)
        : w_(width), h_(height), ss_(supersample) {
        if (width == 0 || height == 0) throw GraphicsException("renderer needs a non-empty target");
        if (supersample < 1 || supersample > 4) throw GraphicsException("supersample factor must be 1..4");
        fw_ = w_ * ss_, fh_ = h_ * ss_;
        if (fw_ > (usize(1) << 15) || fh_ > (usize(1) << 15) || fw_ * fh_ > (usize(1) << 28)) throw GraphicsException("render target too large");
        color_.assign(fw_ * fh_, Color{0, 0, 0, 0});
        depth_.assign(fw_ * fh_, 1.0f);
    }

    [[nodiscard]] usize width() const noexcept { return w_; }
    [[nodiscard]] usize height() const noexcept { return h_; }
    [[nodiscard]] float aspect() const noexcept { return static_cast<float>(w_) / static_cast<float>(h_); }
    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    void reset_stats() { stats_ = {}; }

    void clear(Color c = Color{0, 0, 0, 0}, float depth = 1.0f) {
        std::fill(color_.begin(), color_.end(), c);
        std::fill(depth_.begin(), depth_.end(), depth);
    }
    void clear_color(Color c) { std::fill(color_.begin(), color_.end(), c); }
    void clear_depth(float d = 1.0f) { std::fill(depth_.begin(), depth_.end(), d); }

    /// Depth in [0,1] of full-resolution sample (x,y) (x,y in supersampled coordinates).
    [[nodiscard]] float depth_at(usize x, usize y) const { return depth_.at(y * fw_ + x); }

    /// Draw an indexed triangle mesh.
    template <VertexShader S>
    void draw(const Mesh& mesh, S& shader, const DrawState& st = {}) {
        mesh.validate();
        draw(std::span<const Vertex>(mesh.vertices), std::span<const u32>(mesh.indices), shader, st);
    }
    /// Generic form: `VertexT` is whatever the shader's vertex() accepts.
    template <class VertexT, class S>
    void draw(std::span<const VertexT> verts, std::span<const u32> idx, S& shader, const DrawState& st = {}) {
        using V = typename S::Varying;
        if (idx.size() % 3) throw GraphicsException("index count must be a multiple of 3");
        struct Out { Vector4f clip; V v; };
        std::vector<Out> out(verts.size());
        for (usize i = 0; i < verts.size(); ++i) out[i].clip = shader.vertex(verts[i], out[i].v);
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            if (idx[t] >= verts.size() || idx[t + 1] >= verts.size() || idx[t + 2] >= verts.size()) throw GraphicsException("index out of range");
            ++stats_.triangles_submitted;
            std::array<CV<V>, 3> tri = {CV<V>{out[idx[t]].clip, out[idx[t]].v}, CV<V>{out[idx[t + 1]].clip, out[idx[t + 1]].v}, CV<V>{out[idx[t + 2]].clip, out[idx[t + 2]].v}};
            process_triangle(tri, shader, st);
        }
    }

    /// Replace the colour buffer with `img` (resized to the supersampled target with nearest sampling when needed).
    void load_color(const Image& img) {
        if (img.width() != w_ || img.height() != h_) throw GraphicsException("load_color: size mismatch");
        for (usize y = 0; y < fh_; ++y)
            for (usize x = 0; x < fw_; ++x) color_[y * fw_ + x] = img(x / ss_, y / ss_);
    }

    /// Averaged final image (size width x height).
    [[nodiscard]] Image resolve() const {
        Image full(fw_, fh_);
        std::copy(color_.begin(), color_.end(), full.data());
        return ss_ == 1 ? full : resize(full, w_, h_, Filter::area);
    }

private:
    template <class V>
    struct CV { Vector4f p; V v; };
    template <class V>
    struct SV { float x, y, z, inv_w; V v; };

    template <class V>
    static CV<V> lerp(const CV<V>& a, const CV<V>& b, float t) {
        return {Vector4f{a.p.x + (b.p.x - a.p.x) * t, a.p.y + (b.p.y - a.p.y) * t, a.p.z + (b.p.z - a.p.z) * t, a.p.w + (b.p.w - a.p.w) * t}, a.v * (1 - t) + b.v * t};
    }
    static float plane_dist(const Vector4f& p, int plane) noexcept {
        switch (plane) {
            case 0: return p.w + p.x;
            case 1: return p.w - p.x;
            case 2: return p.w + p.y;
            case 3: return p.w - p.y;
            case 4: return p.w + p.z;
            default: return p.w - p.z;
        }
    }

    template <class V, class S>
    void process_triangle(const std::array<CV<V>, 3>& tri, S& shader, const DrawState& st) {
        for (const auto& c : tri)
            if (!std::isfinite(c.p.x) || !std::isfinite(c.p.y) || !std::isfinite(c.p.z) || !std::isfinite(c.p.w)) return;
        // trivial reject / accept
        bool all_in = true;
        for (int pl = 0; pl < 6; ++pl) {
            int outside = 0;
            for (const auto& c : tri) if (plane_dist(c.p, pl) < 0) ++outside;
            if (outside == 3) return;
            if (outside) all_in = false;
        }
        std::vector<CV<V>> poly(tri.begin(), tri.end());
        if (!all_in) {
            for (int pl = 0; pl < 6 && !poly.empty(); ++pl) {
                std::vector<CV<V>> next;
                for (usize i = 0; i < poly.size(); ++i) {
                    const CV<V>& a = poly[i];
                    const CV<V>& b = poly[(i + 1) % poly.size()];
                    const float da = plane_dist(a.p, pl), db = plane_dist(b.p, pl);
                    if (da >= 0) next.push_back(a);
                    if ((da >= 0) != (db >= 0)) next.push_back(lerp(a, b, da / (da - db)));
                }
                poly = std::move(next);
            }
            if (poly.size() < 3) return;
        }
        std::vector<SV<V>> sv;
        sv.reserve(poly.size());
        for (const auto& c : poly) {
            if (c.p.w <= 1e-12f) return;
            const float iw = 1.0f / c.p.w;
            sv.push_back({(c.p.x * iw * 0.5f + 0.5f) * static_cast<float>(fw_), (1.0f - (c.p.y * iw * 0.5f + 0.5f)) * static_cast<float>(fh_), c.p.z * iw * 0.5f + 0.5f, iw, c.v});
        }
        // facing (use the clipped polygon's first triangle; clipping preserves orientation)
        const double area2 = (static_cast<double>(sv[1].x) - sv[0].x) * (static_cast<double>(sv[2].y) - sv[0].y) - (static_cast<double>(sv[2].x) - sv[0].x) * (static_cast<double>(sv[1].y) - sv[0].y);
        if (area2 == 0) return;
        const bool front = st.front_ccw ? area2 < 0 : area2 > 0;  // y is flipped on screen
        if (st.cull == CullMode::back && !front) return;
        if (st.cull == CullMode::front && front) return;
        ++stats_.triangles_rasterised;
        if (st.polygon == PolygonMode::fill) {
            for (usize i = 1; i + 1 < sv.size(); ++i) raster_triangle(sv[0], sv[i], sv[i + 1], shader, st);
        } else if (st.polygon == PolygonMode::line) {
            for (usize i = 0; i < sv.size(); ++i) raster_line(sv[i], sv[(i + 1) % sv.size()], shader, st);
        } else {
            for (const auto& p : sv) raster_point(p, shader, st);
        }
    }

    static bool depth_pass(DepthFunc f, float z, float old) noexcept {
        switch (f) {
            case DepthFunc::less: return z < old;
            case DepthFunc::lequal: return z <= old;
            case DepthFunc::greater: return z > old;
            case DepthFunc::gequal: return z >= old;
            case DepthFunc::equal: return z == old;
            case DepthFunc::not_equal: return z != old;
            case DepthFunc::always: return true;
            case DepthFunc::never: return false;
        }
        return false;
    }

    static const std::array<float, 256>& srgb_to_lin() { return color::srgb8_to_linear_table(); }
    static u8 lin_to_srgb8(float v) noexcept {
        static const std::array<u8, 4097> lut = [] {
            std::array<u8, 4097> t{};
            for (usize i = 0; i < t.size(); ++i) t[i] = static_cast<u8>(std::clamp(color::linear_to_srgb(static_cast<float>(i) / 4096.0f) * 255.0f + 0.5f, 0.0f, 255.0f));
            return t;
        }();
        return lut[static_cast<usize>(std::clamp(v, 0.0f, 1.0f) * 4096.0f + 0.5f)];
    }

    template <class S, class V>
    static auto call_fragment(S& shader, const V& v, const V& dx, const V& dy) {
        if constexpr (requires { S::needs_derivatives; } && requires { shader.fragment(v, dx, dy); }) return shader.fragment(v, dx, dy);
        else return shader.fragment(v);
    }
    template <class S>
    static constexpr bool wants_derivs() { return requires { S::needs_derivatives; }; }

    template <class V, class S>
    void write_fragment(usize x, usize y, float z, const V& v, const V& dx, const V& dy, S& shader, const DrawState& st) {
        const usize i = y * fw_ + x;
        if (st.depth_test && !depth_pass(st.depth_func, z, depth_[i])) return;
        std::optional<ColorF> frag = call_fragment(shader, v, dx, dy);
        ++stats_.fragments_shaded;
        if (!frag) return;
        ColorF c = *frag;
        if (st.depth_write) depth_[i] = z;
        const float a = std::clamp(c.a, 0.0f, 1.0f);
        Color& dst = color_[i];
        auto enc = [&](float f) -> u8 { return st.linear_output ? lin_to_srgb8(f) : static_cast<u8>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
        if (!st.blend || a >= 0.9999f) { dst = Color{enc(c.r), enc(c.g), enc(c.b), static_cast<u8>(a * 255.0f + 0.5f)}; return; }
        const float da = static_cast<float>(dst.a) / 255.0f;
        auto dec = [&](u8 b) { return st.linear_output ? srgb_to_lin()[b] : static_cast<float>(b) / 255.0f; };
        const float oa = a + da * (1 - a);
        auto mix = [&](float s, u8 d) { return oa > 0 ? (s * a + dec(d) * da * (1 - a)) / oa : 0.0f; };
        dst = Color{enc(mix(c.r, dst.r)), enc(mix(c.g, dst.g)), enc(mix(c.b, dst.b)), static_cast<u8>(oa * 255.0f + 0.5f)};
    }

    template <class V, class S>
    void raster_triangle(SV<V> a, SV<V> b, SV<V> c, S& shader, const DrawState& st) {
        auto edge = [](double ax, double ay, double bx, double by, double px, double py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
        double area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
        if (area == 0) return;
        if (area < 0) { std::swap(b, c); area = -area; }
        const double minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x}), miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
        long lo_x = 0, lo_y = 0, hi_x = static_cast<long>(fw_) - 1, hi_y = static_cast<long>(fh_) - 1;
        if (st.scissor) scissor_bounds(st, lo_x, lo_y, hi_x, hi_y);
        const long x0 = std::max<long>(lo_x, static_cast<long>(std::floor(minx))), x1 = std::min<long>(hi_x, static_cast<long>(std::ceil(maxx)));
        const long y0 = std::max<long>(lo_y, static_cast<long>(std::floor(miny))), y1 = std::min<long>(hi_y, static_cast<long>(std::ceil(maxy)));
        auto top_left = [](const SV<V>& p, const SV<V>& q) {
            const double dx = static_cast<double>(q.x) - p.x, dy = static_cast<double>(q.y) - p.y;
            return (dy == 0 && dx > 0) || dy < 0;
        };
        const bool tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);
        const double inv_area = 1.0 / area;
        auto interp = [&](double l0, double l1, double l2) {
            const double p0 = l0 * a.inv_w, p1 = l1 * b.inv_w, p2 = l2 * c.inv_w;
            const double s = p0 + p1 + p2;
            const float f0 = static_cast<float>(p0 / s), f1 = static_cast<float>(p1 / s), f2 = static_cast<float>(p2 / s);
            return a.v * f0 + b.v * f1 + c.v * f2;
        };
        for (long y = y0; y <= y1; ++y)
            for (long x = x0; x <= x1; ++x) {
                const double px = static_cast<double>(x) + 0.5, py = static_cast<double>(y) + 0.5;
                const double e0 = edge(b.x, b.y, c.x, c.y, px, py), e1 = edge(c.x, c.y, a.x, a.y, px, py), e2 = edge(a.x, a.y, b.x, b.y, px, py);
                if (e0 < 0 || e1 < 0 || e2 < 0) continue;
                if ((e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2)) continue;
                const double l0 = e0 * inv_area, l1 = e1 * inv_area, l2 = e2 * inv_area;
                const float z = static_cast<float>(l0 * a.z + l1 * b.z + l2 * c.z);
                if (z < 0.0f || z > 1.0f) continue;
                if (st.depth_test && !depth_pass(st.depth_func, z, depth_[static_cast<usize>(y) * fw_ + static_cast<usize>(x)])) continue;
                V v = interp(l0, l1, l2);
                if constexpr (wants_derivs<S>()) {
                    const double ex0 = edge(b.x, b.y, c.x, c.y, px + 1, py), ex1 = edge(c.x, c.y, a.x, a.y, px + 1, py), ex2 = edge(a.x, a.y, b.x, b.y, px + 1, py);
                    const double ey0 = edge(b.x, b.y, c.x, c.y, px, py + 1), ey1 = edge(c.x, c.y, a.x, a.y, px, py + 1), ey2 = edge(a.x, a.y, b.x, b.y, px, py + 1);
                    const V vx = interp(ex0 * inv_area, ex1 * inv_area, ex2 * inv_area), vy = interp(ey0 * inv_area, ey1 * inv_area, ey2 * inv_area);
                    write_fragment(static_cast<usize>(x), static_cast<usize>(y), z, v, vx + v * -1.0f, vy + v * -1.0f, shader, st);
                } else {
                    write_fragment(static_cast<usize>(x), static_cast<usize>(y), z, v, v, v, shader, st);
                }
            }
    }

    template <class V, class S>
    void raster_line(const SV<V>& a, const SV<V>& b, S& shader, const DrawState& st) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const long n = static_cast<long>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
        if (n <= 0) return;
        for (long i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const long x = static_cast<long>(std::floor(a.x + dx * t)), y = static_cast<long>(std::floor(a.y + dy * t));
            if (!in_bounds(x, y, st)) continue;
            const double p0 = (1 - t) * a.inv_w, p1 = t * b.inv_w, s = p0 + p1;
            const V v = a.v * static_cast<float>(p0 / s) + b.v * static_cast<float>(p1 / s);
            const float z = std::clamp(a.z + (b.z - a.z) * t - 1e-5f, 0.0f, 1.0f);
            write_fragment(static_cast<usize>(x), static_cast<usize>(y), z, v, v, v, shader, st);
        }
    }
    template <class V, class S>
    void raster_point(const SV<V>& p, S& shader, const DrawState& st) {
        const long x = static_cast<long>(std::floor(p.x)), y = static_cast<long>(std::floor(p.y));
        if (!in_bounds(x, y, st)) return;
        write_fragment(static_cast<usize>(x), static_cast<usize>(y), std::clamp(p.z - 1e-5f, 0.0f, 1.0f), p.v, p.v, p.v, shader, st);
    }

    void scissor_bounds(const DrawState& st, long& lx, long& ly, long& hx, long& hy) const {
        const long k = static_cast<long>(ss_);
        lx = std::max<long>(0, st.scissor_x * k);
        ly = std::max<long>(0, st.scissor_y * k);
        hx = std::min<long>(static_cast<long>(fw_) - 1, (st.scissor_x + st.scissor_w) * k - 1);
        hy = std::min<long>(static_cast<long>(fh_) - 1, (st.scissor_y + st.scissor_h) * k - 1);
    }
    bool in_bounds(long x, long y, const DrawState& st) const {
        long lx = 0, ly = 0, hx = static_cast<long>(fw_) - 1, hy = static_cast<long>(fh_) - 1;
        if (st.scissor) scissor_bounds(st, lx, ly, hx, hy);
        return x >= lx && y >= ly && x <= hx && y <= hy;
    }

    usize w_, h_, fw_ = 0, fh_ = 0;
    unsigned ss_;
    std::vector<Color> color_;
    std::vector<float> depth_;
    RenderStats stats_;
};

}  // namespace flyg::graphics
