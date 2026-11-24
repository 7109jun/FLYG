// flyG - allocator interface, statistics, heap allocator and typed helpers
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#include <flyg/core/util.hpp>
#include <flyg/memory/align.hpp>

namespace flyg::memory {

using core::u64;

/// Snapshot of allocation statistics.
struct AllocationStats {
    usize current_bytes = 0;      ///< bytes currently live
    usize peak_bytes = 0;         ///< highest value current_bytes ever reached
    u64 total_bytes_allocated = 0;
    u64 total_bytes_freed = 0;
    u64 allocation_count = 0;
    u64 free_count = 0;

    [[nodiscard]] constexpr u64 live_allocations() const noexcept {
        return allocation_count >= free_count ? allocation_count - free_count : 0;
    }
};

/// Lock-free statistics accumulator (relaxed atomics).
class StatsCounter {
public:
    void on_allocate(usize bytes, u64 count = 1) noexcept {
        const usize cur = current_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        total_alloc_.fetch_add(bytes, std::memory_order_relaxed);
        n_alloc_.fetch_add(count, std::memory_order_relaxed);
        usize peak = peak_.load(std::memory_order_relaxed);
        while (cur > peak &&
               !peak_.compare_exchange_weak(peak, cur, std::memory_order_relaxed)) {
        }
    }
    void on_free(usize bytes, u64 count = 1) noexcept {
        current_.fetch_sub(bytes, std::memory_order_relaxed);
        total_free_.fetch_add(bytes, std::memory_order_relaxed);
        n_free_.fetch_add(count, std::memory_order_relaxed);
    }
    [[nodiscard]] AllocationStats snapshot() const noexcept {
        AllocationStats s;
        s.current_bytes = current_.load(std::memory_order_relaxed);
        s.peak_bytes = peak_.load(std::memory_order_relaxed);
        s.total_bytes_allocated = total_alloc_.load(std::memory_order_relaxed);
        s.total_bytes_freed = total_free_.load(std::memory_order_relaxed);
        s.allocation_count = n_alloc_.load(std::memory_order_relaxed);
        s.free_count = n_free_.load(std::memory_order_relaxed);
        return s;
    }
    void reset() noexcept {
        current_ = 0;
        peak_ = 0;
        total_alloc_ = 0;
        total_free_ = 0;
        n_alloc_ = 0;
        n_free_ = 0;
    }

private:
    std::atomic<usize> current_{0};
    std::atomic<usize> peak_{0};
    std::atomic<u64> total_alloc_{0};
    std::atomic<u64> total_free_{0};
    std::atomic<u64> n_alloc_{0};
    std::atomic<u64> n_free_{0};
};

/// Abstract allocator. Derive from it to plug in a user-defined allocator.
/// allocate() throws MemoryException on failure and never returns nullptr.
class Allocator {
public:
    Allocator() = default;
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    virtual ~Allocator() = default;

    [[nodiscard]] virtual void* allocate(usize size, usize alignment = default_alignment) = 0;
    virtual void deallocate(void* p, usize size, usize alignment = default_alignment) = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual AllocationStats stats() const noexcept { return {}; }
};

/// General purpose allocator on top of the platform aligned allocator. Thread-safe.
class HeapAllocator final : public Allocator {
public:
    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) override {
        validate_alignment(alignment);
        // sizes beyond PTRDIFF_MAX can never be valid objects (same rule as std::allocator)
        if (size > static_cast<usize>(std::numeric_limits<core::isize>::max()) - alignment)
            FLYG_UNLIKELY
            throw MemoryException("allocation size exceeds the maximum object size",
                                  ErrorCode::out_of_memory, size, alignment);
        void* p = aligned_alloc_raw(size, alignment);
        if (!p) FLYG_UNLIKELY
            throw MemoryException("heap allocation failed", ErrorCode::out_of_memory, size,
                                  alignment);
        stats_.on_allocate(size);
        return p;
    }
    void deallocate(void* p, usize size, usize = default_alignment) override {
        if (!p) return;
        aligned_free_raw(p);
        stats_.on_free(size);
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "heap"; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

private:
    StatsCounter stats_;
};

/// Process-wide default heap allocator.
[[nodiscard]] inline HeapAllocator& default_allocator() noexcept {
    static HeapAllocator instance;
    return instance;
}

// ---- typed helpers --------------------------------------------------------

/// Allocates and constructs a `T`.
template <class T, class... Args>
[[nodiscard]] T* create(Allocator& a, Args&&... args) {
    void* mem = a.allocate(sizeof(T), alignof(T));
    try {
        return ::new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
        a.deallocate(mem, sizeof(T), alignof(T));
        throw;
    }
}

/// Destroys and frees an object created with create().
template <class T>
void destroy(Allocator& a, T* p) {
    if (!p) return;
    p->~T();
    a.deallocate(p, sizeof(T), alignof(T));
}

/// Allocates and value-initialises `n` objects.
template <class T>
[[nodiscard]] T* create_array(Allocator& a, usize n) {
    const usize bytes = core::checked_mul(sizeof(T), n);
    void* mem = a.allocate(bytes, alignof(T));
    T* first = static_cast<T*>(mem);
    usize built = 0;
    try {
        for (; built < n; ++built) ::new (static_cast<void*>(first + built)) T();
    } catch (...) {
        while (built > 0) first[--built].~T();
        a.deallocate(mem, bytes, alignof(T));
        throw;
    }
    return first;
}

template <class T>
void destroy_array(Allocator& a, T* p, usize n) {
    if (!p) return;
    for (usize i = n; i > 0; --i) p[i - 1].~T();
    a.deallocate(p, sizeof(T) * n, alignof(T));
}

/// Deleter that returns memory to the allocator it came from.
template <class T>
struct Deleter {
    Allocator* allocator = nullptr;
    void operator()(T* p) const noexcept {
        if (!p || !allocator) return;
        p->~T();
        allocator->deallocate(p, sizeof(T), alignof(T));
    }
};

template <class T>
using UniquePtr = std::unique_ptr<T, Deleter<T>>;

template <class T, class... Args>
[[nodiscard]] UniquePtr<T> make_unique(Allocator& a, Args&&... args) {
    return UniquePtr<T>(create<T>(a, std::forward<Args>(args)...), Deleter<T>{&a});
}

/// Adapter so that standard containers can allocate through a flyG Allocator.
template <class T>
class StdAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    StdAllocator() noexcept : a_(&default_allocator()) {}
    explicit StdAllocator(Allocator& a) noexcept : a_(&a) {}
    template <class U>
    StdAllocator(const StdAllocator<U>& o) noexcept : a_(o.allocator()) {}

    [[nodiscard]] T* allocate(usize n) {
        return static_cast<T*>(a_->allocate(core::checked_mul(sizeof(T), n), alignof(T)));
    }
    void deallocate(T* p, usize n) noexcept {
        try {
            a_->deallocate(p, sizeof(T) * n, alignof(T));
        } catch (...) {
        }
    }
    [[nodiscard]] Allocator* allocator() const noexcept { return a_; }

    template <class U>
    friend bool operator==(const StdAllocator& l, const StdAllocator<U>& r) noexcept {
        return l.a_ == r.allocator();
    }

private:
    Allocator* a_;
};

}  // namespace flyg::memory
