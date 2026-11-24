// flyG - graphics module: colors (8-bit, 16-bit and float), sRGB transfer functions, HSV/HSL
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::graphics {

using core::u16;
using core::u32;
using core::u8;
using core::usize;

struct ColorF;

/// 8-bit RGBA, straight (non-premultiplied) alpha unless stated otherwise.
struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;

    constexpr Color() noexcept = default;
    constexpr Color(u8 r_, u8 g_, u8 b_, u8 a_ = 255) noexcept : r(r_), g(g_), b(b_), a(a_) {}
    /// 0xRRGGBB (opaque) or, with has_alpha, 0xRRGGBBAA.
    [[nodiscard]] static constexpr Color from_rgb(u32 v) noexcept { return {static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v), 255}; }
    [[nodiscard]] static constexpr Color from_rgba(u32 v) noexcept { return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; }
    [[nodiscard]] constexpr u32 to_rgba() const noexcept { return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a; }
    [[nodiscard]] constexpr u32 to_argb() const noexcept { return (u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | b; }

    /// Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" (leading '#' optional) and the CSS basic color names.
    [[nodiscard]] static Color parse(std::string_view s);

    friend constexpr bool operator==(const Color&, const Color&) noexcept = default;

    [[nodiscard]] constexpr Color with_alpha(u8 alpha) const noexcept { return {r, g, b, alpha}; }
    [[nodiscard]] constexpr Color premultiplied() const noexcept {
        auto m = [&](u8 c) { return static_cast<u8>((u32(c) * a + 127) / 255); };
        return {m(r), m(g), m(b), a};
    }
    [[nodiscard]] constexpr Color unpremultiplied() const noexcept {
        if (a == 0 || a == 255) return a == 0 ? Color{0, 0, 0, 0} : *this;
        auto u = [&](u8 c) { return static_cast<u8>(std::min<u32>(255, (u32(c) * 255 + a / 2) / a)); };
        return {u(r), u(g), u(b), a};
    }
    [[nodiscard]] constexpr u8 luma() const noexcept { return static_cast<u8>((u32(r) * 54 + u32(g) * 183 + u32(b) * 19 + 128) >> 8); }

    /// Source-over compositing of straight-alpha colors.
    [[nodiscard]] static constexpr Color over(Color src, Color dst) noexcept {
        if (src.a == 255 || dst.a == 0) return src;
        if (src.a == 0) return dst;
        const u32 sa = src.a, da = dst.a;
        const u32 oa = sa * 255 + da * (255 - sa);  // alpha * 255
        auto ch = [&](u8 s, u8 d) { return static_cast<u8>((u32(s) * sa * 255 + u32(d) * da * (255 - sa) + oa / 2) / oa); };
        return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<u8>((oa + 127) / 255)};
    }
    [[nodiscard]] static constexpr Color lerp(Color x, Color y, float t) noexcept {
        auto l = [&](u8 p, u8 q) { return static_cast<u8>(std::clamp(static_cast<float>(p) + (static_cast<float>(q) - static_cast<float>(p)) * t + 0.5f, 0.0f, 255.0f)); };
        return {l(x.r, y.r), l(x.g, y.g), l(x.b, y.b), l(x.a, y.a)};
    }
    [[nodiscard]] constexpr ColorF to_float() const noexcept;
    [[nodiscard]] std::string to_hex() const;

    static const Color transparent, black, white, red, green, blue, yellow, cyan, magenta, gray;
};
inline constexpr Color Color::transparent{0, 0, 0, 0};
inline constexpr Color Color::black{0, 0, 0, 255};
inline constexpr Color Color::white{255, 255, 255, 255};
inline constexpr Color Color::red{255, 0, 0, 255};
inline constexpr Color Color::green{0, 255, 0, 255};
inline constexpr Color Color::blue{0, 0, 255, 255};
inline constexpr Color Color::yellow{255, 255, 0, 255};
inline constexpr Color Color::cyan{0, 255, 255, 255};
inline constexpr Color Color::magenta{255, 0, 255, 255};
inline constexpr Color Color::gray{128, 128, 128, 255};

/// 16-bit RGBA (PNG 16-bit images).
struct Color16 {
    u16 r = 0, g = 0, b = 0, a = 65535;
    friend constexpr bool operator==(const Color16&, const Color16&) noexcept = default;
};

/// Floating point RGBA (linear or encoded, depending on context). Used for HDR images and blending maths.
struct ColorF {
    float r = 0, g = 0, b = 0, a = 1;
    friend constexpr bool operator==(const ColorF&, const ColorF&) noexcept = default;
    [[nodiscard]] constexpr ColorF premultiplied() const noexcept { return {r * a, g * a, b * a, a}; }
    [[nodiscard]] Color to_color() const noexcept {
        auto q = [](float v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return {q(r), q(g), q(b), q(a)};
    }
    friend constexpr ColorF operator+(ColorF x, ColorF y) noexcept { return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a}; }
    friend constexpr ColorF operator-(ColorF x, ColorF y) noexcept { return {x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a}; }
    friend constexpr ColorF operator*(ColorF x, float s) noexcept { return {x.r * s, x.g * s, x.b * s, x.a * s}; }
    friend constexpr ColorF operator*(float s, ColorF x) noexcept { return x * s; }
    friend constexpr ColorF operator*(ColorF x, ColorF y) noexcept { return {x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a}; }
};
inline constexpr ColorF Color::to_float() const noexcept { return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f}; }

