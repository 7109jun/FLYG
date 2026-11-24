// flyG example - memory module: allocators, tracking and leak detection
// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>

#include <flyg/flyg.hpp>

struct Particle {
    float x, y, z;
    float life;
};

int main() {
    using namespace flyg;
    std::printf("flyG %.*s on %.*s/%.*s (%.*s)\n", static_cast<int>(core::version_string.size()),
                core::version_string.data(),
                static_cast<int>(core::to_string(core::os).size()), core::to_string(core::os).data(),
                static_cast<int>(core::to_string(core::arch).size()),
                core::to_string(core::arch).data(),
                static_cast<int>(core::to_string(core::compiler).size()),
                core::to_string(core::compiler).data());

    try {
        memory::MemoryManager mm;

        // pool for fixed-size objects
        auto& particles = mm.create_pool("particles", sizeof(Particle), alignof(Particle), 128);
        Particle* p = ::new (particles.allocate(sizeof(Particle), alignof(Particle)))
            Particle{1.0f, 2.0f, 3.0f, 0.5f};
        std::printf("particle at %p life=%.2f\n", static_cast<void*>(p), static_cast<double>(p->life));
        particles.deallocate(p, sizeof(Particle), alignof(Particle));

        // per-frame arena: allocate freely, reset once
        auto& frame = mm.create_arena("frame", 4 * core::KiB);
        for (int i = 0; i < 1000; ++i) (void)frame.create<Particle>(Particle{0, 0, 0, 1});
        std::printf("arena used %s of %s reserved\n", core::format_bytes(frame.bytes_used()).c_str(),
                    core::format_bytes(frame.bytes_reserved()).c_str());
        frame.reset();

        // tracked heap allocation with a tag; the reports show it as live until it is freed
        void* leak = mm.allocate_tagged(256, 16, "example-leak");
        std::printf("%s", mm.report().c_str());
        std::printf("%s", mm.tracker().report().c_str());
        mm.deallocate(leak, 256, 16);
        mm.check_leaks();  // throws if anything is still live
        std::printf("no leaks\n");
    } catch (const flyg::Exception& e) {
        std::fprintf(stderr, "flyG error: %s\n", e.what());
        return 1;
    }
    return 0;
}
