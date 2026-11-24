// flyG - memory module tests
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/memory/memory.hpp>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::memory;

namespace {

struct Counted {
    static inline int alive = 0;
    static inline int throw_at = -1;  // construct number that throws
    static inline int constructed = 0;
    int value;
    explicit Counted(int v = 0) : value(v) {
        if (constructed++ == throw_at) throw std::runtime_error("ctor failed");
        ++alive;
    }
    ~Counted() { --alive; }
};

struct alignas(64) Wide {
    char data[64];
};

// user-defined allocator plugged into the framework
class CountingAllocator final : public Allocator {
public:
    void* allocate(usize size, usize alignment) override {
        ++allocs;
        return inner_.allocate(size, alignment);
    }
    void deallocate(void* p, usize size, usize alignment) override {
        ++frees;
        inner_.deallocate(p, size, alignment);
    }
    std::string_view name() const noexcept override { return "counting"; }
    int allocs = 0;
    int frees = 0;

private:
    HeapAllocator inner_;
};

}  // namespace

// ---------------------------------------------------------------- align ----
FLYG_TEST(memory_align_helpers) {
    CHECK(is_power_of_two(1));
    CHECK(is_power_of_two(64));
    CHECK(!is_power_of_two(0));
    CHECK(!is_power_of_two(48));
    CHECK_EQ(align_up(13, 8), usize{16});
    CHECK_EQ(align_up(16, 8), usize{16});
    CHECK_EQ(align_down(13, 8), usize{8});
    CHECK_EQ(checked_align_up(13, 16), usize{16});
    CHECK_THROWS_AS(checked_align_up(13, 3), MemoryException);
    CHECK_THROWS_AS(checked_align_up(static_cast<usize>(-1), 16), MemoryException);
    CHECK_THROWS_AS(validate_alignment(0), MemoryException);
    alignas(32) char buf[64];
    CHECK(is_aligned(buf, 32));
    CHECK(!is_aligned(buf + 1, 2));
}

// ----------------------------------------------------------------- heap ----
FLYG_TEST(memory_heap_alignment_and_stats) {
    HeapAllocator h;
    for (usize al : {usize{1}, usize{8}, usize{16}, usize{64}, usize{4096}}) {
        void* p = h.allocate(100, al);
        CHECK(is_aligned(p, al));
        std::memset(p, 0xAB, 100);
        h.deallocate(p, 100, al);
    }
    void* a = h.allocate(1000);
    void* b = h.allocate(500);
    AllocationStats s = h.stats();
    CHECK_EQ(s.current_bytes, usize{1500});
    CHECK_EQ(s.live_allocations(), u64{2});
    h.deallocate(a, 1000);
    s = h.stats();
    CHECK_EQ(s.current_bytes, usize{500});
    CHECK(s.peak_bytes >= 1500);
    h.deallocate(b, 500);
    CHECK_EQ(h.stats().live_allocations(), u64{0});
    CHECK_NOTHROW(h.deallocate(nullptr, 0));
}

FLYG_TEST(memory_heap_errors) {
    HeapAllocator h;
    CHECK_THROWS_AS(h.allocate(16, 3), MemoryException);
    CHECK_THROWS_AS(h.allocate(16, 0), MemoryException);
    try {
        (void)h.allocate(static_cast<usize>(-1) / 2, 16);  // absurd size
        CHECK(false);
    } catch (const MemoryException& e) {
        CHECK(e.code() == ErrorCode::out_of_memory);
        CHECK(e.requested_size() != 0);
    }
    void* z = h.allocate(0);  // zero size still yields a unique valid pointer
    CHECK(z != nullptr);
    h.deallocate(z, 0);
}

