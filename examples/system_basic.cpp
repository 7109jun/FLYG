// SPDX-License-Identifier: GPL-2.0-only
// flyG - system module example: machine info, thread pool, timers, files, child process
#include <flyg/flyg.hpp>

#include <iostream>

namespace sys = flyg::system;
using namespace std::chrono_literals;

int main() {
    std::cout << sys::describe_system() << '\n';

    // Thread pool
    sys::ThreadPool pool(4);
    auto f = pool.submit([] { return 6 * 7; });
    std::cout << "pool result: " << f.get() << '\n';

    // Timer
    sys::TimerQueue timers;
    std::atomic<int> ticks{0};
    timers.schedule_every(10ms, [&] { ++ticks; });
    sys::sleep_ms(105);
    std::cout << "timer ticks >= 5: " << std::boolalpha << (ticks.load() >= 5) << '\n';

    // Files
    sys::TempDir dir;
    sys::write_text(dir / "hello.txt", "hello flyG");
    std::cout << "file: " << sys::read_text(dir / "hello.txt") << " (" << sys::file_size(dir / "hello.txt")
              << " bytes)\n";

    // Child process (this very executable is not a helper here, so run a common tool)
#if FLYG_PLATFORM_WINDOWS
    const auto r = sys::run("cmd.exe", {"/c", "echo", "child", "says", "hi"});
#else
    const auto r = sys::run("echo", {"child", "says", "hi"});
#endif
    std::cout << "process exit " << r.exit_code << ": " << r.out;
    std::cout << "now: " << sys::now_iso8601() << '\n';
    return r.success() ? 0 : 1;
}
