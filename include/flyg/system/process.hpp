// flyG - child processes with pipes, timeouts and output capture
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

#include <flyg/core/util.hpp>

#include <flyg/system/env.hpp>
#include <flyg/system/native.hpp>

namespace flyg::system {

struct ProcessOptions {
    std::vector<std::string> args;                ///< arguments after the program name
    std::filesystem::path working_directory;      ///< empty = inherit
    std::map<std::string, std::string> env;       ///< added to / overriding the inherited environment
    bool clear_env = false;                       ///< start from an empty environment instead
    bool pipe_stdin = false;                      ///< child's stdin is a pipe (write_stdin)
    bool pipe_stdout = false;                     ///< child's stdout is a pipe (read_stdout)
    bool pipe_stderr = false;                     ///< child's stderr is a pipe (read_stderr)
    bool merge_stderr = false;                    ///< send stderr into the stdout pipe (implies pipe_stdout)
};

struct ProcessResult {
    int exit_code = -1;      ///< exit status; on Linux 128+signal when killed by a signal
    bool signaled = false;   ///< terminated by a signal (Linux) / killed by us (timeout)
    bool timed_out = false;  ///< `run` gave up waiting and killed the process
    std::string out;         ///< captured stdout (when requested)
    std::string err;         ///< captured stderr (when requested)
    [[nodiscard]] bool success() const noexcept { return exit_code == 0 && !timed_out; }
};

namespace detail {
#if FLYG_PLATFORM_WINDOWS
inline void append_quoted_arg(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') ++i, ++bs;
        if (i == arg.size()) {
            cmd.append(bs * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') cmd.append(bs * 2 + 1, L'\\');
        else cmd.append(bs, L'\\');
        cmd += arg[i];
    }
    cmd += L'"';
}
#endif
}  // namespace detail

/// A running (or finished) child process. Move-only. The destructor kills a child that is still
/// running and reaps it, so no zombie is ever left behind.
class Process {
public:
    Process() noexcept = default;
    Process(Process&&) noexcept = default;
    Process& operator=(Process&& o) noexcept {
        if (this != &o) {
            shutdown();
            move_from(std::move(o));
        }
        return *this;
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ~Process() { shutdown(); }

    /// Starts `program` (looked up in PATH when it has no directory part). Throws SystemException
    /// if the program cannot be started.
    [[nodiscard]] static Process spawn(const std::string& program, const ProcessOptions& opt = {}) {
        Process p;
        p.start(program, opt);
        return p;
    }

    [[nodiscard]] bool valid() const noexcept { return started_; }
    [[nodiscard]] u32 pid() const noexcept { return pid_; }

    /// True while the child has not exited (does not block).
    [[nodiscard]] bool running() { return started_ && !try_collect(); }

    /// Waits for exit; returns the exit code. Throws SystemException on timeout.
    int wait() {
        require_started();
        while (!collected_) block_collect();
        return exit_code_;
    }

    /// Waits up to `timeout`; returns the exit code, or nullopt when it is still running.
    template <class Rep, class Period>
    [[nodiscard]] std::optional<int> wait_for(std::chrono::duration<Rep, Period> timeout) {
        require_started();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!try_collect()) {
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return exit_code_;
    }

    [[nodiscard]] bool signaled() const noexcept { return signaled_; }

    /// Forcefully ends the process (SIGKILL / TerminateProcess).
    void kill() {
        require_started();
        if (collected_) return;
#if FLYG_PLATFORM_LINUX
        if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0 && errno != ESRCH)
            throw SystemException::from_errno("kill failed");
#else
        if (!::TerminateProcess(process_.get(), 1) && ::GetLastError() != ERROR_ACCESS_DENIED)
            throw SystemException("TerminateProcess failed", detail::last_error());
#endif
        killed_ = true;
    }

#if FLYG_PLATFORM_LINUX
    /// Asks the process to end (SIGTERM). Not available on Windows, where it is equal to kill().
    void terminate() {
        require_started();
        if (collected_) return;
        if (::kill(static_cast<pid_t>(pid_), SIGTERM) != 0 && errno != ESRCH)
            throw SystemException::from_errno("kill(SIGTERM) failed");
    }
#else
    void terminate() { kill(); }
#endif

