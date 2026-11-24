// flyG - graphics module: raster images (RGBA8 / RGBA16 / float) and basic image operations
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include <flyg/graphics/color.hpp>

namespace flyg::graphics {

/// A row-major image of pixels of type P, origin at the top-left.
template <class P>
class BasicImage {
public:
    using pixel_type = P;

    BasicImage() = default;
    BasicImage(usize w, usize h, P fill = P{}) : w_(w), h_(h) {
        if (w != 0 && h > (usize(1) << 31) / w) throw GraphicsException("image dimensions too large");
        px_.assign(w * h, fill);
    }

    [[nodiscard]] usize width() const noexcept { return w_; }
    [[nodiscard]] usize height() const noexcept { return h_; }
    [[nodiscard]] bool empty() const noexcept { return px_.empty(); }
    [[nodiscard]] usize pixel_count() const noexcept { return px_.size(); }
    [[nodiscard]] P* data() noexcept { return px_.data(); }
    [[nodiscard]] const P* data() const noexcept { return px_.data(); }
    [[nodiscard]] std::span<P> pixels() noexcept { return px_; }
    [[nodiscard]] std::span<const P> pixels() const noexcept { return px_; }
    [[nodiscard]] std::span<P> row(usize y) { check_y(y); return std::span<P>(px_).subspan(y * w_, w_); }
    [[nodiscard]] std::span<const P> row(usize y) const { check_y(y); return std::span<const P>(px_).subspan(y * w_, w_); }

    /// Unchecked access.
    [[nodiscard]] P& operator()(usize x, usize y) noexcept { return px_[y * w_ + x]; }
    [[nodiscard]] const P& operator()(usize x, usize y) const noexcept { return px_[y * w_ + x]; }
    /// Bounds-checked access.
    [[nodiscard]] P& at(usize x, usize y) {
        if (x >= w_ || y >= h_) throw GraphicsException("pixel coordinates out of range");
        return px_[y * w_ + x];
    }
    [[nodiscard]] const P& at(usize x, usize y) const {
        if (x >= w_ || y >= h_) throw GraphicsException("pixel coordinates out of range");
        return px_[y * w_ + x];
    }
    /// Access with clamping to the nearest edge pixel (for filtering).
    [[nodiscard]] const P& clamped(long x, long y) const noexcept {
        x = std::clamp<long>(x, 0, static_cast<long>(w_) - 1);
        y = std::clamp<long>(y, 0, static_cast<long>(h_) - 1);
        return px_[static_cast<usize>(y) * w_ + static_cast<usize>(x)];
    }

    void fill(P v) { std::fill(px_.begin(), px_.end(), v); }
    void resize_canvas(usize w, usize h, P fill_value = P{}) {
        BasicImage n(w, h, fill_value);
        n.blit(*this, 0, 0);
        *this = std::move(n);
    }

