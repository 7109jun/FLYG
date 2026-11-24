// flyG - graphics module: PNG codec (all colour types and bit depths, Adam7, tRNS, tEXt; palette/gray optimising encoder)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <flyg/data/compress.hpp>
#include <flyg/graphics/image.hpp>

namespace flyg::graphics::png {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

enum class ColorType : u8 { gray = 0, rgb = 2, palette = 3, gray_alpha = 4, rgba = 6 };

struct Info {
    u32 width = 0, height = 0;
    u8 bit_depth = 8;
    ColorType color_type = ColorType::rgba;
    bool interlaced = false;
    bool has_alpha = false;                       ///< alpha channel or tRNS present
    std::map<std::string, std::string> text;      ///< tEXt / zTXt keyword -> text
    double gamma = 0;                             ///< from gAMA (0 = absent)
};

struct EncodeOptions {
    int level = 6;                  ///< zlib level 0..9
    bool interlace = false;         ///< Adam7
    bool optimize = true;           ///< choose palette / gray / smaller colour types automatically
    bool adaptive_filter = true;    ///< per-row filter selection (min sum of absolute differences)
    std::map<std::string, std::string> text;  ///< tEXt chunks (Latin-1 keywords, max 79 chars)
};

namespace detail {

inline constexpr std::array<u8, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};

[[noreturn]] inline void bad(const std::string& m) { throw DataException("png: " + m); }

inline u32 be32(const u8* p) noexcept { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

struct Chunk {
    std::string type;
    ByteView data;
};

inline std::vector<Chunk> read_chunks(ByteView in) {
    if (in.size() < 8 || !std::equal(signature.begin(), signature.end(), reinterpret_cast<const u8*>(in.data()))) bad("not a PNG file");
    std::vector<Chunk> out;
    usize pos = 8;
    bool saw_iend = false;
    while (pos + 12 <= in.size() && !saw_iend) {
        const u8* p = reinterpret_cast<const u8*>(in.data()) + pos;
        const u32 len = be32(p);
        if (len > 0x7fffffffu || pos + 12 + len > in.size()) bad("truncated chunk");
        Chunk c;
        c.type.assign(reinterpret_cast<const char*>(p + 4), 4);
        c.data = in.subspan(pos + 8, len);
        const u32 want = be32(p + 8 + len);
        const u32 got = data::crc32(in.subspan(pos + 4, 4 + len));
        if (want != got) bad("CRC mismatch in chunk " + c.type);
        if (c.type == "IEND") saw_iend = true;
        out.push_back(std::move(c));
        pos += 12 + len;
    }
    if (!saw_iend) bad("missing IEND chunk");
    return out;
}

struct Header {
    u32 w = 0, h = 0;
    u8 depth = 0, ctype = 0, interlace = 0;
    [[nodiscard]] unsigned channels() const { return ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4; }
    [[nodiscard]] unsigned bits_per_pixel() const { return channels() * depth; }
    [[nodiscard]] usize row_bytes(usize width) const { return (width * bits_per_pixel() + 7) / 8; }
};

inline Header parse_ihdr(const std::vector<Chunk>& chunks) {
    if (chunks.empty() || chunks[0].type != "IHDR" || chunks[0].data.size() != 13) bad("missing or malformed IHDR");
    const u8* p = reinterpret_cast<const u8*>(chunks[0].data.data());
    Header h;
    h.w = be32(p);
    h.h = be32(p + 4);
    h.depth = p[8];
    h.ctype = p[9];
    h.interlace = p[12];
    if (h.w == 0 || h.h == 0 || h.w > 1u << 20 || h.h > 1u << 20 || u64(h.w) * h.h > (u64(1) << 28)) bad("unsupported image dimensions");
    if (p[10] != 0 || p[11] != 0) bad("unsupported compression or filter method");
    if (h.interlace > 1) bad("invalid interlace method");
    const bool ok = (h.ctype == 0 && (h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8 || h.depth == 16)) ||
                    ((h.ctype == 2 || h.ctype == 4 || h.ctype == 6) && (h.depth == 8 || h.depth == 16)) ||
                    (h.ctype == 3 && (h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8));
    if (!ok) bad("invalid colour type / bit depth combination");
    return h;
}

struct Pass { u32 x0, y0, dx, dy; };
inline constexpr std::array<Pass, 7> adam7 = {{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};

inline void unfilter_row(u8 ft, u8* row, const u8* prev, usize n, usize bpp) {
    switch (ft) {
        case 0: break;
        case 1: for (usize i = bpp; i < n; ++i) row[i] = static_cast<u8>(row[i] + row[i - bpp]); break;
        case 2: if (prev) for (usize i = 0; i < n; ++i) row[i] = static_cast<u8>(row[i] + prev[i]); break;
        case 3:
            for (usize i = 0; i < n; ++i) {
                const unsigned a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0;
                row[i] = static_cast<u8>(row[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (usize i = 0; i < n; ++i) {
                const int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                const int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                row[i] = static_cast<u8>(row[i] + pred);
            }
            break;
        default: bad("invalid filter type");
    }
}

template <class Pix, bool Wide>
Pix make_pixel(const Header& h, const u8* row, usize x, const std::vector<Color>& palette, const std::vector<u8>& palette_alpha, bool has_trns, u16 trns[3]) {
    const unsigned d = h.depth;
    auto sample = [&](usize idx) -> u32 {  // idx = sample index inside the row
        if (d == 16) return (u32(row[2 * idx]) << 8) | row[2 * idx + 1];
        if (d == 8) return row[idx];
        const usize bit = idx * d;
        return (row[bit >> 3] >> (8 - d - (bit & 7))) & ((1u << d) - 1);
    };
    auto scale8 = [&](u32 v) -> u8 {
        if (d == 8) return static_cast<u8>(v);
        if (d == 16) return static_cast<u8>((v * 255 + 32767) / 65535);
        return static_cast<u8>(v * 255 / ((1u << d) - 1));
    };
    auto out = [&](u32 r, u32 g, u32 b, u32 a_raw, bool a8_given) -> Pix {
        if constexpr (Wide) {
            auto w16 = [&](u32 v) -> u16 { return d == 16 ? static_cast<u16>(v) : static_cast<u16>(scale8(v) * 257); };
            return Pix{w16(r), w16(g), w16(b), a8_given ? static_cast<u16>(a_raw * 257) : static_cast<u16>(a_raw)};
        } else {
            return Pix{scale8(r), scale8(g), scale8(b), a8_given ? static_cast<u8>(a_raw) : static_cast<u8>(d == 16 ? (a_raw * 255 + 32767) / 65535 : a_raw)};
        }
    };
    const u32 opaque = Wide ? 65535u : 255u;
    switch (h.ctype) {
        case 0: {
            const u32 v = sample(x);
            const bool transparent = has_trns && v == trns[0];
            return out(v, v, v, transparent ? 0 : (Wide ? 65535u : 255u), Wide ? false : true);
        }
        case 2: {
            const u32 r = sample(3 * x), g = sample(3 * x + 1), b = sample(3 * x + 2);
            const bool transparent = has_trns && r == trns[0] && g == trns[1] && b == trns[2];
            return out(r, g, b, transparent ? 0 : opaque, Wide ? false : true);
        }
        case 3: {
            const u32 idx = sample(x);
            if (idx >= palette.size()) bad("palette index out of range");
            const Color c = palette[idx];
            const u8 a = idx < palette_alpha.size() ? palette_alpha[idx] : 255;
            if constexpr (Wide) return Pix{static_cast<u16>(c.r * 257), static_cast<u16>(c.g * 257), static_cast<u16>(c.b * 257), static_cast<u16>(a * 257)};
            else return Pix{c.r, c.g, c.b, a};
        }
        case 4: {
            const u32 v = sample(2 * x), a = sample(2 * x + 1);
            return out(v, v, v, a, false);
        }
        default: {
            const u32 r = sample(4 * x), g = sample(4 * x + 1), b = sample(4 * x + 2), a = sample(4 * x + 3);
            return out(r, g, b, a, false);
        }
    }
}

template <class Img, bool Wide>
Img decode_impl(ByteView in, Info* info) {
    const auto chunks = read_chunks(in);
    const Header h = parse_ihdr(chunks);
    std::vector<Color> palette;
    std::vector<u8> palette_alpha;
    bool has_trns = false;
    u16 trns[3] = {0, 0, 0};
    Bytes idat;
    Info meta;
    meta.width = h.w;
    meta.height = h.h;
    meta.bit_depth = h.depth;
    meta.color_type = static_cast<ColorType>(h.ctype);
    meta.interlaced = h.interlace != 0;
    bool seen_idat = false, idat_done = false;
    for (usize ci = 1; ci < chunks.size(); ++ci) {
        const auto& c = chunks[ci];
        const u8* d = reinterpret_cast<const u8*>(c.data.data());
        if (c.type == "PLTE") {
            if (c.data.size() % 3 || c.data.empty() || c.data.size() / 3 > (usize(1) << h.depth)) bad("invalid PLTE");
            if (h.ctype == 0 || h.ctype == 4) bad("PLTE in a grayscale image");
            palette.clear();
            for (usize i = 0; i < c.data.size(); i += 3) palette.push_back({d[i], d[i + 1], d[i + 2], 255});
        } else if (c.type == "tRNS") {
            if (h.ctype == 3) {
                palette_alpha.assign(d, d + c.data.size());
                if (palette_alpha.size() > 256) bad("invalid tRNS");
                has_trns = true;
            } else if (h.ctype == 0 && c.data.size() == 2) {
                trns[0] = static_cast<u16>((d[0] << 8) | d[1]);
                has_trns = true;
            } else if (h.ctype == 2 && c.data.size() == 6) {
                for (int i = 0; i < 3; ++i) trns[i] = static_cast<u16>((d[2 * i] << 8) | d[2 * i + 1]);
                has_trns = true;
            }
        } else if (c.type == "IDAT") {
            if (idat_done) bad("IDAT chunks are not contiguous");
            seen_idat = true;
            idat.insert(idat.end(), c.data.begin(), c.data.end());
        } else {
            if (seen_idat) idat_done = true;
            if (c.type == "gAMA" && c.data.size() == 4) meta.gamma = be32(d) / 100000.0;
            else if (c.type == "tEXt") {
                const auto z = std::find(c.data.begin(), c.data.end(), std::byte{0});
                if (z != c.data.end()) meta.text[std::string(reinterpret_cast<const char*>(d), static_cast<usize>(z - c.data.begin()))] = std::string(reinterpret_cast<const char*>(&*(z + 1)), static_cast<usize>(c.data.end() - z - 1));
            } else if (c.type == "zTXt" && c.data.size() > 2) {
                const auto z = std::find(c.data.begin(), c.data.end(), std::byte{0});
                if (z != c.data.end() && z + 2 <= c.data.end()) {
                    try {
                        const auto txt = data::zlib_decompress(ByteView(&*(z + 2), static_cast<usize>(c.data.end() - z - 2)), 1 << 20);
                        meta.text[std::string(reinterpret_cast<const char*>(d), static_cast<usize>(z - c.data.begin()))] = std::string(reinterpret_cast<const char*>(txt.data()), txt.size());
                    } catch (const DataException&) {}
                }
            }
        }
    }
    if (!seen_idat) bad("no image data");
    if (h.ctype == 3 && palette.empty()) bad("missing PLTE for a palette image");
    meta.has_alpha = h.ctype == 4 || h.ctype == 6 || has_trns;

    // expected size of the uncompressed stream
    usize expected = 0;
    if (!h.interlace) {
        expected = (h.row_bytes(h.w) + 1) * h.h;
    } else {
        for (const auto& p : adam7) {
            const u32 pw = (h.w + p.dx - 1 - p.x0) / p.dx, ph = (h.h + p.dy - 1 - p.y0) / p.dy;
            if (h.w <= p.x0 || h.h <= p.y0 || pw == 0 || ph == 0) continue;
            expected += (h.row_bytes(pw) + 1) * ph;
        }
    }
    Bytes raw = data::zlib_decompress(idat, expected + 1024);
    if (raw.size() < expected) bad("image data is truncated");
    const usize bpp = std::max<usize>(1, h.bits_per_pixel() / 8);
    Img img(h.w, h.h);
    auto* base = reinterpret_cast<u8*>(raw.data());
    usize off = 0;
    auto do_pass = [&](u32 pw, u32 ph, u32 x0, u32 y0, u32 dx, u32 dy) {
        const usize rb = h.row_bytes(pw);
        const u8* prev = nullptr;
        for (u32 y = 0; y < ph; ++y) {
            u8* row = base + off + 1;
            unfilter_row(base[off], row, prev, rb, bpp);
            for (u32 x = 0; x < pw; ++x) img(x0 + x * dx, y0 + y * dy) = make_pixel<typename Img::pixel_type, Wide>(h, row, x, palette, palette_alpha, has_trns, trns);
            prev = row;
            off += rb + 1;
        }
    };
    if (!h.interlace) {
        do_pass(h.w, h.h, 0, 0, 1, 1);
    } else {
        for (const auto& p : adam7) {
            if (h.w <= p.x0 || h.h <= p.y0) continue;
            const u32 pw = (h.w - p.x0 + p.dx - 1) / p.dx, ph = (h.h - p.y0 + p.dy - 1) / p.dy;
            do_pass(pw, ph, p.x0, p.y0, p.dx, p.dy);
        }
    }
    if (info) *info = std::move(meta);
    return img;
}

inline void put32(Bytes& o, u32 v) { for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<std::byte>(v >> s)); }
inline void write_chunk(Bytes& out, const char* type, ByteView body) {
    put32(out, static_cast<u32>(body.size()));
    const usize start = out.size();
    out.insert(out.end(), reinterpret_cast<const std::byte*>(type), reinterpret_cast<const std::byte*>(type) + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, data::crc32(ByteView(out.data() + start, out.size() - start)));
}

/// Packs pixels into filtered, optionally interlaced scanlines and compresses them.
template <class GetSample>
Bytes build_idat(u32 w, u32 h, unsigned channels, unsigned depth, bool interlace, bool adaptive, int level, GetSample&& get) {
    const unsigned bits = channels * depth;
    const usize bpp = std::max<usize>(1, bits / 8);
    Bytes raw;
    auto emit_pass = [&](u32 pw, u32 ph, u32 x0, u32 y0, u32 dx, u32 dy) {
        const usize rb = (usize(pw) * bits + 7) / 8;
        std::vector<u8> cur(rb), prev(rb, 0), best(rb), tmp(rb);
        for (u32 y = 0; y < ph; ++y) {
            std::fill(cur.begin(), cur.end(), 0);
            for (u32 x = 0; x < pw; ++x)
                for (unsigned c = 0; c < channels; ++c) {
                    const u32 v = get(x0 + x * dx, y0 + y * dy, c);
                    if (depth == 16) {
                        cur[(usize(x) * channels + c) * 2] = static_cast<u8>(v >> 8);
                        cur[(usize(x) * channels + c) * 2 + 1] = static_cast<u8>(v);
                    } else if (depth == 8) {
                        cur[usize(x) * channels + c] = static_cast<u8>(v);
                    } else {
                        const usize bit = (usize(x) * channels + c) * depth;
                        cur[bit >> 3] = static_cast<u8>(cur[bit >> 3] | (v << (8 - depth - (bit & 7))));
                    }
                }
            u8 bestf = 0;
            if (!adaptive || depth < 8) {
                best = cur;
            } else {
                u64 best_score = ~u64(0);
                for (u8 f = 0; f < 5; ++f) {
                    for (usize i = 0; i < rb; ++i) {
                        const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                        int pred = 0;
                        switch (f) {
                            case 1: pred = a; break;
                            case 2: pred = b; break;
                            case 3: pred = (a + b) >> 1; break;
                            case 4: { const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c); pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
                            default: break;
                        }
                        tmp[i] = static_cast<u8>(cur[i] - pred);
                    }
                    u64 score = 0;
                    for (u8 t : tmp) score += static_cast<u64>(std::abs(static_cast<int>(static_cast<signed char>(t))));
                    if (score < best_score) { best_score = score; bestf = f; best = tmp; }
                }
            }
            raw.push_back(static_cast<std::byte>(bestf));
            raw.insert(raw.end(), reinterpret_cast<std::byte*>(best.data()), reinterpret_cast<std::byte*>(best.data()) + rb);
            prev = cur;
        }
    };
    if (!interlace) {
        emit_pass(w, h, 0, 0, 1, 1);
    } else {
        for (const auto& p : adam7) {
            if (w <= p.x0 || h <= p.y0) continue;
            emit_pass((w - p.x0 + p.dx - 1) / p.dx, (h - p.y0 + p.dy - 1) / p.dy, p.x0, p.y0, p.dx, p.dy);
        }
    }
    return data::zlib_compress(raw, level);
}

inline void write_ihdr(Bytes& out, u32 w, u32 h, u8 depth, u8 ctype, bool interlace) {
    Bytes ih;
    put32(ih, w);
    put32(ih, h);
    ih.push_back(static_cast<std::byte>(depth));
    ih.push_back(static_cast<std::byte>(ctype));
    ih.push_back(std::byte{0});
    ih.push_back(std::byte{0});
    ih.push_back(static_cast<std::byte>(interlace ? 1 : 0));
    write_chunk(out, "IHDR", ih);
}
inline void write_text(Bytes& out, const std::map<std::string, std::string>& text) {
    for (const auto& [k, v] : text) {
        if (k.empty() || k.size() > 79) bad("invalid tEXt keyword");
        Bytes b;
        for (char c : k) b.push_back(static_cast<std::byte>(c));
        b.push_back(std::byte{0});
        for (char c : v) b.push_back(static_cast<std::byte>(c));
        write_chunk(out, "tEXt", b);
    }
}

}  // namespace detail

/// Reads the header and metadata without decoding pixels.
[[nodiscard]] inline Info read_info(ByteView in) {
    Info info;
    (void)detail::decode_impl<Image, false>(in, &info);
    return info;
}

/// Decodes any PNG to 8-bit RGBA (16-bit samples are rounded down to 8 bits).
[[nodiscard]] inline Image decode(ByteView in, Info* info = nullptr) { return detail::decode_impl<Image, false>(in, info); }
/// Decodes to 16-bit RGBA, keeping full precision of 16-bit files.
[[nodiscard]] inline Image16 decode16(ByteView in, Info* info = nullptr) { return detail::decode_impl<Image16, true>(in, info); }

[[nodiscard]] inline Bytes encode(const Image& img, const EncodeOptions& opt = {}) {
    using namespace detail;
    if (img.empty()) bad("cannot encode an empty image");
    if (img.width() > 0x7fffffffu || img.height() > 0x7fffffffu) bad("image too large");
    if (opt.level < 0 || opt.level > 9) bad("invalid compression level");
    const u32 w = static_cast<u32>(img.width()), h = static_cast<u32>(img.height());
    bool opaque = true, gray = true;
    std::unordered_map<u32, u8> pal_index;
    std::vector<Color> pal;
    bool pal_ok = true;
    for (const Color& c : img.pixels()) {
        opaque &= c.a == 255;
        gray &= c.r == c.g && c.g == c.b;
        if (pal_ok && !pal_index.contains(c.to_rgba())) {
            if (pal.size() == 256) pal_ok = false;
            else { pal_index[c.to_rgba()] = static_cast<u8>(pal.size()); pal.push_back(c); }
        }
    }
    Bytes out(signature.size());
    std::memcpy(out.data(), signature.data(), 8);
    if (opt.optimize && pal_ok) {
        const unsigned depth = pal.size() <= 2 ? 1 : pal.size() <= 4 ? 2 : pal.size() <= 16 ? 4 : 8;
        write_ihdr(out, w, h, static_cast<u8>(depth), 3, opt.interlace);
        Bytes plte, trns;
        bool any_alpha = false;
        for (const Color& c : pal) {
            plte.push_back(static_cast<std::byte>(c.r)), plte.push_back(static_cast<std::byte>(c.g)), plte.push_back(static_cast<std::byte>(c.b));
            trns.push_back(static_cast<std::byte>(c.a));
            any_alpha |= c.a != 255;
        }
        write_chunk(out, "PLTE", plte);
        if (any_alpha) {
            while (!trns.empty() && trns.back() == std::byte{255}) trns.pop_back();
            write_chunk(out, "tRNS", trns);
        }
        write_text(out, opt.text);
        const Bytes z = build_idat(w, h, 1, depth, opt.interlace, opt.adaptive_filter, opt.level, [&](u32 x, u32 y, unsigned) -> u32 { return pal_index.at(img(x, y).to_rgba()); });
        write_chunk(out, "IDAT", z);
    } else if (opt.optimize && gray) {
        const unsigned ch = opaque ? 1 : 2;
        write_ihdr(out, w, h, 8, opaque ? 0 : 4, opt.interlace);
        write_text(out, opt.text);
        write_chunk(out, "IDAT", build_idat(w, h, ch, 8, opt.interlace, opt.adaptive_filter, opt.level, [&](u32 x, u32 y, unsigned c) -> u32 { return c == 0 ? img(x, y).r : img(x, y).a; }));
    } else {
        const bool rgba = !(opt.optimize && opaque);
        write_ihdr(out, w, h, 8, rgba ? 6 : 2, opt.interlace);
        write_text(out, opt.text);
        write_chunk(out, "IDAT", build_idat(w, h, rgba ? 4 : 3, 8, opt.interlace, opt.adaptive_filter, opt.level, [&](u32 x, u32 y, unsigned c) -> u32 {
            const Color& p = img(x, y);
            return c == 0 ? p.r : c == 1 ? p.g : c == 2 ? p.b : p.a;
        }));
    }
    write_chunk(out, "IEND", {});
    return out;
}

/// 16-bit encoder (gray / gray+alpha / RGB / RGBA chosen automatically).
[[nodiscard]] inline Bytes encode16(const Image16& img, const EncodeOptions& opt = {}) {
    using namespace detail;
    if (img.empty()) bad("cannot encode an empty image");
    const u32 w = static_cast<u32>(img.width()), h = static_cast<u32>(img.height());
    bool opaque = true, gray = true;
    for (const Color16& c : img.pixels()) {
        opaque &= c.a == 65535;
        gray &= c.r == c.g && c.g == c.b;
    }
    Bytes out(signature.size());
    std::memcpy(out.data(), signature.data(), 8);
    const unsigned ch = gray ? (opaque ? 1 : 2) : (opaque ? 3 : 4);
    const u8 ctype = gray ? (opaque ? 0 : 4) : (opaque ? 2 : 6);
    write_ihdr(out, w, h, 16, ctype, opt.interlace);
    write_text(out, opt.text);
    write_chunk(out, "IDAT", build_idat(w, h, ch, 16, opt.interlace, opt.adaptive_filter, opt.level, [&](u32 x, u32 y, unsigned c) -> u32 {
        const Color16& p = img(x, y);
        if (gray) return c == 0 ? p.r : p.a;
        return c == 0 ? p.r : c == 1 ? p.g : c == 2 ? p.b : p.a;
    }));
    write_chunk(out, "IEND", {});
    return out;
}

}  // namespace flyg::graphics::png
