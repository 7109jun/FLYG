// flyG - math module tests: linalg, numeric, bigint, bigfloat, simd
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/math/bigfloat.hpp>
#include <flyg/math/bigint.hpp>
#include <flyg/math/linalg.hpp>
#include <flyg/math/numeric.hpp>
#include <flyg/math/random.hpp>
#include <flyg/math/simd.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::math;

namespace {
bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

Matrix<double> random_matrix(Random<>& r, usize m, usize n) {
    Matrix<double> a(m, n);
    for (usize i = 0; i < m; ++i)
        for (usize j = 0; j < n; ++j) a(i, j) = r.uniform_real(-2.0, 2.0);
    return a;
}

BigInt random_bigint(Random<>& r, usize max_limbs) {
    const usize limbs = r.uniform_int<usize>(1, max_limbs);
    BigInt v;
    for (usize i = 0; i < limbs; ++i) {
        // mix in all-ones / zero limbs: they provoke the rare branches of Knuth division
        const u32 pick = r.next_u32() % 5;
        const u64 limb = pick == 0 ? 0xFFFFFFFFull : (pick == 1 ? 0ull : r.next_u32());
        v = v.shl(32) + BigInt(limb);
    }
    return r.bernoulli(0.5) ? -v : v;
}
}  // namespace

// ---------------------------------------------------------------- linalg ----
FLYG_TEST(math_linalg_matrix_basics) {
    const Matrix<double> a{{1, 2, 3}, {4, 5, 6}};
    CHECK_EQ(a.rows(), usize{2});
    CHECK_EQ(a.cols(), usize{3});
    CHECK_EQ(a(1, 2), 6.0);
    const Matrix<double> t = a.transposed();
    CHECK_EQ(t.rows(), usize{3});
    CHECK_EQ(t(2, 1), 6.0);
    const Matrix<double> p = a * t;  // 2x2
    CHECK_EQ(p(0, 0), 14.0);
    CHECK_EQ(p(1, 1), 77.0);
    CHECK((a * std::vector<double>{1, 0, -1}) == (std::vector<double>{-2, -2}));
    CHECK_THROWS_AS(a * a, MathException);
    CHECK_THROWS_AS(a + t, MathException);
    CHECK_THROWS_AS(a.at(2, 0), MathException);
    CHECK_THROWS_AS((a * std::vector<double>{1, 2}), MathException);
    CHECK_THROWS_AS(Matrix<double>({{1, 2}, {3}}), MathException);
    CHECK(near(Matrix<double>::identity(4).frobenius_norm(), 2.0));
    CHECK(a.col(1) == (std::vector<double>{2, 5}));
    CHECK(a.row(1) == (std::vector<double>{4, 5, 6}));
    CHECK(near(dot(std::vector<double>{1, 2, 3}, std::vector<double>{4, 5, 6}), 32.0));
    CHECK(near(norm(std::vector<double>{3, 4}), 5.0));
}

FLYG_TEST(math_linalg_lu_solve) {
    const Matrix<double> a{{2, 1, -1}, {-3, -1, 2}, {-2, 1, 2}};
    const std::vector<double> b{8, -11, -3};
    const auto x = solve(a, b);
    CHECK(near(x[0], 2.0) && near(x[1], 3.0) && near(x[2], -1.0));
    CHECK(near(determinant(a), -1.0));
    CHECK(approx_equal(a * inverse(a), Matrix<double>::identity(3), 1e-12, 1e-12));

    const LuDecomposition<double> lu(a);
    CHECK(approx_equal(lu.permutation_matrix() * a, lu.lower() * lu.upper(), 1e-12, 1e-12));  // P A = L U
    CHECK(!lu.is_singular());
    CHECK(near(residual_norm(a, x, b), 0.0, 1e-12));
    const auto refined = refine(lu, a, x, b);
    CHECK(near(residual_norm(a, refined, b), 0.0, 1e-12));

    const Matrix<double> rhs{{8, 1}, {-11, 0}, {-3, 0}};  // several right-hand sides at once
    const Matrix<double> xs = lu.solve(rhs);
    CHECK(near(xs(0, 0), 2.0) && near(xs(2, 0), -1.0));
    CHECK(approx_equal(a * xs, rhs, 1e-12, 1e-12));

    const Matrix<double> singular{{1, 2}, {2, 4}};
    CHECK(LuDecomposition<double>(singular).is_singular());
    CHECK_THROWS_AS(solve(singular, std::vector<double>{1, 2}), MathException);
    CHECK_THROWS_AS(inverse(singular), MathException);
    CHECK_EQ(determinant(singular), 0.0);
    CHECK_THROWS_AS(LuDecomposition<double>(Matrix<double>(2, 3)), MathException);
    CHECK_THROWS_AS(solve(a, std::vector<double>{1, 2}), MathException);

    // random systems: the residual must be tiny
    Random<> rng(2024);
    for (int trial = 0; trial < 20; ++trial) {
        const usize n = 3 + static_cast<usize>(trial % 6);
        const Matrix<double> m = random_matrix(rng, n, n) + Matrix<double>::identity(n) * 5.0;
        std::vector<double> rhs_v(n);
        for (double& v : rhs_v) v = rng.uniform_real(-1.0, 1.0);
        CHECK(residual_norm(m, solve(m, rhs_v), rhs_v) < 1e-10);
    }
}

