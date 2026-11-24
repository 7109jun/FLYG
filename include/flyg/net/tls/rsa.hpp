// flyG - net/tls: RSA signatures (PKCS#1 v1.5 and PSS verification, PSS and PKCS#1 signing)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <flyg/data/encoding.hpp>
#include <flyg/data/hash.hpp>
#include <flyg/net/tls/aead.hpp>
#include <flyg/net/tls/asn1.hpp>
#include <flyg/net/tls/bigint.hpp>
#include <flyg/system/random.hpp>

namespace flyg::net::tls {

enum class HashAlg { sha1, sha256, sha384, sha512 };

[[nodiscard]] inline usize hash_size(HashAlg h) noexcept {
    switch (h) {
        case HashAlg::sha1: return 20;
        case HashAlg::sha256: return 32;
        case HashAlg::sha384: return 48;
        case HashAlg::sha512: return 64;
    }
    return 0;
}
[[nodiscard]] inline Bytes hash_data(HashAlg h, ByteView d) {
    auto conv = [](const auto& a) { return Bytes(a.begin(), a.end()); };
    switch (h) {
        case HashAlg::sha1: return conv(data::sha1(d));
        case HashAlg::sha256: return conv(data::sha256(d));
        case HashAlg::sha384: return conv(data::sha384(d));
        case HashAlg::sha512: return conv(data::sha512(d));
    }
    return {};
}

struct RsaPublicKey {
    BigInt n, e;
    [[nodiscard]] usize size() const { return (n.bit_length() + 7) / 8; }
};

struct RsaPrivateKey {
    RsaPublicKey pub;
    BigInt d, p, q, dp, dq, qinv;  ///< CRT parameters (may be zero when only d is known)

