// flyG - data module: SHA-1, SHA-256, SHA-384, SHA-512, HMAC and HKDF
// SPDX-License-Identifier: GPL-2.0-only
//
// Straightforward, portable implementations of FIPS 180-4 / RFC 2104 / RFC 5869. SHA-1 is only
// provided for protocols that still mandate it (WebSocket handshake, legacy certificates); do
// not use it for new security designs. These are not hardened against power/cache side
// channels beyond avoiding secret-dependent branches and table lookups.
#pragma once

#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::data {

using core::u32;
using core::u64;
using core::u8;
using core::usize;

namespace detail {
template <usize N>
using Digest = std::array<std::byte, N>;

inline u32 load_be32(const std::byte* p) noexcept {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}
inline u64 load_be64(const std::byte* p) noexcept { return (static_cast<u64>(load_be32(p)) << 32) | load_be32(p + 4); }
inline void store_be32(std::byte* p, u32 v) noexcept {
    p[0] = static_cast<std::byte>(v >> 24), p[1] = static_cast<std::byte>(v >> 16), p[2] = static_cast<std::byte>(v >> 8), p[3] = static_cast<std::byte>(v);
}
inline void store_be64(std::byte* p, u64 v) noexcept {
    store_be32(p, static_cast<u32>(v >> 32));
    store_be32(p + 4, static_cast<u32>(v));
}
}  // namespace detail

/// Block-buffering base for the Merkle–Damgård hashes. `Derived` supplies `compress(block)`.
template <class Derived, usize BlockSize, usize DigestSize>
class HashBase {
public:
    static constexpr usize block_size = BlockSize;
    static constexpr usize digest_size = DigestSize;
    using digest_type = detail::Digest<DigestSize>;

    Derived& update(std::span<const std::byte> data) noexcept {
        total_ += data.size();
        if (fill_) {
            const usize take = std::min(BlockSize - fill_, data.size());
            std::memcpy(buf_ + fill_, data.data(), take);
            fill_ += take;
            data = data.subspan(take);
            if (fill_ == BlockSize) {
                self().compress(buf_);
                fill_ = 0;
            }
        }
        while (data.size() >= BlockSize) {
            self().compress(data.data());
            data = data.subspan(BlockSize);
        }
        if (!data.empty()) {
            std::memcpy(buf_, data.data(), data.size());
            fill_ = data.size();
        }
        return self();
    }
    Derived& update(std::string_view s) noexcept { return update(std::as_bytes(std::span(s.data(), s.size()))); }

    /// Finishes the computation. The object must not be used afterwards.
    [[nodiscard]] digest_type finish() noexcept {
        constexpr usize len_bytes = BlockSize == 128 ? 16 : 8;
        const u64 bits = total_ * 8;
        std::byte pad[BlockSize * 2] = {};
        pad[0] = std::byte{0x80};
        const usize rem = static_cast<usize>(total_ % BlockSize);
        const usize padlen = (rem < BlockSize - len_bytes ? BlockSize - len_bytes - rem : 2 * BlockSize - len_bytes - rem);
        std::byte lenb[16] = {};
        detail::store_be64(lenb + (len_bytes - 8), bits);
        update(std::span<const std::byte>(pad, padlen));
        update(std::span<const std::byte>(lenb, len_bytes));
        return self().output();
    }

protected:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }
    std::byte buf_[BlockSize] = {};
    usize fill_ = 0;
    u64 total_ = 0;
};

