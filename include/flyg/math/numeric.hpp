// flyG - numerical calculus: integration, differentiation, root finding, ODEs, interpolation
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include <flyg/math/scalar.hpp>

namespace flyg::math {

template <class F, class T>
concept ScalarFunction = Real<T> && std::invocable<F&, T> && std::convertible_to<std::invoke_result_t<F&, T>, T>;

/// Result of an adaptive algorithm: value plus an error estimate and the work done.
template <Real T>
struct NumericResult {
    T value{};
    T error_estimate{};
    usize evaluations = 0;
};

// ============================================================================
// integration
// ============================================================================

/// Composite trapezoid rule with `n` panels.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T integrate_trapezoid(F&& f, T a, T b, usize n) {
    if (n == 0) throw MathException("integrate_trapezoid: n must be > 0");
    const T h = (b - a) / static_cast<T>(n);
    T sum = (f(a) + f(b)) / T(2);
    for (usize i = 1; i < n; ++i) sum += f(a + h * static_cast<T>(i));
    return sum * h;
}

/// Composite Simpson rule; `n` must be even.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T integrate_simpson(F&& f, T a, T b, usize n) {
    if (n == 0 || n % 2 != 0) throw MathException("integrate_simpson: n must be even and > 0");
    const T h = (b - a) / static_cast<T>(n);
    T odd = T(0), even = T(0);
    for (usize i = 1; i < n; ++i) (i % 2 ? odd : even) += f(a + h * static_cast<T>(i));
    return (f(a) + f(b) + T(4) * odd + T(2) * even) * h / T(3);
}

namespace detail {
template <Real T, class F>
T adaptive_simpson_rec(F& f, T a, T b, T fa, T fm, T fb, T whole, T tol, int depth, usize& evals, T& err) {
    const T m = (a + b) / T(2);
    const T lm = (a + m) / T(2), rm = (m + b) / T(2);
    const T flm = f(lm), frm = f(rm);
    evals += 2;
    const T left = (m - a) / T(6) * (fa + T(4) * flm + fm);
    const T right = (b - m) / T(6) * (fm + T(4) * frm + fb);
    const T delta = left + right - whole;
    if (depth <= 0 || std::fabs(delta) <= T(15) * tol) {
        err += std::fabs(delta) / T(15);
        return left + right + delta / T(15);
    }
    return adaptive_simpson_rec(f, a, m, fa, flm, fm, left, tol / T(2), depth - 1, evals, err) +
           adaptive_simpson_rec(f, m, b, fm, frm, fb, right, tol / T(2), depth - 1, evals, err);
}
}  // namespace detail

/// Adaptive Simpson with absolute tolerance `tol` and recursion limit `max_depth`.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] NumericResult<T> integrate_adaptive(F&& f, T a, T b, T tol = T(1e-9), int max_depth = 40) {
    if (!(tol > T(0))) throw MathException("integrate_adaptive: tol must be > 0");
    const T fa = f(a), fb = f(b), fm = f((a + b) / T(2));
    const T whole = (b - a) / T(6) * (fa + T(4) * fm + fb);
    NumericResult<T> r;
    r.evaluations = 3;
    r.value = detail::adaptive_simpson_rec(f, a, b, fa, fm, fb, whole, tol, max_depth, r.evaluations, r.error_estimate);
    return r;
}

/// Romberg integration (Richardson extrapolation of the trapezoid rule).
template <Real T, ScalarFunction<T> F>
[[nodiscard]] NumericResult<T> integrate_romberg(F&& f, T a, T b, T tol = T(1e-12), usize max_levels = 20) {
    if (max_levels < 2) throw MathException("integrate_romberg: max_levels must be >= 2");
    std::vector<T> prev, cur;
    NumericResult<T> res;
    T h = b - a;
    prev.push_back(h * (f(a) + f(b)) / T(2));
    res.evaluations = 2;
    for (usize k = 1; k < max_levels; ++k) {
        h /= T(2);
        T s = T(0);
        const usize pts = usize{1} << (k - 1);
        for (usize i = 1; i <= pts; ++i) s += f(a + h * static_cast<T>(2 * i - 1));
        res.evaluations += pts;
        cur.assign(k + 1, T(0));
        cur[0] = prev[0] / T(2) + h * s;
        T pow4 = T(4);
        for (usize j = 1; j <= k; ++j) {
            cur[j] = cur[j - 1] + (cur[j - 1] - prev[j - 1]) / (pow4 - T(1));
            pow4 *= T(4);
        }
        res.value = cur[k];
        res.error_estimate = std::fabs(cur[k] - prev[k - 1]);
        if (res.error_estimate <= tol * std::max(T(1), std::fabs(res.value))) return res;
        prev = cur;
    }
    return res;
}

