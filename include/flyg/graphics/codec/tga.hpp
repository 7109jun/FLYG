// flyG - graphics module: TGA codec (types 1/2/3 and RLE 9/10/11; 8/15/16/24/32 bpp; any origin)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <span>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::tga {

constexpr unsigned le16u(unsigned lo, unsigned hi) noexcept { return lo | (hi << 8); }

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

[[nodiscard]] inline Image decode(ByteView in) {
    auto bad = [](const char* m) { throw DataException(std::string("tga: ") + m); };
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    if (n < 18) bad("file too short");
    const unsigned id_len = d[0], cmap_type = d[1], type = d[2];
    const unsigned cmap_first = le16u(d[3], d[4]), cmap_len = le16u(d[5], d[6]), cmap_bits = d[7];
    const unsigned w = le16u(d[12], d[13]), h = le16u(d[14], d[15]), bpp = d[16], desc = d[17];
    if (w == 0 || h == 0) bad("invalid dimensions");
    const bool rle = type >= 9;
    const unsigned base = type & 7;
    if (base != 1 && base != 2 && base != 3) bad("unsupported image type");
    if (cmap_type > 1) bad("invalid colour map type");
    usize p = 18 + id_len;
    std::vector<Color> cmap;
    if (cmap_type == 1) {
        const usize eb = (cmap_bits + 7) / 8;
        if (cmap_bits != 15 && cmap_bits != 16 && cmap_bits != 24 && cmap_bits != 32) bad("unsupported colour map depth");
        if (p + eb * cmap_len > n) bad("truncated colour map");
        cmap.resize(cmap_first + cmap_len);
        for (usize i = 0; i < cmap_len; ++i) {
            const u8* e = d + p + i * eb;
            Color c{};
            if (eb == 2) { const unsigned v = le16u(e[0], e[1]); c = {static_cast<u8>(((v >> 10) & 31) * 255 / 31), static_cast<u8>(((v >> 5) & 31) * 255 / 31), static_cast<u8>((v & 31) * 255 / 31), 255}; }
            else c = {e[2], e[1], e[0], eb == 4 ? e[3] : u8(255)};
            cmap[cmap_first + i] = c;
        }
        p += eb * cmap_len;
    }
    if (base == 1 && cmap_type == 0) bad("colour-mapped image without a colour map");
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) bad("unsupported pixel depth");
    if (base == 3 && bpp != 8 && bpp != 16) bad("unsupported grayscale depth");
    const usize pb = (bpp + 7) / 8;
    Image img(w, h);
    const bool top = desc & 0x20, right = desc & 0x10;
    const unsigned alpha_bits = desc & 15;
    auto decode_px = [&](const u8* e) -> Color {
        if (base == 1) {
            const unsigned idx = pb == 1 ? e[0] : (le16u(e[0], e[1]));
            if (idx >= cmap.size()) bad("colour map index out of range");
            return cmap[idx];
        }
        if (base == 3) return pb == 1 ? Color{e[0], e[0], e[0], 255} : Color{e[0], e[0], e[0], e[1]};
        if (pb == 2) {
            const unsigned v = le16u(e[0], e[1]);
            return {static_cast<u8>(((v >> 10) & 31) * 255 / 31), static_cast<u8>(((v >> 5) & 31) * 255 / 31), static_cast<u8>((v & 31) * 255 / 31), static_cast<u8>(bpp == 16 && alpha_bits ? ((v >> 15) ? 255 : 0) : 255)};
        }
        if (pb == 3) return {e[2], e[1], e[0], 255};
        return {e[2], e[1], e[0], alpha_bits ? e[3] : u8(255)};
    };
    const usize total = usize(w) * h;
    usize i = 0;
    auto store = [&](Color c) {
        const usize x = i % w, y = i / w;
        img(right ? w - 1 - x : x, top ? y : h - 1 - y) = c;
        ++i;
    };
    if (!rle) {
        if (p + total * pb > n) bad("pixel data is truncated");
        while (i < total) store(decode_px(d + p + i * pb));
    } else {
        while (i < total) {
            if (p >= n) bad("RLE data is truncated");
            const u8 hdr = d[p++];
            const usize count = (hdr & 0x7f) + 1;
            if (hdr & 0x80) {
                if (p + pb > n) bad("RLE data is truncated");
                const Color c = decode_px(d + p);
                p += pb;
                for (usize k = 0; k < count && i < total; ++k) store(c);
            } else {
                if (p + count * pb > n) bad("RLE data is truncated");
                for (usize k = 0; k < count && i < total; ++k) store(decode_px(d + p + k * pb));
                p += count * pb;
            }
        }
    }
    return img;
}

[[nodiscard]] inline Bytes encode(const Image& img, bool rle = true) {
    if (img.empty() || img.width() > 65535 || img.height() > 65535) throw DataException("tga: invalid image size");
    bool opaque = true;
    for (const Color& c : img.pixels()) opaque &= c.a == 255;
    const unsigned pb = opaque ? 3 : 4;
    Bytes o(18, std::byte{0});
    o[2] = static_cast<std::byte>(rle ? 10 : 2);
    o[12] = static_cast<std::byte>(img.width()), o[13] = static_cast<std::byte>(img.width() >> 8);
    o[14] = static_cast<std::byte>(img.height()), o[15] = static_cast<std::byte>(img.height() >> 8);
    o[16] = static_cast<std::byte>(pb * 8);
    o[17] = static_cast<std::byte>(0x20 | (opaque ? 0 : 8));  // top-left origin
    auto px = [&](const Color& c, Bytes& out) {
        out.push_back(static_cast<std::byte>(c.b)), out.push_back(static_cast<std::byte>(c.g)), out.push_back(static_cast<std::byte>(c.r));
        if (pb == 4) out.push_back(static_cast<std::byte>(c.a));
    };
    if (!rle) {
        for (const Color& c : img.pixels()) px(c, o);
    } else {
        for (usize y = 0; y < img.height(); ++y) {  // packets never span rows
            const auto row = img.row(y);
            usize x = 0;
            while (x < row.size()) {
                usize run = 1;
                while (x + run < row.size() && run < 128 && row[x + run] == row[x]) ++run;
                if (run >= 2) {
                    o.push_back(static_cast<std::byte>(0x80 | (run - 1)));
                    px(row[x], o);
                    x += run;
                } else {
                    usize raw = 1;
                    while (x + raw < row.size() && raw < 128 && !(x + raw + 1 < row.size() && row[x + raw] == row[x + raw + 1])) ++raw;
                    o.push_back(static_cast<std::byte>(raw - 1));
                    for (usize k = 0; k < raw; ++k) px(row[x + k], o);
                    x += raw;
                }
            }
        }
    }
    return o;
}

}  // namespace flyg::graphics::tga
