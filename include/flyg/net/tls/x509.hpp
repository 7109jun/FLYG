// flyG - net/tls: X.509 certificates, chain validation, host name matching, trust stores
// SPDX-License-Identifier: GPL-2.0-only
//
// Scope: RFC 5280 path validation for TLS server authentication (signature, validity period,
// issuer/subject chaining by exact DER name match, basicConstraints/pathLen, keyUsage, EKU).
// Not implemented: name constraints, policy processing, revocation (CRL/OCSP), Ed25519/Ed448 keys,
// DSA, SHA-1 signatures (rejected on purpose), RSA keys below 2048 bits in chains (rejected).
#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <flyg/net/address.hpp>
#include <flyg/net/tls/aead.hpp>
#include <flyg/net/tls/ec.hpp>
#include <flyg/net/tls/rsa.hpp>
#include <flyg/system/filesystem.hpp>
#include <flyg/system/time.hpp>

#ifdef _WIN32
#include <flyg/net/native.hpp>
#include <wincrypt.h>
#endif

namespace flyg::net::tls {

/// Any TLS failure (handshake, protocol violation, alert).
class TlsException : public NetworkException {
public:
    explicit TlsException(const std::string& what) : NetworkException("TLS: " + what) {}
};
/// The peer certificate could not be validated.
class CertificateException : public TlsException {
public:
    explicit CertificateException(const std::string& what) : TlsException("certificate: " + what) {}
};

namespace oid {
inline constexpr const char* rsa_encryption = "1.2.840.113549.1.1.1";
inline constexpr const char* rsa_pss = "1.2.840.113549.1.1.10";
inline constexpr const char* sha256_rsa = "1.2.840.113549.1.1.11";
inline constexpr const char* sha384_rsa = "1.2.840.113549.1.1.12";
inline constexpr const char* sha512_rsa = "1.2.840.113549.1.1.13";
inline constexpr const char* sha1_rsa = "1.2.840.113549.1.1.5";
inline constexpr const char* ec_public_key = "1.2.840.10045.2.1";
inline constexpr const char* prime256v1 = "1.2.840.10045.3.1.7";
inline constexpr const char* secp384r1 = "1.3.132.0.34";
inline constexpr const char* ecdsa_sha256 = "1.2.840.10045.4.3.2";
inline constexpr const char* ecdsa_sha384 = "1.2.840.10045.4.3.3";
inline constexpr const char* ecdsa_sha512 = "1.2.840.10045.4.3.4";
inline constexpr const char* sha256 = "2.16.840.1.101.3.4.2.1";
inline constexpr const char* sha384 = "2.16.840.1.101.3.4.2.2";
inline constexpr const char* sha512 = "2.16.840.1.101.3.4.2.3";
inline constexpr const char* mgf1 = "1.2.840.113549.1.1.8";
inline constexpr const char* ext_san = "2.5.29.17";
inline constexpr const char* ext_basic = "2.5.29.19";
inline constexpr const char* ext_key_usage = "2.5.29.15";
inline constexpr const char* ext_eku = "2.5.29.37";
inline constexpr const char* eku_server_auth = "1.3.6.1.5.5.7.3.1";
inline constexpr const char* eku_any = "2.5.29.37.0";
inline constexpr const char* cn = "2.5.4.3";
}  // namespace oid

/// Signature scheme of a certificate (how its issuer signed it).
struct SigAlg {
    enum class Kind { rsa_pkcs1, rsa_pss, ecdsa } kind = Kind::rsa_pkcs1;
    HashAlg hash = HashAlg::sha256;
    usize pss_salt = 0;
};

struct PublicKey {
    enum class Kind { none, rsa, ec } kind = Kind::none;
    RsaPublicKey rsa;
    CurveId curve = CurveId::secp256r1;
    Curve::Point ec;
};

class Certificate {
public:
    Bytes der, tbs, signature, subject, issuer;
    SigAlg sig_alg;
    PublicKey key;
    core::i64 not_before = 0, not_after = 0;
    std::vector<std::string> dns_names;
    std::vector<Bytes> ip_names;  ///< 4 or 16 bytes each
    bool has_san = false, is_ca = false, has_basic = false, has_key_usage = false, has_eku = false;
    int path_len = -1;  ///< -1 = unlimited
    core::u16 key_usage = 0;  ///< bit i (MSB-first, as in RFC 5280) at position (15 - i) of this value
    std::vector<std::string> eku;

