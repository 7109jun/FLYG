// flyG - net/tls: arbitrary-precision unsigned integers (for RSA and elliptic curves)
// SPDX-License-Identifier: GPL-2.0-only
//
// Simple, correct schoolbook arithmetic (Knuth algorithm D for division). Operations are NOT
// constant time. They are used for public-key verification (no secrets) and for server-side
// signing where timing side channels from co-resident attackers are outside the threat model;
// see the README/status document for the security notes.
#pragma once

#include <algorithm>
#include <bit>
#include <span>
#include <compare>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::net::tls {

using core::u32;
using core::u64;
using core::u8;
using core::usize;

class BigInt {
public:
    BigInt() = default;
    BigInt(u64 v) {  // NOLINT: implicit by design (BigInt x = 5)
        while (v) {
            l_.push_back(static_cast<u32>(v));
            v >>= 32;
        }
    }

    [[nodiscard]] static BigInt from_bytes(std::span<const std::byte> be) {
        BigInt r;
        usize i = be.size();
        while (i > 0) {
            u32 limb = 0;
            for (int s = 0; s < 32 && i > 0; s += 8) limb |= static_cast<u32>(be[--i]) << s;
            r.l_.push_back(limb);
        }
        r.trim();
        return r;
    }
    [[nodiscard]] static BigInt from_hex(std::string_view hex) {
        std::vector<std::byte> b;
        if (hex.size() % 2) b.push_back(std::byte{0});
        auto nib = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
            throw DataException("invalid hex digit");
        };
        std::string h(hex);
        if (hex.size() % 2) h = "0" + h;
        for (usize i = 0; i < h.size(); i += 2) b.push_back(static_cast<std::byte>(nib(h[i]) * 16 + nib(h[i + 1])));
        if (hex.size() % 2) b.erase(b.begin());
        return from_bytes(b);
    }
    /// Big-endian bytes, left-padded with zeros to `len` (throws if the value does not fit).
    [[nodiscard]] std::vector<std::byte> to_bytes(usize len) const {
        std::vector<std::byte> out(len);
        for (usize i = 0; i < len; ++i) {
            const usize limb = i / 4;
            const u32 v = limb < l_.size() ? l_[limb] : 0;
            out[len - 1 - i] = static_cast<std::byte>(v >> (8 * (i % 4)));
        }
        if (bit_length() > len * 8) throw DataException("BigInt does not fit into the requested length");
        return out;
    }
    [[nodiscard]] std::vector<std::byte> to_bytes() const { return to_bytes((bit_length() + 7) / 8); }
    [[nodiscard]] std::string to_hex() const {
        static constexpr char d[] = "0123456789abcdef";
        std::string s;
        for (auto b : to_bytes()) {
            s += d[static_cast<u8>(b) >> 4];
            s += d[static_cast<u8>(b) & 15];
        }
        return s.empty() ? "0" : s;
    }

    /// Low 64 bits (throws if the value does not fit).
    [[nodiscard]] u64 to_u64() const {
        if (l_.size() > 2) throw DataException("BigInt does not fit in 64 bits");
        u64 v = 0;
        for (usize i = l_.size(); i-- > 0;) v = (v << 32) | l_[i];
        return v;
    }
    [[nodiscard]] bool is_zero() const noexcept { return l_.empty(); }
    [[nodiscard]] bool is_odd() const noexcept { return !l_.empty() && (l_[0] & 1); }
    [[nodiscard]] usize bit_length() const noexcept {
        if (l_.empty()) return 0;
        usize n = (l_.size() - 1) * 32;
        u32 top = l_.back();
        while (top) ++n, top >>= 1;
        return n;
    }
    [[nodiscard]] bool bit(usize i) const noexcept { return i / 32 < l_.size() && ((l_[i / 32] >> (i % 32)) & 1); }
    [[nodiscard]] u64 low64() const noexcept { return (l_.empty() ? 0 : l_[0]) | (l_.size() > 1 ? static_cast<u64>(l_[1]) << 32 : 0); }

    friend std::strong_ordering operator<=>(const BigInt& a, const BigInt& b) noexcept {
        if (a.l_.size() != b.l_.size()) return a.l_.size() <=> b.l_.size();
        for (usize i = a.l_.size(); i-- > 0;)
            if (a.l_[i] != b.l_[i]) return a.l_[i] <=> b.l_[i];
        return std::strong_ordering::equal;
    }
    friend bool operator==(const BigInt& a, const BigInt& b) noexcept { return a.l_ == b.l_; }

    friend BigInt operator+(const BigInt& a, const BigInt& b) {
        BigInt r;
        const usize n = std::max(a.l_.size(), b.l_.size());
        r.l_.resize(n + 1);
        u64 carry = 0;
        for (usize i = 0; i < n; ++i) {
            const u64 s = carry + (i < a.l_.size() ? a.l_[i] : 0) + (i < b.l_.size() ? b.l_[i] : 0);
            r.l_[i] = static_cast<u32>(s);
            carry = s >> 32;
        }
        r.l_[n] = static_cast<u32>(carry);
        r.trim();
        return r;
    }
    /// a - b, requires a >= b.
    friend BigInt operator-(const BigInt& a, const BigInt& b) {
        if (a < b) throw DataException("BigInt subtraction underflow");
        BigInt r;
        r.l_.resize(a.l_.size());
        long long borrow = 0;
        for (usize i = 0; i < a.l_.size(); ++i) {
            long long d = static_cast<long long>(a.l_[i]) - borrow - (i < b.l_.size() ? static_cast<long long>(b.l_[i]) : 0);
            borrow = d < 0;
            if (d < 0) d += (1ll << 32);
            r.l_[i] = static_cast<u32>(d);
        }
        r.trim();
        return r;
    }
    friend BigInt operator*(const BigInt& a, const BigInt& b) {
        if (a.is_zero() || b.is_zero()) return {};
        BigInt r;
        r.l_.assign(a.l_.size() + b.l_.size(), 0);
        for (usize i = 0; i < a.l_.size(); ++i) {
            u64 carry = 0;
            for (usize j = 0; j < b.l_.size(); ++j) {
                const u64 t = static_cast<u64>(a.l_[i]) * b.l_[j] + r.l_[i + j] + carry;
                r.l_[i + j] = static_cast<u32>(t);
                carry = t >> 32;
            }
            r.l_[i + b.l_.size()] += static_cast<u32>(carry);
        }
        r.trim();
        return r;
    }
    BigInt operator<<(usize bits) const {
        if (is_zero()) return {};
        BigInt r;
        const usize limbs = bits / 32, s = bits % 32;
        r.l_.assign(limbs, 0);
        u32 carry = 0;
        for (u32 v : l_) {
            r.l_.push_back(s ? (v << s) | carry : v);
            carry = s ? v >> (32 - s) : 0;
        }
        if (carry) r.l_.push_back(carry);
        r.trim();
        return r;
    }
    BigInt operator>>(usize bits) const {
        const usize limbs = bits / 32, s = bits % 32;
        if (limbs >= l_.size()) return {};
        BigInt r;
        for (usize i = limbs; i < l_.size(); ++i) {
            u32 v = l_[i] >> s;
            if (s && i + 1 < l_.size()) v |= l_[i + 1] << (32 - s);
            r.l_.push_back(v);
        }
        r.trim();
        return r;
    }

    /// Quotient and remainder (Knuth, TAOCP vol. 2, 4.3.1 algorithm D).
    [[nodiscard]] static std::pair<BigInt, BigInt> divmod(const BigInt& a, const BigInt& b) {
        if (b.is_zero()) throw DataException("BigInt division by zero");
        if (a < b) return {BigInt{}, a};
        if (b.l_.size() == 1) {
            BigInt q;
            q.l_.resize(a.l_.size());
            u64 rem = 0;
            for (usize i = a.l_.size(); i-- > 0;) {
                const u64 cur = (rem << 32) | a.l_[i];
                q.l_[i] = static_cast<u32>(cur / b.l_[0]);
                rem = cur % b.l_[0];
            }
            q.trim();
            return {q, BigInt(rem)};
        }
        const int shift = std::countl_zero(b.l_.back());
        BigInt u = a << static_cast<usize>(shift), v = b << static_cast<usize>(shift);
        const usize n = v.l_.size();
        if (u.l_.size() == a.l_.size()) u.l_.push_back(0);
        if (u.l_.size() < a.l_.size() + 1) u.l_.resize(a.l_.size() + 1, 0);
        const usize m = u.l_.size() - n - 1;
        BigInt q;
        q.l_.assign(m + 1, 0);
        const u64 B = 1ull << 32;
        for (usize j = m + 1; j-- > 0;) {
            const u64 num = (static_cast<u64>(u.l_[j + n]) << 32) | u.l_[j + n - 1];
            u64 qhat = num / v.l_[n - 1];
            u64 rhat = num % v.l_[n - 1];
            while (qhat >= B || qhat * v.l_[n - 2] > ((rhat << 32) | u.l_[j + n - 2])) {
                --qhat;
                rhat += v.l_[n - 1];
                if (rhat >= B) break;
            }
            long long borrow = 0;
            u64 carry = 0;
            for (usize i = 0; i < n; ++i) {
                const u64 p = qhat * v.l_[i] + carry;
                carry = p >> 32;
                const long long t = static_cast<long long>(u.l_[i + j]) - borrow - static_cast<long long>(p & 0xFFFFFFFFu);
                u.l_[i + j] = static_cast<u32>(t);
                borrow = t < 0;
            }
            const long long t = static_cast<long long>(u.l_[j + n]) - borrow - static_cast<long long>(carry);
            u.l_[j + n] = static_cast<u32>(t);
            if (t < 0) {  // qhat was one too large: add the divisor back
                --qhat;
                u64 c = 0;
                for (usize i = 0; i < n; ++i) {
                    const u64 s = static_cast<u64>(u.l_[i + j]) + v.l_[i] + c;
                    u.l_[i + j] = static_cast<u32>(s);
                    c = s >> 32;
                }
                u.l_[j + n] += static_cast<u32>(c);
            }
            q.l_[j] = static_cast<u32>(qhat);
        }
        q.trim();
        u.trim();
        return {q, u >> static_cast<usize>(shift)};
    }
    friend BigInt operator/(const BigInt& a, const BigInt& b) { return divmod(a, b).first; }
    friend BigInt operator%(const BigInt& a, const BigInt& b) { return divmod(a, b).second; }

    // ---- modular arithmetic ---------------------------------------------------------------------------------
    [[nodiscard]] static BigInt add_mod(const BigInt& a, const BigInt& b, const BigInt& m) {
        BigInt s = a + b;
        return s >= m ? s - m : s;
    }
    [[nodiscard]] static BigInt sub_mod(const BigInt& a, const BigInt& b, const BigInt& m) { return a >= b ? a - b : a + m - b; }
    [[nodiscard]] static BigInt mul_mod(const BigInt& a, const BigInt& b, const BigInt& m) { return (a * b) % m; }
    [[nodiscard]] static BigInt pow_mod(const BigInt& base, const BigInt& exp, const BigInt& m) {
        if (m == BigInt(1)) return {};
        BigInt result(1), b = base % m;
        for (usize i = exp.bit_length(); i-- > 0;) {
            result = mul_mod(result, result, m);
            if (exp.bit(i)) result = mul_mod(result, b, m);
        }
        return result;
    }
    /// Modular inverse via the extended Euclidean algorithm; throws if gcd(a, m) != 1.
    [[nodiscard]] static BigInt inv_mod(const BigInt& a, const BigInt& m) {
        BigInt r0 = m, r1 = a % m, t0(0), t1(1);
        while (!r1.is_zero()) {
            const auto [q, r] = divmod(r0, r1);
            // t2 = t0 - q*t1 (mod m)
            const BigInt qt = mul_mod(q, t1, m);
            const BigInt t2 = sub_mod(t0, qt, m);
            r0 = r1, r1 = r, t0 = t1, t1 = t2;
        }
        if (r0 != BigInt(1)) throw DataException("value is not invertible");
        return t0;
    }

    friend bool operator!=(const BigInt& a, const BigInt& b) noexcept { return !(a == b); }

private:
    void trim() noexcept {
        while (!l_.empty() && l_.back() == 0) l_.pop_back();
    }
    std::vector<u32> l_;  // little-endian limbs, no leading zero limbs
};

}  // namespace flyg::net::tls
