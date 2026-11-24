// flyG - system module: OS headers and small native helpers (internal)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
#include <string_view>

#include <flyg/core/exception.hpp>
#include <flyg/core/platform.hpp>
#include <flyg/core/types.hpp>

#if FLYG_PLATFORM_WINDOWS
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifdef DrawState  // winuser.h macro would rename flyg::graphics::DrawState
#    undef DrawState
#  endif
#  include <psapi.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "advapi32.lib")
#  endif
#else
#  include <fcntl.h>
#  include <sched.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <sys/sysinfo.h>
#  include <sys/types.h>
#  include <sys/utsname.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <pthread.h>
extern char** environ;
#endif

namespace flyg::system::detail {

#if FLYG_PLATFORM_WINDOWS
[[nodiscard]] inline std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw SystemException("invalid UTF-8 string", static_cast<int>(::GetLastError()));
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

[[nodiscard]] inline std::string from_wide(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) throw SystemException("invalid UTF-16 string", static_cast<int>(::GetLastError()));
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                          nullptr);
    return s;
}

[[nodiscard]] inline int last_error() noexcept { return static_cast<int>(::GetLastError()); }

/// Owning Win32 HANDLE.
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE h) noexcept : h_(h) {}
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.release();
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset() noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};
#else
/// Owning POSIX file descriptor.
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = o.release();
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};
#endif

}  // namespace flyg::system::detail
