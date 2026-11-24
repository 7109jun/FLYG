// flyG - graphics module: Radiance HDR (RGBE) codec -> ImageF (linear float RGB)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::hdr {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

[[nodiscard]] inline ImageF decode(ByteView in) {
    auto bad = [](const char* m) { throw DataException(std::string("hdr: ") + m); };
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    usize p = 0;
    auto line = [&]() {
        std::string s;
        while (p < n && d[p] != '\n') s += static_cast<char>(d[p++]);
        if (p >= n) bad("truncated header");
        ++p;
        return s;
    };
    const std::string magic = line();
    if (magic.rfind("#?", 0) != 0) bad("not a Radiance HDR file");
    bool rgbe = false;
    for (;;) {
        const std::string l = line();
        if (l.empty()) break;
        if (l.rfind("FORMAT=32-bit_rle_rgbe", 0) == 0) rgbe = true;
        if (l.rfind("FORMAT=32-bit_rle_xyze", 0) == 0) bad("XYZE colour space is not supported");
    }
    (void)rgbe;
    const std::string res = line();
    char ya = 0, xa = 0;
    long hh = 0, ww = 0;
    if (std::sscanf(res.c_str(), "%cY %ld %cX %ld", &ya, &hh, &xa, &ww) != 4 || (ya != '-' && ya != '+') || (xa != '-' && xa != '+') || hh <= 0 || ww <= 0 || hh > 1 << 16 || ww > 1 << 16)
        bad("unsupported resolution line");
    const usize W = static_cast<usize>(ww), H = static_cast<usize>(hh);
    ImageF img(W, H);
    std::vector<u8> scan(W * 4);
    for (usize y = 0; y < H; ++y) {
        if (p + 4 > n) bad("truncated pixel data");
        if (W >= 8 && W < 32768 && d[p] == 2 && d[p + 1] == 2 && !(d[p + 2] & 0x80)) {
            if (usize((d[p + 2] << 8) | d[p + 3]) != W) bad("scanline width mismatch");
            p += 4;
            for (int c = 0; c < 4; ++c) {
                usize x = 0;
                while (x < W) {
                    if (p >= n) bad("truncated RLE data");
                    unsigned cnt = d[p++];
                    if (cnt > 128) {
                        cnt -= 128;
                        if (p >= n || x + cnt > W) bad("bad RLE run");
                        const u8 v = d[p++];
                        while (cnt--) scan[4 * x++ + static_cast<usize>(c)] = v;
                    } else {
                        if (cnt == 0 || p + cnt > n || x + cnt > W) bad("bad RLE literal");
                        while (cnt--) scan[4 * x++ + static_cast<usize>(c)] = d[p++];
                    }
                }
            }
        } else {  // flat (possibly old-style RLE with 1,1,1,n runs)
            usize x = 0;
            unsigned shift = 0;
            while (x < W) {
                if (p + 4 > n) bad("truncated pixel data");
                if (d[p] == 1 && d[p + 1] == 1 && d[p + 2] == 1 && x > 0) {
                    const usize cnt = static_cast<usize>(d[p + 3]) << shift;
                    if (x + cnt > W) bad("bad RLE run");
                    for (usize k = 0; k < cnt; ++k, ++x) std::memcpy(&scan[4 * x], &scan[4 * (x - 1)], 4);
                    shift += 8;
                } else {
                    std::memcpy(&scan[4 * x++], d + p, 4);
                    shift = 0;
                }
                p += 4;
            }
        }
        for (usize x = 0; x < W; ++x) {
            const u8* e = &scan[4 * x];
            ColorF c{0, 0, 0, 1};
            if (e[3]) {
                const float f = std::ldexp(1.0f, static_cast<int>(e[3]) - (128 + 8));
                c = {static_cast<float>(e[0]) * f, static_cast<float>(e[1]) * f, static_cast<float>(e[2]) * f, 1};
            }
            img(xa == '+' ? x : W - 1 - x, ya == '-' ? y : H - 1 - y) = c;
        }
    }
    return img;
}

[[nodiscard]] inline Bytes encode(const ImageF& img) {
    if (img.empty()) throw DataException("hdr: cannot encode an empty image");
    const std::string head = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(img.height()) + " +X " + std::to_string(img.width()) + "\n";
    Bytes o(reinterpret_cast<const std::byte*>(head.data()), reinterpret_cast<const std::byte*>(head.data()) + head.size());
    const usize W = img.width();
    std::vector<u8> scan(W * 4);
    for (usize y = 0; y < img.height(); ++y) {
        for (usize x = 0; x < W; ++x) {
            const ColorF c = img(x, y);
            const float m = std::max({c.r, c.g, c.b});
            u8* e = &scan[4 * x];
            if (!(m > 1e-32f)) { e[0] = e[1] = e[2] = e[3] = 0; continue; }
            int ex = 0;
            const float mant = std::frexp(m, &ex) * 256.0f / m;
            e[0] = static_cast<u8>(std::clamp(c.r, 0.0f, m) * mant);
            e[1] = static_cast<u8>(std::clamp(c.g, 0.0f, m) * mant);
            e[2] = static_cast<u8>(std::clamp(c.b, 0.0f, m) * mant);
            e[3] = static_cast<u8>(ex + 128);
        }
        if (W < 8 || W >= 32768) {
            for (usize i = 0; i < W * 4; ++i) o.push_back(static_cast<std::byte>(scan[i]));
            continue;
        }
        o.push_back(std::byte{2}), o.push_back(std::byte{2}), o.push_back(static_cast<std::byte>(W >> 8)), o.push_back(static_cast<std::byte>(W & 255));
        for (usize c = 0; c < 4; ++c) {
            usize x = 0;
            while (x < W) {
                usize run = 1;
                while (x + run < W && run < 127 && scan[4 * (x + run) + c] == scan[4 * x + c]) ++run;
                if (run >= 3) {
                    o.push_back(static_cast<std::byte>(128 + run)), o.push_back(static_cast<std::byte>(scan[4 * x + c]));
                    x += run;
                } else {
                    usize lit = 1;
                    while (x + lit < W && lit < 128) {
                        usize r2 = 1;
                        while (x + lit + r2 < W && r2 < 3 && scan[4 * (x + lit + r2) + c] == scan[4 * (x + lit) + c]) ++r2;
                        if (r2 >= 3) break;
                        ++lit;
                    }
                    o.push_back(static_cast<std::byte>(lit));
                    for (usize k = 0; k < lit; ++k) o.push_back(static_cast<std::byte>(scan[4 * (x + k) + c]));
                    x += lit;
                }
            }
        }
    }
    return o;
}

}  // namespace flyg::graphics::hdr
