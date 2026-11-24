// flyG - net/tls: the TLS connection object (record layer + handshakes). See tls.hpp for scope.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <flyg/net/tls/tls.hpp>

namespace flyg::net::tls {
namespace detail {
struct Rec {
    u8 type = 0;
    Bytes data;
};
}  // namespace detail

class TlsStream final : public ByteStream {
public:
    /// Performs a client handshake over `lower` (blocking; honours the stream's timeouts).
    [[nodiscard]] static std::unique_ptr<TlsStream> connect(std::unique_ptr<ByteStream> lower, const TlsClientConfig& cfg) {
        std::unique_ptr<TlsStream> s(new TlsStream(std::move(lower)));
        s->guarded([&] { s->client_handshake(cfg); });
        return s;
    }
    /// Performs a TLS 1.3 server handshake over `lower`.
    [[nodiscard]] static std::unique_ptr<TlsStream> accept(std::unique_ptr<ByteStream> lower, const TlsServerConfig& cfg) {
        std::unique_ptr<TlsStream> s(new TlsStream(std::move(lower)));
        s->guarded([&] { s->server_handshake(cfg); });
        return s;
    }
    using ByteStream::write_all;
    ~TlsStream() override { close(); }
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    usize read_some(std::span<std::byte> buf) override {
        if (buf.empty()) return 0;
        int empty_records = 0;
        while (apos_ >= app_.size()) {
            app_.clear();
            apos_ = 0;
            if (got_close_) return 0;
            detail::Rec r;
            try {
                r = read_record();
            } catch (const detail::ProtocolError& e) {
                abort_with(e.alert);
                throw;
            } catch (const TlsException&) {
                if (eof_clean_ && tolerate_truncation_) return 0;
                throw;
            }
            switch (r.type) {
                case detail::ct_appdata:
                    if (r.data.empty() && ++empty_records > 1000) abort_with(detail::alert::unexpected_message), throw TlsException("too many empty records");
                    app_ = std::move(r.data);
                    break;
                case detail::ct_alert:
                    if (handle_alert(r.data)) return 0;
                    break;
                case detail::ct_handshake:
                    if (!tls13_) { abort_with(detail::alert::unexpected_message); throw TlsException("renegotiation is not supported"); }
                    try { post_handshake(r.data); } catch (const detail::ProtocolError& e) { abort_with(e.alert); throw; }
                    break;
                default:
                    abort_with(detail::alert::unexpected_message);
                    throw TlsException("unexpected record type");
            }
        }
        const usize n = std::min(buf.size(), app_.size() - apos_);
        std::memcpy(buf.data(), app_.data() + apos_, n);
        apos_ += n;
        return n;
    }

    void write_all(std::span<const std::byte> data) override {
        std::lock_guard lk(wmu_);
        if (closed_ || sent_close_) throw TlsException("write on a closed connection");
        if (tls13_ && wr_.seq >= (1ull << 24)) rotate_write_locked(true);
        for (usize off = 0; off < data.size() || (data.empty() && off == 0); off += 16384) {
            write_record(detail::ct_appdata, data.subspan(off, std::min<usize>(16384, data.size() - off)));
            if (data.empty()) break;
        }
        flush();
    }
    void shutdown_write() override {
        send_close_notify();
        lower_->shutdown_write();
    }
    void close() noexcept override {
        if (closed_.exchange(true)) return;
        send_close_notify();
        lower_->close();
    }
    void interrupt() noexcept override { lower_->interrupt(); }
    void set_timeouts(Timeout r, Timeout w) override { lower_->set_timeouts(r, w); }
    [[nodiscard]] Endpoint peer_endpoint() const override { return lower_->peer_endpoint(); }
    [[nodiscard]] bool is_secure() const noexcept override { return true; }

    /// When true, a TCP close without close_notify is reported as a normal end of stream.
    void set_tolerate_truncation(bool v) noexcept { tolerate_truncation_ = v; }
    [[nodiscard]] const char* version() const noexcept { return tls13_ ? "TLSv1.3" : "TLSv1.2"; }
    [[nodiscard]] const char* cipher_suite() const noexcept { return suite_ ? suite_->name : ""; }
    [[nodiscard]] const std::string& alpn() const noexcept { return alpn_; }
    [[nodiscard]] const std::optional<Certificate>& peer_certificate() const noexcept { return peer_cert_; }

private:
    using Wr = detail::Wr;
    using Rd = detail::Rd;

    struct Dir {
        detail::AeadCipher c;
        Bytes iv, secret;
        u64 seq = 0;
        bool on = false;
    };

    explicit TlsStream(std::unique_ptr<ByteStream> lower) : lower_(std::move(lower)) {}

    template <class F>
    void guarded(F&& f) {
        try {
            f();
        } catch (const detail::ProtocolError& e) {
            abort_with(e.alert);
            closed_ = true;
            lower_->close();
            throw;
        } catch (const CertificateException&) {
            abort_with(detail::alert::bad_certificate);
            closed_ = true;
            lower_->close();
            throw;
        } catch (...) {
            closed_ = true;
            lower_->close();
            throw;
        }
    }

    // ---- raw I/O -----------------------------------------------------------------------------------------------

    void fill(usize n) {
        if (rpos_ > 0 && rpos_ == rbuf_.size()) {
            rbuf_.clear();
            rpos_ = 0;
        }
        while (rbuf_.size() - rpos_ < n) {
            const usize old = rbuf_.size();
            rbuf_.resize(old + 16384);
            usize got = 0;
            try {
                got = lower_->read_some(std::span<std::byte>(rbuf_).subspan(old));
            } catch (...) {
                rbuf_.resize(old);
                throw;
            }
            rbuf_.resize(old + got);
            if (got == 0) {
                eof_clean_ = (rbuf_.size() == rpos_);
                throw TlsException(eof_clean_ ? "connection closed without close_notify (possible truncation)" : "connection closed in the middle of a record");
            }
        }
    }

    Bytes nonce_for(const Dir& d) const {
        Bytes n = d.iv;
        for (int i = 0; i < 8; ++i) n[n.size() - 1 - static_cast<usize>(i)] ^= static_cast<std::byte>(d.seq >> (8 * i));
        return n;
    }

