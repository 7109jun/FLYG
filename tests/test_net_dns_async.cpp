// flyG - net module tests: DNS wire format + resolver, event loop, async TCP, ICMP
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/net.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net;

namespace {
std::string S(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

/// Tiny in-process DNS server (UDP + TCP) answering from a handler.
struct FakeDns {
    using Handler = std::function<dns::Message(const dns::Message&, bool tcp)>;
    UdpSocket udp = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
    TcpListener tcp;
    std::atomic<bool> stop{false};
    std::atomic<int> udp_queries{0}, tcp_queries{0};
    std::thread t_udp, t_tcp;
    Handler handler;

    explicit FakeDns(Handler h) : handler(std::move(h)) {
        tcp = TcpListener::bind(Endpoint(IpAddress::loopback(), udp.local_endpoint().port()));
        t_udp = std::thread([this] {
            std::vector<std::byte> buf(4096);
            while (!stop) {
                auto d = udp.receive_for(buf, Timeout{20});
                if (!d) continue;
                ++udp_queries;
                try {
                    const auto q = dns::Message::decode(std::span<const std::byte>(buf.data(), d->size));
                    auto resp = handler(q, false);
                    const auto w = resp.encode();
                    udp.send_to(std::as_bytes(std::span(w)), d->from);
                } catch (const NetworkException&) {
                }
            }
        });
        t_tcp = std::thread([this] {
            while (!stop) {
                auto c = tcp.accept(Timeout{20});
                if (!c) continue;
                try {
                    auto& s = c->first;
                    s.set_read_timeout(Timeout{1000});
                    std::byte hdr[2];
                    s.read_exact(hdr);
                    std::vector<std::byte> body((static_cast<usize>(hdr[0]) << 8) | static_cast<usize>(hdr[1]));
                    s.read_exact(body);
                    ++tcp_queries;
                    auto resp = handler(dns::Message::decode(body), true);
                    const auto w = resp.encode();
                    std::vector<std::byte> framed(w.size() + 2);
                    framed[0] = static_cast<std::byte>(w.size() >> 8);
                    framed[1] = static_cast<std::byte>(w.size());
                    std::memcpy(framed.data() + 2, w.data(), w.size());
                    s.write_all(framed);
                } catch (const NetworkException&) {
                }
            }
        });
    }
    ~FakeDns() {
        stop = true;
        t_udp.join();
        t_tcp.join();
    }
    Endpoint endpoint() const { return udp.local_endpoint(); }
};

dns::Message reply_to(const dns::Message& q) {
    dns::Message r;
    r.id = q.id;
    r.response = true;
    r.recursion_available = true;
    r.questions = q.questions;
    return r;
}
dns::Record rec_a(std::string name, const char* ip, u32 ttl = 60) { return {std::move(name), dns::Type::A, dns::class_in, ttl, IpAddress::from_string(ip)}; }
}  // namespace

FLYG_TEST(net_dns_encode_known_vector) {
    const auto q = dns::Message::query("www.example.com", dns::Type::A, 0x1234, false);
    const auto w = q.encode();
    const std::vector<u8> expect = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    CHECK(w == expect);
    const auto d = dns::Message::decode(std::as_bytes(std::span(w)));
    CHECK_EQ(d.id, 0x1234);
    CHECK(!d.response);
    CHECK_EQ(d.questions[0].name, "www.example.com");
}

FLYG_TEST(net_dns_roundtrip_all_types_and_compression) {
    dns::Message m;
    m.id = 77;
    m.response = true;
    m.authoritative = true;
    m.recursion_available = true;
    m.rcode = dns::Rcode::ok;
    m.edns_udp_size = 1232;
    m.questions.push_back({"example.com", dns::Type::ANY, dns::class_in});
    m.answers.push_back(rec_a("example.com", "93.184.216.34"));
    m.answers.push_back({"example.com", dns::Type::AAAA, dns::class_in, 30, IpAddress::from_string("2001:db8::1")});
    m.answers.push_back({"www.example.com", dns::Type::CNAME, dns::class_in, 30, dns::NameData{"example.com"}});
    m.answers.push_back({"example.com", dns::Type::MX, dns::class_in, 30, dns::Mx{10, "mail.example.com"}});
    m.answers.push_back({"example.com", dns::Type::TXT, dns::class_in, 30, dns::Txt{{"v=spf1 -all", "second", ""}}});
    m.answers.push_back({"_sip._tcp.example.com", dns::Type::SRV, dns::class_in, 30, dns::Srv{1, 2, 5060, "sip.example.com"}});
    m.authority.push_back({"example.com", dns::Type::SOA, dns::class_in, 30, dns::Soa{"ns1.example.com", "hostmaster.example.com", 1, 2, 3, 4, 5}});
    m.authority.push_back({"example.com", dns::Type::NS, dns::class_in, 30, dns::NameData{"ns1.example.com"}});
    m.additional.push_back({"example.com", static_cast<dns::Type>(99), dns::class_in, 30, dns::Raw{1, 2, 3}});
    const auto w = m.encode();
    const auto d = dns::Message::decode(std::as_bytes(std::span(w)));
    CHECK(d.answers == m.answers);
    CHECK(d.authority == m.authority);
    CHECK(d.additional == m.additional);
    CHECK(d.questions == m.questions);
    CHECK(d.authoritative && d.recursion_available && d.response);
    CHECK_EQ(d.edns_udp_size, 1232);
    // compression actually shrinks the message: "example.com" appears many times
    usize occurrences = 0;
    for (usize i = 0; i + 7 <= w.size(); ++i)
        if (std::memcmp(w.data() + i, "example", 7) == 0) ++occurrences;
    CHECK(occurrences <= 3);
    // name edge cases
    const auto root = dns::Message::query(".", dns::Type::NS, 1, false);
    const auto rw = root.encode();
    CHECK_EQ(dns::Message::decode(std::as_bytes(std::span(rw))).questions[0].name, ".");
    CHECK_THROWS_AS(dns::Message::query(std::string(64, 'a') + ".com", dns::Type::A, 1).encode(), NetworkException);
    CHECK_THROWS_AS(dns::Message::query("a..b", dns::Type::A, 1).encode(), NetworkException);
    std::string longname;
    for (int i = 0; i < 9; ++i) longname += std::string(30, 'a') + ".";
    CHECK_THROWS_AS(dns::Message::query(longname, dns::Type::A, 1).encode(), NetworkException);
    const auto esc = dns::Message::query("a\\.b.example", dns::Type::A, 1, false);  // a label containing a dot
    const auto ew = esc.encode();
    CHECK_EQ(dns::Message::decode(std::as_bytes(std::span(ew))).questions[0].name, "a\\.b.example");
}

FLYG_TEST(net_dns_decode_rejects_malicious_input) {
    auto q = dns::Message::query("a.example.com", dns::Type::A, 1, false).encode();
    // truncations at every length must throw, never crash
    for (usize n = 0; n < q.size(); ++n) CHECK_THROWS_AS(dns::Message::decode(std::as_bytes(std::span(q.data(), n))), NetworkException);
    // compression pointer loop
    std::vector<u8> loop = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    CHECK_THROWS_AS(dns::Message::decode(std::as_bytes(std::span(loop))), NetworkException);
    // forward pointer
    std::vector<u8> fwd = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 20, 0, 1, 0, 1, 0, 0, 1, 'a', 0};
    CHECK_THROWS_AS(dns::Message::decode(std::as_bytes(std::span(fwd))), NetworkException);
    // absurd counts
    std::vector<u8> cnt = {0, 1, 0x80, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_THROWS_AS(dns::Message::decode(std::as_bytes(std::span(cnt))), NetworkException);
    // bad A length
    dns::Message m;
    m.response = true;
    m.answers.push_back({"x", dns::Type::A, 1, 1, dns::Raw{1, 2, 3}});
    auto w = m.encode();
    CHECK_THROWS_AS(dns::Message::decode(std::as_bytes(std::span(w))), NetworkException);
    // random garbage and single-byte mutations of a valid message never crash
    std::mt19937 rng(5);
    dns::Message good = reply_to(dns::Message::query("host.example.com", dns::Type::A, 9));
    good.answers.push_back(rec_a("host.example.com", "1.2.3.4"));
    good.answers.push_back({"host.example.com", dns::Type::MX, 1, 1, dns::Mx{5, "mx.example.com"}});
    const auto gw = good.encode();
    for (int i = 0; i < 20000; ++i) {
        auto mut = gw;
        mut[rng() % mut.size()] = static_cast<u8>(rng());
        if (rng() % 4 == 0) mut.resize(rng() % mut.size());
        try { (void)dns::Message::decode(std::as_bytes(std::span(mut))); } catch (const NetworkException&) {}
    }
    for (int i = 0; i < 5000; ++i) {
        std::vector<u8> junk(rng() % 80);
        for (auto& b : junk) b = static_cast<u8>(rng());
        try { (void)dns::Message::decode(std::as_bytes(std::span(junk))); } catch (const NetworkException&) {}
    }
}

FLYG_TEST(net_dns_resolver_udp) {
    FakeDns srv([](const dns::Message& q, bool) {
        auto r = reply_to(q);
        const auto& n = q.questions[0];
        if (n.name == "alias.test" ) {
            r.answers.push_back({"alias.test", dns::Type::CNAME, 1, 60, dns::NameData{"real.test"}});
            if (n.type == dns::Type::A) r.answers.push_back(rec_a("real.test", "10.0.0.7"));
            if (n.type == dns::Type::AAAA) r.answers.push_back({"real.test", dns::Type::AAAA, 1, 60, IpAddress::from_string("fd00::7")});
        } else if (n.name == "mail.test" && n.type == dns::Type::MX) {
            r.answers.push_back({"mail.test", dns::Type::MX, 1, 60, dns::Mx{20, "b.test"}});
            r.answers.push_back({"mail.test", dns::Type::MX, 1, 60, dns::Mx{10, "a.test"}});
        } else if (n.name == "txt.test" && n.type == dns::Type::TXT) {
            r.answers.push_back({"txt.test", dns::Type::TXT, 1, 60, dns::Txt{{"hello"}}});
        } else if (n.name == "4.3.2.1.in-addr.arpa" && n.type == dns::Type::PTR) {
            r.answers.push_back({n.name, dns::Type::PTR, 1, 60, dns::NameData{"host.test"}});
        } else if (n.name == "empty.test") {
            // exists, no data
        } else {
            r.rcode = dns::Rcode::name_error;
        }
        return r;
    });
    dns::ResolverOptions o;
    o.servers = {srv.endpoint()};
    o.timeout = Timeout{1000};
    o.consult_hosts_file = false;
    dns::Resolver res(o);
    auto a = res.addresses("alias.test");
    CHECK_EQ(a.size(), 2u);
    CHECK(a[0] == IpAddress::from_string("10.0.0.7"));
    CHECK(a[1] == IpAddress::from_string("fd00::7"));
    CHECK_EQ(res.addresses("alias.test", Family::v4).size(), 1u);
    CHECK(res.addresses("empty.test").empty());
    CHECK_THROWS_AS(res.addresses("missing.test"), NetworkException);
    CHECK(res.addresses("9.9.9.9").front() == IpAddress::from_string("9.9.9.9"));
    const auto mx = res.mx("mail.test");
    CHECK_EQ(mx.size(), 2u);
    CHECK_EQ(res.txt("txt.test")[0].strings[0], "hello");
    CHECK_EQ(res.reverse(IpAddress::from_string("1.2.3.4"))[0], "host.test");
    CHECK(srv.udp_queries.load() >= 6);
}

FLYG_TEST(net_dns_resolver_tcp_fallback_and_robustness) {
    int phase = 0;
    FakeDns srv([&](const dns::Message& q, bool tcp) {
        auto r = reply_to(q);
        if (!tcp) {
            r.truncated = true;  // force the TCP retry
            return r;
        }
        r.answers.push_back(rec_a(q.questions[0].name, "10.1.1.1"));
        (void)phase;
        return r;
    });
    dns::ResolverOptions o;
    o.servers = {srv.endpoint()};
    o.timeout = Timeout{1000};
    o.consult_hosts_file = false;
    dns::Resolver res(o);
    CHECK(res.addresses("big.test", Family::v4).front() == IpAddress::from_string("10.1.1.1"));
    CHECK_EQ(srv.tcp_queries.load(), 1);
    // spoofed packets (wrong id) are ignored: a raw responder sends a bad answer first, then the good one
    auto raw = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
    std::thread t([&] {
        std::vector<std::byte> buf(1024);
        auto d = raw.receive_for(buf, Timeout{3000});
        if (!d) return;
        auto q = dns::Message::decode(std::span<const std::byte>(buf.data(), d->size));
        auto bad = reply_to(q);
        bad.id = static_cast<u16>(q.id + 1);
        bad.answers.push_back(rec_a("x.test", "6.6.6.6"));
        auto wb = bad.encode();
        raw.send_to(std::as_bytes(std::span(wb)), d->from);
        const u8 junk[3] = {1, 2, 3};
        raw.send_to(std::as_bytes(std::span(junk)), d->from);
        auto good = reply_to(q);
        good.answers.push_back(rec_a("x.test", "1.1.1.1"));
        auto wg = good.encode();
        raw.send_to(std::as_bytes(std::span(wg)), d->from);
    });
    dns::ResolverOptions o2 = o;
    o2.servers = {raw.local_endpoint()};
    dns::Resolver res2(o2);
    CHECK(res2.addresses("x.test", Family::v4).front() == IpAddress::from_string("1.1.1.1"));
    t.join();
    // dead server first, then a live one
    auto dead = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));  // never answers
    dns::ResolverOptions o3 = o;
    o3.servers = {dead.local_endpoint(), srv.endpoint()};
    o3.timeout = Timeout{150};
    o3.attempts = 1;
    dns::Resolver res3(o3);
    CHECK(res3.addresses("y.test", Family::v4).front() == IpAddress::from_string("10.1.1.1"));
    // SERVFAIL from every server -> error
    FakeDns bad([](const dns::Message& q, bool) { auto r = reply_to(q); r.rcode = dns::Rcode::server_failure; return r; });
    dns::ResolverOptions o4 = o;
    o4.servers = {bad.endpoint()};
    CHECK_THROWS_AS(dns::Resolver(o4).query("z.test", dns::Type::A), NetworkException);
    dns::ResolverOptions o5 = o;
    o5.servers = {};
    // system servers may or may not exist; querying with an unreachable one must fail cleanly
    o5.servers = {Endpoint(IpAddress::loopback(), 1)};
    o5.timeout = Timeout{100};
    o5.attempts = 1;
    CHECK_THROWS_AS(dns::Resolver(o5).query("z.test", dns::Type::A), NetworkException);
}

