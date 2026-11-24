// flyG - net/tls: DER (ASN.1) reader/writer and PEM decoding
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/net/tls/bigint.hpp>

namespace flyg::net::tls::asn1 {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

namespace tag {
inline constexpr u8 boolean = 0x01, integer = 0x02, bit_string = 0x03, octet_string = 0x04, null = 0x05, oid = 0x06, utf8_string = 0x0C,
                    printable_string = 0x13, ia5_string = 0x16, utc_time = 0x17, generalized_time = 0x18, sequence = 0x30, set = 0x31;
constexpr u8 context(unsigned n, bool constructed = true) { return static_cast<u8>(0x80 | (constructed ? 0x20 : 0) | n); }
}  // namespace tag

/// One parsed TLV; `content` is a view into the original buffer.
struct Element {
    u8 tag = 0;
    ByteView content;
    ByteView whole;  ///< header + content (needed to hash the exact signed bytes)
    [[nodiscard]] bool constructed() const noexcept { return tag & 0x20; }
};

class Reader {
public:
    Reader() = default;
    explicit Reader(ByteView d) : d_(d) {}
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= d_.size(); }
    [[nodiscard]] usize remaining() const noexcept { return d_.size() - pos_; }

    /// Reads the next element (strict DER lengths: minimal encoding, no indefinite form).
    [[nodiscard]] Element next() {
        if (remaining() < 2) throw DataException("ASN.1: truncated element");
        const usize start = pos_;
        const u8 t = static_cast<u8>(d_[pos_++]);
        if ((t & 0x1F) == 0x1F) throw DataException("ASN.1: high tag numbers are not supported");
        usize len = static_cast<u8>(d_[pos_++]);
        if (len & 0x80) {
            const usize n = len & 0x7F;
            if (n == 0 || n > 4) throw DataException("ASN.1: unsupported length encoding");
            if (remaining() < n) throw DataException("ASN.1: truncated length");
            len = 0;
            for (usize i = 0; i < n; ++i) len = (len << 8) | static_cast<u8>(d_[pos_++]);
            if (len < 128 || (n > 1 && static_cast<u8>(d_[pos_ - n]) == 0)) throw DataException("ASN.1: non-minimal length");
        }
        if (remaining() < len) throw DataException("ASN.1: element extends past its container");
        Element e;
        e.tag = t;
        e.content = d_.subspan(pos_, len);
        e.whole = d_.subspan(start, pos_ - start + len);
        pos_ += len;
        return e;
    }
    [[nodiscard]] Element expect(u8 t) {
        Element e = next();
        if (e.tag != t) throw DataException("ASN.1: unexpected tag 0x" + hex2(e.tag) + " (wanted 0x" + hex2(t) + ")");
        return e;
    }
    /// If the next element has tag `t`, consumes and returns it.
    [[nodiscard]] std::optional<Element> optional(u8 t) {
        if (at_end() || static_cast<u8>(d_[pos_]) != t) return std::nullopt;
        return next();
    }
    [[nodiscard]] u8 peek_tag() const {
        if (at_end()) throw DataException("ASN.1: unexpected end of data");
        return static_cast<u8>(d_[pos_]);
    }

private:
    static std::string hex2(u8 v) {
        static constexpr char d[] = "0123456789abcdef";
        return std::string{d[v >> 4], d[v & 15]};
    }
    ByteView d_;
    usize pos_ = 0;
};

[[nodiscard]] inline Reader open(const Element& e) { return Reader(e.content); }

[[nodiscard]] inline BigInt to_integer(const Element& e) {
    if (e.tag != tag::integer || e.content.empty()) throw DataException("ASN.1: not an INTEGER");
    if (static_cast<u8>(e.content[0]) & 0x80) throw DataException("ASN.1: negative INTEGER");
    return BigInt::from_bytes(e.content);
}

[[nodiscard]] inline std::string to_oid(const Element& e) {
    if (e.tag != tag::oid || e.content.empty()) throw DataException("ASN.1: not an OBJECT IDENTIFIER");
    std::string out;
    core::u64 v = 0;
    bool first = true;
    for (std::byte b : e.content) {
        v = (v << 7) | (static_cast<u8>(b) & 0x7F);
        if (v > (1ull << 56)) throw DataException("ASN.1: OID component too large");
        if (!(static_cast<u8>(b) & 0x80)) {
            if (first) {
                const core::u64 a = v < 40 ? 0 : v < 80 ? 1 : 2;
                out = std::to_string(a) + "." + std::to_string(v - a * 40);
                first = false;
            } else {
                out += "." + std::to_string(v);
            }
            v = 0;
        }
    }
    if (e.content.back() >= std::byte{0x80}) throw DataException("ASN.1: truncated OID");
    return out;
}