    detail::Rec read_record() {
        for (;;) {
            fill(5);
            const u8 type = static_cast<u8>(rbuf_[rpos_]);
            const core::u16 ver = static_cast<core::u16>((static_cast<u8>(rbuf_[rpos_ + 1]) << 8) | static_cast<u8>(rbuf_[rpos_ + 2]));
            const usize len = (static_cast<usize>(static_cast<u8>(rbuf_[rpos_ + 3])) << 8) | static_cast<u8>(rbuf_[rpos_ + 4]);
            if (type < 20 || type > 23 || (ver >> 8) != 3) detail::fail(detail::alert::unexpected_message, "not a TLS record");
            if (len > 16384 + 2048 || (rd_.on && tls13_ && len > 16384 + 256)) detail::fail(detail::alert::record_overflow, "record too large");
            fill(5 + len);
            u8 hdr[5];
            for (int i = 0; i < 5; ++i) hdr[i] = static_cast<u8>(rbuf_[rpos_ + static_cast<usize>(i)]);
            Bytes payload(rbuf_.begin() + static_cast<std::ptrdiff_t>(rpos_ + 5), rbuf_.begin() + static_cast<std::ptrdiff_t>(rpos_ + 5 + len));
            rpos_ += 5 + len;
            if (!rd_.on) {
                if (len > 16384) detail::fail(detail::alert::record_overflow, "record too large");
                return {type, std::move(payload)};
            }
            if (tls13_) {
                if (type == detail::ct_ccs && !hs_done_ && payload.size() == 1 && payload[0] == std::byte{1}) continue;  // middlebox compatibility
                if (type != detail::ct_appdata) detail::fail(detail::alert::unexpected_message, "unencrypted record after key exchange");
                const auto pt = rd_.c.open(nonce_for(rd_), ByteView(reinterpret_cast<const std::byte*>(hdr), 5), payload);
                if (!pt) detail::fail(detail::alert::bad_record_mac, "bad record MAC");
                ++rd_.seq;
                Bytes inner = *pt;
                while (!inner.empty() && inner.back() == std::byte{0}) inner.pop_back();
                if (inner.empty()) detail::fail(detail::alert::unexpected_message, "record without content type");
                const u8 itype = static_cast<u8>(inner.back());
                inner.pop_back();
                if (inner.size() > 16384) detail::fail(detail::alert::record_overflow, "record too large");
                return {itype, std::move(inner)};
            }
            // TLS 1.2 AEAD
            if (type == detail::ct_ccs) return {type, std::move(payload)};
            Bytes nonce;
            ByteView ct(payload);
            if (suite_->chacha) {
                nonce = nonce_for(rd_);
            } else {
                if (payload.size() < 8 + 16) detail::fail(detail::alert::bad_record_mac, "short record");
                nonce = rd_.iv;
                nonce.insert(nonce.end(), payload.begin(), payload.begin() + 8);
                ct = ct.subspan(8);
            }
            if (ct.size() < 16) detail::fail(detail::alert::bad_record_mac, "short record");
            const usize plen = ct.size() - 16;
            Wr aad;
            for (int i = 7; i >= 0; --i) aad.u8_(static_cast<u8>(rd_.seq >> (8 * i)));
            aad.u8_(type).u16_(0x0303).u16_(static_cast<core::u16>(plen));
            const auto pt = rd_.c.open(nonce, aad.b, ct);
            if (!pt) detail::fail(detail::alert::bad_record_mac, "bad record MAC");
            ++rd_.seq;
            return {type, *pt};
        }
    }

    void write_record(u8 type, ByteView data) {
        Wr w;
        if (!wr_.on || type == detail::ct_ccs) {
            w.u8_(type).u16_(0x0303).u16_(static_cast<core::u16>(data.size())).bytes(data);
        } else if (tls13_) {
            Bytes inner(data.begin(), data.end());
            inner.push_back(static_cast<std::byte>(type));
            Wr hdr;
            hdr.u8_(detail::ct_appdata).u16_(0x0303).u16_(static_cast<core::u16>(inner.size() + 16));
            const Bytes ct = wr_.c.seal(nonce_for(wr_), hdr.b, inner);
            ++wr_.seq;
            w.bytes(hdr.b).bytes(ct);
        } else {
            Bytes nonce;
            Wr aad;
            for (int i = 7; i >= 0; --i) aad.u8_(static_cast<u8>(wr_.seq >> (8 * i)));
            aad.u8_(type).u16_(0x0303).u16_(static_cast<core::u16>(data.size()));
            Bytes explicit_part;
            if (suite_->chacha) {
                nonce = nonce_for(wr_);
            } else {
                nonce = wr_.iv;
                for (int i = 7; i >= 0; --i) explicit_part.push_back(static_cast<std::byte>(wr_.seq >> (8 * i)));
                nonce.insert(nonce.end(), explicit_part.begin(), explicit_part.end());
            }
            const Bytes ct = wr_.c.seal(nonce, aad.b, data);
            ++wr_.seq;
            w.u8_(type).u16_(0x0303).u16_(static_cast<core::u16>(explicit_part.size() + ct.size())).bytes(explicit_part).bytes(ct);
        }
        wbuf_.insert(wbuf_.end(), w.b.begin(), w.b.end());
    }
    void flush() {
        if (wbuf_.empty()) return;
        Bytes out;
        out.swap(wbuf_);
        lower_->write_all(out);
    }
    void send_hs(ByteView msg) { write_record(detail::ct_handshake, msg); }
    void send_compat_ccs() {
        if (ccs_sent_) return;
        ccs_sent_ = true;
        const std::byte one{1};
        write_record(detail::ct_ccs, ByteView(&one, 1));
    }

    void send_alert(u8 desc, bool fatal) noexcept {
        try {
            std::lock_guard lk(wmu_);
            if (alert_sent_ && fatal) return;
            const std::byte a[2] = {static_cast<std::byte>(fatal ? 2 : 1), static_cast<std::byte>(desc)};
            write_record(detail::ct_alert, ByteView(a, 2));
            flush();
            if (fatal) alert_sent_ = true;
        } catch (...) {
        }
    }
    void abort_with(u8 desc) noexcept { send_alert(desc, true); }
    void send_close_notify() noexcept {
        if (sent_close_.exchange(true)) return;
        send_alert(detail::alert::close_notify, false);
    }
    /// Returns true on close_notify (clean EOF); throws on any other alert.
    bool handle_alert(const Bytes& d) {
        if (d.size() != 2) detail::fail(detail::alert::decode_error, "malformed alert");
        const u8 desc = static_cast<u8>(d[1]);
        if (desc == detail::alert::close_notify) {
            got_close_ = true;
            return true;
        }
        got_close_ = true;
        throw TlsException("peer sent alert " + std::to_string(desc));
    }

    // ---- handshake plumbing ------------------------------------------------------------------------------------

