// flyG - net/tls: TLS 1.3 (client + server) and TLS 1.2 (client) over any net::ByteStream
// SPDX-License-Identifier: GPL-2.0-only
//
// Implemented: RFC 8446 (TLS 1.3: HelloRetryRequest, KeyUpdate, ALPN, SNI, certificate verification),
// RFC 5246/5288/7905/7627 subset for the TLS 1.2 client (ECDHE + AEAD suites, extended master secret
// required). Not implemented: session resumption/PSK/0-RTT, client certificates, renegotiation,
// TLS 1.2 server, FFDHE/RSA key exchange, compression, DTLS, OCSP stapling.
// Security notes: big-number and AES S-box code is not constant time (see README); server-side
// signing is therefore not hardened against a co-resident timing attacker.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

#include <flyg/net/stream.hpp>
#include <flyg/net/tls/aead.hpp>
#include <flyg/net/tls/keys.hpp>
#include <flyg/net/tls/x509.hpp>

namespace flyg::net::tls {

struct TlsClientConfig {
    std::string server_name;                ///< SNI and certificate host name
    const TrustStore* trust = nullptr;      ///< null = system roots
    bool verify = true;                     ///< false disables ALL certificate checks (testing only)
    std::vector<std::string> alpn;          ///< offered application protocols
    bool allow_tls12 = true;
    core::i64 now = -1;                     ///< override "current time" (Unix seconds) for validation
};

struct TlsServerConfig {
    std::vector<Bytes> chain;               ///< leaf first, DER
    PrivateKey key;
    std::vector<std::string> alpn;          ///< supported protocols in preference order