FLYG_TEST(math_linalg_qr_and_least_squares) {
    Random<> rng(5);
    const Matrix<double> a = random_matrix(rng, 6, 4);
    const auto [q, r] = qr_decompose(a);
    CHECK(approx_equal(q * r, a, 1e-12, 1e-12));
    CHECK(approx_equal(q.transposed() * q, Matrix<double>::identity(6), 1e-12, 1e-12));
    bool upper = true;
    for (usize i = 1; i < r.rows(); ++i)
        for (usize j = 0; j < std::min(i, r.cols()); ++j) upper = upper && r(i, j) == 0.0;
    CHECK(upper);

    // exact line fit y = 1 + 2x
    Matrix<double> design(5, 2);
    std::vector<double> y(5);
    for (usize i = 0; i < 5; ++i) {
        design(i, 0) = 1.0;
        design(i, 1) = static_cast<double>(i);
        y[i] = 1.0 + 2.0 * static_cast<double>(i);
    }
    const auto coef = least_squares(design, y);
    CHECK(near(coef[0], 1.0, 1e-12) && near(coef[1], 2.0, 1e-12));
    y[2] += 0.5;  // noisy: the normal equations must hold, A^T (A x - y) = 0
    const auto fit = least_squares(design, y);
    std::vector<double> res = design * fit;
    for (usize i = 0; i < 5; ++i) res[i] -= y[i];
    const auto grad = design.transposed() * res;
    CHECK(near(grad[0], 0.0, 1e-12) && near(grad[1], 0.0, 1e-12));
    CHECK_THROWS_AS(least_squares(Matrix<double>(2, 3), std::vector<double>(2)), MathException);
    CHECK_THROWS_AS(least_squares(Matrix<double>{{1, 2}, {2, 4}, {3, 6}}, std::vector<double>{1, 2, 3}), MathException);  // rank deficient
}

FLYG_TEST(math_linalg_cholesky_and_eigen) {
    const Matrix<double> spd{{4, 12, -16}, {12, 37, -43}, {-16, -43, 98}};
    const Matrix<double> l = cholesky(spd);
    CHECK(approx_equal(l, Matrix<double>{{2, 0, 0}, {6, 1, 0}, {-8, 5, 3}}, 1e-12, 1e-12));
    CHECK(approx_equal(l * l.transposed(), spd, 1e-12, 1e-12));
    const std::vector<double> b{1, 2, 3};
    const auto x = cholesky_solve(l, b);
    CHECK(residual_norm(spd, x, b) < 1e-12);
    CHECK_THROWS_AS(cholesky(Matrix<double>{{1, 2}, {2, 1}}), MathException);  // indefinite
    CHECK_THROWS_AS(cholesky(Matrix<double>(2, 3)), MathException);

    const auto e2 = eigen_symmetric(Matrix<double>{{2, 1}, {1, 2}});
    CHECK(near(e2.values[0], 3.0, 1e-12) && near(e2.values[1], 1.0, 1e-12));
    Random<> rng(11);
    Matrix<double> s = random_matrix(rng, 5, 5);
    s = (s + s.transposed()) * 0.5;
    const auto e = eigen_symmetric(s);
    for (usize i = 1; i < e.values.size(); ++i) CHECK(e.values[i - 1] >= e.values[i]);
    for (usize k = 0; k < 5; ++k) {  // A v = lambda v
        const auto v = e.vectors.col(k);
        const auto av = s * v;
        for (usize i = 0; i < 5; ++i) CHECK(near(av[i], e.values[k] * v[i], 1e-10));
    }
    CHECK(approx_equal(e.vectors.transposed() * e.vectors, Matrix<double>::identity(5), 1e-12, 1e-12));
    CHECK_THROWS_AS(eigen_symmetric(Matrix<double>{{1, 2}, {0, 1}}), MathException);
    const auto diag = eigen_symmetric(Matrix<double>{{5, 0}, {0, 7}});  // already diagonal
    CHECK(near(diag.values[0], 7.0) && near(diag.values[1], 5.0));
}

