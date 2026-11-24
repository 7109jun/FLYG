// flyG - exception hierarchy
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cerrno>
#include <exception>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <flyg/core/types.hpp>

namespace flyg {

enum class ErrorCode : i32 {
    ok = 0,
    unknown,
    invalid_argument,
    out_of_range,
    out_of_memory,
    invalid_state,
    not_supported,
    io_error,
    timeout,
    parse_error,
    overflow,
    bad_alignment,
    bad_free,
    leak_detected,
    network_error,
    graphics_error,
    math_error,
    data_error,
    system_error,
};

constexpr std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::unknown: return "unknown";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::out_of_range: return "out_of_range";
        case ErrorCode::out_of_memory: return "out_of_memory";
        case ErrorCode::invalid_state: return "invalid_state";
        case ErrorCode::not_supported: return "not_supported";
        case ErrorCode::io_error: return "io_error";
        case ErrorCode::timeout: return "timeout";
        case ErrorCode::parse_error: return "parse_error";
        case ErrorCode::overflow: return "overflow";
        case ErrorCode::bad_alignment: return "bad_alignment";
        case ErrorCode::bad_free: return "bad_free";
        case ErrorCode::leak_detected: return "leak_detected";
        case ErrorCode::network_error: return "network_error";
        case ErrorCode::graphics_error: return "graphics_error";
        case ErrorCode::math_error: return "math_error";
        case ErrorCode::data_error: return "data_error";
        case ErrorCode::system_error: return "system_error";
    }
    return "unknown";
}

/// Base class of every exception thrown by flyG.
class Exception : public std::exception {
public:
    explicit Exception(std::string message, ErrorCode code = ErrorCode::unknown,
                       std::source_location loc = std::source_location::current())
        : message_(std::move(message)), code_(code), loc_(loc) {
        full_ = std::format("[{}] {} ({}:{} in {})", to_string(code_), message_, loc_.file_name(),
                            loc_.line(), loc_.function_name());
    }

    const char* what() const noexcept override { return full_.c_str(); }
    const std::string& message() const noexcept { return message_; }
    ErrorCode code() const noexcept { return code_; }
    const std::source_location& location() const noexcept { return loc_; }

private:
    std::string message_;
    ErrorCode code_;
    std::source_location loc_;
    std::string full_;
};

/// Operating-system level failure; carries the native error number.
class SystemException : public Exception {
public:
    explicit SystemException(std::string message, int native_code = 0,
                             std::source_location loc = std::source_location::current())
        : Exception(native_code != 0 ? std::format("{}: {} (os error {})", message,
                                                   std::system_category().message(native_code),
                                                   native_code)
                                     : std::move(message),
                    ErrorCode::system_error, loc),
          native_code_(native_code) {}

    int native_code() const noexcept { return native_code_; }

    /// Builds an exception from the current `errno`.
    static SystemException from_errno(std::string message,
                                      std::source_location loc = std::source_location::current()) {
        const int e = errno;
        return SystemException(std::move(message), e, loc);
    }

private:
    int native_code_;
};

class GraphicsException : public Exception {
public:
    explicit GraphicsException(std::string message, std::string backend = {},
                               std::source_location loc = std::source_location::current())
        : Exception(backend.empty() ? std::move(message)
                                    : std::format("{} [backend: {}]", message, backend),
                    ErrorCode::graphics_error, loc),
          backend_(std::move(backend)) {}
    const std::string& backend() const noexcept { return backend_; }

private:
    std::string backend_;
};

class NetworkException : public Exception {
public:
    explicit NetworkException(std::string message, int native_code = 0,
                              std::source_location loc = std::source_location::current())
        : Exception(native_code != 0 ? std::format("{} (net error {})", message, native_code)
                                     : std::move(message),
                    ErrorCode::network_error, loc),
          native_code_(native_code) {}
    int native_code() const noexcept { return native_code_; }

private:
    int native_code_;
};

class DataException : public Exception {
public:
    static constexpr usize no_offset = static_cast<usize>(-1);

    explicit DataException(std::string message, usize offset = no_offset,
                           std::source_location loc = std::source_location::current())
        : Exception(offset == no_offset ? std::move(message)
                                        : std::format("{} (at offset {})", message, offset),
                    ErrorCode::data_error, loc),
          offset_(offset) {}
    /// Byte offset (or character index) where the problem was detected, or `no_offset`.
    usize offset() const noexcept { return offset_; }

private:
    usize offset_;
};

class MathException : public Exception {
public:
    explicit MathException(std::string message,
                           std::source_location loc = std::source_location::current())
        : Exception(std::move(message), ErrorCode::math_error, loc) {}
};

class MemoryException : public Exception {
public:
    explicit MemoryException(std::string message, ErrorCode code = ErrorCode::out_of_memory,
                             usize requested_size = 0, usize requested_alignment = 0,
                             std::source_location loc = std::source_location::current())
        : Exception(requested_size != 0
                        ? std::format("{} (size={}, alignment={})", message, requested_size,
                                      requested_alignment)
                        : std::move(message),
                    code, loc),
          size_(requested_size),
          alignment_(requested_alignment) {}
    usize requested_size() const noexcept { return size_; }
    usize requested_alignment() const noexcept { return alignment_; }

private:
    usize size_;
    usize alignment_;
};

}  // namespace flyg
