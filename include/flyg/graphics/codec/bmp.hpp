// flyG - graphics module: BMP codec (1/4/8/16/24/32 bpp, RLE4/RLE8, bitfields, OS/2 core headers, top-down)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::bmp {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

namespace detail {
[[noreturn]] inline void bad(const std::string& m) { throw DataException("bmp: " + m); }
inline u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
inline u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline void w16(Bytes& o, u32 v) { o.push_back(static_cast<std::byte>(v)); o.push_back(static_cast<std::byte>(v >> 8)); }
inline void w32(Bytes& o, u32 v) { for (int s = 0; s < 32; s += 8) o.push_back(static_cast<std::byte>(v >> s)); }

struct Mask {
    u32 mask = 0;
    unsigned shift = 0, bits = 0;
    void set(u32 m) {
        mask = m;
        shift = 0;
        bits = 0;
        if (!m) return;
        while (shift < 31 && !((m >> shift) & 1)) ++shift;
        while (shift + bits < 32 && ((m >> (shift + bits)) & 1)) ++bits;
    }
    [[nodiscard]] u8 get(u32 v) const {
        if (!mask) return 0;
        const u32 x = (v & mask) >> shift;
        if (bits >= 8) return static_cast<u8>(x >> (bits - 8));
        return static_cast<u8>((u64(x) * 255) / ((u64(1) << bits) - 1));
    }
};
}  // namespace detail