    /// m^d mod n using the CRT when available.
    [[nodiscard]] BigInt apply(const BigInt& m) const {
        if (p.is_zero() || q.is_zero()) return BigInt::pow_mod(m, d, pub.n);
        const BigInt m1 = BigInt::pow_mod(m % p, dp, p), m2 = BigInt::pow_mod(m % q, dq, q);
        const BigInt h = BigInt::mul_mod(qinv, BigInt::sub_mod(m1, m2 % p, p), p);
        return m2 + h * q;
    }
};

namespace rsa {

inline Bytes mgf1(HashAlg h, ByteView seed, usize len) {
    Bytes out;
    for (u32 counter = 0; out.size() < len; ++counter) {
        Bytes in(seed.begin(), seed.end());
        for (int i = 3; i >= 0; --i) in.push_back(static_cast<std::byte>(counter >> (8 * i)));
        const Bytes d = hash_data(h, in);
        out.insert(out.end(), d.begin(), d.end());
    }
    out.resize(len);
    return out;
}

inline ByteView digest_info_prefix(HashAlg h) {
    static const Bytes sha1 = data::hex_decode("3021300906052b0e03021a05000414");
    static const Bytes sha256 = data::hex_decode("3031300d060960864801650304020105000420");
    static const Bytes sha384 = data::hex_decode("3041300d060960864801650304020205000430");
    static const Bytes sha512 = data::hex_decode("3051300d060960864801650304020305000440");
    switch (h) {
        case HashAlg::sha1: return sha1;
        case HashAlg::sha256: return sha256;
        case HashAlg::sha384: return sha384;
        case HashAlg::sha512: return sha512;
    }
    return {};
}

/// EMSA-PKCS1-v1_5 verification: signature over the already computed `digest`.
[[nodiscard]] inline bool verify_pkcs1(const RsaPublicKey& key, HashAlg h, ByteView digest, ByteView sig) {
    const usize k = key.size();
    if (sig.size() != k || k < 64 || digest.size() != hash_size(h)) return false;
    const BigInt s = BigInt::from_bytes(sig);
    if (s >= key.n) return false;
    const Bytes em = BigInt::pow_mod(s, key.e, key.n).to_bytes(k);
    // 0x00 0x01 FF..FF 0x00 DigestInfo
    const auto prefix = digest_info_prefix(h);
    const usize tlen = prefix.size() + digest.size();
    if (k < tlen + 11) return false;
    Bytes expect(k, std::byte{0xff});
    expect[0] = std::byte{0};
    expect[1] = std::byte{1};
    expect[k - tlen - 1] = std::byte{0};
    std::memcpy(expect.data() + k - tlen, prefix.data(), prefix.size());
    std::memcpy(expect.data() + k - digest.size(), digest.data(), digest.size());
    return data::constant_time_equal(em, expect);
}

[[nodiscard]] inline Bytes sign_pkcs1(const RsaPrivateKey& key, HashAlg h, ByteView digest) {
    const usize k = key.pub.size();
    const auto prefix = digest_info_prefix(h);
    const usize tlen = prefix.size() + digest.size();
    if (k < tlen + 11) throw DataException("RSA key too small for this hash");
    Bytes em(k, std::byte{0xff});
    em[0] = std::byte{0};
    em[1] = std::byte{1};
    em[k - tlen - 1] = std::byte{0};
    std::memcpy(em.data() + k - tlen, prefix.data(), prefix.size());
    std::memcpy(em.data() + k - digest.size(), digest.data(), digest.size());
    return key.apply(BigInt::from_bytes(em)).to_bytes(k);
}

/// RSASSA-PSS verification (MGF1 with the same hash, salt length = digest length as TLS 1.3 requires).
[[nodiscard]] inline bool verify_pss(const RsaPublicKey& key, HashAlg h, ByteView digest, ByteView sig, usize salt_len) {
    const usize k = key.size(), hlen = hash_size(h);
    if (sig.size() != k || digest.size() != hlen) return false;
    const BigInt s = BigInt::from_bytes(sig);
    if (s >= key.n) return false;
    const usize em_bits = key.n.bit_length() - 1;
    const usize em_len = (em_bits + 7) / 8;
    Bytes em = BigInt::pow_mod(s, key.e, key.n).to_bytes(k);
    em.erase(em.begin(), em.begin() + static_cast<std::ptrdiff_t>(k - em_len));
    if (em_len < hlen + salt_len + 2 || em.back() != std::byte{0xbc}) return false;
    const usize db_len = em_len - hlen - 1;
    const ByteView masked_db(em.data(), db_len), H(em.data() + db_len, hlen);
    const unsigned top_bits = static_cast<unsigned>(8 * em_len - em_bits);
    if (top_bits && (static_cast<u8>(masked_db[0]) >> (8 - top_bits)) != 0) return false;
    Bytes db = mgf1(h, H, db_len);
    for (usize i = 0; i < db_len; ++i) db[i] ^= masked_db[i];
    if (top_bits) db[0] &= static_cast<std::byte>(0xFF >> top_bits);
    const usize ps_len = em_len - hlen - salt_len - 2;
    for (usize i = 0; i < ps_len; ++i)
        if (db[i] != std::byte{0}) return false;
    if (db[ps_len] != std::byte{1}) return false;
    Bytes mprime(8, std::byte{0});
    mprime.insert(mprime.end(), digest.begin(), digest.end());
    mprime.insert(mprime.end(), db.begin() + static_cast<std::ptrdiff_t>(ps_len + 1), db.end());
    return data::constant_time_equal(hash_data(h, mprime), H);
}

[[nodiscard]] inline Bytes sign_pss(const RsaPrivateKey& key, HashAlg h, ByteView digest) {
    const usize k = key.pub.size(), hlen = hash_size(h), salt_len = hlen;
    const usize em_bits = key.pub.n.bit_length() - 1;
    const usize em_len = (em_bits + 7) / 8;
    if (em_len < hlen + salt_len + 2) throw DataException("RSA key too small for PSS with this hash");
    Bytes salt(salt_len);
    system::secure_random(salt);
    Bytes mprime(8, std::byte{0});
    mprime.insert(mprime.end(), digest.begin(), digest.end());
    mprime.insert(mprime.end(), salt.begin(), salt.end());
    const Bytes H = hash_data(h, mprime);
    const usize db_len = em_len - hlen - 1;
    Bytes db(db_len, std::byte{0});
    db[db_len - salt_len - 1] = std::byte{1};
    std::memcpy(db.data() + db_len - salt_len, salt.data(), salt_len);
    const Bytes mask = mgf1(h, H, db_len);
    for (usize i = 0; i < db_len; ++i) db[i] ^= mask[i];
    const unsigned top_bits = static_cast<unsigned>(8 * em_len - em_bits);
    if (top_bits) db[0] &= static_cast<std::byte>(0xFF >> top_bits);
    Bytes em = db;
    em.insert(em.end(), H.begin(), H.end());
    em.push_back(std::byte{0xbc});
    return key.apply(BigInt::from_bytes(em)).to_bytes(k);
}

/// Parses PKCS#1 RSAPublicKey (SEQUENCE { n, e }).
[[nodiscard]] inline RsaPublicKey parse_public_pkcs1(ByteView der) {
    asn1::Reader top(der);
    auto in = asn1::open(top.expect(asn1::tag::sequence));
    RsaPublicKey k;
    k.n = asn1::to_integer(in.next());
    k.e = asn1::to_integer(in.next());
    if (!in.at_end() || !top.at_end()) throw DataException("RSA: trailing data in public key");
    if (k.n.bit_length() < 512 || !k.n.is_odd() || k.e < BigInt(3) || !k.e.is_odd()) throw DataException("RSA: invalid public key");
    return k;
}

/// Parses PKCS#1 RSAPrivateKey.
[[nodiscard]] inline RsaPrivateKey parse_private_pkcs1(ByteView der) {
    asn1::Reader top(der);
    auto in = asn1::open(top.expect(asn1::tag::sequence));
    (void)asn1::to_integer(in.next());  // version
    RsaPrivateKey k;
    k.pub.n = asn1::to_integer(in.next());
    k.pub.e = asn1::to_integer(in.next());
    k.d = asn1::to_integer(in.next());
    k.p = asn1::to_integer(in.next());
    k.q = asn1::to_integer(in.next());
    k.dp = asn1::to_integer(in.next());
    k.dq = asn1::to_integer(in.next());
    k.qinv = asn1::to_integer(in.next());
    return k;
}

}  // namespace rsa
}  // namespace flyg::net::tls
