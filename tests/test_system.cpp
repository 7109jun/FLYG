// flyG - system module tests
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/system/system.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <numeric>

#if FLYG_PLATFORM_WINDOWS
#  include <fcntl.h>
#  include <io.h>
#endif

#include "test_framework.hpp"

using namespace flyg;
namespace fs = std::filesystem;
namespace sys = flyg::system;
using namespace std::chrono_literals;

// ---- helper child process (re-executes this binary) -------------------------------------------
int flyg_child_main(int argc, char** argv) {
#if FLYG_PLATFORM_WINDOWS
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const std::string mode = argc > 2 ? argv[2] : "";
    if (mode == "exit") return argc > 3 ? std::atoi(argv[3]) : 0;
    if (mode == "cat") {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) std::fwrite(buf, 1, n, stdout);
        return 0;
    }
    if (mode == "streams") {
        std::fputs("to-stdout", stdout);
        std::fflush(stdout);
        std::fputs("to-stderr", stderr);
        std::fflush(stderr);
        return 3;
    }
    if (mode == "sleep") {
        sys::sleep_ms(60000);
        return 0;
    }
    if (mode == "env") {
        const auto v = sys::get_env(argc > 3 ? argv[3] : "");
        std::fputs(v ? v->c_str() : "<unset>", stdout);
        return 0;
    }
    if (mode == "cwd") {
        std::fputs(sys::current_directory().string().c_str(), stdout);
        return 0;
    }
    if (mode == "args") {
        const auto all = sys::command_line();  // UTF-8 on every platform (argv is ANSI on Windows)
        for (std::size_t i = 3; i < all.size(); ++i) std::printf("[%s]", all[i].c_str());
        return 0;
    }
    if (mode == "big") {  // writes 1 MiB to stdout and 1 MiB to stderr (pipe back-pressure)
        const std::string chunk(1024, 'x');
        for (int i = 0; i < 1024; ++i) {
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fwrite(chunk.data(), 1, chunk.size(), stderr);
        }
        return 0;
    }
    return 99;
}

namespace {
const std::string self = sys::executable_path().string();
}

// ---- environment -----------------------------------------------------------------------------
FLYG_TEST(system_env_basic) {
    CHECK(!sys::has_env("FLYG_TEST_VAR"));
    sys::set_env("FLYG_TEST_VAR", "hello");
    CHECK_EQ(sys::get_env("FLYG_TEST_VAR").value(), "hello");
    sys::set_env("FLYG_TEST_VAR", "");
    CHECK(sys::get_env("FLYG_TEST_VAR").has_value());
    CHECK_EQ(sys::get_env("FLYG_TEST_VAR").value(), "");
    CHECK(sys::env_vars().count("FLYG_TEST_VAR") == 1);
    sys::unset_env("FLYG_TEST_VAR");
    CHECK(!sys::get_env("FLYG_TEST_VAR").has_value());
    CHECK_EQ(sys::get_env_or("FLYG_TEST_VAR", "dflt"), "dflt");
    CHECK_NOTHROW(sys::unset_env("FLYG_TEST_VAR"));  // already gone
    CHECK_THROWS_AS(sys::set_env("A=B", "x"), SystemException);
    CHECK_THROWS_AS(sys::set_env("", "x"), SystemException);
    sys::set_env("FLYG_UNICODE", "한글-é");
    CHECK_EQ(sys::get_env("FLYG_UNICODE").value(), "한글-é");
    sys::unset_env("FLYG_UNICODE");
}

FLYG_TEST(system_env_scoped_and_expand) {
    sys::set_env("FLYG_A", "alpha");
    {
        sys::ScopedEnv s("FLYG_A", "beta");
        CHECK_EQ(sys::get_env("FLYG_A").value(), "beta");
        sys::ScopedEnv s2("FLYG_B", "new");
        CHECK(sys::has_env("FLYG_B"));
    }
    CHECK_EQ(sys::get_env("FLYG_A").value(), "alpha");
    CHECK(!sys::has_env("FLYG_B"));
    CHECK_EQ(sys::expand_env("x=${FLYG_A}/$FLYG_A!"), "x=alpha/alpha!");
    CHECK_EQ(sys::expand_env("cost $$5 $ end ${NOPE_NOT_SET}."), "cost $5 $ end .");
    CHECK_THROWS_AS(sys::expand_env("${unterminated"), SystemException);
    sys::unset_env("FLYG_A");
}

