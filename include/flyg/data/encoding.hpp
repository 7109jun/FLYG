// flyG - text/binary encodings: UTF-8, Base64, hex, percent-encoding, CRC-32, FNV-1a
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::data {

using core::u8;
using core::u16;
using core::u32;
using core::u64;
using core::i64;
using core::usize;

// ---------------------------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------------------------
namespace utf8 {

inline constexpr char32_t replacement = 0xFFFD;

/// Decodes one code point at `pos` (advancing it). Returns false for malformed / overlong /
/// surrogate / out-of-range sequences, leaving `pos` advanced by one byte.
[[nodiscard]] constexpr bool decode(std::string_view s, usize& pos, char32_t& cp) noexcept {
    const auto b0 = static_cast<u8>(s[pos]);
    auto cont = [&](usize i) { return i < s.size() && (static_cast<u8>(s[i]) & 0xC0) == 0x80; };
    if (b0 < 0x80) {
        cp = b0;
        ++pos;
        return true;
    }
    usize len = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; min = 0x10000; }
    else { ++pos; return false; }
    for (usize i = 1; i < len; ++i) {
        if (!cont(pos + i)) { ++pos; return false; }
        cp = (cp << 6) | (static_cast<u8>(s[pos + i]) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++pos; return false; }
    pos += len;
    return true;
}

inline void append(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

[[nodiscard]] constexpr bool is_valid(std::string_view s) noexcept {
    usize pos = 0;
    char32_t cp;
    while (pos < s.size())
        if (!decode(s, pos, cp)) return false;
    return true;
}

/// Replaces malformed sequences with U+FFFD.
[[nodiscard]] inline std::string sanitize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    usize pos = 0;
    char32_t cp;
    while (pos < s.size()) {
        if (decode(s, pos, cp)) append(out, cp);
        else append(out, replacement);
    }
    return out;
}

[[nodiscard]] inline usize length(std::string_view s) {
    usize pos = 0, n = 0;
    char32_t cp;
    while (pos < s.size()) {
        (void)decode(s, pos, cp);
        ++n;
    }
    return n;
}

[[nodiscard]] inline std::u32string to_utf32(std::string_view s) {
    std::u32string out;
    usize pos = 0;
    char32_t cp;
    while (pos < s.size()) {
        if (!decode(s, pos, cp)) throw DataException("invalid UTF-8", pos - 1);
        out += cp;
    }
    return out;
}

[[nodiscard]] inline std::string from_utf32(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) {
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) throw DataException("invalid code point");
        append(out, c);
    }
    return out;
}

[[nodiscard]] inline std::u16string to_utf16(std::string_view s) {
    std::u16string out;
    for (char32_t c : to_utf32(s)) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

[[nodiscard]] inline std::string from_utf16(std::u16string_view s) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF)
                throw DataException("unpaired UTF-16 high surrogate", i);
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            throw DataException("unpaired UTF-16 low surrogate", i);
        }
        append(out, c);
    }
    return out;
}

}  // namespace utf8

// ---------------------------------------------------------------------------------------------
// Base64 (RFC 4648) and hex
// ---------------------------------------------------------------------------------------------
[[nodiscard]] inline std::string base64_encode(std::span<const std::byte> data, bool url_safe = false,
                                               bool pad = true) {
    const char* tbl = url_safe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                               : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const u32 v = (static_cast<u32>(data[i]) << 16) | (static_cast<u32>(data[i + 1]) << 8) | static_cast<u32>(data[i + 2]);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    const usize rest = data.size() - i;
    if (rest > 0) {
        u32 v = static_cast<u32>(data[i]) << 16;
        if (rest == 2) v |= static_cast<u32>(data[i + 1]) << 8;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        if (rest == 2) out += tbl[(v >> 6) & 63];
        else if (pad) out += '=';
        if (pad) out += '=';
    }
    return out;
}