    /// Returns the next complete handshake message (header included).
    Bytes next_hs() {
        for (;;) {
            if (hs_buf_.size() >= 4) {
                const usize len = (static_cast<usize>(static_cast<u8>(hs_buf_[1])) << 16) | (static_cast<usize>(static_cast<u8>(hs_buf_[2])) << 8) | static_cast<u8>(hs_buf_[3]);
                if (len > (1u << 17)) detail::fail(detail::alert::illegal_parameter, "handshake message too large");
                if (hs_buf_.size() >= 4 + len) {
                    Bytes msg(hs_buf_.begin(), hs_buf_.begin() + static_cast<std::ptrdiff_t>(4 + len));
                    hs_buf_.erase(hs_buf_.begin(), hs_buf_.begin() + static_cast<std::ptrdiff_t>(4 + len));
                    return msg;
                }
            }
            detail::Rec r = read_record();
            if (r.type == detail::ct_handshake) {
                hs_buf_.insert(hs_buf_.end(), r.data.begin(), r.data.end());
            } else if (r.type == detail::ct_alert) {
                (void)handle_alert(r.data);
                throw TlsException("connection closed during the handshake");
            } else if (r.type == detail::ct_ccs && allow_ccs_ && hs_buf_.empty() && r.data.size() == 1 && r.data[0] == std::byte{1}) {
                continue;
            } else {
                detail::fail(detail::alert::unexpected_message, "unexpected record during the handshake");
            }
        }
    }
    static ByteView body_of(const Bytes& msg) { return ByteView(msg).subspan(4); }
    static u8 type_of(const Bytes& msg) { return static_cast<u8>(msg[0]); }
    static Bytes make_hs(u8 type, ByteView body) {
        Wr w;
        w.u8_(type).u24_(static_cast<u32>(body.size())).bytes(body);
        return std::move(w.b);
    }
    void add_transcript(ByteView m) { transcript_.insert(transcript_.end(), m.begin(), m.end()); }
    [[nodiscard]] Bytes th() const { return hash_data(hash_, transcript_); }
    usize hlen() const { return hash_size(hash_); }

    void install(Dir& d, ByteView secret) {
        d.secret.assign(secret.begin(), secret.end());
        const Bytes key = detail::hkdf_expand_label(hash_, secret, "key", {}, suite_->key_len);
        d.iv = detail::hkdf_expand_label(hash_, secret, "iv", {}, 12);
        d.c = detail::AeadCipher(*suite_, key);
        d.seq = 0;
        d.on = true;
    }
    Bytes finished_mac(ByteView base_secret) const {
        const Bytes fk = detail::hkdf_expand_label(hash_, base_secret, "finished", {}, hlen());
        return detail::hmac_bytes(hash_, fk, th());
    }
    Bytes cert_verify_content(bool server) const {
        Bytes c(64, std::byte{0x20});
        const std::string_view label = server ? "TLS 1.3, server CertificateVerify" : "TLS 1.3, client CertificateVerify";
        for (char ch : label) c.push_back(static_cast<std::byte>(ch));
        c.push_back(std::byte{0});
        const Bytes t = th();
        c.insert(c.end(), t.begin(), t.end());
        return c;
    }
    void rotate_write_locked(bool announce) {
        if (announce) {
            const Bytes msg = make_hs(detail::hs_key_update, Bytes{std::byte{0}});
            write_record(detail::ct_handshake, msg);
            flush();
        }
        install(wr_, detail::hkdf_expand_label(hash_, wr_.secret, "traffic upd", {}, hlen()));
    }
    void post_handshake(const Bytes& data) {
        hs_buf_.insert(hs_buf_.end(), data.begin(), data.end());
        while (hs_buf_.size() >= 4) {
            const usize len = (static_cast<usize>(static_cast<u8>(hs_buf_[1])) << 16) | (static_cast<usize>(static_cast<u8>(hs_buf_[2])) << 8) | static_cast<u8>(hs_buf_[3]);
            if (len > (1u << 17)) detail::fail(detail::alert::illegal_parameter, "handshake message too large");
            if (hs_buf_.size() < 4 + len) return;
            const Bytes msg(hs_buf_.begin(), hs_buf_.begin() + static_cast<std::ptrdiff_t>(4 + len));
            hs_buf_.erase(hs_buf_.begin(), hs_buf_.begin() + static_cast<std::ptrdiff_t>(4 + len));
            if (type_of(msg) == detail::hs_new_session_ticket) continue;  // resumption is not implemented
            if (type_of(msg) == detail::hs_key_update) {
                if (msg.size() != 5 || (msg[4] != std::byte{0} && msg[4] != std::byte{1})) detail::fail(detail::alert::illegal_parameter, "bad KeyUpdate");
                install(rd_, detail::hkdf_expand_label(hash_, rd_.secret, "traffic upd", {}, hlen()));
                if (msg[4] == std::byte{1}) {
                    std::lock_guard lk(wmu_);
                    rotate_write_locked(true);
                }
                continue;
            }
            detail::fail(detail::alert::unexpected_message, "unexpected post-handshake message");
        }
    }

    // ---- ClientHello -------------------------------------------------------------------------------------------

    static bool is_ip_literal(std::string_view s) { return IpAddress::parse(s).has_value(); }

