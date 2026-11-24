// flyG - binary encoding: ByteWriter/ByteReader (varints, LE integers) and the compact "FBIN"
//        container format for Value trees
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data {

/// Appends little-endian primitives, LEB128 varints and length-prefixed strings to a buffer.
class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(usize reserve) { buf_.reserve(reserve); }

    void put_u8(core::u8 v) { buf_.push_back(static_cast<std::byte>(v)); }
    void put_u16(core::u16 v) { put_le(v); }
    void put_u32(u32 v) { put_le(v); }
    void put_u64(u64 v) { put_le(v); }
    void put_i64(core::i64 v) { put_le(static_cast<u64>(v)); }
    void put_f32(float v) { put_le(std::bit_cast<u32>(v)); }
    void put_f64(double v) { put_le(std::bit_cast<u64>(v)); }
    /// Unsigned LEB128.
    void put_varint(u64 v) {
        while (v >= 0x80) {
            put_u8(static_cast<core::u8>(v | 0x80));
            v >>= 7;
        }
        put_u8(static_cast<core::u8>(v));
    }
    /// Zig-zag encoded signed varint (small magnitudes stay small).
    void put_svarint(core::i64 v) { put_varint((static_cast<u64>(v) << 1) ^ static_cast<u64>(v >> 63)); }
    void put_raw(std::span<const std::byte> b) { buf_.insert(buf_.end(), b.begin(), b.end()); }
    void put_bytes(std::span<const std::byte> b) {
        put_varint(b.size());
        put_raw(b);
    }
    void put_string(std::string_view s) {
        put_varint(s.size());
        put_raw(std::as_bytes(std::span(s.data(), s.size())));
    }
    /// Overwrites 4 bytes at `offset` (used to back-patch lengths / checksums).
    void patch_u32(usize offset, u32 v) {
        if (offset + 4 > buf_.size()) throw DataException("ByteWriter::patch_u32 out of range", offset);
        for (int i = 0; i < 4; ++i) buf_[offset + static_cast<usize>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }

    [[nodiscard]] usize size() const noexcept { return buf_.size(); }
    [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buf_; }
    [[nodiscard]] std::span<const std::byte> view() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buf_); }
    void clear() noexcept { buf_.clear(); }

private:
    template <class T>
    void put_le(T v) {
        for (usize i = 0; i < sizeof(T); ++i) buf_.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
    std::vector<std::byte> buf_;
};

/// Bounds-checked reader over a byte span; every failure throws DataException with the offset.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : d_(data) {}

    [[nodiscard]] usize position() const noexcept { return pos_; }
    [[nodiscard]] usize remaining() const noexcept { return d_.size() - pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == d_.size(); }

    core::u8 read_u8() { return static_cast<core::u8>(take(1)[0]); }
    core::u16 read_u16() { return get_le<core::u16>(); }
    u32 read_u32() { return get_le<u32>(); }
    u64 read_u64() { return get_le<u64>(); }
    core::i64 read_i64() { return static_cast<core::i64>(get_le<u64>()); }
    float read_f32() { return std::bit_cast<float>(get_le<u32>()); }
    double read_f64() { return std::bit_cast<double>(get_le<u64>()); }

    u64 read_varint() {
        u64 v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const core::u8 b = read_u8();
            if (shift == 63 && b > 1) throw DataException("varint overflows 64 bits", pos_ - 1);
            v |= static_cast<u64>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        throw DataException("varint is too long", pos_);
    }
    core::i64 read_svarint() {
        const u64 z = read_varint();
        return static_cast<core::i64>((z >> 1) ^ (~(z & 1) + 1));
    }
    std::span<const std::byte> take(usize n) {
        if (n > remaining()) throw DataException("unexpected end of data (need " + std::to_string(n) + " bytes, have " + std::to_string(remaining()) + ")", pos_);
        const auto s = d_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    std::span<const std::byte> read_bytes() { return take(checked_len()); }
    std::string read_string() {
        const auto b = read_bytes();
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }
    void skip(usize n) { (void)take(n); }

private:
    usize checked_len() {
        const u64 n = read_varint();
        if (n > remaining()) throw DataException("length prefix exceeds the available data", pos_);
        return static_cast<usize>(n);
    }
    template <class T>
    T get_le() {
        const auto b = take(sizeof(T));
        T v = 0;
        for (usize i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(b[i]) << (8 * i));
        return v;
    }
    std::span<const std::byte> d_;
    usize pos_ = 0;
};

