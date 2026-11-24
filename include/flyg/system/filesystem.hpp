// flyG - filesystem helpers (std::filesystem with exception-based errors), temp files, mmap
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/system/info.hpp>
#include <flyg/system/native.hpp>

namespace flyg::system {

namespace fs = std::filesystem;

namespace detail {
[[noreturn]] inline void fs_fail(const std::string& what, const fs::path& p, const std::error_code& ec) {
    throw SystemException(what + " '" + p.string() + "'", ec.value());
}
}  // namespace detail

[[nodiscard]] inline bool exists(const fs::path& p) {
    std::error_code ec;
    const bool r = fs::exists(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
        detail::fs_fail("cannot stat", p, ec);
    return r;
}

[[nodiscard]] inline bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

[[nodiscard]] inline bool is_directory(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

[[nodiscard]] inline u64 file_size(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    if (ec) detail::fs_fail("cannot get size of", p, ec);
    return n;
}

[[nodiscard]] inline fs::file_time_type last_write_time(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) detail::fs_fail("cannot get modification time of", p, ec);
    return t;
}

[[nodiscard]] inline fs::path current_directory() {
    std::error_code ec;
    auto p = fs::current_path(ec);
    if (ec) detail::fs_fail("cannot get current directory", {}, ec);
    return p;
}

inline void set_current_directory(const fs::path& p) {
    std::error_code ec;
    fs::current_path(p, ec);
    if (ec) detail::fs_fail("cannot change directory to", p, ec);
}

[[nodiscard]] inline fs::path absolute_path(const fs::path& p) {
    std::error_code ec;
    auto r = fs::absolute(p, ec);
    if (ec) detail::fs_fail("cannot make absolute", p, ec);
    return r;
}

/// Absolute path with symlinks resolved; the path must exist.
[[nodiscard]] inline fs::path canonical_path(const fs::path& p) {
    std::error_code ec;
    auto r = fs::canonical(p, ec);
    if (ec) detail::fs_fail("cannot canonicalize", p, ec);
    return r;
}

/// Creates the directory and all missing parents; returns true if something was created.
inline bool create_directories(const fs::path& p) {
    std::error_code ec;
    const bool r = fs::create_directories(p, ec);
    if (ec) detail::fs_fail("cannot create directory", p, ec);
    return r;
}

/// Removes a file or an empty directory; returns false if it did not exist.
inline bool remove(const fs::path& p) {
    std::error_code ec;
    const bool r = fs::remove(p, ec);
    if (ec) detail::fs_fail("cannot remove", p, ec);
    return r;
}

/// Recursively removes; returns the number of entries removed.
inline u64 remove_all(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::remove_all(p, ec);
    if (ec) detail::fs_fail("cannot remove", p, ec);
    return static_cast<u64>(n);
}

inline void rename(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) detail::fs_fail("cannot rename '" + from.string() + "' to", to, ec);
}

/// Copies a file or (recursively) a directory tree, overwriting existing files.
inline void copy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) detail::fs_fail("cannot copy '" + from.string() + "' to", to, ec);
}

[[nodiscard]] inline std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) throw SystemException("cannot open for reading '" + p.string() + "'", errno ? errno : 2);
    const std::streamoff size = in.tellg();
    if (size < 0) throw SystemException("cannot determine size of '" + p.string() + "'", 0);
    std::vector<std::byte> data(static_cast<usize>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw SystemException("read failed '" + p.string() + "'", errno);
    return data;
}

[[nodiscard]] inline std::string read_text(const fs::path& p) {
    const auto bytes = read_file(p);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

namespace detail {
inline void write_impl(const fs::path& p, std::span<const std::byte> data, std::ios::openmode mode) {
    std::ofstream out(p, std::ios::binary | mode);
    if (!out) throw SystemException("cannot open for writing '" + p.string() + "'", errno ? errno : 2);
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw SystemException("write failed '" + p.string() + "'", errno);
}
}  // namespace detail

inline void write_file(const fs::path& p, std::span<const std::byte> data) {
    detail::write_impl(p, data, std::ios::trunc);
}
inline void write_text(const fs::path& p, std::string_view text) {
    write_file(p, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}
inline void append_file(const fs::path& p, std::span<const std::byte> data) {
    detail::write_impl(p, data, std::ios::app);
}
inline void append_text(const fs::path& p, std::string_view text) {
    append_file(p, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

/// Writes to a temporary sibling and renames over the target, so readers never see a partial file.
inline void write_file_atomic(const fs::path& p, std::span<const std::byte> data) {
    fs::path tmp = p;
    tmp += ".flyg-tmp-" + std::to_string(process_id()) + "-" + std::to_string(std::random_device{}());
    try {
        write_file(tmp, data);
        std::error_code ec;
        fs::rename(tmp, p, ec);  // replaces atomically on POSIX; MoveFileEx semantics on Windows
        if (ec) detail::fs_fail("cannot replace", p, ec);
    } catch (...) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        throw;
    }
}

struct DirEntry {
    fs::path path;
    bool is_directory = false;
    bool is_file = false;
    u64 size = 0;  ///< 0 for directories
};

/// Lists a directory (optionally recursively), sorted by path for deterministic results.
[[nodiscard]] inline std::vector<DirEntry> list_directory(const fs::path& dir, bool recursive = false) {
    std::vector<DirEntry> out;
    std::error_code ec;
    auto visit = [&](auto it) {
        const auto end = decltype(it){};
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const auto& e = *it;
            DirEntry d;
            d.path = e.path();
            std::error_code e2;
            d.is_directory = e.is_directory(e2);
            d.is_file = e.is_regular_file(e2);
            if (d.is_file) {
                const auto sz = e.file_size(e2);
                d.size = e2 ? 0 : static_cast<u64>(sz);
            }
            out.push_back(std::move(d));
        }
    };
    const auto opts = fs::directory_options::skip_permission_denied;
    if (recursive) visit(fs::recursive_directory_iterator(dir, opts, ec));
    else visit(fs::directory_iterator(dir, opts, ec));
    if (ec) detail::fs_fail("cannot list directory", dir, ec);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.path < b.path; });
    return out;
}

