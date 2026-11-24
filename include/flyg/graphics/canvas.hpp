// flyG - graphics module: 2D software canvas (HTML-canvas-like state machine on top of an Image)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include <flyg/graphics/font.hpp>
#include <flyg/graphics/image.hpp>
#include <flyg/graphics/paint.hpp>
#include <flyg/graphics/path.hpp>
#include <flyg/graphics/raster.hpp>
#include <flyg/graphics/stroke.hpp>

namespace flyg::graphics {

/// Anti-aliased 2D renderer drawing into an RGBA8 image (straight alpha).
class Canvas {
public:
    Canvas(usize width, usize height, Color background = Color{0, 0, 0, 0}) : img_(width, height, background) { init(); }
    explicit Canvas(Image image) : img_(std::move(image)) { init(); }

    [[nodiscard]] usize width() const noexcept { return img_.width(); }
    [[nodiscard]] usize height() const noexcept { return img_.height(); }
    [[nodiscard]] const Image& image() const noexcept { return img_; }
    [[nodiscard]] Image& image() noexcept { return img_; }
    /// Move the finished image out (the canvas becomes empty).
    [[nodiscard]] Image take() { return std::move(img_); }

    // ------------------------------------------------------------------ state
    void save() { stack_.push_back(st_); }
    void restore() {
        if (stack_.empty()) throw GraphicsException("Canvas::restore without matching save");
        st_ = std::move(stack_.back());
        stack_.pop_back();
    }
    [[nodiscard]] usize save_depth() const noexcept { return stack_.size(); }

    void translate(float x, float y) { st_.ctm = st_.ctm * Transform::translate(x, y); }
    void scale(float x, float y) { st_.ctm = st_.ctm * Transform::scale(x, y); }
    void rotate(float radians) { st_.ctm = st_.ctm * Transform::rotate(radians); }
    void transform(const Transform& t) { st_.ctm = st_.ctm * t; }
    void set_transform(const Transform& t) { st_.ctm = t; }
    void reset_transform() { st_.ctm = Transform{}; }
    [[nodiscard]] const Transform& current_transform() const noexcept { return st_.ctm; }

    void set_fill(Paint p) { st_.fill = std::move(p); }
    void set_fill(Color c) { st_.fill = SolidPaint{c}; }
    void set_stroke(Paint p) { st_.stroke = std::move(p); }
    void set_stroke(Color c) { st_.stroke = SolidPaint{c}; }
    void set_global_alpha(float a) { st_.alpha = std::clamp(a, 0.0f, 1.0f); }
    void set_blend_mode(BlendMode m) { st_.blend = m; }
    void set_line_width(float w) { st_.line.width = w; }
    void set_line_cap(LineCap c) { st_.line.cap = c; }
    void set_line_join(LineJoin j) { st_.line.join = j; }
    void set_miter_limit(float m) { st_.line.miter_limit = m; }
    void set_line_dash(std::vector<float> d, float offset = 0) { st_.line.dash = std::move(d); st_.line.dash_offset = offset; }
    void set_fill_rule(FillRule r) { st_.rule = r; }
    [[nodiscard]] const StrokeStyle& stroke_style() const noexcept { return st_.line; }

    // ------------------------------------------------------------------ clipping
    /// Intersect the clip region with the path (anti-aliased). Cleared by restore() or reset_clip().
    void clip(const Path& path, FillRule rule = FillRule::non_zero) {
        std::vector<u8> mask(img_.pixel_count(), 0);
        const PolygonSet polys = polygons(path);
        raster::fill(polys, rule, static_cast<int>(width()), static_cast<int>(height()), [&](int y, int xb, int xe, const float* cov) {
            for (int x = xb; x < xe; ++x) mask[static_cast<usize>(y) * width() + static_cast<usize>(x)] = static_cast<u8>(cov[x - xb] * 255.0f + 0.5f);
        });
        if (st_.clip) {
            for (usize i = 0; i < mask.size(); ++i) mask[i] = static_cast<u8>((u32(mask[i]) * (*st_.clip)[i] + 127) / 255);
        }
        st_.clip = std::make_shared<const std::vector<u8>>(std::move(mask));
    }
    void clip_rect(float x, float y, float w, float h) { clip(Path().rect(x, y, w, h)); }
    void reset_clip() { st_.clip.reset(); }

