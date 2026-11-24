// flyG - system module: operating-system cryptographic random numbers
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <span>

#include <flyg/system/native.hpp>

#if FLYG_PLATFORM_WINDOWS
#  include <bcrypt.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "bcrypt.lib")
#  endif
#else
#  include <sys/random.h>
#endif

namespace flyg::system {

/// Fills `out` with cryptographically secure random bytes from the OS (getrandom / BCryptGenRandom).
inline void secure_random(std::span<std::byte> out) {
    std::size_t done = 0;
#if FLYG_PLATFORM_WINDOWS
    while (done < out.size()) {
        const ULONG n = static_cast<ULONG>(std::min<std::size_t>(out.size() - done, 1u << 20));
        if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data() + done), n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            throw SystemException("BCryptGenRandom failed", 0);
        done += n;
    }
#else
    while (done < out.size()) {
        const ssize_t n = ::getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw SystemException::from_errno("getrandom failed");
        }
        done += static_cast<std::size_t>(n);
    }
#endif
}

template <std::size_t N>
[[nodiscard]] std::array<std::byte, N> secure_random_array() {
    std::array<std::byte, N> a;
    secure_random(a);
    return a;
}

}  // namespace flyg::system
