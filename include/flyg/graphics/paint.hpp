// flyG - graphics module: paints (solid, linear/radial/conic gradients, image patterns) and blend/compositing modes
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <variant>
#include <vector>

#include <flyg/graphics/image.hpp>
#include <flyg/graphics/path.hpp>

namespace flyg::graphics {

enum class SpreadMode { pad, repeat, reflect };

struct GradientStop {
    float offset = 0;   ///< 0..1
    Color color;
};

/// Colour ramp; interpolation happens on straight (non-premultiplied) sRGB values like the HTML canvas.
class Gradient {
public:
    Gradient() = default;
    Gradient(std::initializer_list<GradientStop> s) : stops_(s) { normalise(); }
    explicit Gradient(std::vector<GradientStop> s) : stops_(std::move(s)) { normalise(); }
    Gradient& add_stop(float offset, Color c) {
        if (!(offset >= 0 && offset <= 1)) throw GraphicsException("gradient stop offset must be within [0,1]");
        stops_.push_back({offset, c});
        normalise();
        return *this;
    }
    [[nodiscard]] const std::vector<GradientStop>& stops() const noexcept { return stops_; }
    [[nodiscard]] ColorF at(float t) const noexcept {
        if (stops_.empty()) return {0, 0, 0, 0};
        if (t <= stops_.front().offset) return stops_.front().color.to_float();
        if (t >= stops_.back().offset) return stops_.back().color.to_float();
        usize i = 1;
        while (i < stops_.size() && stops_[i].offset < t) ++i;
        const auto& a = stops_[i - 1];
        const auto& b = stops_[i];
        const float span = b.offset - a.offset;
        const float u = span > 0 ? (t - a.offset) / span : 1.0f;
        return a.color.to_float() * (1 - u) + b.color.to_float() * u;
    }

private:
    void normalise() { std::stable_sort(stops_.begin(), stops_.end(), [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; }); }
    std::vector<GradientStop> stops_;
};

struct SolidPaint { Color color = Color::black; };
struct LinearGradientPaint { Point p0, p1; Gradient gradient; SpreadMode spread = SpreadMode::pad; };
/// Two-point conical gradient (circle (c0,r0) -> circle (c1,r1)); r0 = 0 and c0 = c1 gives a plain radial gradient.
struct RadialGradientPaint { Point c0, c1; float r0 = 0, r1 = 1; Gradient gradient; SpreadMode spread = SpreadMode::pad; };
struct ConicGradientPaint { Point center; float start_angle = 0; Gradient gradient; };
enum class SampleFilter { nearest, bilinear };
struct PatternPaint {
    std::shared_ptr<const Image> image;
    Transform transform;            ///< maps image pixel space to user space
    bool repeat_x = false, repeat_y = false;
    SampleFilter filter = SampleFilter::bilinear;
};

using Paint = std::variant<SolidPaint, LinearGradientPaint, RadialGradientPaint, ConicGradientPaint, PatternPaint>;

[[nodiscard]] inline Paint solid(Color c) { return SolidPaint{c}; }
[[nodiscard]] inline Paint linear_gradient(Point a, Point b, Gradient g, SpreadMode s = SpreadMode::pad) { return LinearGradientPaint{a, b, std::move(g), s}; }
[[nodiscard]] inline Paint radial_gradient(Point c, float r, Gradient g, SpreadMode s = SpreadMode::pad) { return RadialGradientPaint{c, c, 0, r, std::move(g), s}; }
[[nodiscard]] inline Paint radial_gradient(Point c0, float r0, Point c1, float r1, Gradient g, SpreadMode s = SpreadMode::pad) { return RadialGradientPaint{c0, c1, r0, r1, std::move(g), s}; }
[[nodiscard]] inline Paint conic_gradient(Point c, float start_angle, Gradient g) { return ConicGradientPaint{c, start_angle, std::move(g)}; }
[[nodiscard]] inline Paint pattern(Image img, Transform t = {}, bool repeat_x = true, bool repeat_y = true, SampleFilter f = SampleFilter::bilinear) {
    return PatternPaint{std::make_shared<const Image>(std::move(img)), t, repeat_x, repeat_y, f};
}

namespace detail_paint {
inline float spread(float t, SpreadMode m) noexcept {
    switch (m) {
        case SpreadMode::pad: return std::clamp(t, 0.0f, 1.0f);
        case SpreadMode::repeat: return t - std::floor(t);
        case SpreadMode::reflect: {
            float u = std::fmod(t, 2.0f);
            if (u < 0) u += 2;
            return u <= 1 ? u : 2 - u;
        }
    }
    return t;
}
}  // namespace detail_paint

/// Evaluates a paint at device pixels. Construct once per draw call with the device->user inverse transform.
class PaintSampler {
public:
    PaintSampler(const Paint& p, const Transform& user_to_device) : paint_(&p) {
        if (const auto* pat = std::get_if<PatternPaint>(&p)) {
            if (!pat->image || pat->image->empty()) throw GraphicsException("pattern paint has no image");
            const Transform full = user_to_device * pat->transform;  // image px -> device
            const auto inv = full.inverse();
            if (!inv) { degenerate_ = true; return; }
            inv_ = *inv;
        } else if (!std::holds_alternative<SolidPaint>(p)) {
            const auto inv = user_to_device.inverse();
            if (!inv) { degenerate_ = true; return; }
            inv_ = *inv;
        }
    }
    [[nodiscard]] bool is_solid() const noexcept { return std::holds_alternative<SolidPaint>(*paint_); }
    [[nodiscard]] Color solid_color() const { return std::get<SolidPaint>(*paint_).color; }

