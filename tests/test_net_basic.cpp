// flyG - net module tests: addresses, TCP, UDP, interfaces
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/net.hpp>

#include <random>
#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net;

namespace {
std::string S(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }
}  // namespace

FLYG_TEST(net_ip_parse_format) {
    CHECK_EQ(IpAddress::from_string("192.168.1.10").to_string(), "192.168.1.10");
    CHECK(!IpAddress::parse("256.1.1.1"));
    CHECK(!IpAddress::parse("1.2.3"));
    CHECK(!IpAddress::parse("1.2.3.4.5"));
    CHECK(!IpAddress::parse("01.2.3.4"));
    CHECK(!IpAddress::parse(""));
    CHECK(!IpAddress::parse("a.b.c.d"));
    struct C { const char* in; const char* out; };
    for (auto [in, out] : {C{"::1", "::1"}, C{"::", "::"}, C{"2001:db8::1", "2001:db8::1"}, C{"2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"},
                           C{"fe80::1%3", "fe80::1%3"}, C{"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"}, C{"1::8", "1::8"}, C{"::ffff:1.2.3.4", "::ffff:102:304"},
                           C{"2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"}, C{"2001:0:0:1:0:0:0:1", "2001:0:0:1::1"}, C{"ABCD::EF", "abcd::ef"}}) {
        const auto a = IpAddress::parse(in);
        CHECK(a.has_value());
        if (a) CHECK_EQ(a->to_string(), out);
    }
    for (auto bad : {":::", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", ":1:2:3:4:5:6:7", "1:2:3:4:5:6:7:", "g::1", "::1%", "1.2.3.4::"})
        CHECK(!IpAddress::parse(bad));
    CHECK(IpAddress::from_string("127.0.0.1").is_loopback());
    CHECK(IpAddress::from_string("::1").is_loopback());
    CHECK(IpAddress::from_string("10.1.2.3").is_private());
    CHECK(IpAddress::from_string("172.16.0.1").is_private());
    CHECK(!IpAddress::from_string("172.32.0.1").is_private());
    CHECK(IpAddress::from_string("fd00::1").is_private());
    CHECK(IpAddress::from_string("224.0.0.1").is_multicast());
    CHECK(IpAddress::from_string("ff02::1").is_multicast());
    CHECK(IpAddress::from_string("169.254.1.1").is_link_local());
    CHECK(IpAddress::from_string("fe80::1").is_link_local());
    CHECK(IpAddress{}.is_unspecified());
    const auto v4 = IpAddress::from_string("1.2.3.4");
    CHECK(v4.mapped().is_v4_mapped());
    CHECK(v4.mapped().unmapped() == v4);
    CHECK_EQ(v4.to_u32(), 0x01020304u);
    CHECK(IpAddress::v4(0x01020304u) == v4);
    CHECK(IpAddress::from_string("10.1.2.3").in_subnet(IpAddress::from_string("10.0.0.0"), 8));
    CHECK(!IpAddress::from_string("11.1.2.3").in_subnet(IpAddress::from_string("10.0.0.0"), 8));
    CHECK(IpAddress::from_string("10.1.2.3").in_subnet(IpAddress::from_string("10.1.0.0"), 17));
    CHECK(IpAddress::from_string("2001:db8::5").in_subnet(IpAddress::from_string("2001:db8::"), 32));
    CHECK(!v4.in_subnet(IpAddress::from_string("::"), 0));
    CHECK_EQ(v4.reverse_name(), "4.3.2.1.in-addr.arpa");
    CHECK_EQ(IpAddress::from_string("2001:db8::1").reverse_name(),
             "1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa");
    CHECK(IpAddress::from_string("1.2.3.4") < IpAddress::from_string("1.2.3.5"));
    CHECK(IpAddress::from_string("255.255.255.255") < IpAddress::from_string("::"));
    CHECK_THROWS_AS(IpAddress::from_string("nope"), NetworkException);
    // text round trip over many generated addresses
    std::mt19937_64 rng(7);
    for (int i = 0; i < 2000; ++i) {
        std::array<u8, 16> b{};
        for (auto& x : b) x = (rng() % 3 == 0) ? 0 : static_cast<u8>(rng());
        const auto a = IpAddress::v6(b);
        const auto r = IpAddress::parse(a.to_string());
        if (!r || !(*r == a)) { CHECK(false); break; }
    }
}

FLYG_TEST(net_endpoint) {
    CHECK_EQ(Endpoint::from_string("1.2.3.4:80").to_string(), "1.2.3.4:80");
    CHECK_EQ(Endpoint::from_string("[::1]:8080").to_string(), "[::1]:8080");
    CHECK(!Endpoint::parse("1.2.3.4"));
    CHECK(!Endpoint::parse("1.2.3.4:70000"));
    CHECK(!Endpoint::parse("::1:80"));
    CHECK(!Endpoint::parse("[::1]80"));
    CHECK(!Endpoint::parse("1.2.3.4:"));
    sockaddr_storage ss;
    const Endpoint e(IpAddress::from_string("fe80::2%4"), 99);
    e.to_sockaddr(ss);
    CHECK(Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss)) == e);
    const Endpoint e4 = Endpoint::from_string("9.8.7.6:5");
    e4.to_sockaddr(ss);
    CHECK(Endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss)) == e4);
}

