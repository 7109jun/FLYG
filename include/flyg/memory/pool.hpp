// flyG - fixed-size block pool allocator
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <vector>

#include <flyg/memory/allocator.hpp>

namespace flyg::memory {

/// Pool of equally sized blocks with O(1) allocate/free via an intrusive free list.
/// Grows chunk by chunk from an upstream allocator. Not thread-safe.
class PoolAllocator final : public Allocator {
public:
    /// @param block_size       size of every block in bytes (> 0)
    /// @param block_alignment  alignment of every block (power of two)
    /// @param blocks_per_chunk number of blocks fetched from upstream per growth step
    /// @param max_chunks       0 = unlimited; otherwise allocation throws once exhausted
    PoolAllocator(usize block_size, usize block_alignment = default_alignment,
                  usize blocks_per_chunk = 64, Allocator& upstream = default_allocator(),
                  usize max_chunks = 0)
        : upstream_(&upstream), block_size_(block_size), max_chunks_(max_chunks) {
        validate_alignment(block_alignment);
        if (block_size == 0 || blocks_per_chunk == 0)
            throw MemoryException("pool block size and blocks_per_chunk must be non-zero",
                                  ErrorCode::invalid_argument);
        align_ = std::max(block_alignment, alignof(Node));
        stride_ = checked_align_up(std::max(block_size, sizeof(Node)), align_);
        blocks_per_chunk_ = blocks_per_chunk;
        chunk_bytes_ = core::checked_mul(stride_, blocks_per_chunk);
    }

    ~PoolAllocator() override { release(); }

    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) override {
        if (size > block_size_)
            throw MemoryException("request larger than pool block size", ErrorCode::invalid_argument,
                                  size, alignment);
        validate_alignment(alignment);
        if (alignment > align_)
            throw MemoryException("request alignment larger than pool alignment",
                                  ErrorCode::bad_alignment, size, alignment);
        return allocate_block();
    }

    void deallocate(void* p, usize, usize = default_alignment) override { deallocate_block(p); }

    [[nodiscard]] void* allocate_block() {
        if (!free_head_) grow();
        Node* n = free_head_;
        free_head_ = n->next;
        ++used_;
        stats_.on_allocate(block_size_);
        return n;
    }

    void deallocate_block(void* p) {
        if (!p) return;
        if (!owns(p))
            throw MemoryException("pointer does not belong to this pool", ErrorCode::bad_free);
        if (used_ == 0)
            throw MemoryException("pool free with no live blocks (double free?)",
                                  ErrorCode::bad_free);
        free_head_ = ::new (p) Node{free_head_};
        --used_;
        stats_.on_free(block_size_);
    }

    /// True when `p` is the start of a block inside one of this pool's chunks.
    [[nodiscard]] bool owns(const void* p) const noexcept {
        const auto* b = static_cast<const std::byte*>(p);
        for (const std::byte* c : chunks_)
            if (b >= c && b < c + chunk_bytes_) return static_cast<usize>(b - c) % stride_ == 0;
        return false;
    }

    /// Marks every block free again (invalidates all outstanding pointers).
    void reset() noexcept {
        stats_.on_free(used_ * block_size_, used_);
        used_ = 0;
        free_head_ = nullptr;
        for (auto it = chunks_.rbegin(); it != chunks_.rend(); ++it) thread_chunk(*it);
    }

    /// Returns all chunks to the upstream allocator.
    void release() noexcept {
        if (used_ != 0) stats_.on_free(used_ * block_size_, used_);
        for (std::byte* c : chunks_) {
            try {
                upstream_->deallocate(c, chunk_bytes_, align_);
            } catch (...) {
            }
        }
        chunks_.clear();
        free_head_ = nullptr;
        used_ = 0;
    }

    [[nodiscard]] usize block_size() const noexcept { return block_size_; }
    [[nodiscard]] usize block_stride() const noexcept { return stride_; }
    [[nodiscard]] usize block_alignment() const noexcept { return align_; }
    [[nodiscard]] usize used_blocks() const noexcept { return used_; }
    [[nodiscard]] usize capacity_blocks() const noexcept { return chunks_.size() * blocks_per_chunk_; }
    [[nodiscard]] usize chunk_count() const noexcept { return chunks_.size(); }

    [[nodiscard]] std::string_view name() const noexcept override { return "pool"; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

private:
    struct Node {
        Node* next;
    };

    void thread_chunk(std::byte* c) noexcept {
        for (usize i = blocks_per_chunk_; i > 0; --i)
            free_head_ = ::new (c + (i - 1) * stride_) Node{free_head_};
    }

    void grow() {
        if (max_chunks_ != 0 && chunks_.size() >= max_chunks_)
            throw MemoryException("pool exhausted", ErrorCode::out_of_memory, block_size_, align_);
        auto* c = static_cast<std::byte*>(upstream_->allocate(chunk_bytes_, align_));
        try {
            chunks_.push_back(c);
        } catch (...) {
            upstream_->deallocate(c, chunk_bytes_, align_);
            throw;
        }
        thread_chunk(c);
    }

    Allocator* upstream_;
    usize block_size_;
    usize align_ = 0;
    usize stride_ = 0;
    usize blocks_per_chunk_ = 0;
    usize chunk_bytes_ = 0;
    usize max_chunks_;
    Node* free_head_ = nullptr;
    usize used_ = 0;
    std::vector<std::byte*> chunks_;
    StatsCounter stats_;
};

/// Typed object pool built on PoolAllocator.
template <class T>
class ObjectPool {
public:
    explicit ObjectPool(usize objects_per_chunk = 64, Allocator& upstream = default_allocator())
        : pool_(sizeof(T), alignof(T), objects_per_chunk, upstream) {}
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* mem = pool_.allocate_block();
        try {
            return ::new (mem) T(std::forward<Args>(args)...);
        } catch (...) {
            pool_.deallocate_block(mem);
            throw;
        }
    }
    void destroy(T* p) {
        if (!p) return;
        p->~T();
        pool_.deallocate_block(p);
    }
    [[nodiscard]] usize live_objects() const noexcept { return pool_.used_blocks(); }
    [[nodiscard]] PoolAllocator& pool() noexcept { return pool_; }

private:
    PoolAllocator pool_;
};

}  // namespace flyg::memory
