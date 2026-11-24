// flyG example - math module: vectors, transforms, linear algebra, SIMD and big numbers
// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include <vector>

#include <flyg/flyg.hpp>

int main() {
    using namespace flyg;
    using namespace flyg::math;
    try {
        // --- vectors, quaternions, matrices ---
        const Vector3d forward = Vector3d::unit_x();
        const Quaterniond turn = Quaterniond::from_axis_angle(Vector3d::unit_z(), radians(90.0));
        const Vector3d rotated = rotate(turn, forward);
        std::printf("rotate +x by 90 deg about z -> %s\n", to_string(rotated).c_str());

        const Matrix4d model = translation(Vector3d(1, 2, 3)) * to_matrix4(turn);
        const Vector3d moved = transform_point(model, Vector3d(1, 0, 0));
        std::printf("model * (1,0,0) = %s, back = %s\n", to_string(moved).c_str(),
                    to_string(transform_point(inverse(model), moved)).c_str());

        // --- geometry ---
        const Rayd ray{Vector3d(0, 0, 10), Vector3d(0, 0, -1)};
        const Sphered ball{Vector3d(0, 0, 0), 2.0};
        if (const auto t = intersect(ray, ball)) std::printf("ray hits sphere at t = %.3f\n", *t);

        // --- linear algebra: solve A x = b and fit a line ---
        const MatrixXd a{{2, 1, -1}, {-3, -1, 2}, {-2, 1, 2}};
        const auto x = solve(a, std::vector<double>{8, -11, -3});
        std::printf("solve: x = (%.3f, %.3f, %.3f)\n", x[0], x[1], x[2]);

        // --- numeric calculus ---
        const auto area = integrate_adaptive([](double v) { return std::sin(v); }, 0.0, pi, 1e-10);
        std::printf("integral of sin over [0, pi] = %.10f (%zu evaluations)\n", area.value, area.evaluations);

        // --- SIMD with runtime backend selection ---
        std::vector<float> u(1000, 0.5f), v(1000, 2.0f);
        std::printf("simd backend: %s\n", simd::describe().c_str());
        std::printf("dot(u, v) = %.1f\n", static_cast<double>(simd::dot(u, v)));

        // --- arbitrary precision ---
        std::printf("pi (60 digits)     = %s\n", BigFloat::pi(256).to_fixed(60).c_str());
        std::printf("sqrt(2) (60 digits) = %s\n", BigFloat::sqrt(BigFloat(2, 256)).to_fixed(60).c_str());
        BigInt fact(1);
        for (int i = 2; i <= 40; ++i) fact *= BigInt(i);
        std::printf("40! = %s\n", fact.to_string().c_str());
    } catch (const flyg::Exception& e) {
        std::fprintf(stderr, "flyG error: %s\n", e.what());
        return 1;
    }
    return 0;
}