FLYG_TEST(system_command_line) {
    const auto args = sys::command_line();
    CHECK(!args.empty());
}

// ---- time ------------------------------------------------------------------------------------
FLYG_TEST(system_time_calendar) {
    CHECK_EQ(sys::format_iso8601(0, 0), "1970-01-01T00:00:00Z");
    CHECK_EQ(sys::format_iso8601(1'000'000'000LL * 1'000'000'000LL, 3), "2001-09-09T01:46:40.000Z");
    CHECK_EQ(sys::format_iso8601(-1, 9), "1969-12-31T23:59:59.999999999Z");
    CHECK_EQ(sys::format_iso8601(951'782'400LL * 1'000'000'000LL, 0), "2000-02-29T00:00:00Z");
    CHECK_THROWS_AS(sys::format_iso8601(0, 10), SystemException);

    const sys::DateTime dt{2026, 11, 2, 8, 47, 5, 123456789};
    CHECK(sys::to_datetime(sys::from_datetime(dt)) == dt);
    CHECK_THROWS_AS(sys::from_datetime({2026, 2, 30, 0, 0, 0, 0}), SystemException);
    CHECK_THROWS_AS(sys::from_datetime({2026, 13, 1, 0, 0, 0, 0}), SystemException);
    CHECK_EQ(sys::day_of_week({1970, 1, 1, 0, 0, 0, 0}), 4);   // Thursday
    CHECK_EQ(sys::day_of_week({2026, 11, 2, 0, 0, 0, 0}), 1);  // Monday
    CHECK(sys::is_leap_year(2000) && !sys::is_leap_year(1900) && sys::is_leap_year(2024));

    // Round trip over a wide range of days, including pre-1970.
    for (i64 day = -100000; day <= 100000; day += 37) {
        const i64 ns = day * 86400LL * 1'000'000'000LL + 12345;
        CHECK_EQ(sys::from_datetime(sys::to_datetime(ns)), ns);
    }
}

FLYG_TEST(system_time_parse) {
    CHECK_EQ(sys::parse_iso8601("1970-01-01T00:00:00Z"), i64{0});
    CHECK_EQ(sys::parse_iso8601("1970-01-01"), i64{0});
    CHECK_EQ(sys::parse_iso8601("1970-01-01 00:00:01"), i64{1'000'000'000});
    CHECK_EQ(sys::parse_iso8601("1970-01-01T09:00:00+09:00"), i64{0});
    CHECK_EQ(sys::parse_iso8601("1970-01-01T00:00:00-0130"), i64{5400} * 1'000'000'000);
    CHECK_EQ(sys::parse_iso8601("1970-01-01T00:00:00.5Z"), i64{500'000'000});
    CHECK_EQ(sys::parse_iso8601("1970-01-01T00:00:00.123456789123Z"), i64{123'456'789});
    const auto now = sys::unix_time_ns();
    CHECK_EQ(sys::parse_iso8601(sys::format_iso8601(now, 9)), now);
    CHECK_THROWS_AS(sys::parse_iso8601("garbage"), SystemException);
    CHECK_THROWS_AS(sys::parse_iso8601("2026-02-30"), SystemException);
    CHECK_THROWS_AS(sys::parse_iso8601("2026-01-01T00:00:00Zjunk"), SystemException);
    CHECK_THROWS_AS(sys::parse_iso8601("2026-01-01T00:00:00."), SystemException);
    CHECK(std::abs(sys::local_utc_offset_seconds()) <= 14 * 3600);
}

FLYG_TEST(system_time_clocks_and_stopwatch) {
    const u64 a = sys::monotonic_ns();
    sys::Stopwatch sw;
    sys::sleep_ms(20);
    sw.stop();
    const u64 b = sys::monotonic_ns();
    CHECK(b >= a + 15'000'000);
    CHECK(sw.milliseconds() >= 15.0);
    CHECK(sw.milliseconds() < 2000.0);
    const double frozen = sw.seconds();
    sys::sleep_ms(5);
    CHECK_EQ(sw.seconds(), frozen);
    sw.restart();
    CHECK(sw.milliseconds() < 15.0);
    CHECK(sys::unix_time_seconds() > 1.7e9);  // after Nov 2023
}

FLYG_TEST(system_timer_queue) {
    sys::TimerQueue q;
    std::atomic<int> once{0}, periodic{0}, cancelled{0};
    q.schedule_after(10ms, [&] { ++once; });
    q.schedule_after(30ms, [&] { once += 10; });
    const auto id = q.schedule_after(5s, [&] { ++cancelled; });
    CHECK(q.cancel(id));
    CHECK(!q.cancel(id));
    const auto p = q.schedule_every(10ms, [&] { ++periodic; });
    sys::sleep_ms(250);
    CHECK_EQ(once.load(), 11);
    CHECK_EQ(cancelled.load(), 0);
    CHECK(periodic.load() >= 5);
    CHECK(q.cancel(p));
    sys::sleep_ms(40);
    const int frozen = periodic.load();
    sys::sleep_ms(80);
    CHECK_EQ(periodic.load(), frozen);
    CHECK_THROWS_AS(q.schedule_every(0ns, [] {}), SystemException);
    CHECK_THROWS_AS(q.schedule_after(1ms, nullptr), SystemException);
    // an exception in a callback must not kill the timer thread
    std::atomic<int> after{0};
    q.schedule_after(1ms, [] { throw 1; });
    q.schedule_after(20ms, [&] { ++after; });
    sys::sleep_ms(100);
    CHECK_EQ(after.load(), 1);
}

FLYG_TEST(system_timer_queue_destroy_with_pending) {
    std::atomic<int> n{0};
    {
        sys::TimerQueue q;
        for (int i = 0; i < 100; ++i) q.schedule_after(10s, [&] { ++n; });
        CHECK_EQ(q.pending(), usize{100});
    }
    CHECK_EQ(n.load(), 0);
}

// ---- info ------------------------------------------------------------------------------------
FLYG_TEST(system_info) {
    const auto c = sys::cpu_info();
    CHECK(c.logical_cores >= 1);
    CHECK(c.physical_cores >= 1 && c.physical_cores <= c.logical_cores);
    CHECK(!c.architecture.empty());
#if FLYG_ARCH_X64
    CHECK(c.features.sse2);
    CHECK(!c.vendor.empty());
#endif

    const auto m = sys::memory_info();
    CHECK(m.total_physical > 16 * core::MiB);
    CHECK(m.available_physical > 0 && m.available_physical <= m.total_physical);
    CHECK_EQ(m.used_physical(), m.total_physical - m.available_physical);

    const auto o = sys::os_info();
    CHECK(!o.name.empty());
    CHECK(!o.hostname.empty());
    CHECK(!o.username.empty());
    CHECK(!o.architecture.empty());
    CHECK(sys::uptime_seconds() > 0.0);

    const usize ps = sys::page_size();
    CHECK(ps >= 4096 && (ps & (ps - 1)) == 0);
    CHECK(sys::process_id() > 0);
    CHECK(sys::process_resident_bytes() > 64 * 1024);
    CHECK(sys::is_file(sys::executable_path()));
    CHECK(sys::is_directory(sys::home_directory()));
    CHECK(sys::describe_system().find("CPU") != std::string::npos);
}

// ---- threads ---------------------------------------------------------------------------------
FLYG_TEST(system_thread_pool) {
    sys::ThreadPool pool(4);
    CHECK_EQ(pool.size(), usize{4});
    std::vector<std::future<int>> f;
    for (int i = 0; i < 200; ++i) f.push_back(pool.submit([i] { return i * i; }));
    long total = 0;
    for (auto& x : f) total += x.get();
    CHECK_EQ(total, 2646700L);  // sum of squares 0..199

    auto with_args = pool.submit([](int a, const std::string& b) { return b + std::to_string(a); }, 7, std::string("n="));
    CHECK_EQ(with_args.get(), "n=7");

    auto bad = pool.submit([]() -> int { throw MathException("task failed"); });
    CHECK_THROWS_AS(bad.get(), MathException);

    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) (void)pool.submit([&] { sys::sleep_ms(1); ++done; });
    pool.wait_idle();
    CHECK_EQ(done.load(), 100);
}

FLYG_TEST(system_thread_pool_parallel_for) {
    sys::ThreadPool pool(3);
    std::vector<int> v(10007, 0);
    pool.parallel_for(0, v.size(), [&](usize i) { v[i] = static_cast<int>(i) + 1; });
    CHECK_EQ(std::accumulate(v.begin(), v.end(), 0LL), 10007LL * 10008 / 2);
    pool.parallel_for(5, 5, [&](usize) { v[0] = -1; });  // empty range
    CHECK_EQ(v[0], 1);
    CHECK_THROWS_AS(pool.parallel_for(0, 100, [](usize i) { if (i == 77) throw DataException("bad"); }), DataException);
}

FLYG_TEST(system_thread_pool_drains_on_destroy) {
    std::atomic<int> n{0};
    {
        sys::ThreadPool pool(2);
        for (int i = 0; i < 50; ++i) (void)pool.submit([&] { ++n; });
    }
    CHECK_EQ(n.load(), 50);
}

FLYG_TEST(system_event_and_spinlock) {
    sys::Event manual;
    CHECK(!manual.wait_for(5ms));
    std::thread t([&] { sys::sleep_ms(20); manual.set(); });
    manual.wait();
    CHECK(manual.is_set());
    CHECK(manual.wait_for(1ms));  // manual reset: still signaled
    manual.reset();
    CHECK(!manual.is_set());
    t.join();

    sys::Event autoev(sys::Event::Mode::auto_reset, true);
    CHECK(autoev.wait_for(1ms));
    CHECK(!autoev.wait_for(1ms));  // consumed by the first wait

    sys::SpinLock lock;
    long counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < 20000; ++k) {
                std::lock_guard g(lock);
                ++counter;
            }
        });
    for (auto& x : ts) x.join();
    CHECK_EQ(counter, 80000L);
    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());
    lock.unlock();
}

