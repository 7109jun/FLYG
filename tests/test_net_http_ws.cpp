// flyG - net module tests: URL, HTTP client/server, WebSocket
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/net/net.hpp>

#include <atomic>
#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::net;

namespace {
/// Sends raw bytes to a server and returns everything it answers until close.
std::string raw_exchange(u16 port, std::string_view request, Timeout t = Timeout{3000}) {
    auto s = TcpStream::connect(Endpoint(IpAddress::loopback(), port), t);
    s.set_read_timeout(t);
    s.write_all(request);
    std::string out;
    std::byte buf[4096];
    try {
        for (;;) {
            const usize n = s.read_some(buf);
            if (n == 0) break;
            out.append(reinterpret_cast<const char*>(buf), n);
        }
    } catch (const TimeoutException&) {
    }
    return out;
}
std::string url_for(const http::Server& s, std::string_view path) { return "http://127.0.0.1:" + std::to_string(s.port()) + std::string(path); }
}  // namespace

FLYG_TEST(net_url_parse_and_resolve) {
    auto u = Url::from_string("HTTP://User:pw@Example.COM:8080/a/b%20c?x=1&y=2#frag");
    CHECK_EQ(u.scheme, "http");
    CHECK_EQ(u.userinfo, "User:pw");
    CHECK_EQ(u.host, "example.com");
    CHECK_EQ(*u.port, 8080);
    CHECK_EQ(u.path, "/a/b%20c");
    CHECK_EQ(*u.query, "x=1&y=2");
    CHECK_EQ(*u.fragment, "frag");
    CHECK_EQ(u.request_target(), "/a/b%20c?x=1&y=2");
    CHECK_EQ(u.host_header(), "example.com:8080");
    CHECK_EQ(Url::from_string("http://h").request_target(), "/");
    CHECK_EQ(Url::from_string("https://h:443/").host_header(), "h");
    CHECK_EQ(Url::from_string("https://h/").effective_port(), 443);
    CHECK_EQ(Url::from_string("ws://h/").effective_port(), 80);
    auto v6 = Url::from_string("http://[::1]:81/x");
    CHECK_EQ(v6.host, "::1");
    CHECK_EQ(v6.host_header(), "[::1]:81");
    CHECK_EQ(v6.to_string(), "http://[::1]:81/x");
    for (auto bad : {"", "nope", "http://", "http://host:99999/", "http://ho st/", "http://[::1/", "://x", "1http://x", "http://a:b/", "http://[abc]/"})
        CHECK(!Url::parse(bad));
    // RFC 3986 section 5.4 reference resolution examples
    const Url base = Url::from_string("http://a/b/c/d;p?q");
    struct C { const char* ref; const char* want; };
    for (auto [ref, want] : {C{"g", "http://a/b/c/g"}, C{"./g", "http://a/b/c/g"}, C{"g/", "http://a/b/c/g/"}, C{"/g", "http://a/g"}, C{"//g", "http://g"},
                             C{"?y", "http://a/b/c/d;p?y"}, C{"g?y", "http://a/b/c/g?y"}, C{"#s", "http://a/b/c/d;p?q#s"}, C{"g#s", "http://a/b/c/g#s"},
                             C{"", "http://a/b/c/d;p?q"}, C{".", "http://a/b/c/"}, C{"./", "http://a/b/c/"}, C{"..", "http://a/b/"}, C{"../", "http://a/b/"},
                             C{"../g", "http://a/b/g"}, C{"../..", "http://a/"}, C{"../../g", "http://a/g"}, C{"../../../g", "http://a/g"}, C{"/./g", "http://a/g"},
                             C{"g.", "http://a/b/c/g."}, C{"./g/.", "http://a/b/c/g/"}, C{"g/./h", "http://a/b/c/g/h"}, C{"g/../h", "http://a/b/c/h"},
                             C{"https://other/x", "https://other/x"}})
        CHECK_EQ(base.resolve(ref).to_string(), want);
    const auto q = parse_query("a=1&b=two%20words&c&d=x+y&=e&&f=%zz");
    CHECK_EQ(q.size(), 6u);
    CHECK_EQ(q[1].second, "two words");
    CHECK_EQ(q[3].second, "x y");
    CHECK_EQ(build_query({{"k y", "v&="}, {"é", "1"}}), "k+y=v%26%3D&%C3%A9=1");
}

