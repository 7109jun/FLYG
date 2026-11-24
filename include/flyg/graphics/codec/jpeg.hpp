// flyG - graphics module: JPEG codec
//   decode: baseline / extended-sequential / progressive Huffman, 8-bit, 1/3/4 components, any sampling factors,
//           restart intervals, JFIF / Adobe (RGB, YCbCr, CMYK, YCCK) colour handling.
//   encode: baseline Huffman, quality 1..100, 4:4:4 / 4:2:2 / 4:2:0 / grayscale.
//   Not supported (rejected with DataException): arithmetic coding, lossless, 12-bit and hierarchical modes.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <flyg/graphics/image.hpp>

namespace flyg::graphics::jpeg {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

namespace detail {

[[noreturn]] inline void bad(const char* m) { throw DataException(std::string("jpeg: ") + m); }

inline constexpr std::array<u8, 64> kZigzag = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                               41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                               30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// T[x][u] = c(u)/2 * cos((2x+1) u pi / 16); the 8x8 (I)DCT is T F T^T.
inline const std::array<std::array<float, 8>, 8>& dct_matrix() {
    static const auto m = [] {
        std::array<std::array<float, 8>, 8> t{};
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                t[static_cast<usize>(x)][static_cast<usize>(u)] =
                    static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        return t;
    }();
    return m;
}

// ----------------------------------------------------------------------------------------------- Huffman (decode)
struct HuffTable {
    bool defined = false;
    std::array<int, 18> maxcode{};
    std::array<int, 17> valptr{};
    std::array<int, 17> mincode{};
    std::array<u8, 256> vals{};
    u32 count = 0;
    void build(const u8* bits, const u8* v, u32 n) {
        count = n;
        std::memcpy(vals.data(), v, n);
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[static_cast<usize>(l)] = k;
            mincode[static_cast<usize>(l)] = code;
            code += bits[l - 1];
            k += bits[l - 1];
            maxcode[static_cast<usize>(l)] = bits[l - 1] ? code - 1 : -1;
            if (code > (1 << l)) bad("invalid Huffman table");
            code <<= 1;
        }
        defined = true;
    }
};

class BitReader {
public:
    BitReader(const u8* d, usize n, usize pos) : d_(d), n_(n), p_(pos) {}
    [[nodiscard]] usize pos() const { return p_; }
    int bit() {
        if (cnt_ == 0) fill();
        --cnt_;
        check();
        return static_cast<int>((buf_ >> cnt_) & 1);
    }
    int bits(int n) {
        if (n == 0) return 0;
        while (cnt_ < n) fill();
        cnt_ -= n;
        check();
        return static_cast<int>((buf_ >> cnt_) & ((1u << n) - 1));
    }
    int decode(const HuffTable& t) {
        if (!t.defined) bad("scan uses an undefined Huffman table");
        int code = 0;
        for (int l = 1; l <= 16; ++l) {
            code = (code << 1) | bit();
            if (t.maxcode[static_cast<usize>(l)] >= 0 && code <= t.maxcode[static_cast<usize>(l)] && code >= t.mincode[static_cast<usize>(l)]) {
                const int idx = t.valptr[static_cast<usize>(l)] + code - t.mincode[static_cast<usize>(l)];
                if (idx >= static_cast<int>(t.count)) bad("corrupt Huffman code");
                return t.vals[static_cast<usize>(idx)];
            }
        }
        bad("corrupt Huffman code");
    }
    static int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }
    /// Align to a byte boundary and consume the RSTn marker.
    void restart() {
        cnt_ = 0;
        buf_ = 0;
        fake_ = 0;
        marker_ = false;
        while (p_ + 1 < n_) {
            if (d_[p_] == 0xff && d_[p_ + 1] >= 0xd0 && d_[p_ + 1] <= 0xd7) { p_ += 2; return; }
            if (d_[p_] == 0xff && d_[p_ + 1] != 0 && d_[p_ + 1] != 0xff) break;  // some other marker: restart is missing
            ++p_;
        }
        bad("missing restart marker");
    }
    /// After the scan: position of the next real marker (0xFF followed by a marker code).
    usize next_marker() {
        while (p_ + 1 < n_) {
            if (d_[p_] == 0xff && d_[p_ + 1] != 0 && d_[p_ + 1] != 0xff && !(d_[p_ + 1] >= 0xd0 && d_[p_ + 1] <= 0xd7)) return p_;
            ++p_;
        }
        return n_;
    }

private:
    void fill() {
        while (cnt_ <= 24) {
            u32 b = 0;
            if (!marker_ && p_ < n_) {
                b = d_[p_++];
                if (b == 0xff) {
                    if (p_ < n_ && d_[p_] == 0) ++p_;
                    else { marker_ = true; --p_; b = 0; }
                }
            }
            if (marker_ || p_ >= n_) fake_ += 8;
            buf_ = (buf_ << 8) | b;
            cnt_ += 8;
        }
    }
    void check() const {
        if (fake_ > cnt_ && fake_ - cnt_ > 64) bad("entropy-coded data is truncated");
    }
    const u8* d_;
    usize n_, p_;
    u32 buf_ = 0;
    int cnt_ = 0;
    int fake_ = 0;
    bool marker_ = false;
};