FLYG_TEST(system_thread_attributes) {
    CHECK(sys::hardware_threads() >= 1);
    CHECK(sys::current_thread_id() != 0);
    // Run in a helper thread so the test runner's own thread keeps its settings.
    std::thread t([] {
#if FLYG_PLATFORM_LINUX
        sys::set_current_thread_name("flyg-test-thr-long-name");
        CHECK_EQ(sys::current_thread_name(), "flyg-test-thr-l");  // truncated to 15 bytes
#else
        try {
            sys::set_current_thread_name("flyg-test-thread");
            CHECK_EQ(sys::current_thread_name(), "flyg-test-thread");
        } catch (const SystemException&) {  // SetThreadDescription missing on very old Windows/Wine
        }
#endif
        const auto before = sys::current_thread_affinity();
        CHECK(!before.empty());
        sys::set_current_thread_affinity({before.front()});
        const auto after = sys::current_thread_affinity();
        CHECK_EQ(after.size(), usize{1});
        CHECK_EQ(after.front(), before.front());
        CHECK_THROWS_AS(sys::set_current_thread_affinity({}), SystemException);

        sys::set_current_thread_priority(sys::ThreadPriority::lowest);
        CHECK(sys::current_thread_priority() == sys::ThreadPriority::lowest);
    });
    t.join();
}

