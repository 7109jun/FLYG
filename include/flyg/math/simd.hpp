// flyG - SIMD kernels with runtime CPU detection and automatic backend selection
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <span>
#include <string>

#include <flyg/core/cpu_features.hpp>
#include <flyg/core/exception.hpp>
#include <flyg/core/platform.hpp>
#include <flyg/core/types.hpp>

// Instruction-set headers. On GCC/Clang every x86 backend is compiled with a per-function
// `target` attribute, so the library needs no -mavx/-mavx2 flags: the right code path is
// picked at run time from CPUID.
#if FLYG_ARCH_X64 || FLYG_ARCH_X86
#  include <immintrin.h>
#  define FLYG_SIMD_X86 1
#else
#  define FLYG_SIMD_X86 0
#endif

#if (FLYG_ARCH_ARM64 || FLYG_ARCH_ARM32) && (defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64))
#  include <arm_neon.h>
#  define FLYG_SIMD_NEON 1
#else
#  define FLYG_SIMD_NEON 0
#endif

#if FLYG_COMPILER_MSVC
#  define FLYG_SIMD_TARGET(t)
#else
#  define FLYG_SIMD_TARGET(t) __attribute__((target(t)))
#endif

namespace flyg::math::simd {

using core::usize;

enum class Backend { scalar, sse2, avx, avx2, avx512, neon };

[[nodiscard]] constexpr const char* to_string(Backend b) noexcept {
    switch (b) {
        case Backend::scalar: return "scalar";
        case Backend::sse2: return "sse2";
        case Backend::avx: return "avx";
        case Backend::avx2: return "avx2+fma";
        case Backend::avx512: return "avx512f";
        case Backend::neon: return "neon";
    }
    return "unknown";
}

/// Function table of one backend. All kernels are element-wise (out may alias an input
/// exactly) except dot/sum, which are reductions (the summation order differs per backend,
/// so results can differ in the last bits between backends).
struct Kernels {
    Backend backend;
    void (*add_f32)(const float* a, const float* b, float* out, usize n);
    void (*sub_f32)(const float* a, const float* b, float* out, usize n);
    void (*mul_f32)(const float* a, const float* b, float* out, usize n);
    void (*scale_f32)(const float* a, float s, float* out, usize n);
    float (*dot_f32)(const float* a, const float* b, usize n);
    float (*sum_f32)(const float* a, usize n);
    void (*add_f64)(const double* a, const double* b, double* out, usize n);
    double (*dot_f64)(const double* a, const double* b, usize n);
    double (*sum_f64)(const double* a, usize n);
};

// ============================================================================
// scalar reference backend (always available)
// ============================================================================
namespace detail::scalar {
inline void add_f32(const float* a, const float* b, float* o, usize n) { for (usize i = 0; i < n; ++i) o[i] = a[i] + b[i]; }
inline void sub_f32(const float* a, const float* b, float* o, usize n) { for (usize i = 0; i < n; ++i) o[i] = a[i] - b[i]; }
inline void mul_f32(const float* a, const float* b, float* o, usize n) { for (usize i = 0; i < n; ++i) o[i] = a[i] * b[i]; }
inline void scale_f32(const float* a, float s, float* o, usize n) { for (usize i = 0; i < n; ++i) o[i] = a[i] * s; }
inline float dot_f32(const float* a, const float* b, usize n) { float s = 0; for (usize i = 0; i < n; ++i) s += a[i] * b[i]; return s; }
inline float sum_f32(const float* a, usize n) { float s = 0; for (usize i = 0; i < n; ++i) s += a[i]; return s; }
inline void add_f64(const double* a, const double* b, double* o, usize n) { for (usize i = 0; i < n; ++i) o[i] = a[i] + b[i]; }
inline double dot_f64(const double* a, const double* b, usize n) { double s = 0; for (usize i = 0; i < n; ++i) s += a[i] * b[i]; return s; }
inline double sum_f64(const double* a, usize n) { double s = 0; for (usize i = 0; i < n; ++i) s += a[i]; return s; }
}  // namespace detail::scalar

inline constexpr Kernels scalar_kernels{Backend::scalar, detail::scalar::add_f32, detail::scalar::sub_f32, detail::scalar::mul_f32,
                                        detail::scalar::scale_f32, detail::scalar::dot_f32, detail::scalar::sum_f32,
                                        detail::scalar::add_f64, detail::scalar::dot_f64, detail::scalar::sum_f64};

#if FLYG_SIMD_X86
// ============================================================================
// SSE2 (128 bit)
// ============================================================================
namespace detail::sse2 {
FLYG_SIMD_TARGET("sse2") inline float hsum(__m128 v) noexcept {
    __m128 t = _mm_add_ps(v, _mm_movehl_ps(v, v));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
    return _mm_cvtss_f32(t);
}
FLYG_SIMD_TARGET("sse2") inline double hsum(__m128d v) noexcept { return _mm_cvtsd_f64(_mm_add_sd(v, _mm_unpackhi_pd(v, v))); }

FLYG_SIMD_TARGET("sse2") inline void add_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(o + i, _mm_add_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("sse2") inline void sub_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(o + i, _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] - b[i];
}
FLYG_SIMD_TARGET("sse2") inline void mul_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(o + i, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] * b[i];
}
FLYG_SIMD_TARGET("sse2") inline void scale_f32(const float* a, float s, float* o, usize n) {
    const __m128 vs = _mm_set1_ps(s);
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(o + i, _mm_mul_ps(_mm_loadu_ps(a + i), vs));
    for (; i < n; ++i) o[i] = a[i] * s;
}
FLYG_SIMD_TARGET("sse2") inline float dot_f32(const float* a, const float* b, usize n) {
    __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps();
    usize i = 0;
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    float s = hsum(_mm_add_ps(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("sse2") inline float sum_f32(const float* a, usize n) {
    __m128 acc = _mm_setzero_ps();
    usize i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm_add_ps(acc, _mm_loadu_ps(a + i));
    float s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
FLYG_SIMD_TARGET("sse2") inline void add_f64(const double* a, const double* b, double* o, usize n) {
    usize i = 0;
    for (; i + 2 <= n; i += 2) _mm_storeu_pd(o + i, _mm_add_pd(_mm_loadu_pd(a + i), _mm_loadu_pd(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("sse2") inline double dot_f64(const double* a, const double* b, usize n) {
    __m128d acc = _mm_setzero_pd();
    usize i = 0;
    for (; i + 2 <= n; i += 2) acc = _mm_add_pd(acc, _mm_mul_pd(_mm_loadu_pd(a + i), _mm_loadu_pd(b + i)));
    double s = hsum(acc);
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("sse2") inline double sum_f64(const double* a, usize n) {
    __m128d acc = _mm_setzero_pd();
    usize i = 0;
    for (; i + 2 <= n; i += 2) acc = _mm_add_pd(acc, _mm_loadu_pd(a + i));
    double s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
}  // namespace detail::sse2

inline constexpr Kernels sse2_kernels{Backend::sse2, detail::sse2::add_f32, detail::sse2::sub_f32, detail::sse2::mul_f32,
                                      detail::sse2::scale_f32, detail::sse2::dot_f32, detail::sse2::sum_f32,
                                      detail::sse2::add_f64, detail::sse2::dot_f64, detail::sse2::sum_f64};

// ============================================================================
// AVX (256 bit, no FMA)
// ============================================================================
namespace detail::avx {
FLYG_SIMD_TARGET("avx") inline float hsum(__m256 v) noexcept {
    __m128 t = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    t = _mm_add_ps(t, _mm_movehl_ps(t, t));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
    return _mm_cvtss_f32(t);
}
FLYG_SIMD_TARGET("avx") inline double hsum(__m256d v) noexcept {
    __m128d t = _mm_add_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
    return _mm_cvtsd_f64(_mm_add_sd(t, _mm_unpackhi_pd(t, t)));
}

FLYG_SIMD_TARGET("avx") inline void add_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("avx") inline void sub_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] - b[i];
}
FLYG_SIMD_TARGET("avx") inline void mul_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] * b[i];
}
FLYG_SIMD_TARGET("avx") inline void scale_f32(const float* a, float s, float* o, usize n) {
    const __m256 vs = _mm256_set1_ps(s);
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), vs));
    for (; i < n; ++i) o[i] = a[i] * s;
}
FLYG_SIMD_TARGET("avx") inline float dot_f32(const float* a, const float* b, usize n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    usize i = 0;
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm256_add_ps(acc0, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
        acc1 = _mm256_add_ps(acc1, _mm256_mul_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8)));
    }
    float s = hsum(_mm256_add_ps(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("avx") inline float sum_f32(const float* a, usize n) {
    __m256 acc = _mm256_setzero_ps();
    usize i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm256_add_ps(acc, _mm256_loadu_ps(a + i));
    float s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
FLYG_SIMD_TARGET("avx") inline void add_f64(const double* a, const double* b, double* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm256_storeu_pd(o + i, _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("avx") inline double dot_f64(const double* a, const double* b, usize n) {
    __m256d acc = _mm256_setzero_pd();
    usize i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm256_add_pd(acc, _mm256_mul_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    double s = hsum(acc);
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("avx") inline double sum_f64(const double* a, usize n) {
    __m256d acc = _mm256_setzero_pd();
    usize i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm256_add_pd(acc, _mm256_loadu_pd(a + i));
    double s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
}  // namespace detail::avx

inline constexpr Kernels avx_kernels{Backend::avx, detail::avx::add_f32, detail::avx::sub_f32, detail::avx::mul_f32,
                                     detail::avx::scale_f32, detail::avx::dot_f32, detail::avx::sum_f32,
                                     detail::avx::add_f64, detail::avx::dot_f64, detail::avx::sum_f64};

// ============================================================================
// AVX2 + FMA (256 bit; fused multiply-add reductions)
// ============================================================================
namespace detail::avx2 {
FLYG_SIMD_TARGET("avx2,fma") inline float hsum(__m256 v) noexcept {
    __m128 t = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    t = _mm_add_ps(t, _mm_movehl_ps(t, t));
    t = _mm_add_ss(t, _mm_shuffle_ps(t, t, 1));
    return _mm_cvtss_f32(t);
}
FLYG_SIMD_TARGET("avx2,fma") inline double hsum(__m256d v) noexcept {
    __m128d t = _mm_add_pd(_mm256_castpd256_pd128(v), _mm256_extractf128_pd(v, 1));
    return _mm_cvtsd_f64(_mm_add_sd(t, _mm_unpackhi_pd(t, t)));
}

FLYG_SIMD_TARGET("avx2,fma") inline void add_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("avx2,fma") inline void sub_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] - b[i];
}
FLYG_SIMD_TARGET("avx2,fma") inline void mul_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
    for (; i < n; ++i) o[i] = a[i] * b[i];
}
FLYG_SIMD_TARGET("avx2,fma") inline void scale_f32(const float* a, float s, float* o, usize n) {
    const __m256 vs = _mm256_set1_ps(s);
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(o + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), vs));
    for (; i < n; ++i) o[i] = a[i] * s;
}
FLYG_SIMD_TARGET("avx2,fma") inline float dot_f32(const float* a, const float* b, usize n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    usize i = 0;
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
    }
    float s = hsum(_mm256_add_ps(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("avx2,fma") inline float sum_f32(const float* a, usize n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    usize i = 0;
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm256_add_ps(acc0, _mm256_loadu_ps(a + i));
        acc1 = _mm256_add_ps(acc1, _mm256_loadu_ps(a + i + 8));
    }
    float s = hsum(_mm256_add_ps(acc0, acc1));
    for (; i < n; ++i) s += a[i];
    return s;
}
FLYG_SIMD_TARGET("avx2,fma") inline void add_f64(const double* a, const double* b, double* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) _mm256_storeu_pd(o + i, _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
FLYG_SIMD_TARGET("avx2,fma") inline double dot_f64(const double* a, const double* b, usize n) {
    __m256d acc0 = _mm256_setzero_pd(), acc1 = _mm256_setzero_pd();
    usize i = 0;
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i), acc0);
        acc1 = _mm256_fmadd_pd(_mm256_loadu_pd(a + i + 4), _mm256_loadu_pd(b + i + 4), acc1);
    }
    double s = hsum(_mm256_add_pd(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
FLYG_SIMD_TARGET("avx2,fma") inline double sum_f64(const double* a, usize n) {
    __m256d acc = _mm256_setzero_pd();
    usize i = 0;
    for (; i + 4 <= n; i += 4) acc = _mm256_add_pd(acc, _mm256_loadu_pd(a + i));
    double s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
}  // namespace detail::avx2

inline constexpr Kernels avx2_kernels{Backend::avx2, detail::avx2::add_f32, detail::avx2::sub_f32, detail::avx2::mul_f32,
                                      detail::avx2::scale_f32, detail::avx2::dot_f32, detail::avx2::sum_f32,
                                      detail::avx2::add_f64, detail::avx2::dot_f64, detail::avx2::sum_f64};

// ============================================================================
// AVX-512F (512 bit; masked loads/stores handle the tail without a scalar loop)
// ============================================================================
namespace detail::avx512 {
FLYG_SIMD_TARGET("avx512f") inline __mmask16 tail_mask16(usize rem) noexcept { return static_cast<__mmask16>((1u << rem) - 1u); }
FLYG_SIMD_TARGET("avx512f") inline __mmask8 tail_mask8(usize rem) noexcept { return static_cast<__mmask8>((1u << rem) - 1u); }

FLYG_SIMD_TARGET("avx512f") inline void add_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(o + i, _mm512_add_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i)));
    if (i < n) {
        const __mmask16 m = tail_mask16(n - i);
        _mm512_mask_storeu_ps(o + i, m, _mm512_add_ps(_mm512_maskz_loadu_ps(m, a + i), _mm512_maskz_loadu_ps(m, b + i)));
    }
}
FLYG_SIMD_TARGET("avx512f") inline void sub_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(o + i, _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i)));
    if (i < n) {
        const __mmask16 m = tail_mask16(n - i);
        _mm512_mask_storeu_ps(o + i, m, _mm512_sub_ps(_mm512_maskz_loadu_ps(m, a + i), _mm512_maskz_loadu_ps(m, b + i)));
    }
}
FLYG_SIMD_TARGET("avx512f") inline void mul_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(o + i, _mm512_mul_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i)));
    if (i < n) {
        const __mmask16 m = tail_mask16(n - i);
        _mm512_mask_storeu_ps(o + i, m, _mm512_mul_ps(_mm512_maskz_loadu_ps(m, a + i), _mm512_maskz_loadu_ps(m, b + i)));
    }
}
FLYG_SIMD_TARGET("avx512f") inline void scale_f32(const float* a, float s, float* o, usize n) {
    const __m512 vs = _mm512_set1_ps(s);
    usize i = 0;
    for (; i + 16 <= n; i += 16) _mm512_storeu_ps(o + i, _mm512_mul_ps(_mm512_loadu_ps(a + i), vs));
    if (i < n) {
        const __mmask16 m = tail_mask16(n - i);
        _mm512_mask_storeu_ps(o + i, m, _mm512_mul_ps(_mm512_maskz_loadu_ps(m, a + i), vs));
    }
}
FLYG_SIMD_TARGET("avx512f") inline float dot_f32(const float* a, const float* b, usize n) {
    __m512 acc0 = _mm512_setzero_ps(), acc1 = _mm512_setzero_ps();
    usize i = 0;
    for (; i + 32 <= n; i += 32) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc0);
        acc1 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16), acc1);
    }
    for (; i + 16 <= n; i += 16) acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i), acc0);
    if (i < n) {
        const __mmask16 m = tail_mask16(n - i);
        acc0 = _mm512_fmadd_ps(_mm512_maskz_loadu_ps(m, a + i), _mm512_maskz_loadu_ps(m, b + i), acc0);
    }
    return _mm512_reduce_add_ps(_mm512_add_ps(acc0, acc1));
}
FLYG_SIMD_TARGET("avx512f") inline float sum_f32(const float* a, usize n) {
    __m512 acc = _mm512_setzero_ps();
    usize i = 0;
    for (; i + 16 <= n; i += 16) acc = _mm512_add_ps(acc, _mm512_loadu_ps(a + i));
    if (i < n) acc = _mm512_add_ps(acc, _mm512_maskz_loadu_ps(tail_mask16(n - i), a + i));
    return _mm512_reduce_add_ps(acc);
}
FLYG_SIMD_TARGET("avx512f") inline void add_f64(const double* a, const double* b, double* o, usize n) {
    usize i = 0;
    for (; i + 8 <= n; i += 8) _mm512_storeu_pd(o + i, _mm512_add_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i)));
    if (i < n) {
        const __mmask8 m = tail_mask8(n - i);
        _mm512_mask_storeu_pd(o + i, m, _mm512_add_pd(_mm512_maskz_loadu_pd(m, a + i), _mm512_maskz_loadu_pd(m, b + i)));
    }
}
FLYG_SIMD_TARGET("avx512f") inline double dot_f64(const double* a, const double* b, usize n) {
    __m512d acc = _mm512_setzero_pd();
    usize i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm512_fmadd_pd(_mm512_loadu_pd(a + i), _mm512_loadu_pd(b + i), acc);
    if (i < n) {
        const __mmask8 m = tail_mask8(n - i);
        acc = _mm512_fmadd_pd(_mm512_maskz_loadu_pd(m, a + i), _mm512_maskz_loadu_pd(m, b + i), acc);
    }
    return _mm512_reduce_add_pd(acc);
}
FLYG_SIMD_TARGET("avx512f") inline double sum_f64(const double* a, usize n) {
    __m512d acc = _mm512_setzero_pd();
    usize i = 0;
    for (; i + 8 <= n; i += 8) acc = _mm512_add_pd(acc, _mm512_loadu_pd(a + i));
    if (i < n) acc = _mm512_add_pd(acc, _mm512_maskz_loadu_pd(tail_mask8(n - i), a + i));
    return _mm512_reduce_add_pd(acc);
}
}  // namespace detail::avx512

