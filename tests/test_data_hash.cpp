// flyG - data module tests: hashes, HMAC, HKDF
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

namespace {
template <usize N>
std::string hx(const std::array<std::byte, N>& d) { return hex_encode(d); }
std::vector<std::byte> unhex(std::string_view s) { return hex_decode(s); }
}  // namespace

FLYG_TEST(data_hash_known_vectors) {
    CHECK_EQ(hx(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK_EQ(hx(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_EQ(hx(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK_EQ(hx(sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(hx(sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(hx(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK_EQ(hx(sha384("abc")), "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    CHECK_EQ(hx(sha512("abc")),
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK_EQ(hx(sha512("")),
             "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    // one million 'a'
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk);
    CHECK_EQ(hx(h.finish()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

FLYG_TEST(data_hash_incremental_equals_oneshot) {
    std::string data(5000, 0);
    for (usize i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31 + 7);
    for (usize split : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 111u, 112u, 127u, 128u, 129u, 1000u}) {
        Sha256 a;
        Sha512 b;
        Sha1 c;
        a.update(std::string_view(data).substr(0, split)).update(std::string_view(data).substr(split));
        b.update(std::string_view(data).substr(0, split)).update(std::string_view(data).substr(split));
        c.update(std::string_view(data).substr(0, split)).update(std::string_view(data).substr(split));
        CHECK(a.finish() == sha256(data));
        CHECK(b.finish() == sha512(data));
        CHECK(c.finish() == sha1(data));
    }
    // padding boundaries: every length 0..300
    for (usize n = 0; n <= 300; ++n) {
        const std::string s(n, 'q');
        Sha256 x;
        for (char ch : s) x.update(std::string_view(&ch, 1));
        if (!(x.finish() == sha256(s))) { CHECK(false); break; }
    }
}

FLYG_TEST(data_hmac_hkdf_vectors) {
    // RFC 4231 test case 1 and 2
    const auto key1 = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const std::string d1 = "Hi There";
    CHECK_EQ(hx(hmac<Sha256>(key1, std::as_bytes(std::span(d1.data(), d1.size())))), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const std::string k2 = "Jefe", d2 = "what do ya want for nothing?";
    CHECK_EQ(hx(hmac<Sha256>(std::as_bytes(std::span(k2.data(), k2.size())), std::as_bytes(std::span(d2.data(), d2.size())))),
             "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK_EQ(hx(hmac<Sha512>(std::as_bytes(std::span(k2.data(), k2.size())), std::as_bytes(std::span(d2.data(), d2.size())))),
             "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
    // key longer than the block size
    const std::vector<std::byte> longkey(131, std::byte{0xaa});
    const std::string d3 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK_EQ(hx(hmac<Sha256>(longkey, std::as_bytes(std::span(d3.data(), d3.size())))), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    // RFC 5869 test case 1
    const auto ikm = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const auto salt = unhex("000102030405060708090a0b0c");
    const auto info = unhex("f0f1f2f3f4f5f6f7f8f9");
    const auto prk = hkdf_extract<Sha256>(salt, ikm);
    CHECK_EQ(hx(prk), "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    CHECK_EQ(hex_encode(hkdf_expand<Sha256>(prk, info, 42)),
             "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    CHECK_THROWS_AS(hkdf_expand<Sha256>(prk, info, 255 * 32 + 1), DataException);
    CHECK(constant_time_equal(ikm, ikm));
    CHECK(!constant_time_equal(ikm, salt));
}