class Sha1 : public HashBase<Sha1, 64, 20> {
public:
    Sha1() noexcept = default;
    void compress(const std::byte* p) noexcept {
        u32 w[80];
        for (int i = 0; i < 16; ++i) w[i] = detail::load_be32(p + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const u32 t = std::rotl(a, 5) + f + e + k + w[i];
            e = d, d = c, c = std::rotl(b, 30), b = a, a = t;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
    }
    digest_type output() noexcept {
        digest_type d;
        for (int i = 0; i < 5; ++i) detail::store_be32(d.data() + 4 * i, h_[i]);
        return d;
    }

private:
    u32 h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
};

class Sha256 : public HashBase<Sha256, 64, 32> {
public:
    Sha256() noexcept = default;
    void compress(const std::byte* p) noexcept {
        static constexpr u32 K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
            0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
            0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
            0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = detail::load_be32(p + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const u32 S1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = h + S1 + ch + K[i] + w[i];
            const u32 S0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + maj;
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }
    digest_type output() noexcept {
        digest_type d;
        for (int i = 0; i < 8; ++i) detail::store_be32(d.data() + 4 * i, h_[i]);
        return d;
    }

private:
    u32 h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

namespace detail {
template <class Derived, usize DigestSize>
class Sha512Core : public HashBase<Derived, 128, DigestSize> {
public:
    void compress(const std::byte* p) noexcept {
        static constexpr u64 K[80] = {
            0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
            0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
            0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
            0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
            0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
            0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
            0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
            0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec, 0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
            0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
            0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};
        u64 w[80];
        for (int i = 0; i < 16; ++i) w[i] = load_be64(p + 8 * i);
        for (int i = 16; i < 80; ++i) {
            const u64 s0 = std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const u64 s1 = std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u64 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 80; ++i) {
            const u64 S1 = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
            const u64 ch = (e & f) ^ (~e & g);
            const u64 t1 = h + S1 + ch + K[i] + w[i];
            const u64 S0 = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
            const u64 maj = (a & b) ^ (a & c) ^ (b & c);
            const u64 t2 = S0 + maj;
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }
    typename HashBase<Derived, 128, DigestSize>::digest_type output() noexcept {
        std::byte full[64];
        for (int i = 0; i < 8; ++i) store_be64(full + 8 * i, h_[i]);
        typename HashBase<Derived, 128, DigestSize>::digest_type d;
        std::memcpy(d.data(), full, DigestSize);
        return d;
    }

protected:
    explicit Sha512Core(const u64 (&iv)[8]) noexcept {
        for (int i = 0; i < 8; ++i) h_[i] = iv[i];
    }
    u64 h_[8];
};
}  // namespace detail

class Sha512 : public detail::Sha512Core<Sha512, 64> {
    static constexpr u64 iv[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};

public:
    Sha512() noexcept : Sha512Core(iv) {}
};

class Sha384 : public detail::Sha512Core<Sha384, 48> {
    static constexpr u64 iv[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939, 0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};

public:
    Sha384() noexcept : Sha512Core(iv) {}
};

template <class H>
[[nodiscard]] typename H::digest_type hash(std::span<const std::byte> data) noexcept {
    H h;
    h.update(data);
    return h.finish();
}
template <class H>
[[nodiscard]] typename H::digest_type hash(std::string_view s) noexcept {
    return hash<H>(std::as_bytes(std::span(s.data(), s.size())));
}
[[nodiscard]] inline auto sha1(std::span<const std::byte> d) noexcept { return hash<Sha1>(d); }
[[nodiscard]] inline auto sha1(std::string_view d) noexcept { return hash<Sha1>(d); }
[[nodiscard]] inline auto sha256(std::span<const std::byte> d) noexcept { return hash<Sha256>(d); }
[[nodiscard]] inline auto sha256(std::string_view d) noexcept { return hash<Sha256>(d); }
[[nodiscard]] inline auto sha384(std::span<const std::byte> d) noexcept { return hash<Sha384>(d); }
[[nodiscard]] inline auto sha384(std::string_view d) noexcept { return hash<Sha384>(d); }
[[nodiscard]] inline auto sha512(std::span<const std::byte> d) noexcept { return hash<Sha512>(d); }
[[nodiscard]] inline auto sha512(std::string_view d) noexcept { return hash<Sha512>(d); }

/// Incremental HMAC (RFC 2104) over any hash from this header.
template <class H>
class Hmac {
public:
    explicit Hmac(std::span<const std::byte> key) {
        std::byte k[H::block_size] = {};
        if (key.size() > H::block_size) {
            const auto d = hash<H>(key);
            std::memcpy(k, d.data(), d.size());
        } else if (!key.empty()) {
            std::memcpy(k, key.data(), key.size());
        }
        std::byte ipad[H::block_size];
        for (usize i = 0; i < H::block_size; ++i) {
            ipad[i] = k[i] ^ std::byte{0x36};
            opad_[i] = k[i] ^ std::byte{0x5c};
        }
        inner_.update(std::span<const std::byte>(ipad, H::block_size));
    }
    Hmac& update(std::span<const std::byte> d) noexcept {
        inner_.update(d);
        return *this;
    }
    Hmac& update(std::string_view s) noexcept {
        inner_.update(s);
        return *this;
    }
    [[nodiscard]] typename H::digest_type finish() noexcept {
        const auto ih = inner_.finish();
        H outer;
        outer.update(std::span<const std::byte>(opad_, H::block_size));
        outer.update(std::span<const std::byte>(ih));
        return outer.finish();
    }

private:
    H inner_;
    std::byte opad_[H::block_size];
};

template <class H>
[[nodiscard]] typename H::digest_type hmac(std::span<const std::byte> key, std::span<const std::byte> data) noexcept {
    Hmac<H> m(key);
    m.update(data);
    return m.finish();
}

/// HKDF (RFC 5869).
template <class H>
[[nodiscard]] typename H::digest_type hkdf_extract(std::span<const std::byte> salt, std::span<const std::byte> ikm) {
    if (salt.empty()) {
        const std::byte zeros[H::digest_size] = {};
        return hmac<H>(std::span<const std::byte>(zeros, H::digest_size), ikm);
    }
    return hmac<H>(salt, ikm);
}
template <class H>
[[nodiscard]] std::vector<std::byte> hkdf_expand(std::span<const std::byte> prk, std::span<const std::byte> info, usize length) {
    if (length > 255 * H::digest_size) throw DataException("hkdf_expand: requested output is too long");
    std::vector<std::byte> out;
    out.reserve(length);
    typename H::digest_type t{};
    usize tlen = 0;
    for (u8 counter = 1; out.size() < length; ++counter) {
        Hmac<H> m(prk);
        m.update(std::span<const std::byte>(t.data(), tlen));
        m.update(info);
        m.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&counter), 1));
        t = m.finish();
        tlen = t.size();
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(std::min(tlen, length - out.size())));
    }
    return out;
}

/// Constant-time comparison of two byte ranges.
[[nodiscard]] inline bool constant_time_equal(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (usize i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace flyg::data
