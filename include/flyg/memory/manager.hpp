// flyG - MemoryManager: tracked heap plus named allocators
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <flyg/memory/allocator.hpp>
#include <flyg/memory/arena.hpp>
#include <flyg/memory/linear.hpp>
#include <flyg/memory/pool.hpp>
#include <flyg/memory/stack.hpp>
#include <flyg/memory/tracker.hpp>

namespace flyg::memory {

/// Owns a tracked heap and a registry of named pool/arena/linear/stack allocators.
///
/// Leak model:
///  * blocks taken from heap()/allocate()/create() are tracked individually;
///  * pool and stack allocators (and adopted user allocators) report any block that was
///    never returned;
///  * arena and linear allocators release memory in bulk, so live data in them is not a leak.
/// The named allocators draw their backing memory from an internal untracked heap, so the
/// registry itself never shows up as a leak.
///
/// Registry operations are thread-safe; the named allocators keep their own thread-safety
/// rules (pool/arena/linear/stack are single-threaded).
class MemoryManager {
public:
    MemoryManager() : tracked_(heap_, "memory-manager") {}
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    ~MemoryManager() {
        std::lock_guard lock(mutex_);
        named_.clear();
    }

    /// Process-wide instance for code that does not want to pass a manager around.
    [[nodiscard]] static MemoryManager& global() {
        static MemoryManager instance;
        return instance;
    }

    // ---- default (tracked) heap ----
    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) {
        return tracked_.allocate(size, alignment);
    }
    [[nodiscard]] void* allocate_tagged(usize size, usize alignment, std::string_view tag) {
        return tracked_.allocate_tagged(size, alignment, tag);
    }
    void deallocate(void* p, usize size, usize alignment = default_alignment) {
        tracked_.deallocate(p, size, alignment);
    }
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        return memory::create<T>(tracked_, std::forward<Args>(args)...);
    }
    template <class T>
    void destroy(T* p) {
        memory::destroy(tracked_, p);
    }

    [[nodiscard]] Allocator& heap() noexcept { return tracked_; }
    [[nodiscard]] TrackedAllocator& tracker() noexcept { return tracked_; }

    // ---- named allocators ----
    PoolAllocator& create_pool(const std::string& name, usize block_size,
                               usize block_alignment = default_alignment,
                               usize blocks_per_chunk = 64) {
        return add<PoolAllocator>(name, true, block_size, block_alignment, blocks_per_chunk,
                                  static_cast<Allocator&>(heap_), usize{0});
    }
    ArenaAllocator& create_arena(const std::string& name, usize block_size = 64 * core::KiB) {
        return add<ArenaAllocator>(name, false, block_size, static_cast<Allocator&>(heap_));
    }
    LinearAllocator& create_linear(const std::string& name, usize capacity) {
        return add<LinearAllocator>(name, false, capacity, static_cast<Allocator&>(heap_));
    }
    StackAllocator& create_stack(const std::string& name, usize capacity) {
        return add<StackAllocator>(name, true, capacity, static_cast<Allocator&>(heap_));
    }

    /// Registers a user-defined allocator under `name` (the manager takes ownership). Its
    /// stats().live_allocations() are counted as leaks.
    Allocator& adopt(const std::string& name, std::unique_ptr<Allocator> a) {
        if (!a) throw MemoryException("cannot adopt a null allocator", ErrorCode::invalid_argument);
        std::lock_guard lock(mutex_);
        if (named_.contains(name))
            throw MemoryException("allocator name already registered: " + name,
                                  ErrorCode::invalid_argument);
        Allocator& ref = *a;
        named_.emplace(name, Entry{std::move(a), true});
        return ref;
    }

    [[nodiscard]] Allocator* find(const std::string& name) const {
        std::lock_guard lock(mutex_);
        auto it = named_.find(name);
        return it == named_.end() ? nullptr : it->second.alloc.get();
    }

    [[nodiscard]] Allocator& get(const std::string& name) const {
        Allocator* a = find(name);
        if (!a) throw MemoryException("unknown allocator: " + name, ErrorCode::invalid_argument);
        return *a;
    }

    /// Destroys a named allocator; returns false when the name is unknown.
    bool destroy_allocator(const std::string& name) {
        Entry victim;
        {
            std::lock_guard lock(mutex_);
            auto it = named_.find(name);
            if (it == named_.end()) return false;
            victim = std::move(it->second);
            named_.erase(it);
        }
        return true;  // `victim` is released here, outside the lock
    }

    [[nodiscard]] usize allocator_count() const {
        std::lock_guard lock(mutex_);
        return named_.size();
    }

    // ---- statistics / leaks ----
    /// Statistics of the tracked heap.
    [[nodiscard]] AllocationStats stats() const noexcept { return tracked_.stats(); }

    /// Tracked heap blocks plus live blocks of pool/stack/adopted allocators.
    [[nodiscard]] usize leak_count() const {
        usize n = tracked_.leak_count();
        std::lock_guard lock(mutex_);
        for (const auto& [name, e] : named_)
            if (e.check_live) n += static_cast<usize>(e.alloc->stats().live_allocations());
        return n;
    }

    /// Throws MemoryException(leak_detected) with a full report when leak_count() != 0.
    void check_leaks() const {
        if (leak_count() == 0) return;
        std::string msg = tracked_.report();
        std::lock_guard lock(mutex_);
        for (const auto& [name, e] : named_) {
            const u64 live = e.check_live ? e.alloc->stats().live_allocations() : 0;
            if (live != 0)
                msg += std::format("[flyg::memory] allocator '{}' ({}): {} block(s) never returned\n",
                                   name, e.alloc->name(), live);
        }
        throw MemoryException(std::move(msg), ErrorCode::leak_detected);
    }

    [[nodiscard]] std::string report() const {
        const AllocationStats s = tracked_.stats();
        std::string out = std::format(
            "[flyg::memory] manager: live={} peak={} allocs={} frees={} leaks={}\n",
            core::format_bytes(s.current_bytes), core::format_bytes(s.peak_bytes),
            s.allocation_count, s.free_count, leak_count());
        std::lock_guard lock(mutex_);
        for (const auto& [n, e] : named_) {
            const AllocationStats as = e.alloc->stats();
            out += std::format("  {} ({}): live={} peak={} allocs={} frees={}\n", n,
                               e.alloc->name(), core::format_bytes(as.current_bytes),
                               core::format_bytes(as.peak_bytes), as.allocation_count,
                               as.free_count);
        }
        return out;
    }

private:
    struct Entry {
        std::unique_ptr<Allocator> alloc;
        bool check_live = false;
    };

    template <class A, class... Args>
    A& add(const std::string& name, bool check_live, Args&&... args) {
        // construct first (may throw), then register under the lock
        auto a = std::make_unique<A>(std::forward<Args>(args)...);
        A& ref = *a;
        std::lock_guard lock(mutex_);
        if (named_.contains(name))
            throw MemoryException("allocator name already registered: " + name,
                                  ErrorCode::invalid_argument);
        named_.emplace(name, Entry{std::move(a), check_live});
        return ref;
    }

    HeapAllocator heap_;
    TrackedAllocator tracked_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> named_;
};

}  // namespace flyg::memory
