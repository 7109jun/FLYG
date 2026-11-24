// flyG - net module tests: TLS handshake/record layer, HTTPS, WSS, interop with the openssl CLI
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/net.hpp>
#include <flyg/system/process.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net;
#include "tls_certs.inc"

namespace {
tls::TrustStore trust_of(const char* pem) {
    tls::TrustStore t;
    t.add_pem(pem);
    return t;
}
std::string leaf_chain_rsa() { return std::string(pem_rsa_leaf_crt) + pem_rsa_int_crt; }

/// One-shot TLS echo server: reads a line, answers "echo:<line>", then (optionally) streams `bulk` bytes.
struct EchoServer {
    TcpListener listener = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    tls::TlsServerConfig cfg;
    std::thread th;
    std::atomic<bool> handshake_ok{false};
    std::string error, alpn, received;
    explicit EchoServer(tls::TlsServerConfig c, usize connections = 1) : cfg(std::move(c)) {
        th = std::thread([this, connections] {
            for (usize i = 0; i < connections; ++i) {
                try {
                    auto acc = listener.accept(Timeout{5000});
                    if (!acc) throw NetworkException("accept timed out");
                    auto lower = std::make_unique<TcpByteStream>(std::move(acc->first));
                    lower->set_timeouts(Timeout{5000}, Timeout{5000});
                    auto s = tls::TlsStream::accept(std::move(lower), cfg);
                    handshake_ok = true;
                    alpn = s->alpn();
                    std::string line;
                    BufferedReader r(*s);
                    if (r.read_line(line, 1 << 20)) {
                        received = line;
                        if (line.rfind("BULK ", 0) == 0) {
                            const usize n = std::stoul(line.substr(5));
                            std::string blob(n, 0);
                            for (usize k = 0; k < n; ++k) blob[k] = static_cast<char>((k * 31 + 7) & 0xff);
                            s->write_all(blob);
                        } else {
                            s->write_all("echo:" + line + "\n");
                        }
                    }
                    s->close();
                } catch (const std::exception& e) {
                    error = e.what();
                }
            }
        });
    }
    ~EchoServer() { if (th.joinable()) th.join(); }
    [[nodiscard]] u16 port() const { return listener.local_endpoint().port(); }
};

std::unique_ptr<tls::TlsStream> dial(u16 port, const tls::TlsClientConfig& cfg) {
    auto lower = std::make_unique<TcpByteStream>(TcpStream::connect(Endpoint(IpAddress::loopback(), port), Timeout{5000}));
    lower->set_timeouts(Timeout{5000}, Timeout{5000});
    return tls::TlsStream::connect(std::move(lower), cfg);
}
std::string read_all(ByteStream& s) {
    std::string out;
    std::byte buf[8192];
    for (;;) {
        const usize n = s.read_some(buf);
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf), n);
    }
    return out;
}
}  // namespace

FLYG_TEST(tls_loopback_rsa_chain_tls13) {
    EchoServer srv(tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key));
    const auto trust = trust_of(pem_rsa_root_crt);
    tls::TlsClientConfig cc;
    cc.server_name = "localhost";
    cc.trust = &trust;
    auto c = dial(srv.port(), cc);
    CHECK_EQ(std::string(c->version()), "TLSv1.3");
    CHECK(c->is_secure());
    CHECK(c->peer_certificate().has_value());
    CHECK_EQ(c->peer_certificate()->display_name(), "localhost");
    c->write_all("hello tls\n");
    CHECK_EQ(read_all(*c), "echo:hello tls\n");  // also proves close_notify is delivered as a clean EOF
    srv.th.join();
    CHECK(srv.handshake_ok);
    CHECK_EQ(srv.error, "");
}