    // ------------------------------------------------------------------ drawing
    void fill(const Path& path) { fill(path, st_.rule); }
    void fill(const Path& path, FillRule rule) { paint_polygons(polygons(path), rule, st_.fill, st_.blend); }
    void stroke(const Path& path) {
        const float scale = std::max(st_.ctm.max_scale(), 1e-6f);
        const PolygonSet user = flyg::graphics::stroke(path, st_.line, 0.1f / scale);
        PolygonSet dev = user;
        for (auto& poly : dev) for (auto& p : poly) p = st_.ctm(p);
        paint_polygons(dev, FillRule::non_zero, st_.stroke, st_.blend);
    }
    void fill_rect(float x, float y, float w, float h) { fill(Path().rect(x, y, w, h)); }
    void stroke_rect(float x, float y, float w, float h) { stroke(Path().rect(x, y, w, h)); }
    void fill_circle(float cx, float cy, float r) { fill(Path().circle(cx, cy, r)); }
    void stroke_circle(float cx, float cy, float r) { stroke(Path().circle(cx, cy, r)); }
    void draw_line(float x0, float y0, float x1, float y1) { stroke(Path().move_to(x0, y0).line_to(x1, y1)); }
    /// Erase a rectangle to transparent (ignores paint, blend mode and global alpha; honours transform and clip).
    void clear_rect(float x, float y, float w, float h) {
        const Paint transparent = SolidPaint{Color{0, 0, 0, 0}};
        paint_polygons(polygons(Path().rect(x, y, w, h)), FillRule::non_zero, transparent, BlendMode::clear);
    }
    /// Overwrite the whole image (ignores transform and clip).
    void clear(Color c = Color{0, 0, 0, 0}) { img_.fill(c); }

    /// Draw `src` with its top-left corner at (dx,dy).
    void draw_image(const Image& src, float dx, float dy, SampleFilter f = SampleFilter::bilinear) {
        draw_image(src, Rect{0, 0, static_cast<float>(src.width()), static_cast<float>(src.height())}, Rect{dx, dy, static_cast<float>(src.width()), static_cast<float>(src.height())}, f);
    }
    void draw_image(const Image& src, Rect dst, SampleFilter f = SampleFilter::bilinear) {
        draw_image(src, Rect{0, 0, static_cast<float>(src.width()), static_cast<float>(src.height())}, dst, f);
    }
    /// Draw the part `s` of `src` into rectangle `d` (both in their own spaces; `d` is transformed by the CTM).
    void draw_image(const Image& src, Rect s, Rect d, SampleFilter f = SampleFilter::bilinear) {
        if (src.empty() || s.empty() || d.empty()) return;
        const float sw = static_cast<float>(src.width()), sh = static_cast<float>(src.height());
        if (s.x < 0 || s.y < 0 || s.right() > sw + 1e-3f || s.bottom() > sh + 1e-3f) throw GraphicsException("draw_image: source rectangle outside the image");
        const float kx = d.w / s.w, ky = d.h / s.h;
        std::shared_ptr<const Image> use;
        Transform t;
        const float sx = std::hypot(st_.ctm.a, st_.ctm.b) * kx, sy = std::hypot(st_.ctm.c, st_.ctm.d) * ky;
        const bool shrink = (sx < 0.5f || sy < 0.5f) && sx > 0 && sy > 0;
        if (shrink) {  // pre-filter heavily minified images with an area average to avoid aliasing
            const Image sub = src.crop(static_cast<long>(std::floor(s.x)), static_cast<long>(std::floor(s.y)),
                                       std::min(src.width() - static_cast<usize>(std::floor(s.x)), static_cast<usize>(std::ceil(s.w))),
                                       std::min(src.height() - static_cast<usize>(std::floor(s.y)), static_cast<usize>(std::ceil(s.h))));
            const usize nw = std::max<usize>(1, static_cast<usize>(std::lround(std::min(sx, 1.0f) * static_cast<float>(sub.width()))));
            const usize nh = std::max<usize>(1, static_cast<usize>(std::lround(std::min(sy, 1.0f) * static_cast<float>(sub.height()))));
            use = std::make_shared<const Image>(resize(sub, nw, nh, Filter::area));
            t = Transform::translate(d.x, d.y) * Transform::scale(d.w / static_cast<float>(nw), d.h / static_cast<float>(nh));
        } else {
            use = std::shared_ptr<const Image>(std::shared_ptr<const Image>{}, &src);  // non-owning alias
            t = Transform::translate(d.x, d.y) * Transform::scale(kx, ky) * Transform::translate(-s.x, -s.y);
        }
        PatternPaint pp{use, t, false, false, f};
        paint_polygons(polygons(Path().rect(d.x, d.y, d.w, d.h)), FillRule::non_zero, Paint{pp}, st_.blend);
    }

