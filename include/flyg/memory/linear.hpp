// flyG - fixed-capacity linear allocator
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <flyg/memory/allocator.hpp>

namespace flyg::memory {

/// Bump allocator over a single fixed buffer. Never grows: when the buffer is exhausted
/// allocate() throws MemoryException(out_of_memory). Frees are no-ops; use reset().
/// The buffer is either owned (obtained from an upstream allocator) or supplied by the caller.
/// Not thread-safe.
class LinearAllocator final : public Allocator {
public:
    /// Owning: reserves `capacity` bytes from `upstream`.
    explicit LinearAllocator(usize capacity, Allocator& upstream = default_allocator())
        : upstream_(&upstream), capacity_(capacity) {
        if (capacity == 0)
            throw MemoryException("linear allocator capacity must be non-zero",
                                  ErrorCode::invalid_argument);
        base_ = static_cast<std::byte*>(upstream.allocate(capacity, default_alignment));
    }

    /// Non-owning: uses caller supplied memory that must outlive the allocator.
    LinearAllocator(void* buffer, usize size) : base_(static_cast<std::byte*>(buffer)), capacity_(size) {
        if (!buffer || size == 0)
            throw MemoryException("linear allocator needs a non-empty buffer",
                                  ErrorCode::invalid_argument);
    }

    ~LinearAllocator() override {
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
        const auto start = reinterpret_cast<std::uintptr_t>(base_) + offset_;
        const std::uintptr_t aligned = (start + (alignment - 1)) & ~(std::uintptr_t{alignment} - 1);
        const std::uintptr_t limit = reinterpret_cast<std::uintptr_t>(base_) + capacity_;
        if (aligned < start || aligned > limit || size > limit - aligned)
            throw MemoryException("linear allocator exhausted", ErrorCode::out_of_memory, size,
                                  alignment);
        offset_ = static_cast<usize>(aligned + size - reinterpret_cast<std::uintptr_t>(base_));
        live_bytes_ += size;
        ++live_count_;
        stats_.on_allocate(size);
        return base_ + (aligned - reinterpret_cast<std::uintptr_t>(base_));
    }

    void deallocate(void*, usize, usize = default_alignment) override {}

    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "LinearAllocator never runs destructors; use a trivially destructible type");
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    void reset() noexcept {
        if (live_count_ != 0) stats_.on_free(live_bytes_, live_count_);
        offset_ = 0;
        live_bytes_ = 0;
        live_count_ = 0;
    }

    [[nodiscard]] usize used() const noexcept { return offset_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] usize remaining() const noexcept { return capacity_ - offset_; }

    [[nodiscard]] std::string_view name() const noexcept override { return "linear"; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

private:
    Allocator* upstream_ = nullptr;
    std::byte* base_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
    usize live_bytes_ = 0;
    u64 live_count_ = 0;
    StatsCounter stats_;
};

}  // namespace flyg::memory