struct Component {
    int id = 0, h = 1, v = 1, tq = 0;
    usize bw = 0, bh = 0;          // blocks, padded to whole MCUs
    usize cw = 0, ch = 0;          // real sample dimensions
    usize nbw = 0, nbh = 0;        // blocks covering the real samples (non-interleaved scans)
    std::vector<i16> coef;         // bw*bh*64, natural order (not dequantised)
    int pred = 0;
    int dc_tbl = 0, ac_tbl = 0;
};

struct Decoder {
    const u8* d;
    usize n;
    usize W = 0, H = 0;
    bool progressive = false;
    int adobe = -1;
    bool jfif = false;
    u32 restart_interval = 0;
    std::array<std::array<u16, 64>, 4> qt{};
    std::array<bool, 4> qt_ok{};
    std::array<HuffTable, 4> dc{}, ac{};
    std::vector<Component> comps;
    int hmax = 1, vmax = 1;
    usize mcux = 0, mcuy = 0;
    bool have_sof = false, any_scan = false;

    Decoder(const u8* data, usize size) : d(data), n(size) {}

    static u32 r16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }

    void parse_sof(usize p, usize len, int type) {
        if (have_sof) bad("multiple frames");
        if (len < 8) bad("bad SOF");
        if (d[p] != 8) bad("only 8-bit precision is supported");
        H = r16(d + p + 1);
        W = r16(d + p + 3);
        const usize nc = d[p + 5];
        if (W == 0 || H == 0) bad("invalid dimensions");
        if (W * H > (usize(1) << 27)) bad("image is too large");
        if (nc != 1 && nc != 3 && nc != 4) bad("unsupported component count");
        if (len < 6 + 3 * nc) bad("bad SOF");
        progressive = type == 2;
        for (usize i = 0; i < nc; ++i) {
            Component c;
            c.id = d[p + 6 + 3 * i];
            c.h = d[p + 7 + 3 * i] >> 4;
            c.v = d[p + 7 + 3 * i] & 15;
            c.tq = d[p + 8 + 3 * i];
            if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.tq > 3) bad("bad component parameters");
            hmax = std::max(hmax, c.h);
            vmax = std::max(vmax, c.v);
            comps.push_back(std::move(c));
        }
        mcux = (W + 8 * static_cast<usize>(hmax) - 1) / (8 * static_cast<usize>(hmax));
        mcuy = (H + 8 * static_cast<usize>(vmax) - 1) / (8 * static_cast<usize>(vmax));
        for (auto& c : comps) {
            c.bw = mcux * static_cast<usize>(c.h);
            c.bh = mcuy * static_cast<usize>(c.v);
            c.cw = (W * static_cast<usize>(c.h) + static_cast<usize>(hmax) - 1) / static_cast<usize>(hmax);
            c.ch = (H * static_cast<usize>(c.v) + static_cast<usize>(vmax) - 1) / static_cast<usize>(vmax);
            c.nbw = (c.cw + 7) / 8;
            c.nbh = (c.ch + 7) / 8;
            c.coef.assign(c.bw * c.bh * 64, 0);
        }
        have_sof = true;
    }

    void parse_dht(usize p, usize len) {
        const usize end = p + len;
        while (p < end) {
            if (p + 17 > end) bad("bad DHT");
            const int tc = d[p] >> 4, th = d[p] & 15;
            if (tc > 1 || th > 3) bad("bad DHT");
            u32 total = 0;
            for (int i = 0; i < 16; ++i) total += d[p + 1 + static_cast<usize>(i)];
            if (total > 256 || p + 17 + total > end) bad("bad DHT");
            (tc == 0 ? dc : ac)[static_cast<usize>(th)].build(d + p + 1, d + p + 17, total);
            p += 17 + total;
        }
    }

    void parse_dqt(usize p, usize len) {
        const usize end = p + len;
        while (p < end) {
            const int pq = d[p] >> 4, tq = d[p] & 15;
            if (pq > 1 || tq > 3) bad("bad DQT");
            ++p;
            const usize sz = pq ? 128 : 64;
            if (p + sz > end) bad("bad DQT");
            for (usize i = 0; i < 64; ++i) qt[static_cast<usize>(tq)][kZigzag[i]] = static_cast<u16>(pq ? r16(d + p + 2 * i) : d[p + i]);
            qt_ok[static_cast<usize>(tq)] = true;
            p += sz;
        }
    }

    // ------------------------------------------------------------------ scans
    usize scan(usize p, usize len) {
        if (!have_sof) bad("scan before frame header");
        if (len < 6) bad("bad SOS");
        const usize ns = d[p];
        if (ns < 1 || ns > 4 || len != 4 + 2 * ns) bad("bad SOS");
        std::vector<Component*> sc;
        for (usize i = 0; i < ns; ++i) {
            const int cid = d[p + 1 + 2 * i];
            Component* found = nullptr;
            for (auto& c : comps)
                if (c.id == cid) found = &c;
            if (!found) bad("scan references an unknown component");
            if (std::find(sc.begin(), sc.end(), found) != sc.end()) bad("duplicate component in scan");
            found->dc_tbl = d[p + 2 + 2 * i] >> 4;
            found->ac_tbl = d[p + 2 + 2 * i] & 15;
            if (found->dc_tbl > 3 || found->ac_tbl > 3) bad("bad table selector");
            sc.push_back(found);
        }
        const int ss = d[p + 1 + 2 * ns], se = d[p + 2 + 2 * ns], ah = d[p + 3 + 2 * ns] >> 4, al = d[p + 3 + 2 * ns] & 15;
        if (progressive) {
            if (ss > se || se > 63 || al > 13 || (ss == 0 && se != 0) || (ss > 0 && ns != 1)) bad("bad progressive scan parameters");
        }
        BitReader br(d, n, p + len);
        int eobrun = 0;
        for (auto* c : sc) c->pred = 0;

        auto decode_block = [&](Component& c, i16* blk) {
            if (!progressive) {
                const int t = br.decode(dc[static_cast<usize>(c.dc_tbl)]);
                if (t > 11) bad("corrupt DC coefficient");
                const int diff = t ? BitReader::extend(br.bits(t), t) : 0;
                c.pred += diff;
                blk[0] = static_cast<i16>(c.pred);
                for (int k = 1; k < 64;) {
                    const int rs = br.decode(ac[static_cast<usize>(c.ac_tbl)]);
                    const int r = rs >> 4, s = rs & 15;
                    if (s == 0) {
                        if (r == 15) { k += 16; continue; }
                        break;
                    }
                    k += r;
                    if (k > 63) bad("corrupt AC coefficient");
                    blk[kZigzag[static_cast<usize>(k)]] = static_cast<i16>(BitReader::extend(br.bits(s), s));
                    ++k;
                }
                return;
            }
            if (ss == 0) {  // DC scans
                if (ah == 0) {
                    const int t = br.decode(dc[static_cast<usize>(c.dc_tbl)]);
                    if (t > 11) bad("corrupt DC coefficient");
                    const int diff = t ? BitReader::extend(br.bits(t), t) : 0;
                    c.pred += diff;
                    blk[0] = static_cast<i16>(c.pred * (1 << al));
                } else if (br.bit()) {
                    blk[0] = static_cast<i16>(blk[0] | (1 << al));
                }
                return;
            }
            if (ah == 0) {  // AC first
                if (eobrun > 0) { --eobrun; return; }
                for (int k = ss; k <= se;) {
                    const int rs = br.decode(ac[static_cast<usize>(c.ac_tbl)]);
                    const int r = rs >> 4, s = rs & 15;
                    if (s == 0) {
                        if (r < 15) {
                            eobrun = (1 << r) - 1;
                            if (r) eobrun += br.bits(r);
                            break;
                        }
                        k += 16;
                        continue;
                    }
                    k += r;
                    if (k > 63) bad("corrupt AC coefficient");
                    blk[kZigzag[static_cast<usize>(k)]] = static_cast<i16>(BitReader::extend(br.bits(s), s) * (1 << al));
                    ++k;
                }
                return;
            }
            // AC refinement
            const int p1 = 1 << al, m1 = -(1 << al);
            int k = ss;
            if (eobrun == 0) {
                for (; k <= se; ++k) {
                    const int rs = br.decode(ac[static_cast<usize>(c.ac_tbl)]);
                    int r = rs >> 4, s = rs & 15;
                    if (s) {
                        if (s != 1) bad("corrupt AC refinement");
                        s = br.bit() ? p1 : m1;
                    } else if (r != 15) {
                        eobrun = 1 << r;
                        if (r) eobrun += br.bits(r);
                        break;
                    }
                    do {
                        i16& co = blk[kZigzag[static_cast<usize>(k)]];
                        if (co != 0) {
                            if (br.bit() && (co & p1) == 0) co = static_cast<i16>(co >= 0 ? co + p1 : co + m1);
                        } else if (--r < 0) {
                            break;
                        }
                        ++k;
                    } while (k <= se);
                    if (s) {
                        if (k > 63) bad("corrupt AC refinement");
                        blk[kZigzag[static_cast<usize>(k)]] = static_cast<i16>(s);
                    }
                }
            }
            if (eobrun > 0) {
                for (; k <= se; ++k) {
                    i16& co = blk[kZigzag[static_cast<usize>(k)]];
                    if (co != 0 && br.bit() && (co & p1) == 0) co = static_cast<i16>(co >= 0 ? co + p1 : co + m1);
                }
                --eobrun;
            }
        };

        const bool single = sc.size() == 1;
        const usize units_x = single ? sc[0]->nbw : mcux, units_y = single ? sc[0]->nbh : mcuy;
        u32 since = 0;
        for (usize uy = 0; uy < units_y; ++uy)
            for (usize ux = 0; ux < units_x; ++ux) {
                if (restart_interval && since == restart_interval) {
                    br.restart();
                    eobrun = 0;
                    for (auto* c : sc) c->pred = 0;
                    since = 0;
                }
                ++since;
                if (single) {
                    Component& c = *sc[0];
                    decode_block(c, &c.coef[(uy * c.bw + ux) * 64]);
                } else {
                    for (auto* c : sc)
                        for (usize by = 0; by < static_cast<usize>(c->v); ++by)
                            for (usize bx = 0; bx < static_cast<usize>(c->h); ++bx)
                                decode_block(*c, &c->coef[((uy * static_cast<usize>(c->v) + by) * c->bw + ux * static_cast<usize>(c->h) + bx) * 64]);
                }
            }
        any_scan = true;
        return br.next_marker();
    }

    // ------------------------------------------------------------------ reconstruction
    std::vector<u8> plane(const Component& c) const {
        if (!qt_ok[static_cast<usize>(c.tq)]) bad("missing quantisation table");
        const auto& q = qt[static_cast<usize>(c.tq)];
        const auto& T = dct_matrix();
        const usize pw = c.bw * 8;
        std::vector<u8> out(pw * c.bh * 8);
        float tmp[64], in[64];
        for (usize by = 0; by < c.bh; ++by)
            for (usize bx = 0; bx < c.bw; ++bx) {
                const i16* blk = &c.coef[(by * c.bw + bx) * 64];
                for (usize i = 0; i < 64; ++i) in[i] = static_cast<float>(blk[i] * q[i]);
                for (usize y = 0; y < 8; ++y)       // tmp = F * T^T  (rows)
                    for (usize x = 0; x < 8; ++x) {
                        float s = 0;
                        for (usize u = 0; u < 8; ++u) s += in[y * 8 + u] * T[x][u];
                        tmp[y * 8 + x] = s;
                    }
                for (usize x = 0; x < 8; ++x)       // out = T * tmp (columns)
                    for (usize y = 0; y < 8; ++y) {
                        float s = 0;
                        for (usize v = 0; v < 8; ++v) s += T[y][v] * tmp[v * 8 + x];
                        const float val = std::floor(s + 128.5f);
                        out[(by * 8 + y) * pw + bx * 8 + x] = static_cast<u8>(std::clamp(val, 0.0f, 255.0f));
                    }
            }
        return out;
    }

    /// Samples of component `c`, resampled to the full image size (triangle filter for subsampled components).
    std::vector<u8> full_plane(const Component& c, const std::vector<u8>& pl) const {
        const usize pw = c.bw * 8;
        std::vector<u8> out(W * H);
        if (c.h == hmax && c.v == vmax) {
            for (usize y = 0; y < H; ++y) std::memcpy(&out[y * W], &pl[y * pw], W);
            return out;
        }
        const float fx = static_cast<float>(c.h) / static_cast<float>(hmax), fy = static_cast<float>(c.v) / static_cast<float>(vmax);
        std::vector<usize> x0(W), x1(W);
        std::vector<float> wx(W);
        for (usize x = 0; x < W; ++x) {
            const float s = std::clamp((static_cast<float>(x) + 0.5f) * fx - 0.5f, 0.0f, static_cast<float>(c.cw - 1));
            x0[x] = static_cast<usize>(s);
            x1[x] = std::min(x0[x] + 1, c.cw - 1);
            wx[x] = s - static_cast<float>(x0[x]);
        }
        for (usize y = 0; y < H; ++y) {
            const float s = std::clamp((static_cast<float>(y) + 0.5f) * fy - 0.5f, 0.0f, static_cast<float>(c.ch - 1));
            const usize y0 = static_cast<usize>(s), y1 = std::min(y0 + 1, c.ch - 1);
            const float wy = s - static_cast<float>(y0);
            for (usize x = 0; x < W; ++x) {
                const float a = pl[y0 * pw + x0[x]] * (1 - wx[x]) + pl[y0 * pw + x1[x]] * wx[x];
                const float b = pl[y1 * pw + x0[x]] * (1 - wx[x]) + pl[y1 * pw + x1[x]] * wx[x];
                out[y * W + x] = static_cast<u8>(a * (1 - wy) + b * wy + 0.5f);
            }
        }
        return out;
    }

    static u8 clamp8(float v) { return static_cast<u8>(std::clamp(v + 0.5f, 0.0f, 255.0f)); }

    Image finish() {
        if (!have_sof || !any_scan) bad("no image data");
        std::vector<std::vector<u8>> pl;
        for (const auto& c : comps) pl.push_back(full_plane(c, plane(c)));
        Image img(W, H);
        const usize nc = comps.size();
        bool ycc = true;
        if (nc == 3) {
            if (adobe >= 0) ycc = adobe != 0;
            else if (!jfif && comps[0].id == 'R' && comps[1].id == 'G' && comps[2].id == 'B') ycc = false;
        } else if (nc == 4) {
            ycc = adobe == 2;
        }
        for (usize i = 0; i < W * H; ++i) {
            Color& o = img.data()[i];
            o.a = 255;
            if (nc == 1) {
                o.r = o.g = o.b = pl[0][i];
                continue;
            }
            float a = pl[0][i], b = pl[1][i], c = pl[2][i];
            if (ycc) {
                const float y = a, cb = b - 128.0f, cr = c - 128.0f;
                a = y + 1.402f * cr;
                b = y - 0.344136f * cb - 0.714136f * cr;
                c = y + 1.772f * cb;
                a = std::clamp(a, 0.0f, 255.0f), b = std::clamp(b, 0.0f, 255.0f), c = std::clamp(c, 0.0f, 255.0f);
            }
            if (nc == 4) {  // Adobe CMYK/YCCK files store inverted ink values (255 = no ink)
                if (adobe == 2) a = 255.0f - a, b = 255.0f - b, c = 255.0f - c;  // YCCK -> stored CMY
                const float k = pl[3][i];
                o.r = clamp8(a * k / 255.0f - 0.5f);
                o.g = clamp8(b * k / 255.0f - 0.5f);
                o.b = clamp8(c * k / 255.0f - 0.5f);
                continue;
            }
            o.r = clamp8(a - 0.5f), o.g = clamp8(b - 0.5f), o.b = clamp8(c - 0.5f);
        }
        return img;
    }

    Image run() {
        if (n < 4 || d[0] != 0xff || d[1] != 0xd8) bad("not a JPEG file");
        usize p = 2;
        for (;;) {
            while (p < n && d[p] != 0xff) ++p;  // tolerate junk between segments
            while (p < n && d[p] == 0xff) ++p;
            if (p >= n) break;
            const int m = d[p++];
            if (m == 0xd9) break;                                  // EOI
            if (m == 0x00 || m == 0x01 || (m >= 0xd0 && m <= 0xd8)) continue;  // standalone
            if (p + 2 > n) bad("truncated segment");
            const usize len = r16(d + p);
            if (len < 2 || p + len > n) bad("truncated segment");
            const usize body = p + 2, blen = len - 2;
            switch (m) {
                case 0xc0: case 0xc1: parse_sof(body, blen, 0); break;
                case 0xc2: parse_sof(body, blen, 2); break;
                case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
                    bad("unsupported coding process (arithmetic / lossless / hierarchical)");
                case 0xc4: parse_dht(body, blen); break;
                case 0xdb: parse_dqt(body, blen); break;
                case 0xdd: if (blen < 2) bad("bad DRI"); restart_interval = r16(d + body); break;
                case 0xe0: if (blen >= 5 && std::memcmp(d + body, "JFIF", 5) == 0) jfif = true; break;
                case 0xee: if (blen >= 12 && std::memcmp(d + body, "Adobe", 5) == 0) adobe = d[body + 11]; break;
                case 0xda: p = scan(body, blen); continue;
                default: break;
            }
            p += len;
        }
        return finish();
    }
};