    /// Writes to the child's stdin pipe; returns bytes written. Throws on error (including a
    /// closed pipe).
    usize write_stdin(std::span<const std::byte> data) {
        if (!in_valid()) throw SystemException("process stdin is not piped", 0);
        usize done = 0;
        while (done < data.size()) {
#if FLYG_PLATFORM_LINUX
            const ssize_t n = ::write(in_.get(), data.data() + done, data.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw SystemException::from_errno("write to process stdin failed");
            }
            done += static_cast<usize>(n);
#else
            DWORD n = 0;
            if (!::WriteFile(in_.get(), data.data() + done, static_cast<DWORD>(std::min<usize>(data.size() - done, 1u << 20)), &n, nullptr))
                throw SystemException("write to process stdin failed", detail::last_error());
            done += n;
#endif
        }
        return done;
    }
    usize write_stdin(std::string_view text) {
        return write_stdin(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    }
    /// Signals end of input to the child.
    void close_stdin() noexcept { in_.reset(); }

    /// Blocking read of up to `buf.size()` bytes; 0 means end of stream.
    usize read_stdout(std::span<std::byte> buf) { return read_pipe(out_, buf, "stdout"); }
    usize read_stderr(std::span<std::byte> buf) { return read_pipe(err_, buf, "stderr"); }

    /// Reads until end of stream.
    [[nodiscard]] std::string read_all_stdout() { return read_all(out_, "stdout"); }
    [[nodiscard]] std::string read_all_stderr() { return read_all(err_, "stderr"); }

private:
#if FLYG_PLATFORM_LINUX
    using PipeEnd = detail::Fd;
#else
    using PipeEnd = detail::Handle;
#endif

    bool in_valid() const noexcept { return in_.valid(); }

    void require_started() const {
        if (!started_) throw SystemException("process was not started", 0);
    }

    void move_from(Process&& o) noexcept {
        started_ = std::exchange(o.started_, false);
        collected_ = o.collected_;
        killed_ = o.killed_;
        signaled_ = o.signaled_;
        exit_code_ = o.exit_code_;
        pid_ = o.pid_;
        in_ = std::move(o.in_);
        out_ = std::move(o.out_);
        err_ = std::move(o.err_);
#if FLYG_PLATFORM_WINDOWS
        process_ = std::move(o.process_);
#endif
    }

    void shutdown() noexcept {
        if (!started_) return;
        in_.reset();
        try {
            if (!collected_ && !try_collect()) {
                kill();
                block_collect();
            }
        } catch (...) {
        }
        out_.reset();
        err_.reset();
        started_ = false;
    }

    usize read_pipe(PipeEnd& p, std::span<std::byte> buf, const char* what) {
        if (!p.valid()) throw SystemException(std::string("process ") + what + " is not piped", 0);
        if (buf.empty()) return 0;
#if FLYG_PLATFORM_LINUX
        for (;;) {
            const ssize_t n = ::read(p.get(), buf.data(), buf.size());
            if (n >= 0) return static_cast<usize>(n);
            if (errno == EINTR) continue;
            throw SystemException::from_errno(std::string("read from process ") + what + " failed");
        }
#else
        DWORD n = 0;
        if (!::ReadFile(p.get(), buf.data(), static_cast<DWORD>(std::min<usize>(buf.size(), 1u << 20)), &n, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE) return 0;
            throw SystemException(std::string("read from process ") + what + " failed", detail::last_error());
        }
        return n;
#endif
    }

    std::string read_all(PipeEnd& p, const char* what) {
        std::string s;
        std::byte buf[8192];
        for (;;) {
            const usize n = read_pipe(p, buf, what);
            if (n == 0) break;
            s.append(reinterpret_cast<const char*>(buf), n);
        }
        return s;
    }

#if FLYG_PLATFORM_LINUX
    bool try_collect() {
        if (collected_) return true;
        int status = 0;
        pid_t r;
        do r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        while (r < 0 && errno == EINTR);
        if (r == 0) return false;
        if (r < 0) throw SystemException::from_errno("waitpid failed");
        record(status);
        return true;
    }
    void block_collect() {
        int status = 0;
        pid_t r;
        do r = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
        while (r < 0 && errno == EINTR);
        if (r < 0) throw SystemException::from_errno("waitpid failed");
        record(status);
    }
    void record(int status) {
        collected_ = true;
        if (WIFEXITED(status)) {
            exit_code_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code_ = 128 + WTERMSIG(status);
            signaled_ = true;
        }
    }

    void start(const std::string& program, const ProcessOptions& opt) {
        if (program.empty()) throw SystemException("Process: empty program name", 0);
        const bool want_out = opt.pipe_stdout || opt.merge_stderr;
        const bool want_err = opt.pipe_stderr && !opt.merge_stderr;

        auto make_pipe = [](detail::Fd& r, detail::Fd& w) {
            int fds[2];
            if (::pipe2(fds, O_CLOEXEC) != 0) throw SystemException::from_errno("pipe2 failed");
            r = detail::Fd(fds[0]);
            w = detail::Fd(fds[1]);
        };
        detail::Fd in_r, in_w, out_r, out_w, err_r, err_w;
        if (opt.pipe_stdin) make_pipe(in_r, in_w);
        if (want_out) make_pipe(out_r, out_w);
        if (want_err) make_pipe(err_r, err_w);

        posix_spawn_file_actions_t fa;
        if (int rc = ::posix_spawn_file_actions_init(&fa); rc != 0) throw SystemException("posix_spawn_file_actions_init failed", rc);
        auto cleanup = core::scope_exit([&] { ::posix_spawn_file_actions_destroy(&fa); });
        auto check = [](int rc, const char* what) {
            if (rc != 0) throw SystemException(what, rc);
        };
        if (opt.pipe_stdin) check(::posix_spawn_file_actions_adddup2(&fa, in_r.get(), 0), "adddup2(stdin)");
        if (want_out) check(::posix_spawn_file_actions_adddup2(&fa, out_w.get(), 1), "adddup2(stdout)");
        if (want_err) check(::posix_spawn_file_actions_adddup2(&fa, err_w.get(), 2), "adddup2(stderr)");
        if (opt.merge_stderr) check(::posix_spawn_file_actions_adddup2(&fa, out_w.get(), 2), "adddup2(stderr)");
        if (!opt.working_directory.empty())
            check(::posix_spawn_file_actions_addchdir_np(&fa, opt.working_directory.c_str()), "addchdir");

        std::vector<std::string> argv_s;
        argv_s.push_back(program);
        argv_s.insert(argv_s.end(), opt.args.begin(), opt.args.end());
        std::vector<char*> argv;
        for (auto& a : argv_s) argv.push_back(a.data());
        argv.push_back(nullptr);

        std::map<std::string, std::string> env = opt.clear_env ? std::map<std::string, std::string>{} : env_vars();
        for (const auto& [k, v] : opt.env) env[k] = v;
        std::vector<std::string> env_s;
        for (const auto& [k, v] : env) env_s.push_back(k + "=" + v);
        std::vector<char*> envp;
        for (auto& e : env_s) envp.push_back(e.data());
        envp.push_back(nullptr);

        // A relative program with a directory part (./x) is resolved by the child's cwd, but
        // posix_spawn resolves it against ours; make it absolute when a cwd is requested.
        pid_t pid = 0;
        const int rc = ::posix_spawnp(&pid, program.c_str(), &fa, nullptr, argv.data(), envp.data());
        if (rc != 0) throw SystemException("cannot start '" + program + "'", rc);

        pid_ = static_cast<u32>(pid);
        started_ = true;
        if (opt.pipe_stdin) in_ = std::move(in_w);
        if (want_out) out_ = std::move(out_r);
        if (want_err) err_ = std::move(err_r);
        // The child's ends close here as the local Fd objects go out of scope.
    }
#else
    bool try_collect() {
        if (collected_) return true;
        const DWORD r = ::WaitForSingleObject(process_.get(), 0);
        if (r == WAIT_TIMEOUT) return false;
        if (r != WAIT_OBJECT_0) throw SystemException("WaitForSingleObject failed", detail::last_error());
        record();
        return true;
    }
    void block_collect() {
        if (::WaitForSingleObject(process_.get(), INFINITE) != WAIT_OBJECT_0)
            throw SystemException("WaitForSingleObject failed", detail::last_error());
        record();
    }
    void record() {
        DWORD code = 0;
        if (!::GetExitCodeProcess(process_.get(), &code)) throw SystemException("GetExitCodeProcess failed", detail::last_error());
        collected_ = true;
        exit_code_ = static_cast<int>(code);
        signaled_ = killed_;
    }

    void start(const std::string& program, const ProcessOptions& opt) {
        if (program.empty()) throw SystemException("Process: empty program name", 0);
        const bool want_out = opt.pipe_stdout || opt.merge_stderr;
        const bool want_err = opt.pipe_stderr && !opt.merge_stderr;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        // pipe(parent_end, child_end): the parent's end must not be inherited.
        auto make_pipe = [&](detail::Handle& parent, detail::Handle& child, bool parent_reads) {
            HANDLE r = nullptr, w = nullptr;
            if (!::CreatePipe(&r, &w, &sa, 0)) throw SystemException("CreatePipe failed", detail::last_error());
            detail::Handle hr(r), hw(w);
            detail::Handle& mine = parent_reads ? hr : hw;
            if (!::SetHandleInformation(mine.get(), HANDLE_FLAG_INHERIT, 0))
                throw SystemException("SetHandleInformation failed", detail::last_error());
            parent = parent_reads ? std::move(hr) : std::move(hw);
            child = parent_reads ? std::move(hw) : std::move(hr);
        };
        detail::Handle child_in, child_out, child_err;
        if (opt.pipe_stdin) make_pipe(in_, child_in, false);
        if (want_out) make_pipe(out_, child_out, true);
        if (want_err) make_pipe(err_, child_err, true);

        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = opt.pipe_stdin ? child_in.get() : ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = want_out ? child_out.get() : ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = opt.merge_stderr ? child_out.get() : want_err ? child_err.get() : ::GetStdHandle(STD_ERROR_HANDLE);

        std::wstring cmd;
        detail::append_quoted_arg(cmd, detail::to_wide(program));
        for (const auto& a : opt.args) {
            cmd += L' ';
            detail::append_quoted_arg(cmd, detail::to_wide(a));
        }

        std::map<std::string, std::string> env = opt.clear_env ? std::map<std::string, std::string>{} : env_vars();
        for (const auto& [k, v] : opt.env) env[k] = v;
        std::wstring block;
        for (const auto& [k, v] : env) block += detail::to_wide(k + "=" + v) + L'\0';
        block += L'\0';
        if (env.empty()) block += L'\0';

        const std::wstring cwd = opt.working_directory.empty() ? std::wstring{} : opt.working_directory.wstring();
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                              block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
            throw SystemException("cannot start '" + program + "'", detail::last_error());
        ::CloseHandle(pi.hThread);
        process_ = detail::Handle(pi.hProcess);
        pid_ = pi.dwProcessId;
        started_ = true;
    }
#endif

    bool started_ = false;
    bool collected_ = false;
    bool killed_ = false;
    bool signaled_ = false;
    int exit_code_ = -1;
    u32 pid_ = 0;
    PipeEnd in_, out_, err_;
#if FLYG_PLATFORM_WINDOWS
    detail::Handle process_;
#endif
};

/// Runs a program to completion, capturing stdout/stderr and optionally feeding stdin. When
/// `timeout` is non-zero and expires, the process is killed and `timed_out` is set.
[[nodiscard]] inline ProcessResult run(const std::string& program, std::vector<std::string> args = {},
                                       std::string_view stdin_data = {},
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
                                       ProcessOptions opt = {}) {
    opt.args = std::move(args);
    opt.pipe_stdout = true;
    opt.pipe_stderr = !opt.merge_stderr;
    opt.pipe_stdin = true;
    Process p = Process::spawn(program, opt);

    ProcessResult res;
    std::thread out_t([&] {
        try {
            res.out = p.read_all_stdout();
        } catch (...) {
        }
    });
    std::thread err_t;
    if (opt.pipe_stderr) {
        err_t = std::thread([&] {
            try {
                res.err = p.read_all_stderr();
            } catch (...) {
            }
        });
    }
    std::thread in_t([&] {
        try {
            if (!stdin_data.empty()) p.write_stdin(stdin_data);
        } catch (...) {  // child may exit without reading everything
        }
        p.close_stdin();
    });

    if (timeout > std::chrono::milliseconds::zero()) {
        if (!p.wait_for(timeout)) {
            res.timed_out = true;
            p.kill();
        }
    }
    res.exit_code = p.wait();
    res.signaled = p.signaled();
    // close_stdin may race with a blocked writer only if the child is gone: the write fails then.
    in_t.join();
    out_t.join();
    if (err_t.joinable()) err_t.join();
    return res;
}

}  // namespace flyg::system