/// Shell-style wildcard match: `*` (any run, not crossing '/'), `?` (one char), `[abc]`, `[a-z]`,
/// `[!x]`. Matching is case sensitive.
[[nodiscard]] inline bool wildcard_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        bool matched = false;
        std::size_t next_p = p;
        if (p < pattern.size()) {
            const char c = pattern[p];
            if (c == '*') {
                star_p = ++p;
                star_t = t;
                continue;
            }
            if (c == '?' && text[t] != '/') {
                matched = true;
                next_p = p + 1;
            } else if (c == '[') {
                const auto close = pattern.find(']', p + 2);
                if (close != std::string_view::npos) {
                    std::size_t i = p + 1;
                    const bool neg = pattern[i] == '!' || pattern[i] == '^';
                    if (neg) ++i;
                    bool in = false;
                    while (i < close) {
                        if (i + 2 < close && pattern[i + 1] == '-') {
                            if (text[t] >= pattern[i] && text[t] <= pattern[i + 2]) in = true;
                            i += 3;
                        } else {
                            if (text[t] == pattern[i]) in = true;
                            ++i;
                        }
                    }
                    matched = in != neg;
                    next_p = close + 1;
                } else {
                    matched = text[t] == '[';
                    next_p = p + 1;
                }
            } else if (c == text[t]) {
                matched = true;
                next_p = p + 1;
            }
        }
        if (matched) {
            p = next_p;
            ++t;
        } else if (star_p != std::string_view::npos && text[star_t] != '/') {
            p = star_p;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

namespace detail {
inline std::vector<std::string> split_path(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= s.size()) {
        const auto j = s.find('/', i);
        const auto e = j == std::string_view::npos ? s.size() : j;
        if (e > i) out.emplace_back(s.substr(i, e - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}
inline bool glob_segments(const std::vector<std::string>& pat, std::size_t pi,
                          const std::vector<std::string>& path, std::size_t si) {
    if (pi == pat.size()) return si == path.size();
    if (pat[pi] == "**") {
        for (std::size_t k = si; k <= path.size(); ++k)
            if (glob_segments(pat, pi + 1, path, k)) return true;
        return false;
    }
    return si < path.size() && wildcard_match(pat[pi], path[si]) && glob_segments(pat, pi + 1, path, si + 1);
}
}  // namespace detail

/// Finds files under `dir` whose relative path (generic '/' form) matches `pattern`. A `**`
/// segment matches any number of directories (including none), e.g. "**/*.txt".
[[nodiscard]] inline std::vector<fs::path> glob(const fs::path& dir, std::string_view pattern) {
    std::vector<fs::path> out;
    const auto pat = detail::split_path(pattern);
    for (const auto& e : list_directory(dir, true)) {
        if (!e.is_file) continue;
        const auto rel = detail::split_path(e.path.lexically_relative(dir).generic_string());
        if (detail::glob_segments(pat, 0, rel, 0)) out.push_back(e.path);
    }
    return out;
}

[[nodiscard]] inline fs::path temp_directory() {
    std::error_code ec;
    auto p = fs::temp_directory_path(ec);
    if (ec) detail::fs_fail("cannot determine temp directory", {}, ec);
    return p;
}

namespace detail {
[[nodiscard]] inline std::string random_suffix() {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 10; ++i) s += alphabet[rd() % (sizeof alphabet - 1)];
    return s;
}
}  // namespace detail

/// A uniquely named directory that is deleted (recursively) on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "flyg-", const fs::path& parent = {}) {
        const fs::path base = parent.empty() ? temp_directory() : parent;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / (prefix + detail::random_suffix());
            std::error_code ec;
            if (fs::create_directory(path_, ec)) return;
            if (ec) detail::fs_fail("cannot create temp directory", path_, ec);
        }
        throw SystemException("cannot find an unused temp directory name", 0);
    }
    TempDir(TempDir&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempDir& operator=(TempDir&& o) noexcept {
        if (this != &o) {
            cleanup();
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() { cleanup(); }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] fs::path operator/(const fs::path& p) const { return path_ / p; }
    /// Keeps the directory on disk and returns its path.
    fs::path release() noexcept {
        fs::path p = std::move(path_);
        path_.clear();
        return p;
    }

private:
    void cleanup() noexcept {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec);
        path_.clear();
    }
    fs::path path_;
};