FLYG_TEST(tls_loopback_ec_curves_and_alpn) {
    struct Case { const char* chain; const char* key; const char* root; };
    for (auto [chain, key, root] : {Case{pem_ec_leaf_crt, pem_ec_leaf_key, pem_ec_root_crt}, Case{pem_ec384_leaf_crt, pem_ec384_leaf_key, pem_ec384_root_crt}}) {
        auto sc = tls::TlsServerConfig::from_pem(chain, key);
        sc.alpn = {"h2", "http/1.1"};
        EchoServer srv(std::move(sc));
        const auto trust = trust_of(root);
        tls::TlsClientConfig cc;
        cc.server_name = "127.0.0.1";  // IP SAN
        cc.trust = &trust;
        cc.alpn = {"spdy/3", "http/1.1"};
        auto c = dial(srv.port(), cc);
        CHECK_EQ(c->alpn(), "http/1.1");
        c->write_all("x\n");
        CHECK_EQ(read_all(*c), "echo:x\n");
        srv.th.join();
        CHECK_EQ(srv.alpn, "http/1.1");
    }
}

FLYG_TEST(tls_loopback_bulk_transfer) {
    EchoServer srv(tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key));
    const auto trust = trust_of(pem_rsa_root_crt);
    tls::TlsClientConfig cc;
    cc.server_name = "localhost";
    cc.trust = &trust;
    auto c = dial(srv.port(), cc);
    const usize n = 1'000'000;  // many records, crosses the 16 KiB record boundary
    c->write_all("BULK " + std::to_string(n) + "\n");
    const std::string got = read_all(*c);
    CHECK_EQ(got.size(), n);
    bool same = true;
    for (usize k = 0; k < n && same; ++k) same = static_cast<unsigned char>(got[k]) == ((k * 31 + 7) & 0xff);
    CHECK(same);
}

FLYG_TEST(tls_certificate_failures) {
    const auto good = trust_of(pem_rsa_root_crt);
    struct Probe { const char* name; tls::TlsClientConfig cc; };
    tls::TlsClientConfig wrong_host;
    wrong_host.server_name = "evil.example.org";
    wrong_host.trust = &good;
    const auto other = trust_of(pem_other_root_crt);
    tls::TlsClientConfig untrusted;
    untrusted.server_name = "localhost";
    untrusted.trust = &other;
    tls::TlsClientConfig expired;
    expired.server_name = "localhost";
    expired.trust = &good;
    expired.now = 5'200'000'000;
    for (auto* cc : {&wrong_host, &untrusted, &expired}) {
        EchoServer srv(tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key));
        CHECK_THROWS_AS(dial(srv.port(), *cc), tls::CertificateException);
        srv.th.join();
        CHECK(!srv.handshake_ok);  // the client aborted with an alert
    }
    // verification can be disabled explicitly
    EchoServer srv(tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key));
    tls::TlsClientConfig insecure;
    insecure.server_name = "whatever";
    insecure.verify = false;
    auto c = dial(srv.port(), insecure);
    c->write_all("ok\n");
    CHECK_EQ(read_all(*c), "echo:ok\n");
}

FLYG_TEST(tls_server_rejects_garbage) {
    EchoServer srv(tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key));
    auto s = TcpStream::connect(Endpoint(IpAddress::loopback(), srv.port()), Timeout{3000});
    s.write_all("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    srv.th.join();
    CHECK(!srv.handshake_ok);
    CHECK(!srv.error.empty());
}