    static constexpr core::u16 ku_digital_signature = 0x8000, ku_key_cert_sign = 0x0400;

    [[nodiscard]] static Certificate parse(ByteView der_bytes) {
        Certificate c;
        c.der.assign(der_bytes.begin(), der_bytes.end());
        asn1::Reader top{ByteView(c.der)};
        const auto cert = top.expect(asn1::tag::sequence);
        if (!top.at_end()) throw CertificateException("trailing data after certificate");
        auto in = asn1::open(cert);
        const auto tbs = in.expect(asn1::tag::sequence);
        c.tbs.assign(tbs.whole.begin(), tbs.whole.end());
        const auto outer_alg = in.expect(asn1::tag::sequence);
        const auto sig = in.expect(asn1::tag::bit_string);
        if (!in.at_end()) throw CertificateException("trailing data in certificate");
        const auto sb = asn1::to_bits(sig);
        c.signature.assign(sb.begin(), sb.end());

        auto t = asn1::open(tbs);
        if (auto v = t.optional(asn1::tag::context(0))) {
            auto vr = asn1::open(*v);
            const auto ver = asn1::to_integer(vr.expect(asn1::tag::integer));
            if (ver != BigInt(2)) throw CertificateException("only X.509 v3 certificates are supported");
        } else {
            throw CertificateException("only X.509 v3 certificates are supported");
        }
        (void)t.expect(asn1::tag::integer);  // serial
        const auto inner_alg = t.expect(asn1::tag::sequence);
        if (!std::ranges::equal(inner_alg.whole, outer_alg.whole)) throw CertificateException("signature algorithm mismatch");
        c.sig_alg = parse_sig_alg(inner_alg);
        const auto issuer = t.expect(asn1::tag::sequence);
        c.issuer.assign(issuer.whole.begin(), issuer.whole.end());
        auto validity = asn1::open(t.expect(asn1::tag::sequence));
        c.not_before = asn1::to_time(validity.next());
        c.not_after = asn1::to_time(validity.next());
        const auto subject = t.expect(asn1::tag::sequence);
        c.subject.assign(subject.whole.begin(), subject.whole.end());
        c.key = parse_spki(t.expect(asn1::tag::sequence));
        (void)t.optional(asn1::tag::context(1, false));
        (void)t.optional(asn1::tag::context(2, false));
        if (auto ext = t.optional(asn1::tag::context(3))) {
            auto er = asn1::open(*ext);
            c.parse_extensions(er.expect(asn1::tag::sequence));
        }
        return c;
    }

    [[nodiscard]] bool is_self_issued() const { return subject == issuer; }

    /// Display name: the subject's common name if present, else "?".
    [[nodiscard]] std::string display_name() const {
        try {
            asn1::Reader r{ByteView(subject)};
            auto name = asn1::open(r.expect(asn1::tag::sequence));
            std::string cn = "?";
            while (!name.at_end()) {
                auto rdn = asn1::open(name.expect(asn1::tag::set));
                while (!rdn.at_end()) {
                    auto atv = asn1::open(rdn.expect(asn1::tag::sequence));
                    if (asn1::to_oid(atv.expect(asn1::tag::oid)) == oid::cn) cn = asn1::to_string_value(atv.next());
                }
            }
            return cn;
        } catch (const std::exception&) {
            return "?";
        }
    }

