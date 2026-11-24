// flyG - allocation tracking and leak detection
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <flyg/memory/allocator.hpp>

namespace flyg::memory {

/// One live allocation known to a TrackedAllocator.
struct AllocationRecord {
    const void* ptr = nullptr;
    usize size = 0;
    usize alignment = 0;
    u64 id = 0;           ///< monotonically increasing allocation sequence number
    std::string tag;      ///< optional user label
};

/// Wraps another allocator and records every live allocation.
/// Detects double frees, foreign pointers and size/alignment mismatches (throws
/// MemoryException(bad_free)) and reports leaks. Thread-safe.
class TrackedAllocator final : public Allocator {
public:
    using LeakHandler = std::function<void(const std::vector<AllocationRecord>&)>;

    explicit TrackedAllocator(Allocator& upstream = default_allocator(),
                              std::string name = "tracked")
        : upstream_(&upstream), name_(std::move(name)) {}

    ~TrackedAllocator() override {
        if (!report_on_destroy_) return;
        try {
            auto l = leaks();
            if (l.empty()) return;
            if (handler_)
                handler_(l);
            else
                std::fputs(report().c_str(), stderr);
        } catch (...) {
        }
    }

    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) override {
        return allocate_tagged(size, alignment, {});
    }

    /// Allocation with a descriptive tag that shows up in leak reports.
    [[nodiscard]] void* allocate_tagged(usize size, usize alignment, std::string_view tag) {
        void* p = upstream_->allocate(size, alignment);
        try {
            std::lock_guard lock(mutex_);
            live_.emplace(p, AllocationRecord{p, size, alignment, ++next_id_, std::string(tag)});
        } catch (...) {
            upstream_->deallocate(p, size, alignment);
            throw;
        }
        stats_.on_allocate(size);
        return p;
    }

    void deallocate(void* p, usize size, usize alignment = default_alignment) override {
        if (!p) return;
        {
            std::lock_guard lock(mutex_);
            auto it = live_.find(p);
            if (it == live_.end())
                throw MemoryException(
                    "free of unknown pointer (double free or foreign pointer)",
                    ErrorCode::bad_free, size, alignment);
            if (it->second.size != size || it->second.alignment != alignment)
                throw MemoryException(
                    std::format("free with mismatching size/alignment (allocated size={}, "
                                "alignment={})",
                                it->second.size, it->second.alignment),
                    ErrorCode::bad_free, size, alignment);
            live_.erase(it);
        }
        upstream_->deallocate(p, size, alignment);
        stats_.on_free(size);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

    [[nodiscard]] bool is_live(const void* p) const {
        std::lock_guard lock(mutex_);
        return live_.contains(p);
    }

    /// Live allocations, ordered by allocation id.
    [[nodiscard]] std::vector<AllocationRecord> leaks() const {
        std::vector<AllocationRecord> out;
        {
            std::lock_guard lock(mutex_);
            out.reserve(live_.size());
            for (const auto& [ptr, rec] : live_) out.push_back(rec);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        return out;
    }

    [[nodiscard]] usize leak_count() const {
        std::lock_guard lock(mutex_);
        return live_.size();
    }

    [[nodiscard]] usize leaked_bytes() const {
        std::lock_guard lock(mutex_);
        usize n = 0;
        for (const auto& [ptr, rec] : live_) n += rec.size;
        return n;
    }

    /// Human readable leak report ("no leaks" when empty).
    [[nodiscard]] std::string report() const {
        const auto l = leaks();
        if (l.empty()) return std::format("[flyg::memory] {}: no leaks\n", name_);
        std::string s = std::format("[flyg::memory] {}: {} leaked allocation(s), {} bytes\n", name_,
                                    l.size(), leaked_bytes());
        for (const auto& r : l)
            s += std::format("  #{} {} size={} align={}{}{}\n", r.id, r.ptr, r.size, r.alignment,
                             r.tag.empty() ? "" : " tag=", r.tag);
        return s;
    }

    /// Throws MemoryException(leak_detected) if any allocation is still live.
    void check_leaks() const {
        if (leak_count() != 0)
            throw MemoryException(report(), ErrorCode::leak_detected);
    }

    /// Called from the destructor when leaks remain (default: print to stderr).
    void set_leak_handler(LeakHandler h) { handler_ = std::move(h); }
    void set_report_on_destroy(bool v) noexcept { report_on_destroy_ = v; }

private:
    Allocator* upstream_;
    std::string name_;
    mutable std::mutex mutex_;
    std::unordered_map<const void*, AllocationRecord> live_;
    u64 next_id_ = 0;
    StatsCounter stats_;
    LeakHandler handler_;
    bool report_on_destroy_ = true;
};

}  // namespace flyg::memory