FLYG_TEST(math_linalg_svd) {
    Random<> rng(17);
    const std::pair<usize, usize> shapes[] = {{5, 3}, {4, 4}, {3, 6}};
    for (const auto& [m, n] : shapes) {
        const Matrix<double> a = random_matrix(rng, m, n);
        const auto r = svd(a);
        const usize k = std::min(m, n);
        CHECK_EQ(r.s.size(), k);
        Matrix<double> sigma(k, k);
        for (usize i = 0; i < k; ++i) sigma(i, i) = r.s[i];
        CHECK(approx_equal(r.u * sigma * r.v.transposed(), a, 1e-10, 1e-10));
        CHECK(approx_equal(r.u.transposed() * r.u, Matrix<double>::identity(k), 1e-10, 1e-10));
        CHECK(approx_equal(r.v.transposed() * r.v, Matrix<double>::identity(k), 1e-10, 1e-10));
        for (usize i = 1; i < k; ++i) CHECK(r.s[i - 1] >= r.s[i] && r.s[i] >= 0.0);
        const Matrix<double> pinv = pseudo_inverse(a);
        CHECK(approx_equal(a * pinv * a, a, 1e-9, 1e-9));
        CHECK(approx_equal(pinv * a * pinv, pinv, 1e-9, 1e-9));
    }
    const Matrix<double> diag{{3, 0}, {0, -2}};
    const auto d = svd(diag);
    CHECK(near(d.s[0], 3.0) && near(d.s[1], 2.0));
    CHECK_EQ(rank(Matrix<double>{{1, 2}, {2, 4}}), usize{1});
    CHECK_EQ(rank(Matrix<double>::identity(4)), usize{4});
    CHECK_EQ(rank(Matrix<double>(3, 3)), usize{0});
    CHECK(near(condition_number(Matrix<double>{{2, 0}, {0, 0.5}}), 4.0));
    CHECK(condition_number(Matrix<double>{{1, 2}, {2, 4}}) > 1e15);
    CHECK_THROWS_AS(svd(Matrix<double>()), MathException);
    // pseudo-inverse of an invertible matrix is the inverse
    const Matrix<double> inv_me{{4, 7}, {2, 6}};
    CHECK(approx_equal(pseudo_inverse(inv_me), inverse(inv_me), 1e-10, 1e-10));
}

// --------------------------------------------------------------- numeric ----
FLYG_TEST(math_numeric_integration) {
    auto sinf = [](double x) { return std::sin(x); };
    CHECK(near(integrate_trapezoid(sinf, 0.0, pi, 10000), 2.0, 1e-7));
    CHECK(near(integrate_simpson(sinf, 0.0, pi, 100), 2.0, 1e-7));
    CHECK_THROWS_AS(integrate_simpson(sinf, 0.0, pi, 7), MathException);
    CHECK_THROWS_AS(integrate_trapezoid(sinf, 0.0, pi, 0), MathException);

    const auto ad = integrate_adaptive(sinf, 0.0, pi, 1e-10);
    CHECK(near(ad.value, 2.0, 1e-9));
    CHECK(ad.evaluations > 3);
    const auto rb = integrate_romberg(sinf, 0.0, pi);
    CHECK(near(rb.value, 2.0, 1e-11));
    CHECK(rb.error_estimate < 1e-9);

    auto gauss = [](double x) { return std::exp(-x * x); };
    CHECK(near(integrate_gauss_legendre(gauss, -6.0, 6.0, 40), std::sqrt(pi), 1e-9));
    auto poly = [](double x) { return std::pow(x, 15); };
    CHECK(near(integrate_gauss_legendre(poly, 0.0, 1.0, 8), 1.0 / 16.0, 1e-14));  // exact up to degree 2n-1
    auto cubic = [](double x) { return 4 * x * x * x - 3 * x * x + 2; };  // integral over [-1, 2] = [x^4 - x^3 + 2x] = 12
    for (const usize n : {usize{2}, usize{3}, usize{4}, usize{7}, usize{20}})  // odd and even node counts
        CHECK(near(integrate_gauss_legendre(cubic, -1.0, 2.0, n), 12.0, 1e-9));
    CHECK(std::fabs(integrate_gauss_legendre(cubic, -1.0, 2.0, 1) - 12.0) > 1.0);  // one node cannot be exact
    CHECK_THROWS_AS(integrate_gauss_legendre(gauss, 0.0, 1.0, 0), MathException);
    CHECK_THROWS_AS(integrate_adaptive(sinf, 0.0, 1.0, 0.0), MathException);

    const std::vector<double> xs{0, 1, 2, 4}, ys{0, 1, 4, 16};
    CHECK(near(integrate_samples(std::span<const double>(xs), std::span<const double>(ys)), 0.5 + 2.5 + 20.0));
    const std::vector<double> bad{0, 2, 1, 4};
    CHECK_THROWS_AS(integrate_samples(std::span<const double>(bad), std::span<const double>(ys)), MathException);
}

