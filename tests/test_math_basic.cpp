// flyG - math module tests: scalar, random, stats, vectors, matrices, quaternion, geometry
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/math/geometry.hpp>
#include <flyg/math/matrix.hpp>
#include <flyg/math/matrix2.hpp>
#include <flyg/math/matrix3.hpp>
#include <flyg/math/matrix4.hpp>
#include <flyg/math/quaternion.hpp>
#include <flyg/math/random.hpp>
#include <flyg/math/scalar.hpp>
#include <flyg/math/stats.hpp>
#include <flyg/math/vector2.hpp>
#include <flyg/math/vector3.hpp>
#include <flyg/math/vector4.hpp>

#include <format>
#include <set>
#include <vector>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::math;

namespace {
bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
}  // namespace

// ---------------------------------------------------------------- scalar ----
FLYG_TEST(math_scalar_basics) {
    CHECK_EQ(clamp(5, 0, 3), 3);
    CHECK_EQ(clamp(-1.5, -1.0, 1.0), -1.0);
    CHECK_EQ(sign(-4), -1);
    CHECK_EQ(sign(0.0), 0.0);
    CHECK_EQ(square(3), 9);
    CHECK_EQ(cube(2.0), 8.0);
    CHECK_EQ(lerp(10.0, 20.0, 0.25), 12.5);
    CHECK_EQ(inverse_lerp(10.0, 20.0, 12.5), 0.25);
    CHECK_EQ(remap(5.0, 0.0, 10.0, 100.0, 200.0), 150.0);
    CHECK_THROWS_AS(inverse_lerp(1.0, 1.0, 1.0), MathException);
    CHECK(near(smoothstep(0.0, 1.0, 0.5), 0.5));
    CHECK_EQ(smoothstep(0.0, 1.0, -3.0), 0.0);
    CHECK_EQ(smoothstep(0.0, 1.0, 9.0), 1.0);
    CHECK(near(smootherstep(0.0, 1.0, 0.5), 0.5));
    CHECK_EQ(step(1.0, 0.5), 0.0);
    CHECK_EQ(step(1.0, 1.0), 1.0);
    CHECK(near(radians(180.0), pi));
    CHECK(near(degrees(half_pi), 90.0));
    CHECK(near(wrap_pi(3.0 * pi), pi) || near(wrap_pi(3.0 * pi), -pi));
    CHECK(near(wrap_pi(0.25), 0.25));
    CHECK(near(wrap_two_pi(-0.5), tau - 0.5));
    CHECK_EQ(powi(2.0, 10), 1024.0);
    CHECK_EQ(powi(2.0, -2), 0.25);
    CHECK_EQ(powi(5.0, 0), 1.0);
    CHECK(near(math::asin(1.0000000001), half_pi));  // clamped instead of NaN (::asin would return NaN)
    CHECK(near(math::acos(-1.0000000001), pi));
    CHECK_EQ(safe_sqrt(-1e-20), 0.0);
    CHECK(near(safe_sqrt(9.0), 3.0));
    const auto [s, c] = sincos(0.0);
    CHECK_EQ(s, 0.0);
    CHECK_EQ(c, 1.0);
    CHECK(near(log2(8.0), 3.0));
    CHECK(near(euler, 2.718281828459045));
    static_assert(radians(180.0) > 3.14 && radians(180.0) < 3.15);  // constexpr usable
}

FLYG_TEST(math_float_comparison_and_ulp) {
    CHECK(approx_equal(0.1 + 0.2, 0.3));
    CHECK(!approx_equal(1.0, 1.001));
    CHECK(approx_equal(0.0, 1e-20));  // absolute tolerance near zero
    CHECK(approx_equal(infinity_v<double>, infinity_v<double>));
    CHECK(!approx_equal(infinity_v<double>, 1.0));
    CHECK(!approx_equal(std::nan(""), std::nan("")));
    CHECK(is_nan(std::nan("")));
    CHECK(!is_finite(infinity_v<float>));
    CHECK_EQ(ulp_distance(1.0, 1.0), u64{0});
    CHECK_EQ(ulp_distance(1.0, next_after(1.0, 2.0)), u64{1});
    CHECK_EQ(ulp_distance(0.0, -0.0), u64{0});
    CHECK_EQ(ulp_distance(1.0f, next_after(1.0f, 2.0f)), u64{1});
    CHECK_EQ(ulp_distance(-1.0, next_after(-1.0, -2.0)), u64{1});
    CHECK(ulp_distance(-1e-300, 1e-300) > 1000000);  // across zero
    CHECK(ulp_distance(std::nan(""), 1.0) == std::numeric_limits<u64>::max());
    CHECK(near(relative_error(1.001, 1.0), 0.001, 1e-12));
    CHECK_EQ(relative_error(0.25, 0.0), 0.25);
}