// ---- filesystem ------------------------------------------------------------------------------
FLYG_TEST(system_fs_read_write) {
    sys::TempDir dir;
    CHECK(sys::is_directory(dir.path()));
    const auto f = dir / "a.txt";
    CHECK(!sys::exists(f));
    sys::write_text(f, "hello");
    sys::append_text(f, " world");
    CHECK_EQ(sys::read_text(f), "hello world");
    CHECK_EQ(sys::file_size(f), u64{11});
    CHECK(sys::is_file(f));

    std::vector<std::byte> bin(300000);
    for (usize i = 0; i < bin.size(); ++i) bin[i] = static_cast<std::byte>(i * 7);
    sys::write_file(dir / "b.bin", bin);
    CHECK(sys::read_file(dir / "b.bin") == bin);
    sys::write_file(dir / "empty", {});
    CHECK(sys::read_file(dir / "empty").empty());

    sys::write_file_atomic(f, std::as_bytes(std::span("replaced", 8)));
    CHECK_EQ(sys::read_text(f), "replaced");
    CHECK_EQ(sys::list_directory(dir.path()).size(), usize{3});  // no temp file left behind

    CHECK_THROWS_AS(sys::read_file(dir / "missing"), SystemException);
    CHECK_THROWS_AS(sys::write_text(dir / "no/such/dir/x", "x"), SystemException);
    CHECK_THROWS_AS(sys::file_size(dir / "missing"), SystemException);
    CHECK_THROWS_AS(sys::last_write_time(dir / "missing"), SystemException);
    CHECK_NOTHROW(sys::last_write_time(f));
}