    /// Copies a rectangle (clipped to the image).
    [[nodiscard]] BasicImage crop(long x, long y, usize w, usize h) const {
        BasicImage out(w, h);
        for (usize j = 0; j < h; ++j)
            for (usize i = 0; i < w; ++i) {
                const long sx = x + static_cast<long>(i), sy = y + static_cast<long>(j);
                if (sx >= 0 && sy >= 0 && static_cast<usize>(sx) < w_ && static_cast<usize>(sy) < h_) out(i, j) = (*this)(static_cast<usize>(sx), static_cast<usize>(sy));
            }
        return out;
    }
    /// Copies `src` to (dx, dy) without blending, clipped.
    void blit(const BasicImage& src, long dx, long dy) {
        for (usize j = 0; j < src.h_; ++j) {
            const long ty = dy + static_cast<long>(j);
            if (ty < 0 || static_cast<usize>(ty) >= h_) continue;
            for (usize i = 0; i < src.w_; ++i) {
                const long tx = dx + static_cast<long>(i);
                if (tx < 0 || static_cast<usize>(tx) >= w_) continue;
                (*this)(static_cast<usize>(tx), static_cast<usize>(ty)) = src(i, j);
            }
        }
    }
    [[nodiscard]] BasicImage flipped_horizontal() const {
        BasicImage o(w_, h_);
        for (usize y = 0; y < h_; ++y)
            for (usize x = 0; x < w_; ++x) o(w_ - 1 - x, y) = (*this)(x, y);
        return o;
    }
    [[nodiscard]] BasicImage flipped_vertical() const {
        BasicImage o(w_, h_);
        for (usize y = 0; y < h_; ++y) std::copy(px_.begin() + static_cast<std::ptrdiff_t>(y * w_), px_.begin() + static_cast<std::ptrdiff_t>((y + 1) * w_), o.px_.begin() + static_cast<std::ptrdiff_t>((h_ - 1 - y) * w_));
        return o;
    }
    /// Rotates clockwise by 90 degrees `quarter_turns` times.
    [[nodiscard]] BasicImage rotated(int quarter_turns) const {
        const int q = ((quarter_turns % 4) + 4) % 4;
        if (q == 0) return *this;
        if (q == 2) return flipped_horizontal().flipped_vertical();
        BasicImage o(h_, w_);
        for (usize y = 0; y < h_; ++y)
            for (usize x = 0; x < w_; ++x) {
                if (q == 1) o(h_ - 1 - y, x) = (*this)(x, y);
                else o(y, w_ - 1 - x) = (*this)(x, y);
            }
        return o;
    }
    friend bool operator==(const BasicImage& a, const BasicImage& b) { return a.w_ == b.w_ && a.h_ == b.h_ && a.px_ == b.px_; }

private:
    void check_y(usize y) const {
        if (y >= h_) throw GraphicsException("row out of range");
    }
    usize w_ = 0, h_ = 0;
    std::vector<P> px_;
};

using Image = BasicImage<Color>;      ///< 8-bit RGBA, straight alpha
using Image16 = BasicImage<Color16>;  ///< 16-bit RGBA
using ImageF = BasicImage<ColorF>;    ///< float RGBA (HDR)

enum class Filter { nearest, bilinear, bicubic, area };

/// Resamples to the given size. `area` (box average) is the right choice for strong down-scaling.
[[nodiscard]] inline Image resize(const Image& src, usize nw, usize nh, Filter f = Filter::bilinear) {
    if (src.empty() || nw == 0 || nh == 0) return Image(nw, nh);
    Image out(nw, nh);
    const float sx = static_cast<float>(src.width()) / static_cast<float>(nw), sy = static_cast<float>(src.height()) / static_cast<float>(nh);
    // all filtering is done on premultiplied floats so that transparent pixels do not bleed their colour
    auto get = [&](long x, long y) { return src.clamped(x, y).to_float().premultiplied(); };
    auto store = [](ColorF c) {
        if (c.a <= 0.0f) return Color{0, 0, 0, 0};
        const float inv = 1.0f / c.a;
        return ColorF{c.r * inv, c.g * inv, c.b * inv, c.a}.to_color();
    };
    auto cubic = [](float t) {  // Catmull-Rom
        t = std::fabs(t);
        if (t < 1) return 1.5f * t * t * t - 2.5f * t * t + 1;
        if (t < 2) return -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2;
        return 0.0f;
    };
    for (usize y = 0; y < nh; ++y)
        for (usize x = 0; x < nw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f, fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
            ColorF acc{0, 0, 0, 0};
            switch (f) {
                case Filter::nearest: {
                    out(x, y) = src.clamped(static_cast<long>(std::floor((static_cast<float>(x) + 0.5f) * sx)), static_cast<long>(std::floor((static_cast<float>(y) + 0.5f) * sy)));
                    continue;
                }
                case Filter::bilinear: {
                    const long x0 = static_cast<long>(std::floor(fx)), y0 = static_cast<long>(std::floor(fy));
                    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
                    acc = get(x0, y0) * ((1 - tx) * (1 - ty)) + get(x0 + 1, y0) * (tx * (1 - ty)) + get(x0, y0 + 1) * ((1 - tx) * ty) + get(x0 + 1, y0 + 1) * (tx * ty);
                    break;
                }
                case Filter::bicubic: {
                    const long x0 = static_cast<long>(std::floor(fx)), y0 = static_cast<long>(std::floor(fy));
                    float wsum = 0;
                    for (long j = -1; j <= 2; ++j)
                        for (long i = -1; i <= 2; ++i) {
                            const float w = cubic(fx - static_cast<float>(x0 + i)) * cubic(fy - static_cast<float>(y0 + j));
                            acc = acc + get(x0 + i, y0 + j) * w;
                            wsum += w;
                        }
                    acc = acc * (1.0f / wsum);
                    acc = {std::clamp(acc.r, 0.0f, 1.0f), std::clamp(acc.g, 0.0f, 1.0f), std::clamp(acc.b, 0.0f, 1.0f), std::clamp(acc.a, 0.0f, 1.0f)};
                    break;
                }
                case Filter::area: {
                    const float x0 = static_cast<float>(x) * sx, x1 = x0 + sx, y0 = static_cast<float>(y) * sy, y1 = y0 + sy;
                    float wsum = 0;
                    for (long j = static_cast<long>(std::floor(y0)); j < static_cast<long>(std::ceil(y1)); ++j)
                        for (long i = static_cast<long>(std::floor(x0)); i < static_cast<long>(std::ceil(x1)); ++i) {
                            const float wx = std::min(x1, static_cast<float>(i + 1)) - std::max(x0, static_cast<float>(i));
                            const float wy = std::min(y1, static_cast<float>(j + 1)) - std::max(y0, static_cast<float>(j));
                            const float w = wx * wy;
                            acc = acc + get(i, j) * w;
                            wsum += w;
                        }
                    acc = acc * (1.0f / wsum);
                    break;
                }
            }
            out(x, y) = store(acc);
        }
    return out;
}

/// Converts straight alpha to premultiplied in place (and back).
inline void premultiply(Image& img) { for (auto& p : img.pixels()) p = p.premultiplied(); }
inline void unpremultiply(Image& img) { for (auto& p : img.pixels()) p = p.unpremultiplied(); }

/// Source-over composite of `src` onto `dst` at (dx, dy).
inline void composite(Image& dst, const Image& src, long dx, long dy) {
    for (usize j = 0; j < src.height(); ++j) {
        const long ty = dy + static_cast<long>(j);
        if (ty < 0 || static_cast<usize>(ty) >= dst.height()) continue;
        for (usize i = 0; i < src.width(); ++i) {
            const long tx = dx + static_cast<long>(i);
            if (tx < 0 || static_cast<usize>(tx) >= dst.width()) continue;
            auto& d = dst(static_cast<usize>(tx), static_cast<usize>(ty));
            d = Color::over(src(i, j), d);
        }
    }
}

[[nodiscard]] inline Image to_image(const Image16& s) {
    Image o(s.width(), s.height());
    for (usize i = 0; i < s.pixel_count(); ++i) {
        const auto& p = s.data()[i];
        auto d = [](u16 v) { return static_cast<u8>((v * 255u + 32767u) / 65535u); };
        o.data()[i] = {d(p.r), d(p.g), d(p.b), d(p.a)};
    }
    return o;
}
[[nodiscard]] inline Image16 to_image16(const Image& s) {
    Image16 o(s.width(), s.height());
    for (usize i = 0; i < s.pixel_count(); ++i) {
        const auto& p = s.data()[i];
        o.data()[i] = {static_cast<u16>(p.r * 257), static_cast<u16>(p.g * 257), static_cast<u16>(p.b * 257), static_cast<u16>(p.a * 257)};
    }
    return o;
}
[[nodiscard]] inline Image to_image(const ImageF& s) {
    Image o(s.width(), s.height());
    for (usize i = 0; i < s.pixel_count(); ++i) {
        const auto& p = s.data()[i];  // simple clamp (no tone mapping) with sRGB encode
        o.data()[i] = ColorF{color::linear_to_srgb(std::clamp(p.r, 0.0f, 1.0f)), color::linear_to_srgb(std::clamp(p.g, 0.0f, 1.0f)), color::linear_to_srgb(std::clamp(p.b, 0.0f, 1.0f)), p.a}.to_color();
    }
    return o;
}

}  // namespace flyg::graphics
