// flyG - core module tests
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/core/core.hpp>

#include <cerrno>
#include <limits>

#include "test_framework.hpp"

using namespace flyg;

FLYG_TEST(core_platform_constants) {
    CHECK(core::is_windows != core::is_linux);
    CHECK(core::is_64bit);
    CHECK(core::is_little_endian);
#if defined(_WIN32)
    CHECK(core::os == core::OperatingSystem::windows);
    CHECK_EQ(core::to_string(core::os), "windows");
#else
    CHECK(core::os == core::OperatingSystem::linux_os);
    CHECK_EQ(core::to_string(core::os), "linux");
#endif
#if FLYG_ARCH_X64
    CHECK_EQ(core::to_string(core::arch), "x86_64");
#elif FLYG_ARCH_ARM64
    CHECK_EQ(core::to_string(core::arch), "arm64");
#endif
}

FLYG_TEST(core_types_and_version) {
    static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
    static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8);
    CHECK_EQ(core::MiB, usize{1048576});
    CHECK_EQ(core::version.number(), FLYG_VERSION_NUMBER);
    CHECK_EQ(core::version_string, "0.1.0");
    constexpr core::Version next{0, 2, 0};
    CHECK(core::version < next);
    CHECK_EQ(core::license, "GPL-2.0-only");
}

FLYG_TEST(core_exception_hierarchy) {
    try {
        throw MemoryException("boom", ErrorCode::out_of_memory, 128, 16);
    } catch (const Exception& e) {  // catch through the base class
        CHECK(e.code() == ErrorCode::out_of_memory);
        CHECK_EQ(e.message(), std::string("boom (size=128, alignment=16)"));
        const std::string w = e.what();
        CHECK(w.find("out_of_memory") != std::string::npos);
        CHECK(w.find("test_core.cpp") != std::string::npos);  // source location recorded
        CHECK(e.location().line() != 0);
    }
    CHECK_THROWS_AS(throw SystemException("x"), Exception);
    CHECK_THROWS_AS(throw GraphicsException("x", "vulkan"), Exception);
    CHECK_THROWS_AS(throw NetworkException("x"), Exception);
    CHECK_THROWS_AS(throw DataException("x"), Exception);
    CHECK_THROWS_AS(throw MathException("x"), Exception);
    CHECK_THROWS_AS(throw MemoryException("x"), Exception);
    CHECK_THROWS_AS(throw Exception("x"), std::exception);
}

FLYG_TEST(core_exception_details) {
    DataException d("bad token", 42);
    CHECK_EQ(d.offset(), usize{42});
    CHECK(d.code() == ErrorCode::data_error);
    CHECK(std::string(d.what()).find("offset 42") != std::string::npos);
    CHECK_EQ(DataException("plain").offset(), DataException::no_offset);

    GraphicsException g("device lost", "vulkan");
    CHECK_EQ(g.backend(), "vulkan");
    CHECK(g.code() == ErrorCode::graphics_error);

    NetworkException n("connect failed", 111);
    CHECK_EQ(n.native_code(), 111);
    CHECK(n.code() == ErrorCode::network_error);

    errno = ENOENT;
    SystemException s = SystemException::from_errno("open failed");
    CHECK_EQ(s.native_code(), ENOENT);
    CHECK(s.code() == ErrorCode::system_error);

    MathException m("singular matrix");
    CHECK(m.code() == ErrorCode::math_error);
}

FLYG_TEST(core_checked_arithmetic) {
    CHECK_EQ(core::checked_cast<u8>(255), u8{255});
    CHECK_THROWS_AS(core::checked_cast<u8>(256), Exception);
    CHECK_THROWS_AS(core::checked_cast<u32>(-1), Exception);
    CHECK_THROWS_AS(core::checked_cast<i32>(std::numeric_limits<u64>::max()), Exception);

    CHECK_EQ(core::checked_add(1, 2), usize{3});
    CHECK_THROWS_AS(core::checked_add(std::numeric_limits<usize>::max(), 1), Exception);
    CHECK_EQ(core::checked_mul(1000, 1000), usize{1000000});
    CHECK_EQ(core::checked_mul(0, std::numeric_limits<usize>::max()), usize{0});
    CHECK_THROWS_AS(core::checked_mul(std::numeric_limits<usize>::max(), 2), Exception);

    try {
        (void)core::checked_mul(std::numeric_limits<usize>::max(), 2);
    } catch (const Exception& e) {
        CHECK(e.code() == ErrorCode::overflow);
    }
}

FLYG_TEST(core_scope_exit_and_ensure) {
    int n = 0;
    {
        auto g = core::scope_exit([&] { ++n; });
        CHECK_EQ(n, 0);
    }
    CHECK_EQ(n, 1);
    {
        auto g = core::scope_exit([&] { ++n; });
        g.dismiss();
    }
    CHECK_EQ(n, 1);
    CHECK_NOTHROW(core::ensure(true, "fine"));
    CHECK_THROWS_AS(core::ensure(false, "nope"), Exception);
    try {
        core::ensure(false, "range", ErrorCode::out_of_range);
    } catch (const Exception& e) {
        CHECK(e.code() == ErrorCode::out_of_range);
    }
}

FLYG_TEST(core_formatting_helpers) {
    const std::byte data[] = {std::byte{0xde}, std::byte{0xad}, std::byte{0x00}, std::byte{0xff}};
    CHECK_EQ(core::to_hex(data), "de ad 00 ff");
    CHECK_EQ(core::to_hex(data, '\0'), "dead00ff");
    CHECK_EQ(core::format_bytes(512), "512 B");
    CHECK_EQ(core::format_bytes(1536), "1.50 KiB");
    CHECK_EQ(core::format_bytes(3 * core::MiB), "3.00 MiB");

    usize seed = 0;
    core::hash_combine(seed, 1);
    const usize a = seed;
    core::hash_combine(seed, 2);
    CHECK(a != seed);
}