FLYG_TEST(net_http_headers) {
    http::Headers h;
    h.add("Content-Type", "text/html");
    h.add("set-cookie", "a=1");
    h.add("Set-Cookie", "b=2");
    h.add("Connection", "keep-alive, Upgrade");
    CHECK(h.contains("content-type"));
    CHECK_EQ(h.get_all("SET-COOKIE").size(), 2u);
    CHECK_EQ(h.combined("set-cookie"), "a=1, b=2");
    CHECK(h.has_token("connection", "upgrade"));
    CHECK(!h.has_token("connection", "close"));
    h.set("content-type", "x");
    CHECK_EQ(h.get("Content-Type"), "x");
    CHECK_EQ(h.size(), 4u);
    CHECK(h.remove("Set-Cookie"));
    CHECK_THROWS_AS(h.add("Bad Name", "x"), NetworkException);
    CHECK_THROWS_AS(h.add("X", "a\r\nInjected: 1"), NetworkException);
    CHECK_THROWS_AS(h.add("", "x"), NetworkException);
    CHECK_EQ(h.get("missing", "dflt"), "dflt");
}

FLYG_TEST(net_http_client_server_basic) {
    std::atomic<int> hits{0};
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [&](const http::Request& req, http::Response& resp) {
        ++hits;
        if (req.path() == "/echo") {
            resp.set_body(req.method + "|" + req.body + "|" + req.query_param("q") + "|" + req.headers.get("X-Test"), "text/plain");
        } else if (req.path() == "/hello world") {
            resp.set_body("decoded");
        } else if (req.path() == "/redir") {
            resp.status = 302;
            resp.headers.set("Location", "/echo?q=landed");
        } else if (req.path() == "/loop") {
            resp.status = 302;
            resp.headers.set("Location", "/loop");
        } else if (req.path() == "/post-redir") {
            resp.status = 307;
            resp.headers.set("Location", "/echo");
        } else if (req.path() == "/see-other") {
            resp.status = 303;
            resp.headers.set("Location", "/echo");
        } else if (req.path() == "/stream") {
            int i = 0;
            resp.producer = [i](std::string& out) mutable { out = "part" + std::to_string(i); return ++i < 4; };
        } else if (req.path() == "/big") {
            resp.set_body(std::string(2'000'000, 'z'));
        } else if (req.path() == "/throw") {
            throw std::runtime_error("handler failed");
        } else if (req.path() == "/204") {
            resp.status = 204;
        } else {
            resp = http::Response::text(404, "nope");
        }
    });
    srv.start();
    http::Client c;
    auto r = c.get(url_for(srv, "/echo?q=a%20b"), [] { http::Headers h; h.add("X-Test", "yes"); return h; }());
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "GET||a b|yes");
    CHECK_EQ(r.headers.get("Content-Type"), "text/plain");
    CHECK(r.headers.contains("Date"));
    CHECK_EQ(r.headers.get("Server"), "flyG");
    r = c.post(url_for(srv, "/echo"), "payload", "text/plain");
    CHECK_EQ(r.body, "POST|payload||");
    r = c.put(url_for(srv, "/echo"), std::string(100000, 'p'));
    CHECK_EQ(r.body.size(), std::string("PUT|").size() + 100000 + 2);
    CHECK_EQ(c.get(url_for(srv, "/hello%20world")).body, "decoded");
    CHECK_EQ(c.get(url_for(srv, "/missing")).status, 404);
    CHECK_EQ(c.get(url_for(srv, "/204")).status, 204);
    CHECK_EQ(c.get(url_for(srv, "/204")).body, "");
    CHECK_EQ(c.get(url_for(srv, "/throw")).status, 500);
    CHECK_EQ(c.get(url_for(srv, "/echo")).body, "GET|||");  // connection still fine after a 500
    // HEAD: headers like GET, no body
    r = c.head(url_for(srv, "/echo"));
    CHECK_EQ(r.status, 200);
    CHECK(r.body.empty());
    CHECK(r.headers.get("Content-Length") != "0");
    // redirects
    CHECK_EQ(c.get(url_for(srv, "/redir")).body, "GET||landed|");
    r = c.post(url_for(srv, "/see-other"), "x");
    CHECK_EQ(r.body, "GET|||");                       // 303 turns POST into GET
    r = c.post(url_for(srv, "/post-redir"), "keep");
    CHECK_EQ(r.body, "POST|keep||");                  // 307 keeps method and body
    CHECK_THROWS_AS(c.get(url_for(srv, "/loop")), NetworkException);
    c.options().follow_redirects = false;
    CHECK_EQ(c.get(url_for(srv, "/redir")).status, 302);
    c.options().follow_redirects = true;
    // chunked streaming and large bodies
    CHECK_EQ(c.get(url_for(srv, "/stream")).body, "part0part1part2part3");
    CHECK_EQ(c.get(url_for(srv, "/big")).body.size(), 2'000'000u);
    usize sunk = 0;
    r = c.send(Url::from_string(url_for(srv, "/big")), http::Request{}, [&](std::span<const std::byte> d) { sunk += d.size(); });
    CHECK_EQ(sunk, 2'000'000u);
    CHECK(r.body.empty());
    // keep-alive: many requests, one accepted connection at a time
    for (int i = 0; i < 30; ++i) CHECK_EQ(c.get(url_for(srv, "/echo?q=" + std::to_string(i))).body, "GET||" + std::to_string(i) + "|");
    CHECK(srv.active_connections() <= 2u);
    // client with keep-alive disabled
    http::ClientOptions no_ka;
    no_ka.keep_alive = false;
    http::Client c2(no_ka);
    for (int i = 0; i < 3; ++i) CHECK_EQ(c2.get(url_for(srv, "/echo")).status, 200);
    CHECK_THROWS_AS(c.get("https://127.0.0.1:1/"), NetworkException);  // no TLS connector installed
    CHECK_THROWS_AS(c.get("ftp://x/"), NetworkException);
    srv.stop();
    CHECK_THROWS_AS(c.get(url_for(srv, "/echo")), NetworkException);
}