// ---------------------------------------------------------------- random ----
FLYG_TEST(math_random_generators_known_values) {
    SplitMix64 sm(0);
    CHECK_EQ(sm(), u64{0xE220A8397B1DCDAFULL});  // reference value from the SplitMix64 authors
    Pcg32 pcg(42, 54);  // pcg32 demo seed/sequence
    CHECK_EQ(pcg(), u32{0xa15c02b7u});
    CHECK_EQ(pcg(), u32{0x7b47f409u});
    CHECK_EQ(pcg(), u32{0xba1d3330u});
}

FLYG_TEST(math_random_distributions) {
    Random<> a(123), b(123), c(124);
    bool same = true, differs = false;
    for (int i = 0; i < 100; ++i) {
        const u64 x = a(), y = b(), z = c();
        same = same && x == y;
        differs = differs || x != z;
    }
    CHECK(same);
    CHECK(differs);

    Random<> r(7);
    std::set<int> seen;
    for (int i = 0; i < 2000; ++i) {
        const int v = r.uniform_int(0, 5);
        CHECK(v >= 0 && v <= 5);
        seen.insert(v);
    }
    CHECK_EQ(seen.size(), usize{6});
    CHECK_EQ(r.uniform_int<int>(4, 4), 4);
    CHECK_NOTHROW(r.uniform_int<long long>(std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()));
    CHECK_THROWS_AS(r.uniform_int(5, 1), MathException);

    RunningStats<double> u, n;
    for (int i = 0; i < 100000; ++i) {
        const double x = r.uniform01<double>();
        CHECK(x >= 0.0 && x < 1.0);
        u.push(x);
        n.push(r.normal(10.0, 2.0));
    }
    CHECK(near(u.mean(), 0.5, 0.01));
    CHECK(near(u.variance(), 1.0 / 12.0, 0.005));
    CHECK(near(n.mean(), 10.0, 0.05));
    CHECK(near(n.stddev(), 2.0, 0.05));
    const float f = r.uniform01<float>();
    CHECK(f >= 0.0f && f < 1.0f);
    CHECK(near(r.uniform_real(5.0, 5.0), 5.0));
    CHECK_THROWS_AS(r.uniform_real(2.0, 1.0), MathException);
    CHECK_THROWS_AS(r.normal(0.0, -1.0), MathException);
    CHECK_THROWS_AS(r.exponential(0.0), MathException);
    CHECK_THROWS_AS(r.bernoulli(1.5), MathException);

    RunningStats<double> ex;
    for (int i = 0; i < 100000; ++i) ex.push(r.exponential(4.0));
    CHECK(near(ex.mean(), 0.25, 0.01));

    std::vector<int> v(50);
    for (int i = 0; i < 50; ++i) v[static_cast<usize>(i)] = i;
    r.shuffle(std::span<int>(v));
    std::set<int> uniq(v.begin(), v.end());
    CHECK_EQ(uniq.size(), usize{50});
    CHECK(!std::is_sorted(v.begin(), v.end()));

    Random<Pcg32> p(99);  // works with a 32-bit engine too
    CHECK(p.uniform01<double>() < 1.0);
    Random<> e = Random<>::from_entropy();
    CHECK(e.uniform01() < 1.0);
}

