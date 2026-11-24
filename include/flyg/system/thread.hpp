// flyG - threads: naming, affinity, priority, thread pool, events, spin lock
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <flyg/system/native.hpp>

namespace flyg::system {

[[nodiscard]] inline u32 hardware_threads() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

/// Numeric id of the calling thread as the OS sees it (tid / thread id).
[[nodiscard]] inline u64 current_thread_id() noexcept {
#if FLYG_PLATFORM_LINUX
    return static_cast<u64>(::syscall(SYS_gettid));
#else
    return static_cast<u64>(::GetCurrentThreadId());
#endif
}

/// Names the calling thread (visible in debuggers / top). Linux truncates to 15 bytes.
inline void set_current_thread_name(const std::string& name) {
#if FLYG_PLATFORM_LINUX
    const std::string n = name.substr(0, 15);
    const int rc = ::pthread_setname_np(::pthread_self(), n.c_str());
    if (rc != 0) throw SystemException("set_current_thread_name failed", rc);
#else
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    auto fn = k ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(k, "SetThreadDescription")))
                : nullptr;
    if (!fn) throw SystemException("SetThreadDescription is not available on this Windows version", 0);
    if (FAILED(fn(::GetCurrentThread(), detail::to_wide(name).c_str())))
        throw SystemException("SetThreadDescription failed", 0);
#endif
}

[[nodiscard]] inline std::string current_thread_name() {
#if FLYG_PLATFORM_LINUX
    char buf[32]{};
    const int rc = ::pthread_getname_np(::pthread_self(), buf, sizeof buf);
    if (rc != 0) throw SystemException("current_thread_name failed", rc);
    return buf;
#else
    using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    auto fn = k ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(k, "GetThreadDescription")))
                : nullptr;
    if (!fn) throw SystemException("GetThreadDescription is not available on this Windows version", 0);
    PWSTR w = nullptr;
    if (FAILED(fn(::GetCurrentThread(), &w))) throw SystemException("GetThreadDescription failed", 0);
    std::string s = detail::from_wide(w);
    ::LocalFree(w);
    return s;
#endif
}

/// Restricts the calling thread to the given logical CPUs.
inline void set_current_thread_affinity(const std::vector<u32>& cpus) {
    if (cpus.empty()) throw SystemException("affinity: empty CPU list", 0);
#if FLYG_PLATFORM_LINUX
    cpu_set_t set;
    CPU_ZERO(&set);
    for (u32 c : cpus) {
        if (c >= CPU_SETSIZE) throw SystemException("affinity: CPU index out of range", 0);
        CPU_SET(c, &set);
    }
    if (::sched_setaffinity(0, sizeof set, &set) != 0) throw SystemException::from_errno("sched_setaffinity failed");
#else
    DWORD_PTR mask = 0;
    for (u32 c : cpus) {
        if (c >= sizeof(DWORD_PTR) * 8) throw SystemException("affinity: CPU index out of range", 0);
        mask |= DWORD_PTR{1} << c;
    }
    if (::SetThreadAffinityMask(::GetCurrentThread(), mask) == 0)
        throw SystemException("SetThreadAffinityMask failed", detail::last_error());
#endif
}

/// CPUs the calling thread may run on.
[[nodiscard]] inline std::vector<u32> current_thread_affinity() {
    std::vector<u32> out;
#if FLYG_PLATFORM_LINUX
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof set, &set) != 0) throw SystemException::from_errno("sched_getaffinity failed");
    for (u32 i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set)) out.push_back(i);
#else
    // Windows has no GetThreadAffinityMask: SetThreadAffinityMask returns the previous mask, so
    // widen to the process mask, read the old value from the result and restore it.
    DWORD_PTR proc = 0, sys = 0;
    if (!::GetProcessAffinityMask(::GetCurrentProcess(), &proc, &sys))
        throw SystemException("GetProcessAffinityMask failed", detail::last_error());
    const DWORD_PTR prev = ::SetThreadAffinityMask(::GetCurrentThread(), proc);
    if (prev == 0) throw SystemException("SetThreadAffinityMask failed", detail::last_error());
    ::SetThreadAffinityMask(::GetCurrentThread(), prev);
    for (u32 i = 0; i < sizeof(DWORD_PTR) * 8; ++i)
        if (prev & (DWORD_PTR{1} << i)) out.push_back(i);
#endif
    return out;
}

enum class ThreadPriority { lowest, low, normal, high, highest };

/// Sets the priority of the calling thread. On Linux this adjusts the thread's nice value
/// (raising priority needs privileges and throws SystemException without them).
inline void set_current_thread_priority(ThreadPriority p) {
#if FLYG_PLATFORM_LINUX
    int nice = 0;
    switch (p) {
        case ThreadPriority::lowest: nice = 19; break;
        case ThreadPriority::low: nice = 10; break;
        case ThreadPriority::normal: nice = 0; break;
        case ThreadPriority::high: nice = -5; break;
        case ThreadPriority::highest: nice = -10; break;
    }
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()), nice) != 0)
        throw SystemException::from_errno("setpriority failed");
#else
    int w = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case ThreadPriority::lowest: w = THREAD_PRIORITY_LOWEST; break;
        case ThreadPriority::low: w = THREAD_PRIORITY_BELOW_NORMAL; break;
        case ThreadPriority::normal: w = THREAD_PRIORITY_NORMAL; break;
        case ThreadPriority::high: w = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case ThreadPriority::highest: w = THREAD_PRIORITY_HIGHEST; break;
    }
    if (!::SetThreadPriority(::GetCurrentThread(), w)) throw SystemException("SetThreadPriority failed", detail::last_error());