FLYG_TEST(net_resolve_literals_and_localhost) {
    CHECK(resolve("1.2.3.4").front() == IpAddress::from_string("1.2.3.4"));
    CHECK(resolve("::1", Family::v6).front().is_loopback());
    CHECK(resolve("1.2.3.4", Family::v6).front().is_v4_mapped());
    CHECK_THROWS_AS(resolve("::1", Family::v4), NetworkException);
    const auto lh = resolve("localhost");
    CHECK(!lh.empty());
    CHECK(lh.front().is_loopback());
    CHECK_THROWS_AS(resolve("no-such-host.invalid"), NetworkException);
    CHECK(!host_name().empty());
}

FLYG_TEST(net_tcp_echo) {
    auto lis = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    const u16 port = lis.local_endpoint().port();
    CHECK(port != 0);
    std::thread srv([&] {
        auto c = lis.accept(Timeout{5000});
        CHECK(c.has_value());
        auto& [stream, peer] = *c;
        CHECK(peer.address().is_loopback());
        std::byte buf[100];
        for (;;) {
            const usize n = stream.read_some(buf);
            if (n == 0) break;
            stream.write_all(std::span<const std::byte>(buf, n));
        }
    });
    auto cl = TcpStream::connect(Endpoint(IpAddress::loopback(), port), Timeout{5000});
    CHECK_EQ(cl.peer_endpoint().port(), port);
    cl.write_all("hello");
    std::byte buf[5];
    cl.read_exact(buf);
    CHECK_EQ(S(buf), "hello");
    // large transfer in both directions (exceeds socket buffers: exercises partial writes)
    std::string big(3 * 1024 * 1024, 'x');
    for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 7) % 26);
    std::string back(big.size(), '\0');
    std::thread rd([&] { cl.read_exact(std::as_writable_bytes(std::span(back.data(), back.size()))); });
    cl.write_all(big);
    rd.join();
    CHECK(back == big);
    cl.shutdown(TcpStream::Shutdown::write);
    std::byte one[1];
    CHECK_EQ(cl.read_some(one), 0u);  // server closed after our FIN
    srv.join();
}

FLYG_TEST(net_tcp_timeouts_and_errors) {
    auto lis = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    CHECK(!lis.accept(Timeout{50}).has_value());
    CHECK(!lis.try_accept().has_value());
    const u16 port = lis.local_endpoint().port();
    auto cl = TcpStream::connect(Endpoint(IpAddress::loopback(), port), Timeout{2000});  // completes via backlog
    auto acc = lis.accept(Timeout{2000});
    CHECK(acc.has_value());
    cl.set_read_timeout(Timeout{80});
    std::byte b[1];
    CHECK_THROWS_AS(cl.read_some(b), TimeoutException);
    CHECK(!cl.try_read(b).has_value());
    acc->first.close();
    CHECK_EQ(cl.read_some(b), 0u);
    CHECK_THROWS_AS(cl.read_exact(b), NetworkException);
    // connection refused: take a free port and close the listener
    u16 dead;
    { auto t = TcpListener::bind(Endpoint(IpAddress::loopback(), 0)); dead = t.local_endpoint().port(); }
    CHECK_THROWS_AS(TcpStream::connect(Endpoint(IpAddress::loopback(), dead), Timeout{2000}), NetworkException);
    // port already in use
    CHECK_THROWS_AS(TcpListener::bind(Endpoint(IpAddress::loopback(), port), 8, false), NetworkException);
}

