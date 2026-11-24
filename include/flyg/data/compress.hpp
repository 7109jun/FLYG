// flyG - data module: DEFLATE (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952)
// SPDX-License-Identifier: GPL-2.0-only
//
// Own implementation (no zlib dependency). The compressor does LZ77 with hash chains + lazy matching and picks
// stored / fixed / dynamic Huffman blocks by size. The decompressor validates everything (over-subscribed trees,
// bad distances, truncated input, checksums) and enforces an output limit against decompression bombs.
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>
#include <flyg/data/encoding.hpp>

namespace flyg::data {

using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

[[nodiscard]] inline u32 adler32(std::span<const std::byte> d, u32 adler = 1) noexcept {
    u32 a = adler & 0xffff, b = adler >> 16;
    usize i = 0;
    while (i < d.size()) {
        const usize n = std::min<usize>(5552, d.size() - i);
        for (usize k = 0; k < n; ++k) {
            a += static_cast<u8>(d[i + k]);
            b += a;
        }
        a %= 65521;
        b %= 65521;
        i += n;
    }
    return (b << 16) | a;
}

namespace detail_deflate {

inline constexpr std::array<u16, 29> len_base = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr std::array<u8, 29> len_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr std::array<u16, 30> dist_base = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::array<u8, 30> dist_extra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
inline constexpr std::array<u8, 19> clen_order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

[[noreturn]] inline void bad(const char* m) { throw DataException(std::string("deflate: ") + m); }

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> d) : p_(reinterpret_cast<const u8*>(d.data())), end_(p_ + d.size()) {}
    void refill() noexcept {
        while (n_ <= 56 && p_ < end_) {
            buf_ |= static_cast<u64>(*p_++) << n_;
            n_ += 8;
        }
    }
    u32 bits(unsigned n) {
        if (n == 0) return 0;
        if (n_ < n) {
            refill();
            if (n_ < n) bad("unexpected end of data");
        }
        const u32 v = static_cast<u32>(buf_ & ((1ull << n) - 1));
        buf_ >>= n;
        n_ -= n;
        return v;
    }
    [[nodiscard]] u64 peek() noexcept { refill(); return buf_; }
    [[nodiscard]] unsigned avail() const noexcept { return n_; }
    void drop(unsigned n) noexcept { buf_ >>= n; n_ -= n; }
    void align_byte() noexcept { drop(n_ & 7); }
    /// Bytes not consumed yet (valid after align_byte()).
    [[nodiscard]] const u8* cursor() noexcept {
        // give back whole bytes still held in the bit buffer
        p_ -= n_ / 8;
        buf_ = 0;
        n_ = 0;
        return p_;
    }
    [[nodiscard]] const u8* end() const noexcept { return end_; }
    void advance(usize n) noexcept { p_ += n; }

private:
    const u8* p_;
    const u8* end_;
    u64 buf_ = 0;
    unsigned n_ = 0;
};

struct Decoder {
    std::vector<u16> table;  ///< (symbol << 4) | length, indexed by the next `bits` input bits (LSB-first)
    unsigned bits = 0;
    void build(const u8* lens, usize n, bool allow_incomplete) {
        std::array<u32, 16> count{};
        for (usize i = 0; i < n; ++i) ++count[lens[i]];
        count[0] = 0;
        unsigned maxlen = 0;
        for (unsigned l = 1; l < 16; ++l)
            if (count[l]) maxlen = l;
        // Kraft check
        long left = 1;
        for (unsigned l = 1; l < 16; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) bad("over-subscribed Huffman code");
        }
        if (left > 0 && !allow_incomplete && maxlen > 0) bad("incomplete Huffman code");
        bits = std::max(maxlen, 1u);
        table.assign(usize(1) << bits, 0);
        std::array<u32, 17> next{};
        u32 code = 0;
        for (unsigned l = 1; l < 16; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        for (usize s = 0; s < n; ++s) {
            const unsigned l = lens[s];
            if (!l) continue;
            const u32 c = next[l]++;
            u32 rev = 0;
            for (unsigned i = 0; i < l; ++i) rev |= ((c >> i) & 1u) << (l - 1 - i);
            for (usize i = rev; i < table.size(); i += usize(1) << l) table[i] = static_cast<u16>((s << 4) | l);
        }
    }
    unsigned decode(BitReader& br) const {
        const u64 b = br.peek();
        const u16 e = table[b & ((1u << bits) - 1)];
        const unsigned l = e & 15;
        if (l == 0 || l > br.avail()) bad(l == 0 ? "invalid Huffman code" : "unexpected end of data");
        br.drop(l);
        return e >> 4;
    }
};

}  // namespace detail_deflate