inline constexpr Kernels avx512_kernels{Backend::avx512, detail::avx512::add_f32, detail::avx512::sub_f32, detail::avx512::mul_f32,
                                        detail::avx512::scale_f32, detail::avx512::dot_f32, detail::avx512::sum_f32,
                                        detail::avx512::add_f64, detail::avx512::dot_f64, detail::avx512::sum_f64};
#endif  // FLYG_SIMD_X86

#if FLYG_SIMD_NEON
// ============================================================================
// ARM NEON (128 bit). float64 vectors exist on AArch64 only; ARMv7 uses scalar code for f64.
// ============================================================================
namespace detail::neon {
inline float hsum(float32x4_t v) noexcept {
#  if defined(__aarch64__) || defined(_M_ARM64)
    return vaddvq_f32(v);
#  else
    const float32x2_t t = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    return vget_lane_f32(vpadd_f32(t, t), 0);
#  endif
}

inline void add_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(o + i, vaddq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
inline void sub_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(o + i, vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    for (; i < n; ++i) o[i] = a[i] - b[i];
}
inline void mul_f32(const float* a, const float* b, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(o + i, vmulq_f32(vld1q_f32(a + i), vld1q_f32(b + i)));
    for (; i < n; ++i) o[i] = a[i] * b[i];
}
inline void scale_f32(const float* a, float s, float* o, usize n) {
    usize i = 0;
    for (; i + 4 <= n; i += 4) vst1q_f32(o + i, vmulq_n_f32(vld1q_f32(a + i), s));
    for (; i < n; ++i) o[i] = a[i] * s;
}
inline float dot_f32(const float* a, const float* b, usize n) {
    float32x4_t acc0 = vdupq_n_f32(0.0f), acc1 = vdupq_n_f32(0.0f);
    usize i = 0;
    for (; i + 8 <= n; i += 8) {
        acc0 = vmlaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    float s = hsum(vaddq_f32(acc0, acc1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
inline float sum_f32(const float* a, usize n) {
    float32x4_t acc = vdupq_n_f32(0.0f);
    usize i = 0;
    for (; i + 4 <= n; i += 4) acc = vaddq_f32(acc, vld1q_f32(a + i));
    float s = hsum(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
#  if defined(__aarch64__) || defined(_M_ARM64)
inline void add_f64(const double* a, const double* b, double* o, usize n) {
    usize i = 0;
    for (; i + 2 <= n; i += 2) vst1q_f64(o + i, vaddq_f64(vld1q_f64(a + i), vld1q_f64(b + i)));
    for (; i < n; ++i) o[i] = a[i] + b[i];
}
inline double dot_f64(const double* a, const double* b, usize n) {
    float64x2_t acc = vdupq_n_f64(0.0);
    usize i = 0;
    for (; i + 2 <= n; i += 2) acc = vfmaq_f64(acc, vld1q_f64(a + i), vld1q_f64(b + i));
    double s = vaddvq_f64(acc);
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}
inline double sum_f64(const double* a, usize n) {
    float64x2_t acc = vdupq_n_f64(0.0);
    usize i = 0;
    for (; i + 2 <= n; i += 2) acc = vaddq_f64(acc, vld1q_f64(a + i));
    double s = vaddvq_f64(acc);
    for (; i < n; ++i) s += a[i];
    return s;
}
#  else
using scalar::add_f64;
using scalar::dot_f64;
using scalar::sum_f64;
#  endif
}  // namespace detail::neon

inline constexpr Kernels neon_kernels{Backend::neon, detail::neon::add_f32, detail::neon::sub_f32, detail::neon::mul_f32,
                                      detail::neon::scale_f32, detail::neon::dot_f32, detail::neon::sum_f32,
                                      detail::neon::add_f64, detail::neon::dot_f64, detail::neon::sum_f64};
#endif  // FLYG_SIMD_NEON

// ============================================================================
// backend discovery and selection
// ============================================================================

/// True when this build contains code for the backend (independent of the CPU).
[[nodiscard]] constexpr bool compiled_in(Backend b) noexcept {
    switch (b) {
        case Backend::scalar: return true;
        case Backend::sse2:
        case Backend::avx:
        case Backend::avx2:
        case Backend::avx512: return FLYG_SIMD_X86 != 0;
        case Backend::neon: return FLYG_SIMD_NEON != 0;
    }
    return false;
}

/// True when the CPU running the program (and its OS) can execute the backend.
[[nodiscard]] inline bool cpu_supports(Backend b) {
    const core::CpuFeatures& f = core::cpu_features();
    switch (b) {
        case Backend::scalar: return true;
        case Backend::sse2: return f.sse2;
        case Backend::avx: return f.avx;
        case Backend::avx2: return f.avx2 && f.fma;
        case Backend::avx512: return f.avx512f;
        case Backend::neon: return f.neon;
    }
    return false;
}

[[nodiscard]] inline bool supported(Backend b) { return compiled_in(b) && cpu_supports(b); }

/// Highest-capability backend that is both compiled in and supported by the CPU.
[[nodiscard]] inline Backend best_backend() {
    for (Backend b : {Backend::avx512, Backend::avx2, Backend::avx, Backend::sse2, Backend::neon})
        if (supported(b)) return b;
    return Backend::scalar;
}

/// Kernel table of `b`. Throws MathException(not_supported) if the backend is not compiled in or
/// the CPU cannot run it.
[[nodiscard]] inline const Kernels& kernels_for(Backend b) {
    if (!supported(b))
        throw MathException(std::string("SIMD backend not available on this build/CPU: ") + to_string(b));
    switch (b) {
        case Backend::scalar: return scalar_kernels;
#if FLYG_SIMD_X86
        case Backend::sse2: return sse2_kernels;
        case Backend::avx: return avx_kernels;
        case Backend::avx2: return avx2_kernels;
        case Backend::avx512: return avx512_kernels;
#endif
#if FLYG_SIMD_NEON
        case Backend::neon: return neon_kernels;
#endif
        default: break;
    }
    throw MathException("SIMD backend not available");
}

namespace detail {
inline std::atomic<const Kernels*>& active_slot() noexcept {
    static std::atomic<const Kernels*> slot{nullptr};
    return slot;
}
}  // namespace detail

/// The kernel table in use: the automatically selected best backend unless set_backend() was called.
[[nodiscard]] inline const Kernels& kernels() {
    const Kernels* k = detail::active_slot().load(std::memory_order_acquire);
    if (!k) {
        k = &kernels_for(best_backend());
        const Kernels* expected = nullptr;
        if (!detail::active_slot().compare_exchange_strong(expected, k, std::memory_order_acq_rel)) k = expected;
    }
    return *k;
}

/// Forces a backend (e.g. for benchmarking or tests). Throws if it is not available.
inline void set_backend(Backend b) { detail::active_slot().store(&kernels_for(b), std::memory_order_release); }
/// Returns to automatic selection.
inline void reset_backend() noexcept { detail::active_slot().store(nullptr, std::memory_order_release); }
[[nodiscard]] inline Backend active_backend() { return kernels().backend; }

/// "avx512f (available: avx512f, avx2+fma, avx, sse2, scalar)".
[[nodiscard]] inline std::string describe() {
    std::string s = std::string(to_string(active_backend())) + " (available:";
    bool first = true;
    for (Backend b : {Backend::avx512, Backend::avx2, Backend::avx, Backend::sse2, Backend::neon, Backend::scalar})
        if (supported(b)) { s += first ? " " : ", "; s += to_string(b); first = false; }
    return s + ")";
}

// ============================================================================
// public convenience API (dispatches through kernels())
// ============================================================================
namespace detail {
inline void require_same(usize a, usize b, const char* fn) {
    if (a != b) throw MathException(std::string("simd::") + fn + ": size mismatch");
}
}  // namespace detail

inline void add(std::span<const float> a, std::span<const float> b, std::span<float> out) {
    detail::require_same(a.size(), b.size(), "add"); detail::require_same(a.size(), out.size(), "add");
    kernels().add_f32(a.data(), b.data(), out.data(), a.size());
}
inline void sub(std::span<const float> a, std::span<const float> b, std::span<float> out) {
    detail::require_same(a.size(), b.size(), "sub"); detail::require_same(a.size(), out.size(), "sub");
    kernels().sub_f32(a.data(), b.data(), out.data(), a.size());
}
inline void mul(std::span<const float> a, std::span<const float> b, std::span<float> out) {
    detail::require_same(a.size(), b.size(), "mul"); detail::require_same(a.size(), out.size(), "mul");
    kernels().mul_f32(a.data(), b.data(), out.data(), a.size());
}
inline void scale(std::span<const float> a, float s, std::span<float> out) {
    detail::require_same(a.size(), out.size(), "scale");
    kernels().scale_f32(a.data(), s, out.data(), a.size());
}
[[nodiscard]] inline float dot(std::span<const float> a, std::span<const float> b) {
    detail::require_same(a.size(), b.size(), "dot");
    return kernels().dot_f32(a.data(), b.data(), a.size());
}
[[nodiscard]] inline float sum(std::span<const float> a) { return kernels().sum_f32(a.data(), a.size()); }
inline void add(std::span<const double> a, std::span<const double> b, std::span<double> out) {
    detail::require_same(a.size(), b.size(), "add"); detail::require_same(a.size(), out.size(), "add");
    kernels().add_f64(a.data(), b.data(), out.data(), a.size());
}
[[nodiscard]] inline double dot(std::span<const double> a, std::span<const double> b) {
    detail::require_same(a.size(), b.size(), "dot");
    return kernels().dot_f64(a.data(), b.data(), a.size());
}
[[nodiscard]] inline double sum(std::span<const double> a) { return kernels().sum_f64(a.data(), a.size()); }

}  // namespace flyg::math::simd