FLYG_TEST(math_numeric_differentiation_roots_ode) {
    auto f = [](double x) { return std::sin(x) * std::exp(x); };
    const double exact = std::exp(1.0) * (std::sin(1.0) + std::cos(1.0));
    CHECK(near(derivative(f, 1.0), exact, 1e-8));
    const auto rd = derivative_richardson(f, 1.0);
    CHECK(near(rd.value, exact, 1e-10));
    CHECK(rd.error_estimate < 1e-8);
    CHECK(near(second_derivative([](double x) { return x * x * x; }, 2.0), 12.0, 1e-4));

    auto rosen = [](std::span<const double> v) { return (1 - v[0]) * (1 - v[0]) + 100 * (v[1] - v[0] * v[0]) * (v[1] - v[0] * v[0]); };
    const std::vector<double> pt{1.0, 1.0};
    const auto g = gradient(rosen, std::span<const double>(pt));
    CHECK(near(g[0], 0.0, 1e-6) && near(g[1], 0.0, 1e-6));
    const std::vector<double> pt2{0.0, 0.0};
    const auto g2 = gradient(rosen, std::span<const double>(pt2));
    CHECK(near(g2[0], -2.0, 1e-6) && near(g2[1], 0.0, 1e-6));

    auto sq2 = [](double x) { return x * x - 2.0; };
    auto dsq2 = [](double x) { return 2.0 * x; };
    CHECK(near(find_root_bisection(sq2, 0.0, 2.0), std::sqrt(2.0), 1e-11));
    CHECK(near(find_root_newton(sq2, dsq2, 1.0), std::sqrt(2.0), 1e-12));
    CHECK(near(find_root_brent(sq2, 0.0, 2.0), std::sqrt(2.0), 1e-12));
    auto cosx_minus_x = [](double x) { return std::cos(x) - x; };
    CHECK(near(find_root_brent(cosx_minus_x, 0.0, 1.0), 0.7390851332151607, 1e-12));
    CHECK_EQ(find_root_bisection([](double x) { return x - 1.5; }, 1.5, 3.0), 1.5);  // root exactly at the bracket end
    CHECK_THROWS_AS(find_root_bisection(sq2, 2.0, 3.0), MathException);        // not bracketed
    CHECK_THROWS_AS(find_root_brent(sq2, 2.0, 3.0), MathException);
    CHECK_THROWS_AS(find_root_newton(sq2, [](double) { return 0.0; }, 1.0), MathException);

    auto grow = [](double, double y) { return y; };
    CHECK(near(solve_ode_rk4(grow, 0.0, 1.0, 1.0, 100), std::exp(1.0), 1e-8));
    auto decay = [](double t, double y) { return -2.0 * t * y; };  // y = exp(-t^2)
    CHECK(near(solve_ode_rk4(decay, 0.0, 1.0, 2.0, 200), std::exp(-4.0), 1e-8));
    CHECK_THROWS_AS(solve_ode_rk4(grow, 0.0, 1.0, 1.0, 0), MathException);

    const std::vector<double> coeffs{1, -3, 0, 2};  // 1 - 3x + 2x^3
    CHECK(near(horner(std::span<const double>(coeffs), 2.0), 11.0));
}

FLYG_TEST(math_numeric_interpolation) {
    std::vector<double> x, y;
    for (int i = 0; i <= 20; ++i) {
        x.push_back(i * 0.3);
        y.push_back(std::sin(i * 0.3));
    }
    const CubicSpline<double> spline{std::span<const double>(x), std::span<const double>(y)};
    for (int i = 0; i <= 20; ++i) CHECK(near(spline(x[static_cast<usize>(i)]), y[static_cast<usize>(i)], 1e-12));  // passes through the data
    CHECK(near(spline(1.234), std::sin(1.234), 1e-3));
    CHECK(near(spline(4.5), std::sin(4.5), 1e-3));
    CHECK_THROWS_AS((CubicSpline<double>{std::span<const double>(x).first(2), std::span<const double>(y).first(2)}), MathException);
    const std::vector<double> dup{0, 1, 1, 2};
    CHECK_THROWS_AS((CubicSpline<double>{std::span<const double>(dup), std::span<const double>(dup)}), MathException);

    const std::vector<double> lx{0, 1, 3}, ly{0, 10, 14};
    CHECK(near(interpolate_linear(std::span<const double>(lx), std::span<const double>(ly), 0.5), 5.0));
    CHECK(near(interpolate_linear(std::span<const double>(lx), std::span<const double>(ly), 2.0), 12.0));
    CHECK_EQ(interpolate_linear(std::span<const double>(lx), std::span<const double>(ly), -4.0), 0.0);  // clamped
    CHECK_EQ(interpolate_linear(std::span<const double>(lx), std::span<const double>(ly), 9.0), 14.0);
}