FLYG_TEST(memory_typed_helpers) {
    CountingAllocator ca;
    Counted::alive = 0;
    Counted::constructed = 0;
    Counted::throw_at = -1;
    {
        Counted* c = create<Counted>(ca, 7);
        CHECK_EQ(c->value, 7);
        CHECK_EQ(Counted::alive, 1);
        destroy(ca, c);
        CHECK_EQ(Counted::alive, 0);
    }
    CHECK_EQ(ca.allocs, ca.frees);

    // exception thrown by the constructor must release the memory
    Counted::throw_at = Counted::constructed;
    CHECK_THROWS_AS(create<Counted>(ca, 1), std::runtime_error);
    CHECK_EQ(ca.allocs, ca.frees);
    Counted::throw_at = -1;

    // array: all-or-nothing
    Counted::alive = 0;
    Counted::throw_at = Counted::constructed + 3;  // 4th element throws
    CHECK_THROWS_AS(create_array<Counted>(ca, 8), std::runtime_error);
    CHECK_EQ(Counted::alive, 0);
    CHECK_EQ(ca.allocs, ca.frees);
    Counted::throw_at = -1;

    Counted* arr = create_array<Counted>(ca, 5);
    CHECK_EQ(Counted::alive, 5);
    destroy_array(ca, arr, 5);
    CHECK_EQ(Counted::alive, 0);
    CHECK_EQ(ca.allocs, ca.frees);

    CHECK_THROWS_AS(create_array<u64>(ca, static_cast<usize>(-1)), Exception);  // overflow

    Wide* w = create<Wide>(ca);
    CHECK(is_aligned(w, 64));
    destroy(ca, w);
}

FLYG_TEST(memory_unique_ptr_and_std_allocator) {
    CountingAllocator ca;
    Counted::alive = 0;
    Counted::throw_at = -1;
    {
        auto p = memory::make_unique<Counted>(ca, 3);
        CHECK_EQ(p->value, 3);
        CHECK_EQ(Counted::alive, 1);
    }
    CHECK_EQ(Counted::alive, 0);
    CHECK_EQ(ca.allocs, ca.frees);

    {
        std::vector<int, StdAllocator<int>> v{StdAllocator<int>(ca)};
        for (int i = 0; i < 1000; ++i) v.push_back(i);
        CHECK_EQ(v[999], 999);
        CHECK(ca.allocs > 0);
    }
    CHECK_EQ(ca.allocs, ca.frees);
    CHECK(StdAllocator<int>(ca) == StdAllocator<char>(ca));
}

// -------------------------------------------------------------- tracker ----
FLYG_TEST(memory_tracker_detects_leaks) {
    HeapAllocator heap;
    TrackedAllocator t(heap, "t");
    t.set_report_on_destroy(false);
    void* a = t.allocate(64, 16);
    void* b = t.allocate_tagged(32, 8, "texture-cache");
    CHECK_EQ(t.leak_count(), usize{2});
    CHECK_EQ(t.leaked_bytes(), usize{96});
    CHECK(t.is_live(a));
    const auto leaks = t.leaks();
    CHECK_EQ(leaks.size(), usize{2});
    CHECK(leaks[0].id < leaks[1].id);
    CHECK_EQ(leaks[1].tag, std::string("texture-cache"));
    const std::string r = t.report();
    CHECK(r.find("2 leaked allocation(s)") != std::string::npos);
    CHECK(r.find("texture-cache") != std::string::npos);
    try {
        t.check_leaks();
        CHECK(false);
    } catch (const MemoryException& e) {
        CHECK(e.code() == ErrorCode::leak_detected);
    }
    t.deallocate(a, 64, 16);
    t.deallocate(b, 32, 8);
    CHECK_EQ(t.leak_count(), usize{0});
    CHECK_NOTHROW(t.check_leaks());
    CHECK(t.report().find("no leaks") != std::string::npos);
    CHECK_EQ(t.stats().peak_bytes, usize{96});
    CHECK_EQ(heap.stats().live_allocations(), u64{0});
}

FLYG_TEST(memory_tracker_detects_misuse) {
    HeapAllocator heap;
    TrackedAllocator t(heap);
    void* p = t.allocate(64, 16);
    CHECK_THROWS_AS(t.deallocate(p, 32, 16), MemoryException);  // wrong size
    CHECK_THROWS_AS(t.deallocate(p, 64, 32), MemoryException);  // wrong alignment
    int local = 0;
    CHECK_THROWS_AS(t.deallocate(&local, 4, 4), MemoryException);  // foreign pointer
    t.deallocate(p, 64, 16);
    try {
        t.deallocate(p, 64, 16);  // double free
        CHECK(false);
    } catch (const MemoryException& e) {
        CHECK(e.code() == ErrorCode::bad_free);
    }
    CHECK_EQ(heap.stats().live_allocations(), u64{0});  // upstream untouched by bad frees
}

