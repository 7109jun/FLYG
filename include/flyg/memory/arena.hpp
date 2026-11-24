// flyG - growable arena (region) allocator
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/memory/allocator.hpp>

namespace flyg::memory {

/// Bump allocator that grows by acquiring blocks from an upstream allocator.
/// Individual frees are no-ops; memory comes back through reset()/rewind()/release().
/// Objects created with create() must be trivially destructible. Not thread-safe.
class ArenaAllocator final : public Allocator {
public:
    struct Marker {
        usize block = 0;
        usize used = 0;
        usize live_bytes = 0;
        u64 live_count = 0;
    };

    explicit ArenaAllocator(usize block_size = 64 * core::KiB,
                            Allocator& upstream = default_allocator())
        : upstream_(&upstream), block_size_(block_size) {
        if (block_size == 0)
            throw MemoryException("arena block size must be non-zero", ErrorCode::invalid_argument);
    }

    ~ArenaAllocator() override { release(); }

    [[nodiscard]] void* allocate(usize size, usize alignment = default_alignment) override {
        validate_alignment(alignment);
        if (size == 0) size = 1;
        while (current_ < blocks_.size()) {
            if (void* p = try_fit(blocks_[current_], size, alignment)) return account(p, size);
            ++current_;
        }
        const usize need = core::checked_add(size, alignment);
        const usize bsize = std::max(block_size_, need);
        auto* data = static_cast<std::byte*>(upstream_->allocate(bsize, default_alignment));
        try {
            blocks_.push_back(Block{data, bsize, 0});
        } catch (...) {
            upstream_->deallocate(data, bsize, default_alignment);
            throw;
        }
        current_ = blocks_.size() - 1;
        reserved_ += bsize;
        void* p = try_fit(blocks_[current_], size, alignment);
        return account(p, size);  // cannot fail: block was sized for this request
    }

    /// Individual frees are intentionally ignored.
    void deallocate(void*, usize, usize = default_alignment) override {}

    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "ArenaAllocator never runs destructors; use a trivially destructible type");
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /// Copies a string into the arena; the result is NUL terminated.
    [[nodiscard]] std::string_view copy_string(std::string_view s) {
        char* p = static_cast<char*>(allocate(s.size() + 1, 1));
        if (!s.empty()) std::memcpy(p, s.data(), s.size());
        p[s.size()] = '\0';
        return {p, s.size()};
    }

    [[nodiscard]] Marker mark() const noexcept {
        if (blocks_.empty()) return {};
        return {current_, blocks_[current_].used, live_bytes_, live_count_};
    }

    /// Releases everything allocated after `m` was taken.
    void rewind(Marker m) {
        if (blocks_.empty()) {
            if (m.block != 0 || m.used != 0 || m.live_bytes != 0 || m.live_count != 0)
                throw MemoryException("invalid arena marker", ErrorCode::invalid_argument);
            return;
        }
        if (m.block >= blocks_.size() || m.block > current_ ||
            (m.block == current_ && m.used > blocks_[current_].used) ||
            m.live_bytes > live_bytes_ || m.live_count > live_count_)
            throw MemoryException("invalid arena marker", ErrorCode::invalid_argument);
        for (usize i = m.block + 1; i <= current_ && i < blocks_.size(); ++i) blocks_[i].used = 0;
        blocks_[m.block].used = m.used;
        current_ = m.block;
        stats_.on_free(live_bytes_ - m.live_bytes, live_count_ - m.live_count);
        live_bytes_ = m.live_bytes;
        live_count_ = m.live_count;
    }

    /// Rewinds to empty but keeps the blocks for reuse.
    void reset() noexcept {
        for (Block& b : blocks_) b.used = 0;
        current_ = 0;
        if (live_bytes_ != 0 || live_count_ != 0) stats_.on_free(live_bytes_, live_count_);
        live_bytes_ = 0;
        live_count_ = 0;
    }

    /// Returns all blocks to the upstream allocator.
    void release() noexcept {
        reset();
        for (const Block& b : blocks_) {
            try {
                upstream_->deallocate(b.data, b.size, default_alignment);
            } catch (...) {
            }
        }
        blocks_.clear();
        current_ = 0;
        reserved_ = 0;
    }

    [[nodiscard]] usize bytes_used() const noexcept { return live_bytes_; }
    [[nodiscard]] usize bytes_reserved() const noexcept { return reserved_; }
    [[nodiscard]] usize block_count() const noexcept { return blocks_.size(); }

    [[nodiscard]] std::string_view name() const noexcept override { return "arena"; }
    [[nodiscard]] AllocationStats stats() const noexcept override { return stats_.snapshot(); }

private:
    struct Block {
        std::byte* data;
        usize size;
        usize used;
    };

    static void* try_fit(Block& b, usize size, usize alignment) noexcept {
        const auto base = reinterpret_cast<std::uintptr_t>(b.data);
        const std::uintptr_t start = base + b.used;
        const std::uintptr_t aligned = (start + (alignment - 1)) & ~(std::uintptr_t{alignment} - 1);
        if (aligned < start) return nullptr;  // overflow
        const std::uintptr_t end = aligned + size;
        if (end < aligned || end > base + b.size) return nullptr;
        b.used = static_cast<usize>(end - base);
        return b.data + (aligned - base);
    }

    void* account(void* p, usize size) noexcept {
        live_bytes_ += size;
        ++live_count_;
        stats_.on_allocate(size);
        return p;
    }

    Allocator* upstream_;
    usize block_size_;
    std::vector<Block> blocks_;
    usize current_ = 0;
    usize reserved_ = 0;
    usize live_bytes_ = 0;
    u64 live_count_ = 0;
    StatsCounter stats_;
};

}  // namespace flyg::memory
