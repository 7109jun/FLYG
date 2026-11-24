// flyG - net/tls: AES-GCM and ChaCha20-Poly1305 authenticated encryption
// SPDX-License-Identifier: GPL-2.0-only
//
// Portable implementations. The AES S-box lookups are not cache-timing hardened (no AES-NI path
// yet); GHASH and Poly1305 are branch-free. See the README/status document for details.
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>
#include <flyg/data/encoding.hpp>
#include <flyg/data/hash.hpp>

namespace flyg::net::tls {

using core::u32;
using core::u64;
using core::u8;
using core::usize;
using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

namespace detail {
[[nodiscard]] inline bool tag_equal(const u8* a, const u8* b, usize n) noexcept {
    unsigned d = 0;
    for (usize i = 0; i < n; ++i) d |= static_cast<unsigned>(a[i] ^ b[i]);
    return d == 0;
}
inline u32 le32(const u8* p) noexcept { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
inline void put_le32(u8* p, u32 v) noexcept { p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8), p[2] = static_cast<u8>(v >> 16), p[3] = static_cast<u8>(v >> 24); }
}  // namespace detail

// ---- AES ----------------------------------------------------------------------------------------------------------

class Aes {
public:
    /// key must be 16 or 32 bytes (AES-128 / AES-256; AES-192 is not used by TLS 1.2/1.3).
    explicit Aes(ByteView key) {
        if (key.size() != 16 && key.size() != 32) throw CryptoKeySize();
        const usize nk = key.size() / 4;
        rounds_ = static_cast<int>(nk) + 6;
        u8 w[240];
        for (usize i = 0; i < key.size(); ++i) w[i] = static_cast<u8>(key[i]);
        const auto& sb = sbox();
        u8 rcon = 1;
        for (usize i = nk; i < 4 * static_cast<usize>(rounds_ + 1); ++i) {
            u8 t[4];
            std::memcpy(t, w + 4 * (i - 1), 4);
            if (i % nk == 0) {
                const u8 t0 = t[0];
                t[0] = static_cast<u8>(sb[t[1]] ^ rcon);
                t[1] = sb[t[2]];
                t[2] = sb[t[3]];
                t[3] = sb[t0];
                rcon = static_cast<u8>((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
            } else if (nk > 6 && i % nk == 4) {
                for (auto& b : t) b = sb[b];
            }
            for (int k = 0; k < 4; ++k) w[4 * i + static_cast<usize>(k)] = static_cast<u8>(w[4 * (i - nk) + static_cast<usize>(k)] ^ t[k]);
        }
        std::memcpy(rk_, w, 16 * static_cast<usize>(rounds_ + 1));
    }
    struct CryptoKeySize : DataException {
        CryptoKeySize() : DataException("AES key must be 16 or 32 bytes") {}
    };

    void encrypt_block(const u8 in[16], u8 out[16]) const noexcept {
        const auto& sb = sbox();
        u8 s[16];
        for (int i = 0; i < 16; ++i) s[i] = static_cast<u8>(in[i] ^ rk_[i]);
        for (int r = 1; r <= rounds_; ++r) {
            u8 t[16];
            // SubBytes + ShiftRows
            static constexpr int shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};
            for (int i = 0; i < 16; ++i) t[i] = sb[s[shift[i]]];
            if (r < rounds_) {
                for (int c = 0; c < 4; ++c) {
                    const u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                    const u8 all = static_cast<u8>(a0 ^ a1 ^ a2 ^ a3);
                    t[4 * c] = static_cast<u8>(a0 ^ all ^ xtime(static_cast<u8>(a0 ^ a1)));
                    t[4 * c + 1] = static_cast<u8>(a1 ^ all ^ xtime(static_cast<u8>(a1 ^ a2)));
                    t[4 * c + 2] = static_cast<u8>(a2 ^ all ^ xtime(static_cast<u8>(a2 ^ a3)));
                    t[4 * c + 3] = static_cast<u8>(a3 ^ all ^ xtime(static_cast<u8>(a3 ^ a0)));
                }
            }
            for (int i = 0; i < 16; ++i) s[i] = static_cast<u8>(t[i] ^ rk_[16 * r + i]);
        }
        std::memcpy(out, s, 16);
    }

private:
    static u8 xtime(u8 x) noexcept { return static_cast<u8>((x << 1) ^ ((x >> 7) * 0x1b)); }
    static const std::array<u8, 256>& sbox() {
        static const std::array<u8, 256> table = [] {
            std::array<u8, 256> sb{};
            // multiplicative inverse in GF(2^8) via log/antilog tables, then the affine transform
            u8 p = 1, q = 1;
            do {
                p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));  // p *= 3
                q ^= static_cast<u8>(q << 1);                                   // q /= 3
                q ^= static_cast<u8>(q << 2);
                q ^= static_cast<u8>(q << 4);
                if (q & 0x80) q ^= 0x09;
                const u8 x = static_cast<u8>(q ^ std::rotl(q, 1) ^ std::rotl(q, 2) ^ std::rotl(q, 3) ^ std::rotl(q, 4));
                sb[p] = static_cast<u8>(x ^ 0x63);
            } while (p != 1);
            sb[0] = 0x63;
            return sb;
        }();
        return table;
    }
    u8 rk_[240];
    int rounds_ = 10;
};