[[nodiscard]] inline Image decode(ByteView in) {
    using namespace detail;
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    if (n < 26 || d[0] != 'B' || d[1] != 'M') bad("not a BMP file");
    const u32 data_off = rd32(d + 10);
    const u32 hsize = rd32(d + 14);
    long w = 0, h = 0;
    unsigned bpp = 0, compression = 0, ncolors = 0;
    usize pal_entry = 4;
    u32 masks[4] = {0, 0, 0, 0};
    bool v4plus = false;
    if (hsize == 12) {
        if (n < 26) bad("truncated header");
        w = rd16(d + 18);
        h = rd16(d + 20);
        bpp = rd16(d + 24);
        pal_entry = 3;
    } else if (hsize >= 40) {
        if (n < 14 + hsize) bad("truncated header");
        w = static_cast<int32_t>(rd32(d + 18));
        h = static_cast<int32_t>(rd32(d + 22));
        bpp = rd16(d + 28);
        compression = rd32(d + 30);
        ncolors = rd32(d + 46);
        if (hsize >= 52) { masks[0] = rd32(d + 54); masks[1] = rd32(d + 58); masks[2] = rd32(d + 62); }
        if (hsize >= 56) masks[3] = rd32(d + 66);
        v4plus = hsize >= 108;
    } else {
        bad("unsupported header size");
    }
    const bool top_down = h < 0;
    if (top_down) h = -h;
    if (w <= 0 || h <= 0 || w > (1 << 16) || h > (1 << 16)) bad("invalid dimensions");
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bad("unsupported bit depth");
    if (compression > 6 || compression == 4 || compression == 5) bad("unsupported compression (JPEG/PNG-in-BMP)");
    const usize W = static_cast<usize>(w), H = static_cast<usize>(h);
    Image img(W, H, Color{0, 0, 0, 255});

    usize pal_off = 14 + hsize;
    if (hsize == 40 && compression == 3) pal_off += 12;   // masks follow the 40-byte header
    if (hsize == 40 && compression == 6) pal_off += 16;
    if (compression == 3 || compression == 6) {
        if (hsize == 40) {
            if (n < 14 + 40 + 12) bad("truncated bitfields");
            masks[0] = rd32(d + 54), masks[1] = rd32(d + 58), masks[2] = rd32(d + 62);
            if (compression == 6) masks[3] = rd32(d + 66);
        }
    }
    std::vector<Color> pal;
    if (bpp <= 8) {
        const usize count = ncolors ? ncolors : (usize(1) << bpp);
        if (count > 256 || pal_off + count * pal_entry > n) bad("truncated palette");
        for (usize i = 0; i < count; ++i) {
            const u8* p = d + pal_off + i * pal_entry;
            pal.push_back({p[2], p[1], p[0], 255});
        }
    }
    if (data_off > n) bad("pixel data offset out of range");
    const u8* px = d + data_off;
    const usize avail = n - data_off;
    auto put = [&](usize x, long row, Color c) {
        const usize y = top_down ? static_cast<usize>(row) : H - 1 - static_cast<usize>(row);
        if (x < W && static_cast<usize>(row) < H) img(x, y) = c;
    };

    if (compression == 1 || compression == 2) {  // RLE8 / RLE4
        if (bpp != (compression == 1 ? 8u : 4u)) bad("RLE compression does not match bit depth");
        usize p = 0;
        long x = 0, y = 0;
        auto pal_at = [&](unsigned i) { return i < pal.size() ? pal[i] : Color{0, 0, 0, 255}; };
        bool done = false;
        while (!done && p + 1 < avail) {
            const u8 c = px[p], v = px[p + 1];
            p += 2;
            if (c > 0) {
                for (unsigned i = 0; i < c; ++i) {
                    const unsigned idx = compression == 1 ? v : ((i & 1) ? (v & 15) : (v >> 4));
                    put(static_cast<usize>(x++), y, pal_at(idx));
                }
            } else if (v == 0) { x = 0; ++y; }
            else if (v == 1) done = true;
            else if (v == 2) {
                if (p + 1 >= avail) bad("truncated RLE data");
                x += px[p];
                y += px[p + 1];
                p += 2;
            } else {
                const unsigned cnt = v;
                const usize bytes = compression == 1 ? cnt : (cnt + 1) / 2;
                if (p + bytes > avail) bad("truncated RLE data");
                for (unsigned i = 0; i < cnt; ++i) {
                    const unsigned idx = compression == 1 ? px[p + i] : ((i & 1) ? (px[p + i / 2] & 15) : (px[p + i / 2] >> 4));
                    put(static_cast<usize>(x++), y, pal_at(idx));
                }
                p += bytes + (bytes & 1);
            }
            if (y >= h) break;
        }
        return img;
    }

    const usize stride = ((W * bpp + 31) / 32) * 4;
    if (stride * H > avail) bad("pixel data is truncated");
    detail::Mask mr, mg, mb, ma;
    bool bitfields = compression == 3 || compression == 6;
    if (bpp == 16 && !bitfields) { masks[0] = 0x7c00, masks[1] = 0x03e0, masks[2] = 0x001f; bitfields = true; }
    if (bpp == 32 && !bitfields) { masks[0] = 0x00ff0000, masks[1] = 0x0000ff00, masks[2] = 0x000000ff; masks[3] = v4plus ? masks[3] : 0; bitfields = true; if (!v4plus) masks[3] = 0; }
    mr.set(masks[0]); mg.set(masks[1]); mb.set(masks[2]); ma.set(masks[3]);
    bool any_alpha = false;
    for (usize row = 0; row < H; ++row) {
        const u8* r = px + row * stride;
        for (usize x = 0; x < W; ++x) {
            Color c{0, 0, 0, 255};
            if (bpp <= 8) {
                const usize bit = x * bpp;
                const unsigned idx = (r[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1u << bpp) - 1);
                if (idx >= pal.size()) bad("palette index out of range");
                c = pal[idx];
            } else if (bpp == 24) {
                c = {r[3 * x + 2], r[3 * x + 1], r[3 * x], 255};
            } else {
                const u32 v = bpp == 16 ? rd16(r + 2 * x) : rd32(r + 4 * x);
                c = {mr.get(v), mg.get(v), mb.get(v), ma.mask ? ma.get(v) : u8(255)};
                any_alpha |= c.a != 0;
            }
            put(x, static_cast<long>(row), c);
        }
    }
    // A 32-bit file declaring an alpha mask but containing only zeros is really opaque.
    if (bpp == 32 && ma.mask && !any_alpha)
        for (auto& p : img.pixels()) p.a = 255;
    return img;
}