FLYG_TEST(net_dns_hosts_file) {
    const auto path = (std::filesystem::temp_directory_path() / ("flyg_hosts_" + std::to_string(std::random_device{}()))).string();
    {
        std::ofstream f(path);
        f << "# comment\n127.0.0.1 localhost LOCAL.test # trailing\n::1 localhost ip6-localhost\n10.9.8.7\tbox.test\nbadline\n300.1.1.1 bad.test\n";
    }
    CHECK_EQ(dns::hosts_lookup("localhost", path).size(), 2u);
    CHECK_EQ(dns::hosts_lookup("local.TEST", path).size(), 1u);
    CHECK(dns::hosts_lookup("box.test", path)[0] == IpAddress::from_string("10.9.8.7"));
    CHECK(dns::hosts_lookup("bad.test", path).empty());
    CHECK(dns::hosts_lookup("nothere", path).empty());
    std::filesystem::remove(path);
    CHECK(dns::hosts_lookup("x", "/definitely/not/here").empty());
    (void)dns::system_servers();  // must not throw whatever the machine's configuration is
}

FLYG_TEST(net_event_loop_timers_and_post) {
    EventLoop loop;
    std::vector<int> order;
    loop.run_after(Timeout{60}, [&] { order.push_back(2); });
    loop.run_after(Timeout{20}, [&] { order.push_back(1); });
    const auto cancelled = loop.run_after(Timeout{30}, [&] { order.push_back(99); });
    CHECK(loop.cancel(cancelled));
    CHECK(!loop.cancel(cancelled));
    loop.run_after(Timeout{100}, [&] { loop.stop(); });
    std::thread other([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        loop.post([&] { order.push_back(0); });
    });
    const auto t0 = std::chrono::steady_clock::now();
    loop.run();
    other.join();
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(95));
    CHECK((order == std::vector<int>{0, 1, 2}));
    CHECK(!loop.run_once(Timeout{10}));  // nothing to do
}