/// A uniquely named empty file that is deleted on destruction.
class TempFile {
public:
    explicit TempFile(const std::string& prefix = "flyg-", const std::string& extension = ".tmp",
                      const fs::path& parent = {}) {
        const fs::path base = parent.empty() ? temp_directory() : parent;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / (prefix + detail::random_suffix() + extension);
            if (!fs::exists(path_)) {
                std::ofstream f(path_, std::ios::binary | std::ios::trunc);
                if (!f) throw SystemException("cannot create temp file '" + path_.string() + "'", errno);
                return;
            }
        }
        throw SystemException("cannot find an unused temp file name", 0);
    }
    TempFile(TempFile&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempFile& operator=(TempFile&& o) noexcept {
        if (this != &o) {
            cleanup();
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() { cleanup(); }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    void cleanup() noexcept {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove(path_, ec);
        path_.clear();
    }
    fs::path path_;
};

/// Memory-mapped file. Read-only maps are shared views; read-write maps write through to the file
/// (call flush() to force it to disk).
class MappedFile {
public:
    enum class Access { read_only, read_write };

    MappedFile() noexcept = default;
    explicit MappedFile(const fs::path& path, Access access = Access::read_only) { open(path, access); }
    MappedFile(MappedFile&& o) noexcept { swap(o); }
    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            close();
            swap(o);
        }
        return *this;
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() { close(); }

    void open(const fs::path& path, Access access = Access::read_only) {
        close();
        const bool rw = access == Access::read_write;
#if FLYG_PLATFORM_LINUX
        detail::Fd fd(::open(path.c_str(), (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC));
        if (!fd.valid()) throw SystemException::from_errno("cannot open '" + path.string() + "' for mapping");
        struct stat st {};
        if (::fstat(fd.get(), &st) != 0) throw SystemException::from_errno("fstat failed");
        size_ = static_cast<usize>(st.st_size);
        if (size_ == 0) return;  // empty files cannot be mapped; behave as an empty view
        void* p = ::mmap(nullptr, size_, rw ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED) {
            size_ = 0;
            throw SystemException::from_errno("mmap failed");
        }
        data_ = static_cast<std::byte*>(p);
#else
        detail::Handle file(::CreateFileW(path.c_str(), rw ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) throw SystemException("cannot open '" + path.string() + "' for mapping", detail::last_error());
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(file.get(), &sz)) throw SystemException("GetFileSizeEx failed", detail::last_error());
        size_ = static_cast<usize>(sz.QuadPart);
        if (size_ == 0) return;
        detail::Handle mapping(::CreateFileMappingW(file.get(), nullptr, rw ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr));
        if (!mapping.valid()) {
            size_ = 0;
            throw SystemException("CreateFileMapping failed", detail::last_error());
        }
        void* p = ::MapViewOfFile(mapping.get(), rw ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
        if (!p) {
            size_ = 0;
            throw SystemException("MapViewOfFile failed", detail::last_error());
        }
        data_ = static_cast<std::byte*>(p);
#endif
        writable_ = rw;
    }

    void close() noexcept {
        if (data_) {
#if FLYG_PLATFORM_LINUX
            ::munmap(data_, size_);
#else
            ::UnmapViewOfFile(data_);
#endif
        }
        data_ = nullptr;
        size_ = 0;
        writable_ = false;
    }

    /// Forces modified pages to disk (no-op for read-only maps).
    void flush() {
        if (!data_ || !writable_) return;
#if FLYG_PLATFORM_LINUX
        if (::msync(data_, size_, MS_SYNC) != 0) throw SystemException::from_errno("msync failed");
#else
        if (!::FlushViewOfFile(data_, size_)) throw SystemException("FlushViewOfFile failed", detail::last_error());
#endif
    }

    [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::byte* writable_data() {
        if (!writable_) throw SystemException("mapping is read-only", 0);
        return data_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<std::byte> writable_bytes() {
        if (!writable_) throw SystemException("mapping is read-only", 0);
        return {data_, size_};
    }

private:
    void swap(MappedFile& o) noexcept {
        std::swap(data_, o.data_);
        std::swap(size_, o.size_);
        std::swap(writable_, o.writable_);
    }
    std::byte* data_ = nullptr;
    usize size_ = 0;
    bool writable_ = false;
};

}  // namespace flyg::system