/// Gauss-Legendre quadrature with `points` nodes (1..64); exact for polynomials up to degree 2n-1.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T integrate_gauss_legendre(F&& f, T a, T b, usize points = 16) {
    if (points == 0 || points > 64) throw MathException("integrate_gauss_legendre: points must be in [1, 64]");
    const T mid = (a + b) / T(2), half = (b - a) / T(2);
    T sum = T(0);
    const usize n = points;
    for (usize i = 0; i < (n + 1) / 2; ++i) {  // roots by Newton on the Legendre polynomial
        T x = std::cos(pi_v<T> * (static_cast<T>(i) + T(0.75)) / (static_cast<T>(n) + T(0.5)));
        T dp = T(1);
        for (int it = 0; it < 100; ++it) {
            T p0 = T(1), p1 = x;
            for (usize k = 2; k <= n; ++k) {
                const T p2 = ((T(2) * static_cast<T>(k) - T(1)) * x * p1 - (static_cast<T>(k) - T(1)) * p0) / static_cast<T>(k);
                p0 = p1;
                p1 = p2;
            }
            if (n == 1) { p0 = T(1); p1 = x; }
            dp = static_cast<T>(n) * (x * p1 - p0) / (x * x - T(1));
            const T dx = p1 / dp;
            x -= dx;
            if (std::fabs(dx) < T(4) * std::numeric_limits<T>::epsilon()) break;
        }
        const T w = T(2) / ((T(1) - x * x) * dp * dp);
        sum += w * f(mid + half * x);
        if (!(n % 2 == 1 && i == (n - 1) / 2)) sum += w * f(mid - half * x);
    }
    return sum * half;
}

/// Trapezoid rule over sampled data (x strictly increasing).
template <Real T>
[[nodiscard]] T integrate_samples(std::span<const T> x, std::span<const T> y) {
    if (x.size() != y.size() || x.size() < 2) throw MathException("integrate_samples: need >= 2 matching samples");
    T s = T(0);
    for (usize i = 1; i < x.size(); ++i) {
        if (!(x[i] > x[i - 1])) throw MathException("integrate_samples: x must be strictly increasing");
        s += (x[i] - x[i - 1]) * (y[i] + y[i - 1]) / T(2);
    }
    return s;
}

// ============================================================================
// differentiation
// ============================================================================

/// Central difference with a step scaled to the cube root of machine epsilon (error ~ eps^(2/3)).
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T derivative(F&& f, T x) {
    const T h = std::cbrt(std::numeric_limits<T>::epsilon()) * std::max(T(1), std::fabs(x));
    return (f(x + h) - f(x - h)) / (T(2) * h);
}

/// Richardson-extrapolated derivative (Ridders' scheme); returns value and error estimate.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] NumericResult<T> derivative_richardson(F&& f, T x, T h0 = T(0.1), usize levels = 8) {
    if (!(h0 > T(0)) || levels < 2) throw MathException("derivative_richardson: bad parameters");
    std::vector<std::vector<T>> a(levels, std::vector<T>(levels));
    NumericResult<T> res;
    res.error_estimate = infinity_v<T>;
    T h = h0;
    a[0][0] = (f(x + h) - f(x - h)) / (T(2) * h);
    res.evaluations = 2;
    res.value = a[0][0];
    for (usize i = 1; i < levels; ++i) {
        h /= T(1.4);
        a[0][i] = (f(x + h) - f(x - h)) / (T(2) * h);
        res.evaluations += 2;
        T fac = T(1.96);
        for (usize j = 1; j <= i; ++j) {
            a[j][i] = (a[j - 1][i] * fac - a[j - 1][i - 1]) / (fac - T(1));
            fac *= T(1.96);
            const T err = std::max(std::fabs(a[j][i] - a[j - 1][i]), std::fabs(a[j][i] - a[j - 1][i - 1]));
            if (err <= res.error_estimate) {
                res.error_estimate = err;
                res.value = a[j][i];
            }
        }
        if (std::fabs(a[i][i] - a[i - 1][i - 1]) >= T(2) * res.error_estimate) break;  // error is growing
    }
    return res;
}

