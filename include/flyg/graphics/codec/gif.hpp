// flyG - graphics module: GIF87a/89a codec (LZW, interlace, transparency, animation with disposal methods)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::gif {

constexpr unsigned le16u(unsigned lo, unsigned hi) noexcept { return lo | (hi << 8); }

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

struct AnimFrame {
    Image image;        ///< full-canvas composited frame (disposal already applied for the next one)
    u32 delay_ms = 100;
};
struct Animation {
    usize width = 0, height = 0;
    u32 loop_count = 1;  ///< 0 = infinite
    std::vector<AnimFrame> frames;
};

namespace detail {
[[noreturn]] inline void bad(const char* m) { throw DataException(std::string("gif: ") + m); }

inline std::vector<u8> lzw_decode(const u8* d, usize n, int min_code, usize expected) {
    if (min_code < 2 || min_code > 8) bad("invalid LZW code size");
    const int clear = 1 << min_code, eoi = clear + 1;
    std::array<u16, 4096> prefix{};
    std::array<u8, 4096> suffix{};
    std::array<u16, 4096> length{};
    for (int i = 0; i < clear; ++i) { suffix[static_cast<usize>(i)] = static_cast<u8>(i); length[static_cast<usize>(i)] = 1; }
    std::vector<u8> out;
    out.reserve(expected);
    int size = min_code + 1, next = eoi + 1, prev = -1;
    u32 acc = 0;
    int bits = 0;
    usize p = 0;
    std::vector<u8> tmp;
    while (out.size() < expected) {
        while (bits < size && p < n) { acc |= u32(d[p++]) << bits; bits += 8; }
        if (bits < size) break;  // data ended early: caller pads
        const int code = static_cast<int>(acc & ((1u << size) - 1));
        acc >>= size;
        bits -= size;
        if (code == clear) { size = min_code + 1; next = eoi + 1; prev = -1; continue; }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) bad("corrupt LZW stream");
            out.push_back(static_cast<u8>(code));
            prev = code;
            continue;
        }
        int cur = code;
        u8 first;
        if (code < next) {
            const usize len = length[static_cast<usize>(code)];
            const usize base = out.size();
            out.resize(base + len);
            int c = code;
            for (usize i = len; i-- > 0;) { out[base + i] = suffix[static_cast<usize>(c)]; c = prefix[static_cast<usize>(c)]; }
            first = out[base];
        } else if (code == next && next < 4096) {
            const usize len = length[static_cast<usize>(prev)];
            const usize base = out.size();
            out.resize(base + len + 1);
            int c = prev;
            for (usize i = len; i-- > 0;) { out[base + i] = suffix[static_cast<usize>(c)]; c = prefix[static_cast<usize>(c)]; }
            first = out[base];
            out[base + len] = first;
        } else {
            bad("corrupt LZW stream");
        }
        (void)cur;
        if (next < 4096) {
            prefix[static_cast<usize>(next)] = static_cast<u16>(prev);
            suffix[static_cast<usize>(next)] = first;
            length[static_cast<usize>(next)] = static_cast<u16>(length[static_cast<usize>(prev)] + 1);
            ++next;
            if (next == (1 << size) && size < 12) ++size;
        }
        prev = code;
    }
    return out;
}

inline Bytes lzw_encode(const std::vector<u8>& px, int min_code) {
    Bytes out;
    u32 acc = 0;
    int bits = 0;
    auto put = [&](u32 code, int size) {
        acc |= code << bits;
        bits += size;
        while (bits >= 8) { out.push_back(static_cast<std::byte>(acc & 255)); acc >>= 8; bits -= 8; }
    };
    const int clear = 1 << min_code, eoi = clear + 1;
    int size = min_code + 1, next = eoi + 1;
    std::unordered_map<u32, u16> dict;
    put(static_cast<u32>(clear), size);
    if (px.empty()) { put(static_cast<u32>(eoi), size); if (bits) out.push_back(static_cast<std::byte>(acc)); return out; }
    u32 cur = px[0];
    for (usize i = 1; i < px.size(); ++i) {
        const u32 key = (cur << 8) | px[i];
        auto it = dict.find(key);
        if (it != dict.end()) { cur = it->second; continue; }
        put(cur, size);
        if (next < 4096) {
            dict[key] = static_cast<u16>(next++);
            if (next > (1 << size) && size < 12) ++size;
        } else {
            put(static_cast<u32>(clear), size);
            dict.clear();
            size = min_code + 1;
            next = eoi + 1;
        }
        cur = px[i];
    }
    put(cur, size);
    put(static_cast<u32>(eoi), size);
    if (bits) out.push_back(static_cast<std::byte>(acc));
    return out;
}
}  // namespace detail