namespace color {

/// sRGB <-> linear light.
[[nodiscard]] inline float srgb_to_linear(float c) noexcept { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
[[nodiscard]] inline float linear_to_srgb(float c) noexcept { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
[[nodiscard]] inline const std::array<float, 256>& srgb8_to_linear_table() {
    static const std::array<float, 256> t = [] {
        std::array<float, 256> a{};
        for (usize i = 0; i < 256; ++i) a[i] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        return a;
    }();
    return t;
}
[[nodiscard]] inline ColorF to_linear(Color c) noexcept {
    const auto& t = srgb8_to_linear_table();
    return {t[c.r], t[c.g], t[c.b], c.a / 255.0f};
}
[[nodiscard]] inline Color from_linear(ColorF c) noexcept {
    return ColorF{linear_to_srgb(std::clamp(c.r, 0.0f, 1.0f)), linear_to_srgb(std::clamp(c.g, 0.0f, 1.0f)), linear_to_srgb(std::clamp(c.b, 0.0f, 1.0f)), c.a}.to_color();
}

/// Hue in degrees [0,360), saturation/value/lightness in [0,1].
[[nodiscard]] inline Color from_hsv(float h, float s, float v, u8 alpha = 255) noexcept {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    const int sector = static_cast<int>(h / 60.0f);
    switch (sector) {
        case 0: r = c, g = x; break;
        case 1: r = x, g = c; break;
        case 2: g = c, b = x; break;
        case 3: g = x, b = c; break;
        case 4: r = x, b = c; break;
        default: r = c, b = x; break;
    }
    return ColorF{r + m, g + m, b + m, alpha / 255.0f}.to_color();
}
[[nodiscard]] inline Color from_hsl(float h, float s, float l, u8 alpha = 255) noexcept {
    const float v = l + s * std::min(l, 1 - l);
    return from_hsv(h, v == 0 ? 0 : 2 * (1 - l / v), v, alpha);
}
struct Hsv { float h, s, v; };
[[nodiscard]] inline Hsv to_hsv(Color c) noexcept {
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    float h = 0;
    if (d > 0) {
        if (mx == r) h = 60.0f * std::fmod((g - b) / d, 6.0f);
        else if (mx == g) h = 60.0f * ((b - r) / d + 2);
        else h = 60.0f * ((r - g) / d + 4);
        if (h < 0) h += 360.0f;
    }
    return {h, mx == 0 ? 0 : d / mx, mx};
}

}  // namespace color

inline std::string Color::to_hex() const {
    static constexpr char hx[] = "0123456789abcdef";
    std::string s = "#";
    for (u8 v : {r, g, b}) { s += hx[v >> 4]; s += hx[v & 15]; }
    if (a != 255) { s += hx[a >> 4]; s += hx[a & 15]; }
    return s;
}

inline Color Color::parse(std::string_view s) {
    struct Named { std::string_view name; u32 rgb; };
    static constexpr Named names[] = {
        {"black", 0x000000}, {"silver", 0xc0c0c0}, {"gray", 0x808080}, {"grey", 0x808080}, {"white", 0xffffff}, {"maroon", 0x800000}, {"red", 0xff0000},
        {"purple", 0x800080}, {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff}, {"green", 0x008000}, {"lime", 0x00ff00}, {"olive", 0x808000}, {"yellow", 0xffff00},
        {"navy", 0x000080}, {"blue", 0x0000ff}, {"teal", 0x008080}, {"aqua", 0x00ffff}, {"cyan", 0x00ffff}, {"orange", 0xffa500}, {"pink", 0xffc0cb},
        {"brown", 0xa52a2a}, {"gold", 0xffd700}, {"indigo", 0x4b0082}, {"violet", 0xee82ee}, {"coral", 0xff7f50}, {"crimson", 0xdc143c}};
    std::string lower;
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "transparent") return transparent;
    for (const auto& n : names)
        if (n.name == lower) return from_rgb(n.rgb);
    std::string_view h = lower;
    if (!h.empty() && h[0] == '#') h.remove_prefix(1);
    auto digit = [&](char c) -> u32 {
        if (c >= '0' && c <= '9') return static_cast<u32>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u32>(c - 'a' + 10);
        throw GraphicsException("invalid color '" + std::string(s) + "'");
    };
    if (h.size() == 3 || h.size() == 4) {
        u8 v[4] = {0, 0, 0, 255};
        for (usize i = 0; i < h.size(); ++i) v[i] = static_cast<u8>(digit(h[i]) * 17);
        return {v[0], v[1], v[2], v[3]};
    }
    if (h.size() == 6 || h.size() == 8) {
        u8 v[4] = {0, 0, 0, 255};
        for (usize i = 0; i < h.size() / 2; ++i) v[i] = static_cast<u8>(digit(h[2 * i]) * 16 + digit(h[2 * i + 1]));
        return {v[0], v[1], v[2], v[3]};
    }
    throw GraphicsException("invalid color '" + std::string(s) + "'");
}

}  // namespace flyg::graphics