// ----------------------------------------------------------------------------------------------- encoder
inline constexpr u8 kLumQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,  69,  56,  14, 17,
                                 22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103, 77,  24,  35, 55, 64, 81, 104, 113, 92,  49,  64, 78, 87,
                                 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
inline constexpr u8 kChrQ[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

inline constexpr u8 kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
inline constexpr u8 kDcChrBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
inline constexpr u8 kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
inline constexpr u8 kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
inline constexpr u8 kAcLumVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
inline constexpr u8 kAcChrBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
inline constexpr u8 kAcChrVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffEnc {
    std::array<u16, 256> code{};
    std::array<u8, 256> size{};
    void build(const u8* bits, const u8* vals) {
        u32 c = 0;
        usize k = 0;
        for (int l = 1; l <= 16; ++l) {
            for (int i = 0; i < bits[l - 1]; ++i) {
                code[vals[k]] = static_cast<u16>(c++);
                size[vals[k]] = static_cast<u8>(l);
                ++k;
            }
            c <<= 1;
        }
    }
};

class BitWriter {
public:
    explicit BitWriter(Bytes& o) : o_(o) {}
    void put(u32 v, int n) {
        acc_ = (acc_ << n) | (v & ((1u << n) - 1));
        cnt_ += n;
        while (cnt_ >= 8) {
            const u8 b = static_cast<u8>(acc_ >> (cnt_ - 8));
            o_.push_back(static_cast<std::byte>(b));
            if (b == 0xff) o_.push_back(std::byte{0});
            cnt_ -= 8;
        }
    }
    void flush() { if (cnt_) put(0x7f, 8 - cnt_); }
private:
    Bytes& o_;
    u32 acc_ = 0;
    int cnt_ = 0;
};

}  // namespace detail