/// Decompresses a raw DEFLATE stream. `consumed` (optional) receives the number of input bytes used.
[[nodiscard]] inline std::vector<std::byte> inflate(std::span<const std::byte> in, usize max_output = usize(1) << 30, usize* consumed = nullptr) {
    using namespace detail_deflate;
    BitReader br(in);
    std::vector<std::byte> out;
    out.reserve(std::min<usize>(in.size() * 4, max_output));
    Decoder lit, dist;
    static const Decoder fixed_lit = [] {
        u8 l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        Decoder d;
        d.build(l, 288, false);
        return d;
    }();
    static const Decoder fixed_dist = [] {
        u8 l[30];
        for (auto& v : l) v = 5;
        Decoder d;
        d.build(l, 30, true);
        return d;
    }();
    bool last = false;
    while (!last) {
        last = br.bits(1);
        const u32 type = br.bits(2);
        if (type == 0) {
            br.align_byte();
            const u32 len = br.bits(16), nlen = br.bits(16);
            if ((len ^ 0xffff) != nlen) bad("stored block length check failed");
            // remaining bits in the buffer are whole bytes
            const usize avail_bytes = br.avail() / 8;
            usize need = len;
            while (need && br.avail() >= 8) {
                if (out.size() >= max_output) bad("output limit exceeded");
                out.push_back(static_cast<std::byte>(br.bits(8)));
                --need;
            }
            (void)avail_bytes;
            if (need) {
                const u8* c = br.cursor();
                if (static_cast<usize>(br.end() - c) < need) bad("unexpected end of data");
                if (out.size() + need > max_output) bad("output limit exceeded");
                out.insert(out.end(), reinterpret_cast<const std::byte*>(c), reinterpret_cast<const std::byte*>(c) + need);
                br.advance(need);
            }
            continue;
        }
        if (type == 3) bad("invalid block type");
        const Decoder* L = &fixed_lit;
        const Decoder* D = &fixed_dist;
        if (type == 2) {
            const u32 hlit = br.bits(5) + 257, hdist = br.bits(5) + 1, hclen = br.bits(4) + 4;
            if (hlit > 286 || hdist > 30) bad("too many length or distance codes");
            u8 cl[19] = {};
            for (u32 i = 0; i < hclen; ++i) cl[clen_order[i]] = static_cast<u8>(br.bits(3));
            Decoder clen;
            clen.build(cl, 19, false);
            u8 lens[286 + 30] = {};
            u32 i = 0;
            while (i < hlit + hdist) {
                const unsigned sym = clen.decode(br);
                if (sym < 16) {
                    lens[i++] = static_cast<u8>(sym);
                } else {
                    u32 rep;
                    u8 val = 0;
                    if (sym == 16) {
                        if (i == 0) bad("repeat without previous length");
                        val = lens[i - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (i + rep > hlit + hdist) bad("code length repeat overflows");
                    while (rep--) lens[i++] = val;
                }
            }
            if (lens[256] == 0) bad("missing end-of-block code");
            lit.build(lens, hlit, false);
            dist.build(lens + hlit, hdist, true);
            L = &lit;
            D = &dist;
        }
        for (;;) {
            const unsigned sym = L->decode(br);
            if (sym < 256) {
                if (out.size() >= max_output) bad("output limit exceeded");
                out.push_back(static_cast<std::byte>(sym));
            } else if (sym == 256) {
                break;
            } else {
                const unsigned li = sym - 257;
                if (li >= 29) bad("invalid length symbol");
                const u32 len = len_base[li] + br.bits(len_extra[li]);
                const unsigned ds = D->decode(br);
                if (ds >= 30) bad("invalid distance symbol");
                const u32 d = dist_base[ds] + br.bits(dist_extra[ds]);
                if (d > out.size()) bad("distance too far back");
                if (out.size() + len > max_output) bad("output limit exceeded");
                const usize from = out.size() - d;
                for (u32 k = 0; k < len; ++k) out.push_back(out[from + k]);
            }
        }
    }
    if (consumed) {
        br.align_byte();
        const u8* c = br.cursor();
        *consumed = static_cast<usize>(c - reinterpret_cast<const u8*>(in.data()));
    }
    return out;
}

namespace detail_deflate {

class BitWriter {
public:
    std::vector<std::byte> out;
    void put(u32 v, unsigned n) {
        buf_ |= static_cast<u64>(v) << cnt_;
        cnt_ += n;
        while (cnt_ >= 8) {
            out.push_back(static_cast<std::byte>(buf_ & 0xff));
            buf_ >>= 8;
            cnt_ -= 8;
        }
    }
    void align() {
        if (cnt_) put(0, 8 - cnt_);
    }

private:
    u64 buf_ = 0;
    unsigned cnt_ = 0;
};

inline u32 rev_bits(u32 v, unsigned n) {
    u32 r = 0;
    for (unsigned i = 0; i < n; ++i) r |= ((v >> i) & 1u) << (n - 1 - i);
    return r;
}

/// Computes length-limited Huffman code lengths. Frequencies are halved until the limit holds (simple, always terminates).
inline void huff_lengths(const std::vector<u32>& freq_in, unsigned limit, std::vector<u8>& lens) {
    const usize n = freq_in.size();
    lens.assign(n, 0);
    std::vector<u32> freq = freq_in;
    usize used = 0, last = 0;
    for (usize i = 0; i < n; ++i)
        if (freq[i]) ++used, last = i;
    if (used == 0) return;
    if (used == 1) {
        lens[last] = 1;
        return;
    }
    for (;;) {
        struct Node { u64 w; int a, b; };
        std::vector<Node> nodes;
        using Item = std::pair<u64, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
        std::vector<int> leaf_of(n, -1);
        for (usize i = 0; i < n; ++i)
            if (freq[i]) {
                leaf_of[i] = static_cast<int>(nodes.size());
                nodes.push_back({freq[i], -1, -1});
                pq.push({freq[i], leaf_of[i]});
            }
        while (pq.size() > 1) {
            const auto x = pq.top(); pq.pop();
            const auto y = pq.top(); pq.pop();
            nodes.push_back({x.first + y.first, x.second, y.second});
            pq.push({x.first + y.first, static_cast<int>(nodes.size()) - 1});
        }
        std::vector<u8> depth(nodes.size(), 0);
        unsigned maxd = 0;
        for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i) {
            if (nodes[static_cast<usize>(i)].a >= 0) {
                depth[static_cast<usize>(nodes[static_cast<usize>(i)].a)] = static_cast<u8>(depth[static_cast<usize>(i)] + 1);
                depth[static_cast<usize>(nodes[static_cast<usize>(i)].b)] = static_cast<u8>(depth[static_cast<usize>(i)] + 1);
            }
        }
        for (usize i = 0; i < n; ++i)
            if (leaf_of[i] >= 0) {
                lens[i] = depth[static_cast<usize>(leaf_of[i])];
                maxd = std::max<unsigned>(maxd, lens[i]);
            }
        if (maxd <= limit) return;
        for (auto& f : freq)
            if (f) f = (f + 1) / 2;
    }
}

inline void canonical_codes(const std::vector<u8>& lens, std::vector<u16>& codes) {
    std::array<u32, 17> count{}, next{};
    for (u8 l : lens) ++count[l];
    count[0] = 0;
    u32 code = 0;
    for (unsigned l = 1; l <= 16; ++l) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    codes.assign(lens.size(), 0);
    for (usize i = 0; i < lens.size(); ++i)
        if (lens[i]) codes[i] = static_cast<u16>(rev_bits(next[lens[i]]++, lens[i]));
}

struct Token {
    u16 len;   ///< 0 = literal
    u16 dist;  ///< literal byte when len == 0
};

inline unsigned len_symbol(unsigned len) {
    unsigned i = 28;
    while (len_base[i] > len) --i;
    return i;
}
inline unsigned dist_symbol(unsigned d) {
    unsigned i = 29;
    while (dist_base[i] > d) --i;
    return i;
}

}  // namespace detail_deflate

/// Compresses to a raw DEFLATE stream. level 0 = stored, 1..9 = increasing effort (6 is the usual default).
[[nodiscard]] inline std::vector<std::byte> deflate(std::span<const std::byte> in, int level = 6) {
    using namespace detail_deflate;
    if (level < 0 || level > 9) throw DataException("deflate: level must be 0..9");
    BitWriter bw;
    const u8* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();

    auto write_stored = [&](usize from, usize to, bool final_block) {
        usize pos = from;
        do {
            const usize chunk = std::min<usize>(65535, to - pos);
            const bool fin = final_block && pos + chunk == to;
            bw.put(fin ? 1 : 0, 1);
            bw.put(0, 2);
            bw.align();
            bw.put(static_cast<u32>(chunk), 16);
            bw.put(static_cast<u32>(chunk ^ 0xffff), 16);
            for (usize i = 0; i < chunk; ++i) bw.put(d[pos + i], 8);
            pos += chunk;
        } while (pos < to);
    };
    if (level == 0 || n == 0) {
        if (n == 0) {  // single empty final fixed block
            bw.put(1, 1);
            bw.put(1, 2);
            bw.put(0, 7);
            bw.align();
            return std::move(bw.out);
        }
        write_stored(0, n, true);
        bw.align();
        return std::move(bw.out);
    }

    struct Params { unsigned chain, lazy, nice; };
    static constexpr Params P[10] = {{0, 0, 0}, {4, 0, 8}, {8, 0, 16}, {16, 0, 32}, {16, 4, 16}, {32, 8, 32}, {128, 16, 128}, {256, 32, 128}, {1024, 128, 258}, {4096, 258, 258}};
    const Params prm = P[level];
    constexpr usize W = 32768, HASH = 1 << 15;
    std::vector<u32> head(HASH, 0xffffffffu), prev(W, 0xffffffffu);
    auto hash3 = [&](usize i) -> u32 { return ((u32(d[i]) << 10) ^ (u32(d[i + 1]) << 5) ^ u32(d[i + 2])) & static_cast<u32>(HASH - 1); };
    auto insert = [&](usize i) {
        if (i + 2 >= n) return;
        const u32 h = hash3(i);
        prev[i & (W - 1)] = head[h];
        head[h] = static_cast<u32>(i);
    };
    auto longest = [&](usize i, unsigned& best_dist) -> unsigned {
        unsigned best = 2;
        if (i + 2 >= n) return 0;
        const unsigned maxlen = static_cast<unsigned>(std::min<usize>(258, n - i));
        u32 cand = head[hash3(i)];
        unsigned chain = prm.chain;
        while (cand != 0xffffffffu && chain--) {
            const usize dist = i - cand;
            if (dist == 0 || dist > W) break;
            if (d[cand + best] == d[i + best] || best >= maxlen) {
                unsigned l = 0;
                while (l < maxlen && d[cand + l] == d[i + l]) ++l;
                if (l > best) {
                    best = l;
                    best_dist = static_cast<unsigned>(dist);
                    if (best >= prm.nice || best >= maxlen) break;
                }
            }
            const u32 nxt = prev[cand & (W - 1)];
            if (nxt >= cand) break;
            cand = nxt;
        }
        return best >= 3 ? best : 0;
    };

    std::vector<Token> toks;
    toks.reserve(1 << 16);
    usize block_start = 0, i = 0;
    auto flush_block = [&](bool final_block, usize end_pos) {
        // symbol statistics
        std::vector<u32> lf(286, 0), df(30, 0);
        for (const Token& t : toks) {
            if (t.len == 0) ++lf[t.dist];
            else ++lf[257 + len_symbol(t.len)], ++df[dist_symbol(t.dist)];
        }
        lf[256] = 1;
        std::vector<u8> ll, dl;
        huff_lengths(lf, 15, ll);
        huff_lengths(df, 15, dl);
        // a lone distance code must still be a complete tree for strict decoders: give two codes
        {
            usize used = 0;
            for (u8 l : dl) used += l != 0;
            if (used == 0) dl[0] = 1;
            if (used <= 1) { if (dl[0] == 0) dl[0] = 1; else dl[1] = 1; }
        }
        usize hlit = 286, hdist = 30;
        while (hlit > 257 && ll[hlit - 1] == 0) --hlit;
        while (hdist > 1 && dl[hdist - 1] == 0) --hdist;
        // run-length encode the code lengths
        std::vector<u8> all(ll.begin(), ll.begin() + static_cast<std::ptrdiff_t>(hlit));
        all.insert(all.end(), dl.begin(), dl.begin() + static_cast<std::ptrdiff_t>(hdist));
        struct Rl { u8 sym, extra; };
        std::vector<Rl> rl;
        for (usize k = 0; k < all.size();) {
            usize run = 1;
            while (k + run < all.size() && all[k + run] == all[k]) ++run;
            if (all[k] == 0 && run >= 3) {
                usize r = std::min<usize>(run, 138);
                rl.push_back(r >= 11 ? Rl{18, static_cast<u8>(r - 11)} : Rl{17, static_cast<u8>(r - 3)});
                k += r;
            } else if (all[k] != 0 && run >= 4) {
                rl.push_back({all[k], 0});
                usize r = std::min<usize>(run - 1, 6);
                rl.push_back({16, static_cast<u8>(r - 3)});
                k += 1 + r;
            } else {
                rl.push_back({all[k], 0});
                ++k;
            }
        }
        std::vector<u32> cf(19, 0);
        for (const Rl& r : rl) ++cf[r.sym];
        std::vector<u8> cl;
        huff_lengths(cf, 7, cl);
        usize hclen = 19;
        while (hclen > 4 && cl[clen_order[hclen - 1]] == 0) --hclen;
        // cost estimate
        u64 dyn = 3 + 5 + 5 + 4 + hclen * 3;
        for (const Rl& r : rl) dyn += cl[r.sym] + (r.sym == 16 ? 2 : r.sym == 17 ? 3 : r.sym == 18 ? 7 : 0);
        u64 fix = 3;
        for (usize s = 0; s < 286; ++s) {
            const u64 fl = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
            const unsigned extra = s >= 257 ? len_extra[s - 257] : 0;
            dyn += u64(lf[s]) * (ll[s] + extra);
            fix += u64(lf[s]) * (fl + extra);
        }
        for (usize s = 0; s < 30; ++s) {
            dyn += u64(df[s]) * (dl[s] + dist_extra[s]);
            fix += u64(df[s]) * (5 + dist_extra[s]);
        }
        const u64 stored = (end_pos - block_start) * 8 + 40 + ((end_pos - block_start) / 65535) * 40;
        if (stored <= dyn && stored <= fix) {
            write_stored(block_start, end_pos, final_block);
            return;
        }
        std::vector<u16> lc, dc, cc;
        const bool use_fixed = fix <= dyn;
        std::vector<u8> fl(288), fd(30, 5);
        if (use_fixed) {
            for (int s = 0; s < 288; ++s) fl[static_cast<usize>(s)] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
            canonical_codes(fl, lc);
            canonical_codes(fd, dc);
            bw.put(final_block ? 1 : 0, 1);
            bw.put(1, 2);
        } else {
            canonical_codes(ll, lc);
            canonical_codes(dl, dc);
            canonical_codes(cl, cc);
            bw.put(final_block ? 1 : 0, 1);
            bw.put(2, 2);
            bw.put(static_cast<u32>(hlit - 257), 5);
            bw.put(static_cast<u32>(hdist - 1), 5);
            bw.put(static_cast<u32>(hclen - 4), 4);
            for (usize k = 0; k < hclen; ++k) bw.put(cl[clen_order[k]], 3);
            for (const Rl& r : rl) {
                bw.put(cc[r.sym], cl[r.sym]);
                if (r.sym == 16) bw.put(r.extra, 2);
                else if (r.sym == 17) bw.put(r.extra, 3);
                else if (r.sym == 18) bw.put(r.extra, 7);
            }
        }
        const std::vector<u8>& LL = use_fixed ? fl : ll;
        const std::vector<u8>& DL = use_fixed ? fd : dl;
        for (const Token& t : toks) {
            if (t.len == 0) {
                bw.put(lc[t.dist], LL[t.dist]);
            } else {
                const unsigned ls = len_symbol(t.len);
                bw.put(lc[257 + ls], LL[257 + ls]);
                bw.put(t.len - len_base[ls], len_extra[ls]);
                const unsigned ds = dist_symbol(t.dist);
                bw.put(dc[ds], DL[ds]);
                bw.put(t.dist - dist_base[ds], dist_extra[ds]);
            }
        }
        bw.put(lc[256], LL[256]);
    };

    while (i < n) {
        unsigned dist = 0;
        unsigned len = longest(i, dist);
        if (len >= 3 && prm.lazy && len < prm.lazy && i + 1 < n) {
            insert(i);
            unsigned d2 = 0;
            const unsigned l2 = longest(i + 1, d2);
            if (l2 > len) {  // emit a literal and take the better match next round
                toks.push_back({0, d[i]});
                ++i;
                len = 0;
            } else {
                // keep the current match; position i is already inserted
                toks.push_back({static_cast<u16>(len), static_cast<u16>(dist)});
                for (unsigned k = 1; k < len; ++k) insert(i + k);
                i += len;
                len = 0;
            }
        } else if (len >= 3) {
            toks.push_back({static_cast<u16>(len), static_cast<u16>(dist)});
            for (unsigned k = 0; k < len; ++k) insert(i + k);
            i += len;
            len = 0;
        } else {
            toks.push_back({0, d[i]});
            insert(i);
            ++i;
        }
        if (toks.size() >= (1u << 16) - 8 && i < n) {
            flush_block(false, i);
            block_start = i;
            toks.clear();
        }
    }
    flush_block(true, n);
    bw.align();
    return std::move(bw.out);
}

// ---- zlib ---------------------------------------------------------------------------------------------------------

[[nodiscard]] inline std::vector<std::byte> zlib_compress(std::span<const std::byte> in, int level = 6) {
    std::vector<std::byte> out;
    const u8 flevel = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
    u32 hdr = (0x78u << 8) | (u32(flevel) << 6);
    hdr += 31 - hdr % 31;
    out.push_back(static_cast<std::byte>(hdr >> 8));
    out.push_back(static_cast<std::byte>(hdr & 0xff));
    const auto body = deflate(in, level);
    out.insert(out.end(), body.begin(), body.end());
    const u32 a = adler32(in);
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::byte>(a >> s));
    return out;
}

