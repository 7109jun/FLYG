// flyG - net/tls: NIST P-256 / P-384 (ECDSA, ECDH) and X25519
// SPDX-License-Identifier: GPL-2.0-only
//
// Weierstrass arithmetic uses the generic BigInt (not constant time; see bigint.hpp). X25519
// uses a constant-time Montgomery ladder (TweetNaCl-style field arithmetic).
#pragma once

#include <flyg/data/hash.hpp>
#include <flyg/net/tls/asn1.hpp>
#include <flyg/system/random.hpp>

namespace flyg::net::tls {

enum class CurveId : core::u16 { secp256r1 = 23, secp384r1 = 24, x25519 = 29 };

class Curve {
public:
    BigInt p, b, n, gx, gy;
    usize size = 0;  ///< bytes per coordinate / scalar
    std::string name;

    [[nodiscard]] static const Curve& p256() {
        static const Curve c = make("P-256", 32, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
                                    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                                    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
        return c;
    }
    [[nodiscard]] static const Curve& p384() {
        static const Curve c = make("P-384", 48, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
                                    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
                                    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
                                    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
                                    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f");
        return c;
    }
    [[nodiscard]] static const Curve* by_id(CurveId id) {
        if (id == CurveId::secp256r1) return &p256();
        if (id == CurveId::secp384r1) return &p384();
        return nullptr;
    }

    struct Point {
        BigInt x, y;
        bool infinity = true;
    };

    [[nodiscard]] bool on_curve(const Point& pt) const {
        if (pt.infinity) return false;
        if (pt.x >= p || pt.y >= p) return false;
        const BigInt lhs = BigInt::mul_mod(pt.y, pt.y, p);
        BigInt rhs = BigInt::mul_mod(BigInt::mul_mod(pt.x, pt.x, p), pt.x, p);
        rhs = BigInt::sub_mod(rhs, BigInt::mul_mod(BigInt(3), pt.x, p), p);
        rhs = BigInt::add_mod(rhs, b, p);
        return lhs == rhs;
    }
    [[nodiscard]] Point generator() const { return Point{gx, gy, false}; }

    /// k * P (double-and-add; k < n expected).
    [[nodiscard]] Point multiply(const BigInt& k, const Point& pt) const {
        Jac acc;  // infinity
        const Jac base = to_jac(pt);
        for (usize i = k.bit_length(); i-- > 0;) {
            acc = dbl(acc);
            if (k.bit(i)) acc = add(acc, base);
        }
        return to_affine(acc);
    }
    /// u1*G + u2*Q
    [[nodiscard]] Point multiply_add(const BigInt& u1, const BigInt& u2, const Point& q) const {
        const Jac g = to_jac(generator()), qj = to_jac(q), gq = add(g, qj);
        Jac acc;
        const usize bits = std::max(u1.bit_length(), u2.bit_length());
        for (usize i = bits; i-- > 0;) {
            acc = dbl(acc);
            const bool a = u1.bit(i), c = u2.bit(i);
            if (a && c) acc = add(acc, gq);
            else if (a) acc = add(acc, g);
            else if (c) acc = add(acc, qj);
        }
        return to_affine(acc);
    }

    /// Uncompressed SEC1 point (0x04 || X || Y). Validates that the point is on the curve.
    [[nodiscard]] Point decode_point(std::span<const std::byte> d) const {
        if (d.size() != 1 + 2 * size || d[0] != std::byte{4}) throw DataException(name + ": only uncompressed EC points are supported");
        Point pt{BigInt::from_bytes(d.subspan(1, size)), BigInt::from_bytes(d.subspan(1 + size, size)), false};
        if (!on_curve(pt)) throw DataException(name + ": point is not on the curve");
        return pt;
    }
    [[nodiscard]] std::vector<std::byte> encode_point(const Point& pt) const {
        std::vector<std::byte> out{std::byte{4}};
        for (auto byte_ : pt.x.to_bytes(size)) out.push_back(byte_);
        for (auto byte_ : pt.y.to_bytes(size)) out.push_back(byte_);
        return out;
    }

    struct KeyPair {
        BigInt d;
        Point q;
    };
    [[nodiscard]] KeyPair generate_key() const {
        for (;;) {
            std::vector<std::byte> r(size);
            system::secure_random(r);
            BigInt d = BigInt::from_bytes(r);
            if (d.is_zero() || d >= n) continue;
            return {d, multiply(d, generator())};
        }
    }
    /// ECDH: x coordinate of d*Q, `size` bytes. Throws if Q is invalid.
    [[nodiscard]] std::vector<std::byte> ecdh(const BigInt& d, const Point& q) const {
        if (!on_curve(q)) throw DataException(name + ": invalid peer public key");
        const Point s = multiply(d, q);
        if (s.infinity) throw DataException(name + ": ECDH result is the point at infinity");
        return s.x.to_bytes(size);
    }

    /// Converts a message hash to the integer used by ECDSA (leftmost bits, FIPS 186-4 6.4).
    [[nodiscard]] BigInt hash_to_int(std::span<const std::byte> h) const {
        BigInt e = BigInt::from_bytes(h);
        const usize hb = h.size() * 8, nb = n.bit_length();
        if (hb > nb) e = e >> (hb - nb);
        return e;
    }

    [[nodiscard]] bool ecdsa_verify(std::span<const std::byte> hash, const BigInt& r, const BigInt& s, const Point& q) const {
        if (!on_curve(q)) return false;
        if (r.is_zero() || s.is_zero() || r >= n || s >= n) return false;
        const BigInt e = hash_to_int(hash);
        const BigInt w = BigInt::inv_mod(s, n);
        const BigInt u1 = BigInt::mul_mod(e % n, w, n), u2 = BigInt::mul_mod(r, w, n);
        const Point pt = multiply_add(u1, u2, q);
        if (pt.infinity) return false;
        return pt.x % n == r;
    }
    /// Verifies an ASN.1 DER ECDSA-Sig-Value (SEQUENCE { r INTEGER, s INTEGER }).
    [[nodiscard]] bool ecdsa_verify_der(std::span<const std::byte> hash, std::span<const std::byte> sig, const Point& q) const {
        try {
            asn1::Reader top(sig);
            const auto seq = top.expect(asn1::tag::sequence);
            if (!top.at_end()) return false;
            auto in = asn1::open(seq);
            const BigInt r = asn1::to_integer(in.next()), s = asn1::to_integer(in.next());
            if (!in.at_end()) return false;
            return ecdsa_verify(hash, r, s, q);
        } catch (const DataException&) {
            return false;
        }
    }

    /// Deterministic ECDSA (RFC 6979) with HMAC-`H`. Returns DER-encoded signature.
    template <class H>
    [[nodiscard]] std::vector<std::byte> ecdsa_sign_der(std::span<const std::byte> hash, const BigInt& d) const {
        const BigInt e = hash_to_int(hash) % n;
        const auto [r, s] = sign_rfc6979<H>(hash, e, d);
        const auto ri = asn1::make_integer(r), si = asn1::make_integer(s);
        return asn1::make(asn1::tag::sequence, asn1::concat({ri, si}));
    }
    template <class H>
    [[nodiscard]] std::pair<BigInt, BigInt> sign_rfc6979(std::span<const std::byte> hash, const BigInt& e, const BigInt& d) const {
        const usize qlen = n.bit_length();
        auto bits2octets = [&](const BigInt& v) { return (v % n).to_bytes(size); };
        // RFC 6979 section 3.2
        std::vector<std::byte> V(H::digest_size, std::byte{1}), K(H::digest_size, std::byte{0});
        const auto x = d.to_bytes(size);
        const auto h1 = bits2octets(hash_to_int(hash));
        auto hmac_k = [&](std::initializer_list<std::span<const std::byte>> parts) {
            data::Hmac<H> m(K);
            for (auto part : parts) m.update(part);
            const auto dgst = m.finish();
            return std::vector<std::byte>(dgst.begin(), dgst.end());
        };
        const std::byte z0{0}, z1{1};
        K = hmac_k({V, std::span<const std::byte>(&z0, 1), x, h1});
        V = hmac_k({V});
        K = hmac_k({V, std::span<const std::byte>(&z1, 1), x, h1});
        V = hmac_k({V});
        for (;;) {
            std::vector<std::byte> T;
            while (T.size() * 8 < qlen) {
                V = hmac_k({V});
                T.insert(T.end(), V.begin(), V.end());
            }
            BigInt k = BigInt::from_bytes(T);
            if (T.size() * 8 > qlen) k = k >> (T.size() * 8 - qlen);
            if (!k.is_zero() && k < n) {
                const Point kg = multiply(k, generator());
                const BigInt r = kg.x % n;
                if (!r.is_zero()) {
                    const BigInt s = BigInt::mul_mod(BigInt::inv_mod(k, n), BigInt::add_mod(e, BigInt::mul_mod(r, d, n), n), n);
                    if (!s.is_zero()) return {r, s};
                }
            }
            K = hmac_k({V, std::span<const std::byte>(&z0, 1)});
            V = hmac_k({V});
        }
    }

private:
    struct Jac {  // Jacobian coordinates; z == 0 means infinity
        BigInt x, y, z;
    };
    [[nodiscard]] static Curve make(const char* name, usize size, const char* p, const char* b, const char* n, const char* gx, const char* gy) {
        Curve c;
        c.name = name;
        c.size = size;
        c.p = BigInt::from_hex(p);
        c.b = BigInt::from_hex(b);
        c.n = BigInt::from_hex(n);
        c.gx = BigInt::from_hex(gx);
        c.gy = BigInt::from_hex(gy);
        return c;
    }
    [[nodiscard]] Jac to_jac(const Point& pt) const { return pt.infinity ? Jac{} : Jac{pt.x, pt.y, BigInt(1)}; }
    [[nodiscard]] Point to_affine(const Jac& j) const {
        if (j.z.is_zero()) return {};
        const BigInt zi = BigInt::inv_mod(j.z, p), zi2 = BigInt::mul_mod(zi, zi, p), zi3 = BigInt::mul_mod(zi2, zi, p);
        return Point{BigInt::mul_mod(j.x, zi2, p), BigInt::mul_mod(j.y, zi3, p), false};
    }
    [[nodiscard]] BigInt mm(const BigInt& a, const BigInt& c) const { return BigInt::mul_mod(a, c, p); }
    [[nodiscard]] BigInt sb(const BigInt& a, const BigInt& c) const { return BigInt::sub_mod(a, c, p); }
    [[nodiscard]] BigInt ad(const BigInt& a, const BigInt& c) const { return BigInt::add_mod(a, c, p); }
    [[nodiscard]] Jac dbl(const Jac& a) const {  // a = -3 curves (dbl-2001-b)
        if (a.z.is_zero() || a.y.is_zero()) return {};
        const BigInt delta = mm(a.z, a.z), gamma = mm(a.y, a.y), beta = mm(a.x, gamma);
        const BigInt alpha3 = mm(BigInt(3), mm(sb(a.x, delta), ad(a.x, delta)));
        const BigInt beta8 = mm(BigInt(8), beta);
        Jac r;
        r.x = sb(mm(alpha3, alpha3), beta8);
        const BigInt yz = ad(a.y, a.z);
        r.z = sb(sb(mm(yz, yz), gamma), delta);
        r.y = sb(mm(alpha3, sb(mm(BigInt(4), beta), r.x)), mm(BigInt(8), mm(gamma, gamma)));
        return r;
    }
    [[nodiscard]] Jac add(const Jac& a, const Jac& c) const {
        if (a.z.is_zero()) return c;
        if (c.z.is_zero()) return a;
        const BigInt z1z1 = mm(a.z, a.z), z2z2 = mm(c.z, c.z);
        const BigInt u1 = mm(a.x, z2z2), u2 = mm(c.x, z1z1);
        const BigInt s1 = mm(a.y, mm(c.z, z2z2)), s2 = mm(c.y, mm(a.z, z1z1));
        if (u1 == u2) return s1 == s2 ? dbl(a) : Jac{};
        const BigInt h = sb(u2, u1), r = sb(s2, s1);
        const BigInt hh = mm(h, h), hhh = mm(h, hh), v = mm(u1, hh);
        Jac o;
        o.x = sb(sb(mm(r, r), hhh), ad(v, v));
        o.y = sb(mm(r, sb(v, o.x)), mm(s1, hhh));
        o.z = mm(mm(a.z, c.z), h);
        return o;
    }
};

// ---- X25519 (RFC 7748) -------------------------------------------------------------------------------------

namespace x25519 {
namespace detail {
using gf = core::i64[16];
inline void car(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1ll << 16);
        const core::i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
inline void sel(gf p, gf q, core::i64 b) {
    const core::i64 c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        const core::i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
inline void pack(u8* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car(t), car(t), car(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const core::i64 b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = static_cast<u8>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<u8>(t[i] >> 8);
    }
}
inline void unpack(gf o, const u8* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<core::i64>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}
inline void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
inline void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
inline void M(gf o, const gf a, const gf b) {
    core::i64 t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car(o), car(o);
}
inline void S(gf o, const gf a) { M(o, a, a); }
inline void inv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}
}  // namespace detail

/// Scalar multiplication on Curve25519. `point` is a 32-byte u-coordinate.
[[nodiscard]] inline std::array<std::byte, 32> scalar_mult(std::span<const std::byte, 32> scalar, std::span<const std::byte, 32> point) {
    using namespace detail;
    u8 z[32];
    for (int i = 0; i < 32; ++i) z[i] = static_cast<u8>(scalar[static_cast<usize>(i)]);
    z[31] = static_cast<u8>((z[31] & 127) | 64);
    z[0] &= 248;
    u8 pt[32];
    for (int i = 0; i < 32; ++i) pt[i] = static_cast<u8>(point[static_cast<usize>(i)]);
    core::i64 x[80];
    gf a, b, c, d, e, f;
    unpack(x, pt);
    static const gf k121665 = {0xDB41, 1};
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const core::i64 r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r);
        sel(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, k121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel(a, b, r);
        sel(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    inv(x + 32, x + 32);
    M(x + 16, x + 16, x + 32);
    u8 out[32];
    pack(out, x + 16);
    std::array<std::byte, 32> r;
    for (int i = 0; i < 32; ++i) r[static_cast<usize>(i)] = static_cast<std::byte>(out[i]);
    return r;
}

[[nodiscard]] inline std::array<std::byte, 32> base_point() {
    std::array<std::byte, 32> b{};
    b[0] = std::byte{9};
    return b;
}
[[nodiscard]] inline std::array<std::byte, 32> public_key(std::span<const std::byte, 32> priv) { return scalar_mult(priv, base_point()); }
/// Shared secret; throws if the result is all zero (low-order peer key, RFC 7748 section 6.1).
[[nodiscard]] inline std::array<std::byte, 32> shared_secret(std::span<const std::byte, 32> priv, std::span<const std::byte, 32> peer) {
    const auto s = scalar_mult(priv, peer);
    unsigned acc = 0;
    for (auto b : s) acc |= static_cast<unsigned>(b);
    if (acc == 0) throw DataException("X25519: peer public key has small order");
    return s;
}

}  // namespace x25519
}  // namespace flyg::net::tls