// ----------------------------------------------------------------- stats ----
FLYG_TEST(math_stats) {
    const std::vector<double> d{2, 4, 4, 4, 5, 5, 7, 9};
    CHECK(near(mean(d), 5.0));
    CHECK(near(variance(d, 0), 4.0));
    CHECK(near(variance(d), 32.0 / 7.0));
    CHECK(near(stddev(d, 0), 2.0));
    CHECK(near(median(d), 4.5));
    CHECK(near(percentile(d, 0), 2.0));
    CHECK(near(percentile(d, 100), 9.0));
    CHECK(near(percentile(d, 25), 4.0));
    CHECK_EQ(min_value(d), 2.0);
    CHECK_EQ(max_value(d), 9.0);
    CHECK(near(rms(std::vector<double>{3.0, 4.0}), std::sqrt(12.5)));
    CHECK(near(sum(d), 40.0));

    const std::vector<double> empty;
    CHECK_THROWS_AS(mean(empty), MathException);
    CHECK_THROWS_AS(variance(std::vector<double>{1.0}), MathException);
    CHECK_THROWS_AS(percentile(d, 101), MathException);

    // compensated summation beats naive summation
    const std::vector<double> nasty{1e16, 1.0, -1e16};
    double naive = 0;
    for (double x : nasty) naive += x;
    CHECK_EQ(naive, 0.0);
    CHECK_EQ(neumaier_sum(nasty), 1.0);
    std::vector<float> tenth(1000000, 0.1f);
    float naive_f = 0;
    for (float x : tenth) naive_f += x;
    CHECK(std::fabs(kahan_sum(tenth) - 100000.0f) < std::fabs(naive_f - 100000.0f));
    CHECK(std::fabs(klein_sum(tenth) - 100000.0f) < 0.05f);  // true sum of the float values: 100000.0015
    CHECK(near(static_cast<double>(sum(tenth)), 100000.0015, 0.02));
    CHECK(near(static_cast<double>(mean(tenth)), 0.1, 1e-7));

    const std::vector<double> x{1, 2, 3, 4, 5}, y{2, 4, 6, 8, 10}, z{5, 4, 3, 2, 1};
    CHECK(near(correlation(x, y), 1.0));
    CHECK(near(correlation(x, z), -1.0));
    CHECK(near(covariance(x, y), 5.0));
    CHECK_THROWS_AS(correlation(x, std::vector<double>(5, 1.0)), MathException);
    CHECK_THROWS_AS(covariance(x, std::vector<double>{1.0}), MathException);
    CHECK(near(skewness(std::vector<double>{1, 2, 3, 4, 5}), 0.0));
    CHECK(near(kurtosis(std::vector<double>{1, 2, 3, 4, 5}), -1.3));

    RunningStats<double> a, b, all;
    for (int i = 0; i < 50; ++i) { a.push(i * 0.5); all.push(i * 0.5); }
    for (int i = 50; i < 120; ++i) { b.push(i * 0.5); all.push(i * 0.5); }
    a.merge(b);
    CHECK_EQ(a.count(), all.count());
    CHECK(near(a.mean(), all.mean(), 1e-9));
    CHECK(near(a.variance(), all.variance(), 1e-9));
    CHECK_EQ(a.min(), 0.0);
    CHECK_EQ(a.max(), 59.5);
    CHECK_THROWS_AS(RunningStats<double>().mean(), MathException);

    Histogram<double> h(0.0, 10.0, 5);
    for (double v : {-1.0, 0.0, 1.9, 2.0, 9.99, 10.0, 11.0}) h.add(v);
    CHECK_EQ(h.counts()[0], usize{2});
    CHECK_EQ(h.counts()[1], usize{1});
    CHECK_EQ(h.counts()[4], usize{1});
    CHECK_EQ(h.underflow(), usize{1});
    CHECK_EQ(h.overflow(), usize{2});
    CHECK(near(h.bin_low(2), 4.0));
    CHECK_THROWS_AS(Histogram<double>(1.0, 1.0, 3), MathException);
}

