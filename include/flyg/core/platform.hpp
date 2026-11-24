// flyG - platform detection
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <bit>
#include <string_view>

// ---- language level -------------------------------------------------------
#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 202004L
#    error "flyG requires C++23 (use /std:c++latest)"
#  endif
#elif __cplusplus < 202100L
#  error "flyG requires C++23 (use -std=c++23)"
#endif

// ---- operating system -----------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
#  define FLYG_PLATFORM_WINDOWS 1
#  define FLYG_PLATFORM_LINUX 0
#elif defined(__linux__)
#  define FLYG_PLATFORM_WINDOWS 0
#  define FLYG_PLATFORM_LINUX 1
#else
#  error "flyG supports only Windows and Linux"
#endif

// ---- compiler -------------------------------------------------------------
#if defined(__clang__)
#  define FLYG_COMPILER_CLANG 1
#  define FLYG_COMPILER_GCC 0
#  define FLYG_COMPILER_MSVC 0
#elif defined(__GNUC__)
#  define FLYG_COMPILER_CLANG 0
#  define FLYG_COMPILER_GCC 1
#  define FLYG_COMPILER_MSVC 0
#elif defined(_MSC_VER)
#  define FLYG_COMPILER_CLANG 0
#  define FLYG_COMPILER_GCC 0
#  define FLYG_COMPILER_MSVC 1
#else
#  error "Unsupported compiler"
#endif

// ---- architecture ---------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#  define FLYG_ARCH_X64 1
#  define FLYG_ARCH_X86 0
#  define FLYG_ARCH_ARM64 0
#  define FLYG_ARCH_ARM32 0
#elif defined(__i386__) || defined(_M_IX86)
#  define FLYG_ARCH_X64 0
#  define FLYG_ARCH_X86 1
#  define FLYG_ARCH_ARM64 0
#  define FLYG_ARCH_ARM32 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define FLYG_ARCH_X64 0
#  define FLYG_ARCH_X86 0
#  define FLYG_ARCH_ARM64 1
#  define FLYG_ARCH_ARM32 0
#elif defined(__arm__) || defined(_M_ARM)
#  define FLYG_ARCH_X64 0
#  define FLYG_ARCH_X86 0
#  define FLYG_ARCH_ARM64 0
#  define FLYG_ARCH_ARM32 1
#else
#  error "Unsupported CPU architecture"
#endif

// ---- attributes -----------------------------------------------------------
#if FLYG_COMPILER_MSVC
#  define FLYG_FORCE_INLINE __forceinline
#  define FLYG_NOINLINE __declspec(noinline)
#else
#  define FLYG_FORCE_INLINE inline __attribute__((always_inline))
#  define FLYG_NOINLINE __attribute__((noinline))
#endif

#if defined(__has_cpp_attribute) && __has_cpp_attribute(likely) >= 201803L
#  define FLYG_LIKELY [[likely]]
#  define FLYG_UNLIKELY [[unlikely]]
#else
#  define FLYG_LIKELY
#  define FLYG_UNLIKELY
#endif

namespace flyg::core {

enum class OperatingSystem { windows, linux_os };
enum class Architecture { x86, x64, arm32, arm64 };
enum class Compiler { gcc, clang, msvc };

inline constexpr OperatingSystem os =
#if FLYG_PLATFORM_WINDOWS
    OperatingSystem::windows;
#else
    OperatingSystem::linux_os;
#endif

inline constexpr Architecture arch =
#if FLYG_ARCH_X64
    Architecture::x64;
#elif FLYG_ARCH_X86
    Architecture::x86;
#elif FLYG_ARCH_ARM64
    Architecture::arm64;
#else
    Architecture::arm32;
#endif

inline constexpr Compiler compiler =
#if FLYG_COMPILER_CLANG
    Compiler::clang;
#elif FLYG_COMPILER_GCC
    Compiler::gcc;
#else
    Compiler::msvc;
#endif

inline constexpr bool is_windows = FLYG_PLATFORM_WINDOWS == 1;
inline constexpr bool is_linux = FLYG_PLATFORM_LINUX == 1;
inline constexpr bool is_little_endian = std::endian::native == std::endian::little;
inline constexpr bool is_64bit = sizeof(void*) == 8;

constexpr std::string_view to_string(OperatingSystem v) noexcept {
    return v == OperatingSystem::windows ? "windows" : "linux";
}
constexpr std::string_view to_string(Architecture v) noexcept {
    switch (v) {
        case Architecture::x86: return "x86";
        case Architecture::x64: return "x86_64";
        case Architecture::arm32: return "arm32";
        case Architecture::arm64: return "arm64";
    }
    return "unknown";
}
constexpr std::string_view to_string(Compiler v) noexcept {
    switch (v) {
        case Compiler::gcc: return "gcc";
        case Compiler::clang: return "clang";
        case Compiler::msvc: return "msvc";
    }
    return "unknown";
}

}  // namespace flyg::core