FLYG_TEST(tls_https_server_client_and_wss) {
    const auto sc = tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key);
    http::ServerOptions so;
    so.accept_wrapper = tls_accept_wrapper(sc);
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [](const http::Request& req, http::Response& resp) {
        if (ws::is_upgrade_request(req)) {
            ws::accept(req, resp, [](ws::Connection& c) { while (auto m = c.receive()) c.send_text("echo:" + m->data); });
            return;
        }
        resp.set_body(req.method + " " + req.target + " body=" + std::string(reinterpret_cast<const char*>(req.body.data()), req.body.size()));
    }, so);
    srv.start();
    const auto trust = trust_of(pem_rsa_root_crt);
    tls::TlsClientConfig cc;
    cc.trust = &trust;
    http::Client client;
    enable_https(client, cc);
    const std::string base = "https://localhost:" + std::to_string(srv.port());
    auto r = client.get(base + "/a?b=1");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "GET /a?b=1 body=");
    r = client.post(base + "/p", "payload", "text/plain");  // keep-alive reuse over TLS
    CHECK_EQ(r.body, "POST /p body=payload");
    std::string big(300000, 'z');
    CHECK_EQ(client.post(base + "/big", big, "text/plain").body.size(), std::string("POST /big body=").size() + big.size());
    auto w = ws::connect(client, "wss://localhost:" + std::to_string(srv.port()) + "/sock");
    w->send_text("secure hello");
    CHECK_EQ(w->receive()->data, "echo:secure hello");
    w->close();

    // the default (system roots) must reject our private CA
    http::Client strict;
    CHECK_THROWS_AS(strict.get(base + "/"), tls::CertificateException);
    srv.stop();
}

// ---- interop with the OpenSSL command line tool -----------------------------------------------------------------

namespace {
bool have_openssl() {
#if FLYG_PLATFORM_WINDOWS
    return false;
#else
    static const bool ok = [] {
        try {
            return system::run("openssl", {"version"}).exit_code == 0;
        } catch (const std::exception&) {
            return false;
        }
    }();
    return ok;
#endif
}
std::filesystem::path write_tmp(const std::filesystem::path& dir, const char* name, const char* text) {
    const auto p = dir / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}
u16 free_port() {
    auto l = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    return l.local_endpoint().port();
}
}  // namespace