/// Second derivative by central difference.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T second_derivative(F&& f, T x) {
    const T h = std::pow(std::numeric_limits<T>::epsilon(), T(0.25)) * std::max(T(1), std::fabs(x));
    return (f(x + h) - T(2) * f(x) + f(x - h)) / (h * h);
}

/// Numerical gradient of a function R^n -> R.
template <Real T, class F>
    requires std::invocable<F&, std::span<const T>>
[[nodiscard]] std::vector<T> gradient(F&& f, std::span<const T> x) {
    std::vector<T> p(x.begin(), x.end()), g(x.size());
    for (usize i = 0; i < x.size(); ++i) {
        const T h = std::cbrt(std::numeric_limits<T>::epsilon()) * std::max(T(1), std::fabs(x[i]));
        p[i] = x[i] + h;
        const T fp = f(std::span<const T>(p));
        p[i] = x[i] - h;
        const T fm = f(std::span<const T>(p));
        p[i] = x[i];
        g[i] = (fp - fm) / (T(2) * h);
    }
    return g;
}

// ============================================================================
// root finding (all throw MathException when the method fails)
// ============================================================================

template <Real T, ScalarFunction<T> F>
[[nodiscard]] T find_root_bisection(F&& f, T lo, T hi, T tol = T(1e-12), usize max_iter = 200) {
    T flo = f(lo), fhi = f(hi);
    if (flo == T(0)) return lo;
    if (fhi == T(0)) return hi;
    if ((flo < T(0)) == (fhi < T(0))) throw MathException("bisection: root is not bracketed");
    for (usize i = 0; i < max_iter; ++i) {
        const T mid = lo + (hi - lo) / T(2);
        const T fm = f(mid);
        if (fm == T(0) || (hi - lo) / T(2) < tol) return mid;
        if ((fm < T(0)) == (flo < T(0))) { lo = mid; flo = fm; } else { hi = mid; }
    }
    throw MathException("bisection: did not converge");
}

/// Newton-Raphson with analytic derivative.
template <Real T, ScalarFunction<T> F, ScalarFunction<T> DF>
[[nodiscard]] T find_root_newton(F&& f, DF&& df, T x0, T tol = T(1e-12), usize max_iter = 100) {
    T x = x0;
    for (usize i = 0; i < max_iter; ++i) {
        const T fx = f(x), d = df(x);
        if (d == T(0)) throw MathException("newton: zero derivative");
        const T dx = fx / d;
        x -= dx;
        if (std::fabs(dx) <= tol * std::max(T(1), std::fabs(x))) return x;
    }
    throw MathException("newton: did not converge");
}

/// Brent's method: bracketing with superlinear convergence.
template <Real T, ScalarFunction<T> F>
[[nodiscard]] T find_root_brent(F&& f, T a, T b, T tol = T(1e-14), usize max_iter = 200) {
    T fa = f(a), fb = f(b);
    if (fa == T(0)) return a;
    if (fb == T(0)) return b;
    if ((fa < T(0)) == (fb < T(0))) throw MathException("brent: root is not bracketed");
    if (std::fabs(fa) < std::fabs(fb)) { std::swap(a, b); std::swap(fa, fb); }
    T c = a, fc = fa, d = c;
    bool mflag = true;
    for (usize i = 0; i < max_iter; ++i) {
        if (fb == T(0) || std::fabs(b - a) < tol) return b;
        T s;
        if (fa != fc && fb != fc)
            s = a * fb * fc / ((fa - fb) * (fa - fc)) + b * fa * fc / ((fb - fa) * (fb - fc)) + c * fa * fb / ((fc - fa) * (fc - fb));
        else
            s = b - fb * (b - a) / (fb - fa);
        const T lo = (T(3) * a + b) / T(4);
        const bool out_of_range = !((s > std::min(lo, b)) && (s < std::max(lo, b)));
        if (out_of_range || (mflag && std::fabs(s - b) >= std::fabs(b - c) / T(2)) ||
            (!mflag && std::fabs(s - b) >= std::fabs(c - d) / T(2)) || (mflag && std::fabs(b - c) < tol) ||
            (!mflag && std::fabs(c - d) < tol)) {
            s = (a + b) / T(2);
            mflag = true;
        } else {
            mflag = false;
        }
        const T fs = f(s);
        d = c; c = b; fc = fb;
        if ((fa < T(0)) != (fs < T(0))) { b = s; fb = fs; } else { a = s; fa = fs; }
        if (std::fabs(fa) < std::fabs(fb)) { std::swap(a, b); std::swap(fa, fb); }
    }
    throw MathException("brent: did not converge");
}