// ---------------------------------------------------------------- bigint ----
FLYG_TEST(math_bigint_basics) {
    BigInt f(1);
    for (int i = 2; i <= 30; ++i) f *= BigInt(i);
    CHECK_EQ(f.to_string(), "265252859812191058636308480000000");  // 30!
    CHECK_EQ(BigInt::from_string("265252859812191058636308480000000"), f);
    CHECK_EQ(BigInt::pow(BigInt(2), 200).to_string(), "1606938044258990275541962092341162602522202993782792835301376");
    CHECK_EQ(BigInt::pow10(25).to_string(), "10000000000000000000000000");
    CHECK_EQ(BigInt(0).to_string(), "0");
    CHECK_EQ(BigInt(-42).to_string(), "-42");
    CHECK_EQ(BigInt::from_string("-000123").to_string(), "-123");
    CHECK_EQ(BigInt::from_string("0xFF").to_i64(), 255);
    CHECK_EQ(BigInt::from_string("-0x10").to_i64(), -16);
    CHECK_EQ(BigInt(255).to_string(16), "0xff");
    CHECK_EQ(BigInt(-255).to_string(16), "-0xff");
    CHECK_EQ(BigInt(0).to_string(16), "0");
    CHECK_THROWS_AS(BigInt::from_string(""), DataException);
    CHECK_THROWS_AS(BigInt::from_string("12a4"), DataException);
    CHECK_THROWS_AS(BigInt::from_string("0xZZ"), DataException);
    CHECK_THROWS_AS(BigInt(1).to_string(7), MathException);

    CHECK(BigInt(5) > BigInt(3));
    CHECK(BigInt(-5) < BigInt(3));
    CHECK(BigInt(-5) < BigInt(-3));
    CHECK(BigInt(0) == -BigInt(0));
    CHECK_EQ(BigInt(7) - BigInt(7), BigInt(0));
    CHECK_EQ((BigInt(1) - BigInt(2)).to_i64(), -1);
    CHECK_EQ((BigInt(-3) * BigInt(-4)).to_i64(), 12);
    CHECK(BigInt(0).is_zero() && !BigInt(1).is_zero() && BigInt(-1).is_negative());
    CHECK_EQ(BigInt(-9).sign(), -1);
    CHECK_EQ(BigInt(1).shl(100).bit_length(), usize{101});
    CHECK_EQ(BigInt(1).shl(100).shr(100), BigInt(1));
    CHECK_EQ(BigInt(1).shl(100).trailing_zero_bits(), usize{100});
    CHECK_EQ(BigInt(-5).shr(1).to_i64(), -2);  // magnitude shift: rounds toward zero
    CHECK(BigInt(6).is_even() && !BigInt(7).is_even());
    CHECK(BigInt(1).shl(64).test_bit(64) && !BigInt(1).shl(64).test_bit(63));

    CHECK(BigInt(std::numeric_limits<long long>::max()).fits_i64());
    CHECK(BigInt(std::numeric_limits<long long>::min()).fits_i64());
    CHECK_EQ(BigInt(std::numeric_limits<long long>::min()).to_i64(), std::numeric_limits<long long>::min());
    CHECK(!(BigInt(std::numeric_limits<long long>::max()) + BigInt(1)).fits_i64());
    CHECK_THROWS_AS((BigInt(1).shl(70)).to_i64(), MathException);
    CHECK(near(BigInt(1).shl(100).to_double(), std::ldexp(1.0, 100), 1.0));
    CHECK(near(BigInt(-12345).to_double(), -12345.0));
    CHECK_EQ(BigInt(18446744073709551615ULL).to_string(), "18446744073709551615");

    CHECK_EQ(BigInt::gcd(BigInt(48), BigInt(-18)).to_i64(), 6);
    CHECK_EQ(BigInt::gcd(BigInt(0), BigInt(5)).to_i64(), 5);
    CHECK_EQ(BigInt::mod_pow(BigInt(4), BigInt(13), BigInt(497)).to_i64(), 445);
    CHECK_EQ(BigInt::mod_pow(BigInt(-3), BigInt(3), BigInt(7)).to_i64(), 1);  // (-27) mod 7 = 1
    CHECK_THROWS_AS(BigInt::mod_pow(BigInt(2), BigInt(-1), BigInt(7)), MathException);
    CHECK_THROWS_AS(BigInt::mod_pow(BigInt(2), BigInt(1), BigInt(0)), MathException);
    CHECK_EQ(BigInt::isqrt(BigInt::pow10(40)), BigInt::pow10(20));
    CHECK_EQ(BigInt::isqrt(BigInt(99)).to_i64(), 9);
    CHECK_EQ(BigInt::isqrt(BigInt(0)).to_i64(), 0);
    CHECK_THROWS_AS(BigInt::isqrt(BigInt(-1)), MathException);
    // a known 128-bit prime: 2^127 - 1 (Fermat test with several bases)
    const BigInt m127 = BigInt::pow(BigInt(2), 127) - BigInt(1);
    for (int base : {2, 3, 5, 7}) CHECK_EQ(BigInt::mod_pow(BigInt(base), m127 - BigInt(1), m127).to_i64(), 1);
}

