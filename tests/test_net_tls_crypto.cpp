// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/tls/aead.hpp>
#include <flyg/net/tls/ec.hpp>
#include <flyg/net/tls/rsa.hpp>
#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net::tls;

static Bytes H(std::string_view s) { return data::hex_decode(s); }
static std::string X(ByteView b) { return data::hex_encode(b); }
static Bytes S(std::string_view s) { Bytes b(s.size()); std::memcpy(b.data(), s.data(), s.size()); return b; }

FLYG_TEST(tls_bigint_basic) {
    BigInt a = BigInt::from_hex("ffffffffffffffffffffffffffffffff");
    BigInt b = BigInt::from_hex("123456789abcdef0123456789abcdef");
    CHECK_EQ(((a * b) / b).to_hex(), a.to_hex());
    CHECK_EQ(((a * b + BigInt(7)) % b).to_hex(), std::string("07"));
    CHECK_EQ((a - a).is_zero(), true);
    CHECK_EQ(BigInt::pow_mod(BigInt(4), BigInt(13), BigInt(497)).to_hex(), std::string("01bd"));
    CHECK_EQ(BigInt::inv_mod(BigInt(3), BigInt(11)).to_hex(), std::string("04"));
    CHECK_EQ(((BigInt(1) << 100) >> 98).to_hex(), std::string("04"));
}

FLYG_TEST(tls_curves_valid) {
    for (const Curve* c : {&Curve::p256(), &Curve::p384()}) {
        CHECK(c->on_curve(c->generator()));
        CHECK(c->multiply(c->n, c->generator()).infinity);
        CHECK(!c->multiply(c->n - BigInt(1), c->generator()).infinity);
        const auto kp = c->generate_key();
        CHECK(c->on_curve(c->decode_point(c->encode_point(kp.q))));
        const auto kp2 = c->generate_key();
        CHECK(c->ecdh(kp.d, kp2.q) == c->ecdh(kp2.d, kp.q));
        const Bytes h = hash_data(HashAlg::sha256, S("hello"));
        const auto sig = c->ecdsa_sign_der<data::Sha256>(h, kp.d);
        CHECK(c->ecdsa_verify_der(h, sig, kp.q));
        const Bytes h2 = hash_data(HashAlg::sha256, S("hellO"));
        CHECK(!c->ecdsa_verify_der(h2, sig, kp.q));
    }
}

FLYG_TEST(tls_ecdsa_rfc6979_p256) {
    const auto& c = Curve::p256();
    const BigInt d = BigInt::from_hex("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721");
    const Bytes h = hash_data(HashAlg::sha256, S("sample"));
    const auto [r, s] = c.sign_rfc6979<data::Sha256>(h, c.hash_to_int(h), d);
    CHECK_EQ(r.to_hex(), std::string("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"));
    CHECK_EQ(s.to_hex(), std::string("f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"));
}

FLYG_TEST(tls_x25519_rfc7748) {
    auto a32 = [](std::string_view s) { std::array<std::byte, 32> o; auto b = H(s); std::copy(b.begin(), b.end(), o.begin()); return o; };
    const auto out = x25519::scalar_mult(a32("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                                         a32("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"));
    CHECK_EQ(X(out), std::string("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
    const auto ap = a32("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const auto bp = a32("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const auto pa = x25519::public_key(ap), pb = x25519::public_key(bp);
    CHECK_EQ(X(pa), std::string("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK_EQ(X(x25519::shared_secret(ap, pb)), std::string("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    CHECK(x25519::shared_secret(ap, pb) == x25519::shared_secret(bp, pa));
}

#include "tls_vectors.inc"

FLYG_TEST(tls_aes_gcm_known) {
    AesGcm g0(H("00000000000000000000000000000000"));
    CHECK_EQ(X(g0.seal(H("000000000000000000000000"), {}, {})), std::string("58e2fccefa7e3061367f1d57a4e7455a"));
    CHECK_EQ(X(g0.seal(H("000000000000000000000000"), {}, H("00000000000000000000000000000000"))),
             std::string("0388dace60b6a392f328c2b971b2fe78ab6e47d42cec13bdf53a67b21257bddf"));
}

FLYG_TEST(tls_aes_gcm_vs_reference) {
    for (const auto& v : gcm_vecs) {
        AesGcm g(H(v.key));
        const Bytes sealed = g.seal(H(v.nonce), H(v.aad), H(v.pt));
        CHECK_EQ(X(sealed), std::string(v.sealed));
        auto opened = g.open(H(v.nonce), H(v.aad), H(v.sealed));
        CHECK(opened.has_value() && X(*opened) == v.pt);
        Bytes bad = H(v.sealed);
        bad.back() ^= std::byte{1};
        CHECK(!g.open(H(v.nonce), H(v.aad), bad).has_value());
    }
}

FLYG_TEST(tls_chacha20poly1305_vs_reference) {
    for (const auto& v : chacha_vecs) {
        ChaCha20Poly1305 c(H(v.key));
        CHECK_EQ(X(c.seal(H(v.nonce), H(v.aad), H(v.pt))), std::string(v.sealed));
        auto o = c.open(H(v.nonce), H(v.aad), H(v.sealed));
        CHECK(o.has_value() && X(*o) == v.pt);
        Bytes bad = H(v.sealed);
        bad[0] ^= std::byte{0x80};
        CHECK(!c.open(H(v.nonce), H(v.aad), bad).has_value());
    }
}

FLYG_TEST(tls_rsa_vs_reference) {
    const auto pub = rsa::parse_public_pkcs1(H(rsa_pub));
    const auto priv = rsa::parse_private_pkcs1(H(rsa_priv));
    CHECK(pub.n == priv.pub.n);
    const Bytes msg = S("flyg rsa test");
    for (auto [alg, p1, pss] : {std::tuple{HashAlg::sha256, rsa_pkcs1_sha256, rsa_pss_sha256}, std::tuple{HashAlg::sha384, rsa_pkcs1_sha384, rsa_pss_sha384}}) {
        const Bytes d = hash_data(alg, msg);
        CHECK(rsa::verify_pkcs1(pub, alg, d, H(p1)));
        CHECK(rsa::verify_pss(pub, alg, d, H(pss), hash_size(alg)));
        CHECK(!rsa::verify_pkcs1(pub, alg, d, H(pss)));
        CHECK(!rsa::verify_pss(pub, alg, d, H(p1), hash_size(alg)));
        CHECK_EQ(X(rsa::sign_pkcs1(priv, alg, d)), std::string(p1));  // PKCS#1 v1.5 is deterministic
        const Bytes sp = rsa::sign_pss(priv, alg, d);
        CHECK(rsa::verify_pss(pub, alg, d, sp, hash_size(alg)));
        Bytes other = d;
        other[0] ^= std::byte{1};
        CHECK(!rsa::verify_pkcs1(pub, alg, other, H(p1)));
    }
}

FLYG_TEST(tls_ecdsa_vs_reference) {
    const Bytes msg = S("flyg rsa test");
    const auto& c256 = Curve::p256();
    CHECK(c256.ecdsa_verify_der(hash_data(HashAlg::sha256, msg), H(ec_p256_sig), c256.decode_point(H(ec_p256_pub))));
    const auto& c384 = Curve::p384();
    CHECK(c384.ecdsa_verify_der(hash_data(HashAlg::sha384, msg), H(ec_p384_sig), c384.decode_point(H(ec_p384_pub))));
    CHECK(!c384.ecdsa_verify_der(hash_data(HashAlg::sha256, msg), H(ec_p384_sig), c384.decode_point(H(ec_p384_pub))));
}