    /// Verifies that `issuer_cert` signed this certificate.
    [[nodiscard]] bool verify_signature_by(const Certificate& issuer_cert) const {
        const ByteView msg(tbs), sig(signature);
        const Bytes digest = hash_data(sig_alg.hash, msg);
        const PublicKey& k = issuer_cert.key;
        switch (sig_alg.kind) {
            case SigAlg::Kind::rsa_pkcs1:
                return k.kind == PublicKey::Kind::rsa && rsa::verify_pkcs1(k.rsa, sig_alg.hash, digest, sig);
            case SigAlg::Kind::rsa_pss:
                return k.kind == PublicKey::Kind::rsa && rsa::verify_pss(k.rsa, sig_alg.hash, digest, sig, sig_alg.pss_salt);
            case SigAlg::Kind::ecdsa: {
                if (k.kind != PublicKey::Kind::ec) return false;
                const Curve* c = Curve::by_id(k.curve);
                return c && c->ecdsa_verify_der(digest, sig, k.ec);
            }
        }
        return false;
    }

    [[nodiscard]] bool matches_host(std::string_view host) const {
        if (auto ip = IpAddress::parse(host)) {
            const usize n = ip->is_v4() ? 4 : 16;
            for (const auto& b : ip_names)
                if (b.size() == n && std::memcmp(b.data(), ip->data(), n) == 0) return true;
            return false;
        }
        std::string h(host);
        if (!h.empty() && h.back() == '.') h.pop_back();
        for (const auto& pattern : dns_names)
            if (dns_match(pattern, h)) return true;
        return false;
    }

private:
    static bool ieq(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
    /// RFC 6125 6.4.3: wildcard only as the entire left-most label, and only when at least two labels follow.
    static bool dns_match(std::string pattern, const std::string& host) {
        if (!pattern.empty() && pattern.back() == '.') pattern.pop_back();
        if (pattern.empty() || host.empty()) return false;
        if (pattern.rfind("*.", 0) != 0) return ieq(pattern, host);
        const std::string_view rest = std::string_view(pattern).substr(1);  // ".example.com"
        if (std::ranges::count(rest, '.') < 2) return false;                // refuse "*.com"
        const auto dot = host.find('.');
        if (dot == std::string::npos || dot == 0) return false;
        return ieq(rest, std::string_view(host).substr(dot));
    }

    static SigAlg parse_sig_alg(const asn1::Element& alg) {
        auto r = asn1::open(alg);
        const std::string o = asn1::to_oid(r.expect(asn1::tag::oid));
        SigAlg s;
        if (o == oid::sha256_rsa) return {SigAlg::Kind::rsa_pkcs1, HashAlg::sha256, 0};
        if (o == oid::sha384_rsa) return {SigAlg::Kind::rsa_pkcs1, HashAlg::sha384, 0};
        if (o == oid::sha512_rsa) return {SigAlg::Kind::rsa_pkcs1, HashAlg::sha512, 0};
        if (o == oid::ecdsa_sha256) return {SigAlg::Kind::ecdsa, HashAlg::sha256, 0};
        if (o == oid::ecdsa_sha384) return {SigAlg::Kind::ecdsa, HashAlg::sha384, 0};
        if (o == oid::ecdsa_sha512) return {SigAlg::Kind::ecdsa, HashAlg::sha512, 0};
        if (o == oid::sha1_rsa) throw CertificateException("SHA-1 signatures are not accepted");
        if (o == oid::rsa_pss) {
            // RSASSA-PSS-params: [0] hashAlgorithm, [1] maskGenAlgorithm, [2] saltLength (defaults: SHA-1)
            s.kind = SigAlg::Kind::rsa_pss;
            bool have_hash = false;
            s.pss_salt = 20;
            auto params = asn1::open(r.expect(asn1::tag::sequence));
            if (auto h = params.optional(asn1::tag::context(0))) {
                auto hr = asn1::open(*h);
                auto ha = asn1::open(hr.expect(asn1::tag::sequence));
                const std::string ho = asn1::to_oid(ha.expect(asn1::tag::oid));
                s.hash = ho == oid::sha256 ? HashAlg::sha256 : ho == oid::sha384 ? HashAlg::sha384 : ho == oid::sha512 ? HashAlg::sha512 : throw CertificateException("unsupported PSS hash");
                have_hash = true;
            }
            if (auto m = params.optional(asn1::tag::context(1))) {
                auto mr = asn1::open(*m);
                auto ma = asn1::open(mr.expect(asn1::tag::sequence));
                if (asn1::to_oid(ma.expect(asn1::tag::oid)) != oid::mgf1) throw CertificateException("unsupported PSS mask function");
                auto mh = asn1::open(ma.expect(asn1::tag::sequence));
                const std::string mo = asn1::to_oid(mh.expect(asn1::tag::oid));
                const std::string want = s.hash == HashAlg::sha256 ? oid::sha256 : s.hash == HashAlg::sha384 ? oid::sha384 : oid::sha512;
                if (!have_hash || mo != want) throw CertificateException("PSS MGF1 hash must match the signature hash");
            }
            if (auto sl = params.optional(asn1::tag::context(2))) {
                auto sr = asn1::open(*sl);
                s.pss_salt = static_cast<usize>(asn1::to_integer(sr.expect(asn1::tag::integer)).to_u64());
            }
            if (!have_hash) throw CertificateException("SHA-1 signatures are not accepted");
            return s;
        }
        throw CertificateException("unsupported signature algorithm " + o);
    }

    static PublicKey parse_spki(const asn1::Element& spki) {
        auto r = asn1::open(spki);
        auto alg = asn1::open(r.expect(asn1::tag::sequence));
        const std::string a = asn1::to_oid(alg.expect(asn1::tag::oid));
        const auto bits = asn1::to_bits(r.expect(asn1::tag::bit_string));
        PublicKey k;
        if (a == oid::rsa_encryption) {
            k.kind = PublicKey::Kind::rsa;
            k.rsa = rsa::parse_public_pkcs1(bits);
        } else if (a == oid::ec_public_key) {
            const std::string curve = asn1::to_oid(alg.expect(asn1::tag::oid));
            k.kind = PublicKey::Kind::ec;
            if (curve == oid::prime256v1) k.curve = CurveId::secp256r1;
            else if (curve == oid::secp384r1) k.curve = CurveId::secp384r1;
            else throw CertificateException("unsupported EC curve " + curve);
            k.ec = Curve::by_id(k.curve)->decode_point(bits);
        } else {
            throw CertificateException("unsupported public key algorithm " + a);
        }
        return k;
    }

    void parse_extensions(const asn1::Element& seq) {
        auto exts = asn1::open(seq);
        std::vector<std::string> seen;
        while (!exts.at_end()) {
            auto e = asn1::open(exts.expect(asn1::tag::sequence));
            const std::string id = asn1::to_oid(e.expect(asn1::tag::oid));
            bool critical = false;
            if (auto c = e.optional(asn1::tag::boolean)) critical = !c->content.empty() && c->content[0] != std::byte{0};
            const auto value = e.expect(asn1::tag::octet_string);
            if (std::ranges::find(seen, id) != seen.end()) throw CertificateException("duplicate extension " + id);
            seen.push_back(id);
            asn1::Reader vr(value.content);
            if (id == oid::ext_san) {
                has_san = true;
                auto names = asn1::open(vr.expect(asn1::tag::sequence));
                while (!names.at_end()) {
                    const auto n = names.next();
                    if (n.tag == asn1::tag::context(2, false)) dns_names.emplace_back(reinterpret_cast<const char*>(n.content.data()), n.content.size());
                    else if (n.tag == asn1::tag::context(7, false)) ip_names.emplace_back(n.content.begin(), n.content.end());
                }
            } else if (id == oid::ext_basic) {
                has_basic = true;
                auto bc = asn1::open(vr.expect(asn1::tag::sequence));
                if (auto ca = bc.optional(asn1::tag::boolean)) is_ca = !ca->content.empty() && ca->content[0] != std::byte{0};
                if (auto pl = bc.optional(asn1::tag::integer)) path_len = static_cast<int>(asn1::to_integer(*pl).to_u64());
            } else if (id == oid::ext_key_usage) {
                has_key_usage = true;
                const auto bs = vr.expect(asn1::tag::bit_string);
                key_usage = 0;
                for (usize i = 1; i < bs.content.size() && i <= 2; ++i) key_usage = static_cast<core::u16>(key_usage | (static_cast<u8>(bs.content[i]) << (i == 1 ? 8 : 0)));
            } else if (id == oid::ext_eku) {
                has_eku = true;
                auto ks = asn1::open(vr.expect(asn1::tag::sequence));
                while (!ks.at_end()) eku.push_back(asn1::to_oid(ks.expect(asn1::tag::oid)));
            } else if (critical && id != "2.5.29.14" && id != "2.5.29.35" && id != "2.5.29.32" && id != "2.5.29.31" && id != "2.5.29.30") {
                throw CertificateException("unsupported critical extension " + id);
            }
        }
    }
};

/// A set of trusted root certificates.
class TrustStore {
public:
    void add(Certificate c) { roots_.push_back(std::move(c)); }
    void add_der(ByteView der) { add(Certificate::parse(der)); }
    /// Adds every CERTIFICATE block of a PEM text. Unparseable certificates are skipped when `lenient`.
    usize add_pem(std::string_view pem, bool lenient = false) {
        usize n = 0;
        for (auto& b : asn1::parse_pem(pem)) {
            if (b.label != "CERTIFICATE") continue;
            try {
                add_der(b.der);
                ++n;
            } catch (const std::exception&) {
                if (!lenient) throw;
            }
        }
        return n;
    }
    usize add_pem_file(const std::filesystem::path& p, bool lenient = false) { return add_pem(system::read_text(p), lenient); }
    [[nodiscard]] const std::vector<Certificate>& roots() const noexcept { return roots_; }
    [[nodiscard]] bool empty() const noexcept { return roots_.empty(); }

