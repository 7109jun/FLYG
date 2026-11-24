// flyG - CPU, memory and operating system information
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <flyg/core/cpu_features.hpp>
#include <flyg/core/util.hpp>
#include <flyg/system/env.hpp>
#include <flyg/system/native.hpp>

#if FLYG_PLATFORM_WINDOWS
#  include <lmcons.h>
#endif

namespace flyg::system {

// ---------------------------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------------------------
struct CpuInfo {
    std::string vendor;       ///< "GenuineIntel", "AuthenticAMD", ... (may be empty on ARM)
    std::string brand;        ///< marketing name, best effort
    u32 logical_cores = 0;    ///< hardware threads visible to the process
    u32 physical_cores = 0;   ///< physical cores (== logical_cores when unknown)
    std::string architecture; ///< "x86_64", "arm64", ...
    core::CpuFeatures features;
};

namespace detail {

[[nodiscard]] inline std::string read_small_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] inline std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// "key : value" lookup in /proc style files; returns the first match.
[[nodiscard]] inline std::string proc_value(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return trim(line.substr(colon + 1));
        }
    }
    return {};
}
}  // namespace detail

[[nodiscard]] inline CpuInfo cpu_info() {
    CpuInfo info;
    info.features = core::cpu_features();
    info.vendor = info.features.vendor;
    info.brand = info.features.brand;
    info.architecture = std::string(core::to_string(core::arch));
    info.logical_cores = std::max(1u, std::thread::hardware_concurrency());
    info.physical_cores = info.logical_cores;

#if FLYG_PLATFORM_LINUX
    const std::string cpuinfo = detail::read_small_file("/proc/cpuinfo");
    if (info.brand.empty()) {
        info.brand = detail::proc_value(cpuinfo, "model name");
        if (info.brand.empty()) info.brand = detail::proc_value(cpuinfo, "Hardware");
    }
    if (info.vendor.empty()) info.vendor = detail::proc_value(cpuinfo, "vendor_id");

    // Physical cores: distinct (package, core) pairs from sysfs topology.
    std::set<std::pair<std::string, std::string>> cores;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/devices/system/cpu", ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 || name.find_first_not_of("0123456789", 3) != std::string::npos)
            continue;
        const auto topo = e.path() / "topology";
        const std::string core_id = detail::trim(detail::read_small_file(topo / "core_id"));
        if (core_id.empty()) continue;
        cores.emplace(detail::trim(detail::read_small_file(topo / "physical_package_id")), core_id);
    }
    if (!cores.empty()) info.physical_cores = static_cast<u32>(std::min<std::size_t>(cores.size(), info.logical_cores));
#else
    DWORD len = 0;
    ::GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (::GetLogicalProcessorInformation(buf.data(), &len)) {
            u32 n = 0;
            for (const auto& b : buf)
                if (b.Relationship == RelationProcessorCore) ++n;
            if (n > 0) info.physical_cores = std::min(n, info.logical_cores);
        }
    }
    if (info.brand.empty()) {
        HKEY key;
        if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                            KEY_READ, &key) == ERROR_SUCCESS) {
            char b[256];
            DWORD sz = sizeof b;
            if (::RegQueryValueExA(key, "ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(b), &sz) == ERROR_SUCCESS)
                info.brand = detail::trim(std::string(b, strnlen(b, sizeof b)));
            ::RegCloseKey(key);
        }
    }
#endif
    return info;
}

// ---------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------
struct MemoryInfo {
    u64 total_physical = 0;      ///< bytes
    u64 available_physical = 0;  ///< bytes that can be allocated without swapping
    u64 total_swap = 0;          ///< page file / swap size
    u64 available_swap = 0;
    [[nodiscard]] u64 used_physical() const noexcept {
        return total_physical > available_physical ? total_physical - available_physical : 0;
    }
};

