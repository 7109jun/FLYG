// flyG - graphics module: QOI ("Quite OK Image") codec
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <span>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::qoi {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

[[nodiscard]] inline Bytes encode(const Image& img) {
    if (img.empty()) throw DataException("qoi: cannot encode an empty image");
    Bytes o;
    auto put = [&](u32 v) { o.push_back(static_cast<std::byte>(v)); };
    auto put32 = [&](u32 v) { for (int s = 24; s >= 0; s -= 8) put(v >> s); };
    for (char c : {'q', 'o', 'i', 'f'}) put(static_cast<u8>(c));
    put32(static_cast<u32>(img.width()));
    put32(static_cast<u32>(img.height()));
    bool alpha = false;
    for (const Color& c : img.pixels()) alpha |= c.a != 255;
    put(alpha ? 4 : 3);
    put(0);
    Color index[64]{};
    Color prev{0, 0, 0, 255};
    unsigned run = 0;
    const usize n = img.pixel_count();
    for (usize i = 0; i < n; ++i) {
        const Color px = img.data()[i];
        if (px == prev) {
            if (++run == 62 || i + 1 == n) { put(0xc0 | (run - 1)); run = 0; }
            continue;
        }
        if (run) { put(0xc0 | (run - 1)); run = 0; }
        const unsigned h = (px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) & 63;
        if (index[h] == px) {
            put(h);
        } else {
            index[h] = px;
            if (px.a == prev.a) {
                const int dr = px.r - prev.r, dg = px.g - prev.g, db = px.b - prev.b;
                const int dr_dg = dr - dg, db_dg = db - dg;
                if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
                    put(0x40 | static_cast<u32>((dr + 2) << 4) | static_cast<u32>((dg + 2) << 2) | static_cast<u32>(db + 2));
                } else if (dr_dg > -9 && dr_dg < 8 && dg > -33 && dg < 32 && db_dg > -9 && db_dg < 8) {
                    put(0x80 | static_cast<u32>(dg + 32));
                    put(static_cast<u32>((dr_dg + 8) << 4) | static_cast<u32>(db_dg + 8));
                } else {
                    put(0xfe), put(px.r), put(px.g), put(px.b);
                }
            } else {
                put(0xff), put(px.r), put(px.g), put(px.b), put(px.a);
            }
        }
        prev = px;
    }
    for (int i = 0; i < 7; ++i) put(0);
    put(1);
    return o;
}

[[nodiscard]] inline Image decode(ByteView in) {
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    auto bad = [](const char* m) { throw DataException(std::string("qoi: ") + m); };
    if (n < 14 + 8 || d[0] != 'q' || d[1] != 'o' || d[2] != 'i' || d[3] != 'f') bad("not a QOI file");
    auto r32 = [&](usize o) { return (u32(d[o]) << 24) | (u32(d[o + 1]) << 16) | (u32(d[o + 2]) << 8) | d[o + 3]; };
    const u32 w = r32(4), h = r32(8);
    if (w == 0 || h == 0 || u64(w) * h > (u64(1) << 28)) bad("invalid dimensions");
    if (d[12] != 3 && d[12] != 4) bad("invalid channel count");
    Image img(w, h);
    Color index[64]{};
    Color px{0, 0, 0, 255};
    usize p = 14;
    const usize end = n - 8;
    unsigned run = 0;
    for (usize i = 0; i < img.pixel_count(); ++i) {
        if (run) {
            --run;
        } else {
            if (p >= end) bad("truncated data");
            const u8 b = d[p++];
            if (b == 0xfe) { if (p + 3 > end) bad("truncated data"); px.r = d[p], px.g = d[p + 1], px.b = d[p + 2]; p += 3; }
            else if (b == 0xff) { if (p + 4 > end) bad("truncated data"); px = {d[p], d[p + 1], d[p + 2], d[p + 3]}; p += 4; }
            else if ((b & 0xc0) == 0x00) px = index[b];
            else if ((b & 0xc0) == 0x40) {
                px.r = static_cast<u8>(px.r + ((b >> 4) & 3) - 2);
                px.g = static_cast<u8>(px.g + ((b >> 2) & 3) - 2);
                px.b = static_cast<u8>(px.b + (b & 3) - 2);
            } else if ((b & 0xc0) == 0x80) {
                if (p >= end) bad("truncated data");
                const int dg = (b & 0x3f) - 32, b2 = d[p++];
                px.r = static_cast<u8>(px.r + dg - 8 + ((b2 >> 4) & 15));
                px.g = static_cast<u8>(px.g + dg);
                px.b = static_cast<u8>(px.b + dg - 8 + (b2 & 15));
            } else {
                run = b & 0x3f;
            }
            index[(px.r * 3 + px.g * 5 + px.b * 7 + px.a * 11) & 63] = px;
        }
        img.data()[i] = px;
    }
    return img;
}

}  // namespace flyg::graphics::qoi
