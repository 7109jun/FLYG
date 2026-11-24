# flyG `net` module — status, scope and security notes

Header-only, namespace `flyg::net`, GPL-2.0-only. Same blocking-style API on Windows and Linux.

## What is implemented

| Area | Header(s) | Notes |
|---|---|---|
| Addresses, resolution | `address.hpp`, `resolve.hpp` | IPv4/IPv6 parse/format, Endpoint, getaddrinfo wrapper |
| TCP / UDP / raw | `socket.hpp` | Non-blocking internally, poll-based timeouts, `try_*` calls never block, `interrupt()`-safe shutdown |
| Async | `async.hpp` | `EventLoop` (poll + wake socket, timers, post), `AsyncConnection`, `AsyncServer`, `async_connect` |
| DNS | `dns.hpp` | Own wire codec (compression, pointer-loop protection), UDP + TCP fallback, EDNS0, spoof checks, hosts file |
| ICMP ping | `icmp.hpp` | Raw or datagram ICMP sockets; needs privileges / `ping_group_range` |
| HTTP/1.1 | `http.hpp`, `url.hpp` | Strict framing (CL+TE rejected, obs-fold rejected, limits), keep-alive pool, redirects (no https→http, auth stripped cross-origin), 100-continue, chunked streaming, threaded server |
| WebSocket | `websocket.hpp` | RFC 6455: masking, fragmentation, ping/pong, close codes, UTF-8 validation, size limit, lingering close |
| **TLS / HTTPS / WSS** | `https.hpp`, `tls/*.hpp` | Self-implemented (see below); `https://` and `wss://` work out of the box once `<flyg/net/net.hpp>` is included |

## TLS stack (all in-tree; no OpenSSL — OpenSSL 3 is Apache-2.0, which is not GPL-2.0 compatible)

* **Protocol**: TLS 1.3 client + server (HelloRetryRequest, KeyUpdate, ALPN, SNI, `close_notify`, middlebox-compat CCS);
  TLS 1.2 *client* (ECDHE + AEAD suites, extended master secret **required**).
* **Suites**: `TLS_AES_128_GCM_SHA256`, `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`;
  TLS 1.2 `ECDHE-{RSA,ECDSA}-{AES128-GCM,AES256-GCM,CHACHA20-POLY1305}`.
* **Groups**: X25519, secp256r1, secp384r1. **Signatures**: RSA-PSS, RSA-PKCS#1 v1.5 (1.2/cert chains), ECDSA P-256/P-384.
* **X.509**: DER parser, chain building/validation (signature, validity, CA/pathLen, keyUsage, EKU), host-name and IP SAN matching,
  system trust store (Linux PEM bundles incl. `SSL_CERT_FILE`; Windows ROOT store through CryptoAPI).
* **Keys**: PKCS#8, PKCS#1, SEC1 PEM/DER (unencrypted).

### Not implemented (by design or yet)
HTTP/2, gzip/deflate (client sends `Accept-Encoding: identity`), WebSocket permessage-deflate, TLS session resumption / PSK / 0-RTT,
client certificates (an empty Certificate is sent when requested), TLS 1.2 *server*, renegotiation, FFDHE / RSA key exchange,
DTLS, OCSP/CRL revocation checking, name constraints, policy processing, Ed25519/Ed448 certificates, SHA-1 signatures (rejected),
encrypted private keys.

### Security notes (read before exposing a server)
* BigInt, RSA and EC code is **not constant-time**; AES uses S-box tables (not cache-timing hardened). This is acceptable for
  verifying peers, but a server that signs with a long-lived key is exposed to a co-resident local timing attacker. Run
  untrusted tenants on separate hosts, or put a hardened TLS terminator in front.
* `TlsClientConfig::verify = false` disables *all* certificate checks and exists for tests only.
* A TCP close without `close_notify` is an error by default (truncation protection); `TlsStream::set_tolerate_truncation(true)`
  relaxes this for servers that do not send it.
* The implementation has been validated against reference vectors (RFC 6979, RFC 7748, NIST GCM, cryptography-library
  generated AEAD/RSA/ECDSA vectors) and by interoperating with the `openssl` CLI in both directions, but it has **not** had an
  independent security audit.

## Verification matrix (net + TLS)

| Target | Result |
|---|---|
| ELF, g++ 13, Debug, ASan+UBSan, `-Werror` | full suite: 204 tests, 0 failed |
| clang 18 | builds clean; net/tls/x509 tests pass |
| TSan (g++ 13) | `net_*`, `tls_*`, `x509_*` clean (found and fixed a close-vs-read race, see `ByteStream::interrupt`) |
| PE (mingw) under wine | passed before the last two fixes (socket shutdown-on-close, lingering WS close); **not re-run** — wine became unusable in the build sandbox |
| aarch64 under qemu-user | see status report |
| Real Windows / MSVC | **not tested** |