FLYG_TEST(net_async_echo_server_and_clients) {
    EventLoop loop;
    std::vector<std::shared_ptr<AsyncConnection>> conns;
    AsyncServer server(loop, Endpoint(IpAddress::loopback(), 0), [&](std::shared_ptr<AsyncConnection> c) {
        conns.push_back(c);
        std::weak_ptr<AsyncConnection> w = c;
        c->start([w](std::span<const std::byte> d) { if (auto s = w.lock()) s->write(d); });
    });
    const u16 port = server.local_endpoint().port();
    std::thread loop_thread([&] { loop.run(); });
    // several blocking clients, each sending a few MB
    std::vector<std::thread> clients;
    std::atomic<int> ok{0};
    for (int i = 0; i < 4; ++i)
        clients.emplace_back([&, i] {
            auto s = TcpStream::connect(Endpoint(IpAddress::loopback(), port), Timeout{3000});
            s.set_read_timeout(Timeout{10000});
            std::string data(1 << 20, static_cast<char>('A' + i));
            for (usize k = 0; k < data.size(); k += 1000) data[k] = static_cast<char>(k % 251);
            std::string back(data.size(), 0);
            std::thread rd([&] { s.read_exact(std::as_writable_bytes(std::span(back.data(), back.size()))); });
            s.write_all(data);
            rd.join();
            if (back == data) ++ok;
        });
    for (auto& c : clients) c.join();
    CHECK_EQ(ok.load(), 4);
    loop.post([&] { for (auto& c : conns) c->close(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.stop();
    loop_thread.join();
}

FLYG_TEST(net_async_connect_and_close_callbacks) {
    EventLoop loop;
    auto lis = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    const Endpoint ep = lis.local_endpoint();
    std::thread srv([&] {
        auto c = lis.accept(Timeout{3000});
        if (!c) return;
        c->first.write_all("greetings");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    std::string got, close_err = "unset";
    bool connected = false;
    std::shared_ptr<AsyncConnection> conn;
    async_connect(loop, ep, [&](TcpStream s, const std::string& err) {
        CHECK(err.empty());
        connected = s.valid();
        conn = AsyncConnection::create(loop, std::move(s));
        conn->start([&](std::span<const std::byte> d) { got += S(d); },
                    [&](const std::string& e) { close_err = e; loop.stop(); });
    }, Timeout{3000});
    loop.run_after(Timeout{5000}, [&] { loop.stop(); });
    loop.run();
    srv.join();
    CHECK(connected);
    CHECK_EQ(got, "greetings");
    CHECK_EQ(close_err, "");
    CHECK(!conn->is_open());
    // refused connection reports an error through the callback
    u16 dead;
    { auto t = TcpListener::bind(Endpoint(IpAddress::loopback(), 0)); dead = t.local_endpoint().port(); }
    std::string err;
    bool called = false;
    async_connect(loop, Endpoint(IpAddress::loopback(), dead), [&](TcpStream s, const std::string& e) { called = true; err = e; CHECK(!s.valid()); loop.stop(); }, Timeout{3000});
    loop.run_after(Timeout{5000}, [&] { loop.stop(); });
    loop.run();
    CHECK(called);
    CHECK(!err.empty());
}

FLYG_TEST(net_icmp_checksum_and_ping) {
    // RFC 1071 example: 0x0001f203f4f5f6f7 -> checksum 0x220d
    const u8 ex[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK_EQ(icmp::checksum(ex), 0x220d);
    const auto pkt = icmp::make_echo_request(false, 0xBEEF, 7, 16);
    CHECK_EQ(pkt.size(), 24u);
    CHECK_EQ(icmp::checksum(pkt), 0);  // a packet including its checksum sums to zero
    CHECK_EQ(pkt[0], 8);
    CHECK_EQ(icmp::make_echo_request(true, 1, 1, 0)[0], 128);
    // a real ping needs privileges; accept either a reply or a clean permission error
    try {
        const auto r = icmp::ping(IpAddress::loopback(), Timeout{1000});
        CHECK(r.has_value());
        if (r) CHECK(r->from.is_loopback());
    } catch (const NetworkException&) {
    }
}
