// flyG - minimal self-contained unit test framework (header-only)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace flyg_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline std::atomic<int>& failure_count() {
    static std::atomic<int> n{0};
    return n;
}

inline std::atomic<int>& check_count() {
    static std::atomic<int> n{0};
    return n;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failure_count();
    std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, what.c_str());
}

inline int run_all(const char* filter = nullptr) {
    int ran = 0;
    int failed_cases = 0;
    for (const Case& c : registry()) {
        if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failure_count();
        try {
            c.fn();
        } catch (const std::exception& e) {
            report_failure(c.name, 0, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            report_failure(c.name, 0, "unexpected unknown exception");
        }
        const bool ok = failure_count() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
        ++ran;
        if (!ok) ++failed_cases;
    }
    std::printf("\n%d test(s), %d failed, %d check(s)\n", ran, failed_cases, check_count().load());
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace flyg_test

#define FLYG_TEST(name)                                                        \
    static void name();                                                        \
    static ::flyg_test::Registrar flyg_test_registrar_##name(#name, &name);    \
    static void name()

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++::flyg_test::check_count();                                          \
        if (!(expr)) ::flyg_test::report_failure(__FILE__, __LINE__, #expr);   \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++::flyg_test::check_count();                                          \
        if (!((a) == (b)))                                                     \
            ::flyg_test::report_failure(__FILE__, __LINE__, #a " == " #b);     \
    } while (0)

#define CHECK_THROWS_AS(expr, type)                                            \
    do {                                                                       \
        ++::flyg_test::check_count();                                          \
        try {                                                                  \
            (void)(expr);                                                      \
            ::flyg_test::report_failure(__FILE__, __LINE__,                    \
                                        "no exception from " #expr);           \
        } catch (const type&) {                                                \
        } catch (...) {                                                        \
            ::flyg_test::report_failure(__FILE__, __LINE__,                    \
                                        "wrong exception type from " #expr);   \
        }                                                                      \
    } while (0)

#define CHECK_NOTHROW(expr)                                                    \
    do {                                                                       \
        ++::flyg_test::check_count();                                          \
        try {                                                                  \
            (void)(expr);                                                      \
        } catch (...) {                                                        \
            ::flyg_test::report_failure(__FILE__, __LINE__,                    \
                                        "unexpected exception from " #expr);   \
        }                                                                      \
    } while (0)