FLYG_TEST(memory_tracker_leak_handler_on_destroy) {
    HeapAllocator heap;
    int reported = 0;
    usize reported_size = 0;
    void* leaked = nullptr;
    {
        TrackedAllocator t(heap, "scoped");
        t.set_leak_handler([&](const std::vector<AllocationRecord>& l) {
            reported = static_cast<int>(l.size());
            reported_size = l[0].size;
        });
        leaked = t.allocate(24, 8);
    }
    CHECK_EQ(reported, 1);
    CHECK_EQ(reported_size, usize{24});
    heap.deallocate(leaked, 24, 8);  // tracker does not free leaked blocks itself
}

FLYG_TEST(memory_tracker_thread_safety) {
    HeapAllocator heap;
    TrackedAllocator t(heap);
    constexpr int threads = 4;
    constexpr int iterations = 2000;
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&] {
            std::vector<std::pair<void*, usize>> mine;
            for (int k = 0; k < iterations; ++k) {
                const usize sz = 32 + static_cast<usize>(k % 7) * 8;
                mine.emplace_back(t.allocate(sz, 16), sz);
                if (k % 3 == 0) {
                    t.deallocate(mine.back().first, mine.back().second, 16);
                    mine.pop_back();
                }
            }
            for (auto& [p, sz] : mine) t.deallocate(p, sz, 16);
        });
    for (auto& th : ts) th.join();
    CHECK_EQ(t.leak_count(), usize{0});
    CHECK_EQ(t.stats().live_allocations(), u64{0});
}

// ----------------------------------------------------------------- pool ----
FLYG_TEST(memory_pool_basic) {
    HeapAllocator heap;
    {
        PoolAllocator pool(48, 16, 4, heap);
        CHECK_EQ(pool.block_size(), usize{48});
        CHECK_EQ(pool.block_stride() % 16, usize{0});
        void* a = pool.allocate(48, 16);
        void* b = pool.allocate(10, 8);
        CHECK(is_aligned(a, 16));
        CHECK(a != b);
        CHECK(pool.owns(a));
        CHECK_EQ(pool.used_blocks(), usize{2});
        CHECK_EQ(pool.chunk_count(), usize{1});
        CHECK_EQ(pool.capacity_blocks(), usize{4});

        pool.deallocate(a, 48, 16);
        void* c = pool.allocate(48, 16);
        CHECK_EQ(c, a);  // LIFO free list reuses the block just freed

        // grow past one chunk
        std::vector<void*> blocks{b, c};
        for (int i = 0; i < 10; ++i) blocks.push_back(pool.allocate(48, 16));
        CHECK(pool.chunk_count() >= 3);
        for (void* p : blocks) CHECK(is_aligned(p, 16));
        CHECK_EQ(pool.used_blocks(), usize{12});
        for (void* p : blocks) pool.deallocate(p, 48, 16);
        CHECK_EQ(pool.used_blocks(), usize{0});
        CHECK_EQ(pool.stats().live_allocations(), u64{0});
    }
    CHECK_EQ(heap.stats().live_allocations(), u64{0});  // chunks returned on destruction
}

FLYG_TEST(memory_pool_errors_and_limits) {
    HeapAllocator heap;
    PoolAllocator pool(32, 8, 2, heap, /*max_chunks=*/1);
    CHECK_THROWS_AS(pool.allocate(33, 8), MemoryException);     // too large
    CHECK_THROWS_AS(pool.allocate(8, 128), MemoryException);    // alignment too large
    CHECK_THROWS_AS(pool.allocate(8, 7), MemoryException);      // bad alignment
    void* a = pool.allocate(32, 8);
    void* b = pool.allocate(32, 8);
    try {
        (void)pool.allocate(32, 8);
        CHECK(false);
    } catch (const MemoryException& e) {
        CHECK(e.code() == ErrorCode::out_of_memory);
    }
    int local;
    CHECK_THROWS_AS(pool.deallocate(&local, 4), MemoryException);  // foreign pointer
    CHECK_THROWS_AS(pool.deallocate(static_cast<char*>(a) + 1, 32), MemoryException);  // misaligned
    pool.deallocate(a, 32);
    pool.deallocate(b, 32);
    CHECK_THROWS_AS(pool.deallocate(b, 32), MemoryException);  // nothing live any more
    CHECK_THROWS_AS(PoolAllocator(0), MemoryException);
    CHECK_THROWS_AS(PoolAllocator(16, 3), MemoryException);
    CHECK_THROWS_AS(PoolAllocator(16, 8, 0), MemoryException);
}

