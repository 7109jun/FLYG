// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/tls/x509.hpp>
#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net::tls;
#include "tls_certs.inc"

static Bytes der_of(const char* pem) { return asn1::parse_pem(pem).at(0).der; }
static TrustStore store_of(std::initializer_list<const char*> pems) {
    TrustStore t;
    for (auto p : pems) t.add_pem(p);
    return t;
}

FLYG_TEST(x509_parse_leaf) {
    const auto c = Certificate::parse(der_of(pem_rsa_leaf_crt));
    CHECK(c.has_san && !c.is_ca && c.has_eku);
    CHECK(c.matches_host("localhost"));
    CHECK(c.matches_host("LOCALHOST"));
    CHECK(c.matches_host("www.example.test"));
    CHECK(!c.matches_host("example.test"));
    CHECK(!c.matches_host("a.b.example.test"));
    CHECK(c.matches_host("127.0.0.1"));
    CHECK(c.matches_host("::1"));
    CHECK(!c.matches_host("127.0.0.2"));
    CHECK(!c.matches_host("evil.com"));
    CHECK_EQ(c.display_name(), std::string("localhost"));
    const auto root = Certificate::parse(der_of(pem_rsa_root_crt));
    CHECK(root.is_ca && root.is_self_issued());
    CHECK(root.verify_signature_by(root));
    CHECK(!c.verify_signature_by(root));  // signed by the intermediate
}

FLYG_TEST(x509_chain_ok) {
    VerifyOptions o;
    o.host = "localhost";
    const std::vector<Bytes> chain{der_of(pem_rsa_leaf_crt), der_of(pem_rsa_int_crt)};
    CHECK_NOTHROW(verify_chain(chain, store_of({pem_rsa_root_crt}), o));
    // root sent by the server too
    CHECK_NOTHROW(verify_chain({chain[0], chain[1], der_of(pem_rsa_root_crt)}, store_of({pem_rsa_root_crt}), o));
    // EC chains (P-256 and P-384)
    CHECK_NOTHROW(verify_chain({der_of(pem_ec_leaf_crt)}, store_of({pem_ec_root_crt}), o));
    CHECK_NOTHROW(verify_chain({der_of(pem_ec384_leaf_crt)}, store_of({pem_ec384_root_crt}), o));
    // a self-signed root used directly as a trust anchor
    CHECK_NOTHROW(verify_chain({der_of(pem_ec_root_crt)}, store_of({pem_ec_root_crt}), VerifyOptions{}));
}

FLYG_TEST(x509_chain_failures) {
    VerifyOptions o;
    o.host = "localhost";
    const std::vector<Bytes> chain{der_of(pem_rsa_leaf_crt), der_of(pem_rsa_int_crt)};
    const auto good = store_of({pem_rsa_root_crt});
    CHECK_THROWS_AS(verify_chain(chain, store_of({pem_other_root_crt}), o), CertificateException);   // untrusted
    CHECK_THROWS_AS(verify_chain({chain[0]}, good, o), CertificateException);                          // missing intermediate
    CHECK_THROWS_AS(verify_chain({der_of(pem_ec_leaf_crt)}, store_of({pem_ec384_root_crt}), o), CertificateException);  // wrong CA
    VerifyOptions w = o;
    w.host = "evil.example.org";
    CHECK_THROWS_AS(verify_chain(chain, good, w), CertificateException);                               // hostname
    VerifyOptions late = o;
    late.now = 5'200'000'000;  // year ~2134: past the 100-year validity
    CHECK_THROWS_AS(verify_chain(chain, good, late), CertificateException);
    VerifyOptions early = o;
    early.now = 1'000'000'000;  // 2001
    CHECK_THROWS_AS(verify_chain(chain, good, early), CertificateException);                           // not yet valid
    // a leaf may not act as an issuer (intermediate must be CA)
    CHECK_THROWS_AS(verify_chain({der_of(pem_ec_leaf_crt), der_of(pem_rsa_leaf_crt)}, store_of({pem_other_root_crt}), o), CertificateException);
    // tampered signature
    Bytes bad = chain[0];
    bad[bad.size() - 5] ^= std::byte{0x40};
    CHECK_THROWS_AS(verify_chain({bad, chain[1]}, good, o), CertificateException);
    CHECK_THROWS_AS(verify_chain({}, good, o), CertificateException);
}

FLYG_TEST(x509_system_store) {
    const auto& s = TrustStore::system();
    std::printf("      system trust store: %zu roots\n", s.roots().size());
    CHECK(true);
}
