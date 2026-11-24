// flyG - data module tests: DEFLATE / zlib / gzip, plus transparent HTTP content decoding
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/compress.hpp>
#include <flyg/net/net.hpp>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

namespace {
std::vector<std::byte> B(std::string_view s) { return std::vector<std::byte>(reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()); }
std::string S(const std::vector<std::byte>& v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size()); }
std::vector<std::byte> noise(usize n, u64 seed) {
    std::vector<std::byte> v(n);
    for (auto& b : v) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; b = static_cast<std::byte>(seed >> 56); }
    return v;
}
}  // namespace

FLYG_TEST(data_compress_known_vectors) {
    // produced by CPython's zlib / gzip modules
    CHECK_EQ(S(inflate(hex_decode("cb48cdc9c957c8402701"))), "hello hello hello hello");
    CHECK_EQ(S(zlib_decompress(hex_decode("789ccb48cdc9c95728cf2fca4901001a0b045d"))), "hello world");
    CHECK_EQ(S(zlib_decompress(hex_decode("789c030000000001"))), "");
    CHECK_EQ(S(gzip_decompress(hex_decode("1f8b08000000000002ffcb48cdc9c95728cf2fca49010085114a0d0b000000"))), "hello world");
    CHECK_EQ(adler32(B("Wikipedia")), 0x11E60398u);
}

FLYG_TEST(data_compress_roundtrip_all_levels) {
    std::string text;
    for (int i = 0; i < 4000; ++i) text += "line " + std::to_string(i % 97) + ": the quick brown fox jumps over the lazy dog\n";
    const std::vector<std::vector<std::byte>> inputs = {{}, B("a"), B("abc"), B(text), noise(100000, 1), std::vector<std::byte>(200000, std::byte{7}), noise(70000, 2)};
    for (const auto& in : inputs)
        for (int level : {0, 1, 4, 6, 9}) {
            const auto z = deflate(in, level);
            CHECK(inflate(z) == in);
            CHECK(zlib_decompress(zlib_compress(in, level)) == in);
            CHECK(gzip_decompress(gzip_compress(in, level)) == in);
        }
    CHECK(deflate(B(text), 6).size() < text.size() / 8);
    CHECK_THROWS_AS(deflate(B("x"), 10), DataException);
}

FLYG_TEST(data_compress_rejects_malformed) {
    const auto good = zlib_compress(B(std::string(5000, 'a') + "end"));
    CHECK(zlib_decompress(good).size() == 5003);
    auto bad = good;
    bad.back() ^= std::byte{1};
    CHECK_THROWS_AS(zlib_decompress(bad), DataException);              // adler mismatch
    CHECK_THROWS_AS(zlib_decompress(std::span(good).first(good.size() - 6)), DataException);  // truncated
    CHECK_THROWS_AS(inflate(hex_decode("07")), DataException);        // reserved block type
    CHECK_EQ(S(inflate(hex_decode("010100feff41"))), "A");              // valid stored block
    CHECK_THROWS_AS(inflate(hex_decode("0101000000")), DataException);    // LEN/NLEN mismatch
    // decompression bomb guard
    const auto bomb = deflate(std::vector<std::byte>(10'000'000, std::byte{0}), 6);
    CHECK_THROWS_AS(inflate(bomb, 1'000'000), DataException);
    CHECK_EQ(inflate(bomb, 20'000'000).size(), 10'000'000u);
    // gzip trailer checks
    auto gz = gzip_compress(B("payload"));
    gz[gz.size() - 5] ^= std::byte{1};
    CHECK_THROWS_AS(gzip_decompress(gz), DataException);
    // random corruption never crashes (ASan/UBSan watch) and either throws or yields something
    u64 seed = 99;
    const auto base = zlib_compress(noise(3000, 5));
    for (int i = 0; i < 500; ++i) {
        auto c = base;
        for (int k = 0; k < 3; ++k) { seed = seed * 6364136223846793005ull + 1; c[(seed >> 33) % c.size()] ^= static_cast<std::byte>(1u << ((seed >> 20) & 7)); }
        try { (void)zlib_decompress(c, 1 << 20); } catch (const DataException&) {}
    }
}

FLYG_TEST(net_http_content_encoding) {
    std::string big;
    for (int i = 0; i < 2000; ++i) big += "{\"k\":" + std::to_string(i) + ",\"v\":\"value value value\"}\n";
    net::http::ServerOptions so;
    so.compress_responses = true;
    net::http::Server srv(net::Endpoint(net::IpAddress::loopback(), 0), [&](const net::http::Request& req, net::http::Response& resp) {
        if (req.target == "/plain") { resp.set_body(big, "application/json"); return; }
        if (req.target == "/zlib") {
            const auto z = zlib_compress(B(big));
            resp.headers.set("Content-Encoding", "deflate");
            resp.set_body(S(z));
            return;
        }
        if (req.target == "/raw") {
            const auto z = deflate(B(big));
            resp.headers.set("Content-Encoding", "deflate");
            resp.set_body(S(z));
            return;
        }
        if (req.target == "/stacked") {
            const auto inner = zlib_compress(B(big));
            const auto z = gzip_compress(inner);
            resp.headers.set("Content-Encoding", "deflate, gzip");
            resp.set_body(S(z));
            return;
        }
        if (req.target == "/corrupt") {
            resp.headers.set("Content-Encoding", "gzip");
            resp.set_body("definitely not gzip data");
            return;
        }
        resp.set_body(std::string(2000, 'x'), "image/png");
    }, so);
    srv.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(srv.port());
    net::http::Client c;
    CHECK_EQ(c.get(base + "/plain").body, big);                      // gzip negotiated and undone
    CHECK_EQ(c.get(base + "/plain").headers.get("Content-Encoding"), "");
    CHECK_EQ(c.get(base + "/zlib").body, big);
    CHECK_EQ(c.get(base + "/raw").body, big);
    CHECK_EQ(c.get(base + "/stacked").body, big);
    CHECK_THROWS_AS(c.get(base + "/corrupt"), NetworkException);
    // the server really compressed on the wire: a client that does not decode sees the gzip bytes
    net::http::ClientOptions raw;
    raw.decompress = false;
    net::http::Client c2(raw);
    net::http::Headers h;
    h.add("Accept-Encoding", "gzip");
    const auto r = c2.get(base + "/plain", h);
    CHECK_EQ(r.headers.get("Content-Encoding"), "gzip");
    CHECK(r.body.size() < big.size() / 4);
    CHECK_EQ(S(gzip_decompress(B(r.body))), big);
    // not compressed when the client does not ask / non-text content types
    net::http::Headers none;
    none.add("Accept-Encoding", "identity");
    CHECK_EQ(c2.get(base + "/plain", none).headers.get("Content-Encoding"), "");
    CHECK_EQ(c2.get(base + "/img", h).headers.get("Content-Encoding"), "");
    srv.stop();
}