    Bytes build_client_hello(const TlsClientConfig& cfg, ByteView random, ByteView sid, const detail::KeyShare& ks, ByteView cookie) {
        Wr b;
        b.u16_(0x0303).bytes(random).vec8([&](Wr& w) { w.bytes(sid); });
        b.vec16([&](Wr& w) {
            for (const auto& s : detail::suites)
                if (s.tls13 || cfg.allow_tls12) w.u16_(s.id);
        });
        b.vec8([](Wr& w) { w.u8_(0); });
        b.vec16([&](Wr& e) {
            if (!cfg.server_name.empty() && !is_ip_literal(cfg.server_name))
                e.ext(detail::ext_server_name, [&](Wr& w) { w.vec16([&](Wr& l) { l.u8_(0).vec16([&](Wr& n) { n.str(cfg.server_name); }); }); });
            e.ext(detail::ext_supported_versions, [&](Wr& w) { w.vec8([&](Wr& v) { v.u16_(0x0304); if (cfg.allow_tls12) v.u16_(0x0303); }); });
            e.ext(detail::ext_supported_groups, [](Wr& w) { w.vec16([](Wr& g) { g.u16_(29).u16_(23).u16_(24); }); });
            e.ext(detail::ext_ec_point_formats, [](Wr& w) { w.vec8([](Wr& f) { f.u8_(0); }); });
            e.ext(detail::ext_sig_algs, [](Wr& w) {
                w.vec16([](Wr& s) {
                    for (core::u16 v : {detail::sig_rsa_pss_sha256, detail::sig_rsa_pss_sha384, detail::sig_rsa_pss_sha512, detail::sig_ecdsa_p256_sha256, detail::sig_ecdsa_p384_sha384,
                                        detail::sig_rsa_pkcs1_sha256, detail::sig_rsa_pkcs1_sha384, detail::sig_rsa_pkcs1_sha512})
                        s.u16_(v);
                });
            });
            e.ext(detail::ext_key_share, [&](Wr& w) { w.vec16([&](Wr& l) { l.u16_(ks.group).vec16([&](Wr& k) { k.bytes(ks.pub); }); }); });
            if (cfg.allow_tls12) e.ext(detail::ext_ems, [](Wr&) {});
            e.ext(detail::ext_renegotiation_info, [](Wr& w) { w.u8_(0); });
            if (!cfg.alpn.empty())
                e.ext(detail::ext_alpn, [&](Wr& w) { w.vec16([&](Wr& l) { for (const auto& p : cfg.alpn) l.vec8([&](Wr& n) { n.str(p); }); }); });
            if (!cookie.empty()) e.ext(detail::ext_cookie, [&](Wr& w) { w.vec16([&](Wr& c) { c.bytes(cookie); }); });
        });
        return make_hs(detail::hs_client_hello, b.b);
    }

    struct ServerHelloInfo {
        core::u16 suite = 0;
        core::u16 version = 0x0303;
        bool is_hrr = false, has_version_ext = false, ems = false, sid_echoed = false;
        core::u16 group = 0;
        Bytes key_share, cookie, random;
    };
    static const Bytes& hrr_random() {
        static const Bytes r = data::hex_decode("CF21AD74E59A6111BE1D8C021E65B891C2A211167ABB8C5E079E09E2C8A8339C");
        return r;
    }
    ServerHelloInfo parse_server_hello(const Bytes& msg, ByteView sid) {
        Rd r(body_of(msg));
        ServerHelloInfo si;
        if (r.u16_() != 0x0303) detail::fail(detail::alert::illegal_parameter, "bad legacy_version");
        const ByteView rnd = r.bytes(32);
        si.random.assign(rnd.begin(), rnd.end());
        const ByteView echoed = r.bytes(r.u8_());
        si.sid_echoed = std::ranges::equal(echoed, sid);
        si.suite = r.u16_();
        if (r.u8_() != 0) detail::fail(detail::alert::illegal_parameter, "compression is not supported");
        si.is_hrr = si.random == hrr_random();
        if (!r.empty()) {
            Rd ex = r.sub16();
            if (!r.empty()) detail::fail(detail::alert::decode_error, "trailing data in ServerHello");
            std::vector<core::u16> seen;
            while (!ex.empty()) {
                const core::u16 t = ex.u16_();
                Rd d = ex.sub16();
                if (std::ranges::find(seen, t) != seen.end()) detail::fail(detail::alert::illegal_parameter, "duplicate extension");
                seen.push_back(t);
                if (t == detail::ext_supported_versions) {
                    si.has_version_ext = true;
                    si.version = d.u16_();
                } else if (t == detail::ext_key_share) {
                    si.group = d.u16_();
                    if (!d.empty()) {
                        const ByteView k = d.bytes(d.u16_());
                        si.key_share.assign(k.begin(), k.end());
                    }
                } else if (t == detail::ext_cookie) {
                    const ByteView c = d.bytes(d.u16_());
                    si.cookie.assign(c.begin(), c.end());
                } else if (t == detail::ext_ems) {
                    si.ems = true;
                }
            }
        }
        return si;
    }

    // ---- certificate handling (client) -------------------------------------------------------------------------

    std::vector<Bytes> parse_certificate_list(Rd r, bool tls13, Bytes* context = nullptr) {
        if (tls13) {
            const ByteView ctx = r.bytes(r.u8_());
            if (context) context->assign(ctx.begin(), ctx.end());
        }
        Rd list = r.sub24();
        if (!r.empty()) detail::fail(detail::alert::decode_error, "trailing data in Certificate");
        std::vector<Bytes> chain;
        while (!list.empty()) {
            const ByteView der = list.bytes(list.u24_());
            chain.emplace_back(der.begin(), der.end());
            if (tls13) (void)list.bytes(list.u16_());
        }
        return chain;
    }
    void check_peer_chain(const std::vector<Bytes>& chain, const TlsClientConfig& cfg) {
        if (chain.empty()) detail::fail(detail::alert::bad_certificate, "the server sent no certificate");
        if (cfg.verify) {
            if (cfg.server_name.empty()) throw TlsException("a server name is required when certificate verification is enabled");
            VerifyOptions vo;
            vo.host = cfg.server_name;
            vo.now = cfg.now;
            const TrustStore& ts = cfg.trust ? *cfg.trust : TrustStore::system();
            peer_cert_ = verify_chain(chain, ts, vo);
        } else {
            peer_cert_ = Certificate::parse(chain[0]);
        }
    }

    // ---- client handshake --------------------------------------------------------------------------------------