class AesGcm {
public:
    static constexpr usize tag_size = 16, nonce_size = 12;
    explicit AesGcm(ByteView key) : aes_(key) {
        u8 zero[16] = {};
        u8 h[16];
        aes_.encrypt_block(zero, h);
        h_hi_ = load64(h);
        h_lo_ = load64(h + 8);
    }

    /// Returns ciphertext || 16-byte tag.
    [[nodiscard]] Bytes seal(ByteView nonce, ByteView aad, ByteView plaintext) const {
        check_nonce(nonce);
        Bytes out(plaintext.size() + tag_size);
        u8 j0[16];
        make_j0(nonce, j0);
        ctr_xor(j0, plaintext, reinterpret_cast<u8*>(out.data()));
        u8 tag[16];
        compute_tag(j0, aad, ByteView(out.data(), plaintext.size()), tag);
        std::memcpy(out.data() + plaintext.size(), tag, 16);
        return out;
    }
    /// Verifies and decrypts `ciphertext || tag`. Returns nullopt if authentication fails.
    [[nodiscard]] std::optional<Bytes> open(ByteView nonce, ByteView aad, ByteView sealed) const {
        check_nonce(nonce);
        if (sealed.size() < tag_size) return std::nullopt;
        const ByteView ct = sealed.first(sealed.size() - tag_size);
        u8 j0[16];
        make_j0(nonce, j0);
        u8 tag[16];
        compute_tag(j0, aad, ct, tag);
        if (!detail::tag_equal(tag, reinterpret_cast<const u8*>(sealed.data() + ct.size()), 16)) return std::nullopt;
        Bytes out(ct.size());
        ctr_xor(j0, ct, reinterpret_cast<u8*>(out.data()));
        return out;
    }

private:
    static void check_nonce(ByteView n) {
        if (n.size() != nonce_size) throw DataException("AES-GCM nonce must be 12 bytes");
    }
    static u64 load64(const u8* p) noexcept {
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
        return v;
    }
    static void store64(u8* p, u64 v) noexcept {
        for (int i = 7; i >= 0; --i) p[i] = static_cast<u8>(v), v >>= 8;
    }
    static void make_j0(ByteView nonce, u8 j0[16]) noexcept {
        std::memcpy(j0, nonce.data(), 12);
        j0[12] = j0[13] = j0[14] = 0;
        j0[15] = 1;
    }
    static void inc32(u8 ctr[16]) noexcept {
        for (int i = 15; i >= 12; --i)
            if (++ctr[i] != 0) break;
    }
    void ctr_xor(const u8 j0[16], ByteView in, u8* out) const noexcept {
        u8 ctr[16];
        std::memcpy(ctr, j0, 16);
        for (usize off = 0; off < in.size(); off += 16) {
            inc32(ctr);
            u8 ks[16];
            aes_.encrypt_block(ctr, ks);
            const usize n = std::min<usize>(16, in.size() - off);
            for (usize i = 0; i < n; ++i) out[off + i] = static_cast<u8>(static_cast<u8>(in[off + i]) ^ ks[i]);
        }
    }
    /// (xh, xl) = (xh, xl) * H in GF(2^128), constant time.
    void gmul(u64& xh, u64& xl) const noexcept {
        u64 zh = 0, zl = 0, vh = h_hi_, vl = h_lo_;
        for (int i = 0; i < 128; ++i) {
            const u64 bit = i < 64 ? (xh >> (63 - i)) & 1 : (xl >> (127 - i)) & 1;
            const u64 m = 0 - bit;
            zh ^= vh & m;
            zl ^= vl & m;
            const u64 lsb = vl & 1;
            vl = (vl >> 1) | (vh << 63);
            vh = (vh >> 1) ^ (0xE100000000000000ull & (0 - lsb));
        }
        xh = zh;
        xl = zl;
    }
    void ghash_update(u64& yh, u64& yl, ByteView data) const noexcept {
        for (usize off = 0; off < data.size(); off += 16) {
            u8 blk[16] = {};
            std::memcpy(blk, data.data() + off, std::min<usize>(16, data.size() - off));
            yh ^= load64(blk);
            yl ^= load64(blk + 8);
            gmul(yh, yl);
        }
    }
    void compute_tag(const u8 j0[16], ByteView aad, ByteView ct, u8 tag[16]) const noexcept {
        u64 yh = 0, yl = 0;
        ghash_update(yh, yl, aad);
        ghash_update(yh, yl, ct);
        yh ^= static_cast<u64>(aad.size()) * 8;
        yl ^= static_cast<u64>(ct.size()) * 8;
        gmul(yh, yl);
        u8 s[16], e[16];
        store64(s, yh);
        store64(s + 8, yl);
        aes_.encrypt_block(j0, e);
        for (int i = 0; i < 16; ++i) tag[i] = static_cast<u8>(s[i] ^ e[i]);
    }
    Aes aes_;
    u64 h_hi_ = 0, h_lo_ = 0;
};