    /// Loads a PEM certificate chain and a PEM private key.
    [[nodiscard]] static TlsServerConfig from_pem(std::string_view cert_chain_pem, std::string_view key_pem) {
        TlsServerConfig c;
        for (auto& b : asn1::parse_pem(cert_chain_pem))
            if (b.label == "CERTIFICATE") c.chain.push_back(std::move(b.der));
        if (c.chain.empty()) throw TlsException("no certificate in PEM data");
        c.key = PrivateKey::from_pem(key_pem);
        const Certificate leaf = Certificate::parse(c.chain[0]);
        const bool match = (leaf.key.kind == PublicKey::Kind::rsa && c.key.kind == PrivateKey::Kind::rsa && leaf.key.rsa.n == c.key.rsa.pub.n) ||
                           (leaf.key.kind == PublicKey::Kind::ec && c.key.kind == PrivateKey::Kind::ec && leaf.key.curve == c.key.curve &&
                            Curve::by_id(c.key.curve)->multiply(c.key.ec_d, Curve::by_id(c.key.curve)->generator()).x == leaf.key.ec.x);
        if (!match) throw TlsException("private key does not match the certificate");
        return c;
    }
};

namespace detail {

class ProtocolError : public TlsException {
public:
    ProtocolError(u8 alert_code, const std::string& what) : TlsException(what), alert(alert_code) {}
    u8 alert;
};
[[noreturn]] inline void fail(u8 alert, const std::string& what) { throw ProtocolError(alert, what); }

namespace alert {
inline constexpr u8 close_notify = 0, unexpected_message = 10, bad_record_mac = 20, record_overflow = 22, handshake_failure = 40, bad_certificate = 42,
                    illegal_parameter = 47, decode_error = 50, decrypt_error = 51, protocol_version = 70, internal_error = 80, missing_extension = 109,
                    no_application_protocol = 120;
}

enum : u8 { ct_ccs = 20, ct_alert = 21, ct_handshake = 22, ct_appdata = 23 };
enum : u8 {
    hs_client_hello = 1, hs_server_hello = 2, hs_new_session_ticket = 4, hs_end_of_early_data = 5, hs_encrypted_extensions = 8, hs_certificate = 11,
    hs_server_key_exchange = 12, hs_certificate_request = 13, hs_server_hello_done = 14, hs_certificate_verify = 15, hs_client_key_exchange = 16,
    hs_finished = 20, hs_key_update = 24, hs_message_hash = 254
};
enum : core::u16 {
    ext_server_name = 0, ext_supported_groups = 10, ext_ec_point_formats = 11, ext_sig_algs = 13, ext_alpn = 16, ext_ems = 23, ext_supported_versions = 43,
    ext_cookie = 44, ext_key_share = 51, ext_renegotiation_info = 0xff01
};

/// Big-endian reader that throws decode_error on overrun.
class Rd {
public:
    Rd() = default;
    explicit Rd(ByteView d) : d_(d) {}
    [[nodiscard]] bool empty() const noexcept { return p_ >= d_.size(); }
    [[nodiscard]] usize left() const noexcept { return d_.size() - p_; }
    u8 u8_() { need(1); return static_cast<u8>(d_[p_++]); }
    core::u16 u16_() { need(2); const auto v = static_cast<core::u16>((static_cast<u8>(d_[p_]) << 8) | static_cast<u8>(d_[p_ + 1])); p_ += 2; return v; }
    u32 u24_() { need(3); const u32 v = (static_cast<u32>(static_cast<u8>(d_[p_])) << 16) | (static_cast<u32>(static_cast<u8>(d_[p_ + 1])) << 8) | static_cast<u8>(d_[p_ + 2]); p_ += 3; return v; }
    ByteView bytes(usize n) { need(n); auto v = d_.subspan(p_, n); p_ += n; return v; }
    Rd sub8() { return Rd(bytes(u8_())); }
    Rd sub16() { return Rd(bytes(u16_())); }
    Rd sub24() { return Rd(bytes(u24_())); }
    ByteView rest() { return bytes(left()); }

private:
    void need(usize n) const {
        if (left() < n) fail(alert::decode_error, "malformed message");
    }
    ByteView d_;
    usize p_ = 0;
};

class Wr {
public:
    Bytes b;
    Wr& u8_(u8 v) { b.push_back(static_cast<std::byte>(v)); return *this; }
    Wr& u16_(core::u16 v) { u8_(static_cast<u8>(v >> 8)); return u8_(static_cast<u8>(v)); }
    Wr& u24_(u32 v) { u8_(static_cast<u8>(v >> 16)); return u16_(static_cast<core::u16>(v)); }
    Wr& bytes(ByteView v) { b.insert(b.end(), v.begin(), v.end()); return *this; }
    Wr& str(std::string_view s) { return bytes(std::as_bytes(std::span(s.data(), s.size()))); }
    template <class F> Wr& vec8(F&& f) { const usize at = b.size(); u8_(0); f(*this); b[at] = static_cast<std::byte>(b.size() - at - 1); return *this; }
    template <class F> Wr& vec16(F&& f) {
        const usize at = b.size();
        u16_(0);
        f(*this);
        const usize n = b.size() - at - 2;
        b[at] = static_cast<std::byte>(n >> 8);
        b[at + 1] = static_cast<std::byte>(n);
        return *this;
    }
    template <class F> Wr& vec24(F&& f) {
        const usize at = b.size();
        u24_(0);
        f(*this);
        const usize n = b.size() - at - 3;
        b[at] = static_cast<std::byte>(n >> 16);
        b[at + 1] = static_cast<std::byte>(n >> 8);
        b[at + 2] = static_cast<std::byte>(n);
        return *this;
    }
    template <class F> Wr& ext(core::u16 type, F&& f) { u16_(type); return vec16(f); }
};

[[nodiscard]] inline Bytes hmac_bytes(HashAlg h, ByteView key, ByteView msg) {
    if (h == HashAlg::sha384) { auto d = data::hmac<data::Sha384>(key, msg); return Bytes(d.begin(), d.end()); }
    auto d = data::hmac<data::Sha256>(key, msg);
    return Bytes(d.begin(), d.end());
}
[[nodiscard]] inline Bytes hkdf_extract_bytes(HashAlg h, ByteView salt, ByteView ikm) {
    if (h == HashAlg::sha384) { auto d = data::hkdf_extract<data::Sha384>(salt, ikm); return Bytes(d.begin(), d.end()); }
    auto d = data::hkdf_extract<data::Sha256>(salt, ikm);
    return Bytes(d.begin(), d.end());
}
[[nodiscard]] inline Bytes hkdf_expand_label(HashAlg h, ByteView secret, std::string_view label, ByteView context, usize len) {
    Wr info;
    info.u16_(static_cast<core::u16>(len));
    info.vec8([&](Wr& w) { w.str("tls13 ").str(label); });
    info.vec8([&](Wr& w) { w.bytes(context); });
    return h == HashAlg::sha384 ? data::hkdf_expand<data::Sha384>(secret, info.b, len) : data::hkdf_expand<data::Sha256>(secret, info.b, len);
}
[[nodiscard]] inline Bytes prf12(HashAlg h, ByteView secret, std::string_view label, ByteView seed, usize len) {
    Bytes ls;
    for (char c : label) ls.push_back(static_cast<std::byte>(c));
    ls.insert(ls.end(), seed.begin(), seed.end());
    Bytes out, a = ls;
    while (out.size() < len) {
        a = hmac_bytes(h, secret, a);
        Bytes in = a;
        in.insert(in.end(), ls.begin(), ls.end());
        const Bytes blk = hmac_bytes(h, secret, in);
        out.insert(out.end(), blk.begin(), blk.end());
    }
    out.resize(len);
    return out;
}

struct Suite {
    core::u16 id;
    const char* name;
    HashAlg hash;
    usize key_len;
    bool chacha, tls13, ecdsa;  ///< `ecdsa` only meaningful for TLS 1.2 suites
};
inline constexpr Suite suites[] = {
    {0x1301, "TLS_AES_128_GCM_SHA256", HashAlg::sha256, 16, false, true, false},
    {0x1303, "TLS_CHACHA20_POLY1305_SHA256", HashAlg::sha256, 32, true, true, false},
    {0x1302, "TLS_AES_256_GCM_SHA384", HashAlg::sha384, 32, false, true, false},
    {0xC02B, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", HashAlg::sha256, 16, false, false, true},
    {0xCCA9, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256", HashAlg::sha256, 32, true, false, true},
    {0xC02C, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", HashAlg::sha384, 32, false, false, true},
    {0xC02F, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256", HashAlg::sha256, 16, false, false, false},
    {0xCCA8, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256", HashAlg::sha256, 32, true, false, false},
    {0xC030, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", HashAlg::sha384, 32, false, false, false},
};
[[nodiscard]] inline const Suite* find_suite(core::u16 id) {
    for (const auto& s : suites)
        if (s.id == id) return &s;
    return nullptr;
}

class AeadCipher {
public:
    AeadCipher() = default;
    AeadCipher(const Suite& s, ByteView key) {
        if (s.chacha) c_.emplace<ChaCha20Poly1305>(key);
        else c_.emplace<AesGcm>(key);
    }
    [[nodiscard]] bool active() const noexcept { return c_.index() != 0; }
    [[nodiscard]] Bytes seal(ByteView nonce, ByteView aad, ByteView pt) const {
        return std::visit([&](const auto& c) -> Bytes { if constexpr (std::is_same_v<std::decay_t<decltype(c)>, std::monostate>) return {}; else return c.seal(nonce, aad, pt); }, c_);
    }
    [[nodiscard]] std::optional<Bytes> open(ByteView nonce, ByteView aad, ByteView ct) const {
        return std::visit([&](const auto& c) -> std::optional<Bytes> { if constexpr (std::is_same_v<std::decay_t<decltype(c)>, std::monostate>) return std::nullopt; else return c.open(nonce, aad, ct); }, c_);
    }

private:
    std::variant<std::monostate, AesGcm, ChaCha20Poly1305> c_;
};

// Signature schemes (TLS 1.3 numbering).
inline constexpr core::u16 sig_rsa_pkcs1_sha256 = 0x0401, sig_rsa_pkcs1_sha384 = 0x0501, sig_rsa_pkcs1_sha512 = 0x0601, sig_ecdsa_p256_sha256 = 0x0403,
                           sig_ecdsa_p384_sha384 = 0x0503, sig_rsa_pss_sha256 = 0x0804, sig_rsa_pss_sha384 = 0x0805, sig_rsa_pss_sha512 = 0x0806;

[[nodiscard]] inline bool verify_signature(core::u16 scheme, const PublicKey& key, ByteView content, ByteView sig, bool tls13) {
    const auto rsa_pss = [&](HashAlg h) { return key.kind == PublicKey::Kind::rsa && rsa::verify_pss(key.rsa, h, hash_data(h, content), sig, hash_size(h)); };
    const auto rsa_pk = [&](HashAlg h) { return key.kind == PublicKey::Kind::rsa && rsa::verify_pkcs1(key.rsa, h, hash_data(h, content), sig); };
    switch (scheme) {
        case sig_rsa_pss_sha256: return rsa_pss(HashAlg::sha256);
        case sig_rsa_pss_sha384: return rsa_pss(HashAlg::sha384);
        case sig_rsa_pss_sha512: return rsa_pss(HashAlg::sha512);
        case sig_rsa_pkcs1_sha256: return !tls13 && rsa_pk(HashAlg::sha256);
        case sig_rsa_pkcs1_sha384: return !tls13 && rsa_pk(HashAlg::sha384);
        case sig_rsa_pkcs1_sha512: return !tls13 && rsa_pk(HashAlg::sha512);
        case sig_ecdsa_p256_sha256:
            return key.kind == PublicKey::Kind::ec && key.curve == CurveId::secp256r1 && Curve::p256().ecdsa_verify_der(hash_data(HashAlg::sha256, content), sig, key.ec);
        case sig_ecdsa_p384_sha384:
            return key.kind == PublicKey::Kind::ec && key.curve == CurveId::secp384r1 && Curve::p384().ecdsa_verify_der(hash_data(HashAlg::sha384, content), sig, key.ec);
        default: return false;
    }
}
[[nodiscard]] inline bool scheme_ok_for_tls13(core::u16 s) { return s == sig_rsa_pss_sha256 || s == sig_rsa_pss_sha384 || s == sig_rsa_pss_sha512 || s == sig_ecdsa_p256_sha256 || s == sig_ecdsa_p384_sha384; }

/// An ephemeral key-exchange key for one of the supported groups.
struct KeyShare {
    core::u16 group = 0;
    Bytes priv;      ///< x25519 scalar, or big-endian EC scalar
    Bytes pub;       ///< wire encoding of our public value
    [[nodiscard]] static KeyShare generate(core::u16 group) {
        KeyShare k;
        k.group = group;
        if (group == static_cast<core::u16>(CurveId::x25519)) {
            std::array<std::byte, 32> p{};
            system::secure_random(p);
            const auto pub = x25519::public_key(p);
            k.priv.assign(p.begin(), p.end());
            k.pub.assign(pub.begin(), pub.end());
        } else if (const Curve* c = Curve::by_id(static_cast<CurveId>(group))) {
            const auto kp = c->generate_key();
            k.priv = kp.d.to_bytes(c->size);
            k.pub = c->encode_point(kp.q);
        } else {
            fail(alert::internal_error, "unsupported group");
        }
        return k;
    }
    /// Shared secret with the peer's public value (validates the peer value).
    [[nodiscard]] Bytes agree(ByteView peer) const {
        if (group == static_cast<core::u16>(CurveId::x25519)) {
            if (peer.size() != 32 || priv.size() != 32) fail(alert::illegal_parameter, "bad x25519 share");
            std::array<std::byte, 32> a{}, b{};
            std::copy(priv.begin(), priv.end(), a.begin());
            std::copy(peer.begin(), peer.end(), b.begin());
            const auto s = x25519::shared_secret(a, b);
            std::byte acc{0};
            for (auto v : s) acc |= v;
            if (acc == std::byte{0}) fail(alert::illegal_parameter, "x25519 low-order point");
            return Bytes(s.begin(), s.end());
        }
        const Curve* c = Curve::by_id(static_cast<CurveId>(group));
        if (!c) fail(alert::internal_error, "unsupported group");
        try {
            return c->ecdh(BigInt::from_bytes(priv), c->decode_point(peer));
        } catch (const DataException&) {
            fail(alert::illegal_parameter, "invalid EC public value");
        }
    }
};

}  // namespace detail
}  // namespace flyg::net::tls