    void client_handshake(const TlsClientConfig& cfg) {
        using namespace detail;
        Bytes random(32), sid(32);
        system::secure_random(random);
        system::secure_random(sid);
        offered_alpn_ = cfg.alpn;
        KeyShare ks = KeyShare::generate(29);
        std::vector<core::u16> tried_groups{29};
        Bytes cookie;
        Bytes ch = build_client_hello(cfg, random, sid, ks, {});
        add_transcript(ch);
        send_hs(ch);
        flush();

        Bytes sh = next_hs();
        if (type_of(sh) != hs_server_hello) fail(alert::unexpected_message, "expected ServerHello");
        ServerHelloInfo si = parse_server_hello(sh, sid);

        if (si.is_hrr) {
            if (si.version != 0x0304) fail(alert::illegal_parameter, "HelloRetryRequest without TLS 1.3");
            if (!si.sid_echoed) fail(alert::illegal_parameter, "session id echo mismatch");
            const Suite* s = find_suite(si.suite);
            if (!s || !s->tls13) fail(alert::illegal_parameter, "HelloRetryRequest with a bad cipher suite");
            if (std::ranges::find(tried_groups, si.group) != tried_groups.end() || (si.group != 23 && si.group != 24 && si.group != 29))
                fail(alert::illegal_parameter, "HelloRetryRequest selects an unusable group");
            suite_ = s;
            hash_ = s->hash;
            tls13_ = true;
            allow_ccs_ = true;
            // transcript := message_hash(CH1) || HRR
            Bytes mh = make_hs(hs_message_hash, th());
            transcript_ = mh;
            add_transcript(sh);
            send_compat_ccs();
            ks = KeyShare::generate(si.group);
            tried_groups.push_back(si.group);
            cookie = si.cookie;
            ch = build_client_hello(cfg, random, sid, ks, cookie);
            add_transcript(ch);
            send_hs(ch);
            flush();
            sh = next_hs();
            if (type_of(sh) != hs_server_hello) fail(alert::unexpected_message, "expected ServerHello");
            const u16 hrr_suite = si.suite;
            si = parse_server_hello(sh, sid);
            if (si.is_hrr) fail(alert::unexpected_message, "second HelloRetryRequest");
            if (!si.sid_echoed) fail(alert::illegal_parameter, "session id echo mismatch");
            if (si.suite != hrr_suite) fail(alert::illegal_parameter, "cipher suite changed after HelloRetryRequest");
        }

        const Suite* s = find_suite(si.suite);
        if (!s) fail(alert::illegal_parameter, "server selected an unknown cipher suite");
        const bool down = si.random.size() == 32 && std::equal(si.random.begin() + 24, si.random.begin() + 31, reinterpret_cast<const std::byte*>("DOWNGRD"));
        if (si.has_version_ext && si.version == 0x0304) {
            if (!s->tls13) fail(alert::illegal_parameter, "TLS 1.3 with a TLS 1.2 cipher suite");
            if (!si.sid_echoed) fail(alert::illegal_parameter, "session id echo mismatch");
            suite_ = s;
            hash_ = s->hash;
            tls13_ = true;
            allow_ccs_ = true;
            if (si.group != ks.group || si.key_share.empty()) fail(alert::illegal_parameter, "server key share does not match our offer");
            add_transcript(sh);
            client_tls13(cfg, ks, si);
        } else {
            if (si.has_version_ext) fail(alert::illegal_parameter, "unsupported TLS version selected");
            if (!cfg.allow_tls12) fail(alert::protocol_version, "server does not support TLS 1.3");
            if (down && (si.random.back() == std::byte{1} || si.random.back() == std::byte{0})) fail(alert::illegal_parameter, "TLS downgrade detected");
            if (s->tls13) fail(alert::illegal_parameter, "TLS 1.2 with a TLS 1.3 cipher suite");
            if (!si.ems) fail(alert::handshake_failure, "server does not support extended master secret");
            if (tried_groups.size() > 1) fail(alert::illegal_parameter, "TLS 1.2 after HelloRetryRequest");
            suite_ = s;
            hash_ = s->hash;
            tls13_ = false;
            add_transcript(sh);
            client_tls12(cfg, si, random);
        }
        hs_done_ = true;
        hs_buf_.clear();
    }

    void client_tls13(const TlsClientConfig& cfg, const detail::KeyShare& ks, const ServerHelloInfo& si) {
        using namespace detail;
        send_compat_ccs();
        const Bytes shared = ks.agree(si.key_share);
        const usize L = hlen();
        const Bytes zeros(L, std::byte{0});
        const Bytes empty_hash = hash_data(hash_, {});
        const Bytes early = hkdf_extract_bytes(hash_, {}, zeros);
        const Bytes derived = hkdf_expand_label(hash_, early, "derived", empty_hash, L);
        const Bytes hs_secret = hkdf_extract_bytes(hash_, derived, shared);
        const Bytes c_hs = hkdf_expand_label(hash_, hs_secret, "c hs traffic", th(), L);
        const Bytes s_hs = hkdf_expand_label(hash_, hs_secret, "s hs traffic", th(), L);
        if (!hs_buf_.empty()) fail(alert::unexpected_message, "data after ServerHello");
        install(rd_, s_hs);

        // EncryptedExtensions
        Bytes m = next_hs();
        if (type_of(m) != hs_encrypted_extensions) fail(alert::unexpected_message, "expected EncryptedExtensions");
        {
            Rd r(body_of(m));
            Rd ex = r.sub16();
            while (!ex.empty()) {
                const core::u16 t = ex.u16_();
                Rd d = ex.sub16();
                if (t == ext_alpn) {
                    Rd list = d.sub16();
                    const ByteView p = list.bytes(list.u8_());
                    alpn_.assign(reinterpret_cast<const char*>(p.data()), p.size());
                    if (!list.empty() || std::ranges::find(offered_alpn_, alpn_) == offered_alpn_.end()) fail(alert::illegal_parameter, "unsolicited ALPN protocol");
                } else if (t == ext_supported_versions || t == ext_key_share || t == ext_cookie) {
                    fail(alert::illegal_parameter, "extension not allowed in EncryptedExtensions");
                }
            }
        }
        add_transcript(m);

        m = next_hs();
        Bytes request_context;
        bool cert_requested = false;
        if (type_of(m) == hs_certificate_request) {
            Rd r(body_of(m));
            const ByteView ctx = r.bytes(r.u8_());
            request_context.assign(ctx.begin(), ctx.end());
            cert_requested = true;
            add_transcript(m);
            m = next_hs();
        }
        if (type_of(m) != hs_certificate) fail(alert::unexpected_message, "expected Certificate");
        const auto chain = parse_certificate_list(Rd(body_of(m)), true);
        check_peer_chain(chain, cfg);
        add_transcript(m);

        m = next_hs();
        if (type_of(m) != hs_certificate_verify) fail(alert::unexpected_message, "expected CertificateVerify");
        {
            Rd r(body_of(m));
            const core::u16 scheme = r.u16_();
            const ByteView sig = r.bytes(r.u16_());
            if (!r.empty()) fail(alert::decode_error, "trailing data in CertificateVerify");
            if (!scheme_ok_for_tls13(scheme)) fail(alert::illegal_parameter, "unacceptable signature scheme");
            if (!verify_signature(scheme, peer_cert_->key, cert_verify_content(true), sig, true)) fail(alert::decrypt_error, "CertificateVerify signature is invalid");
        }
        add_transcript(m);

        m = next_hs();
        if (type_of(m) != hs_finished) fail(alert::unexpected_message, "expected Finished");
        const Bytes expect = finished_mac(s_hs);
        if (body_of(m).size() != L || !data::constant_time_equal(body_of(m), expect)) fail(alert::decrypt_error, "server Finished is invalid");
        add_transcript(m);
        if (!hs_buf_.empty()) fail(alert::unexpected_message, "data after Finished");

        const Bytes derived2 = hkdf_expand_label(hash_, hs_secret, "derived", empty_hash, L);
        const Bytes master = hkdf_extract_bytes(hash_, derived2, zeros);
        const Bytes c_ap = hkdf_expand_label(hash_, master, "c ap traffic", th(), L);
        const Bytes s_ap = hkdf_expand_label(hash_, master, "s ap traffic", th(), L);

        std::lock_guard lk(wmu_);
        install(wr_, c_hs);
        if (cert_requested) {
            Wr w;
            w.vec8([&](Wr& c) { c.bytes(request_context); }).u24_(0);
            const Bytes cm = make_hs(hs_certificate, w.b);
            send_hs(cm);
            add_transcript(cm);
        }
        const Bytes fin = make_hs(hs_finished, finished_mac(c_hs));
        send_hs(fin);
        add_transcript(fin);
        flush();
        install(wr_, c_ap);
        install(rd_, s_ap);
    }

