// flyG - net module: HTTPS / WSS glue (TLS connector for http::Client, TLS accept wrapper for http::Server)
// SPDX-License-Identifier: GPL-2.0-only
//
// Including this header registers the built-in TLS connector as the default for every
// http::Client, so https:// and wss:// URLs work without further setup.
#pragma once

#include <flyg/net/http.hpp>
#include <flyg/net/tls/tls_stream.hpp>

namespace flyg::net {

/// Creates an https connector. `base.server_name` is filled from the URL host when empty and ALPN
/// defaults to http/1.1.
[[nodiscard]] inline http::Connector tls_connector(tls::TlsClientConfig base = {}, bool tolerate_truncation = false) {
    return [base = std::move(base), tolerate_truncation](const Url& u, Timeout connect_timeout) -> std::unique_ptr<ByteStream> {
        auto tcp = std::make_unique<TcpByteStream>(TcpStream::connect(u.host, u.effective_port(), connect_timeout));
        tcp->set_timeouts(connect_timeout, connect_timeout);  // bounds the handshake
        tls::TlsClientConfig cfg = base;
        if (cfg.server_name.empty()) cfg.server_name = u.host;
        if (cfg.alpn.empty()) cfg.alpn = {"http/1.1"};
        auto s = tls::TlsStream::connect(std::move(tcp), cfg);
        s->set_tolerate_truncation(tolerate_truncation);
        return s;
    };
}

/// Makes `client` use a TLS connector with the given configuration (e.g. a custom trust store).
inline void enable_https(http::Client& client, tls::TlsClientConfig cfg = {}, bool tolerate_truncation = false) {
    client.set_secure_connector(tls_connector(std::move(cfg), tolerate_truncation));
}

/// Accept wrapper for `http::ServerOptions::accept_wrapper`: performs the TLS 1.3 handshake on each
/// accepted connection. `config` must outlive the server.
[[nodiscard]] inline std::function<std::unique_ptr<ByteStream>(TcpStream&&)> tls_accept_wrapper(const tls::TlsServerConfig& config, Timeout handshake_timeout = Timeout{10000}) {
    return [&config, handshake_timeout](TcpStream&& tcp) -> std::unique_ptr<ByteStream> {
        auto lower = std::make_unique<TcpByteStream>(std::move(tcp));
        lower->set_timeouts(handshake_timeout, handshake_timeout);
        return tls::TlsStream::accept(std::move(lower), config);
    };
}

namespace detail {
inline const bool https_registered = (http::default_secure_connector() = tls_connector(), true);
}

}  // namespace flyg::net