/// BIT STRING content without the "unused bits" byte (requires zero unused bits).
[[nodiscard]] inline ByteView to_bits(const Element& e) {
    if (e.tag != tag::bit_string || e.content.empty()) throw DataException("ASN.1: not a BIT STRING");
    if (static_cast<u8>(e.content[0]) > 7) throw DataException("ASN.1: invalid BIT STRING");
    return e.content.subspan(1);
}
[[nodiscard]] inline std::string to_string_value(const Element& e) { return std::string(reinterpret_cast<const char*>(e.content.data()), e.content.size()); }

/// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
[[nodiscard]] constexpr core::i64 days_from_civil(core::i64 y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const core::i64 era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<core::i64>(doe) - 719468;
}

/// UTCTime / GeneralizedTime (UTC "Z" form only, as required by RFC 5280) -> Unix seconds.
[[nodiscard]] inline core::i64 to_time(const Element& e) {
    const std::string s = to_string_value(e);
    usize i = 0;
    auto num = [&](usize n) {
        if (i + n > s.size()) throw DataException("ASN.1: truncated time");
        int v = 0;
        for (usize k = 0; k < n; ++k) {
            const char c = s[i + k];
            if (c < '0' || c > '9') throw DataException("ASN.1: invalid time");
            v = v * 10 + (c - '0');
        }
        i += n;
        return v;
    };
    int year;
    if (e.tag == tag::utc_time) {
        year = num(2);
        year += year >= 50 ? 1900 : 2000;
    } else if (e.tag == tag::generalized_time) {
        year = num(4);
    } else {
        throw DataException("ASN.1: not a time value");
    }
    const int mo = num(2), d = num(2), h = num(2), mi = num(2), sec = num(2);
    if (i >= s.size() || s[i] != 'Z' || i + 1 != s.size()) throw DataException("ASN.1: time must be in Zulu form");
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) throw DataException("ASN.1: time field out of range");
    return days_from_civil(year, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + sec;
}

// ---- writer ---------------------------------------------------------------------------------------------------

inline void append_length(Bytes& out, usize len) {
    if (len < 128) {
        out.push_back(static_cast<std::byte>(len));
        return;
    }
    u8 tmp[8];
    int n = 0;
    while (len) tmp[n++] = static_cast<u8>(len), len >>= 8;
    out.push_back(static_cast<std::byte>(0x80 | n));
    while (n) out.push_back(static_cast<std::byte>(tmp[--n]));
}
[[nodiscard]] inline Bytes make(u8 t, ByteView content) {
    Bytes out{static_cast<std::byte>(t)};
    append_length(out, content.size());
    out.insert(out.end(), content.begin(), content.end());
    return out;
}
[[nodiscard]] inline Bytes concat(std::initializer_list<ByteView> parts) {
    Bytes out;
    for (auto p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
[[nodiscard]] inline Bytes make_integer(const BigInt& v) {
    Bytes b = v.to_bytes();
    if (b.empty()) b.push_back(std::byte{0});
    if (static_cast<u8>(b[0]) & 0x80) b.insert(b.begin(), std::byte{0});
    return make(tag::integer, b);
}

// ---- PEM -------------------------------------------------------------------------------------------------------

struct PemBlock {
    std::string label;
    Bytes der;
};

/// Extracts all "-----BEGIN X-----" blocks from text.
[[nodiscard]] inline std::vector<PemBlock> parse_pem(std::string_view text) {
    std::vector<PemBlock> out;
    usize pos = 0;
    while (true) {
        const auto b = text.find("-----BEGIN ", pos);
        if (b == std::string_view::npos) break;
        const auto lend = text.find("-----", b + 11);
        if (lend == std::string_view::npos) break;
        const std::string label(text.substr(b + 11, lend - b - 11));
        const auto body_start = lend + 5;
        const auto e = text.find("-----END " + label + "-----", body_start);
        if (e == std::string_view::npos) throw DataException("PEM: missing END line for '" + label + "'");
        std::string b64;
        for (char c : text.substr(body_start, e - body_start))
            if (c != '\r' && c != '\n' && c != ' ' && c != '\t') b64 += c;
        out.push_back({label, data::base64_decode(b64)});
        pos = e + 9 + label.size() + 5;
    }
    return out;
}

}  // namespace flyg::net::tls::asn1