#endif
}

[[nodiscard]] inline ThreadPriority current_thread_priority() {
#if FLYG_PLATFORM_LINUX
    errno = 0;
    const int n = ::getpriority(PRIO_PROCESS, static_cast<id_t>(current_thread_id()));
    if (n == -1 && errno != 0) throw SystemException::from_errno("getpriority failed");
    if (n >= 15) return ThreadPriority::lowest;
    if (n >= 5) return ThreadPriority::low;
    if (n > -3) return ThreadPriority::normal;
    if (n > -8) return ThreadPriority::high;
    return ThreadPriority::highest;
#else
    switch (::GetThreadPriority(::GetCurrentThread())) {
        case THREAD_PRIORITY_LOWEST: return ThreadPriority::lowest;
        case THREAD_PRIORITY_BELOW_NORMAL: return ThreadPriority::low;
        case THREAD_PRIORITY_ABOVE_NORMAL: return ThreadPriority::high;
        case THREAD_PRIORITY_HIGHEST: return ThreadPriority::highest;
        default: return ThreadPriority::normal;
    }
#endif
}

// ---------------------------------------------------------------------------------------------
// Synchronization helpers
// ---------------------------------------------------------------------------------------------

/// Test-and-test-and-set spin lock (BasicLockable, so usable with std::lock_guard).
class SpinLock {
public:
    void lock() noexcept {
        for (;;) {
            if (!flag_.exchange(true, std::memory_order_acquire)) return;
            while (flag_.load(std::memory_order_relaxed)) std::this_thread::yield();
        }
    }
    [[nodiscard]] bool try_lock() noexcept { return !flag_.exchange(true, std::memory_order_acquire); }
    void unlock() noexcept { flag_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> flag_{false};
};

/// Win32-style event. Manual-reset: stays signaled until reset(). Auto-reset: releases exactly one
/// waiter per set().
class Event {
public:
    enum class Mode { manual_reset, auto_reset };

    explicit Event(Mode mode = Mode::manual_reset, bool initially_set = false) noexcept
        : auto_(mode == Mode::auto_reset), set_(initially_set) {}

    void set() {
        {
            std::lock_guard lock(m_);
            set_ = true;
        }
        if (auto_) cv_.notify_one();
        else cv_.notify_all();
    }
    void reset() {
        std::lock_guard lock(m_);
        set_ = false;
    }
    void wait() {
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return set_; });
        if (auto_) set_ = false;
    }
    /// Returns true if the event was signaled before the timeout.
    template <class Rep, class Period>
    [[nodiscard]] bool wait_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(m_);
        if (!cv_.wait_for(lock, timeout, [&] { return set_; })) return false;
        if (auto_) set_ = false;
        return true;
    }
    [[nodiscard]] bool is_set() const {
        std::lock_guard lock(m_);
        return set_;
    }

private:
    const bool auto_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool set_;
};

// ---------------------------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------------------------

/// Fixed-size pool of worker threads. Exceptions thrown by tasks travel through the futures.
class ThreadPool {
public:
    explicit ThreadPool(usize threads = 0, const std::string& name_prefix = "flyg-worker") {
        if (threads == 0) threads = hardware_threads();
        workers_.reserve(threads);
        for (usize i = 0; i < threads; ++i) {
            workers_.emplace_back([this, name = name_prefix + "-" + std::to_string(i)] {
                try {
                    set_current_thread_name(name);
                } catch (...) {  // naming is best effort
                }
                worker_loop();
            });
        }
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool() {
        {
            std::lock_guard lock(m_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& w : workers_) w.join();  // remaining queued tasks are still executed
    }

    [[nodiscard]] usize size() const noexcept { return workers_.size(); }

    template <class F, class... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(std::move(fn), std::move(tup));
            });
        std::future<R> fut = task->get_future();
        {
            std::lock_guard lock(m_);
            if (stopping_) throw SystemException("ThreadPool: submit after shutdown", 0);
            queue_.emplace_back([task] { (*task)(); });
        }
        work_cv_.notify_one();
        return fut;
    }

    /// Blocks until every queued and running task has finished.
    void wait_idle() {
        std::unique_lock lock(m_);
        idle_cv_.wait(lock, [&] { return queue_.empty() && active_ == 0; });
    }

    /// Runs `body(i)` for i in [begin, end) split into contiguous chunks across the workers and
    /// waits for completion. The first exception thrown by any chunk is rethrown.
    template <class F>
    void parallel_for(usize begin, usize end, F&& body) {
        if (end <= begin) return;
        const usize total = end - begin;
        const usize chunks = std::min(total, workers_.size() * 4);
        const usize per = (total + chunks - 1) / chunks;
        std::vector<std::future<void>> futs;
        futs.reserve(chunks);
        for (usize lo = begin; lo < end; lo += per) {
            const usize hi = std::min(end, lo + per);
            futs.push_back(submit([&body, lo, hi] {
                for (usize i = lo; i < hi; ++i) body(i);
            }));
        }
        std::exception_ptr first;
        for (auto& f : futs) {
            try {
                f.get();
            } catch (...) {
                if (!first) first = std::current_exception();
            }
        }
        if (first) std::rethrow_exception(first);
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(m_);
                work_cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stopping and drained
                job = std::move(queue_.front());
                queue_.pop_front();
                ++active_;
            }
            job();  // packaged_task never throws
            {
                std::lock_guard lock(m_);
                --active_;
                if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
            }
        }
    }

    std::mutex m_;
    std::condition_variable work_cv_, idle_cv_;
    std::deque<std::function<void()>> queue_;
    usize active_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace flyg::system
