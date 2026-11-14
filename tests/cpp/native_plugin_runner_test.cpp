#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>

#include "native_plugin_manager.hpp"

namespace {

void wait_ms(uint32_t ms) noexcept {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        return;
    }
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        (void)WaitForSingleObject(timer, ms + 250u);
    }
    CloseHandle(timer);
}

std::filesystem::path find_test_plugin_dll() {
    std::array<wchar_t, 1024> buf{};
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    REQUIRE(len > 0);
    const std::filesystem::path exe_dir = std::filesystem::path(buf.data()).parent_path();
    const std::filesystem::path dll = exe_dir / L"test_native_plugin.dll";
    REQUIRE(std::filesystem::exists(dll));
    return dll;
}

bool wait_for_restart_and_running(aether::native_plugin::NativePluginManager& mgr, uint32_t slot, uint32_t min_restarts,
                                  uint32_t max_wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto h = mgr.health(slot);
        if (h.state == aether::native_plugin::PluginState::Running && h.restart_count >= min_restarts) {
            return true;
        }
        if (h.permanently_disabled) {
            return false;
        }
        wait_ms(20);
    }
    return false;
}

bool wait_until_disabled(aether::native_plugin::NativePluginManager& mgr, uint32_t slot, uint32_t max_wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto h = mgr.health(slot);
        if (h.permanently_disabled && h.state == aether::native_plugin::PluginState::Disabled) {
            return true;
        }
        wait_ms(20);
    }
    return false;
}

} // namespace

TEST_CASE("NativePluginManager runs normal IPC and enforces Job Object limits", "[phase6][native_plugin]") {
    aether::native_plugin::NativePluginManager mgr;
    aether::native_plugin::NativePluginConfig cfg{};
    cfg.dll_path = find_test_plugin_dll();

    const int32_t slot = mgr.load_plugin(cfg);
    REQUIRE(slot >= 0);
    const uint32_t sid = static_cast<uint32_t>(slot);

    REQUIRE(mgr.wait_until_ready(sid, 5000));

    const auto h = mgr.health(sid);
    CHECK(h.state == aether::native_plugin::PluginState::Running);
    CHECK(h.job_object_active);
    CHECK(std::string(h.desc.id) == "com.aether.test.native");

    // Normal mode 0: inputs[i] * 2 + 1
    std::array<float, 4> in0{0.0f, 3.0f, -2.5f, 7.5f};
    std::array<float, 4> out0{};
    int32_t plugin_rc = -999;
    REQUIRE(mgr.process_sync(sid, 1, 0.016, in0.data(), 4, out0.data(), 4, 2000, &plugin_rc));
    CHECK(plugin_rc == 0);
    CHECK(out0[0] == Catch::Approx(1.0f));
    CHECK(out0[1] == Catch::Approx(7.0f));
    CHECK(out0[2] == Catch::Approx(-4.0f));
    CHECK(out0[3] == Catch::Approx(16.0f));

    // Mode 4: ActiveProcessLimit = 1 blocks spawning child processes
    std::array<float, 4> in_spawn{4.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> out_spawn{};
    REQUIRE(mgr.process_sync(sid, 2, 0.033, in_spawn.data(), 4, out_spawn.data(), 4, 2000, &plugin_rc));
    CHECK(plugin_rc == 0);
    CHECK(out_spawn[0] == Catch::Approx(1.0f));

    // Mode 2: >512 MiB OOM bomb rejected cleanly without killing host
    std::array<float, 4> in_oom{2.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> out_oom{};
    std::string err{};
    const bool oom_ok = mgr.process_sync(sid, 3, 0.050, in_oom.data(), 4, out_oom.data(), 4, 4000, &plugin_rc, &err);
    CHECK(!oom_ok);
    CHECK((plugin_rc == -12 || mgr.health(sid).crash_count > 0 || !err.empty()));

    mgr.unload_plugin(sid);
}

TEST_CASE("NativePluginManager recovers from DLL crash and enforces 3-restart watchdog cap on hangs",
          "[phase6][native_plugin]") {
    aether::native_plugin::NativePluginManager mgr;
    aether::native_plugin::NativePluginConfig cfg{};
    cfg.dll_path = find_test_plugin_dll();
    cfg.heartbeat_timeout_ms = 500;
    cfg.max_restarts_in_window = 3;
    cfg.restart_window_sec = 300;
    cfg.backoff_ms = {50, 150, 450};

    // Part A: crash containment and automatic backoff restart (restarts 1 -> continues working)
    const int32_t crash_slot = mgr.load_plugin(cfg);
    REQUIRE(crash_slot >= 0);
    const uint32_t csid = static_cast<uint32_t>(crash_slot);
    REQUIRE(mgr.wait_until_ready(csid, 5000));

    std::array<float, 4> in_crash{1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> out_crash{};
    CHECK_FALSE(mgr.process_sync(csid, 1, 0.016, in_crash.data(), 4, out_crash.data(), 4, 2000));

    REQUIRE(wait_for_restart_and_running(mgr, csid, 1, 4000));
    const auto post_crash = mgr.health(csid);
    CAPTURE(post_crash.crash_count, post_crash.hang_kill_count, post_crash.last_exit_code, post_crash.last_error);
    CHECK(post_crash.crash_count >= 1);
    CHECK(post_crash.restart_count == 1);
    CHECK_FALSE(post_crash.permanently_disabled);

    std::array<float, 4> in_ok{0.25f, 5.0f, 0.0f, 0.0f};
    std::array<float, 4> out_ok{};
    REQUIRE(mgr.process_sync(csid, 2, 0.033, in_ok.data(), 4, out_ok.data(), 4, 2000));
    CHECK(out_ok[1] == Catch::Approx(11.0f));
    mgr.unload_plugin(csid);

    // Part B: 500 ms heartbeat watchdog kills hangs, backs off 50/150/450 ms, disables on 4th failure
    const int32_t hang_slot = mgr.load_plugin(cfg);
    REQUIRE(hang_slot >= 0);
    const uint32_t hsid = static_cast<uint32_t>(hang_slot);
    REQUIRE(mgr.wait_until_ready(hsid, 5000));

    std::array<float, 4> in_hang{3.0f, 0.0f, 0.0f, 0.0f};
    uint32_t expected_restarts = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t seq = 0;
        REQUIRE(mgr.try_submit_process(hsid, static_cast<uint64_t>(attempt + 1), 0.016 * (attempt + 1), in_hang.data(),
                                       4, 4, &seq));
        ++expected_restarts;
        REQUIRE(wait_for_restart_and_running(mgr, hsid, expected_restarts, 4000));
    }

    // 4th hang failure within 300s must permanently disable instead of restarting a 4th time
    REQUIRE(mgr.try_submit_process(hsid, 4, 0.066, in_hang.data(), 4, 4));
    REQUIRE(wait_until_disabled(mgr, hsid, 4000));

    const auto final_health = mgr.health(hsid);
    CHECK(final_health.permanently_disabled);
    CHECK(final_health.state == aether::native_plugin::PluginState::Disabled);
    CHECK(final_health.restart_count == 3);
    CHECK(final_health.hang_kill_count >= 4);

    mgr.unload_plugin(hsid);
}