FLYG_TEST(memory_pool_reset_and_object_pool) {
    HeapAllocator heap;
    PoolAllocator pool(16, 8, 8, heap);
    for (int i = 0; i < 20; ++i) (void)pool.allocate(16, 8);
    CHECK_EQ(pool.used_blocks(), usize{20});
    const usize chunks = pool.chunk_count();
    pool.reset();
    CHECK_EQ(pool.used_blocks(), usize{0});
    CHECK_EQ(pool.chunk_count(), chunks);  // memory kept
    for (int i = 0; i < 20; ++i) (void)pool.allocate(16, 8);
    CHECK_EQ(pool.chunk_count(), chunks);  // reused, no new chunk
    pool.release();
    CHECK_EQ(pool.chunk_count(), usize{0});
    CHECK_EQ(heap.stats().live_allocations(), u64{0});

    Counted::alive = 0;
    Counted::throw_at = -1;
    ObjectPool<Counted> op(4, heap);
    Counted* x = op.create(1);
    Counted* y = op.create(2);
    CHECK_EQ(op.live_objects(), usize{2});
    CHECK_EQ(Counted::alive, 2);
    op.destroy(x);
    op.destroy(y);
    CHECK_EQ(Counted::alive, 0);
    Counted::throw_at = Counted::constructed;
    CHECK_THROWS_AS(op.create(3), std::runtime_error);
    CHECK_EQ(op.live_objects(), usize{0});  // block went back to the pool
    Counted::throw_at = -1;
}

// ---------------------------------------------------------------- arena ----
FLYG_TEST(memory_arena_basic) {
    HeapAllocator heap;
    {
        ArenaAllocator arena(256, heap);
        void* a = arena.allocate(10, 1);
        void* b = arena.allocate(10, 64);
        CHECK(is_aligned(b, 64));
        CHECK(a != b);
        CHECK_EQ(arena.block_count(), usize{1});

        // exceed the first block: new block appears
        for (int i = 0; i < 40; ++i) CHECK(arena.allocate(32, 8) != nullptr);
        CHECK(arena.block_count() > 1);

        // oversize request gets its own (bigger) block
        void* big = arena.allocate(5000, 16);
        CHECK(is_aligned(big, 16));
        std::memset(big, 0x5A, 5000);
        CHECK(arena.bytes_reserved() >= 5000);

        auto* i = arena.create<int>(1234);
        CHECK_EQ(*i, 1234);
        auto sv = arena.copy_string("hello flyg");
        CHECK_EQ(sv, "hello flyg");
        CHECK_EQ(sv.data()[sv.size()], '\0');

        arena.deallocate(a, 10);  // no-op, must not crash
        CHECK(arena.stats().current_bytes >= 5000);
    }
    CHECK_EQ(heap.stats().live_allocations(), u64{0});
}

FLYG_TEST(memory_arena_reset_mark_rewind) {
    HeapAllocator heap;
    ArenaAllocator arena(128, heap);
    void* first = arena.allocate(16, 16);
    const auto m = arena.mark();
    const usize used_at_mark = arena.bytes_used();
    for (int i = 0; i < 30; ++i) (void)arena.allocate(32, 8);  // spills into more blocks
    CHECK(arena.block_count() > 1);
    arena.rewind(m);
    CHECK_EQ(arena.bytes_used(), used_at_mark);
    CHECK_EQ(arena.stats().live_allocations(), u64{1});
    void* again = arena.allocate(32, 8);  // memory after the marker is handed out again
    CHECK(again != nullptr);

    const usize reserved = arena.bytes_reserved();
    arena.reset();
    CHECK_EQ(arena.bytes_used(), usize{0});
    CHECK_EQ(arena.bytes_reserved(), reserved);  // blocks retained
    CHECK_EQ(arena.stats().live_allocations(), u64{0});
    CHECK_EQ(arena.allocate(16, 16), first);  // starts again at the beginning

    ArenaAllocator::Marker bogus{99, 0, 0, 0};
    CHECK_THROWS_AS(arena.rewind(bogus), MemoryException);
    arena.release();
    CHECK_EQ(arena.block_count(), usize{0});
    CHECK_EQ(heap.stats().live_allocations(), u64{0});
    CHECK_THROWS_AS(ArenaAllocator(0), MemoryException);
    CHECK_THROWS_AS(arena.allocate(8, 6), MemoryException);
}