[[nodiscard]] inline Animation decode_animation(ByteView in) {
    using namespace detail;
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    if (n < 13 || std::memcmp(d, "GIF8", 4) != 0 || (d[4] != '7' && d[4] != '9') || d[5] != 'a') bad("not a GIF file");
    Animation anim;
    anim.width = le16u(d[6], d[7]);
    anim.height = le16u(d[8], d[9]);
    if (!anim.width || !anim.height) bad("invalid dimensions");
    usize p = 13;
    std::vector<Color> gct;
    const u8 flags = d[10];
    const u8 bg_index = d[11];
    if (flags & 0x80) {
        const usize cnt = usize(2) << (flags & 7);
        if (p + 3 * cnt > n) bad("truncated colour table");
        for (usize i = 0; i < cnt; ++i) gct.push_back({d[p + 3 * i], d[p + 3 * i + 1], d[p + 3 * i + 2], 255});
        p += 3 * cnt;
    }
    (void)bg_index;
    Image canvas(anim.width, anim.height, Color{0, 0, 0, 0});
    int transp = -1;
    u32 delay = 100;
    int disposal = 0;
    Image restore;
    auto skip_sub = [&](usize q) {
        while (true) {
            if (q >= n) bad("truncated data");
            const usize l = d[q++];
            if (!l) return q;
            q += l;
        }
    };
    bool done = false;
    while (!done && p < n) {
        const u8 b = d[p++];
        if (b == 0x3b) break;
        if (b == 0x21) {
            if (p >= n) bad("truncated extension");
            const u8 label = d[p++];
            if (label == 0xf9 && p + 5 < n && d[p] == 4) {
                disposal = (d[p + 1] >> 2) & 7;
                delay = (le16u(d[p + 2], d[p + 3])) * 10u;
                transp = (d[p + 1] & 1) ? d[p + 4] : -1;
            } else if (label == 0xff && p + 12 < n && d[p] == 11 && std::memcmp(d + p + 1, "NETSCAPE2.0", 11) == 0 && d[p + 12] == 3 && p + 15 < n && d[p + 13] == 1) {
                anim.loop_count = le16u(d[p + 14], d[p + 15]);
            }
            p = skip_sub(p);
            continue;
        }
        if (b != 0x2c) bad("unexpected block");
        if (p + 9 > n) bad("truncated image descriptor");
        const usize fx = le16u(d[p], d[p + 1]), fy = le16u(d[p + 2], d[p + 3]), fw = le16u(d[p + 4], d[p + 5]), fh = le16u(d[p + 6], d[p + 7]);
        const u8 lf = d[p + 8];
        p += 9;
        std::vector<Color> lct;
        if (lf & 0x80) {
            const usize cnt = usize(2) << (lf & 7);
            if (p + 3 * cnt > n) bad("truncated colour table");
            for (usize i = 0; i < cnt; ++i) lct.push_back({d[p + 3 * i], d[p + 3 * i + 1], d[p + 3 * i + 2], 255});
            p += 3 * cnt;
        }
        const auto& pal = lf & 0x80 ? lct : gct;
        if (p >= n) bad("truncated image data");
        const int min_code = d[p++];
        std::vector<u8> data;
        while (true) {
            if (p >= n) bad("truncated image data");
            const usize l = d[p++];
            if (!l) break;
            if (p + l > n) bad("truncated image data");
            data.insert(data.end(), d + p, d + p + l);
            p += l;
        }
        if (fw == 0 || fh == 0) continue;
        if (fx + fw > anim.width || fy + fh > anim.height) bad("frame exceeds canvas");
        std::vector<u8> idx = lzw_decode(data.data(), data.size(), min_code, fw * fh);
        idx.resize(fw * fh, 0);
        // disposal 3 needs the canvas before drawing
        if (disposal == 3) restore = canvas;
        const bool interlaced = lf & 0x40;
        std::vector<usize> rows(fh);
        if (interlaced) {
            usize r = 0;
            for (auto [start, step] : {std::pair<usize, usize>{0, 8}, {4, 8}, {2, 4}, {1, 2}})
                for (usize y = start; y < fh; y += step) rows[y] = r++;
        } else {
            for (usize y = 0; y < fh; ++y) rows[y] = y;
        }
        for (usize y = 0; y < fh; ++y)
            for (usize x = 0; x < fw; ++x) {
                const u8 ci = idx[rows[y] * fw + x];
                if (static_cast<int>(ci) == transp) continue;
                if (ci >= pal.size()) bad("colour index out of range");
                canvas(fx + x, fy + y) = pal[ci];
            }
        anim.frames.push_back({canvas, delay});
        switch (disposal) {
            case 2:
                for (usize y = 0; y < fh; ++y)
                    for (usize x = 0; x < fw; ++x) canvas(fx + x, fy + y) = Color{0, 0, 0, 0};
                break;
            case 3: canvas = restore; break;
            default: break;
        }
        disposal = 0;
        transp = -1;
        delay = 100;
        if (anim.frames.size() > 100000) bad("too many frames");
    }
    if (anim.frames.empty()) bad("no image data");
    return anim;
}

