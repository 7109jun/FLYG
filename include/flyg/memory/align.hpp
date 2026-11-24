// flyG - memory alignment helpers and raw aligned allocation
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <flyg/core/exception.hpp>
#include <flyg/core/platform.hpp>
#include <flyg/core/types.hpp>

#if FLYG_PLATFORM_WINDOWS
#  include <malloc.h>
#endif

namespace flyg::memory {

using core::usize;

inline constexpr usize default_alignment = alignof(std::max_align_t);

[[nodiscard]] constexpr bool is_power_of_two(usize x) noexcept { return std::has_single_bit(x); }

/// Rounds `v` up to a multiple of `a` (`a` must be a power of two, no overflow check).
[[nodiscard]] constexpr usize align_up(usize v, usize a) noexcept { return (v + (a - 1)) & ~(a - 1); }
[[nodiscard]] constexpr usize align_down(usize v, usize a) noexcept { return v & ~(a - 1); }

[[nodiscard]] inline bool is_aligned(const void* p, usize a) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0;
}

/// Throws MemoryException(bad_alignment) unless `a` is a non-zero power of two.
inline void validate_alignment(usize a) {
    if (!is_power_of_two(a))
        throw MemoryException("alignment must be a non-zero power of two", ErrorCode::bad_alignment,
                              0, a);
}

/// Like align_up but throws on overflow and validates the alignment.
[[nodiscard]] inline usize checked_align_up(usize v, usize a) {
    validate_alignment(a);
    if (v > static_cast<usize>(-1) - (a - 1))
        throw MemoryException("aligned size overflow", ErrorCode::overflow, v, a);
    return align_up(v, a);
}

/// Aligned allocation primitive. Returns nullptr on failure. Size 0 is rounded up to 1.
[[nodiscard]] inline void* aligned_alloc_raw(usize size, usize alignment) noexcept {
    if (size == 0) size = 1;
#if FLYG_PLATFORM_WINDOWS
    return ::_aligned_malloc(size, alignment);
#else
    void* p = nullptr;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (::posix_memalign(&p, alignment, size) != 0) return nullptr;
    return p;
#endif
}

inline void aligned_free_raw(void* p) noexcept {
#if FLYG_PLATFORM_WINDOWS
    ::_aligned_free(p);
#else
    ::free(p);
#endif
}

}  // namespace flyg::memory