// --------------------------------------------------------------- vectors ----
FLYG_TEST(math_vector2_vector3_vector4) {
    const Vector2d a{3, 4}, b{1, 2};
    CHECK(a + b == Vector2d(4, 6));
    CHECK(a - b == Vector2d(2, 2));
    CHECK(a * 2.0 == Vector2d(6, 8));
    CHECK(2.0 * a == Vector2d(6, 8));
    CHECK(a / 2.0 == Vector2d(1.5, 2));
    CHECK(-a == Vector2d(-3, -4));
    CHECK_EQ(dot(a, b), 11.0);
    CHECK_EQ(cross(a, b), 2.0);
    CHECK_EQ(length(a), 5.0);
    CHECK_EQ(length_squared(a), 25.0);
    CHECK(near(length(normalize(a)), 1.0));
    CHECK(near(angle(Vector2d::unit_y()), half_pi));
    CHECK(approx_equal(rotate(Vector2d::unit_x(), half_pi), Vector2d::unit_y()));
    CHECK(near(angle_between(Vector2d::unit_x(), Vector2d::unit_y()), half_pi));
    CHECK(perpendicular(Vector2d::unit_x()) == Vector2d::unit_y());
    CHECK_EQ(a[1], 4.0);
    CHECK_THROWS_AS(a[2], MathException);
    CHECK_THROWS_AS(normalize(Vector2d{}), MathException);
    CHECK(normalize_or(Vector2d{}, Vector2d::unit_x()) == Vector2d::unit_x());
    CHECK(project(Vector2d(2, 2), Vector2d(1, 0)) == Vector2d(2, 0));
    CHECK_THROWS_AS(project(a, Vector2d{}), MathException);

    const Vector3d x = Vector3d::unit_x(), y = Vector3d::unit_y();
    CHECK(cross(x, y) == Vector3d::unit_z());
    CHECK(cross(y, x) == -Vector3d::unit_z());
    CHECK_EQ(dot(Vector3d(1, 2, 3), Vector3d(4, 5, 6)), 32.0);
    CHECK(near(length(Vector3d(2, 3, 6)), 7.0));
    CHECK(near(distance(Vector3d(1, 1, 1), Vector3d(4, 5, 1)), 5.0));
    CHECK(min(Vector3d(1, 5, 3), Vector3d(2, 4, 3)) == Vector3d(1, 4, 3));
    CHECK(max(Vector3d(1, 5, 3), Vector3d(2, 4, 3)) == Vector3d(2, 5, 3));
    CHECK(clamp(Vector3d(-5, 0.5, 9), Vector3d(0.0), Vector3d(1.0)) == Vector3d(0, 0.5, 1));
    CHECK(abs(Vector3d(-1, 2, -3)) == Vector3d(1, 2, 3));
    CHECK_EQ(max_component(Vector3d(1, 9, 3)), 9.0);
    CHECK_EQ(min_component(Vector3d(1, 9, -3)), -3.0);
    CHECK(lerp(Vector3d(0.0), Vector3d(10.0), 0.3) == Vector3d(3, 3, 3));
    CHECK(reflect(Vector3d(1, -1, 0), y) == Vector3d(1, 1, 0));
    const Vector3d r = refract(Vector3d(0, -1, 0), y, 1.0);  // straight through
    CHECK(approx_equal(r, Vector3d(0, -1, 0)));
    CHECK(refract(normalize(Vector3d(1, -0.05, 0)), y, 1.5) == Vector3d{});  // total internal reflection
    CHECK(near(angle_between(x, y), half_pi));
    const Vector3d p = any_perpendicular(normalize(Vector3d(1, 2, 3)));
    CHECK(near(dot(p, normalize(Vector3d(1, 2, 3))), 0.0));
    CHECK(near(length(p), 1.0));
    CHECK(Vector3d(Vector3f(1.5f, 2.5f, 3.5f)) == Vector3d(1.5, 2.5, 3.5));
    CHECK_THROWS_AS(Vector3d{}[3], MathException);
    Vector3d acc{1, 1, 1};
    acc += Vector3d(1, 2, 3);
    acc *= 2.0;
    CHECK(acc == Vector3d(4, 6, 8));

    const Vector4d v4(Vector3d(1, 2, 3), 2.0);
    CHECK(v4.xyz() == Vector3d(1, 2, 3));
    CHECK(homogeneous_divide(v4) == Vector3d(0.5, 1, 1.5));
    CHECK_THROWS_AS(homogeneous_divide(Vector4d(1, 1, 1, 0)), MathException);
    CHECK_EQ(dot(v4, v4), 18.0);
    CHECK(near(length(Vector4d(1, 1, 1, 1)), 2.0));
    CHECK(near(length(normalize(v4)), 1.0));
    CHECK_THROWS_AS(v4[4], MathException);
    CHECK(approx_equal(lerp(Vector4d(0.0), Vector4d(4.0), 0.5), Vector4d(2.0)));

    CHECK_EQ(std::format("{}", Vector3i(1, 2, 3)), "(1, 2, 3)");
    CHECK_EQ(std::format("{}", Vector2d(1.5, 2)), "(1.5, 2)");
    static_assert(sizeof(Vector3f) == 12 && sizeof(Vector4f) == 16 && sizeof(Vector2d) == 16);
}

// -------------------------------------------------------------- matrices ----
FLYG_TEST(math_matrix_fixed_size) {
    const Matrix2d m{{1, 2}, {3, 4}};
    CHECK_EQ(m(0, 1), 2.0);
    CHECK_EQ(m(1, 0), 3.0);
    CHECK_EQ(m.m[1], 3.0);  // column-major storage: second element is (row 1, col 0)
    CHECK_EQ(determinant(m), -2.0);
    CHECK(transpose(m) == Matrix2d({{1, 3}, {2, 4}}));
    CHECK(approx_equal(m * inverse(m), Matrix2d::identity()));
    CHECK(m * Vector2d(1, 1) == Vector2d(3, 7));
    CHECK_EQ(trace(m), 5.0);
    CHECK_THROWS_AS(m.at(2, 0), MathException);
    CHECK_THROWS_AS(Matrix2d({{1, 2}}), MathException);
    CHECK_THROWS_AS(Matrix2d({{1, 2, 3}, {4, 5, 6}}), MathException);

    const Matrix3d m3{{2, 0, 1}, {1, 3, 2}, {1, 1, 4}};
    CHECK(near(determinant(m3), 18.0));  // 2*(12-2) - 0 + 1*(1-3)
    CHECK(approx_equal(inverse(m3) * m3, Matrix3d::identity()));
    CHECK(approx_equal(m3 * Matrix3d::identity(), m3));
    CHECK((m3 + m3) == m3 * 2.0);

    const Matrix4d sing{{1, 2, 3, 4}, {2, 4, 6, 8}, {1, 0, 0, 1}, {0, 1, 0, 1}};
    CHECK(near(determinant(sing), 0.0));
    CHECK_THROWS_AS(inverse(sing), MathException);
    CHECK_THROWS_AS(inverse(Matrix4d{}), MathException);

    // 4x4 determinant against a known value
    const Matrix4d d4{{1, 0, 2, -1}, {3, 0, 0, 5}, {2, 1, 4, -3}, {1, 0, 5, 0}};
    CHECK(near(determinant(d4), 30.0));
    CHECK(approx_equal(inverse(d4) * d4, Matrix4d::identity(), 1e-12, 1e-12));

    CHECK(!to_string(m).empty());
}