/// First frame of the GIF (transparent pixels keep alpha 0).
[[nodiscard]] inline Image decode(ByteView in) { return std::move(decode_animation(in).frames.front().image); }

namespace detail {
struct Quantized {
    std::vector<Color> palette;   // 2..256 entries
    std::vector<u8> index;
    int transparent = -1;
};

/// Exact palette when the image has <=256 colours, otherwise median-cut.
inline Quantized quantize(const Image& img, bool dither) {
    Quantized q;
    const usize N = img.pixel_count();
    std::vector<Color> uniq;
    std::unordered_map<u32, u8> lookup;
    bool has_t = false, overflow = false;
    for (const Color& c : img.pixels()) {
        if (c.a < 128) { has_t = true; continue; }
        const u32 key = (u32(c.r) << 16) | (u32(c.g) << 8) | c.b;
        if (!lookup.count(key)) {
            if (uniq.size() >= (has_t ? 255u : 256u)) { overflow = true; break; }
            lookup[key] = static_cast<u8>(uniq.size());
            uniq.push_back({c.r, c.g, c.b, 255});
        }
    }
    if (!overflow) {  // transparent seen later may need a slot
        if (has_t && uniq.size() > 255) overflow = true;
    }
    const usize slots = 256 - (has_t ? 1 : 0);
    if (overflow) {
        struct Box { std::vector<u32> px; };
        std::unordered_map<u32, u32> hist;
        for (const Color& c : img.pixels())
            if (c.a >= 128) ++hist[(u32(c.r) << 16) | (u32(c.g) << 8) | c.b];
        std::vector<std::pair<u32, u32>> items(hist.begin(), hist.end());
        std::vector<std::vector<std::pair<u32, u32>>> boxes{std::move(items)};
        auto chan = [](u32 k, int c) { return static_cast<int>((k >> (16 - 8 * c)) & 255); };
        while (boxes.size() < slots) {
            usize best = boxes.size();
            int best_range = 0, best_ch = 0;
            for (usize i = 0; i < boxes.size(); ++i) {
                if (boxes[i].size() < 2) continue;
                for (int c = 0; c < 3; ++c) {
                    int lo = 255, hi = 0;
                    for (auto& [k, cnt] : boxes[i]) { lo = std::min(lo, chan(k, c)); hi = std::max(hi, chan(k, c)); }
                    if (hi - lo > best_range) { best_range = hi - lo; best = i; best_ch = c; }
                }
            }
            if (best == boxes.size()) break;
            auto& b = boxes[best];
            std::sort(b.begin(), b.end(), [&](auto& x, auto& y) { return chan(x.first, best_ch) < chan(y.first, best_ch); });
            u64 total = 0;
            for (auto& e : b) total += e.second;
            u64 acc = 0;
            usize cut = 1;
            for (usize i = 0; i < b.size(); ++i) { acc += b[i].second; if (acc * 2 >= total) { cut = std::clamp<usize>(i + 1, 1, b.size() - 1); break; } }
            std::vector<std::pair<u32, u32>> hi(b.begin() + static_cast<std::ptrdiff_t>(cut), b.end());
            b.resize(cut);
            boxes.push_back(std::move(hi));
        }
        uniq.clear();
        for (auto& b : boxes) {
            u64 r = 0, g = 0, bl = 0, t = 0;
            for (auto& [k, cnt] : b) { r += u64(chan(k, 0)) * cnt; g += u64(chan(k, 1)) * cnt; bl += u64(chan(k, 2)) * cnt; t += cnt; }
            if (t) uniq.push_back({static_cast<u8>((r + t / 2) / t), static_cast<u8>((g + t / 2) / t), static_cast<u8>((bl + t / 2) / t), 255});
        }
    }
    if (uniq.empty()) uniq.push_back({0, 0, 0, 255});
    q.palette = uniq;
    if (has_t) { q.transparent = static_cast<int>(q.palette.size()); q.palette.push_back({0, 0, 0, 0}); }
    const usize opaque_n = uniq.size();
    q.index.resize(N);
    std::unordered_map<u32, u8> cache;
    auto nearest = [&](int r, int g, int b) -> u8 {
        const u32 key = (u32(std::clamp(r, 0, 255)) << 16) | (u32(std::clamp(g, 0, 255)) << 8) | u32(std::clamp(b, 0, 255));
        auto it = cache.find(key);
        if (it != cache.end() && !dither) return it->second;
        int bd = 1 << 30;
        u8 bi = 0;
        for (usize i = 0; i < opaque_n; ++i) {
            const int dr = r - uniq[i].r, dg = g - uniq[i].g, db = b - uniq[i].b;
            const int dd = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (dd < bd) { bd = dd; bi = static_cast<u8>(i); }
        }
        cache[key] = bi;
        return bi;
    };
    const usize W = img.width(), H = img.height();
    if (!dither || !overflow) {
        for (usize i = 0; i < N; ++i) {
            const Color c = img.data()[i];
            q.index[i] = c.a < 128 ? static_cast<u8>(q.transparent) : nearest(c.r, c.g, c.b);
        }
    } else {  // Floyd-Steinberg
        std::vector<int> err((W + 2) * 2 * 3, 0);
        for (usize y = 0; y < H; ++y) {
            std::vector<int> nxt((W + 2) * 3, 0);
            for (usize x = 0; x < W; ++x) {
                const Color c = img(x, y);
                if (c.a < 128) { q.index[y * W + x] = static_cast<u8>(q.transparent); continue; }
                const int e[3] = {err[(x + 1) * 3], err[(x + 1) * 3 + 1], err[(x + 1) * 3 + 2]};
                const int r = c.r + e[0] / 16, g = c.g + e[1] / 16, b = c.b + e[2] / 16;
                const u8 ci = nearest(r, g, b);
                q.index[y * W + x] = ci;
                const int dv[3] = {r - uniq[ci].r, g - uniq[ci].g, b - uniq[ci].b};
                for (int k = 0; k < 3; ++k) {
                    err[(x + 2) * 3 + static_cast<usize>(k)] += dv[k] * 7;
                    nxt[x * 3 + static_cast<usize>(k)] += dv[k] * 3;
                    nxt[(x + 1) * 3 + static_cast<usize>(k)] += dv[k] * 5;
                    nxt[(x + 2) * 3 + static_cast<usize>(k)] += dv[k];
                }
            }
            err.assign(nxt.begin(), nxt.end());
            err.resize((W + 2) * 3 * 2, 0);
        }
    }
    return q;
}
}  // namespace detail

