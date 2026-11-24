// flyG - fundamental types
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <flyg/core/platform.hpp>

namespace flyg::core {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;
using isize = std::ptrdiff_t;
using f32 = float;
using f64 = double;

static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "IEEE-754 float sizes required");

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;

inline constexpr usize KiB = usize{1} << 10;
inline constexpr usize MiB = usize{1} << 20;
inline constexpr usize GiB = usize{1} << 30;

}  // namespace flyg::core

namespace flyg {
using core::f32;
using core::f64;
using core::i16;
using core::i32;
using core::i64;
using core::i8;
using core::isize;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;
using core::ByteSpan;
using core::MutableByteSpan;
}  // namespace flyg
