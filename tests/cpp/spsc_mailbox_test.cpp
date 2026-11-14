#include "core/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>

TEST_CASE("SpscRing enforces FIFO, overflow drop, and wait-free SPSC integrity", "[core][spsc]") {
    aether::SpscRing<uint64_t, 8> small;
    for (uint64_t i = 0; i < 8; ++i) {
        REQUIRE(small.try_push(i + 100));
    }
    REQUIRE(small.try_push(999) == false);
    REQUIRE(small.dropped_count() == 1);

    for (uint64_t i = 0; i < 8; ++i) {
        uint64_t v = 0;
        REQUIRE(small.try_pop(v));
        REQUIRE(v == i + 100);
    }
    uint64_t empty = 0;
    REQUIRE(small.try_pop(empty) == false);

    // Reserve slots policy keeps room for critical items.
    for (uint64_t i = 0; i < 6; ++i) {
        REQUIRE(small.try_push_with_reserve(i, 2));
    }
    REQUIRE(small.try_push_with_reserve(99, 2) == false);
    REQUIRE(small.try_push(777) == true);
    REQUIRE(small.try_push(778) == true);

    // Multi-threaded 1,000,000 item SPSC check with exact backpressure retry accounting.
    constexpr uint64_t kCount = 1'000'000ULL;
    aether::SpscRing<uint64_t, 4096> ring;
    uint64_t failed_pushes = 0;
    std::thread prod([&]() {
        for (uint64_t i = 1; i <= kCount; ++i) {
            while (!ring.try_push(i)) {
                ++failed_pushes;
                std::this_thread::yield();
            }
        }
    });
    std::thread cons([&]() {
        uint64_t expected = 1;
        while (expected <= kCount) {
            uint64_t val = 0;
            if (ring.try_pop(val)) {
                REQUIRE(val == expected);
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    prod.join();
    cons.join();
    REQUIRE(ring.dropped_count() == failed_pushes);
}

TEST_CASE("ParamMailbox delivers latest value with strictly monotonic epochs", "[core][mailbox]") {
    aether::ParamMailbox<float> mb;
    uint64_t seen_epoch = 0;
    float out = -1.0f;

    REQUIRE(mb.poll(seen_epoch, out) == false);
    REQUIRE(mb.store(1.0f) == 1);
    REQUIRE(mb.store(2.0f) == 2);
    REQUIRE(mb.store(3.5f) == 3);

    REQUIRE(mb.poll(seen_epoch, out) == true);
    REQUIRE(seen_epoch == 3);
    REQUIRE(out == 3.5f);

    REQUIRE(mb.poll(seen_epoch, out) == false);

    REQUIRE(mb.store(9.25f) == 4);
    REQUIRE(mb.poll(seen_epoch, out) == true);
    REQUIRE(seen_epoch == 4);
    REQUIRE(out == 9.25f);
}