[[nodiscard]] inline std::string base64_encode(std::string_view s, bool url_safe = false, bool pad = true) {
    return base64_encode(std::as_bytes(std::span(s.data(), s.size())), url_safe, pad);
}

/// Accepts both alphabets, with or without padding; whitespace is ignored. Throws DataException
/// on any other character or an impossible length.
[[nodiscard]] inline std::vector<std::byte> base64_decode(std::string_view s) {
    std::vector<std::byte> out;
    out.reserve(s.size() / 4 * 3 + 3);
    u32 acc = 0;
    int bits = 0;
    usize pads = 0, count = 0;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { ++pads; continue; }
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else throw DataException("invalid base64 character", i);
        if (pads) throw DataException("base64 data after padding", i);
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        ++count;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFF));
        }
    }
    if (count % 4 == 1) throw DataException("invalid base64 length");
    if (pads > 2 || (pads && (count + pads) % 4 != 0)) throw DataException("invalid base64 padding");
    return out;
}

[[nodiscard]] inline std::string hex_encode(std::span<const std::byte> data, bool upper = false) {
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (std::byte b : data) {
        s += d[static_cast<u8>(b) >> 4];
        s += d[static_cast<u8>(b) & 15];
    }
    return s;
}

[[nodiscard]] inline std::vector<std::byte> hex_decode(std::string_view s) {
    if (s.size() % 2) throw DataException("odd-length hex string");
    auto nib = [&](usize i) -> u8 {
        const char c = s[i];
        if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
        throw DataException("invalid hex digit", i);
    };
    std::vector<std::byte> out(s.size() / 2);
    for (usize i = 0; i < out.size(); ++i) out[i] = static_cast<std::byte>((nib(2 * i) << 4) | nib(2 * i + 1));
    return out;
}

/// RFC 3986 percent-encoding. Unreserved characters (A-Z a-z 0-9 - . _ ~) are kept.
[[nodiscard]] inline std::string percent_encode(std::string_view s, bool space_as_plus = false) {
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<u8>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            out += ch;
        } else if (c == ' ' && space_as_plus) {
            out += '+';
        } else {
            out += '%';
            out += "0123456789ABCDEF"[c >> 4];
            out += "0123456789ABCDEF"[c & 15];
        }
    }
    return out;
}

[[nodiscard]] inline std::string percent_decode(std::string_view s, bool plus_as_space = false) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) throw DataException("truncated percent escape", i);
            auto h = [&](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = h(s[i + 1]), lo = h(s[i + 2]);
            if (hi < 0 || lo < 0) throw DataException("invalid percent escape", i);
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else if (s[i] == '+' && plus_as_space) {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------------------------
namespace detail {
[[nodiscard]] constexpr std::array<u32, 256> make_crc_table() noexcept {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
inline constexpr auto crc_table = make_crc_table();
}  // namespace detail

/// CRC-32 (IEEE 802.3, as used by zip/png). Pass a previous result as `crc` to continue a stream.
[[nodiscard]] constexpr u32 crc32(std::span<const std::byte> data, u32 crc = 0) noexcept {
    u32 c = ~crc;
    for (std::byte b : data) c = detail::crc_table[(c ^ static_cast<u32>(b)) & 0xFF] ^ (c >> 8);
    return ~c;
}
[[nodiscard]] inline u32 crc32(std::string_view s, u32 crc = 0) noexcept {
    return crc32(std::as_bytes(std::span(s.data(), s.size())), crc);
}

[[nodiscard]] constexpr u64 fnv1a64(std::span<const std::byte> data) noexcept {
    u64 h = 14695981039346656037ull;
    for (std::byte b : data) {
        h ^= static_cast<u64>(b);
        h *= 1099511628211ull;
    }
    return h;
}
[[nodiscard]] inline u64 fnv1a64(std::string_view s) noexcept {
    return fnv1a64(std::as_bytes(std::span(s.data(), s.size())));
}

}  // namespace flyg::data
