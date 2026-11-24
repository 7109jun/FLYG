// flyG - net/tls: private key parsing (PKCS#8, PKCS#1, SEC1) for TLS servers
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <flyg/net/tls/x509.hpp>

namespace flyg::net::tls {

struct PrivateKey {
    enum class Kind { none, rsa, ec } kind = Kind::none;
    RsaPrivateKey rsa;
    CurveId curve = CurveId::secp256r1;
    BigInt ec_d;

    [[nodiscard]] static PrivateKey from_der_pkcs8(ByteView der) {
        asn1::Reader top(der);
        auto in = asn1::open(top.expect(asn1::tag::sequence));
        (void)asn1::to_integer(in.expect(asn1::tag::integer));
        auto alg = asn1::open(in.expect(asn1::tag::sequence));
        const std::string a = asn1::to_oid(alg.expect(asn1::tag::oid));
        const auto key = in.expect(asn1::tag::octet_string);
        if (a == oid::rsa_encryption) return from_rsa(rsa::parse_private_pkcs1(key.content));
        if (a == oid::ec_public_key) {
            const std::string c = asn1::to_oid(alg.expect(asn1::tag::oid));
            return from_sec1(key.content, c);
        }
        throw DataException("unsupported private key algorithm " + a);
    }
    [[nodiscard]] static PrivateKey from_der_pkcs1(ByteView der) { return from_rsa(rsa::parse_private_pkcs1(der)); }
    [[nodiscard]] static PrivateKey from_der_sec1(ByteView der) { return from_sec1(der, ""); }

    /// Accepts "PRIVATE KEY", "RSA PRIVATE KEY" and "EC PRIVATE KEY" PEM blocks (first match wins).
    [[nodiscard]] static PrivateKey from_pem(std::string_view pem) {
        for (const auto& b : asn1::parse_pem(pem)) {
            if (b.label == "PRIVATE KEY") return from_der_pkcs8(b.der);
            if (b.label == "RSA PRIVATE KEY") return from_der_pkcs1(b.der);
            if (b.label == "EC PRIVATE KEY") return from_der_sec1(b.der);
            if (b.label == "ENCRYPTED PRIVATE KEY") throw DataException("encrypted private keys are not supported");
        }
        throw DataException("no private key found in PEM data");
    }

private:
    static PrivateKey from_rsa(RsaPrivateKey k) {
        PrivateKey p;
        p.kind = Kind::rsa;
        p.rsa = std::move(k);
        return p;
    }
    static PrivateKey from_sec1(ByteView der, const std::string& curve_oid) {
        asn1::Reader top(der);
        auto in = asn1::open(top.expect(asn1::tag::sequence));
        (void)asn1::to_integer(in.expect(asn1::tag::integer));
        const auto priv = in.expect(asn1::tag::octet_string);
        std::string c = curve_oid;
        if (auto params = in.optional(asn1::tag::context(0))) {
            auto pr = asn1::open(*params);
            c = asn1::to_oid(pr.expect(asn1::tag::oid));
        }
        PrivateKey p;
        p.kind = Kind::ec;
        if (c == oid::prime256v1) p.curve = CurveId::secp256r1;
        else if (c == oid::secp384r1) p.curve = CurveId::secp384r1;
        else throw DataException("unsupported EC curve " + c);
        p.ec_d = BigInt::from_bytes(priv.content);
        const Curve& cv = *Curve::by_id(p.curve);
        if (p.ec_d.is_zero() || p.ec_d >= cv.n) throw DataException("EC private key out of range");
        return p;
    }
};

}  // namespace flyg::net::tls
