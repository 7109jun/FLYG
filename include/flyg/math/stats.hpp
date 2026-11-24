// flyG - descriptive statistics and compensated summation
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <ranges>
#include <span>
#include <vector>

#include <flyg/math/scalar.hpp>

namespace flyg::math {

template <class R>
concept RealRange = std::ranges::contiguous_range<R> && std::ranges::sized_range<R> &&
                    Real<std::remove_cv_t<std::ranges::range_value_t<R>>>;

template <RealRange R>
[[nodiscard]] auto as_span(const R& r) {
    using T = std::remove_cv_t<std::ranges::range_value_t<R>>;
    return std::span<const T>(std::ranges::data(r), std::ranges::size(r));
}

/// Neumaier (improved Kahan) first-order compensated sum. Handles cancellation (1e16 + 1 - 1e16)
/// but its single compensation term can itself lose accuracy for very long, biased inputs;
/// prefer klein_sum() for those.
template <RealRange R>
[[nodiscard]] auto neumaier_sum(const R& range) {
    const auto data = as_span(range);
    using T = typename decltype(data)::value_type;
    T sum = T(0);
    T comp = T(0);
    for (const T v : data) {
        const T t = sum + v;
        comp += (std::fabs(sum) >= std::fabs(v)) ? (sum - t) + v : (v - t) + sum;
        sum = t;
    }
    return sum + comp;
}

/// Second-order compensated (Klein / iterated Kahan-Babuska) sum. Unlike the first-order
/// Neumaier sum, the compensation term is itself compensated, so it stays accurate when the
/// rounding errors are systematically biased (e.g. adding 0.1f a million times). This is the
/// summation used by sum/mean/variance/....
template <RealRange R>
[[nodiscard]] auto klein_sum(const R& range) {
    const auto data = as_span(range);
    using T = typename decltype(data)::value_type;
    T s = T(0), cs = T(0), ccs = T(0);
    for (const T x : data) {
        T t = s + x;
        T c = (std::fabs(s) >= std::fabs(x)) ? (s - t) + x : (x - t) + s;
        s = t;
        t = cs + c;
        const T cc = (std::fabs(cs) >= std::fabs(c)) ? (cs - t) + c : (c - t) + cs;
        cs = t;
        ccs += cc;
    }
    return s + cs + ccs;
}

/// Classic Kahan compensated sum.
template <RealRange R>
[[nodiscard]] auto kahan_sum(const R& range) {
    const auto data = as_span(range);
    using T = typename decltype(data)::value_type;
    T sum = T(0);
    T c = T(0);
    for (const T v : data) {
        const T y = v - c;
        const T t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

namespace detail {
template <class T>
void require_non_empty(std::span<const T> d, const char* fn) {
    if (d.empty()) throw MathException(std::string(fn) + ": empty input");
}
}  // namespace detail

template <RealRange R>
[[nodiscard]] auto sum(const R& r) { return klein_sum(r); }

template <RealRange R>
[[nodiscard]] auto mean(const R& r) {
    const auto d = as_span(r);
    detail::require_non_empty(d, "mean");
    return klein_sum(d) / static_cast<typename decltype(d)::value_type>(d.size());
}

/// Variance with `ddof` delta degrees of freedom (1 = sample, 0 = population). Two-pass.
template <RealRange R>
[[nodiscard]] auto variance(const R& r, usize ddof = 1) {
    const auto d = as_span(r);
    using T = typename decltype(d)::value_type;
    detail::require_non_empty(d, "variance");
    if (d.size() <= ddof) throw MathException("variance: not enough samples for the requested ddof");
    const T m = mean(d);
    std::vector<T> sq(d.size());
    for (usize i = 0; i < d.size(); ++i) sq[i] = (d[i] - m) * (d[i] - m);
    return klein_sum(sq) / static_cast<T>(d.size() - ddof);
}

template <RealRange R>
[[nodiscard]] auto stddev(const R& r, usize ddof = 1) { return std::sqrt(variance(r, ddof)); }

template <RealRange R>
[[nodiscard]] auto min_value(const R& r) {
    const auto d = as_span(r);
    detail::require_non_empty(d, "min_value");
    return *std::min_element(d.begin(), d.end());
}

template <RealRange R>
[[nodiscard]] auto max_value(const R& r) {
    const auto d = as_span(r);
    detail::require_non_empty(d, "max_value");
    return *std::max_element(d.begin(), d.end());
}

template <RealRange R>
[[nodiscard]] auto rms(const R& r) {
    const auto d = as_span(r);
    using T = typename decltype(d)::value_type;
    detail::require_non_empty(d, "rms");
    std::vector<T> sq(d.size());
    for (usize i = 0; i < d.size(); ++i) sq[i] = d[i] * d[i];
    return std::sqrt(klein_sum(sq) / static_cast<T>(d.size()));
}

/// Percentile with linear interpolation between closest ranks; `p` in [0, 100].
template <RealRange R>
[[nodiscard]] auto percentile(const R& r, double p) {
    const auto d = as_span(r);
    using T = typename decltype(d)::value_type;
    detail::require_non_empty(d, "percentile");
    if (!(p >= 0.0 && p <= 100.0)) throw MathException("percentile: p must be in [0, 100]");
    std::vector<T> v(d.begin(), d.end());
    std::sort(v.begin(), v.end());
    const double pos = p / 100.0 * static_cast<double>(v.size() - 1);
    const usize lo = static_cast<usize>(pos);
    const usize hi = std::min(lo + 1, v.size() - 1);
    const T frac = static_cast<T>(pos - static_cast<double>(lo));
    return v[lo] + (v[hi] - v[lo]) * frac;
}

template <RealRange R>
[[nodiscard]] auto median(const R& r) { return percentile(r, 50.0); }

template <RealRange A, RealRange B>
[[nodiscard]] auto covariance(const A& a, const B& b, usize ddof = 1) {
    const auto x = as_span(a);
    const auto y = as_span(b);
    using T = typename decltype(x)::value_type;
    static_assert(std::is_same_v<T, typename decltype(y)::value_type>, "element types must match");
    if (x.size() != y.size()) throw MathException("covariance: size mismatch");
    detail::require_non_empty(x, "covariance");
    if (x.size() <= ddof) throw MathException("covariance: not enough samples");
    const T mx = mean(x);
    const T my = mean(y);
    std::vector<T> prod(x.size());
    for (usize i = 0; i < x.size(); ++i) prod[i] = (x[i] - mx) * (y[i] - my);
    return klein_sum(prod) / static_cast<T>(x.size() - ddof);
}

/// Pearson correlation coefficient in [-1, 1]. Throws when either input has zero variance.
template <RealRange A, RealRange B>
[[nodiscard]] auto correlation(const A& a, const B& b) {
    const auto cov = covariance(a, b);
    const auto sx = stddev(a);
    const auto sy = stddev(b);
    if (sx == 0 || sy == 0) throw MathException("correlation: zero variance");
    return cov / (sx * sy);
}

/// Fisher-Pearson skewness (population moments).
template <RealRange R>
[[nodiscard]] auto skewness(const R& r) {
    const auto d = as_span(r);
    using T = typename decltype(d)::value_type;
    detail::require_non_empty(d, "skewness");
    const T m = mean(d);
    const T s = std::sqrt(variance(d, 0));
    if (s == 0) throw MathException("skewness: zero variance");
    std::vector<T> c(d.size());
    for (usize i = 0; i < d.size(); ++i) c[i] = std::pow((d[i] - m) / s, T(3));
    return klein_sum(c) / static_cast<T>(d.size());
}

/// Excess kurtosis (population moments; 0 for a normal distribution).
template <RealRange R>
[[nodiscard]] auto kurtosis(const R& r) {
    const auto d = as_span(r);
    using T = typename decltype(d)::value_type;
    detail::require_non_empty(d, "kurtosis");
    const T m = mean(d);
    const T s = std::sqrt(variance(d, 0));
    if (s == 0) throw MathException("kurtosis: zero variance");
    std::vector<T> c(d.size());
    for (usize i = 0; i < d.size(); ++i) c[i] = std::pow((d[i] - m) / s, T(4));
    return klein_sum(c) / static_cast<T>(d.size()) - T(3);
}

/// Streaming statistics (Welford). Supports merging partial results.
template <Real T = double>
class RunningStats {
public:
    void push(T v) noexcept {
        ++n_;
        const T delta = v - mean_;
        mean_ += delta / static_cast<T>(n_);
        m2_ += delta * (v - mean_);
        if (n_ == 1) {
            min_ = max_ = v;
        } else {
            min_ = std::min(min_, v);
            max_ = std::max(max_, v);
        }
    }
    template <RealRange R>
    void push_all(const R& r) {
        for (const auto v : as_span(r)) push(static_cast<T>(v));
    }
    void merge(const RunningStats& o) noexcept {
        if (o.n_ == 0) return;
        if (n_ == 0) {
            *this = o;
            return;
        }
        const T na = static_cast<T>(n_);
        const T nb = static_cast<T>(o.n_);
        const T delta = o.mean_ - mean_;
        const T total = na + nb;
        m2_ += o.m2_ + delta * delta * na * nb / total;
        mean_ += delta * nb / total;
        n_ += o.n_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    [[nodiscard]] usize count() const noexcept { return n_; }
    [[nodiscard]] T mean() const {
        if (n_ == 0) throw MathException("RunningStats: no samples");
        return mean_;
    }
    [[nodiscard]] T variance(usize ddof = 1) const {
        if (n_ <= ddof) throw MathException("RunningStats: not enough samples");
        return m2_ / static_cast<T>(n_ - ddof);
    }
    [[nodiscard]] T stddev(usize ddof = 1) const { return std::sqrt(variance(ddof)); }
    [[nodiscard]] T min() const {
        if (n_ == 0) throw MathException("RunningStats: no samples");
        return min_;
    }
    [[nodiscard]] T max() const {
        if (n_ == 0) throw MathException("RunningStats: no samples");
        return max_;
    }

private:
    usize n_ = 0;
    T mean_ = 0, m2_ = 0, min_ = 0, max_ = 0;
};

/// Fixed-width histogram over [lo, hi); values outside are counted in under/overflow.
template <Real T = double>
class Histogram {
public:
    Histogram(T lo, T hi, usize bins) : lo_(lo), hi_(hi), counts_(bins, 0) {
        if (bins == 0 || !(lo < hi)) throw MathException("Histogram: need bins > 0 and lo < hi");
    }
    void add(T v) noexcept {
        if (v < lo_) {
            ++under_;
        } else if (!(v < hi_)) {
            ++over_;
        } else {
            usize b = static_cast<usize>((v - lo_) / (hi_ - lo_) * static_cast<T>(counts_.size()));
            if (b >= counts_.size()) b = counts_.size() - 1;
            ++counts_[b];
        }
    }
    [[nodiscard]] const std::vector<usize>& counts() const noexcept { return counts_; }
    [[nodiscard]] usize underflow() const noexcept { return under_; }
    [[nodiscard]] usize overflow() const noexcept { return over_; }
    [[nodiscard]] T bin_low(usize i) const {
        if (i > counts_.size()) throw MathException("Histogram: bin index out of range");
        return lo_ + (hi_ - lo_) * static_cast<T>(i) / static_cast<T>(counts_.size());
    }

private:
    T lo_, hi_;
    std::vector<usize> counts_;
    usize under_ = 0, over_ = 0;
};

}  // namespace flyg::math