[[nodiscard]] inline std::vector<std::byte> zlib_decompress(std::span<const std::byte> in, usize max_output = usize(1) << 30) {
    if (in.size() < 6) throw DataException("zlib: stream too short");
    const u32 cmf = static_cast<u8>(in[0]), flg = static_cast<u8>(in[1]);
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0) throw DataException("zlib: bad header");
    if (flg & 0x20) throw DataException("zlib: preset dictionaries are not supported");
    usize used = 0;
    auto out = inflate(in.subspan(2), max_output, &used);
    if (in.size() < 2 + used + 4) throw DataException("zlib: missing checksum");
    u32 want = 0;
    for (int k = 0; k < 4; ++k) want = (want << 8) | static_cast<u8>(in[2 + used + static_cast<usize>(k)]);
    if (want != adler32(out)) throw DataException("zlib: checksum mismatch");
    return out;
}

// ---- gzip ---------------------------------------------------------------------------------------------------------

[[nodiscard]] inline std::vector<std::byte> gzip_compress(std::span<const std::byte> in, int level = 6) {
    std::vector<std::byte> out = {std::byte{0x1f}, std::byte{0x8b}, std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    const auto body = deflate(in, level);
    out.insert(out.end(), body.begin(), body.end());
    const u32 c = crc32(in);
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::byte>(c >> s));
    const u32 sz = static_cast<u32>(in.size());
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::byte>(sz >> s));
    return out;
}