FLYG_TEST(math_bigint_division_properties) {
    CHECK_THROWS_AS(BigInt(1) / BigInt(0), MathException);
    // truncating division semantics match built-in integers
    const std::pair<long long, long long> cases[] = {{7, 2}, {-7, 2}, {7, -2}, {-7, -2}, {0, 5}, {5, 7}};
    for (const auto& [a, b] : cases) {
        CHECK_EQ((BigInt(a) / BigInt(b)).to_i64(), a / b);
        CHECK_EQ((BigInt(a) % BigInt(b)).to_i64(), a % b);
    }
    Random<> rng(31337);
    for (int i = 0; i < 3000; ++i) {
        const BigInt a = random_bigint(rng, 12);
        BigInt b = random_bigint(rng, 8);
        if (b.is_zero()) b = BigInt(1);
        const auto [q, r] = BigInt::divmod(a, b);
        CHECK_EQ(q * b + r, a);                               // a = q*b + r
        CHECK(r.abs() < b.abs());                             // |r| < |b|
        CHECK(r.is_zero() || r.is_negative() == a.is_negative());  // sign of the dividend
        const BigInt c = random_bigint(rng, 6);
        CHECK_EQ((a + c) - c, a);
        CHECK_EQ(a * (b + c), a * b + a * c);                 // distributivity
        CHECK_EQ(BigInt::from_string(a.to_string()), a);
        CHECK_EQ(BigInt::from_string(a.to_string(16)), a);
    }
    // 128-bit cross check against the compiler's __int128
#if defined(__SIZEOF_INT128__)
    __extension__ typedef unsigned __int128 u128;  // GCC/Clang extension, used only as a test oracle
    for (int i = 0; i < 500; ++i) {
        const unsigned long long x = rng.next_u64(), y = rng.next_u64() | 1;
        const u128 prod = static_cast<u128>(x) * y;
        const BigInt p = BigInt(x) * BigInt(y);
        const BigInt hi = p.shr(64);
        const BigInt lo = p - hi.shl(64);
        CHECK_EQ(static_cast<unsigned long long>(prod >> 64), static_cast<unsigned long long>(std::stoull(hi.to_string())));
        CHECK_EQ(static_cast<unsigned long long>(prod), static_cast<unsigned long long>(std::stoull(lo.to_string())));
        CHECK_EQ((p / BigInt(y)), BigInt(x));
    }
#endif
}

// -------------------------------------------------------------- bigfloat ----
FLYG_TEST(math_bigfloat_constants_and_text) {
    const BigFloat pi256 = BigFloat::pi(256);
    CHECK_EQ(pi256.to_fixed(49), "3.1415926535897932384626433832795028841971693993751");
    CHECK(pi256.to_fixed(70).starts_with("3.14159265358979323846264338327950288419716939937510582097494459"));
    CHECK(near(pi256.to_double(), pi, 1e-15));
    CHECK_EQ(pi256.to_string(20), "3.1415926535897932385e0");
    const BigFloat e256 = BigFloat::euler_number(256);
    CHECK(e256.to_fixed(60).starts_with("2.71828182845904523536028747135266249775724709369995"));
    const BigFloat two(2, 300);
    const BigFloat root2 = BigFloat::sqrt(two);
    CHECK(root2.to_fixed(80).starts_with("1.41421356237309504880168872420969807856967187537694807317667973"));
    CHECK(BigFloat::sqrt(BigFloat(4, 100)) == BigFloat(2, 100));  // exact
    CHECK(BigFloat::sqrt(BigFloat(0, 100)).is_zero());
    CHECK_THROWS_AS(BigFloat::sqrt(BigFloat(-1, 100)), MathException);
    const BigFloat sq = root2 * root2;
    CHECK((sq - two).abs().to_double() < 1e-85);

    CHECK_EQ(BigFloat(0).to_string(), "0");
    CHECK_EQ(BigFloat(1234, 64).to_string(4), "1.234e3");
    CHECK_EQ(BigFloat(-1234, 64).to_string(2), "-1.2e3");
    CHECK_EQ(BigFloat::from_string("0.000123", 128).to_string(3), "1.23e-4");
    CHECK_EQ(BigFloat::from_string("-12.5e2", 128).to_fixed(1), "-1250.0");
    CHECK_EQ(BigFloat::from_string("0.5", 64).to_fixed(3), "0.500");
    CHECK_EQ(BigFloat::from_string("0.0004", 64).to_fixed(2), "0.00");
    CHECK_EQ(BigFloat::from_string("-0.0004", 64).to_fixed(2), "0.00");  // no negative zero
    CHECK_EQ(BigFloat(2, 64).to_fixed(0), "2");
    CHECK_THROWS_AS(BigFloat::from_string("", 64), DataException);
    CHECK_THROWS_AS(BigFloat::from_string("1.2.3", 64), DataException);
    CHECK_THROWS_AS(BigFloat::from_string("1e", 64), DataException);
    CHECK_THROWS_AS(BigFloat::from_string("abc", 64), DataException);
    CHECK_THROWS_AS(BigFloat(1, 1), MathException);  // precision too small
}

