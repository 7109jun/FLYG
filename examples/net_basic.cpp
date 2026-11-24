// SPDX-License-Identifier: GPL-2.0-only
// flyG net module example: loopback HTTP server + client, WebSocket echo, UDP, DNS packet codec.
// Pass an https:// URL as the first argument to fetch it with the system trust store (needs internet).
#include <flyg/net/net.hpp>

#include <cstdio>
#include <string>

using namespace flyg;
using namespace flyg::net;

int main(int argc, char** argv) {
    try {
        // --- HTTP server on an ephemeral loopback port, with a WebSocket echo endpoint ---
        http::Server server(Endpoint(IpAddress::loopback(), 0), [](const http::Request& req, http::Response& resp) {
            if (ws::is_upgrade_request(req)) {
                ws::accept(req, resp, [](ws::Connection& c) {
                    while (auto m = c.receive()) c.send_text("echo:" + m->data);
                });
                return;
            }
            resp.set_body("Hello from flyG, you asked for " + req.target + "\n");
        });
        server.start();
        const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

        http::Client client;
        const auto r = client.get(base + "/index?x=1");
        std::printf("HTTP %d: %s", r.status, r.body.c_str());

        auto sock = ws::connect(client, "ws://127.0.0.1:" + std::to_string(server.port()) + "/chat");
        sock->send_text("ping");
        std::printf("WebSocket reply: %s\n", sock->receive()->data.c_str());
        sock->close();
        server.stop();

        // --- UDP round trip on loopback ---
        auto a = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
        auto b = UdpSocket::bind(Endpoint(IpAddress::loopback(), 0));
        const std::string msg = "datagram";
        b.send_to(msg, a.local_endpoint());
        std::byte buf[64];
        if (auto got = a.receive_for(buf, Timeout{2000})) std::printf("UDP received %zu bytes\n", got->size);

        // --- DNS wire codec (no network needed) ---
        const auto q = dns::Message::query("example.com", dns::Type::A, 0x1234);
        const auto wire = q.encode();
        std::printf("DNS query for example.com is %zu bytes on the wire\n", wire.size());

        // --- optional: HTTPS with system roots ---
        if (argc > 1) {
            http::Client https_client;  // https works out of the box: <flyg/net/https.hpp> installs the TLS connector
            const auto resp = https_client.get(argv[1]);
            std::printf("%s -> HTTP %d, %zu bytes\n", argv[1], resp.status, resp.body.size());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