// ---- ChaCha20-Poly1305 (RFC 8439) ----------------------------------------------------------------------------------

namespace detail {
inline void chacha_block(const u32 key[8], u32 counter, const u32 nonce[3], u8 out[64]) noexcept {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) s[4 + i] = key[i];
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = nonce[i];
    u32 x[16];
    std::memcpy(x, s, sizeof x);
    auto qr = [&](int a, int b, int c, int d) {
        x[a] += x[b], x[d] = std::rotl(x[d] ^ x[a], 16);
        x[c] += x[d], x[b] = std::rotl(x[b] ^ x[c], 12);
        x[a] += x[b], x[d] = std::rotl(x[d] ^ x[a], 8);
        x[c] += x[d], x[b] = std::rotl(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        qr(0, 4, 8, 12), qr(1, 5, 9, 13), qr(2, 6, 10, 14), qr(3, 7, 11, 15);
        qr(0, 5, 10, 15), qr(1, 6, 11, 12), qr(2, 7, 8, 13), qr(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) put_le32(out + 4 * i, x[i] + s[i]);
}

class Poly1305 {
public:
    explicit Poly1305(const u8 key[32]) noexcept {
        const u32 t0 = le32(key), t1 = le32(key + 4), t2 = le32(key + 8), t3 = le32(key + 12);
        r_[0] = t0 & 0x3ffffff;
        r_[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
        r_[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
        r_[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
        r_[4] = (t3 >> 8) & 0x00fffff;
        for (int i = 0; i < 4; ++i) pad_[i] = le32(key + 16 + 4 * i);
    }
    void update(const u8* m, usize len) noexcept {
        while (len) {
            const usize take = std::min<usize>(16 - fill_, len);
            std::memcpy(buf_ + fill_, m, take);
            fill_ += take, m += take, len -= take;
            if (fill_ == 16) {
                block(buf_, 1u << 24);
                fill_ = 0;
            }
        }
    }
    /// Pads the data absorbed so far with zeros to a 16-byte boundary (for the AEAD construction).
    void pad16() noexcept {
        if (fill_) {
            std::memset(buf_ + fill_, 0, 16 - fill_);
            block(buf_, 1u << 24);
            fill_ = 0;
        }
    }
    void finish(u8 tag[16]) noexcept {
        if (fill_) {
            buf_[fill_] = 1;
            std::memset(buf_ + fill_ + 1, 0, 15 - fill_);
            block(buf_, 0);
            fill_ = 0;
        }
        u32 h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
        u32 c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
        c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
        c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
        c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
        u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
        u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
        u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
        u32 g4 = h4 + c - (1u << 26);
        u32 mask = (g4 >> 31) - 1;
        g0 &= mask, g1 &= mask, g2 &= mask, g3 &= mask, g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0, h1 = (h1 & mask) | g1, h2 = (h2 & mask) | g2, h3 = (h3 & mask) | g3, h4 = (h4 & mask) | g4;
        h0 = (h0 | (h1 << 26));
        h1 = ((h1 >> 6) | (h2 << 20));
        h2 = ((h2 >> 12) | (h3 << 14));
        h3 = ((h3 >> 18) | (h4 << 8));
        u64 f = static_cast<u64>(h0) + pad_[0];
        put_le32(tag, static_cast<u32>(f));
        f = static_cast<u64>(h1) + pad_[1] + (f >> 32);
        put_le32(tag + 4, static_cast<u32>(f));
        f = static_cast<u64>(h2) + pad_[2] + (f >> 32);
        put_le32(tag + 8, static_cast<u32>(f));
        f = static_cast<u64>(h3) + pad_[3] + (f >> 32);
        put_le32(tag + 12, static_cast<u32>(f));
    }

private:
    void block(const u8* m, u32 hibit) noexcept {
        const u32 r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3], r4 = r_[4];
        const u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        const u32 t0 = le32(m), t1 = le32(m + 4), t2 = le32(m + 8), t3 = le32(m + 12);
        u32 h0 = h_[0] + (t0 & 0x3ffffff);
        u32 h1 = h_[1] + (((t0 >> 26) | (t1 << 6)) & 0x3ffffff);
        u32 h2 = h_[2] + (((t1 >> 20) | (t2 << 12)) & 0x3ffffff);
        u32 h3 = h_[3] + (((t2 >> 14) | (t3 << 18)) & 0x3ffffff);
        u32 h4 = h_[4] + ((t3 >> 8) | hibit);
        const u64 d0 = static_cast<u64>(h0) * r0 + static_cast<u64>(h1) * s4 + static_cast<u64>(h2) * s3 + static_cast<u64>(h3) * s2 + static_cast<u64>(h4) * s1;
        u64 d1 = static_cast<u64>(h0) * r1 + static_cast<u64>(h1) * r0 + static_cast<u64>(h2) * s4 + static_cast<u64>(h3) * s3 + static_cast<u64>(h4) * s2;
        u64 d2 = static_cast<u64>(h0) * r2 + static_cast<u64>(h1) * r1 + static_cast<u64>(h2) * r0 + static_cast<u64>(h3) * s4 + static_cast<u64>(h4) * s3;
        u64 d3 = static_cast<u64>(h0) * r3 + static_cast<u64>(h1) * r2 + static_cast<u64>(h2) * r1 + static_cast<u64>(h3) * r0 + static_cast<u64>(h4) * s4;
        u64 d4 = static_cast<u64>(h0) * r4 + static_cast<u64>(h1) * r3 + static_cast<u64>(h2) * r2 + static_cast<u64>(h3) * r1 + static_cast<u64>(h4) * r0;
        u32 c = static_cast<u32>(d0 >> 26);
        h0 = static_cast<u32>(d0) & 0x3ffffff;
        d1 += c; c = static_cast<u32>(d1 >> 26); h1 = static_cast<u32>(d1) & 0x3ffffff;
        d2 += c; c = static_cast<u32>(d2 >> 26); h2 = static_cast<u32>(d2) & 0x3ffffff;
        d3 += c; c = static_cast<u32>(d3 >> 26); h3 = static_cast<u32>(d3) & 0x3ffffff;
        d4 += c; c = static_cast<u32>(d4 >> 26); h4 = static_cast<u32>(d4) & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        h_[0] = h0, h_[1] = h1, h_[2] = h2, h_[3] = h3, h_[4] = h4;
    }
    u32 r_[5], h_[5] = {0, 0, 0, 0, 0}, pad_[4];
    u8 buf_[16];
    usize fill_ = 0;
};
}  // namespace detail

class ChaCha20Poly1305 {
public:
    static constexpr usize key_size = 32, nonce_size = 12, tag_size = 16;
    explicit ChaCha20Poly1305(ByteView key) {
        if (key.size() != key_size) throw DataException("ChaCha20-Poly1305 key must be 32 bytes");
        for (int i = 0; i < 8; ++i) key_[i] = detail::le32(reinterpret_cast<const u8*>(key.data()) + 4 * i);
    }
    [[nodiscard]] Bytes seal(ByteView nonce, ByteView aad, ByteView plaintext) const {
        check(nonce);
        Bytes out(plaintext.size() + tag_size);
        xor_stream(nonce, plaintext, reinterpret_cast<u8*>(out.data()));
        u8 tag[16];
        mac(nonce, aad, ByteView(out.data(), plaintext.size()), tag);
        std::memcpy(out.data() + plaintext.size(), tag, 16);
        return out;
    }
    [[nodiscard]] std::optional<Bytes> open(ByteView nonce, ByteView aad, ByteView sealed) const {
        check(nonce);
        if (sealed.size() < tag_size) return std::nullopt;
        const ByteView ct = sealed.first(sealed.size() - tag_size);
        u8 tag[16];
        mac(nonce, aad, ct, tag);
        if (!detail::tag_equal(tag, reinterpret_cast<const u8*>(sealed.data() + ct.size()), 16)) return std::nullopt;
        Bytes out(ct.size());
        xor_stream(nonce, ct, reinterpret_cast<u8*>(out.data()));
        return out;
    }

private:
    static void check(ByteView n) {
        if (n.size() != nonce_size) throw DataException("ChaCha20-Poly1305 nonce must be 12 bytes");
    }
    void xor_stream(ByteView nonce, ByteView in, u8* out) const noexcept {
        u32 n[3];
        for (int i = 0; i < 3; ++i) n[i] = detail::le32(reinterpret_cast<const u8*>(nonce.data()) + 4 * i);
        u32 counter = 1;
        for (usize off = 0; off < in.size(); off += 64, ++counter) {
            u8 ks[64];
            detail::chacha_block(key_, counter, n, ks);
            const usize len = std::min<usize>(64, in.size() - off);
            for (usize i = 0; i < len; ++i) out[off + i] = static_cast<u8>(static_cast<u8>(in[off + i]) ^ ks[i]);
        }
    }
    void mac(ByteView nonce, ByteView aad, ByteView ct, u8 tag[16]) const noexcept {
        u32 n[3];
        for (int i = 0; i < 3; ++i) n[i] = detail::le32(reinterpret_cast<const u8*>(nonce.data()) + 4 * i);
        u8 block0[64];
        detail::chacha_block(key_, 0, n, block0);
        detail::Poly1305 p(block0);
        p.update(reinterpret_cast<const u8*>(aad.data()), aad.size());
        p.pad16();
        p.update(reinterpret_cast<const u8*>(ct.data()), ct.size());
        p.pad16();
        u8 lens[16];
        for (int i = 0; i < 8; ++i) {
            lens[i] = static_cast<u8>(static_cast<u64>(aad.size()) >> (8 * i));
            lens[8 + i] = static_cast<u8>(static_cast<u64>(ct.size()) >> (8 * i));
        }
        p.update(lens, 16);
        p.finish(tag);
    }
    u32 key_[8];
};

}  // namespace flyg::net::tls