FLYG_TEST(tls_interop_openssl_s_server) {
    if (!have_openssl()) { std::printf("      (openssl not available, skipped)\n"); return; }
    const auto dir = std::filesystem::temp_directory_path() / ("flyg_tls_" + std::to_string(system::unix_time_ns()));
    std::filesystem::create_directories(dir);
    const auto int_c = write_tmp(dir, "int.crt", pem_rsa_int_crt);
    const auto rsa_c = write_tmp(dir, "rsa.crt", pem_rsa_leaf_crt), rsa_k = write_tmp(dir, "rsa.key", pem_rsa_leaf_key);
    const auto ec_c = write_tmp(dir, "ec.crt", pem_ec_leaf_crt), ec_k = write_tmp(dir, "ec.key", pem_ec_leaf_key);
    struct Case { const char* label; bool rsa; std::vector<std::string> args; const char* version; const char* suite; };
    const std::vector<Case> cases = {
        {"1.3 aes128", true, {"-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256"}, "TLSv1.3", "TLS_AES_128_GCM_SHA256"},
        {"1.3 aes256", true, {"-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384"}, "TLSv1.3", "TLS_AES_256_GCM_SHA384"},
        {"1.3 chacha", false, {"-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"}, "TLSv1.3", "TLS_CHACHA20_POLY1305_SHA256"},
        {"1.3 HRR (P-384 only)", true, {"-tls1_3", "-groups", "P-384"}, "TLSv1.3", ""},
        {"1.3 HRR (P-256 only, ec cert)", false, {"-tls1_3", "-groups", "P-256"}, "TLSv1.3", ""},
        {"1.2 ECDHE-RSA-AES128-GCM", true, {"-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256"}, "TLSv1.2", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"},
        {"1.2 ECDHE-RSA-AES256-GCM", true, {"-tls1_2", "-cipher", "ECDHE-RSA-AES256-GCM-SHA384"}, "TLSv1.2", "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"},
        {"1.2 ECDHE-RSA-CHACHA", true, {"-tls1_2", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305"}, "TLSv1.2", "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"},
        {"1.2 ECDHE-ECDSA-AES128-GCM", false, {"-tls1_2", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256"}, "TLSv1.2", "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"},
        {"1.2 ECDHE-ECDSA-CHACHA", false, {"-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305"}, "TLSv1.2", "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"},
        {"1.2 P-384 group", true, {"-tls1_2", "-groups", "P-384", "-cipher", "ECDHE-RSA-AES256-GCM-SHA384"}, "TLSv1.2", ""},
    };
    for (const auto& cs : cases) {
        const u16 port = free_port();
        system::ProcessOptions po;
        po.pipe_stdin = true;
        po.pipe_stdout = true;
        po.merge_stderr = true;
        po.args = {"s_server", "-accept", std::to_string(port), "-cert", (cs.rsa ? rsa_c : ec_c).string(), "-key", (cs.rsa ? rsa_k : ec_k).string(), "-www", "-quiet"};
        if (cs.rsa) po.args.insert(po.args.end(), {"-cert_chain", int_c.string()});
        po.args.insert(po.args.end(), cs.args.begin(), cs.args.end());
        auto proc = system::Process::spawn("openssl", po);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto trust = trust_of(cs.rsa ? pem_rsa_root_crt : pem_ec_root_crt);
        tls::TlsClientConfig cc;
        cc.server_name = "localhost";
        cc.trust = &trust;
        std::string body, err;
        std::string version, suite;
        try {
            auto c = dial(port, cc);
            version = c->version();
            suite = c->cipher_suite();
            c->write_all("GET / HTTP/1.0\r\n\r\n");
            body = read_all(*c);
        } catch (const std::exception& e) {
            err = e.what();
        }
        proc.kill();
        (void)proc.wait();
        if (!err.empty()) std::printf("      [%s] error: %s\n", cs.label, err.c_str());
        CHECK_EQ(err, "");
        CHECK(body.rfind("HTTP/1.0 200", 0) == 0);
        CHECK_EQ(version, std::string(cs.version));
        if (cs.suite[0]) CHECK_EQ(suite, std::string(cs.suite));
    }
    std::filesystem::remove_all(dir);
}

FLYG_TEST(tls_interop_openssl_s_client) {
    if (!have_openssl()) { std::printf("      (openssl not available, skipped)\n"); return; }
    const auto dir = std::filesystem::temp_directory_path() / ("flyg_tlsc_" + std::to_string(system::unix_time_ns()));
    std::filesystem::create_directories(dir);
    struct Case { const char* label; const char* chain; const char* key; const char* root; bool via_rsa; std::vector<std::string> args; };
    const std::vector<Case> cases = {
        {"rsa default", nullptr, pem_rsa_leaf_key, pem_rsa_root_crt, true, {}},
        {"ec p256 default", pem_ec_leaf_crt, pem_ec_leaf_key, pem_ec_root_crt, false, {}},
        {"ec p384 default", pem_ec384_leaf_crt, pem_ec384_leaf_key, pem_ec384_root_crt, false, {}},
        {"rsa chacha", nullptr, pem_rsa_leaf_key, pem_rsa_root_crt, true, {"-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"}},
        {"rsa aes256", nullptr, pem_rsa_leaf_key, pem_rsa_root_crt, true, {"-ciphersuites", "TLS_AES_256_GCM_SHA384"}},
        {"rsa x25519-only HRR-free", nullptr, pem_rsa_leaf_key, pem_rsa_root_crt, true, {"-groups", "X25519"}},
        {"rsa P-256 only (server HRR)", nullptr, pem_rsa_leaf_key, pem_rsa_root_crt, true, {"-groups", "P-256"}},
    };
    for (const auto& cs : cases) {
        const std::string chain = cs.via_rsa ? leaf_chain_rsa() : std::string(cs.chain);
        EchoServer srv(tls::TlsServerConfig::from_pem(chain, cs.key));
        const auto ca = write_tmp(dir, "ca.pem", cs.root);
        std::vector<std::string> args = {"s_client", "-connect", "127.0.0.1:" + std::to_string(srv.port()), "-CAfile", ca.string(), "-verify_return_error",
                                         "-verify_ip", "127.0.0.1", "-servername", "localhost", "-ign_eof", "-tls1_3"};
        args.insert(args.end(), cs.args.begin(), cs.args.end());
        const auto res = system::run("openssl", args, "interop\n", std::chrono::milliseconds(8000), [] { system::ProcessOptions o; o.merge_stderr = true; return o; }());
        srv.th.join();
        if (res.out.find("echo:interop") == std::string::npos) std::printf("      [%s] openssl output:\n%s\n srv error: %s\n", cs.label, res.out.c_str(), srv.error.c_str());
        CHECK(res.out.find("echo:interop") != std::string::npos);
        CHECK(res.out.find("Verify return code: 0 (ok)") != std::string::npos);
        CHECK(srv.handshake_ok);
    }
    std::filesystem::remove_all(dir);
}

// ---- robustness: malformed input must fail cleanly (ASan/UBSan watch for memory errors) --------------------------

namespace {
/// In-memory stream: reads come from `in`, writes are captured in `out`.
class MemStream final : public ByteStream {
public:
    explicit MemStream(std::vector<std::byte> in = {}, std::vector<std::byte>* capture = nullptr) : out_(capture), in_(std::move(in)) {}
    usize read_some(std::span<std::byte> b) override {
        const usize n = std::min(b.size(), in_.size() - pos_);
        if (n) std::memcpy(b.data(), in_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    void write_all(std::span<const std::byte> d) override { if (out_) out_->insert(out_->end(), d.begin(), d.end()); }
    void shutdown_write() override {}
    void close() noexcept override {}
    void set_timeouts(Timeout, Timeout) override {}
    [[nodiscard]] Endpoint peer_endpoint() const override { return Endpoint(IpAddress::loopback(), 1); }

private:
    std::vector<std::byte>* out_;
    std::vector<std::byte> in_;
    usize pos_ = 0;
};
}  // namespace

FLYG_TEST(tls_fuzz_malformed_handshakes) {
    const auto sc = tls::TlsServerConfig::from_pem(leaf_chain_rsa(), pem_rsa_leaf_key);
    // a genuine ClientHello flight produced by our client (the stream ends right after it)
    std::vector<std::byte> hello;
    {
        tls::TlsClientConfig cc;
        cc.server_name = "localhost";
        cc.alpn = {"http/1.1"};
        try { (void)tls::TlsStream::connect(std::make_unique<MemStream>(std::vector<std::byte>{}, &hello), cc); } catch (const NetworkException&) {}
    }
    CHECK(hello.size() > 100);
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    int rejected = 0;
    for (int i = 0; i < 400; ++i) {
        auto data = hello;
        const int flips = 1 + static_cast<int>(rnd() % 4);
        for (int f = 0; f < flips; ++f) data[rnd() % data.size()] ^= static_cast<std::byte>(1u << (rnd() % 8));
        if (i % 7 == 0) data.resize(rnd() % data.size());
        if (i % 11 == 0) for (auto& b : data) b = static_cast<std::byte>(rnd());
        try {
            (void)tls::TlsStream::accept(std::make_unique<MemStream>(std::move(data)), sc);
        } catch (const NetworkException&) {
            ++rejected;
        }
    }
    CHECK(rejected > 350);  // nothing may succeed without a valid Finished
    // random garbage towards the client
    for (int i = 0; i < 200; ++i) {
        std::vector<std::byte> junk(rnd() % 3000);
        for (auto& b : junk) b = static_cast<std::byte>(rnd());
        if (i % 2 && junk.size() > 5) { junk[0] = std::byte{22}; junk[1] = std::byte{3}; junk[2] = std::byte{3}; junk[3] = std::byte{0}; junk[4] = static_cast<std::byte>(std::min<usize>(junk.size() - 5, 250)); }
        tls::TlsClientConfig cc;
        cc.server_name = "localhost";
        CHECK_THROWS_AS(tls::TlsStream::connect(std::make_unique<MemStream>(std::move(junk)), cc), NetworkException);
    }
}