[[nodiscard]] inline Image decode(ByteView in) {
    detail::Decoder d{reinterpret_cast<const u8*>(in.data()), in.size()};
    return d.run();
}

enum class Subsampling { s444, s422, s420 };

struct EncodeOptions {
    int quality = 90;                       ///< 1..100
    Subsampling subsampling = Subsampling::s420;
    bool grayscale = false;                 ///< write a single-component file
};

/// Baseline JPEG. Transparent pixels are composited over white because JPEG has no alpha channel.
[[nodiscard]] inline Bytes encode(const Image& img, const EncodeOptions& opt = {}) {
    using namespace detail;
    if (img.empty() || img.width() > 65535 || img.height() > 65535) bad("invalid image size");
    if (opt.quality < 1 || opt.quality > 100) bad("quality must be in 1..100");
    const usize W = img.width(), H = img.height();
    const bool gray = opt.grayscale;
    const usize nc = gray ? 1 : 3;
    const usize hs = (!gray && opt.subsampling != Subsampling::s444) ? 2 : 1;
    const usize vs = (!gray && opt.subsampling == Subsampling::s420) ? 2 : 1;
    const usize mw = 8 * hs, mh = 8 * vs;
    const usize mcux = (W + mw - 1) / mw, mcuy = (H + mh - 1) / mh;

    // quantisation tables
    const int scale = opt.quality < 50 ? 5000 / opt.quality : 200 - 2 * opt.quality;
    std::array<std::array<u8, 64>, 2> q{};
    for (usize i = 0; i < 64; ++i) {
        q[0][i] = static_cast<u8>(std::clamp((kLumQ[i] * scale + 50) / 100, 1, 255));
        q[1][i] = static_cast<u8>(std::clamp((kChrQ[i] * scale + 50) / 100, 1, 255));
    }

    // planes, padded to whole MCUs by edge replication
    std::vector<std::vector<float>> pl(nc);
    const usize pw = mcux * mw, ph = mcuy * mh;
    for (auto& p : pl) p.assign(pw * ph, 0.0f);
    for (usize y = 0; y < ph; ++y)
        for (usize x = 0; x < pw; ++x) {
            Color c = img(std::min(x, W - 1), std::min(y, H - 1));
            float r = c.r, g = c.g, b = c.b;
            if (c.a != 255) {
                const float a = static_cast<float>(c.a) / 255.0f;
                r = r * a + 255.0f * (1 - a), g = g * a + 255.0f * (1 - a), b = b * a + 255.0f * (1 - a);
            }
            pl[0][y * pw + x] = 0.299f * r + 0.587f * g + 0.114f * b;
            if (nc == 3) {
                pl[1][y * pw + x] = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
                pl[2][y * pw + x] = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
            }
        }
    // subsample chroma by box averaging
    const usize cpw = pw / hs, cph = ph / vs;
    for (usize ci = 1; ci < nc; ++ci) {
        if (hs == 1 && vs == 1) break;
        std::vector<float> s(cpw * cph);
        for (usize y = 0; y < cph; ++y)
            for (usize x = 0; x < cpw; ++x) {
                float sum = 0;
                for (usize dy = 0; dy < vs; ++dy)
                    for (usize dx = 0; dx < hs; ++dx) sum += pl[ci][(y * vs + dy) * pw + x * hs + dx];
                s[y * cpw + x] = sum / static_cast<float>(hs * vs);
            }
        pl[ci] = std::move(s);
    }

    Bytes o;
    auto m16 = [&](u32 v) { o.push_back(static_cast<std::byte>(v >> 8)); o.push_back(static_cast<std::byte>(v)); };
    auto mark = [&](u32 m) { o.push_back(std::byte{0xff}); o.push_back(static_cast<std::byte>(m)); };
    mark(0xd8);
    mark(0xe0);
    m16(16);
    for (char c : {'J', 'F', 'I', 'F', '\0'}) o.push_back(static_cast<std::byte>(c));
    o.push_back(std::byte{1}), o.push_back(std::byte{1}), o.push_back(std::byte{0});
    m16(1), m16(1);
    o.push_back(std::byte{0}), o.push_back(std::byte{0});
    for (usize t = 0; t < (nc == 1 ? 1u : 2u); ++t) {
        mark(0xdb);
        m16(67);
        o.push_back(static_cast<std::byte>(t));
        for (usize i = 0; i < 64; ++i) o.push_back(static_cast<std::byte>(q[t][kZigzag[i]]));
    }
    mark(0xc0);
    m16(static_cast<u32>(8 + 3 * nc));
    o.push_back(std::byte{8});
    m16(static_cast<u32>(H)), m16(static_cast<u32>(W));
    o.push_back(static_cast<std::byte>(nc));
    for (usize i = 0; i < nc; ++i) {
        o.push_back(static_cast<std::byte>(i + 1));
        o.push_back(static_cast<std::byte>(i == 0 ? (hs << 4 | vs) : 0x11));
        o.push_back(static_cast<std::byte>(i == 0 ? 0 : 1));
    }
    auto dht = [&](int cls, int id, const u8* bits, const u8* vals, usize nv) {
        mark(0xc4);
        m16(static_cast<u32>(19 + nv));
        o.push_back(static_cast<std::byte>(cls << 4 | id));
        for (int i = 0; i < 16; ++i) o.push_back(static_cast<std::byte>(bits[i]));
        for (usize i = 0; i < nv; ++i) o.push_back(static_cast<std::byte>(vals[i]));
    };
    dht(0, 0, kDcLumBits, kDcVals, 12);
    dht(1, 0, kAcLumBits, kAcLumVals, 162);
    if (nc == 3) {
        dht(0, 1, kDcChrBits, kDcVals, 12);
        dht(1, 1, kAcChrBits, kAcChrVals, 162);
    }
    mark(0xda);
    m16(static_cast<u32>(6 + 2 * nc));
    o.push_back(static_cast<std::byte>(nc));
    for (usize i = 0; i < nc; ++i) {
        o.push_back(static_cast<std::byte>(i + 1));
        o.push_back(static_cast<std::byte>(i == 0 ? 0x00 : 0x11));
    }
    o.push_back(std::byte{0}), o.push_back(std::byte{63}), o.push_back(std::byte{0});

    HuffEnc dcl, acl, dcc, acc;
    dcl.build(kDcLumBits, kDcVals);
    acl.build(kAcLumBits, kAcLumVals);
    dcc.build(kDcChrBits, kDcVals);
    acc.build(kAcChrBits, kAcChrVals);
    BitWriter bw(o);
    const auto& T = dct_matrix();
    int pred[3] = {0, 0, 0};

    auto block = [&](usize ci, const std::vector<float>& plane, usize stride, usize bx, usize by) {
        float in[64], tmp[64];
        for (usize y = 0; y < 8; ++y)
            for (usize x = 0; x < 8; ++x) in[y * 8 + x] = plane[(by * 8 + y) * stride + bx * 8 + x] - 128.0f;
        for (usize y = 0; y < 8; ++y)  // tmp = in * T
            for (usize u = 0; u < 8; ++u) {
                float s = 0;
                for (usize x = 0; x < 8; ++x) s += in[y * 8 + x] * T[x][u];
                tmp[y * 8 + u] = s;
            }
        int zz[64];
        const auto& qt = q[ci == 0 ? 0 : 1];
        for (usize v = 0; v < 8; ++v)  // F = T^T * tmp
            for (usize u = 0; u < 8; ++u) {
                float s = 0;
                for (usize y = 0; y < 8; ++y) s += T[y][v] * tmp[y * 8 + u];
                const usize nat = v * 8 + u;
                zz[nat] = static_cast<int>(std::lround(s / static_cast<float>(qt[nat])));
            }
        auto nbits = [](int v) { int n = 0; for (v = std::abs(v); v; v >>= 1) ++n; return n; };
        const HuffEnc& dct = ci == 0 ? dcl : dcc;
        const HuffEnc& act = ci == 0 ? acl : acc;
        const int diff = zz[0] - pred[ci];
        pred[ci] = zz[0];
        int nb = nbits(diff);
        bw.put(dct.code[static_cast<usize>(nb)], dct.size[static_cast<usize>(nb)]);
        if (nb) bw.put(static_cast<u32>(diff < 0 ? diff - 1 : diff), nb);
        int run = 0;
        for (usize k = 1; k < 64; ++k) {
            const int v = zz[kZigzag[k]];
            if (v == 0) { ++run; continue; }
            while (run > 15) { bw.put(act.code[0xf0], act.size[0xf0]); run -= 16; }
            nb = nbits(v);
            const usize sym = static_cast<usize>(run << 4 | nb);
            bw.put(act.code[sym], act.size[sym]);
            bw.put(static_cast<u32>(v < 0 ? v - 1 : v), nb);
            run = 0;
        }
        if (run) bw.put(act.code[0], act.size[0]);
    };

    for (usize my = 0; my < mcuy; ++my)
        for (usize mx = 0; mx < mcux; ++mx) {
            for (usize by = 0; by < vs; ++by)
                for (usize bx = 0; bx < hs; ++bx) block(0, pl[0], pw, mx * hs + bx, my * vs + by);
            for (usize ci = 1; ci < nc; ++ci) block(ci, pl[ci], cpw, mx, my);
        }
    bw.flush();
    mark(0xd9);
    return o;
}

}  // namespace flyg::graphics::jpeg