namespace binary {

namespace tag {
inline constexpr core::u8 null = 0, false_ = 1, true_ = 2, integer = 3, real = 4, string = 5, binary = 6, array = 7, object = 8;
}

inline void encode_raw(ByteWriter& w, const Value& v, usize depth = 0) {
    if (depth > 512) throw DataException("binary: value nested too deeply");
    switch (v.type()) {
        case Type::null: w.put_u8(tag::null); break;
        case Type::boolean: w.put_u8(v.as_bool() ? tag::true_ : tag::false_); break;
        case Type::integer: w.put_u8(tag::integer); w.put_svarint(v.as_int()); break;
        case Type::real: w.put_u8(tag::real); w.put_f64(v.as_double()); break;
        case Type::string: w.put_u8(tag::string); w.put_string(v.as_string()); break;
        case Type::binary: w.put_u8(tag::binary); w.put_bytes(v.as_binary()); break;
        case Type::array:
            w.put_u8(tag::array);
            w.put_varint(v.as_array().size());
            for (const auto& x : v.as_array()) encode_raw(w, x, depth + 1);
            break;
        case Type::object:
            w.put_u8(tag::object);
            w.put_varint(v.as_object().size());
            for (const auto& [k, x] : v.as_object()) {
                w.put_string(k);
                encode_raw(w, x, depth + 1);
            }
            break;
    }
}

[[nodiscard]] inline Value decode_raw(ByteReader& r, usize depth = 0) {
    if (depth > 512) throw DataException("binary: value nested too deeply", r.position());
    const usize at = r.position();
    switch (r.read_u8()) {
        case tag::null: return Value();
        case tag::false_: return Value(false);
        case tag::true_: return Value(true);
        case tag::integer: return Value(r.read_svarint());
        case tag::real: return Value(r.read_f64());
        case tag::string: return Value(r.read_string());
        case tag::binary: {
            const auto b = r.read_bytes();
            return Value(Bytes(b.begin(), b.end()));
        }
        case tag::array: {
            const u64 n = r.read_varint();
            if (n > r.remaining()) throw DataException("array length exceeds the available data", r.position());  // each element needs >= 1 byte
            Array a;
            a.reserve(static_cast<usize>(n));
            for (u64 i = 0; i < n; ++i) a.push_back(decode_raw(r, depth + 1));
            return Value(std::move(a));
        }
        case tag::object: {
            const u64 n = r.read_varint();
            if (n > r.remaining() / 2) throw DataException("object size exceeds the available data", r.position());
            Object o;
            o.reserve(static_cast<usize>(n));
            for (u64 i = 0; i < n; ++i) {
                std::string k = r.read_string();
                if (o.contains(k)) throw DataException("duplicate object key '" + k + "'", r.position());
                o.append_unchecked(std::move(k), decode_raw(r, depth + 1));
            }
            return Value(std::move(o));
        }
        default: throw DataException("unknown value tag", at);
    }
}

/// Container: "FBIN" | version(1) | flags(1) | payload length (u32 LE) | payload | CRC-32 (u32 LE)
/// where the CRC covers everything before it.
inline constexpr u32 current_version = 1;

[[nodiscard]] inline std::vector<std::byte> encode(const Value& v) {
    ByteWriter w;
    w.put_u8('F'); w.put_u8('B'); w.put_u8('I'); w.put_u8('N');
    w.put_u8(static_cast<core::u8>(current_version));
    w.put_u8(0);
    const usize len_at = w.size();
    w.put_u32(0);
    const usize start = w.size();
    encode_raw(w, v);
    const usize payload = w.size() - start;
    if (payload > 0xFFFFFFFFull) throw DataException("binary: payload larger than 4 GiB");
    w.patch_u32(len_at, static_cast<u32>(payload));
    w.put_u32(crc32(w.view()));
    return w.take();
}

[[nodiscard]] inline Value decode(std::span<const std::byte> data) {
    ByteReader r(data);
    if (data.size() < 14) throw DataException("binary: data too short for an FBIN container");
    if (r.read_u8() != 'F' || r.read_u8() != 'B' || r.read_u8() != 'I' || r.read_u8() != 'N') throw DataException("binary: bad magic (not an FBIN container)", 0);
    const core::u8 ver = r.read_u8();
    if (ver != current_version) throw DataException("binary: unsupported FBIN version " + std::to_string(ver), 4);
    (void)r.read_u8();  // flags (reserved)
    const u32 len = r.read_u32();
    if (static_cast<u64>(len) + 4 != r.remaining()) throw DataException("binary: container length mismatch", 6);
    const auto payload = r.take(len);
    const u32 stored = r.read_u32();
    if (crc32(data.first(data.size() - 4)) != stored) throw DataException("binary: checksum mismatch (data is corrupt)");
    ByteReader pr(payload);
    Value v = decode_raw(pr);
    if (!pr.at_end()) throw DataException("binary: trailing bytes after the value", pr.position());
    return v;
}

}  // namespace binary
}  // namespace flyg::data