    /// Straight-alpha colour at the centre of device pixel (x,y).
    [[nodiscard]] ColorF sample(int x, int y) const {
        if (degenerate_) return {0, 0, 0, 0};
        if (const auto* s = std::get_if<SolidPaint>(paint_)) return s->color.to_float();
        const Point q = inv_({static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
        if (const auto* g = std::get_if<LinearGradientPaint>(paint_)) {
            const Point d = g->p1 - g->p0;
            const float l2 = dot(d, d);
            const float t = l2 > 0 ? dot(q - g->p0, d) / l2 : 0.0f;
            return g->gradient.at(detail_paint::spread(t, g->spread));
        }
        if (const auto* g = std::get_if<RadialGradientPaint>(paint_)) {
            const Point cd = g->c1 - g->c0, pd = q - g->c0;
            const float dr = g->r1 - g->r0;
            const float a = dot(cd, cd) - dr * dr, b = dot(pd, cd) + g->r0 * dr, c = dot(pd, pd) - g->r0 * g->r0;
            float t;
            if (std::fabs(a) < 1e-9f) {
                if (std::fabs(b) < 1e-12f) return {0, 0, 0, 0};
                t = c / (2 * b);
                if (g->r0 + t * dr < 0) return {0, 0, 0, 0};
            } else {
                const float disc = b * b - a * c;
                if (disc < 0) return {0, 0, 0, 0};
                const float sq = std::sqrt(disc);
                const float t1 = (b + sq) / a, t2 = (b - sq) / a;
                const float hi = std::max(t1, t2), lo = std::min(t1, t2);
                if (g->r0 + hi * dr >= 0) t = hi;
                else if (g->r0 + lo * dr >= 0) t = lo;
                else return {0, 0, 0, 0};
            }
            return g->gradient.at(detail_paint::spread(t, g->spread));
        }
        if (const auto* g = std::get_if<ConicGradientPaint>(paint_)) {
            float a = std::atan2(q.y - g->center.y, q.x - g->center.x) - g->start_angle;
            a = a / (2 * kPi);
            a -= std::floor(a);
            return g->gradient.at(a);
        }
        return sample_pattern(std::get<PatternPaint>(*paint_), q);
    }

private:
    static ColorF px(const PatternPaint& p, long ix, long iy) {
        const Image& im = *p.image;
        const long w = static_cast<long>(im.width()), h = static_cast<long>(im.height());
        if (p.repeat_x) { ix %= w; if (ix < 0) ix += w; } else if (ix < 0 || ix >= w) return {0, 0, 0, 0};
        if (p.repeat_y) { iy %= h; if (iy < 0) iy += h; } else if (iy < 0 || iy >= h) return {0, 0, 0, 0};
        return im(static_cast<usize>(ix), static_cast<usize>(iy)).to_float().premultiplied();
    }
    static ColorF sample_pattern(const PatternPaint& p, Point q) {
        ColorF r;
        if (p.filter == SampleFilter::nearest) {
            r = px(p, static_cast<long>(std::floor(q.x)), static_cast<long>(std::floor(q.y)));
        } else {
            const float fx = q.x - 0.5f, fy = q.y - 0.5f;
            const long x0 = static_cast<long>(std::floor(fx)), y0 = static_cast<long>(std::floor(fy));
            const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
            r = px(p, x0, y0) * ((1 - tx) * (1 - ty)) + px(p, x0 + 1, y0) * (tx * (1 - ty)) + px(p, x0, y0 + 1) * ((1 - tx) * ty) + px(p, x0 + 1, y0 + 1) * (tx * ty);
        }
        if (r.a <= 0) return {0, 0, 0, 0};
        return {r.r / r.a, r.g / r.a, r.b / r.a, r.a};
    }
    const Paint* paint_;
    Transform inv_;
    bool degenerate_ = false;
};

// ------------------------------------------------------------------------------------------------ blending

enum class BlendMode {
    normal, multiply, screen, overlay, darken, lighten, color_dodge, color_burn, hard_light, soft_light, difference, exclusion,
    hue, saturation, color, luminosity,
    // Porter-Duff operators
    clear, source, dest_over, source_in, source_out, source_atop, dest_in, dest_out, dest_atop, xor_, plus
};

namespace detail_blend {
inline float sep(BlendMode m, float cb, float cs) noexcept {
    switch (m) {
        case BlendMode::multiply: return cb * cs;
        case BlendMode::screen: return cb + cs - cb * cs;
        case BlendMode::overlay: return sep(BlendMode::hard_light, cs, cb);
        case BlendMode::darken: return std::min(cb, cs);
        case BlendMode::lighten: return std::max(cb, cs);
        case BlendMode::color_dodge: return cb == 0 ? 0.0f : (cs >= 1 ? 1.0f : std::min(1.0f, cb / (1 - cs)));
        case BlendMode::color_burn: return cb >= 1 ? 1.0f : (cs <= 0 ? 0.0f : 1 - std::min(1.0f, (1 - cb) / cs));
        case BlendMode::hard_light: return cs <= 0.5f ? cb * 2 * cs : sep(BlendMode::screen, cb, 2 * cs - 1);
        case BlendMode::soft_light: {
            if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
            const float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
            return cb + (2 * cs - 1) * (d - cb);
        }
        case BlendMode::difference: return std::fabs(cb - cs);
        case BlendMode::exclusion: return cb + cs - 2 * cb * cs;
        default: return cs;
    }
}
struct Rgb { float r, g, b; };
inline float lum(Rgb c) noexcept { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }
inline Rgb clip_color(Rgb c) noexcept {
    const float l = lum(c), n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0) { c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)}; }
    if (x > 1) { c = {l + (c.r - l) * (1 - l) / (x - l), l + (c.g - l) * (1 - l) / (x - l), l + (c.b - l) * (1 - l) / (x - l)}; }
    return c;
}
inline Rgb set_lum(Rgb c, float l) noexcept { const float d = l - lum(c); return clip_color({c.r + d, c.g + d, c.b + d}); }
inline float sat(Rgb c) noexcept { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
inline Rgb set_sat(Rgb c, float s) noexcept {
    float* v[3] = {&c.r, &c.g, &c.b};
    std::sort(v, v + 3, [](float* a, float* b) { return *a < *b; });
    if (*v[2] > *v[0]) { *v[1] = (*v[1] - *v[0]) * s / (*v[2] - *v[0]); *v[2] = s; } else { *v[1] = *v[2] = 0; }
    *v[0] = 0;
    return c;
}
inline Rgb nonsep(BlendMode m, Rgb cb, Rgb cs) noexcept {
    switch (m) {
        case BlendMode::hue: return set_lum(set_sat(cs, sat(cb)), lum(cb));
        case BlendMode::saturation: return set_lum(set_sat(cb, sat(cs)), lum(cb));
        case BlendMode::color: return set_lum(cs, lum(cb));
        default: return set_lum(cb, lum(cs));  // luminosity
    }
}
}  // namespace detail_blend

/// Composite straight-alpha source `src` (alpha already includes coverage and global alpha) onto `dst`.
/// `coverage` is only used by the Porter-Duff operators that must interpolate with the destination
/// (all others fold coverage into the source alpha).
[[nodiscard]] inline Color composite(Color dst, ColorF src, BlendMode mode, float coverage = 1.0f) noexcept {
    using namespace detail_blend;
    const ColorF d = dst.to_float();
    const float as = std::clamp(src.a, 0.0f, 1.0f), ad = d.a;
    float ao, r, g, b;  // premultiplied result
    auto finish = [&](float pr, float pg, float pb, float pa) -> Color {
        if (pa <= 0.0f) return Color{0, 0, 0, 0};
        auto q = [&](float v) { return static_cast<u8>(std::clamp(v / pa * 255.0f + 0.5f, 0.0f, 255.0f)); };
        return Color{q(pr), q(pg), q(pb), static_cast<u8>(std::clamp(pa * 255.0f + 0.5f, 0.0f, 255.0f))};
    };
    if (mode <= BlendMode::luminosity) {
        if (mode == BlendMode::normal) {
            ao = as + ad * (1 - as);
            r = src.r * as + d.r * ad * (1 - as);
            g = src.g * as + d.g * ad * (1 - as);
            b = src.b * as + d.b * ad * (1 - as);
            return finish(r, g, b, ao);
        }
        Rgb bl;
        if (mode >= BlendMode::hue) bl = nonsep(mode, {d.r, d.g, d.b}, {src.r, src.g, src.b});
        else bl = {sep(mode, d.r, src.r), sep(mode, d.g, src.g), sep(mode, d.b, src.b)};
        ao = as + ad * (1 - as);
        r = as * (1 - ad) * src.r + as * ad * bl.r + (1 - as) * ad * d.r;
        g = as * (1 - ad) * src.g + as * ad * bl.g + (1 - as) * ad * d.g;
        b = as * (1 - ad) * src.b + as * ad * bl.b + (1 - as) * ad * d.b;
        return finish(r, g, b, ao);
    }
    float fa = 1, fb = 0;
    switch (mode) {
        case BlendMode::clear: fa = 0, fb = 0; break;
        case BlendMode::source: fa = 1, fb = 0; break;
        case BlendMode::dest_over: fa = 1 - ad, fb = 1; break;
        case BlendMode::source_in: fa = ad, fb = 0; break;
        case BlendMode::source_out: fa = 1 - ad, fb = 0; break;
        case BlendMode::source_atop: fa = ad, fb = 1 - as; break;
        case BlendMode::dest_in: fa = 0, fb = as; break;
        case BlendMode::dest_out: fa = 0, fb = 1 - as; break;
        case BlendMode::dest_atop: fa = 1 - ad, fb = as; break;
        case BlendMode::xor_: fa = 1 - ad, fb = 1 - as; break;
        default: fa = 1, fb = 1; break;  // plus
    }
    // Porter-Duff operators work on the full-coverage source and are interpolated with the old destination afterwards.
    const float cov = std::clamp(coverage, 0.0f, 1.0f);
    const float sa = as / std::max(cov, 1e-6f);
    const float s_a = std::clamp(sa, 0.0f, 1.0f);
    float fa2 = fa, fb2 = fb;
    if (mode == BlendMode::source_atop) fb2 = 1 - s_a;
    if (mode == BlendMode::dest_in || mode == BlendMode::dest_atop) fb2 = s_a;
    if (mode == BlendMode::dest_out) fb2 = 1 - s_a;
    if (mode == BlendMode::xor_) fb2 = 1 - s_a;
    ao = s_a * fa2 + ad * fb2;
    r = src.r * s_a * fa2 + d.r * ad * fb2;
    g = src.g * s_a * fa2 + d.g * ad * fb2;
    b = src.b * s_a * fa2 + d.b * ad * fb2;
    if (mode == BlendMode::plus) ao = std::min(ao, 1.0f), r = std::min(r, ao), g = std::min(g, ao), b = std::min(b, ao);
    // interpolate between the old destination and the full-coverage result
    if (cov < 1.0f) {
        ao = ao * cov + ad * (1 - cov);
        r = r * cov + d.r * ad * (1 - cov);
        g = g * cov + d.g * ad * (1 - cov);
        b = b * cov + d.b * ad * (1 - cov);
    }
    return finish(r, g, b, ao);
}

}  // namespace flyg::graphics