FLYG_TEST(system_fs_directories) {
    sys::TempDir dir;
    CHECK(sys::create_directories(dir / "x/y/z"));
    CHECK(!sys::create_directories(dir / "x/y/z"));
    sys::write_text(dir / "x/one.txt", "1");
    sys::write_text(dir / "x/y/two.txt", "22");
    sys::write_text(dir / "x/y/z/three.md", "333");
    sys::write_text(dir / "top.txt", "4444");

    const auto shallow = sys::list_directory(dir.path());
    CHECK_EQ(shallow.size(), usize{2});
    const auto deep = sys::list_directory(dir.path(), true);
    CHECK_EQ(deep.size(), usize{7});
    for (usize i = 1; i < deep.size(); ++i) CHECK(deep[i - 1].path < deep[i].path);

    CHECK_EQ(sys::glob(dir.path(), "*.txt").size(), usize{1});
    CHECK_EQ(sys::glob(dir.path(), "**/*.txt").size(), usize{3});
    CHECK_EQ(sys::glob(dir.path(), "x/**/*.txt").size(), usize{2});
    CHECK_EQ(sys::glob(dir.path(), "**/*.md").size(), usize{1});
    CHECK_EQ(sys::glob(dir.path(), "x/y/*").size(), usize{1});
    CHECK_EQ(sys::glob(dir.path(), "nothing*").size(), usize{0});

    sys::copy(dir / "x", dir / "copy");
    CHECK_EQ(sys::read_text(dir / "copy/y/two.txt"), "22");
    sys::rename(dir / "copy", dir / "moved");
    CHECK(!sys::exists(dir / "copy"));
    CHECK(sys::exists(dir / "moved/y/z/three.md"));
    CHECK_THROWS_AS(sys::remove(dir / "moved"), SystemException);  // not empty
    CHECK(sys::remove_all(dir / "moved") >= 5);
    CHECK(sys::remove(dir / "top.txt"));
    CHECK(!sys::remove(dir / "top.txt"));
    CHECK_THROWS_AS(sys::list_directory(dir / "nope"), SystemException);

    const auto abs = sys::absolute_path("rel/path");
    CHECK(abs.is_absolute());
    CHECK(sys::canonical_path(dir.path()).is_absolute());
    CHECK_THROWS_AS(sys::canonical_path(dir / "nope"), SystemException);
}