// ============================================================================
// ODE integration
// ============================================================================

/// One classic RK4 step for y' = f(t, y) with scalar state.
template <Real T, class F>
    requires std::invocable<F&, T, T>
[[nodiscard]] T rk4_step(F&& f, T t, T y, T h) {
    const T k1 = f(t, y);
    const T k2 = f(t + h / 2, y + h / 2 * k1);
    const T k3 = f(t + h / 2, y + h / 2 * k2);
    const T k4 = f(t + h, y + h * k3);
    return y + h / 6 * (k1 + T(2) * k2 + T(2) * k3 + k4);
}

/// Integrates y' = f(t, y) from t0 to t1 with `steps` RK4 steps.
template <Real T, class F>
    requires std::invocable<F&, T, T>
[[nodiscard]] T solve_ode_rk4(F&& f, T t0, T y0, T t1, usize steps) {
    if (steps == 0) throw MathException("solve_ode_rk4: steps must be > 0");
    const T h = (t1 - t0) / static_cast<T>(steps);
    T y = y0, t = t0;
    for (usize i = 0; i < steps; ++i) {
        y = rk4_step(f, t, y, h);
        t = t0 + h * static_cast<T>(i + 1);
    }
    return y;
}

// ============================================================================
// interpolation
// ============================================================================

/// Piecewise-linear interpolation over increasing x (clamped outside the range).
template <Real T>
[[nodiscard]] T interpolate_linear(std::span<const T> x, std::span<const T> y, T q) {
    if (x.size() != y.size() || x.size() < 2) throw MathException("interpolate_linear: need >= 2 matching samples");
    if (q <= x.front()) return y.front();
    if (q >= x.back()) return y.back();
    const auto it = std::upper_bound(x.begin(), x.end(), q);
    const usize i = static_cast<usize>(it - x.begin());
    return lerp(y[i - 1], y[i], (q - x[i - 1]) / (x[i] - x[i - 1]));
}

/// Natural cubic spline through (x, y) with strictly increasing x.
template <Real T>
class CubicSpline {
public:
    CubicSpline(std::span<const T> x, std::span<const T> y) : x_(x.begin(), x.end()), y_(y.begin(), y.end()) {
        const usize n = x_.size();
        if (n < 3 || y_.size() != n) throw MathException("CubicSpline: need >= 3 matching samples");
        for (usize i = 1; i < n; ++i)
            if (!(x_[i] > x_[i - 1])) throw MathException("CubicSpline: x must be strictly increasing");
        m_.assign(n, T(0));
        std::vector<T> c(n, T(0)), d(n, T(0));  // Thomas algorithm for the second derivatives
        for (usize i = 1; i + 1 < n; ++i) {
            const T h0 = x_[i] - x_[i - 1], h1 = x_[i + 1] - x_[i];
            const T diag = T(2) * (h0 + h1);
            const T rhs = T(6) * ((y_[i + 1] - y_[i]) / h1 - (y_[i] - y_[i - 1]) / h0);
            const T w = diag - h0 * c[i - 1];
            c[i] = h1 / w;
            d[i] = (rhs - h0 * d[i - 1]) / w;
        }
        for (usize i = n - 2; i > 0; --i) m_[i] = d[i] - c[i] * m_[i + 1];
    }
    /// Evaluates the spline; outside the data range the end cubic is extrapolated.
    [[nodiscard]] T operator()(T q) const {
        const usize n = x_.size();
        usize i = static_cast<usize>(std::upper_bound(x_.begin(), x_.end(), q) - x_.begin());
        i = i == 0 ? 1 : (i >= n ? n - 1 : i);
        const T h = x_[i] - x_[i - 1];
        const T a = (x_[i] - q) / h, b = (q - x_[i - 1]) / h;
        return a * y_[i - 1] + b * y_[i] + ((a * a * a - a) * m_[i - 1] + (b * b * b - b) * m_[i]) * h * h / T(6);
    }

private:
    std::vector<T> x_, y_, m_;
};

/// Polynomial evaluation by Horner's method; coefficients in ascending order.
template <Real T>
[[nodiscard]] constexpr T horner(std::span<const T> coeffs, T x) noexcept {
    T r = T(0);
    for (usize i = coeffs.size(); i-- > 0;) r = r * x + coeffs[i];
    return r;
}

}  // namespace flyg::math