FLYG_TEST(math_matrix_transforms) {
    const Vector3d p(1, 2, 3);
    CHECK(transform_point(translation(Vector3d(10, 20, 30)), p) == Vector3d(11, 22, 33));
    CHECK(transform_direction(translation(Vector3d(10, 20, 30)), p) == p);  // directions ignore translation
    CHECK(transform_point(scaling(Vector3d(2, 3, 4)), p) == Vector3d(2, 6, 12));
    const Matrix3d rz = rotation3(Vector3d::unit_z(), half_pi);
    CHECK(approx_equal(rz * Vector3d::unit_x(), Vector3d::unit_y(), 1e-12, 1e-12));
    CHECK(near(determinant(rz), 1.0));
    CHECK(approx_equal(rz * transpose(rz), Matrix3d::identity(), 1e-12, 1e-12));
    CHECK_THROWS_AS(rotation3(Vector3d{}, 1.0), MathException);
    CHECK(approx_equal(rotation2(half_pi) * Vector2d(1, 0), Vector2d(0, 1), 1e-12, 1e-12));
    CHECK(translation2(Vector2d(5, 6)) * Vector3d(1, 1, 1) == Vector3d(6, 7, 1));

    // a composed TRS transform and its inverse
    const Matrix4d trs = translation(Vector3d(1, 2, 3)) * rotation(normalize(Vector3d(1, 1, 0)), 0.7) * scaling(Vector3d(2, 2, 2));
    CHECK(approx_equal(transform_point(inverse(trs), transform_point(trs, p)), p, 1e-12, 1e-12));

    const Vector3d eye(3, 4, 5), target(0, 0, 0);
    const Matrix4d view = look_at(eye, target, Vector3d::unit_y());
    CHECK(approx_equal(transform_point(view, eye), Vector3d(0.0), 1e-12, 1e-12));
    const Vector3d t = transform_point(view, target);
    CHECK(near(t.x, 0.0) && near(t.y, 0.0) && t.z < 0.0);  // target is in front of the camera (-z)
    CHECK(near(t.z, -length(eye - target)));

    const Matrix4d proj = perspective(radians(90.0), 2.0, 0.1, 100.0);
    const Vector4d near_pt = proj * Vector4d(0, 0, -0.1, 1);
    const Vector4d far_pt = proj * Vector4d(0, 0, -100.0, 1);
    CHECK(near(near_pt.z / near_pt.w, -1.0, 1e-9));
    CHECK(near(far_pt.z / far_pt.w, 1.0, 1e-9));
    const Vector4d edge = proj * Vector4d(2.0, 0, -1.0, 1);  // x = aspect * z * tan(45deg) -> NDC x = 1
    CHECK(near(edge.x / edge.w, 1.0, 1e-9));
    CHECK_THROWS_AS(perspective(0.0, 1.0, 0.1, 10.0), MathException);
    CHECK_THROWS_AS(perspective(1.0, 1.0, 5.0, 1.0), MathException);

    const Matrix4d ortho = orthographic(-2.0, 2.0, -1.0, 1.0, 0.0, 10.0);
    CHECK(approx_equal(transform_point(ortho, Vector3d(2, 1, -10)), Vector3d(1, 1, 1), 1e-12, 1e-12));
    CHECK(approx_equal(transform_point(ortho, Vector3d(-2, -1, 0)), Vector3d(-1, -1, -1), 1e-12, 1e-12));
    CHECK_THROWS_AS(orthographic(1.0, 1.0, 0.0, 1.0, 0.0, 1.0), MathException);
}