FLYG_TEST(system_fs_wildcards) {
    CHECK(sys::wildcard_match("*.txt", "a.txt"));
    CHECK(sys::wildcard_match("*.txt", ".txt"));
    CHECK(!sys::wildcard_match("*.txt", "a.txt.bak"));
    CHECK(sys::wildcard_match("a?c", "abc"));
    CHECK(!sys::wildcard_match("a?c", "ac"));
    CHECK(sys::wildcard_match("[a-c]x", "bx"));
    CHECK(!sys::wildcard_match("[a-c]x", "dx"));
    CHECK(sys::wildcard_match("[!a-c]x", "dx"));
    CHECK(sys::wildcard_match("a*b*c", "aXXbYYc"));
    CHECK(!sys::wildcard_match("a*b*c", "aXXbYY"));
    CHECK(sys::wildcard_match("*", ""));
    CHECK(sys::wildcard_match("", ""));
    CHECK(!sys::wildcard_match("*a", "b/a"));  // '*' does not cross '/'
}

FLYG_TEST(system_fs_temp_objects) {
    fs::path kept;
    {
        sys::TempDir d("flyg-test-");
        sys::TempDir d2("flyg-test-");
        CHECK(d.path() != d2.path());
        CHECK(d.path().filename().string().rfind("flyg-test-", 0) == 0);
        sys::write_text(d / "f", "x");
        sys::TempFile tf("pfx-", ".dat", d.path());
        CHECK(sys::exists(tf.path()));
        CHECK_EQ(tf.path().extension().string(), ".dat");
        sys::TempDir moved(std::move(d2));
        CHECK(sys::exists(moved.path()));
        kept = sys::TempDir("flyg-keep-").release();
    }
    CHECK(sys::exists(kept));  // release() keeps it
    sys::remove_all(kept);
}

FLYG_TEST(system_fs_mapped_file) {
    sys::TempDir dir;
    const auto f = dir / "m.bin";
    std::string content(100000, 'a');
    content[99999] = 'z';
    sys::write_text(f, content);
    {
        sys::MappedFile m(f);
        CHECK(m.is_open());
        CHECK_EQ(m.size(), content.size());
        CHECK(std::memcmp(m.data(), content.data(), content.size()) == 0);
        CHECK_THROWS_AS(m.writable_bytes(), SystemException);
        sys::MappedFile m2 = std::move(m);
        CHECK(!m.is_open());
        CHECK(m2.is_open());
    }
    {
        sys::MappedFile rw(f, sys::MappedFile::Access::read_write);
        rw.writable_bytes()[0] = std::byte{'Q'};
        rw.writable_data()[1] = std::byte{'R'};
        rw.flush();
    }
    const auto back = sys::read_text(f);
    CHECK_EQ(back.substr(0, 3), "QRa");
    CHECK_EQ(back.size(), content.size());

    sys::write_file(dir / "empty", {});
    sys::MappedFile e(dir / "empty");
    CHECK(!e.is_open());
    CHECK_EQ(e.size(), usize{0});
    CHECK_THROWS_AS(sys::MappedFile(dir / "missing"), SystemException);
}

// ---- processes -------------------------------------------------------------------------------
FLYG_TEST(system_process_exit_codes) {
    auto r = sys::run(self, {"--child", "exit", "7"});
    CHECK_EQ(r.exit_code, 7);
    CHECK(!r.success());
    CHECK(!r.timed_out);
    CHECK(sys::run(self, {"--child", "exit", "0"}).success());
    CHECK_THROWS_AS(sys::run("flyg-definitely-not-a-program-xyz"), SystemException);
    CHECK_THROWS_AS(sys::Process::spawn(""), SystemException);
}

FLYG_TEST(system_process_capture_and_stdin) {
    auto r = sys::run(self, {"--child", "streams"});
    CHECK_EQ(r.exit_code, 3);
    CHECK_EQ(r.out, "to-stdout");
    CHECK_EQ(r.err, "to-stderr");

    sys::ProcessOptions merged;
    merged.merge_stderr = true;
    auto m = sys::run(self, {"--child", "streams"}, {}, 0ms, merged);
    CHECK_EQ(m.out.size(), usize{18});
    CHECK(m.err.empty());

    auto c = sys::run(self, {"--child", "cat"}, "echo this\nline 2\n");
    CHECK_EQ(c.out, "echo this\nline 2\n");

    // 3 MiB through stdin and back must not deadlock on pipe buffers.
    const std::string big(3 * 1024 * 1024, 'q');
    auto b = sys::run(self, {"--child", "cat"}, big);
    CHECK_EQ(b.out.size(), big.size());

    auto both = sys::run(self, {"--child", "big"});
    CHECK_EQ(both.out.size(), usize{1 << 20});
    CHECK_EQ(both.err.size(), usize{1 << 20});
}

