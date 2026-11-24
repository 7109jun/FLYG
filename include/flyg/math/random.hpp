// flyG - pseudo random number generators and distributions
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <bit>
#include <cmath>
#include <limits>
#include <random>
#include <span>
#include <utility>

#include <flyg/math/scalar.hpp>

namespace flyg::math {

/// SplitMix64 (Vigna). Used to expand seeds; also a fine small generator.
class SplitMix64 {
public:
    using result_type = u64;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<u64>::max(); }

    constexpr explicit SplitMix64(u64 seed = 0) noexcept : s_(seed) {}
    constexpr u64 operator()() noexcept {
        u64 z = (s_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    u64 s_;
};

/// PCG-XSH-RR 32-bit output (O'Neill). Stream selected by `sequence`.
class Pcg32 {
public:
    using result_type = u32;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<u32>::max(); }

    constexpr Pcg32(u64 seed = 0x853c49e6748fea9bULL, u64 sequence = 0xda3e39cb94b95bdbULL) noexcept {
        state_ = 0;
        inc_ = (sequence << 1u) | 1u;
        (void)(*this)();
        state_ += seed;
        (void)(*this)();
    }
    constexpr u32 operator()() noexcept {
        const u64 old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

private:
    u64 state_ = 0;
    u64 inc_ = 1;
};

/// xoshiro256** (Blackman & Vigna): fast, 256-bit state, excellent quality.
class Xoshiro256ss {
public:
    using result_type = u64;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<u64>::max(); }

    constexpr explicit Xoshiro256ss(u64 seed = 0x2545F4914F6CDD1DULL) noexcept {
        SplitMix64 sm(seed);
        for (u64& w : s_) w = sm();
    }
    constexpr u64 operator()() noexcept {
        const u64 result = std::rotl(s_[1] * 5, 7) * 9;
        const u64 t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = std::rotl(s_[3], 45);
        return result;
    }

private:
    u64 s_[4]{};
};

/// Distributions on top of any engine. Satisfies UniformRandomBitGenerator (64-bit output),
/// so it can also be handed to <random> distributions and std::shuffle.
template <class Engine = Xoshiro256ss>
class Random {
public:
    using result_type = u64;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<u64>::max(); }

    Random() = default;
    explicit Random(u64 seed) : engine_(seed) {}

    /// Seeds from std::random_device (non-deterministic).
    [[nodiscard]] static Random from_entropy() {
        std::random_device rd;
        const u64 seed = (static_cast<u64>(rd()) << 32) ^ static_cast<u64>(rd());
        return Random(seed);
    }

    u64 operator()() { return next_u64(); }

    u64 next_u64() {
        if constexpr (sizeof(typename Engine::result_type) == 8) {
            return static_cast<u64>(engine_());
        } else {
            const u64 hi = engine_();
            const u64 lo = engine_();
            return (hi << 32) | lo;
        }
    }
    u32 next_u32() { return static_cast<u32>(next_u64() >> 32); }

    /// Uniform in [0, 1).
    template <Real T = double>
    T uniform01() {
        if constexpr (std::is_same_v<T, float>)
            return static_cast<float>(next_u64() >> 40) * (1.0f / 16777216.0f);  // 24 bits
        else
            return static_cast<T>(static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0));  // 53 bits
    }

    /// Uniform in [lo, hi).
    template <Real T>
    T uniform_real(T lo, T hi) {
        if (!(lo <= hi)) throw MathException("uniform_real: lo must be <= hi");
        return lo + (hi - lo) * uniform01<T>();
    }

    /// Unbiased uniform integer in [lo, hi] (inclusive).
    template <std::integral I>
    I uniform_int(I lo, I hi) {
        if (lo > hi) throw MathException("uniform_int: lo must be <= hi");
        const u64 range = static_cast<u64>(hi) - static_cast<u64>(lo) + 1;  // wraps to 0 for the full 64-bit range
        if (range == 0) return static_cast<I>(next_u64());
        const u64 threshold = (0 - range) % range;  // rejection zone => no modulo bias
        u64 r;
        do {
            r = next_u64();
        } while (r < threshold);
        return static_cast<I>(static_cast<u64>(lo) + r % range);
    }

    bool bernoulli(double p) {
        if (!(p >= 0.0 && p <= 1.0)) throw MathException("bernoulli: p must be in [0, 1]");
        return uniform01<double>() < p;
    }

    /// Normal distribution (Box-Muller with cached second value).
    template <Real T = double>
    T normal(T mean = T(0), T stddev = T(1)) {
        if (stddev < T(0)) throw MathException("normal: stddev must be >= 0");
        if (has_spare_) {
            has_spare_ = false;
            return mean + stddev * static_cast<T>(spare_);
        }
        double u1;
        do {
            u1 = uniform01<double>();
        } while (u1 <= 0.0);
        const double u2 = uniform01<double>();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double ang = 2.0 * std::numbers::pi * u2;
        spare_ = r * std::sin(ang);
        has_spare_ = true;
        return mean + stddev * static_cast<T>(r * std::cos(ang));
    }

    /// Exponential distribution with rate `lambda` > 0.
    template <Real T = double>
    T exponential(T lambda) {
        if (!(lambda > T(0))) throw MathException("exponential: lambda must be > 0");
        double u;
        do {
            u = uniform01<double>();
        } while (u <= 0.0);
        return static_cast<T>(-std::log(u) / static_cast<double>(lambda));
    }

    /// Fisher-Yates shuffle.
    template <class T>
    void shuffle(std::span<T> items) {
        for (usize i = items.size(); i > 1; --i) {
            const usize j = uniform_int<usize>(0, i - 1);
            std::swap(items[i - 1], items[j]);
        }
    }

    Engine& engine() noexcept { return engine_; }

private:
    Engine engine_{};
    double spare_ = 0.0;
    bool has_spare_ = false;
};

}  // namespace flyg::math
