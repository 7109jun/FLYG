// flyG - graphics module: PNM codec (P1-P6 ASCII/binary, P7 PAM; maxval up to 65535)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <span>
#include <string>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::pnm {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

namespace detail {
[[noreturn]] inline void bad(const std::string& m) { throw DataException("pnm: " + m); }
struct Reader {
    const char* p;
    const char* end;
    void skip_ws() {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            if (p < end && *p == '#') { while (p < end && *p != '\n') ++p; } else break;
        }
    }
    u32 number() {
        skip_ws();
        if (p >= end || *p < '0' || *p > '9') bad("expected a number");
        u64 v = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + static_cast<u64>(*p++ - '0');
            if (v > 0xffffffffull) bad("number too large");
        }
        return static_cast<u32>(v);
    }
};
inline u8 scale(u32 v, u32 maxval) { return maxval == 255 ? static_cast<u8>(v) : static_cast<u8>((u64(v) * 255 + maxval / 2) / maxval); }
}  // namespace detail

[[nodiscard]] inline Image decode(ByteView in) {
    using namespace detail;
    const char* s = reinterpret_cast<const char*>(in.data());
    if (in.size() < 3 || s[0] != 'P') bad("not a PNM file");
    const char kind = s[1];
    Reader r{s + 2, s + in.size()};
    u32 w = 0, h = 0, maxval = 1;
    unsigned depth = 0;
    std::string tupltype;
    if (kind == '7') {
        for (;;) {
            r.skip_ws();
            std::string key;
            while (r.p < r.end && *r.p != ' ' && *r.p != '\n' && *r.p != '\r' && *r.p != '\t') key += *r.p++;
            if (key == "ENDHDR") { if (r.p < r.end) ++r.p; break; }
            if (key == "WIDTH") w = r.number();
            else if (key == "HEIGHT") h = r.number();
            else if (key == "DEPTH") depth = r.number();
            else if (key == "MAXVAL") maxval = r.number();
            else if (key == "TUPLTYPE") { r.skip_ws(); while (r.p < r.end && *r.p != '\n') tupltype += *r.p++; }
            else if (key.empty()) bad("truncated PAM header");
            else { while (r.p < r.end && *r.p != '\n') ++r.p; }
        }
    } else {
        if (kind < '1' || kind > '6') bad("unsupported PNM variant");
        w = r.number();
        h = r.number();
        if (kind != '1' && kind != '4') maxval = r.number();
        depth = (kind == '3' || kind == '6') ? 3 : 1;
    }
    if (w == 0 || h == 0 || w > 1u << 16 || h > 1u << 16) bad("invalid dimensions");
    if (maxval == 0 || maxval > 65535) bad("invalid maxval");
    Image img(w, h);
    const bool ascii = kind == '1' || kind == '2' || kind == '3';
    const bool bitmap = kind == '1' || kind == '4';
    if (kind == '7' && (depth < 1 || depth > 4)) bad("unsupported PAM depth");
    const bool wide = maxval > 255;
    if (ascii) {
        for (usize i = 0; i < img.pixel_count(); ++i) {
            if (bitmap) {
                r.skip_ws();
                if (r.p >= r.end || (*r.p != '0' && *r.p != '1')) bad("expected 0 or 1");
                const u8 v = *r.p++ == '1' ? 0 : 255;
                img.data()[i] = {v, v, v, 255};
            } else if (depth == 1) {
                const u8 v = scale(r.number(), maxval);
                img.data()[i] = {v, v, v, 255};
            } else {
                const u8 a = scale(r.number(), maxval), b = scale(r.number(), maxval), c = scale(r.number(), maxval);
                img.data()[i] = {a, b, c, 255};
            }
        }
        return img;
    }
    if (kind != '7') ++r.p;  // single whitespace after the header
    const u8* d = reinterpret_cast<const u8*>(r.p);
    const usize avail = static_cast<usize>(reinterpret_cast<const u8*>(r.end) - d);
    if (bitmap) {
        const usize rb = (w + 7) / 8;
        if (avail < rb * h) bad("pixel data is truncated");
        for (usize y = 0; y < h; ++y)
            for (usize x = 0; x < w; ++x) {
                const u8 v = ((d[y * rb + x / 8] >> (7 - x % 8)) & 1) ? 0 : 255;
                img(x, y) = {v, v, v, 255};
            }
        return img;
    }
    const usize bps = wide ? 2 : 1;
    if (avail < usize(w) * h * depth * bps) bad("pixel data is truncated");
    auto sample = [&](usize idx) -> u8 { return scale(wide ? static_cast<u32>((d[2 * idx] << 8) | d[2 * idx + 1]) : d[idx], maxval); };
    for (usize i = 0; i < img.pixel_count(); ++i) {
        switch (depth) {
            case 1: { const u8 v = sample(i); img.data()[i] = {v, v, v, 255}; break; }
            case 2: { const u8 v = sample(2 * i), a = sample(2 * i + 1); img.data()[i] = {v, v, v, a}; break; }
            case 3: img.data()[i] = {sample(3 * i), sample(3 * i + 1), sample(3 * i + 2), 255}; break;
            default: img.data()[i] = {sample(4 * i), sample(4 * i + 1), sample(4 * i + 2), sample(4 * i + 3)}; break;
        }
    }
    return img;
}

enum class Variant { auto_select, pgm, ppm, pam };

/// Writes binary PNM: PGM for gray, PPM for opaque colour, PAM (RGB_ALPHA) when transparency must be kept.
[[nodiscard]] inline Bytes encode(const Image& img, Variant v = Variant::auto_select) {
    if (img.empty()) throw DataException("pnm: cannot encode an empty image");
    bool opaque = true, gray = true;
    for (const Color& c : img.pixels()) { opaque &= c.a == 255; gray &= c.r == c.g && c.g == c.b; }
    if (v == Variant::auto_select) v = !opaque ? Variant::pam : gray ? Variant::pgm : Variant::ppm;
    std::string head;
    usize ch = 3;
    if (v == Variant::pgm) { head = "P5\n" + std::to_string(img.width()) + " " + std::to_string(img.height()) + "\n255\n"; ch = 1; }
    else if (v == Variant::ppm) head = "P6\n" + std::to_string(img.width()) + " " + std::to_string(img.height()) + "\n255\n";
    else { head = "P7\nWIDTH " + std::to_string(img.width()) + "\nHEIGHT " + std::to_string(img.height()) + "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n"; ch = 4; }
    Bytes o(reinterpret_cast<const std::byte*>(head.data()), reinterpret_cast<const std::byte*>(head.data()) + head.size());
    o.reserve(o.size() + img.pixel_count() * ch);
    for (const Color& c : img.pixels()) {
        if (ch == 1) o.push_back(static_cast<std::byte>(c.luma()));
        else {
            o.push_back(static_cast<std::byte>(c.r)), o.push_back(static_cast<std::byte>(c.g)), o.push_back(static_cast<std::byte>(c.b));
            if (ch == 4) o.push_back(static_cast<std::byte>(c.a));
        }
    }
    return o;
}

}  // namespace flyg::graphics::pnm