// --------------------------------------------------------------- linear ----
FLYG_TEST(memory_linear_allocator) {
    HeapAllocator heap;
    {
        LinearAllocator lin(256, heap);
        CHECK_EQ(lin.capacity(), usize{256});
        void* a = lin.allocate(10, 1);
        void* b = lin.allocate(8, 32);
        CHECK(is_aligned(b, 32));
        CHECK(b > a);
        CHECK(lin.used() >= 18);
        CHECK_THROWS_AS(lin.allocate(1000, 8), MemoryException);  // never grows
        try {
            (void)lin.allocate(static_cast<usize>(-1), 8);
            CHECK(false);
        } catch (const MemoryException& e) {
            CHECK(e.code() == ErrorCode::out_of_memory);
        }
        CHECK_EQ(lin.remaining(), lin.capacity() - lin.used());
        lin.reset();
        CHECK_EQ(lin.used(), usize{0});
        CHECK_EQ(lin.allocate(10, 1), a);
        CHECK_EQ(lin.stats().live_allocations(), u64{1});
    }
    CHECK_EQ(heap.stats().live_allocations(), u64{0});

    // non-owning over a stack buffer
    alignas(16) std::byte buf[64];
    LinearAllocator ext(buf, sizeof buf);
    void* p = ext.allocate(64, 1);
    CHECK_EQ(p, static_cast<void*>(buf));
    CHECK_THROWS_AS(ext.allocate(1, 1), MemoryException);
    CHECK_THROWS_AS(LinearAllocator(nullptr, 10), MemoryException);
    CHECK_THROWS_AS(LinearAllocator(0), MemoryException);
}

// ---------------------------------------------------------------- stack ----
FLYG_TEST(memory_stack_lifo) {
    HeapAllocator heap;
    {
        StackAllocator st(512, heap);
        void* a = st.allocate(24, 8);
        void* b = st.allocate(40, 32);
        void* c = st.allocate(8, 1);
        CHECK(is_aligned(b, 32));
        CHECK(a < b && b < c);
        std::memset(a, 1, 24);
        std::memset(b, 2, 40);
        std::memset(c, 3, 8);
        CHECK_EQ(static_cast<unsigned char*>(a)[23], 1);  // headers did not clobber neighbours

        CHECK_THROWS_AS(st.deallocate(a, 24), MemoryException);  // not the top
        CHECK_THROWS_AS(st.deallocate(b, 40), MemoryException);
        st.deallocate(c, 8);
        st.deallocate(b, 40);
        void* b2 = st.allocate(40, 32);  // space is reusable after popping
        CHECK_EQ(b2, b);
        st.deallocate(b2, 40);
        st.deallocate(a, 24);
        CHECK_EQ(st.used(), usize{0});
        CHECK_EQ(st.stats().live_allocations(), u64{0});
        CHECK_THROWS_AS(st.deallocate(a, 24), MemoryException);  // empty stack
    }
    CHECK_EQ(heap.stats().live_allocations(), u64{0});
}

FLYG_TEST(memory_stack_marker_and_exhaustion) {
    StackAllocator st(256);
    void* a = st.allocate(16, 8);
    const auto m = st.mark();
    (void)st.allocate(32, 8);
    (void)st.allocate(32, 8);
    CHECK_EQ(st.stats().live_allocations(), u64{3});
    st.rewind(m);
    CHECK_EQ(st.stats().live_allocations(), u64{1});
    st.deallocate(a, 16);  // top is `a` again after rewind
    CHECK_EQ(st.used(), usize{0});

    CHECK_THROWS_AS(st.allocate(10000, 8), MemoryException);
    CHECK_THROWS_AS(st.allocate(8, 5), MemoryException);
    StackAllocator::Marker bogus{9999, 0, 0, 0};
    CHECK_THROWS_AS(st.rewind(bogus), MemoryException);
    st.reset();
    CHECK_EQ(st.used(), usize{0});

    alignas(16) std::byte buf[128];
    StackAllocator ext(buf, sizeof buf);
    void* p = ext.allocate(16, 16);
    CHECK(is_aligned(p, 16));
    CHECK(p >= static_cast<void*>(buf) && p < static_cast<void*>(buf + 128));
}

