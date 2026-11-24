// flyG - LIFO stack allocator
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstring>

#include <flyg/memory/allocator.hpp>

namespace flyg::memory {

/// Fixed-capacity allocator that must be freed in strict LIFO order. Every allocation is
/// preceded by a small header so that deallocate() can restore the previous top.
/// Freeing out of order throws MemoryException(bad_free). Not thread-safe.
class StackAllocator final : public Allocator {
public:
    struct Marker {
        usize offset = 0;
        usize top = npos;
        usize live_bytes = 0;
        u64 live_count = 0;
    };

    /// Owning: reserves `capacity` bytes from `upstream`.
    explicit StackAllocator(usize capacity, Allocator& upstream = default_allocator())
        : upstream_(&upstream), capacity_(capacity) {
        if (capacity == 0)
            throw MemoryException("stack allocator capacity must be non-zero",
                                  ErrorCode::invalid_argument);
        base_ = static_cast<std::byte*>(upstream.allocate(capacity, default_alignment));
    }

    /// Non-owning: uses caller supplied memory that must outlive the allocator.
    StackAllocator(void* buffer, usize size) : base_(static_cast<std::byte*>(buffer)), capacity_(size) {
        if (!buffer || size == 0)
            throw MemoryException("stack allocator needs a non-empty buffer",
                                  ErrorCode::invalid_argument);
    }

    ~StackAllocator() override {
        if (upstream_) {
            try {
                upstream_->deallocate(base_, capacity_, default_alignment);
            } catch (...) {
            }
        }
    }

    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) override {
        validate_alignment(alignment);
        if (size == 0) size = 1;
        const auto base = reinterpret_cast<std::uintptr_t>(base_);
        const std::uintptr_t start = base + offset_;
        const std::uintptr_t after_header = start + sizeof(Header);
        const std::uintptr_t aligned = (after_header + (alignment - 1)) & ~(std::uintptr_t{alignment} - 1);
        const std::uintptr_t limit = base + capacity_;
        if (after_header < start || aligned < after_header || aligned > limit ||
            size > limit - aligned)
            throw MemoryException("stack allocator exhausted", ErrorCode::out_of_memory, size,
                                  alignment);
        const Header h{offset_, top_};
        std::memcpy(reinterpret_cast<void*>(aligned - sizeof(Header)), &h, sizeof(Header));
        top_ = static_cast<usize>(aligned - base);
        offset_ = top_ + size;
        live_bytes_ += size;
        ++live_count_;
        stats_.on_allocate(size);
        return base_ + top_;
    }

    void deallocate(void* p, usize size, usize = default_alignment) override {
        if (!p) return;
        const auto* b = static_cast<std::byte*>(p);
        if (top_ == npos || b != base_ + top_)
            throw MemoryException("stack allocator free out of LIFO order", ErrorCode::bad_free,
                                  size);
        Header h;
        std::memcpy(&h, b - sizeof(Header), sizeof(Header));
        const usize freed = offset_ - top_;
        offset_ = h.prev_offset;
        top_ = h.prev_top;
        live_bytes_ -= freed;
        --live_count_;
        stats_.on_free(freed);
    }

    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    [[nodiscard]] Marker mark() const noexcept { return {offset_, top_, live_bytes_, live_count_}; }

    /// Frees everything allocated after `m` was taken (no destructors are run).
    void rewind(Marker m) {
        if (m.offset > offset_ || m.live_bytes > live_bytes_ || m.live_count > live_count_)
            throw MemoryException("invalid stack marker", ErrorCode::invalid_argument);
        stats_.on_free(live_bytes_ - m.live_bytes, live_count_ - m.live_count);
        offset_ = m.offset;
        top_ = m.top;
        live_bytes_ = m.live_bytes;
        live_count_ = m.live_count;
    }

    void reset() noexcept {
        if (live_count_ != 0) stats_.on_free(live_bytes_, live_count_);
        offset_ = 0;
        top_ = npos;
        live_bytes_ = 0;
        live_count_ = 0;
    }

    [[nodiscard]] usize used() const noexcept { return offset_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::string_view name() const noexcept override { return "stack"; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

private:
    static constexpr usize npos = static_cast<usize>(-1);
    struct Header {
        usize prev_offset;
        usize prev_top;
    };

    Allocator* upstream_ = nullptr;
    std::byte* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize top_ = npos;
    usize live_bytes_ = 0;
    u64 live_count_ = 0;
    StatsCounter stats_;
};

}  // namespace flyg::memory