struct EncodeOptions {
    int bits = 0;  ///< 0 = automatic (24, or 32 when the image has transparency), or 8 (palette, <=256 colours), 24, 32
};

[[nodiscard]] inline Bytes encode(const Image& img, const EncodeOptions& opt = {}) {
    using namespace detail;
    if (img.empty()) bad("cannot encode an empty image");
    bool opaque = true;
    for (const Color& c : img.pixels()) opaque &= c.a == 255;
    int bits = opt.bits;
    std::vector<Color> pal;
    if (bits == 8) {
        for (const Color& c : img.pixels())
            if (std::find(pal.begin(), pal.end(), c) == pal.end()) {
                pal.push_back(c);
                if (pal.size() > 256) bad("more than 256 colours: cannot write an 8-bit BMP");
            }
    }
    if (bits == 0) bits = opaque ? 24 : 32;
    if (bits != 8 && bits != 24 && bits != 32) bad("unsupported output bit depth");
    const usize W = img.width(), H = img.height();
    const usize stride = ((W * static_cast<usize>(bits) + 31) / 32) * 4;
    const u32 header = bits == 32 ? 108 : 40;
    const u32 pal_bytes = bits == 8 ? 256 * 4 : 0;
    const u32 off = 14 + header + pal_bytes;
    Bytes o;
    o.push_back(static_cast<std::byte>('B'));
    o.push_back(static_cast<std::byte>('M'));
    w32(o, static_cast<u32>(off + stride * H));
    w32(o, 0);
    w32(o, off);
    w32(o, header);
    w32(o, static_cast<u32>(W));
    w32(o, static_cast<u32>(H));  // bottom-up
    w16(o, 1);
    w16(o, static_cast<u32>(bits));
    w32(o, bits == 32 ? 3 : 0);   // BI_BITFIELDS for 32-bit so the alpha mask is honoured
    w32(o, static_cast<u32>(stride * H));
    w32(o, 2835);
    w32(o, 2835);
    w32(o, bits == 8 ? static_cast<u32>(pal.size()) : 0);
    w32(o, 0);
    if (bits == 32) {
        w32(o, 0x00ff0000), w32(o, 0x0000ff00), w32(o, 0x000000ff), w32(o, 0xff000000);
        w32(o, 0x73524742);  // 'sRGB'
        for (int i = 0; i < 12 + 12 + 12 + 12; ++i) o.push_back(std::byte{0});  // endpoints + gamma (unused for sRGB)
    }
    if (bits == 8) {
        for (usize i = 0; i < 256; ++i) {
            const Color c = i < pal.size() ? pal[i] : Color{};
            o.push_back(static_cast<std::byte>(c.b)), o.push_back(static_cast<std::byte>(c.g)), o.push_back(static_cast<std::byte>(c.r)), o.push_back(std::byte{0});
        }
    }
    for (usize row = 0; row < H; ++row) {
        const usize y = H - 1 - row;
        const usize start = o.size();
        for (usize x = 0; x < W; ++x) {
            const Color c = img(x, y);
            if (bits == 8) o.push_back(static_cast<std::byte>(std::find(pal.begin(), pal.end(), c) - pal.begin()));
            else {
                o.push_back(static_cast<std::byte>(c.b)), o.push_back(static_cast<std::byte>(c.g)), o.push_back(static_cast<std::byte>(c.r));
                if (bits == 32) o.push_back(static_cast<std::byte>(c.a));
            }
        }
        while ((o.size() - start) % 4) o.push_back(std::byte{0});
    }
    return o;
}

}  // namespace flyg::graphics::bmp
