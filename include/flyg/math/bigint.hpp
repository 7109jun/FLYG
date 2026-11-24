// flyG - arbitrary precision signed integer
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <compare>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::math {

using core::i64;
using core::u32;
using core::u64;
using core::usize;

/// Sign-magnitude big integer with 32-bit limbs (little endian). Division truncates toward
/// zero like built-in integers (the remainder takes the sign of the dividend).
/// Multiplication is schoolbook O(n*m); division is Knuth algorithm D.
class BigInt {
public:
    BigInt() = default;
    BigInt(long long v) { assign_i64(static_cast<i64>(v)); }               // NOLINT: implicit by design
    BigInt(int v) : BigInt(static_cast<long long>(v)) {}                  // NOLINT
    BigInt(unsigned long long v) { assign_u64(static_cast<u64>(v)); }     // NOLINT
    BigInt(unsigned v) : BigInt(static_cast<unsigned long long>(v)) {}    // NOLINT
    BigInt(long v) : BigInt(static_cast<long long>(v)) {}                 // NOLINT
    BigInt(unsigned long v) : BigInt(static_cast<unsigned long long>(v)) {}  // NOLINT

    /// Parses an optionally signed decimal number, or hexadecimal with a 0x prefix.
    [[nodiscard]] static BigInt from_string(std::string_view s) {
        usize pos = 0;
        bool neg = false;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) neg = s[pos++] == '-';
        unsigned base = 10;
        if (s.size() >= pos + 2 && s[pos] == '0' && (s[pos + 1] == 'x' || s[pos + 1] == 'X')) {
            base = 16;
            pos += 2;
        }
        if (pos >= s.size()) throw DataException("BigInt::from_string: no digits");
        BigInt r;
        if (base == 10) {
            while (pos < s.size()) {  // 9 decimal digits per step
                const usize take = std::min<usize>(9, s.size() - pos);
                u32 chunk = 0, mul = 1;
                for (usize i = 0; i < take; ++i) {
                    const char c = s[pos + i];
                    if (c < '0' || c > '9') throw DataException("BigInt::from_string: invalid digit", pos + i);
                    chunk = chunk * 10 + static_cast<u32>(c - '0');
                    mul *= 10;
                }
                mul_small(r.mag_, mul);
                add_small(r.mag_, chunk);
                pos += take;
            }
        } else {
            for (; pos < s.size(); ++pos) {
                const char c = s[pos];
                u32 d;
                if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
                else throw DataException("BigInt::from_string: invalid hex digit", pos);
                mul_small(r.mag_, 16);
                add_small(r.mag_, d);
            }
        }
        r.neg_ = neg;
        r.trim();
        return r;
    }

    [[nodiscard]] static BigInt pow10(usize n) {
        BigInt r(1);
        BigInt base(10);
        while (n) {
            if (n & 1) r = r * base;
            n >>= 1;
            if (n) base = base * base;
        }
        return r;
    }

    // ---- queries ----
    [[nodiscard]] bool is_zero() const noexcept { return mag_.empty(); }
    [[nodiscard]] bool is_negative() const noexcept { return neg_; }
    [[nodiscard]] bool is_even() const noexcept { return mag_.empty() || (mag_[0] & 1u) == 0; }
    [[nodiscard]] int sign() const noexcept { return mag_.empty() ? 0 : (neg_ ? -1 : 1); }
    /// Number of bits of the magnitude (0 for zero).
    [[nodiscard]] usize bit_length() const noexcept {
        return mag_.empty() ? 0 : mag_.size() * 32 - static_cast<usize>(std::countl_zero(mag_.back()));
    }
    [[nodiscard]] usize trailing_zero_bits() const noexcept {
        usize n = 0;
        for (const u32 limb : mag_) {
            if (limb == 0) { n += 32; continue; }
            return n + static_cast<usize>(std::countr_zero(limb));
        }
        return n;
    }
    [[nodiscard]] bool fits_i64() const noexcept {
        if (mag_.size() > 2) return false;
        const u64 m = magnitude_u64();
        return neg_ ? m <= (u64{1} << 63) : m < (u64{1} << 63);
    }
    [[nodiscard]] i64 to_i64() const {
        if (!fits_i64()) throw MathException("BigInt::to_i64: value out of range");
        const u64 m = magnitude_u64();
        return neg_ ? static_cast<i64>(0 - m) : static_cast<i64>(m);
    }
    /// Nearest-ish double (truncated to 64 significant bits, then rounded by the FPU).
    [[nodiscard]] double to_double() const noexcept {
        if (mag_.empty()) return 0.0;
        const usize bl = bit_length();
        double r;
        if (bl <= 64) {
            r = static_cast<double>(magnitude_u64());
        } else {
            const usize sh = bl - 64;
            const BigInt top = abs().shr(sh);
            r = std::ldexp(static_cast<double>(top.magnitude_u64()), static_cast<int>(sh));
        }
        return neg_ ? -r : r;
    }

    // ---- sign ops ----
    [[nodiscard]] BigInt abs() const { BigInt r = *this; r.neg_ = false; return r; }
    [[nodiscard]] BigInt operator-() const { BigInt r = *this; if (!r.mag_.empty()) r.neg_ = !r.neg_; return r; }

    // ---- shifts (act on the magnitude; the sign is kept, so shr rounds toward zero) ----
    [[nodiscard]] BigInt shl(usize bits) const {
        if (mag_.empty() || bits == 0) return *this;
        BigInt r;
        const usize limbs = bits / 32, rem = bits % 32;
        r.mag_.assign(limbs, 0);
        u32 carry = 0;
        for (const u32 w : mag_) {
            r.mag_.push_back(rem ? (w << rem) | carry : w);
            carry = rem ? w >> (32 - rem) : 0;
        }
        if (carry) r.mag_.push_back(carry);
        r.neg_ = neg_;
        r.trim();
        return r;
    }
    [[nodiscard]] BigInt shr(usize bits) const {
        const usize limbs = bits / 32, rem = bits % 32;
        if (limbs >= mag_.size()) return BigInt();
        BigInt r;
        r.mag_.assign(mag_.begin() + static_cast<std::ptrdiff_t>(limbs), mag_.end());
        if (rem) {
            for (usize i = 0; i < r.mag_.size(); ++i) {
                const u32 hi = i + 1 < r.mag_.size() ? r.mag_[i + 1] << (32 - rem) : 0;
                r.mag_[i] = (r.mag_[i] >> rem) | hi;
            }
        }
        r.neg_ = neg_;
        r.trim();
        return r;
    }
    BigInt operator<<(usize bits) const { return shl(bits); }
    BigInt operator>>(usize bits) const { return shr(bits); }

    // ---- arithmetic ----
    friend BigInt operator+(const BigInt& a, const BigInt& b) {
        BigInt r;
        if (a.neg_ == b.neg_) {
            r.mag_ = add_mag(a.mag_, b.mag_);
            r.neg_ = a.neg_;
        } else {
            const int c = cmp_mag(a.mag_, b.mag_);
            if (c == 0) return r;
            if (c > 0) { r.mag_ = sub_mag(a.mag_, b.mag_); r.neg_ = a.neg_; }
            else { r.mag_ = sub_mag(b.mag_, a.mag_); r.neg_ = b.neg_; }
        }
        r.trim();
        return r;
    }
    friend BigInt operator-(const BigInt& a, const BigInt& b) { return a + (-b); }
    friend BigInt operator*(const BigInt& a, const BigInt& b) {
        BigInt r;
        if (a.mag_.empty() || b.mag_.empty()) return r;
        r.mag_ = mul_mag(a.mag_, b.mag_);
        r.neg_ = a.neg_ != b.neg_;
        r.trim();
        return r;
    }
    friend BigInt operator/(const BigInt& a, const BigInt& b) { return divmod(a, b).first; }
    friend BigInt operator%(const BigInt& a, const BigInt& b) { return divmod(a, b).second; }
    BigInt& operator+=(const BigInt& o) { return *this = *this + o; }
    BigInt& operator-=(const BigInt& o) { return *this = *this - o; }
    BigInt& operator*=(const BigInt& o) { return *this = *this * o; }
    BigInt& operator/=(const BigInt& o) { return *this = *this / o; }
    BigInt& operator%=(const BigInt& o) { return *this = *this % o; }

    /// Truncated division: a = q*b + r, |r| < |b|, sign(r) = sign(a). Throws on division by zero.
    [[nodiscard]] static std::pair<BigInt, BigInt> divmod(const BigInt& a, const BigInt& b) {
        if (b.mag_.empty()) throw MathException("BigInt: division by zero");
        BigInt q, r;
        divmod_mag(a.mag_, b.mag_, q.mag_, r.mag_);
        q.neg_ = a.neg_ != b.neg_;
        r.neg_ = a.neg_;
        q.trim();
        r.trim();
        return {std::move(q), std::move(r)};
    }

    [[nodiscard]] static BigInt pow(const BigInt& base, usize exponent) {
        BigInt r(1), b = base;
        while (exponent) {
            if (exponent & 1) r = r * b;
            exponent >>= 1;
            if (exponent) b = b * b;
        }
        return r;
    }

    /// (base ^ exponent) mod modulus for exponent >= 0 and modulus > 0; result in [0, modulus).
    [[nodiscard]] static BigInt mod_pow(BigInt base, const BigInt& exponent, const BigInt& modulus) {
        if (modulus.sign() <= 0) throw MathException("mod_pow: modulus must be positive");
        if (exponent.is_negative()) throw MathException("mod_pow: negative exponent");
        BigInt result(1);
        base = base % modulus;
        if (base.is_negative()) base += modulus;
        const usize bits = exponent.bit_length();
        for (usize i = 0; i < bits; ++i) {
            if (exponent.test_bit(i)) result = (result * base) % modulus;
            base = (base * base) % modulus;
        }
        return result % modulus;
    }

    [[nodiscard]] static BigInt gcd(BigInt a, BigInt b) {
        a = a.abs();
        b = b.abs();
        while (!b.is_zero()) {
            BigInt t = a % b;
            a = std::move(b);
            b = std::move(t);
        }
        return a;
    }

    /// floor(sqrt(n)); throws for negative n.
    [[nodiscard]] static BigInt isqrt(const BigInt& n) {
        if (n.is_negative()) throw MathException("isqrt: negative argument");
        if (n.is_zero()) return BigInt();
        BigInt x = BigInt(1).shl((n.bit_length() + 1) / 2);  // >= sqrt(n)
        while (true) {
            const BigInt y = (x + n / x).shr(1);
            if (y >= x) return x;
            x = y;
        }
    }

    [[nodiscard]] bool test_bit(usize i) const noexcept {
        const usize limb = i / 32;
        return limb < mag_.size() && ((mag_[limb] >> (i % 32)) & 1u) != 0;
    }

    // ---- comparison ----
    friend bool operator==(const BigInt& a, const BigInt& b) noexcept { return a.neg_ == b.neg_ && a.mag_ == b.mag_; }
    friend std::strong_ordering operator<=>(const BigInt& a, const BigInt& b) noexcept {
        if (a.neg_ != b.neg_) return a.neg_ ? std::strong_ordering::less : std::strong_ordering::greater;
        const int c = cmp_mag(a.mag_, b.mag_);
        const int s = a.neg_ ? -c : c;
        return s < 0 ? std::strong_ordering::less : (s > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
    }

    // ---- text ----
    [[nodiscard]] std::string to_string(unsigned base = 10) const {
        if (base != 10 && base != 16) throw MathException("BigInt::to_string: base must be 10 or 16");
        if (mag_.empty()) return "0";
        std::string out;
        if (base == 16) {
            static constexpr char digits[] = "0123456789abcdef";
            for (usize i = mag_.size(); i-- > 0;)
                for (int s = 28; s >= 0; s -= 4) out.push_back(digits[(mag_[i] >> s) & 0xF]);
            out.erase(0, out.find_first_not_of('0'));
            out.insert(0, neg_ ? "-0x" : "0x");
            return out;
        }
        std::vector<u32> tmp = mag_;
        std::vector<u32> chunks;
        while (!tmp.empty()) {
            u64 rem = 0;
            for (usize i = tmp.size(); i-- > 0;) {
                const u64 cur = (rem << 32) | tmp[i];
                tmp[i] = static_cast<u32>(cur / 1000000000u);
                rem = cur % 1000000000u;
            }
            chunks.push_back(static_cast<u32>(rem));
            while (!tmp.empty() && tmp.back() == 0) tmp.pop_back();
        }
        if (neg_) out.push_back('-');
        out += std::to_string(chunks.back());
        for (usize i = chunks.size() - 1; i-- > 0;) {
            const std::string c = std::to_string(chunks[i]);
            out.append(9 - c.size(), '0');
            out += c;
        }
        return out;
    }

private:
    using Mag = std::vector<u32>;

    void assign_u64(u64 v) {
        mag_.clear();
        neg_ = false;
        if (v) mag_.push_back(static_cast<u32>(v));
        if (v >> 32) mag_.push_back(static_cast<u32>(v >> 32));
    }
    void assign_i64(i64 v) {
        assign_u64(v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v));
        neg_ = v < 0;
    }
    [[nodiscard]] u64 magnitude_u64() const noexcept {
        u64 m = 0;
        if (!mag_.empty()) m = mag_[0];
        if (mag_.size() > 1) m |= static_cast<u64>(mag_[1]) << 32;
        return m;
    }
    void trim() noexcept {
        while (!mag_.empty() && mag_.back() == 0) mag_.pop_back();
        if (mag_.empty()) neg_ = false;
    }

    static int cmp_mag(const Mag& a, const Mag& b) noexcept {
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        for (usize i = a.size(); i-- > 0;)
            if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }
    static Mag add_mag(const Mag& a, const Mag& b) {
        const Mag& big = a.size() >= b.size() ? a : b;
        const Mag& small = a.size() >= b.size() ? b : a;
        Mag r;
        r.reserve(big.size() + 1);
        u64 carry = 0;
        for (usize i = 0; i < big.size(); ++i) {
            const u64 s = u64{big[i]} + (i < small.size() ? small[i] : 0) + carry;
            r.push_back(static_cast<u32>(s));
            carry = s >> 32;
        }
        if (carry) r.push_back(static_cast<u32>(carry));
        return r;
    }
    /// a - b, requires |a| >= |b|.
    static Mag sub_mag(const Mag& a, const Mag& b) {
        Mag r;
        r.reserve(a.size());
        i64 borrow = 0;
        for (usize i = 0; i < a.size(); ++i) {
            i64 d = static_cast<i64>(a[i]) - borrow - (i < b.size() ? static_cast<i64>(b[i]) : 0);
            borrow = d < 0;
            if (d < 0) d += (i64{1} << 32);
            r.push_back(static_cast<u32>(d));
        }
        while (!r.empty() && r.back() == 0) r.pop_back();
        return r;
    }
    static Mag mul_mag(const Mag& a, const Mag& b) {
        Mag r(a.size() + b.size(), 0);
        for (usize i = 0; i < a.size(); ++i) {
            u64 carry = 0;
            const u64 ai = a[i];
            for (usize j = 0; j < b.size(); ++j) {
                const u64 cur = r[i + j] + ai * b[j] + carry;
                r[i + j] = static_cast<u32>(cur);
                carry = cur >> 32;
            }
            r[i + b.size()] = static_cast<u32>(carry);
        }
        while (!r.empty() && r.back() == 0) r.pop_back();
        return r;
    }
    static void mul_small(Mag& a, u32 m) {
        u64 carry = 0;
        for (u32& w : a) {
            const u64 cur = u64{w} * m + carry;
            w = static_cast<u32>(cur);
            carry = cur >> 32;
        }
        if (carry) a.push_back(static_cast<u32>(carry));
    }
    static void add_small(Mag& a, u32 v) {
        u64 carry = v;
        for (usize i = 0; carry && i < a.size(); ++i) {
            const u64 cur = u64{a[i]} + carry;
            a[i] = static_cast<u32>(cur);
            carry = cur >> 32;
        }
        if (carry) a.push_back(static_cast<u32>(carry));
    }

    /// Knuth algorithm D (TAOCP 4.3.1). q and r receive the magnitudes of the quotient and remainder.
    static void divmod_mag(const Mag& a, const Mag& b, Mag& q, Mag& r) {
        q.clear();
        r.clear();
        if (cmp_mag(a, b) < 0) { r = a; return; }
        if (b.size() == 1) {
            q.assign(a.size(), 0);
            u64 rem = 0;
            for (usize i = a.size(); i-- > 0;) {
                const u64 cur = (rem << 32) | a[i];
                q[i] = static_cast<u32>(cur / b[0]);
                rem = cur % b[0];
            }
            if (rem) r.push_back(static_cast<u32>(rem));
            while (!q.empty() && q.back() == 0) q.pop_back();
            return;
        }
        const usize n = b.size(), m = a.size() - n;
        const unsigned s = static_cast<unsigned>(std::countl_zero(b.back()));
        Mag v(n), u(a.size() + 1);
        for (usize i = n; i-- > 0;)
            v[i] = (b[i] << s) | (s && i > 0 ? b[i - 1] >> (32 - s) : 0);
        u[a.size()] = s ? a.back() >> (32 - s) : 0;
        for (usize i = a.size(); i-- > 0;)
            u[i] = (a[i] << s) | (s && i > 0 ? a[i - 1] >> (32 - s) : 0);
        q.assign(m + 1, 0);
        constexpr u64 B = u64{1} << 32;
        for (usize j = m + 1; j-- > 0;) {
            const u64 num = (u64{u[j + n]} << 32) | u[j + n - 1];
            u64 qhat = num / v[n - 1];
            u64 rhat = num % v[n - 1];
            while (qhat >= B || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
                --qhat;
                rhat += v[n - 1];
                if (rhat >= B) break;
            }
            i64 borrow = 0;
            u64 carry = 0;
            for (usize i = 0; i < n; ++i) {  // u[j..j+n] -= qhat * v
                const u64 p = qhat * v[i] + carry;
                carry = p >> 32;
                const i64 t = static_cast<i64>(u[i + j]) - borrow - static_cast<i64>(p & 0xFFFFFFFFu);
                u[i + j] = static_cast<u32>(t);
                borrow = t < 0 ? 1 : 0;
            }
            const i64 t = static_cast<i64>(u[j + n]) - borrow - static_cast<i64>(carry);
            u[j + n] = static_cast<u32>(t);
            q[j] = static_cast<u32>(qhat);
            if (t < 0) {  // qhat was one too large: add the divisor back
                --q[j];
                u64 c = 0;
                for (usize i = 0; i < n; ++i) {
                    const u64 sum = u64{u[i + j]} + v[i] + c;
                    u[i + j] = static_cast<u32>(sum);
                    c = sum >> 32;
                }
                u[j + n] += static_cast<u32>(c);
            }
        }
        r.assign(n, 0);
        for (usize i = 0; i < n; ++i) r[i] = (u[i] >> s) | (s ? u[i + 1] << (32 - s) : 0);
        while (!q.empty() && q.back() == 0) q.pop_back();
        while (!r.empty() && r.back() == 0) r.pop_back();
    }

    Mag mag_;
    bool neg_ = false;
};

inline std::ostream& operator<<(std::ostream& os, const BigInt& v) { return os << v.to_string(); }

}  // namespace flyg::math