    void client_tls12(const TlsClientConfig& cfg, const ServerHelloInfo& si, ByteView client_random) {
        using namespace detail;
        Bytes m = next_hs();
        if (type_of(m) != hs_certificate) fail(alert::unexpected_message, "expected Certificate");
        const auto chain = parse_certificate_list(Rd(body_of(m)), false);
        check_peer_chain(chain, cfg);
        if ((peer_cert_->key.kind == PublicKey::Kind::ec) != suite_->ecdsa) fail(alert::illegal_parameter, "certificate key type does not match the cipher suite");
        add_transcript(m);

        m = next_hs();
        if (type_of(m) != hs_server_key_exchange) fail(alert::unexpected_message, "expected ServerKeyExchange");
        core::u16 group;
        Bytes server_pub;
        {
            Rd r(body_of(m));
            if (r.u8_() != 3) fail(alert::illegal_parameter, "only named-curve ECDHE is supported");
            group = r.u16_();
            const ByteView pub = r.bytes(r.u8_());
            server_pub.assign(pub.begin(), pub.end());
            const usize params_len = 4 + server_pub.size();
            const core::u16 scheme = r.u16_();
            const ByteView sig = r.bytes(r.u16_());
            if (!r.empty()) fail(alert::decode_error, "trailing data in ServerKeyExchange");
            Bytes signed_data(client_random.begin(), client_random.end());
            signed_data.insert(signed_data.end(), si.random.begin(), si.random.end());
            const ByteView bm = body_of(m);
            signed_data.insert(signed_data.end(), bm.begin(), bm.begin() + static_cast<std::ptrdiff_t>(params_len));
            const bool ec_scheme = scheme == sig_ecdsa_p256_sha256 || scheme == sig_ecdsa_p384_sha384;
            if (ec_scheme != suite_->ecdsa) fail(alert::illegal_parameter, "signature scheme does not match the cipher suite");
            if (!verify_signature(scheme, peer_cert_->key, signed_data, sig, false)) fail(alert::decrypt_error, "ServerKeyExchange signature is invalid");
        }
        add_transcript(m);
        if (group != 29 && group != 23 && group != 24) fail(alert::illegal_parameter, "server selected an unsupported group");

        bool cert_requested = false;
        m = next_hs();
        if (type_of(m) == hs_certificate_request) {
            cert_requested = true;
            add_transcript(m);
            m = next_hs();
        }
        if (type_of(m) != hs_server_hello_done || !body_of(m).empty()) fail(alert::unexpected_message, "expected ServerHelloDone");
        add_transcript(m);
        if (!hs_buf_.empty()) fail(alert::unexpected_message, "data after ServerHelloDone");

        const KeyShare ks = KeyShare::generate(group);
        const Bytes pre_master = ks.agree(server_pub);

        std::lock_guard lk(wmu_);
        if (cert_requested) {
            const Bytes cm = make_hs(hs_certificate, Bytes{std::byte{0}, std::byte{0}, std::byte{0}});
            send_hs(cm);
            add_transcript(cm);
        }
        Wr cke;
        cke.vec8([&](Wr& w) { w.bytes(ks.pub); });
        const Bytes ckem = make_hs(hs_client_key_exchange, cke.b);
        send_hs(ckem);
        add_transcript(ckem);

        const Bytes master = prf12(hash_, pre_master, "extended master secret", th(), 48);
        Bytes seed(si.random.begin(), si.random.end());
        seed.insert(seed.end(), client_random.begin(), client_random.end());
        const usize ivl = suite_->chacha ? 12 : 4;
        const Bytes kb = prf12(hash_, master, "key expansion", seed, 2 * suite_->key_len + 2 * ivl);
        const auto slice = [&](usize off, usize n) { return Bytes(kb.begin() + static_cast<std::ptrdiff_t>(off), kb.begin() + static_cast<std::ptrdiff_t>(off + n)); };
        const usize kl = suite_->key_len;
        const Bytes cwk = slice(0, kl), swk = slice(kl, kl), cwi = slice(2 * kl, ivl), swi = slice(2 * kl + ivl, ivl);

        const std::byte one{1};
        write_record(ct_ccs, ByteView(&one, 1));
        wr_.c = detail::AeadCipher(*suite_, cwk);
        wr_.iv = cwi;
        wr_.seq = 0;
        wr_.on = true;
        const Bytes fin = make_hs(hs_finished, prf12(hash_, master, "client finished", th(), 12));
        send_hs(fin);
        add_transcript(fin);
        flush();

        // server CCS + Finished
        for (;;) {
            Rec r = read_record();
            if (r.type == ct_alert) { (void)handle_alert(r.data); throw TlsException("connection closed during the handshake"); }
            if (r.type == ct_handshake && !hs_buf_.empty()) fail(alert::unexpected_message, "unexpected handshake data");
            if (r.type == ct_handshake) {
                // A server may not send anything but a NewSessionTicket here, and we did not ask for tickets.
                fail(alert::unexpected_message, "unexpected handshake message before ChangeCipherSpec");
            }
            if (r.type != ct_ccs || r.data.size() != 1 || r.data[0] != std::byte{1}) fail(alert::unexpected_message, "expected ChangeCipherSpec");
            break;
        }
        rd_.c = detail::AeadCipher(*suite_, swk);
        rd_.iv = swi;
        rd_.seq = 0;
        rd_.on = true;
        const Bytes expect = prf12(hash_, master, "server finished", th(), 12);
        m = next_hs();
        if (type_of(m) != hs_finished || !data::constant_time_equal(body_of(m), expect)) fail(alert::decrypt_error, "server Finished is invalid");
        add_transcript(m);
        if (!hs_buf_.empty()) fail(alert::unexpected_message, "data after Finished");
    }