/// Decompresses one or more concatenated gzip members.
[[nodiscard]] inline std::vector<std::byte> gzip_decompress(std::span<const std::byte> in, usize max_output = usize(1) << 30) {
    std::vector<std::byte> out;
    usize pos = 0;
    do {
        auto rest = in.subspan(pos);
        if (rest.size() < 18) throw DataException("gzip: stream too short");
        if (rest[0] != std::byte{0x1f} || rest[1] != std::byte{0x8b}) throw DataException("gzip: bad magic");
        if (rest[2] != std::byte{8}) throw DataException("gzip: unsupported compression method");
        const u8 flg = static_cast<u8>(rest[3]);
        if (flg & 0xe0) throw DataException("gzip: reserved flags set");
        usize p = 10;
        auto need = [&](usize k) { if (rest.size() < p + k) throw DataException("gzip: truncated header"); };
        if (flg & 4) { need(2); const usize xl = static_cast<u8>(rest[p]) | (usize(static_cast<u8>(rest[p + 1])) << 8); p += 2; need(xl); p += xl; }
        for (u8 bit : {u8(8), u8(16)})
            if (flg & bit) {
                while (true) { need(1); if (rest[p++] == std::byte{0}) break; }
            }
        if (flg & 2) { need(2); p += 2; }
        usize used = 0;
        auto member = inflate(rest.subspan(p), max_output - out.size(), &used);
        p += used;
        need(8);
        u32 crc = 0, isz = 0;
        for (int k = 3; k >= 0; --k) crc = (crc << 8) | static_cast<u8>(rest[p + static_cast<usize>(k)]);
        for (int k = 3; k >= 0; --k) isz = (isz << 8) | static_cast<u8>(rest[p + 4 + static_cast<usize>(k)]);
        if (crc != crc32(member)) throw DataException("gzip: CRC mismatch");
        if (isz != static_cast<u32>(member.size())) throw DataException("gzip: size mismatch");
        out.insert(out.end(), member.begin(), member.end());
        pos += p + 8;
    } while (pos < in.size());
    return out;
}

[[nodiscard]] inline std::vector<std::byte> deflate(std::string_view s, int level = 6) { return deflate(std::as_bytes(std::span(s.data(), s.size())), level); }

}  // namespace flyg::data
