// flyG - graphics module: textures with mipmaps, wrap modes and nearest/bilinear/trilinear sampling (linear-light float storage)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cmath>
#include <vector>

#include <flyg/graphics/image.hpp>
#include <flyg/math/vector2.hpp>

namespace flyg::graphics {

enum class Wrap { repeat, clamp, mirror };
enum class TexFilter { nearest, bilinear, trilinear };

class Texture {
public:
    Texture() = default;
    /// `srgb`: treat the image as sRGB-encoded colour (converted to linear light); false keeps raw values (data textures).
    explicit Texture(const Image& img, bool mipmaps = true, bool srgb = true) {
        if (img.empty()) throw GraphicsException("texture needs a non-empty image");
        Level base;
        base.w = img.width(), base.h = img.height();
        base.px.resize(img.pixel_count());
        for (usize i = 0; i < img.pixel_count(); ++i) {
            const Color c = img.data()[i];
            ColorF f = srgb ? color::to_linear(c) : c.to_float();
            f.a = static_cast<float>(c.a) / 255.0f;
            base.px[i] = f.premultiplied();
        }
        levels_.push_back(std::move(base));
        while (mipmaps && (levels_.back().w > 1 || levels_.back().h > 1)) levels_.push_back(shrink(levels_.back()));
    }

    Wrap wrap_s = Wrap::repeat, wrap_t = Wrap::repeat;
    TexFilter filter = TexFilter::trilinear;

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] usize width() const noexcept { return levels_.empty() ? 0 : levels_[0].w; }
    [[nodiscard]] usize height() const noexcept { return levels_.empty() ? 0 : levels_[0].h; }
    [[nodiscard]] usize level_count() const noexcept { return levels_.size(); }

    /// Straight-alpha linear colour at `uv` (v = 0 is the first image row). `lod` selects the mip level (0 = full resolution).
    [[nodiscard]] ColorF sample(math::Vector2f uv, float lod = 0) const {
        if (levels_.empty()) return {1, 0, 1, 1};
        ColorF r;
        if (filter == TexFilter::nearest) r = fetch_nearest(clamp_level(lod), uv);
        else if (filter == TexFilter::bilinear || levels_.size() == 1) r = bilinear(clamp_level(std::round(lod)), uv);
        else {
            const float l = std::clamp(lod, 0.0f, static_cast<float>(levels_.size() - 1));
            const usize l0 = static_cast<usize>(std::floor(l)), l1 = std::min(l0 + 1, levels_.size() - 1);
            const float t = l - static_cast<float>(l0);
            r = bilinear(l0, uv) * (1 - t) + bilinear(l1, uv) * t;
        }
        if (r.a <= 0) return {0, 0, 0, 0};
        return {r.r / r.a, r.g / r.a, r.b / r.a, r.a};
    }
    /// Sample with screen-space uv derivatives (selects the mip level like a GPU would).
    [[nodiscard]] ColorF sample(math::Vector2f uv, math::Vector2f duv_dx, math::Vector2f duv_dy) const {
        if (levels_.empty()) return {1, 0, 1, 1};
        const float w = static_cast<float>(levels_[0].w), h = static_cast<float>(levels_[0].h);
        const float lx = std::hypot(duv_dx.x * w, duv_dx.y * h), ly = std::hypot(duv_dy.x * w, duv_dy.y * h);
        const float rho = std::max(lx, ly);
        return sample(uv, rho > 1e-8f ? std::log2(rho) : 0.0f);
    }

private:
    struct Level { usize w = 0, h = 0; std::vector<ColorF> px; };

    static Level shrink(const Level& s) {
        Level d;
        d.w = std::max<usize>(1, s.w / 2), d.h = std::max<usize>(1, s.h / 2);
        d.px.resize(d.w * d.h);
        for (usize y = 0; y < d.h; ++y)
            for (usize x = 0; x < d.w; ++x) {
                ColorF acc{0, 0, 0, 0};
                const usize x0 = std::min(2 * x, s.w - 1), x1 = std::min(2 * x + 1, s.w - 1), y0 = std::min(2 * y, s.h - 1), y1 = std::min(2 * y + 1, s.h - 1);
                acc = s.px[y0 * s.w + x0] + s.px[y0 * s.w + x1] + s.px[y1 * s.w + x0] + s.px[y1 * s.w + x1];
                d.px[y * d.w + x] = acc * 0.25f;
            }
        return d;
    }
    [[nodiscard]] usize clamp_level(float lod) const noexcept {
        return static_cast<usize>(std::clamp(std::round(lod), 0.0f, static_cast<float>(levels_.size() - 1)));
    }
    static long wrap_idx(long i, long n, Wrap w) noexcept {
        switch (w) {
            case Wrap::clamp: return std::clamp<long>(i, 0, n - 1);
            case Wrap::repeat: { long m = i % n; return m < 0 ? m + n : m; }
            case Wrap::mirror: { long m = i % (2 * n); if (m < 0) m += 2 * n; return m < n ? m : 2 * n - 1 - m; }
        }
        return 0;
    }
    [[nodiscard]] const ColorF& texel(usize lvl, long x, long y) const noexcept {
        const Level& L = levels_[lvl];
        return L.px[static_cast<usize>(wrap_idx(y, static_cast<long>(L.h), wrap_t)) * L.w + static_cast<usize>(wrap_idx(x, static_cast<long>(L.w), wrap_s))];
    }
    [[nodiscard]] ColorF fetch_nearest(usize lvl, math::Vector2f uv) const {
        const Level& L = levels_[lvl];
        return texel(lvl, static_cast<long>(std::floor(uv.x * static_cast<float>(L.w))), static_cast<long>(std::floor(uv.y * static_cast<float>(L.h))));
    }
    [[nodiscard]] ColorF bilinear(usize lvl, math::Vector2f uv) const {
        const Level& L = levels_[lvl];
        const float fx = uv.x * static_cast<float>(L.w) - 0.5f, fy = uv.y * static_cast<float>(L.h) - 0.5f;
        const long x0 = static_cast<long>(std::floor(fx)), y0 = static_cast<long>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
        return texel(lvl, x0, y0) * ((1 - tx) * (1 - ty)) + texel(lvl, x0 + 1, y0) * (tx * (1 - ty)) + texel(lvl, x0, y0 + 1) * ((1 - tx) * ty) + texel(lvl, x0 + 1, y0 + 1) * (tx * ty);
    }
    std::vector<Level> levels_;
};

}  // namespace flyg::graphics
