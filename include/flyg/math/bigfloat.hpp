// flyG - arbitrary precision binary floating point
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cmath>
#include <compare>
#include <string>
#include <string_view>

#include <flyg/math/bigint.hpp>

namespace flyg::math {

/// value = mantissa * 2^exponent, with the mantissa limited to `precision` bits.
///
/// Rounding: every operation rounds the exact (or guard-bit-extended) result to nearest,
/// ties away from zero. + - * are correctly rounded; / and sqrt carry an error below
/// 1 ulp of the result precision (they use 2 guard bits and truncating integer division).
/// The result precision is the larger of the two operand precisions. No NaN/Inf: invalid
/// operations (divide by zero, sqrt of a negative) throw MathException.
class BigFloat {
public:
    static constexpr usize default_precision = 128;

    BigFloat() : prec_(default_precision) {}
    /// Zero with the given precision in bits.
    [[nodiscard]] static BigFloat zero(usize precision = default_precision) { return BigFloat(prec_tag{}, precision); }
    BigFloat(const BigInt& v, usize precision = default_precision) : mant_(v), prec_(check_prec(precision)) { normalize(); }
    BigFloat(long long v, usize precision = default_precision) : BigFloat(BigInt(v), precision) {}
    BigFloat(int v, usize precision = default_precision) : BigFloat(BigInt(v), precision) {}

    /// Exact conversion of a finite double.
    [[nodiscard]] static BigFloat from_double(double v, usize precision = default_precision) {
        if (!std::isfinite(v)) throw MathException("BigFloat::from_double: value is not finite");
        BigFloat r(prec_tag{}, precision);
        if (v == 0.0) return r;
        int e = 0;
        const double frac = std::frexp(v, &e);  // v = frac * 2^e, 0.5 <= |frac| < 1
        r.mant_ = BigInt(static_cast<long long>(std::ldexp(frac, 53)));
        r.exp_ = e - 53;
        r.normalize();
        return r;
    }

