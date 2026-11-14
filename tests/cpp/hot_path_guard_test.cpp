#include "core/arena.hpp"
#include "core/hot_path_guard.hpp"
#include "core/log.hpp"
#include "core/spsc_ring.hpp"
#include "core/telemetry.hpp"
#include "core/triple_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <vector>

TEST_CASE("HotPathGuard catches injected heap allocations inside AETHER_HOT_PATH_SCOPE", "[core][hot-path]") {
#if !defined(AETHER_ENABLE_HOT_PATH_GUARD) || !AETHER_ENABLE_HOT_PATH_GUARD
    SUCCEED("Hot-path guard compiled off in this preset");
#else
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_thread_violations();
    REQUIRE_FALSE(aether::HotPathGuard::is_active());

    // Pre-allocate core primitives before entering hot path.
    aether::FrameArena arena(64ULL * 1024ULL);
    aether::LockFreeTripleBuffer<uint64_t> tb;
    aether::SpscRing<uint64_t, 64> ring;
    aether::RollingLatencyStats stats;
    aether::FixedVector<uint32_t, 32> vec;

    {
        AETHER_HOT_PATH_SCOPE;
        REQUIRE(aether::HotPathGuard::is_active());

        // Zero-alloc core operations inside hot path must cause 0 violations.
        arena.reset();
        auto* arr = arena.alloc<uint32_t>(128);
        REQUIRE(arr != nullptr);
        tb.publish(42ULL);
        (void)tb.acquire();
        (void)ring.try_push(7ULL);
        uint64_t tmp = 0;
        (void)ring.try_pop(tmp);
        stats.add_sample_ms(12.3f);
        (void)stats.compute_summary();
        (void)vec.push_back(1U);
        AETHER_LOG_INFO("test", "zero alloc log message %u", 42U);

        CHECK(aether::HotPathGuard::thread_violations() == 0);

        // Nested scope must preserve guard cleanly.
        {
            AETHER_HOT_PATH_SCOPE;
            volatile int* p = new int(99);
            delete p;
        }
        REQUIRE(aether::HotPathGuard::is_active());
        CHECK(aether::HotPathGuard::thread_violations() >= 1);

        // Injected std::vector growth and malloc must trigger violations.
        const uint64_t before_vec = aether::HotPathGuard::thread_violations();
        std::vector<int> bad_vec;
        bad_vec.push_back(1);
        bad_vec.push_back(2);
        CHECK(aether::HotPathGuard::thread_violations() > before_vec);

#if defined(_DEBUG)
        const uint64_t before_malloc = aether::HotPathGuard::thread_violations();
        void* raw = std::malloc(64);
        CHECK(raw != nullptr);
        CHECK(aether::HotPathGuard::thread_violations() > before_malloc);
        std::free(raw);
#endif
    }

    REQUIRE_FALSE(aether::HotPathGuard::is_active());
    // Reset thread counter cleanly so subsequent tests start at 0 violations.
    aether::HotPathGuard::reset_thread_violations();
    aether::HotPathGuard::reset_total_violations();
#endif
}
