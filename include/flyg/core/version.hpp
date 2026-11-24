// flyG - version information
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string_view>

#define FLYG_VERSION_MAJOR 0
#define FLYG_VERSION_MINOR 1
#define FLYG_VERSION_PATCH 0
#define FLYG_VERSION_NUMBER \
    (FLYG_VERSION_MAJOR * 10000 + FLYG_VERSION_MINOR * 100 + FLYG_VERSION_PATCH)

namespace flyg::core {

struct Version {
    int major;
    int minor;
    int patch;
    constexpr int number() const noexcept { return major * 10000 + minor * 100 + patch; }
    friend constexpr bool operator==(const Version&, const Version&) = default;
    friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

inline constexpr Version version{FLYG_VERSION_MAJOR, FLYG_VERSION_MINOR, FLYG_VERSION_PATCH};
inline constexpr std::string_view version_string = "0.1.0";
inline constexpr std::string_view license = "GPL-2.0-only";

static_assert(version.number() == FLYG_VERSION_NUMBER);

}  // namespace flyg::core