FLYG_TEST(net_http_concurrent_clients) {
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [](const http::Request& req, http::Response& resp) { resp.set_body(req.body + "!"); });
    srv.start();
    std::vector<std::thread> ts;
    std::atomic<int> ok{0};
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&, t] {
            http::Client c;
            for (int i = 0; i < 25; ++i) {
                const std::string body = std::string(static_cast<usize>(100 + i * 50), static_cast<char>('a' + t));
                if (c.post(url_for(srv, "/"), body).body == body + "!") ++ok;
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(ok.load(), 200);
}

FLYG_TEST(net_http_server_rejects_malformed_and_smuggling) {
    std::atomic<int> handled{0};
    http::ServerOptions o;
    o.limits.max_header_bytes = 2048;
    o.limits.max_body = 1000;
    o.read_timeout = Timeout{300};
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [&](const http::Request& r, http::Response& resp) { ++handled; resp.set_body("ok:" + r.body); }, o);
    srv.start();
    const u16 p = srv.port();
    auto status = [&](std::string_view req) {
        const auto r = raw_exchange(p, req);
        return r.size() > 12 ? std::stoi(r.substr(9, 3)) : 0;
    };
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), 200);
    CHECK_EQ(status("GET / HTTP/1.0\r\n\r\n"), 200);                                                   // 1.0 needs no Host
    CHECK_EQ(status("GET / HTTP/1.1\r\n\r\n"), 400);                                                   // 1.1 without Host
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 400);                             // duplicate Host
    CHECK_EQ(status("GET / HTTP/2.0\r\nHost: x\r\n\r\n"), 505);
    CHECK_EQ(status("GET /\r\n\r\n"), 400);
    CHECK_EQ(status("GE T / HTTP/1.1\r\nHost: x\r\n\r\n"), 400);
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\nBad Header: 1\r\n\r\n"), 400);
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\nName : v\r\n\r\n"), 400);                            // space before colon
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\nA: 1\r\n folded\r\n\r\n"), 400);                     // obs-fold
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\nA: \x01\r\n\r\n"), 400);                             // control char
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"), 400);  // CL + TE
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\nabcde"), 400);
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n"), 400);
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 4x\r\n\r\n"), 400);
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"), 501);
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n"), 413);
    CHECK_EQ(status(std::string("GET / HTTP/1.1\r\nHost: x\r\nX: ") + std::string(5000, 'a') + "\r\n\r\n"), 431);
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"), 400);  // bad chunk size
    CHECK_EQ(status("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX"), 400);  // missing chunk CRLF
    // valid chunked with extension and trailer
    const auto ch = raw_exchange(p, "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3;ext=1\r\nabc\r\n2\r\nde\r\n0\r\nTrailer: v\r\n\r\n");
    CHECK(ch.find("ok:abcde") != std::string::npos);
    // pipelining: two requests in one write, answered in order
    const auto pl = raw_exchange(p, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nAPOST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nConnection: close\r\n\r\nB");
    CHECK(pl.find("ok:A") != std::string::npos && pl.find("ok:B") != std::string::npos && pl.find("ok:A") < pl.find("ok:B"));
    // Expect: 100-continue
    const auto ex = raw_exchange(p, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nhi");
    CHECK(ex.find("100 Continue") != std::string::npos && ex.find("ok:hi") != std::string::npos);
    // slow client: incomplete request times out with 408 and never reaches the handler
    const int before = handled.load();
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\n"), 408);
    CHECK_EQ(handled.load(), before);
    // garbage and abrupt closes do not kill the server
    raw_exchange(p, std::string(10000, '\xff'));
    raw_exchange(p, "");
    CHECK_EQ(status("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), 200);
}

FLYG_TEST(net_http_client_rejects_bad_servers) {
    auto bad_server = [](std::string reply, std::function<void(http::Client&, u16)> check) {
        auto l = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
        std::thread t([&] {
            auto c = l.accept(Timeout{3000});
            if (!c) return;
            std::byte buf[2048];
            c->first.set_read_timeout(Timeout{1000});
            try { (void)c->first.read_some(buf); c->first.write_all(reply); } catch (const NetworkException&) {}
        });
        http::ClientOptions o;
        o.io_timeout = Timeout{1500};
        http::Client cl(o);
        check(cl, l.local_endpoint().port());
        t.join();
    };
    auto expect_fail = [&](std::string reply) {
        bad_server(std::move(reply), [](http::Client& c, u16 p) { CHECK_THROWS_AS(c.get("http://127.0.0.1:" + std::to_string(p) + "/"), NetworkException); });
    };
    expect_fail("garbage\r\n\r\n");
    expect_fail("HTTP/1.1 20 OK\r\n\r\n");
    expect_fail("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!");
    expect_fail("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort");           // truncated body
    expect_fail("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n");
    expect_fail("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n");
    expect_fail("");
    // body until close
    bad_server("HTTP/1.0 200 OK\r\n\r\nuntil-close", [](http::Client& c, u16 p) { CHECK_EQ(c.get("http://127.0.0.1:" + std::to_string(p) + "/").body, "until-close"); });
    // body limit is enforced
    bad_server("HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n", [](http::Client& c, u16 p) {
        c.options().limits.max_body = 100;
        CHECK_THROWS_AS(c.get("http://127.0.0.1:" + std::to_string(p) + "/"), NetworkException);
    });
    // timeout when the server never answers
    auto l = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    http::ClientOptions o;
    o.io_timeout = Timeout{150};
    http::Client c(o);
    CHECK_THROWS_AS(c.get("http://127.0.0.1:" + std::to_string(l.local_endpoint().port()) + "/"), TimeoutException);
}

FLYG_TEST(net_http_basic_auth_and_cross_origin_redirect) {
    http::Server target(Endpoint(IpAddress::loopback(), 0), [](const http::Request& r, http::Response& resp) { resp.set_body("auth=" + r.headers.get("Authorization")); });
    target.start();
    http::Server origin(Endpoint(IpAddress::loopback(), 0), [&](const http::Request&, http::Response& resp) {
        resp.status = 302;
        resp.headers.set("Location", "http://localhost:" + std::to_string(target.port()) + "/");
    });
    origin.start();
    http::Client c;
    CHECK_EQ(c.get("http://aladdin:open%20sesame@127.0.0.1:" + std::to_string(target.port()) + "/").body, "auth=Basic YWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    http::Headers h;
    h.add("Authorization", "Bearer secret");
    CHECK_EQ(c.get(url_for(origin, "/"), h).body, "auth=");  // different origin: header dropped
}

FLYG_TEST(net_websocket_echo_and_control) {
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [](const http::Request& req, http::Response& resp) {
        if (!ws::is_upgrade_request(req)) {
            resp.set_body("plain");
            return;
        }
        ws::AcceptOptions ao;
        ao.subprotocols = {"chat", "other"};
        ao.ws.max_message_size = 1 << 20;
        ws::accept(req, resp, [](ws::Connection& c) {
            while (auto m = c.receive()) {
                if (!m->binary && m->data == "close-me") { c.close(4001, "bye"); continue; }
                if (m->binary) c.send_binary(m->data);
                else c.send_text("echo:" + m->data);
            }
        }, ao);
    });
    srv.start();
    const std::string url = "ws://127.0.0.1:" + std::to_string(srv.port()) + "/sock";
    ws::ConnectOptions co;
    co.subprotocols = {"nope", "chat"};
    auto c = ws::connect(url, co);
    CHECK_EQ(c->subprotocol(), "chat");
    c->send_text("hello");
    auto m = c->receive();
    CHECK(m && !m->binary && m->data == "echo:hello");
    c->send_text("héllo ✓ 한글");
    CHECK_EQ(c->receive()->data, "echo:héllo ✓ 한글");
    std::string bin(70000, 0);  // 64-bit length path
    for (usize i = 0; i < bin.size(); ++i) bin[i] = static_cast<char>(i * 13);
    c->send_binary(bin);
    m = c->receive();
    CHECK(m && m->binary && m->data == bin);
    std::string mid(300, 'm');  // 16-bit length path
    c->send_binary(mid);
    CHECK_EQ(c->receive()->data, mid);
    // fragmentation is reassembled by the server and echoed as one message
    c->send_fragmented(false, "fragmented message body", 5);
    CHECK_EQ(c->receive()->data, "echo:fragmented message body");
    // ping -> automatic pong
    c->send_ping("pp");
    c->send_text("after-ping");
    CHECK_EQ(c->receive()->data, "echo:after-ping");
    CHECK_EQ(c->last_pong(), "pp");
    // many small messages
    for (int i = 0; i < 200; ++i) c->send_text(std::to_string(i));
    for (int i = 0; i < 200; ++i) CHECK_EQ(c->receive()->data, "echo:" + std::to_string(i));
    // server-initiated close with a code
    c->send_text("close-me");
    CHECK(!c->receive().has_value());
    CHECK_EQ(c->close_code(), 4001);
    CHECK_EQ(c->close_reason(), "bye");
    CHECK_THROWS_AS(c->send_text("late"), NetworkException);
    // a plain HTTP request to the same path still works; non-upgrade handshakes are refused by connect()
    http::Client hc;
    CHECK_EQ(hc.get("http://127.0.0.1:" + std::to_string(srv.port()) + "/x").body, "plain");
    // client-initiated close
    auto c2 = ws::connect(url);
    c2->send_text("x");
    (void)c2->receive();
    c2->close(1000, "done");
    CHECK(!c2->receive().has_value());
    CHECK_EQ(c2->close_code(), 1000);
    // message larger than the server's limit -> 1009
    auto c3 = ws::connect(url);
    c3->send_binary(std::string((1 << 20) + 10, 'x'));
    CHECK(!c3->receive().has_value());
    CHECK_EQ(c3->close_code(), 1009);
    srv.stop();
}

FLYG_TEST(net_websocket_handshake_validation_and_protocol_errors) {
    http::Server srv(Endpoint(IpAddress::loopback(), 0), [](const http::Request& req, http::Response& resp) {
        ws::AcceptOptions ao;
        ao.check_origin = [](const http::Request& r) { return r.headers.get("Origin", "ok") != "http://evil.example"; };
        ws::accept(req, resp, [](ws::Connection& c) { while (c.receive()) {} }, ao);
    });
    srv.start();
    const u16 p = srv.port();
    auto key = [] { return std::string("dGhlIHNhbXBsZSBub25jZQ=="); };
    auto hs = [&](std::string extra = "", std::string version = "13", std::string k = "dGhlIHNhbXBsZSBub25jZQ==") {
        return "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + k + "\r\nSec-WebSocket-Version: " + version + "\r\n" + extra + "Connection: close\r\n\r\n";
    };
    // RFC 6455 section 1.3 example: accept key for "dGhlIHNhbXBsZSBub25jZQ=="
    auto good = raw_exchange(p, hs(), Timeout{500});
    CHECK(good.find("101") != std::string::npos);
    CHECK(good.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(raw_exchange(p, hs("", "12"), Timeout{500}).find("426") != std::string::npos);
    CHECK(raw_exchange(p, hs("", "13", "short"), Timeout{500}).find("400") != std::string::npos);
    CHECK(raw_exchange(p, hs("Origin: http://evil.example\r\n"), Timeout{500}).find("403") != std::string::npos);
    CHECK(raw_exchange(p, "POST / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nContent-Length: 0\r\n\r\n", Timeout{500}).find("405") != std::string::npos);
    (void)key;
    // protocol violations from a raw client socket
    auto frames = [&](std::string_view bytes) {
        auto s = TcpStream::connect(Endpoint(IpAddress::loopback(), p), Timeout{2000});
        s.set_read_timeout(Timeout{1500});
        s.write_all(hs());
        std::string head;
        std::byte b[1];
        while (head.find("\r\n\r\n") == std::string::npos) { if (s.read_some(b) == 0) break; head += static_cast<char>(b[0]); }
        s.write_all(bytes);
        std::string out;
        std::byte buf[512];
        try { for (;;) { const usize n = s.read_some(buf); if (n == 0) break; out.append(reinterpret_cast<const char*>(buf), n); } } catch (const TimeoutException&) {}
        return out;
    };
    auto close_code_of = [&](std::string_view bytes) {
        const auto out = frames(bytes);
        return out.size() >= 4 && (static_cast<u8>(out[0]) & 0x0F) == 8 ? (static_cast<u8>(out[2]) << 8) | static_cast<u8>(out[3]) : -1;
    };
    const char m4[4] = {1, 2, 3, 4};
    auto masked = [&](u8 b0, std::string payload) {
        std::string f;
        f += static_cast<char>(b0);
        f += static_cast<char>(0x80 | payload.size());
        f.append(m4, 4);
        for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ m4[i & 3]);
        return f + payload;
    };
    CHECK_EQ(close_code_of(std::string("\x81\x01x", 3)), 1002);                    // unmasked client frame
    CHECK_EQ(close_code_of(masked(0xC1, "x")), 1002);                              // RSV1 set
    CHECK_EQ(close_code_of(masked(0x83, "x")), 1002);                              // reserved opcode
    CHECK_EQ(close_code_of(masked(0x09, "")), 1002);                               // fragmented control frame (FIN=0 ping)
    CHECK_EQ(close_code_of(masked(0x80, "x")), 1002);                              // continuation without start
    CHECK_EQ(close_code_of(masked(0x01, "a") + masked(0x81, "b")), 1002);          // new data frame inside fragmented message
    CHECK_EQ(close_code_of(masked(0x81, "\xff\xfe")), 1007);                       // invalid UTF-8
    CHECK_EQ(close_code_of(masked(0x88, "\x03\xed")), 1002);                       // close code 1005 is not allowed on the wire
    CHECK_EQ(close_code_of(masked(0x88, "\x03")), 1002);                           // 1-byte close payload
    CHECK_EQ(close_code_of(masked(0x88, std::string("\x03\xe8", 2))), 1000);       // proper close is echoed
    // non-minimal 16-bit length
    std::string nm = std::string("\x82\xFE\x00\x05", 4);
    nm.append(m4, 4);
    nm += std::string(5, 'x');
    CHECK_EQ(close_code_of(nm), 1002);
}

FLYG_TEST(net_websocket_client_rejects_bad_server) {
    auto l = TcpListener::bind(Endpoint(IpAddress::loopback(), 0));
    auto serve_once = [&](std::string reply) {
        std::thread t([&] {
            auto c = l.accept(Timeout{3000});
            if (!c) return;
            std::byte buf[2048];
            c->first.set_read_timeout(Timeout{1000});
            try { (void)c->first.read_some(buf); c->first.write_all(reply); } catch (const NetworkException&) {}
        });
        const std::string url = "ws://127.0.0.1:" + std::to_string(l.local_endpoint().port()) + "/";
        CHECK_THROWS_AS(ws::connect(url), NetworkException);
        t.join();
    };
    serve_once("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    serve_once("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: AAAA\r\n\r\n");
    serve_once("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n\r\n");
    CHECK_THROWS_AS(ws::connect("http://127.0.0.1:1/"), NetworkException);
}