    // ---- server handshake (TLS 1.3) ----------------------------------------------------------------------------

    struct ClientHelloInfo {
        Bytes random, sid;
        std::vector<core::u16> suites, groups, sig_algs, versions;
        std::vector<std::pair<core::u16, Bytes>> shares;
        std::vector<std::string> alpn;
        bool have_sig_algs = false;
    };
    ClientHelloInfo parse_client_hello(const Bytes& msg) {
        using namespace detail;
        Rd r(body_of(msg));
        ClientHelloInfo ci;
        if (r.u16_() < 0x0301) fail(alert::protocol_version, "legacy_version too old");
        const ByteView rnd = r.bytes(32);
        ci.random.assign(rnd.begin(), rnd.end());
        const ByteView sid = r.bytes(r.u8_());
        if (sid.size() > 32) fail(alert::illegal_parameter, "bad session id");
        ci.sid.assign(sid.begin(), sid.end());
        Rd cs = r.sub16();
        while (!cs.empty()) ci.suites.push_back(cs.u16_());
        Rd comp = r.sub8();
        bool null_comp = false;
        while (!comp.empty()) null_comp |= comp.u8_() == 0;
        if (!null_comp) fail(alert::illegal_parameter, "client does not offer the null compression method");
        Rd ex = r.sub16();
        if (!r.empty()) fail(alert::decode_error, "trailing data in ClientHello");
        std::vector<core::u16> seen;
        while (!ex.empty()) {
            const core::u16 t = ex.u16_();
            Rd d = ex.sub16();
            if (std::ranges::find(seen, t) != seen.end()) fail(alert::illegal_parameter, "duplicate extension");
            seen.push_back(t);
            if (t == ext_supported_versions) {
                Rd v = d.sub8();
                while (!v.empty()) ci.versions.push_back(v.u16_());
            } else if (t == ext_supported_groups) {
                Rd g = d.sub16();
                while (!g.empty()) ci.groups.push_back(g.u16_());
            } else if (t == ext_sig_algs) {
                ci.have_sig_algs = true;
                Rd g = d.sub16();
                while (!g.empty()) ci.sig_algs.push_back(g.u16_());
            } else if (t == ext_key_share) {
                Rd l = d.sub16();
                while (!l.empty()) {
                    const core::u16 grp = l.u16_();
                    const ByteView k = l.bytes(l.u16_());
                    ci.shares.emplace_back(grp, Bytes(k.begin(), k.end()));
                }
            } else if (t == ext_alpn) {
                Rd l = d.sub16();
                while (!l.empty()) {
                    const ByteView p = l.bytes(l.u8_());
                    ci.alpn.emplace_back(reinterpret_cast<const char*>(p.data()), p.size());
                }
            }
        }
        return ci;
    }