    /// Parses [sign] digits [. digits] [e|E [sign] digits].
    [[nodiscard]] static BigFloat from_string(std::string_view s, usize precision = default_precision) {
        usize pos = 0;
        bool neg = false;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) neg = s[pos++] == '-';
        std::string digits;
        long long frac_digits = 0;
        bool seen_dot = false, any = false;
        for (; pos < s.size(); ++pos) {
            const char c = s[pos];
            if (c >= '0' && c <= '9') { digits.push_back(c); any = true; if (seen_dot) ++frac_digits; }
            else if (c == '.' && !seen_dot) seen_dot = true;
            else break;
        }
        if (!any) throw DataException("BigFloat::from_string: no digits", pos);
        long long e10 = 0;
        if (pos < s.size()) {
            if (s[pos] != 'e' && s[pos] != 'E') throw DataException("BigFloat::from_string: unexpected character", pos);
            ++pos;
            bool eneg = false;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) eneg = s[pos++] == '-';
            if (pos >= s.size()) throw DataException("BigFloat::from_string: missing exponent digits", pos);
            for (; pos < s.size(); ++pos) {
                if (s[pos] < '0' || s[pos] > '9') throw DataException("BigFloat::from_string: bad exponent", pos);
                e10 = e10 * 10 + (s[pos] - '0');
                if (e10 > 100000000) throw DataException("BigFloat::from_string: exponent too large", pos);
            }
            if (eneg) e10 = -e10;
        }
        const long long k = e10 - frac_digits;
        const usize work = precision + 64;
        BigFloat d(BigInt::from_string(digits), work);
        if (k >= 0) d = d * BigFloat(BigInt::pow10(static_cast<usize>(k)), work);
        else d = d / BigFloat(BigInt::pow10(static_cast<usize>(-k)), work);
        if (neg) d = -d;
        d.set_precision(precision);
        return d;
    }

    /// pi to `precision` bits (Machin: 16 atan(1/5) - 4 atan(1/239)).
    [[nodiscard]] static BigFloat pi(usize precision = default_precision) {
        const usize guard = 32;
        const usize bits = precision + guard;
        const BigInt one = BigInt(1).shl(bits);
        auto arctan_inv = [&](unsigned x) {  // atan(1/x) in fixed point with `bits` fractional bits
            const BigInt x2(static_cast<unsigned long long>(x) * x);
            BigInt term = one / BigInt(x);
            BigInt sum = term;
            for (unsigned k = 1; !term.is_zero(); ++k) {
                term = term / x2;
                const BigInt t = term / BigInt(2 * k + 1);
                sum = (k % 2) ? sum - t : sum + t;
            }
            return sum;
        };
        const BigInt pi_fixed = arctan_inv(5) * BigInt(16) - arctan_inv(239) * BigInt(4);
        BigFloat r(prec_tag{}, precision + guard);
        r.mant_ = pi_fixed;
        r.exp_ = -static_cast<long long>(bits);
        r.normalize();
        r.set_precision(precision);
        return r;
    }

    /// Euler's number e to `precision` bits (sum of 1/k!).
    [[nodiscard]] static BigFloat euler_number(usize precision = default_precision) {
        const usize bits = precision + 32;
        const BigInt one = BigInt(1).shl(bits);
        BigInt term = one, sum = one;
        for (unsigned k = 1; !term.is_zero(); ++k) {
            term = term / BigInt(k);
            sum += term;
        }
        BigFloat r(prec_tag{}, precision + 32);
        r.mant_ = sum;
        r.exp_ = -static_cast<long long>(bits);
        r.normalize();
        r.set_precision(precision);
        return r;
    }

    // ---- queries ----
    [[nodiscard]] usize precision() const noexcept { return prec_; }
    [[nodiscard]] bool is_zero() const noexcept { return mant_.is_zero(); }
    [[nodiscard]] bool is_negative() const noexcept { return mant_.is_negative(); }
    [[nodiscard]] int sign() const noexcept { return mant_.sign(); }
    [[nodiscard]] const BigInt& mantissa() const noexcept { return mant_; }
    [[nodiscard]] long long exponent() const noexcept { return exp_; }

    /// Re-rounds to a new precision.
    void set_precision(usize precision) {
        prec_ = check_prec(precision);
        normalize();
    }

    [[nodiscard]] BigFloat abs() const { BigFloat r = *this; r.mant_ = r.mant_.abs(); return r; }
    [[nodiscard]] BigFloat operator-() const { BigFloat r = *this; r.mant_ = -r.mant_; return r; }

    friend BigFloat operator+(const BigFloat& a, const BigFloat& b) {
        BigFloat r(prec_tag{}, std::max(a.prec_, b.prec_));
        if (a.is_zero() || b.is_zero()) {
            const BigFloat& nz = a.is_zero() ? b : a;
            r.mant_ = nz.mant_;
            r.exp_ = nz.exp_;
            r.normalize();
            return r;
        }
        const long long top_a = static_cast<long long>(a.mant_.bit_length()) + a.exp_;
        const long long top_b = static_cast<long long>(b.mant_.bit_length()) + b.exp_;
        const long long far = static_cast<long long>(r.prec_) + 2;
        if (top_a - top_b > far || top_b - top_a > far) {
            // the smaller operand lies below half an ulp of the result: the larger one wins
            const BigFloat& big = top_a > top_b ? a : b;
            r.mant_ = big.mant_;
            r.exp_ = big.exp_;
            r.normalize();
            return r;
        }
        const long long base_exp = std::min(a.exp_, b.exp_);
        r.mant_ = a.mant_.shl(static_cast<usize>(a.exp_ - base_exp)) + b.mant_.shl(static_cast<usize>(b.exp_ - base_exp));
        r.exp_ = base_exp;
        r.normalize();
        return r;
    }
    friend BigFloat operator-(const BigFloat& a, const BigFloat& b) { return a + (-b); }
    friend BigFloat operator*(const BigFloat& a, const BigFloat& b) {
        BigFloat r(prec_tag{}, std::max(a.prec_, b.prec_));
        r.mant_ = a.mant_ * b.mant_;
        r.exp_ = a.exp_ + b.exp_;
        r.normalize();
        return r;
    }
    /// Throws MathException on division by zero.
    friend BigFloat operator/(const BigFloat& a, const BigFloat& b) {
        if (b.is_zero()) throw MathException("BigFloat: division by zero");
        BigFloat r(prec_tag{}, std::max(a.prec_, b.prec_));
        if (a.is_zero()) return r;
        const long long want = static_cast<long long>(r.prec_) + 2 + static_cast<long long>(b.mant_.bit_length()) -
                               static_cast<long long>(a.mant_.bit_length());
        const usize shift = want > 0 ? static_cast<usize>(want) : 0;
        r.mant_ = a.mant_.shl(shift) / b.mant_;
        r.exp_ = a.exp_ - static_cast<long long>(shift) - b.exp_;
        r.normalize();
        return r;
    }
    BigFloat& operator+=(const BigFloat& o) { return *this = *this + o; }
    BigFloat& operator-=(const BigFloat& o) { return *this = *this - o; }
    BigFloat& operator*=(const BigFloat& o) { return *this = *this * o; }
    BigFloat& operator/=(const BigFloat& o) { return *this = *this / o; }

    [[nodiscard]] static BigFloat sqrt(const BigFloat& x) {
        if (x.is_negative()) throw MathException("BigFloat::sqrt: negative argument");
        BigFloat r(prec_tag{}, x.prec_);
        if (x.is_zero()) return r;
        const long long need = static_cast<long long>(2 * x.prec_ + 4) - static_cast<long long>(x.mant_.bit_length());
        long long shift = need > 0 ? need : 0;
        if (((x.exp_ - shift) % 2) != 0) ++shift;  // keep the exponent even
        r.mant_ = BigInt::isqrt(x.mant_.shl(static_cast<usize>(shift)));
        r.exp_ = (x.exp_ - shift) / 2;
        r.normalize();
        return r;
    }

    // ---- comparison (by value) ----
    friend bool operator==(const BigFloat& a, const BigFloat& b) { return compare(a, b) == 0; }
    friend std::strong_ordering operator<=>(const BigFloat& a, const BigFloat& b) {
        const int c = compare(a, b);
        return c < 0 ? std::strong_ordering::less : (c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
    }

    // ---- conversion ----
    [[nodiscard]] double to_double() const {
        if (mant_.is_zero()) return 0.0;
        const usize bl = mant_.bit_length();
        const BigInt top = bl > 64 ? mant_.abs().shr(bl - 64) : mant_.abs();
        const long long ex = exp_ + static_cast<long long>(bl > 64 ? bl - 64 : 0);
        double r = top.to_double();
        if (ex > 5000) r = HUGE_VAL;
        else if (ex < -5000) r = 0.0;
        else r = std::ldexp(r, static_cast<int>(ex));
        return mant_.is_negative() ? -r : r;
    }

    /// Scientific notation with `significant_digits` digits (default: what `precision` can hold).
    [[nodiscard]] std::string to_string(usize significant_digits = 0) const {
        if (mant_.is_zero()) return "0";
        usize sig = significant_digits ? significant_digits : static_cast<usize>(static_cast<double>(prec_) * 0.30102999566) ;
        if (sig == 0) sig = 1;
        const usize work = prec_ + static_cast<usize>(static_cast<double>(sig) * 3.33) + 64;
        long long e10 = static_cast<long long>(std::floor(static_cast<double>(static_cast<long long>(mant_.bit_length()) + exp_ - 1) * 0.30102999566));
        std::string digits;
        for (int attempt = 0; attempt < 4; ++attempt) {
            digits = scaled_digits(abs(), static_cast<long long>(sig) - 1 - e10, work).to_string();
            if (digits.size() > sig) ++e10;
            else if (digits.size() < sig) --e10;
            else break;
        }
        std::string out = mant_.is_negative() ? "-" : "";
        out += digits[0];
        if (digits.size() > 1) { out += '.'; out.append(digits, 1, std::string::npos); }
        out += 'e';
        out += std::to_string(e10);
        return out;
    }

    /// Fixed notation with exactly `decimals` digits after the decimal point (rounded half away from zero).
    [[nodiscard]] std::string to_fixed(usize decimals) const {
        const usize intbits = mant_.bit_length() + (exp_ > 0 ? static_cast<usize>(exp_) : 0);
        const usize work = prec_ + static_cast<usize>(static_cast<double>(decimals) * 3.33) + intbits + 64;
        std::string digits = scaled_digits(abs(), static_cast<long long>(decimals), work).to_string();
        if (digits.size() <= decimals) digits.insert(0, decimals + 1 - digits.size(), '0');
        std::string out = mant_.is_negative() ? "-" : "";
        out.append(digits, 0, digits.size() - decimals);
        if (decimals) { out += '.'; out.append(digits, digits.size() - decimals, std::string::npos); }
        if (out == "-0" || (out.size() > 1 && out[0] == '-' && out.find_first_not_of("-0.") == std::string::npos)) out.erase(0, 1);
        return out;
    }

private:
    struct prec_tag {};
    BigFloat(prec_tag, usize precision) : prec_(check_prec(precision)) {}

    static usize check_prec(usize p) {
        if (p < 2 || p > (usize{1} << 24)) throw MathException("BigFloat: precision must be in [2, 2^24] bits");
        return p;
    }

    /// Rounds the mantissa to prec_ bits (half away from zero) and strips trailing zero bits.
    void normalize() {
        if (mant_.is_zero()) { exp_ = 0; return; }
        usize bl = mant_.bit_length();
        if (bl > prec_) {
            const usize drop = bl - prec_;
            const bool neg = mant_.is_negative();
            BigInt m = mant_.abs();
            m += BigInt(1).shl(drop - 1);
            m = m.shr(drop);
            mant_ = neg ? -m : m;
            exp_ += static_cast<long long>(drop);
        }
        const usize tz = mant_.trailing_zero_bits();
        if (tz) { mant_ = mant_.shr(tz); exp_ += static_cast<long long>(tz); }
    }

    /// round(|x| * 10^k) as an integer, computed at `work` bits.
    static BigInt scaled_digits(const BigFloat& x, long long k, usize work) {
        BigFloat v = x;
        v.prec_ = std::max(work, x.prec_);
        BigFloat scaled = k >= 0 ? v * BigFloat(BigInt::pow10(static_cast<usize>(k)), v.prec_)
                                 : v / BigFloat(BigInt::pow10(static_cast<usize>(-k)), v.prec_);
        // round to integer, half up
        if (scaled.exp_ >= 0) return scaled.mant_.shl(static_cast<usize>(scaled.exp_));
        const usize drop = static_cast<usize>(-scaled.exp_);
        return (scaled.mant_ + BigInt(1).shl(drop - 1)).shr(drop);
    }

    static int compare(const BigFloat& a, const BigFloat& b) {
        if (a.sign() != b.sign()) return a.sign() < b.sign() ? -1 : 1;
        if (a.is_zero()) return 0;
        const long long ma = static_cast<long long>(a.mant_.bit_length()) + a.exp_;
        const long long mb = static_cast<long long>(b.mant_.bit_length()) + b.exp_;
        if (ma != mb) return (ma < mb) == (a.sign() > 0) ? -1 : 1;
        const long long e = std::min(a.exp_, b.exp_);
        const BigInt x = a.mant_.shl(static_cast<usize>(a.exp_ - e));
        const BigInt y = b.mant_.shl(static_cast<usize>(b.exp_ - e));
        return x < y ? -1 : (x > y ? 1 : 0);
    }

    BigInt mant_;
    long long exp_ = 0;
    usize prec_;
};

}  // namespace flyg::math