// ------------------------------------------------------------ quaternion ----
FLYG_TEST(math_quaternion) {
    const Quaterniond qz = Quaterniond::from_axis_angle(Vector3d::unit_z(), half_pi);
    CHECK(approx_equal(rotate(qz, Vector3d::unit_x()), Vector3d::unit_y(), 1e-12, 1e-12));
    CHECK(near(length(qz), 1.0));
    CHECK(approx_equal(to_matrix3(qz), rotation3(Vector3d::unit_z(), half_pi), 1e-12, 1e-12));
    CHECK(approx_equal(to_matrix4(qz), to_matrix4(rotation3(Vector3d::unit_z(), half_pi)), 1e-12, 1e-12));

    // composition: (b * a) applies a first, then b
    const Quaterniond qx = Quaterniond::from_axis_angle(Vector3d::unit_x(), half_pi);
    const Vector3d via_product = rotate(qx * qz, Vector3d::unit_x());
    const Vector3d via_steps = rotate(qx, rotate(qz, Vector3d::unit_x()));
    CHECK(approx_equal(via_product, via_steps, 1e-12, 1e-12));

    CHECK(rotation_equal(qz * inverse(qz), Quaterniond::identity()));
    CHECK(rotation_equal(qz.conjugate(), inverse(qz)));
    CHECK_THROWS_AS(inverse(Quaterniond(0, 0, 0, 0)), MathException);
    CHECK_THROWS_AS(normalized(Quaterniond(0, 0, 0, 0)), MathException);

    // matrix round trips for rotations covering every branch of from_matrix
    for (const Vector3d axis : {Vector3d(1, 0, 0), Vector3d(0, 1, 0), Vector3d(0, 0, 1), Vector3d(1, 2, 3), Vector3d(-2, 1, -1)})
        for (const double ang : {0.0, 0.3, 1.5, 3.0, 3.14159, 4.5, 6.0}) {
            const Quaterniond q = Quaterniond::from_axis_angle(axis, ang);
            const Quaterniond back = Quaterniond::from_matrix(to_matrix3(q));
            CHECK(rotation_equal(q, back, 1e-9));
        }

    const auto [axis, angle] = to_axis_angle(Quaterniond::from_axis_angle(Vector3d(0, 3, 0), 1.2));
    CHECK(approx_equal(axis, Vector3d::unit_y(), 1e-12, 1e-12));
    CHECK(near(angle, 1.2));
    CHECK(to_axis_angle(Quaterniond::identity()).second == 0.0);

    const Quaterniond e = Quaterniond::from_euler(0.3, -0.4, 1.1);
    const Vector3d eul = to_euler(e);
    CHECK(near(eul.x, 0.3, 1e-12) && near(eul.y, -0.4, 1e-12) && near(eul.z, 1.1, 1e-12));
    CHECK(near(to_euler(Quaterniond::from_euler(0, half_pi, 0)).y, half_pi, 1e-6));  // gimbal-lock input stays finite

    const Quaterniond half = slerp(Quaterniond::identity(), qz, 0.5);
    CHECK(rotation_equal(half, Quaterniond::from_axis_angle(Vector3d::unit_z(), half_pi / 2)));
    CHECK(rotation_equal(slerp(Quaterniond::identity(), qz, 0.0), Quaterniond::identity()));
    CHECK(rotation_equal(slerp(Quaterniond::identity(), qz, 1.0), qz));
    CHECK(rotation_equal(slerp(qz, -qz, 0.3), qz));  // shortest arc across the double cover
    CHECK(near(length(nlerp(Quaterniond::identity(), qz, 0.3)), 1.0));

    const Vector3d from(1, 0, 0), to(0, 1, 1);
    CHECK(approx_equal(rotate(Quaterniond::from_to(from, to), from), normalize(to), 1e-12, 1e-12));
    const Quaterniond flip = Quaterniond::from_to(Vector3d::unit_x(), -Vector3d::unit_x());
    CHECK(approx_equal(rotate(flip, Vector3d::unit_x()), -Vector3d::unit_x(), 1e-12, 1e-12));
    CHECK(!to_string(qz).empty());
}