FLYG_TEST(math_bigfloat_arithmetic) {
    CHECK_EQ(BigFloat::from_double(0.1, 100).to_double(), 0.1);  // exact round trip
    CHECK_EQ(BigFloat::from_double(-1234.5678, 100).to_double(), -1234.5678);
    CHECK_EQ(BigFloat::from_double(0.0, 100).to_double(), 0.0);
    CHECK_THROWS_AS(BigFloat::from_double(std::nan(""), 100), MathException);
    CHECK_THROWS_AS(BigFloat::from_double(infinity_v<double>, 100), MathException);

    const BigFloat third = BigFloat(1, 200) / BigFloat(3, 200);
    CHECK((third * BigFloat(3, 200) - BigFloat(1, 200)).abs().to_double() < 1e-58);
    CHECK_THROWS_AS(BigFloat(1, 64) / BigFloat(0, 64), MathException);
    CHECK((BigFloat(0, 64) / BigFloat(3, 64)).is_zero());
    CHECK(BigFloat(-6, 64) / BigFloat(3, 64) == BigFloat(-2, 64));

    // decimal 0.1 + 0.2 is 0.3 to within the working precision (binary doubles are not)
    const BigFloat sum = BigFloat::from_string("0.1", 200) + BigFloat::from_string("0.2", 200);
    CHECK((sum - BigFloat::from_string("0.3", 200)).abs().to_double() < 1e-58);
    CHECK(sum.to_fixed(30) == "0.300000000000000000000000000000");

    // precision matters: 1 + 2^-200 is distinguishable only with enough bits
    const BigFloat tiny = BigFloat(1, 300) / BigFloat(BigInt(1).shl(200), 300);
    BigFloat tiny128 = tiny;
    tiny128.set_precision(128);                                // the result precision is the larger operand's
    CHECK(BigFloat(1, 128) + tiny128 == BigFloat(1, 128));   // absorbed at 128 bits
    CHECK(BigFloat(1, 128) + tiny != BigFloat(1, 128));      // kept: the sum is computed at 300 bits
    CHECK(BigFloat(1, 300) + tiny != BigFloat(1, 300));
    CHECK(BigFloat(1, 300) + tiny > BigFloat(1, 300));
    const BigFloat huge = BigFloat(BigInt(1).shl(500), 128);
    CHECK(huge + BigFloat(1, 128) == huge);                  // far-apart operands
    CHECK((huge + BigFloat(1, 128)) - huge == BigFloat(0, 128));

    CHECK(BigFloat(3, 64) > BigFloat(2, 64));
    CHECK(BigFloat(-3, 64) < BigFloat(-2, 64));
    CHECK(BigFloat(-1, 64) < BigFloat(0, 64));
    CHECK(BigFloat::from_string("1e-30", 64) < BigFloat::from_string("1e-29", 64));
    CHECK(BigFloat::from_string("1e30", 64) > BigFloat::from_string("9e29", 64));
    CHECK_EQ(BigFloat(5, 100).sign(), 1);
    CHECK((-BigFloat(5, 100)).is_negative());
    CHECK_EQ(BigFloat(7, 100).precision(), usize{100});
    CHECK_EQ((BigFloat(1, 64) + BigFloat(1, 256)).precision(), usize{256});  // larger operand precision wins

    BigFloat acc(0, 150);
    for (int i = 1; i <= 50; ++i) acc += BigFloat(1, 150) / BigFloat(i, 150);  // harmonic number H_50
    CHECK(near(acc.to_double(), 4.499205338329425, 1e-12));

    BigFloat p = BigFloat::pi(100);
    p.set_precision(30);
    CHECK_EQ(p.precision(), usize{30});
    CHECK(near(p.to_double(), pi, 1e-8));
    CHECK_EQ(BigFloat::from_double(1e300, 64).to_double(), 1e300);
    CHECK_EQ(BigFloat::from_double(1e-300, 64).to_double(), 1e-300);
}

// ------------------------------------------------------------------ simd ----
namespace {
std::vector<simd::Backend> available_backends() {
    std::vector<simd::Backend> v;
    for (simd::Backend b : {simd::Backend::scalar, simd::Backend::sse2, simd::Backend::avx, simd::Backend::avx2, simd::Backend::avx512, simd::Backend::neon})
        if (simd::supported(b)) v.push_back(b);
    return v;
}
}  // namespace

FLYG_TEST(math_simd_cpu_features_and_selection) {
    const core::CpuFeatures& f = core::cpu_features();
#if FLYG_ARCH_X64
    CHECK(f.sse2);  // baseline of x86-64
    CHECK(!f.vendor.empty());
    CHECK_EQ(f.vendor.size(), usize{12});
    CHECK(f.avx2 ? f.avx : true);          // feature implications must hold
    CHECK(f.avx512f ? f.avx2 : true);
    CHECK(f.avx ? f.sse42 : true);
#endif
#if FLYG_ARCH_ARM64
    CHECK(f.neon);
#endif
    CHECK(simd::supported(simd::Backend::scalar));
    const simd::Backend best = simd::best_backend();
    CHECK(simd::supported(best));
    CHECK_EQ(simd::active_backend(), best);  // automatic selection picks the best one
    CHECK(simd::describe().find("scalar") != std::string::npos);
    CHECK(simd::describe().find(simd::to_string(best)) != std::string::npos);
    std::printf("    [info] SIMD: %s | cpu: %s\n", simd::describe().c_str(), f.brand.empty() ? "(n/a)" : f.brand.c_str());

    for (simd::Backend b : {simd::Backend::sse2, simd::Backend::avx, simd::Backend::avx2, simd::Backend::avx512, simd::Backend::neon}) {
        if (simd::supported(b)) {
            CHECK_EQ(simd::kernels_for(b).backend, b);
        } else {
            CHECK_THROWS_AS(simd::kernels_for(b), MathException);  // must refuse, never run an illegal instruction
            CHECK_THROWS_AS(simd::set_backend(b), MathException);
        }
    }
    simd::set_backend(simd::Backend::scalar);
    CHECK_EQ(simd::active_backend(), simd::Backend::scalar);
    simd::reset_backend();
    CHECK_EQ(simd::active_backend(), best);
}