    void server_handshake(const TlsServerConfig& cfg) {
        using namespace detail;
        if (cfg.chain.empty()) throw TlsException("server certificate chain is empty");
        Bytes msg = next_hs();
        if (type_of(msg) != hs_client_hello) fail(alert::unexpected_message, "expected ClientHello");
        ClientHelloInfo ci = parse_client_hello(msg);
        if (std::ranges::find(ci.versions, 0x0304) == ci.versions.end()) fail(alert::protocol_version, "client does not support TLS 1.3");
        tls13_ = true;
        const Suite* suite = nullptr;
        for (core::u16 want : std::initializer_list<core::u16>{0x1301, 0x1303, 0x1302})
            if (std::ranges::find(ci.suites, want) != ci.suites.end()) { suite = find_suite(want); break; }
        if (!suite) fail(alert::handshake_failure, "no common cipher suite");
        suite_ = suite;
        hash_ = suite->hash;
        if (!ci.have_sig_algs) fail(alert::missing_extension, "client sent no signature_algorithms");

        // signature scheme
        core::u16 scheme = 0;
        const auto offered = [&](core::u16 s) { return std::ranges::find(ci.sig_algs, s) != ci.sig_algs.end(); };
        if (cfg.key.kind == PrivateKey::Kind::rsa) {
            for (core::u16 s : {sig_rsa_pss_sha256, sig_rsa_pss_sha384, sig_rsa_pss_sha512})
                if (offered(s)) { scheme = s; break; }
        } else if (cfg.key.curve == CurveId::secp256r1 && offered(sig_ecdsa_p256_sha256)) {
            scheme = sig_ecdsa_p256_sha256;
        } else if (cfg.key.curve == CurveId::secp384r1 && offered(sig_ecdsa_p384_sha384)) {
            scheme = sig_ecdsa_p384_sha384;
        }
        if (!scheme) fail(alert::handshake_failure, "no common signature scheme");

        // key exchange group
        const core::u16 pref[] = {29, 23, 24};
        const Bytes* peer_share = nullptr;
        core::u16 group = 0;
        for (core::u16 g : pref) {
            if (std::ranges::find(ci.groups, g) == ci.groups.end()) continue;
            for (const auto& [sg, sk] : ci.shares)
                if (sg == g) { group = g; peer_share = &sk; break; }
            if (peer_share) break;
        }
        add_transcript(msg);
        if (!peer_share) {
            core::u16 want = 0;
            for (core::u16 g : pref)
                if (std::ranges::find(ci.groups, g) != ci.groups.end()) { want = g; break; }
            if (!want) fail(alert::handshake_failure, "no common key exchange group");
            // HelloRetryRequest
            transcript_ = make_hs(hs_message_hash, th());
            Wr b;
            b.u16_(0x0303).bytes(hrr_random()).vec8([&](Wr& w) { w.bytes(ci.sid); }).u16_(suite->id).u8_(0);
            b.vec16([&](Wr& e) {
                e.ext(ext_supported_versions, [](Wr& w) { w.u16_(0x0304); });
                e.ext(ext_key_share, [&](Wr& w) { w.u16_(want); });
            });
            const Bytes hrr = make_hs(hs_server_hello, b.b);
            add_transcript(hrr);
            send_hs(hrr);
            if (!ci.sid.empty()) send_compat_ccs();
            flush();
            allow_ccs_ = true;
            msg = next_hs();
            if (type_of(msg) != hs_client_hello) fail(alert::unexpected_message, "expected second ClientHello");
            const ClientHelloInfo c2 = parse_client_hello(msg);
            if (c2.random != ci.random || c2.sid != ci.sid || c2.suites != ci.suites) fail(alert::illegal_parameter, "ClientHello changed after HelloRetryRequest");
            peer_share = nullptr;
            for (const auto& [sg, sk] : c2.shares) {
                if (sg == want) { peer_share = &sk; group = want; }
            }
            if (!peer_share || c2.shares.size() != 1) fail(alert::illegal_parameter, "second ClientHello has no usable key share");
            add_transcript(msg);
            ci = c2;
            peer_share = nullptr;
            for (const auto& [sg, sk] : ci.shares) if (sg == want) peer_share = &sk;
        }
        const KeyShare ks = KeyShare::generate(group);
        const Bytes shared = ks.agree(*peer_share);

        // ServerHello
        Bytes srandom(32);
        system::secure_random(srandom);
        Wr sh;
        sh.u16_(0x0303).bytes(srandom).vec8([&](Wr& w) { w.bytes(ci.sid); }).u16_(suite->id).u8_(0);
        sh.vec16([&](Wr& e) {
            e.ext(ext_supported_versions, [](Wr& w) { w.u16_(0x0304); });
            e.ext(ext_key_share, [&](Wr& w) { w.u16_(group).vec16([&](Wr& k) { k.bytes(ks.pub); }); });
        });
        const Bytes shm = make_hs(hs_server_hello, sh.b);
        add_transcript(shm);
        send_hs(shm);
        if (!ci.sid.empty()) send_compat_ccs();
        allow_ccs_ = true;

        const usize L = hlen();
        const Bytes zeros(L, std::byte{0});
        const Bytes empty_hash = hash_data(hash_, {});
        const Bytes early = hkdf_extract_bytes(hash_, {}, zeros);
        const Bytes derived = hkdf_expand_label(hash_, early, "derived", empty_hash, L);
        const Bytes hs_secret = hkdf_extract_bytes(hash_, derived, shared);
        const Bytes c_hs = hkdf_expand_label(hash_, hs_secret, "c hs traffic", th(), L);
        const Bytes s_hs = hkdf_expand_label(hash_, hs_secret, "s hs traffic", th(), L);
        {
            std::lock_guard lk(wmu_);
            install(wr_, s_hs);
        }
        install(rd_, c_hs);

        // EncryptedExtensions
        for (const auto& p : cfg.alpn)
            if (std::ranges::find(ci.alpn, p) != ci.alpn.end()) { alpn_ = p; break; }
        if (!ci.alpn.empty() && !cfg.alpn.empty() && alpn_.empty()) fail(alert::no_application_protocol, "no common application protocol");
        Wr ee;
        ee.vec16([&](Wr& e) {
            if (!alpn_.empty()) e.ext(ext_alpn, [&](Wr& w) { w.vec16([&](Wr& l) { l.vec8([&](Wr& n) { n.str(alpn_); }); }); });
        });
        const Bytes eem = make_hs(hs_encrypted_extensions, ee.b);
        add_transcript(eem);
        send_hs(eem);

        Wr cw;
        cw.u8_(0).vec24([&](Wr& l) { for (const auto& c : cfg.chain) l.vec24([&](Wr& d) { d.bytes(c); }).u16_(0); });
        const Bytes certm = make_hs(hs_certificate, cw.b);
        add_transcript(certm);
        send_hs(certm);

        const Bytes content = cert_verify_content(true);
        Bytes sig;
        switch (scheme) {
            case sig_rsa_pss_sha256: sig = rsa::sign_pss(cfg.key.rsa, HashAlg::sha256, hash_data(HashAlg::sha256, content)); break;
            case sig_rsa_pss_sha384: sig = rsa::sign_pss(cfg.key.rsa, HashAlg::sha384, hash_data(HashAlg::sha384, content)); break;
            case sig_rsa_pss_sha512: sig = rsa::sign_pss(cfg.key.rsa, HashAlg::sha512, hash_data(HashAlg::sha512, content)); break;
            case sig_ecdsa_p256_sha256: sig = Curve::p256().ecdsa_sign_der<data::Sha256>(hash_data(HashAlg::sha256, content), cfg.key.ec_d); break;
            default: sig = Curve::p384().ecdsa_sign_der<data::Sha384>(hash_data(HashAlg::sha384, content), cfg.key.ec_d); break;
        }
        Wr cv;
        cv.u16_(scheme).vec16([&](Wr& w) { w.bytes(sig); });
        const Bytes cvm = make_hs(hs_certificate_verify, cv.b);
        add_transcript(cvm);
        send_hs(cvm);

        const Bytes fin = make_hs(hs_finished, finished_mac(s_hs));
        add_transcript(fin);
        send_hs(fin);
        flush();

        const Bytes derived2 = hkdf_expand_label(hash_, hs_secret, "derived", empty_hash, L);
        const Bytes master = hkdf_extract_bytes(hash_, derived2, zeros);
        const Bytes c_ap = hkdf_expand_label(hash_, master, "c ap traffic", th(), L);
        const Bytes s_ap = hkdf_expand_label(hash_, master, "s ap traffic", th(), L);
        const Bytes expect_client_fin = finished_mac(c_hs);

        Bytes cf = next_hs();
        if (type_of(cf) != hs_finished) fail(alert::unexpected_message, "expected client Finished");
        if (body_of(cf).size() != L || !data::constant_time_equal(body_of(cf), expect_client_fin)) fail(alert::decrypt_error, "client Finished is invalid");
        add_transcript(cf);
        if (!hs_buf_.empty()) fail(alert::unexpected_message, "data after Finished");
        {
            std::lock_guard lk(wmu_);
            install(wr_, s_ap);
        }
        install(rd_, c_ap);
        hs_done_ = true;
    }

    std::unique_ptr<ByteStream> lower_;
    Bytes rbuf_, wbuf_, app_, hs_buf_, transcript_;
    usize rpos_ = 0, apos_ = 0;
    Dir rd_, wr_;
    std::mutex wmu_;
    const detail::Suite* suite_ = nullptr;
    HashAlg hash_ = HashAlg::sha256;
    bool tls13_ = false, hs_done_ = false, allow_ccs_ = false, ccs_sent_ = false, alert_sent_ = false;
    bool got_close_ = false, eof_clean_ = false, tolerate_truncation_ = false;
    std::atomic<bool> closed_{false}, sent_close_{false};
    std::string alpn_;
    std::vector<std::string> offered_alpn_;
    std::optional<Certificate> peer_cert_;
};

}  // namespace flyg::net::tls