// -------------------------------------------------------------- geometry ----
FLYG_TEST(math_geometry_primitives_and_distances) {
    const Planed plane = Planed::from_point_normal(Vector3d(0, 5, 0), Vector3d(0, 2, 0));
    CHECK(near(plane.signed_distance(Vector3d(1, 8, 2)), 3.0));
    CHECK(near(plane.signed_distance(Vector3d(1, 1, 2)), -4.0));
    CHECK(approx_equal(plane.closest_point(Vector3d(1, 8, 2)), Vector3d(1, 5, 2)));
    const Planed tri_plane = Planed::from_points(Vector3d(0, 0, 0), Vector3d(1, 0, 0), Vector3d(0, 1, 0));
    CHECK(approx_equal(tri_plane.normal, Vector3d::unit_z()));
    CHECK_THROWS_AS(Planed::from_points(Vector3d(0, 0, 0), Vector3d(1, 0, 0), Vector3d(2, 0, 0)), MathException);

    const Lined line = Lined::from_points(Vector3d(0, 0, 0), Vector3d(10, 0, 0));
    CHECK(near(line.distance_to(Vector3d(3, 4, 0)), 4.0));
    CHECK(approx_equal(line.closest_point(Vector3d(3, 4, 0)), Vector3d(3, 0, 0)));

    const Segmentd seg{Vector3d(0, 0, 0), Vector3d(10, 0, 0)};
    CHECK(near(seg.length(), 10.0));
    CHECK(approx_equal(seg.closest_point(Vector3d(15, 3, 0)), Vector3d(10, 0, 0)));  // clamped to the end
    CHECK(near(seg.distance_to(Vector3d(5, -2, 0)), 2.0));
    CHECK(approx_equal(Segmentd{Vector3d(1, 1, 1), Vector3d(1, 1, 1)}.closest_point(Vector3d(9, 9, 9)), Vector3d(1, 1, 1)));  // degenerate

    const Lined l1{Vector3d(0, 0, 0), Vector3d::unit_x()}, l2{Vector3d(0, 3, 5), Vector3d::unit_z()};
    CHECK(near(distance(l1, l2), 3.0));  // skew lines
    const auto [c1, c2] = closest_points(l1, l2);
    CHECK(approx_equal(c1, Vector3d(0, 0, 0)) && approx_equal(c2, Vector3d(0, 3, 0)));
    CHECK(near(distance(Lined{Vector3d(0, 0, 0), Vector3d::unit_x()}, Lined{Vector3d(0, 2, 0), Vector3d::unit_x()}), 2.0));  // parallel

    const Segmentd s1{Vector3d(0, 0, 0), Vector3d(1, 0, 0)}, s2{Vector3d(3, 4, 0), Vector3d(3, 8, 0)};
    CHECK(near(distance(s1, s2), std::sqrt(4.0 + 16.0)));
    const Segmentd s3{Vector3d(0.5, -1, 0), Vector3d(0.5, 1, 0)};
    CHECK(near(distance(s1, s3), 0.0));

    Boxd box = Boxd::from_center_extents(Vector3d(0, 0, 0), Vector3d(1, 2, 3));
    CHECK(box.valid());
    CHECK(box.contains(Vector3d(0.5, -1.5, 2.9)));
    CHECK(!box.contains(Vector3d(1.1, 0, 0)));
    CHECK(near(box.volume(), 2 * 4 * 6));
    CHECK(box.size() == Vector3d(2, 4, 6));
    box.expand(Vector3d(5, 0, 0));
    CHECK(box.max.x == 5.0);
    const std::vector<Vector3d> pts{{1, 2, 3}, {-1, 5, 0}, {4, -2, 2}};
    const Boxd fit = Boxd::from_points(pts);
    CHECK(fit.min == Vector3d(-1, -2, 0) && fit.max == Vector3d(4, 5, 3));
    CHECK_THROWS_AS(Boxd::from_points({}), MathException);
    CHECK(fit.contains(Boxd{Vector3d(0, 0, 1), Vector3d(1, 1, 2)}));
    CHECK(approx_equal(fit.closest_point(Vector3d(10, 0, 1)), Vector3d(4, 0, 1)));

    const Spheref s{Vector3f(0, 0, 0), 2.0f};
    CHECK(s.contains(Vector3f(1, 1, 1)));
    CHECK(!s.contains(Vector3f(2, 2, 0)));
    CHECK(near(static_cast<double>(s.volume()), 4.0 / 3.0 * pi * 8.0, 1e-4));
    CHECK(near(static_cast<double>(s.surface_area()), 16.0 * pi, 1e-4));
    const Trianglef tri{Vector3f(0, 0, 0), Vector3f(2, 0, 0), Vector3f(0, 2, 0)};
    CHECK(near(static_cast<double>(tri.area()), 2.0, 1e-6));
    CHECK(approx_equal(tri.normal(), Vector3f::unit_z()));
}

