#include "core/clock.hpp"
#include "core/triple_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

struct TestPayload {
    uint64_t sequence{0};
    uint64_t checksum{0};
    std::array<uint64_t, 6> words{};
};

uint64_t compute_checksum(uint64_t seq) noexcept {
    return (seq * 0x9E3779B97F4A7C15ULL) ^ 0xA5A55A5A12345678ULL;
}

} // namespace

TEST_CASE("LockFreeTripleBuffer survives 10^8 concurrent publish iterations without torn reads",
          "[core][triple-buffer]") {
    constexpr uint64_t kIterations = 100'000'000ULL;
    aether::LockFreeTripleBuffer<TestPayload> tb;
    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> consumer_reads{0};
    std::atomic<uint64_t> torn_reads{0};
    std::atomic<uint64_t> non_monotonic_reads{0};

    const uint64_t t_start = aether::now_us();

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= kIterations; ++i) {
            auto& st = tb.staging();
            st.sequence = i;
            st.checksum = compute_checksum(i);
            st.words.fill(st.checksum ^ i);
            tb.publish();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint64_t last_seq = 0;
        uint64_t reads = 0;
        while (!producer_done.load(std::memory_order_acquire)) {
            if (tb.acquire()) {
                const auto& cur = tb.latest();
                if (cur.sequence < last_seq) {
                    non_monotonic_reads.fetch_add(1, std::memory_order_relaxed);
                }
                const uint64_t expected_chk = compute_checksum(cur.sequence);
                if (cur.checksum != expected_chk || cur.words[0] != (expected_chk ^ cur.sequence) ||
                    cur.words[5] != (expected_chk ^ cur.sequence)) {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                last_seq = cur.sequence;
                ++reads;
            }
        }
        // Final drain after producer finishes.
        if (tb.acquire()) {
            const auto& cur = tb.latest();
            if (cur.checksum != compute_checksum(cur.sequence)) {
                torn_reads.fetch_add(1, std::memory_order_relaxed);
            }
            last_seq = cur.sequence;
            ++reads;
        }
        REQUIRE(last_seq == kIterations);
        consumer_reads.store(reads, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    const uint64_t elapsed_us = aether::now_us() - t_start;
    std::printf("[TRIPLE_BUFFER_BENCH] iterations=%llu consumer_reads=%llu elapsed_ms=%.2f\n",
                static_cast<unsigned long long>(kIterations), static_cast<unsigned long long>(consumer_reads.load()),
                static_cast<double>(elapsed_us) * 0.001);

    REQUIRE(torn_reads.load() == 0);
    REQUIRE(non_monotonic_reads.load() == 0);
    REQUIRE(consumer_reads.load() > 0);
}

TEST_CASE("LockFreeTripleBuffer consumer remains non-blocking when producer pauses", "[core][triple-buffer]") {
    aether::LockFreeTripleBuffer<TestPayload> tb;
    TestPayload initial{};
    initial.sequence = 777;
    initial.checksum = compute_checksum(777);
    tb.publish(initial);

    REQUIRE(tb.acquire() == true);
    REQUIRE(tb.latest().sequence == 777);

    // Producer completely paused: repeated consumer calls must return immediately (`false`).
    const uint64_t begin_us = aether::now_us();
    for (int i = 0; i < 10000; ++i) {
        REQUIRE(tb.acquire() == false);
        REQUIRE(tb.latest().sequence == 777);
    }
    const uint64_t elapsed_us = aether::now_us() - begin_us;
    CHECK(elapsed_us < 10'000ULL);
}