FLYG_TEST(system_process_arguments_and_environment) {
    auto a = sys::run(self, {"--child", "args", "plain", "two words", "quote\"d", "back\\slash\\", "", "한글"});
    CHECK_EQ(a.out, "[plain][two words][quote\"d][back\\slash\\][][한글]");

    sys::ProcessOptions o;
    o.env["FLYG_CHILD_VAR"] = "from-parent";
    CHECK_EQ(sys::run(self, {"--child", "env", "FLYG_CHILD_VAR"}, {}, 0ms, o).out, "from-parent");
    CHECK_EQ(sys::run(self, {"--child", "env", "FLYG_CHILD_VAR"}).out, "<unset>");

    sys::set_env("FLYG_INHERITED", "yes");
    CHECK_EQ(sys::run(self, {"--child", "env", "FLYG_INHERITED"}).out, "yes");
    sys::ProcessOptions cleared;
    cleared.clear_env = true;
#if FLYG_PLATFORM_WINDOWS
    cleared.env["SystemRoot"] = sys::get_env_or("SystemRoot", "C:\\windows");
#endif
    CHECK_EQ(sys::run(self, {"--child", "env", "FLYG_INHERITED"}, {}, 0ms, cleared).out, "<unset>");
    sys::unset_env("FLYG_INHERITED");

    sys::TempDir dir;
    sys::ProcessOptions w;
    w.working_directory = dir.path();
    const auto out = sys::run(self, {"--child", "cwd"}, {}, 0ms, w).out;
    CHECK_EQ(sys::canonical_path(out), sys::canonical_path(dir.path()));
}

FLYG_TEST(system_process_timeout_and_kill) {
    sys::Stopwatch sw;
    auto r = sys::run(self, {"--child", "sleep"}, {}, 200ms);
    CHECK(r.timed_out);
    CHECK(!r.success());
    CHECK(r.signaled);
    CHECK(sw.seconds() < 20.0);

    sys::ProcessOptions o;
    sys::Process p = sys::Process::spawn(self, [&] { o.args = {"--child", "sleep"}; return o; }());
    CHECK(p.valid());
    CHECK(p.pid() != 0);
    CHECK(p.running());
    CHECK(!p.wait_for(30ms).has_value());
    p.kill();
    CHECK(p.wait() != 0);
    CHECK(!p.running());
    CHECK(p.signaled());
    p.kill();  // no-op once reaped

    // The destructor must kill and reap a still-running child.
    u32 pid = 0;
    {
        sys::ProcessOptions so;
        so.args = {"--child", "sleep"};
        sys::Process q = sys::Process::spawn(self, so);
        pid = q.pid();
    }
#if FLYG_PLATFORM_LINUX
    CHECK(::kill(static_cast<pid_t>(pid), 0) != 0);  // gone, not a zombie
#else
    (void)pid;
#endif
}

FLYG_TEST(system_process_interactive_pipes) {
    sys::ProcessOptions o;
    o.args = {"--child", "cat"};
    o.pipe_stdin = o.pipe_stdout = true;
    sys::Process p = sys::Process::spawn(self, o);
    p.write_stdin("ping");
    std::byte buf[16];
    // cat in this helper reads until EOF, so close stdin before expecting the echo.
    p.close_stdin();
    const usize n = p.read_stdout(buf);
    CHECK(n > 0);
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf), n), "ping");
    CHECK_EQ(p.read_stdout(buf), usize{0});  // EOF
    CHECK_EQ(p.wait(), 0);
    CHECK_THROWS_AS(p.read_stderr(buf), SystemException);  // not piped
    CHECK_THROWS_AS(p.write_stdin("x"), SystemException);  // closed

    sys::Process none;
    CHECK(!none.valid());
    CHECK_THROWS_AS(none.wait(), SystemException);
}