struct EncodeOptions {
    bool dither = true;   ///< Floyd-Steinberg when more than 256 colours must be reduced
    bool interlace = false;
};

[[nodiscard]] inline Bytes encode_animation(const Animation& a, const EncodeOptions& opt = {}) {
    using namespace detail;
    if (a.frames.empty() || a.width == 0 || a.height == 0 || a.width > 65535 || a.height > 65535) bad("invalid animation");
    Bytes o;
    auto b8 = [&](u32 v) { o.push_back(static_cast<std::byte>(v)); };
    auto b16 = [&](u32 v) { b8(v & 255); b8(v >> 8); };
    for (char c : {'G', 'I', 'F', '8', '9', 'a'}) b8(static_cast<u8>(c));
    b16(static_cast<u32>(a.width)), b16(static_cast<u32>(a.height));
    b8(0x70), b8(0), b8(0);  // no global table
    if (a.frames.size() > 1) {
        b8(0x21), b8(0xff), b8(11);
        for (char c : std::string_view("NETSCAPE2.0")) b8(static_cast<u8>(c));
        b8(3), b8(1), b16(a.loop_count), b8(0);
    }
    for (const auto& f : a.frames) {
        if (f.image.width() != a.width || f.image.height() != a.height) bad("frame size differs from the canvas");
        Quantized q = quantize(f.image, opt.dither);
        int bits = 1;
        while ((usize(1) << bits) < q.palette.size()) ++bits;
        b8(0x21), b8(0xf9), b8(4), b8((q.transparent >= 0 ? 1u : 0u) | 2u << 2);  // disposal 2 for animations keeps frames independent
        b16(std::min<u32>((f.delay_ms + 5) / 10, 65535)), b8(q.transparent >= 0 ? static_cast<u32>(q.transparent) : 0u), b8(0);
        b8(0x2c), b16(0), b16(0), b16(static_cast<u32>(a.width)), b16(static_cast<u32>(a.height));
        b8(0x80 | (opt.interlace ? 0x40u : 0u) | static_cast<u32>(bits - 1));
        for (usize i = 0; i < (usize(1) << bits); ++i) {
            const Color c = i < q.palette.size() ? q.palette[i] : Color{};
            b8(c.r), b8(c.g), b8(c.b);
        }
        std::vector<u8> data;
        data.reserve(q.index.size());
        if (opt.interlace) {
            for (auto [start, step] : {std::pair<usize, usize>{0, 8}, {4, 8}, {2, 4}, {1, 2}})
                for (usize y = start; y < a.height; y += step) data.insert(data.end(), q.index.begin() + static_cast<std::ptrdiff_t>(y * a.width), q.index.begin() + static_cast<std::ptrdiff_t>((y + 1) * a.width));
        } else {
            data = q.index;
        }
        const int min_code = std::max(bits, 2);
        b8(static_cast<u32>(min_code));
        const Bytes lz = lzw_encode(data, min_code);
        for (usize p = 0; p < lz.size(); p += 255) {
            const usize l = std::min<usize>(255, lz.size() - p);
            b8(static_cast<u32>(l));
            o.insert(o.end(), lz.begin() + static_cast<std::ptrdiff_t>(p), lz.begin() + static_cast<std::ptrdiff_t>(p + l));
        }
        b8(0);
    }
    b8(0x3b);
    return o;
}

[[nodiscard]] inline Bytes encode(const Image& img, const EncodeOptions& opt = {}) {
    Animation a;
    a.width = img.width();
    a.height = img.height();
    a.frames.push_back({img, 0});
    return encode_animation(a, opt);
}

}  // namespace flyg::graphics::gif