FLYG_TEST(math_geometry_raycasts) {
    const Rayd ray{Vector3d(0, 0, 10), Vector3d(0, 0, -1)};
    const Sphered sphere{Vector3d(0, 0, 0), 2.0};
    CHECK(near(*intersect(ray, sphere), 8.0));
    CHECK(!intersect(Rayd{Vector3d(0, 0, 10), Vector3d(0, 0, 1)}, sphere).has_value());     // pointing away
    CHECK(!intersect(Rayd{Vector3d(5, 0, 10), Vector3d(0, 0, -1)}, sphere).has_value());    // misses
    CHECK_EQ(*intersect(Rayd{Vector3d(0, 0, 0), Vector3d(1, 0, 0)}, sphere), 0.0);          // origin inside
    CHECK(near(*intersect(Rayd{Vector3d(2, 0, 10), Vector3d(0, 0, -1)}, sphere), 10.0, 1e-6));  // tangent

    const Planed floor = Planed::from_point_normal(Vector3d(0, 0, 0), Vector3d(0, 0, 1));
    CHECK(near(*intersect(ray, floor), 10.0));
    CHECK(!intersect(Rayd{Vector3d(0, 0, 10), Vector3d(1, 0, 0)}, floor).has_value());       // parallel
    CHECK(!intersect(Rayd{Vector3d(0, 0, 10), Vector3d(0, 0, 1)}, floor).has_value());       // behind

    const Boxd box{Vector3d(-1, -1, -1), Vector3d(1, 1, 1)};
    CHECK(near(*intersect(ray, box), 9.0));
    CHECK(!intersect(Rayd{Vector3d(3, 0, 10), Vector3d(0, 0, -1)}, box).has_value());
    CHECK_EQ(*intersect(Rayd{Vector3d(0, 0, 0), Vector3d(0, 1, 0)}, box), 0.0);               // inside
    CHECK(near(*intersect(Rayd::from_points(Vector3d(-5, -5, -5), Vector3d(0, 0, 0)), box), std::sqrt(75.0) - std::sqrt(3.0), 1e-9));  // diagonal

    const Triangled tri{Vector3d(-1, -1, 0), Vector3d(1, -1, 0), Vector3d(0, 1, 0)};
    const auto hit = intersect(ray, tri);
    CHECK(hit.has_value());
    CHECK(near(hit->t, 10.0));
    const Vector3d bary_point = tri.a * (1 - hit->u - hit->v) + tri.b * hit->u + tri.c * hit->v;
    CHECK(approx_equal(bary_point, ray.at(hit->t), 1e-9, 1e-9));
    CHECK(!intersect(Rayd{Vector3d(5, 5, 10), Vector3d(0, 0, -1)}, tri).has_value());

    const Segmentd crossing{Vector3d(0, 0, -1), Vector3d(0, 0, 3)};
    CHECK(approx_equal(*intersect(crossing, floor), Vector3d(0, 0, 0)));
    CHECK(!intersect(Segmentd{Vector3d(0, 0, 1), Vector3d(0, 0, 3)}, floor).has_value());
}

FLYG_TEST(math_geometry_overlap_and_collision) {
    const Sphered a{Vector3d(0, 0, 0), 1.0}, b{Vector3d(1.5, 0, 0), 1.0}, far_s{Vector3d(5, 0, 0), 1.0};
    CHECK(overlaps(a, b));
    CHECK(!overlaps(a, far_s));
    const auto c = collide(a, b);
    CHECK(c.has_value());
    CHECK(near(c->depth, 0.5));
    CHECK(approx_equal(c->normal, Vector3d::unit_x()));
    CHECK(approx_equal(c->point, Vector3d(0.75, 0, 0)));
    CHECK(!collide(a, far_s).has_value());
    CHECK(collide(a, a).has_value());  // identical centres: arbitrary but finite normal
    CHECK(near(length(collide(a, a)->normal), 1.0));

    const Boxd b1{Vector3d(0, 0, 0), Vector3d(2, 2, 2)}, b2{Vector3d(1.5, 0.5, 0.5), Vector3d(3.5, 1.5, 1.5)}, b3{Vector3d(5, 5, 5), Vector3d(6, 6, 6)};
    CHECK(overlaps(b1, b2));
    CHECK(!overlaps(b1, b3));
    const auto bc = collide(b1, b2);
    CHECK(bc.has_value());
    CHECK(near(bc->depth, 0.5));
    CHECK(approx_equal(bc->normal, Vector3d::unit_x()));
    CHECK(!collide(b1, b3).has_value());

    const Sphered ball{Vector3d(3, 1, 1), 1.5};
    CHECK(overlaps(ball, b1));
    const auto sb = collide(ball, b1);
    CHECK(sb.has_value());
    CHECK(near(sb->depth, 0.5));
    CHECK(approx_equal(sb->normal, -Vector3d::unit_x()));  // from the sphere towards the box
    CHECK(!overlaps(Sphered{Vector3d(10, 10, 10), 1.0}, b1));
    const auto inside = collide(Sphered{Vector3d(1.9, 1, 1), 0.2}, b1);  // centre inside, nearest face is +x
    CHECK(inside.has_value());
    CHECK(approx_equal(inside->normal, Vector3d::unit_x()));
    CHECK(near(inside->depth, 0.2 + 0.1));

    const Planed floor = Planed::from_point_normal(Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    CHECK(overlaps(Sphered{Vector3d(0, 0.5, 0), 1.0}, floor));
    CHECK(!overlaps(Sphered{Vector3d(0, 3, 0), 1.0}, floor));
    const auto pc = collide(Sphered{Vector3d(0, 0.4, 0), 1.0}, floor);
    CHECK(pc.has_value());
    CHECK(near(pc->depth, 0.6));
    CHECK(approx_equal(pc->normal, Vector3d(0, -1, 0)));
    CHECK(overlaps(Boxd{Vector3d(-1, -1, -1), Vector3d(1, 1, 1)}, floor));
    CHECK(!overlaps(Boxd{Vector3d(-1, 2, -1), Vector3d(1, 3, 1)}, floor));
}