[[nodiscard]] inline MemoryInfo memory_info() {
    MemoryInfo m;
#if FLYG_PLATFORM_LINUX
    const std::string text = detail::read_small_file("/proc/meminfo");
    auto kib = [&](const char* key) -> u64 {
        const std::string v = detail::proc_value(text, key);
        return v.empty() ? 0 : static_cast<u64>(std::strtoull(v.c_str(), nullptr, 10)) * 1024;
    };
    m.total_physical = kib("MemTotal");
    m.available_physical = kib("MemAvailable");
    if (m.available_physical == 0) m.available_physical = kib("MemFree") + kib("Cached");
    m.total_swap = kib("SwapTotal");
    m.available_swap = kib("SwapFree");
    if (m.total_physical == 0) {  // /proc not mounted: fall back to sysinfo(2)
        struct sysinfo si {};
        if (::sysinfo(&si) != 0) throw SystemException::from_errno("sysinfo failed");
        m.total_physical = static_cast<u64>(si.totalram) * si.mem_unit;
        m.available_physical = static_cast<u64>(si.freeram) * si.mem_unit;
        m.total_swap = static_cast<u64>(si.totalswap) * si.mem_unit;
        m.available_swap = static_cast<u64>(si.freeswap) * si.mem_unit;
    }
#else
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (!::GlobalMemoryStatusEx(&ms)) throw SystemException("GlobalMemoryStatusEx failed", detail::last_error());
    m.total_physical = ms.ullTotalPhys;
    m.available_physical = ms.ullAvailPhys;
    m.total_swap = ms.ullTotalPageFile > ms.ullTotalPhys ? ms.ullTotalPageFile - ms.ullTotalPhys : 0;
    m.available_swap = ms.ullAvailPageFile > ms.ullAvailPhys ? ms.ullAvailPageFile - ms.ullAvailPhys : 0;
#endif
    return m;
}

/// Memory page size in bytes.
[[nodiscard]] inline usize page_size() {
#if FLYG_PLATFORM_LINUX
    const long p = ::sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<usize>(p) : 4096;
#else
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return si.dwPageSize;
#endif
}

/// Resident set size of the current process in bytes.
[[nodiscard]] inline u64 process_resident_bytes() {
#if FLYG_PLATFORM_LINUX
    const std::string text = detail::read_small_file("/proc/self/statm");
    std::istringstream in(text);
    u64 size = 0, resident = 0;
    if (!(in >> size >> resident)) throw SystemException("cannot read /proc/self/statm", 0);
    return resident * page_size();
#else
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    // GetProcessMemoryInfo lives in psapi/kernel32 (K32 variant is always exported).
    if (!::K32GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof pmc))
        throw SystemException("GetProcessMemoryInfo failed", detail::last_error());
    return pmc.WorkingSetSize;
#endif
}

// ---------------------------------------------------------------------------------------------
// Operating system
// ---------------------------------------------------------------------------------------------
struct OsInfo {
    std::string name;          ///< "Ubuntu 24.04 LTS", "Windows 11", ...
    std::string version;       ///< distribution/OS version, e.g. "24.04" or "10.0.22631"
    std::string kernel;        ///< kernel release / build
    std::string architecture;  ///< as reported by the OS
    std::string hostname;
    std::string username;
};

[[nodiscard]] inline std::string host_name() {
#if FLYG_PLATFORM_LINUX
    char buf[256]{};
    if (::gethostname(buf, sizeof buf - 1) != 0) throw SystemException::from_errno("gethostname failed");
    return buf;
#else
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!::GetComputerNameW(buf, &n)) throw SystemException("GetComputerName failed", detail::last_error());
    return detail::from_wide(std::wstring_view(buf, n));
#endif
}

[[nodiscard]] inline std::string user_name() {
#if FLYG_PLATFORM_LINUX
    if (auto u = get_env("USER"); u && !u->empty()) return *u;
    if (auto u = get_env("LOGNAME"); u && !u->empty()) return *u;
    const std::string passwd = detail::read_small_file("/etc/passwd");
    std::istringstream in(passwd);
    std::string line;
    const std::string uid = std::to_string(::geteuid());
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t s = 0;
        for (std::size_t i = 0; i <= line.size(); ++i)
            if (i == line.size() || line[i] == ':') f.push_back(line.substr(s, i - s)), s = i + 1;
        if (f.size() > 2 && f[2] == uid) return f[0];
    }
    return uid;
#else
    wchar_t buf[UNLEN + 1];
    DWORD n = UNLEN + 1;
    if (!::GetUserNameW(buf, &n)) throw SystemException("GetUserName failed", detail::last_error());
    return detail::from_wide(std::wstring_view(buf, n > 0 ? n - 1 : 0));
#endif
}

