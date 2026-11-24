// flyG - runtime CPU feature detection
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>

#include <flyg/core/platform.hpp>
#include <flyg/core/types.hpp>

#if FLYG_ARCH_X64 || FLYG_ARCH_X86
#  if FLYG_COMPILER_MSVC
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

#if FLYG_PLATFORM_LINUX && (FLYG_ARCH_ARM64 || FLYG_ARCH_ARM32)
#  include <sys/auxv.h>
#endif

namespace flyg::core {

/// Instruction-set features of the CPU the program is *running on* (OS support included:
/// AVX/AVX-512 are only reported when the OS saves the wider register state).
struct CpuFeatures {
    // x86
    bool sse = false, sse2 = false, sse3 = false, ssse3 = false, sse41 = false, sse42 = false;
    bool popcnt = false, fma = false, f16c = false;
    bool avx = false, avx2 = false;
    bool bmi1 = false, bmi2 = false;
    bool avx512f = false, avx512dq = false, avx512bw = false, avx512vl = false;
    // ARM
    bool neon = false;

    std::string vendor;  ///< e.g. "GenuineIntel" (x86 only)
    std::string brand;   ///< marketing name (x86 only)
};

#if FLYG_ARCH_X64 || FLYG_ARCH_X86
namespace detail {

struct CpuidRegs {
    u32 eax, ebx, ecx, edx;
};

inline CpuidRegs cpuid(u32 leaf, u32 sub = 0) noexcept {
#  if FLYG_COMPILER_MSVC
    int r[4];
    __cpuidex(r, static_cast<int>(leaf), static_cast<int>(sub));
    return {static_cast<u32>(r[0]), static_cast<u32>(r[1]), static_cast<u32>(r[2]),
            static_cast<u32>(r[3])};
#  else
    u32 a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(leaf, sub, a, b, c, d);
    return {a, b, c, d};
#  endif
}

inline u64 xgetbv0() noexcept {
#  if FLYG_COMPILER_MSVC
    return _xgetbv(0);
#  else
    u32 eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<u64>(edx) << 32) | eax;
#  endif
}

inline void append_reg(std::string& s, u32 r) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((r >> (8 * i)) & 0xFF));
}

}  // namespace detail
#endif

[[nodiscard]] inline CpuFeatures detect_cpu_features() {
    CpuFeatures f;
#if FLYG_ARCH_X64 || FLYG_ARCH_X86
    using namespace detail;
    const CpuidRegs l0 = cpuid(0);
    const u32 max_leaf = l0.eax;
    append_reg(f.vendor, l0.ebx);
    append_reg(f.vendor, l0.edx);
    append_reg(f.vendor, l0.ecx);

    bool os_avx = false, os_avx512 = false;
    if (max_leaf >= 1) {
        const CpuidRegs l1 = cpuid(1);
        f.sse = (l1.edx >> 25) & 1;
        f.sse2 = (l1.edx >> 26) & 1;
        f.sse3 = (l1.ecx >> 0) & 1;
        f.ssse3 = (l1.ecx >> 9) & 1;
        f.sse41 = (l1.ecx >> 19) & 1;
        f.sse42 = (l1.ecx >> 20) & 1;
        f.popcnt = (l1.ecx >> 23) & 1;
        const bool osxsave = (l1.ecx >> 27) & 1;
        const bool cpu_avx = (l1.ecx >> 28) & 1;
        if (osxsave && cpu_avx) {
            const u64 xcr0 = xgetbv0();
            os_avx = (xcr0 & 0x6) == 0x6;
            os_avx512 = os_avx && (xcr0 & 0xE0) == 0xE0;
        }
        f.avx = cpu_avx && os_avx;
        f.fma = ((l1.ecx >> 12) & 1) && os_avx;
        f.f16c = ((l1.ecx >> 29) & 1) && os_avx;
    }
    if (max_leaf >= 7) {
        const CpuidRegs l7 = cpuid(7, 0);
        f.bmi1 = (l7.ebx >> 3) & 1;
        f.bmi2 = (l7.ebx >> 8) & 1;
        f.avx2 = ((l7.ebx >> 5) & 1) && os_avx;
        f.avx512f = ((l7.ebx >> 16) & 1) && os_avx512;
        f.avx512dq = ((l7.ebx >> 17) & 1) && os_avx512;
        f.avx512bw = ((l7.ebx >> 30) & 1) && os_avx512;
        f.avx512vl = ((l7.ebx >> 31) & 1) && os_avx512;
    }
    const CpuidRegs e0 = cpuid(0x80000000u);
    if (e0.eax >= 0x80000004u) {
        for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
            const CpuidRegs r = cpuid(leaf);
            append_reg(f.brand, r.eax);
            append_reg(f.brand, r.ebx);
            append_reg(f.brand, r.ecx);
            append_reg(f.brand, r.edx);
        }
        while (!f.brand.empty() && (f.brand.back() == '\0' || f.brand.back() == ' '))
            f.brand.pop_back();
        while (!f.brand.empty() && f.brand.front() == ' ') f.brand.erase(f.brand.begin());
    }
#elif FLYG_ARCH_ARM64
    f.neon = true;  // Advanced SIMD is mandatory on AArch64
#elif FLYG_ARCH_ARM32
#  if FLYG_PLATFORM_LINUX
    f.neon = (::getauxval(AT_HWCAP) & (1UL << 12)) != 0;  // HWCAP_NEON
#  elif defined(__ARM_NEON)
    f.neon = true;
#  endif
#endif
    return f;
}

/// Detected once, cached for the lifetime of the process.
[[nodiscard]] inline const CpuFeatures& cpu_features() {
    static const CpuFeatures features = detect_cpu_features();
    return features;
}

}  // namespace flyg::core