// -------------------------------------------------------------- manager ----
FLYG_TEST(memory_manager_registry) {
    MemoryManager mm;
    PoolAllocator& pool = mm.create_pool("particles", 64, 16, 8);
    ArenaAllocator& arena = mm.create_arena("frame", 1024);
    LinearAllocator& lin = mm.create_linear("scratch", 512);
    StackAllocator& stk = mm.create_stack("temp", 512);
    CHECK_EQ(mm.allocator_count(), usize{4});
    CHECK_EQ(mm.find("particles"), static_cast<Allocator*>(&pool));
    CHECK(mm.find("nothing") == nullptr);
    CHECK_EQ(&mm.get("frame"), static_cast<Allocator*>(&arena));
    CHECK_THROWS_AS(mm.get("nothing"), MemoryException);
    CHECK_THROWS_AS(mm.create_pool("particles", 8), MemoryException);  // duplicate name

    void* p = pool.allocate(64, 16);
    (void)arena.allocate(100, 8);
    (void)lin.allocate(10, 1);
    void* s = stk.allocate(10, 1);
    // pool and stack blocks that were not returned count as leaks; arena/linear data does not
    CHECK_EQ(mm.leak_count(), usize{2});
    try {
        mm.check_leaks();
        CHECK(false);
    } catch (const MemoryException& e) {
        CHECK(e.code() == ErrorCode::leak_detected);
        const std::string w = e.what();
        CHECK(w.find("'particles'") != std::string::npos);
        CHECK(w.find("'temp'") != std::string::npos);
        CHECK(w.find("'frame'") == std::string::npos);
    }
    stk.deallocate(s, 10);
    pool.deallocate(p, 64, 16);
    CHECK_EQ(mm.leak_count(), usize{0});
    CHECK_NOTHROW(mm.check_leaks());  // allocators still alive, nothing leaked

    const std::string r = mm.report();
    CHECK(r.find("particles (pool)") != std::string::npos);
    CHECK(r.find("frame (arena)") != std::string::npos);
    CHECK(r.find("scratch (linear)") != std::string::npos);
    CHECK(r.find("temp (stack)") != std::string::npos);

    CHECK(mm.destroy_allocator("particles"));
    CHECK(!mm.destroy_allocator("particles"));
    CHECK(mm.destroy_allocator("frame"));
    CHECK(mm.destroy_allocator("scratch"));
    CHECK(mm.destroy_allocator("temp"));
    CHECK_EQ(mm.allocator_count(), usize{0});
    CHECK_NOTHROW(mm.check_leaks());
    CHECK_EQ(mm.stats().live_allocations(), u64{0});  // the tracked heap was never involved
}

FLYG_TEST(memory_manager_heap_and_custom_allocator) {
    MemoryManager mm;
    Counted::alive = 0;
    Counted::throw_at = -1;
    Counted* c = mm.create<Counted>(5);
    CHECK_EQ(mm.leak_count(), usize{1});
    CHECK_THROWS_AS(mm.check_leaks(), MemoryException);
    mm.destroy(c);
    CHECK_NOTHROW(mm.check_leaks());

    void* raw = mm.allocate_tagged(128, 64, "audio-buffer");
    CHECK(mm.tracker().report().find("audio-buffer") != std::string::npos);
    mm.deallocate(raw, 128, 64);

    auto custom = std::make_unique<CountingAllocator>();
    CountingAllocator* raw_custom = custom.get();
    Allocator& ref = mm.adopt("custom", std::move(custom));
    void* q = ref.allocate(32, 8);
    ref.deallocate(q, 32, 8);
    CHECK_EQ(raw_custom->allocs, 1);
    CHECK_EQ(raw_custom->frees, 1);
    CHECK_THROWS_AS(mm.adopt("custom", std::make_unique<CountingAllocator>()), MemoryException);
    CHECK_THROWS_AS(mm.adopt("null", nullptr), MemoryException);

    CHECK(mm.stats().peak_bytes >= sizeof(Counted));
    CHECK_NOTHROW(MemoryManager::global().check_leaks());
}