    /// The operating system's roots: PEM bundle(s) on Unix, the ROOT store on Windows. Loaded once per process.
    [[nodiscard]] static const TrustStore& system() {
        static const TrustStore store = load_system();
        return store;
    }

private:
    static TrustStore load_system() {
        TrustStore ts;
#ifdef _WIN32
        if (HCERTSTORE h = CertOpenSystemStoreW(0, L"ROOT")) {
            for (const CERT_CONTEXT* ctx = CertEnumCertificatesInStore(h, nullptr); ctx; ctx = CertEnumCertificatesInStore(h, ctx)) {
                try {
                    ts.add_der(ByteView(reinterpret_cast<const std::byte*>(ctx->pbCertEncoded), ctx->cbCertEncoded));
                } catch (const std::exception&) {
                }
            }
            CertCloseStore(h, 0);
        }
#else
        std::vector<std::filesystem::path> files;
        if (const char* f = std::getenv("SSL_CERT_FILE")) files.emplace_back(f);
        for (const char* f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"})
            files.emplace_back(f);
        std::error_code ec;
        for (const auto& f : files) {
            if (!std::filesystem::is_regular_file(f, ec)) continue;
            try {
                if (ts.add_pem_file(f, true) > 0) break;
            } catch (const std::exception&) {
            }
        }
#endif
        return ts;
    }
    std::vector<Certificate> roots_;
};

struct VerifyOptions {
    std::string host;               ///< empty = skip host name check
    core::i64 now = -1;             ///< Unix seconds; -1 = current time
    bool require_server_auth = true;
    usize max_depth = 8;
};

/// Validates `chain` (leaf first, DER) against `trust`. Throws CertificateException on failure; returns the leaf.
inline Certificate verify_chain(const std::vector<Bytes>& chain, const TrustStore& trust, const VerifyOptions& opt) {
    if (chain.empty()) throw CertificateException("the server sent no certificate");
    if (chain.size() > 16) throw CertificateException("certificate chain too long");
    std::vector<Certificate> certs;
    for (const auto& d : chain) certs.push_back(Certificate::parse(d));
    const core::i64 now = opt.now >= 0 ? opt.now : static_cast<core::i64>(system::unix_time_ns() / 1'000'000'000);
    const Certificate& leaf = certs[0];

    if (!opt.host.empty() && !leaf.matches_host(opt.host)) throw CertificateException("host name '" + opt.host + "' does not match the certificate (" + leaf.display_name() + ")");
    if (opt.require_server_auth && leaf.has_eku && std::ranges::find(leaf.eku, oid::eku_server_auth) == leaf.eku.end() && std::ranges::find(leaf.eku, oid::eku_any) == leaf.eku.end())
        throw CertificateException("certificate is not valid for server authentication");
    if (!leaf.is_ca && leaf.has_key_usage && !(leaf.key_usage & Certificate::ku_digital_signature) && leaf.key.kind == PublicKey::Kind::ec)
        throw CertificateException("certificate key usage forbids signing");

    auto check_time = [&](const Certificate& c) {
        if (now < c.not_before) throw CertificateException(c.display_name() + " is not yet valid");
        if (now > c.not_after) throw CertificateException(c.display_name() + " has expired");
    };
    auto strong_enough = [](const Certificate& c) { return c.key.kind != PublicKey::Kind::rsa || c.key.rsa.n.bit_length() >= 2048; };

    // Depth-first search for a path leaf -> ... -> trust anchor.
    std::string last_error = "unable to find a trusted issuer";
    std::vector<const Certificate*> path{&leaf};
    auto ca_ok = [&](const Certificate& ca, usize certs_below) {
        if (!ca.is_ca) return false;
        if (ca.has_key_usage && !(ca.key_usage & Certificate::ku_key_cert_sign)) return false;
        if (ca.path_len >= 0 && static_cast<usize>(ca.path_len) < certs_below) return false;
        return true;
    };
    auto search = [&](auto&& self, const Certificate& cur, usize depth) -> bool {
        check_time(cur);
        // Is `cur` itself a trust anchor (identical DER)?
        for (const auto& r : trust.roots())
            if (r.der == cur.der) return true;
        if (depth >= opt.max_depth) {
            last_error = "certificate chain too deep";
            return false;
        }
        const usize below = depth;  // number of non-self-issued intermediates below the candidate issuer
        for (const auto& r : trust.roots()) {
            if (r.subject != cur.issuer || !ca_ok(r, below > 0 ? below - 1 : 0)) continue;
            if (!cur.verify_signature_by(r)) {
                last_error = "signature verification failed";
                continue;
            }
            if (now < r.not_before || now > r.not_after) {
                last_error = "root certificate " + r.display_name() + " is outside its validity period";
                continue;
            }
            return true;
        }
        for (usize i = 1; i < certs.size(); ++i) {
            const Certificate& cand = certs[i];
            if (std::ranges::find(path, &cand) != path.end()) continue;
            if (cand.subject != cur.issuer || !ca_ok(cand, below > 0 ? below - 1 : 0) || !strong_enough(cand)) continue;
            if (!cur.verify_signature_by(cand)) {
                last_error = "signature verification failed";
                continue;
            }
            path.push_back(&cand);
            if (self(self, cand, depth + 1)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!search(search, leaf, 0)) throw CertificateException(last_error);
    return leaf;
}

}  // namespace flyg::net::tls