FLYG_TEST(net_tcp_connect_by_name_and_v6) {
    auto lis = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    const u16 port = lis.local_endpoint().port();
    std::thread t([&] { auto c = lis.accept(Timeout{3000}); if (c) c->first.write_all("hi"); });
    auto cl = TcpStream::connect("127.0.0.1", port, Timeout{3000});
    std::byte b[2];
    cl.read_exact(b);
    CHECK_EQ(S(b), "hi");
    t.join();
    try {
        auto l6 = TcpListener::bind(Endpoint(IpAddress::loopback(Family::v6), 0));
        std::thread t6([&] { auto c = l6.accept(Timeout{3000}); if (c) c->first.write_all("v6"); });
        auto c6 = TcpStream::connect(Endpoint(IpAddress::loopback(Family::v6), l6.local_endpoint().port()), Timeout{3000});
        std::byte b6[2];
        c6.read_exact(b6);
        CHECK_EQ(S(b6), "v6");
        t6.join();
    } catch (const NetworkException&) {
        // IPv6 loopback unavailable in this environment
    }
}

FLYG_TEST(net_udp) {
    auto a = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
    auto b = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
    b.send_to("ping", a.local_endpoint());
    std::byte buf[64];
    auto d = a.receive_for(buf, Timeout{2000});
    CHECK(d.has_value());
    CHECK_EQ(S(std::span(buf, d->size)), "ping");
    CHECK(d->from == b.local_endpoint());
    CHECK(!a.receive_for(buf, Timeout{30}).has_value());
    a.set_read_timeout(Timeout{30});
    CHECK_THROWS_AS(a.receive(buf), TimeoutException);
    // connected udp + empty datagram
    a.connect(b.local_endpoint());
    a.send(std::span<const std::byte>{});
    auto e = b.receive_for(buf, Timeout{2000});
    CHECK(e.has_value() && e->size == 0);
    // truncation keeps the first bytes
    b.send_to(std::string(200, 'z'), a.local_endpoint());
    std::byte small[10];
    auto t = a.receive_for(small, Timeout{2000});
    CHECK(t.has_value() && t->size == 10);
    // many datagrams, no loss on loopback for modest counts
    for (int i = 0; i < 50; ++i) b.send_to(std::to_string(i), a.local_endpoint());
    for (int i = 0; i < 50; ++i) {
        auto r = a.receive_for(buf, Timeout{2000});
        CHECK(r.has_value());
        if (r) CHECK_EQ(S(std::span(buf, r->size)), std::to_string(i));
    }
}

FLYG_TEST(net_poll_multiple) {
    auto l1 = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    auto u = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
    std::vector<PollItem> items{{l1.native_handle(), poll_read, 0}, {u.native_handle(), poll_read, 0}};
    CHECK_EQ(poll(items, Timeout{20}), 0);
    auto s = UdpSocket::open();
    s.send_to("x", u.local_endpoint());
    CHECK_EQ(poll(items, Timeout{2000}), 1);
    CHECK_EQ(items[0].revents, 0u);
    CHECK(items[1].revents & poll_read);
    auto c = TcpStream::connect(l1.local_endpoint().address().is_unspecified() ? Endpoint(IpAddress::loopback(), l1.local_endpoint().port()) : l1.local_endpoint(), Timeout{2000});
    CHECK_EQ(poll(items, Timeout{2000}), 2);
    std::vector<PollItem> w{{c.native_handle(), poll_write, 0}};
    CHECK_EQ(poll(w, Timeout{2000}), 1);
    CHECK(w[0].revents & poll_write);
}

FLYG_TEST(net_interfaces) {
    const auto ifs = interfaces();
    CHECK(!ifs.empty());
    bool found_loop = false;
    for (const auto& i : ifs) {
        CHECK(!i.name.empty());
        for (const auto& a : i.addresses)
            if (a.address == IpAddress::loopback()) {
                found_loop = true;
                CHECK_EQ(a.prefix_length, 8u);
                CHECK(i.loopback);
            }
    }
    CHECK(found_loop);
}

FLYG_TEST(net_socket_options) {
    auto l = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    l.set_receive_buffer(65536);
    CHECK(l.receive_buffer() >= 65536);
    auto c = TcpStream::connect(l.local_endpoint(), Timeout{2000});
    c.set_keep_alive(true);
    c.set_no_delay(false);
    c.set_no_delay(true);
    CHECK_EQ(c.take_error(), 0);
    auto u = UdpSocket::open();
    u.set_broadcast(true);
    u.set_ttl(5);
    TcpStream moved = std::move(c);
    CHECK(!c.valid());
    CHECK(moved.valid());
}