    // ------------------------------------------------------------------ text
    /// Fill text with its baseline origin at (x,y) using a TrueType font. '\n' starts a new line.
    void fill_text(const Font& font, std::string_view text, float x, float y, float size) { fill(font.text_path(text, x, y, size), FillRule::non_zero); }
    void stroke_text(const Font& font, std::string_view text, float x, float y, float size) { stroke(font.text_path(text, x, y, size)); }
    /// Built-in 5x7 bitmap font; (x,y) is the top-left of the first glyph, `scale` the pixel size of one font pixel.
    void fill_text(std::string_view text, float x, float y, float scale = 2.0f) { fill(BitmapFont::text_path(text, x, y, scale), FillRule::non_zero); }

    /// Pixel of the underlying image (device space).
    [[nodiscard]] Color pixel(usize x, usize y) const { return img_.at(x, y); }

private:
    struct State {
        Transform ctm;
        Paint fill = SolidPaint{Color::black};
        Paint stroke = SolidPaint{Color::black};
        float alpha = 1.0f;
        BlendMode blend = BlendMode::normal;
        StrokeStyle line;
        FillRule rule = FillRule::non_zero;
        std::shared_ptr<const std::vector<u8>> clip;
    };

    void init() {
        if (img_.empty()) throw GraphicsException("Canvas needs a non-empty image");
    }

    [[nodiscard]] PolygonSet polygons(const Path& path) const {
        const float tol = 0.1f;
        const Path dev = path.transformed(st_.ctm);
        PolygonSet out;
        for (const Polyline& pl : dev.flatten(tol)) {
            if (pl.pts.size() >= 3) out.push_back(pl.pts);
        }
        return out;
    }

    void paint_polygons(const PolygonSet& polys, FillRule rule, const Paint& paint, BlendMode mode) {
        if (polys.empty()) return;
        const PaintSampler sampler(paint, st_.ctm);
        const bool solid_fast = sampler.is_solid();
        const Color sc = solid_fast ? sampler.solid_color() : Color{};
        const float ga = st_.alpha;
        const u8* clip = st_.clip ? st_.clip->data() : nullptr;
        const usize W = width();
        raster::fill(polys, rule, static_cast<int>(W), static_cast<int>(height()), [&](int y, int xb, int xe, const float* cov) {
            Color* row = img_.data() + static_cast<usize>(y) * W;
            for (int x = xb; x < xe; ++x) {
                float c = cov[x - xb];
                if (c <= 0.0f) continue;
                if (clip) {
                    c *= static_cast<float>(clip[static_cast<usize>(y) * W + static_cast<usize>(x)]) * (1.0f / 255.0f);
                    if (c <= 0.0f) continue;
                }
                Color& dst = row[x];
                if (solid_fast && mode == BlendMode::normal) {
                    const float a = c * ga * static_cast<float>(sc.a) * (1.0f / 255.0f);
                    if (a >= 0.9999f) { dst = Color{sc.r, sc.g, sc.b, 255}; continue; }
                    if (a <= 0.0f) continue;
                }
                ColorF s = solid_fast ? sc.to_float() : sampler.sample(x, y);
                const float a = s.a * ga * c;
                if (a <= 0.0f && mode <= BlendMode::luminosity) continue;
                s.a = a;
                dst = composite(dst, s, mode, c);
            }
        });
    }

    Image img_;
    State st_;
    std::vector<State> stack_;
};

}  // namespace flyg::graphics