[[nodiscard]] inline OsInfo os_info() {
    OsInfo o;
    o.hostname = host_name();
    o.username = user_name();
#if FLYG_PLATFORM_LINUX
    struct utsname u {};
    if (::uname(&u) != 0) throw SystemException::from_errno("uname failed");
    o.kernel = u.release;
    o.architecture = u.machine;
    o.name = "Linux";
    std::string rel = detail::read_small_file("/etc/os-release");
    if (rel.empty()) rel = detail::read_small_file("/usr/lib/os-release");
    auto field = [&](const char* key) {
        std::istringstream in(rel);
        std::string line;
        const std::string prefix = std::string(key) + "=";
        while (std::getline(in, line)) {
            if (line.compare(0, prefix.size(), prefix) == 0) {
                std::string v = detail::trim(line.substr(prefix.size()));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
                    v = v.substr(1, v.size() - 2);
                return v;
            }
        }
        return std::string{};
    };
    if (auto n = field("PRETTY_NAME"); !n.empty()) o.name = n;
    else if (auto n2 = field("NAME"); !n2.empty()) o.name = n2;
    o.version = field("VERSION_ID");
    if (o.version.empty()) o.version = o.kernel;
#else
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    bool have = false;
    if (HMODULE nt = ::GetModuleHandleW(L"ntdll.dll")) {
        // Cast through void* to silence the function-pointer cast warning.
        auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
        have = fn && fn(&vi) == 0;
    }
    o.name = "Windows";
    if (have) {
        if (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000) o.name = "Windows 11";
        else if (vi.dwMajorVersion == 10) o.name = "Windows 10";
        o.version = std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) + "." +
                    std::to_string(vi.dwBuildNumber);
        o.kernel = "NT " + o.version;
    }
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: o.architecture = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: o.architecture = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: o.architecture = "x86"; break;
        case PROCESSOR_ARCHITECTURE_ARM: o.architecture = "arm"; break;
        default: o.architecture = "unknown"; break;
    }
#endif
    return o;
}

/// Time since the machine booted, in seconds.
[[nodiscard]] inline double uptime_seconds() {
#if FLYG_PLATFORM_LINUX
    const std::string t = detail::read_small_file("/proc/uptime");
    if (!t.empty()) return std::strtod(t.c_str(), nullptr);
    struct sysinfo si {};
    if (::sysinfo(&si) != 0) throw SystemException::from_errno("sysinfo failed");
    return static_cast<double>(si.uptime);
#else
    return static_cast<double>(::GetTickCount64()) / 1000.0;
#endif
}

[[nodiscard]] inline u32 process_id() noexcept {
#if FLYG_PLATFORM_LINUX
    return static_cast<u32>(::getpid());
#else
    return static_cast<u32>(::GetCurrentProcessId());
#endif
}

/// Absolute path of the running executable.
[[nodiscard]] inline std::filesystem::path executable_path() {
#if FLYG_PLATFORM_LINUX
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) throw SystemException("cannot resolve /proc/self/exe", ec.value());
    return p;
#else
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) throw SystemException("GetModuleFileName failed", detail::last_error());
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#endif
}

[[nodiscard]] inline std::filesystem::path home_directory() {
#if FLYG_PLATFORM_LINUX
    if (auto h = get_env("HOME"); h && !h->empty()) return *h;
    return "/";
#else
    if (auto h = get_env("USERPROFILE"); h && !h->empty()) return std::filesystem::path(detail::to_wide(*h));
    return "C:\\";
#endif
}

/// Multi-line human readable summary of the machine (used by examples and diagnostics).
[[nodiscard]] inline std::string describe_system() {
    const CpuInfo c = cpu_info();
    const MemoryInfo m = memory_info();
    const OsInfo o = os_info();
    std::ostringstream s;
    s << "OS      : " << o.name << " " << o.version << " (" << o.kernel << ", " << o.architecture << ")\n"
      << "Host    : " << o.hostname << " / user " << o.username << "\n"
      << "CPU     : " << c.brand << " [" << c.vendor << "] " << c.physical_cores << " cores / "
      << c.logical_cores << " threads\n"
      << "Memory  : " << core::format_bytes(m.total_physical) << " total, "
      << core::format_bytes(m.available_physical) << " available\n"
      << "Uptime  : " << static_cast<u64>(uptime_seconds()) << " s\n";
    return s.str();
}

}  // namespace flyg::system
