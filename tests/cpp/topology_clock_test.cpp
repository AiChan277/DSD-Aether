#include "aether_ffi_contract.h"
#include "core/arena.hpp"
#include "core/clock.hpp"
#include "core/cpu_topology.hpp"
#include "core/crash_handler.hpp"
#include "core/hw_enum.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <atomic>
#include <thread>

TEST_CASE("Monotonic QPC clock and high-resolution waitable timer pacing", "[core][clock]") {
    REQUIRE(aether::qpc_frequency_hz() > 0);

    uint64_t prev = aether::now_us();
    for (int i = 0; i < 10000; ++i) {
        const uint64_t cur = aether::now_us();
        REQUIRE(cur >= prev);
        prev = cur;
    }

    aether::HighResTimer timer;
    REQUIRE(timer.valid());
    const uint64_t t0 = aether::now_us();
    timer.sleep_us(4167ULL);
    const uint64_t elapsed = aether::now_us() - t0;
    CHECK(elapsed >= 3500ULL);
    CHECK(elapsed < 30'000ULL);

#if defined(_WIN32)
    HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(ev != nullptr);
    std::thread waker([&]() {
        aether::HighResTimer t;
        t.sleep_us(2000ULL);
        ::SetEvent(ev);
    });
    const uint64_t w0 = aether::now_us();
    const bool signaled = timer.wait_event_or_deadline_us(ev, w0 + 100'000ULL);
    const uint64_t wait_elapsed = aether::now_us() - w0;
    waker.join();
    ::CloseHandle(ev);
    REQUIRE(signaled);
    CHECK(wait_elapsed < 50'000ULL);
#endif
}

TEST_CASE("CPU topology, hardware enumeration, paths, logs, telemetry, crash handler", "[core][topology]") {
    const auto paths = aether::discover_runtime_paths();
    REQUIRE_FALSE(paths.exe_dir.empty());
    REQUIRE_FALSE(paths.crash_dir.empty());

    REQUIRE(aether::CrashHandler::install(paths.crash_dir));
    REQUIRE(aether::CrashHandler::is_installed());

    const auto hw = aether::enumerate_hardware();
    REQUIRE(hw.cpu.physical_cores > 0);
    REQUIRE(hw.cpu.logical_processors >= hw.cpu.physical_cores);
    REQUIRE(hw.total_ram_mb > 0);
    REQUIRE(hw.npu.probed);
    REQUIRE(hw.npu.ffi_status == AETHER_OK);

    const auto placement = aether::apply_thread_placement(aether::ThreadRole::SimT2, hw.cpu);
    CHECK(placement.priority_applied);
    if (hw.cpu.is_hybrid) {
        CHECK(hw.cpu.p_core_count > 0);
        CHECK(hw.cpu.e_core_count > 0);
        CHECK(placement.cpu_set_applied);
    }

    // Drain logs written above cleanly.
    std::atomic<uint32_t> drained_count{0};
    (void)aether::LogSystem::drain(
        [](const aether::LogRecord& rec, void* ud) {
            auto* cnt = static_cast<std::atomic<uint32_t>*>(ud);
            if (rec.timestamp_us > 0) {
                cnt->fetch_add(1, std::memory_order_relaxed);
            }
        },
        &drained_count, 128);
    CHECK(drained_count.load() > 0);

    // Telemetry percentiles sanity check.
    aether::RollingLatencyStats stats;
    for (int i = 1; i <= 100; ++i) {
        stats.add_sample_ms(static_cast<float>(i) * 0.1f);
    }
    const auto summary = stats.compute_summary();
    CHECK(summary.sample_count == 100);
    CHECK(summary.p50_ms >= 4.5f);
    CHECK(summary.p95_ms >= 9.0f);
    CHECK(summary.p99_ms >= 9.5f);

    // Pool & FrameArena smoke.
    aether::Pool<uint64_t, 4> pool;
    auto* a = pool.acquire();
    auto* b = pool.acquire();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(pool.available() == 2);
    pool.release(a);
    pool.release(b);
    REQUIRE(pool.available() == 4);

    aether::CrashHandler::shutdown();
    REQUIRE_FALSE(aether::CrashHandler::is_installed());
}