FLYG_TEST(math_simd_kernels_match_scalar_on_every_backend) {
    Random<> rng(99);
    const std::vector<usize> sizes{0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 100, 1001};
    const auto& ref = simd::scalar_kernels;
    constexpr usize pad = 8;  // sentinel area after the output: kernels must not write past n
    constexpr float sentinel_f = 12345.0f;
    constexpr double sentinel_d = 54321.0;
    for (const simd::Backend b : available_backends()) {
        const simd::Kernels& k = simd::kernels_for(b);
        for (const usize n : sizes) {
            std::vector<float> a(n), c(n);
            std::vector<double> ad(n), cd(n);
            for (usize i = 0; i < n; ++i) {
                a[i] = rng.uniform_real(-10.0f, 10.0f);
                c[i] = rng.uniform_real(-10.0f, 10.0f);
                ad[i] = rng.uniform_real(-10.0, 10.0);
                cd[i] = rng.uniform_real(-10.0, 10.0);
            }
            using BinF = void (*)(const float*, const float*, float*, usize);
            const struct { BinF got, want; const char* name; } bins[] = {
                {k.add_f32, ref.add_f32, "add"}, {k.sub_f32, ref.sub_f32, "sub"}, {k.mul_f32, ref.mul_f32, "mul"}};
            for (const auto& op : bins) {
                std::vector<float> got(n + pad, sentinel_f), want(n + pad, sentinel_f);
                op.got(a.data(), c.data(), got.data(), n);
                op.want(a.data(), c.data(), want.data(), n);
                CHECK(got == want);  // element-wise IEEE ops must match bit for bit; sentinel proves no overrun
            }
            std::vector<float> sg(n + pad, sentinel_f), sw(n + pad, sentinel_f);
            k.scale_f32(a.data(), 1.5f, sg.data(), n);
            ref.scale_f32(a.data(), 1.5f, sw.data(), n);
            CHECK(sg == sw);
            std::vector<double> dg(n + pad, sentinel_d), dw(n + pad, sentinel_d);
            k.add_f64(ad.data(), cd.data(), dg.data(), n);
            ref.add_f64(ad.data(), cd.data(), dw.data(), n);
            CHECK(dg == dw);

            std::vector<float> inplace = a;  // out aliasing an input is allowed
            k.add_f32(inplace.data(), c.data(), inplace.data(), n);
            std::vector<float> expect(n);
            ref.add_f32(a.data(), c.data(), expect.data(), n);
            CHECK(inplace == expect);

            // reductions: summation order differs, so allow a tolerance relative to the magnitude
            double mag = 1.0, magd = 1.0;
            for (usize i = 0; i < n; ++i) { mag += std::fabs(static_cast<double>(a[i]) * c[i]); magd += std::fabs(ad[i] * cd[i]); }
            const double tol_f = mag * 1e-6, tol_d = magd * 1e-14;
            CHECK(near(static_cast<double>(k.dot_f32(a.data(), c.data(), n)), static_cast<double>(ref.dot_f32(a.data(), c.data(), n)), tol_f));
            CHECK(near(static_cast<double>(k.sum_f32(a.data(), n)), static_cast<double>(ref.sum_f32(a.data(), n)), tol_f));
            CHECK(near(k.dot_f64(ad.data(), cd.data(), n), ref.dot_f64(ad.data(), cd.data(), n), tol_d));
            CHECK(near(k.sum_f64(ad.data(), n), ref.sum_f64(ad.data(), n), tol_d));
        }
    }
}

FLYG_TEST(math_simd_public_api) {
    std::vector<float> a(37), b(37), out(37);
    for (usize i = 0; i < a.size(); ++i) { a[i] = static_cast<float>(i); b[i] = 2.0f; }
    simd::add(a, b, out);
    CHECK_EQ(out[36], 38.0f);
    simd::sub(a, b, out);
    CHECK_EQ(out[0], -2.0f);
    simd::mul(a, b, out);
    CHECK_EQ(out[10], 20.0f);
    simd::scale(a, 0.5f, out);
    CHECK_EQ(out[36], 18.0f);
    CHECK_EQ(simd::dot(a, b), 2.0f * (36.0f * 37.0f / 2.0f));  // exact: all partial sums are integers
    CHECK_EQ(simd::sum(a), 666.0f);

    std::vector<double> x(21, 1.5), y(21, 2.0), z(21);
    simd::add(x, y, z);
    CHECK_EQ(z[20], 3.5);
    CHECK_EQ(simd::dot(x, y), 63.0);
    CHECK_EQ(simd::sum(x), 31.5);

    std::vector<float> shorter(3);
    CHECK_THROWS_AS(simd::add(a, b, shorter), MathException);
    CHECK_THROWS_AS(simd::dot(a, shorter), MathException);
    CHECK_EQ(simd::sum(std::span<const float>{}), 0.0f);  // empty input

    for (const simd::Backend be : available_backends()) {  // the dispatched API honours set_backend
        simd::set_backend(be);
        CHECK_EQ(simd::sum(a), 666.0f);
        CHECK_EQ(simd::dot(x, y), 63.0);
    }
    simd::reset_backend();
}
