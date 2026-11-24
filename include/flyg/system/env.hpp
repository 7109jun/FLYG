// flyG - environment variables and command line
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/system/native.hpp>

namespace flyg::system {

/// Value of an environment variable (UTF-8), or nullopt when it is not set.
[[nodiscard]] inline std::optional<std::string> get_env(std::string_view name) {
    if (name.empty()) return std::nullopt;
#if FLYG_PLATFORM_WINDOWS
    const std::wstring wname = detail::to_wide(name);
    DWORD n = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) {
        if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string{};  // set but empty
    }
    std::wstring buf(n, L'\0');
    n = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return detail::from_wide(buf);
#else
    const std::string n(name);
    const char* v = std::getenv(n.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

[[nodiscard]] inline std::string get_env_or(std::string_view name, std::string fallback) {
    auto v = get_env(name);
    return v ? std::move(*v) : std::move(fallback);
}

[[nodiscard]] inline bool has_env(std::string_view name) { return get_env(name).has_value(); }

inline void set_env(std::string_view name, std::string_view value) {
    if (name.empty() || name.find('=') != std::string_view::npos)
        throw SystemException("set_env: invalid variable name", 0);
#if FLYG_PLATFORM_WINDOWS
    if (!::SetEnvironmentVariableW(detail::to_wide(name).c_str(), detail::to_wide(value).c_str()))
        throw SystemException("set_env failed", detail::last_error());
#else
    if (::setenv(std::string(name).c_str(), std::string(value).c_str(), 1) != 0)
        throw SystemException::from_errno("set_env failed");
#endif
}

inline void unset_env(std::string_view name) {
    if (name.empty() || name.find('=') != std::string_view::npos)
        throw SystemException("unset_env: invalid variable name", 0);
#if FLYG_PLATFORM_WINDOWS
    if (!::SetEnvironmentVariableW(detail::to_wide(name).c_str(), nullptr)) {
        if (::GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            throw SystemException("unset_env failed", detail::last_error());
    }
#else
    if (::unsetenv(std::string(name).c_str()) != 0) throw SystemException::from_errno("unset_env failed");
#endif
}

/// Snapshot of the whole environment. On Windows, pseudo-variables such as "=C:" are skipped.
[[nodiscard]] inline std::map<std::string, std::string> env_vars() {
    std::map<std::string, std::string> out;
#if FLYG_PLATFORM_WINDOWS
    wchar_t* block = ::GetEnvironmentStringsW();
    if (!block) throw SystemException("GetEnvironmentStrings failed", detail::last_error());
    for (const wchar_t* p = block; *p;) {
        const std::wstring_view entry(p);
        p += entry.size() + 1;
        const auto eq = entry.find(L'=', 1);  // skip a leading '=' of "=C:"
        if (entry[0] == L'=' || eq == std::wstring_view::npos) continue;
        out[detail::from_wide(entry.substr(0, eq))] = detail::from_wide(entry.substr(eq + 1));
    }
    ::FreeEnvironmentStringsW(block);
#else
    for (char** e = ::environ; e && *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        out[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
    }
#endif
    return out;
}

/// Replaces `${NAME}` and `$NAME` (and `%NAME%` on Windows) by environment values; unknown names
/// expand to the empty string. `$$` yields a literal `$`.
[[nodiscard]] inline std::string expand_env(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    auto is_name_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '$') {
            if (i + 1 < text.size() && text[i + 1] == '$') {
                out += '$';
                i += 2;
            } else if (i + 1 < text.size() && text[i + 1] == '{') {
                const auto close = text.find('}', i + 2);
                if (close == std::string_view::npos)
                    throw SystemException("expand_env: unterminated ${", 0);
                out += get_env_or(text.substr(i + 2, close - i - 2), "");
                i = close + 1;
            } else {
                std::size_t j = i + 1;
                while (j < text.size() && is_name_char(text[j])) ++j;
                if (j == i + 1) {
                    out += '$';
                    ++i;
                } else {
                    out += get_env_or(text.substr(i + 1, j - i - 1), "");
                    i = j;
                }
            }
        } else if (FLYG_PLATFORM_WINDOWS && c == '%') {
            const auto close = text.find('%', i + 1);
            if (close == std::string_view::npos || close == i + 1) {
                out += c;
                ++i;
            } else {
                out += get_env_or(text.substr(i + 1, close - i - 1), "");
                i = close + 1;
            }
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

/// Sets a variable for the lifetime of the object and restores the previous state afterwards.
class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string_view value) : name_(std::move(name)), old_(get_env(name_)) {
        set_env(name_, value);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ~ScopedEnv() {
        try {
            if (old_) set_env(name_, *old_);
            else unset_env(name_);
        } catch (...) {
        }
    }

private:
    std::string name_;
    std::optional<std::string> old_;
};

/// Process command line arguments (UTF-8), excluding nothing: element 0 is the program.
[[nodiscard]] inline std::vector<std::string> command_line() {
    std::vector<std::string> args;
#if FLYG_PLATFORM_WINDOWS
    // CommandLineToArgvW lives in shell32; parse manually following the MSVC rules instead.
    const std::wstring cl = ::GetCommandLineW();
    std::wstring cur;
    bool in_arg = false, quoted = false;
    for (std::size_t i = 0; i <= cl.size(); ++i) {
        const wchar_t c = i < cl.size() ? cl[i] : L'\0';
        if (c == L'\\') {
            std::size_t bs = 0;
            while (i < cl.size() && cl[i] == L'\\') ++bs, ++i;
            if (i < cl.size() && cl[i] == L'"') {
                cur.append(bs / 2, L'\\');
                if (bs % 2) cur += L'"';
                else quoted = !quoted;
            } else {
                cur.append(bs, L'\\');
                --i;
                in_arg = true;
                continue;
            }
            in_arg = true;
        } else if (c == L'"') {
            quoted = !quoted;
            in_arg = true;
        } else if ((c == L' ' || c == L'\t' || c == L'\0') && !quoted) {
            if (in_arg) args.push_back(detail::from_wide(cur));
            cur.clear();
            in_arg = false;
        } else {
            cur += c;
            in_arg = true;
        }
    }
#else
    std::FILE* f = std::fopen("/proc/self/cmdline", "rb");
    if (!f) throw SystemException::from_errno("cannot read /proc/self/cmdline");
    std::string data;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    std::fclose(f);
    for (std::size_t i = 0; i < data.size();) {
        const std::size_t e = data.find('\0', i);
        const std::size_t end = e == std::string::npos ? data.size() : e;
        args.emplace_back(data.substr(i, end - i));
        i = end + 1;
    }
#endif
    return args;
}

}  // namespace flyg::system
