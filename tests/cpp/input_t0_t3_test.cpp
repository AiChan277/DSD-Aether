#include "app/input_events.hpp"
#include "app/pipeline_skeleton.hpp"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

TEST_CASE("InputEventRing drops continuous mouse moves under flood while preserving KeyDown", "[app][input]") {
    aether::app::InputEventRing ring;

    // Flood beyond usable non-reserved capacity (`512 - 64 = 448`).
    for (int i = 0; i < 650; ++i) {
        aether::app::InputEvent move{};
        move.kind = aether::app::InputEventKind::MouseMove;
        move.x = static_cast<float>(i);
        move.y = static_cast<float>(i * 2);
        (void)ring.push(move);
    }
    REQUIRE(ring.dropped_moves() > 0);
    REQUIRE(ring.dropped_critical() == 0);

    // Push critical discrete KeyDown events into the reserved tail slots.
    for (uint32_t k = 0; k < 16; ++k) {
        aether::app::InputEvent key{};
        key.kind = aether::app::InputEventKind::KeyDown;
        key.code = 'A' + k;
        REQUIRE(ring.push(key));
    }
    REQUIRE(ring.dropped_critical() == 0);

    uint32_t keys_seen = 0;
    aether::app::InputEvent popped{};
    while (ring.pop(popped)) {
        if (popped.kind == aether::app::InputEventKind::KeyDown) {
            REQUIRE(popped.code == ('A' + keys_seen));
            ++keys_seen;
        }
    }
    REQUIRE(keys_seen == 16);
}

TEST_CASE("PipelineSkeleton executes T1/T2/T4 synthetic cadence without hot-path allocations", "[app][pipeline]") {
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_total_violations();

    const auto topo = aether::detect_cpu_topology();
    aether::app::PipelineSkeleton pipeline;
    REQUIRE(pipeline.start(topo));

    aether::HighResTimer timer;
    timer.sleep_us(60'000ULL);

    aether::app::SkinningSnapshot snap{};
    (void)pipeline.acquire_skinning(snap);
    REQUIRE(pipeline.total_t1_frames() >= 1);
    REQUIRE(pipeline.total_t2_ticks() >= 2);

    aether::app::OutputWorkItem work{};
    work.frame_sequence = snap.frame_sequence;
    work.t1_capture_us = snap.t1_capture_us;
    work.t2_start_us = snap.t2_start_us;
    work.t2_done_us = snap.t2_done_us;
    work.t3_start_us = aether::now_us();
    work.t3_submit_us = work.t3_start_us + 200;
    pipeline.submit_rendered_frame(work);

    timer.sleep_us(30'000ULL);
    REQUIRE(pipeline.total_t4_frames() >= 1);

    pipeline.stop();
    CHECK(aether::HotPathGuard::total_violations() == 0);
}

TEST_CASE("Live Phase 3 PipelineSkeleton executes TrackingEngine T1 -> SimEngine T2 -> T3 -> T4 cleanly",
          "[app][pipeline][phase3]") {
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_total_violations();

    const auto hw = aether::enumerate_hardware();
    aether::app::PipelineConfig cfg{};
    cfg.enable_live_tracking = true;
    cfg.use_synthetic_camera = true;
    cfg.preferred_ep = -1;
    cfg.manifest_path = std::filesystem::path(AETHER_SOURCE_DIR) / "models" / "manifest.toml";
    cfg.ov_cache_dir = std::filesystem::temp_directory_path() / "aether_test_ov_cache";

    auto pipeline = std::make_unique<aether::app::PipelineSkeleton>();
    REQUIRE(pipeline->start(hw, cfg));

    aether::HighResTimer timer;
    bool hud_ready = false;
    const uint64_t deadline = aether::now_us() + 15'000'000ULL;
    while (aether::now_us() < deadline) {
        const auto hud = pipeline->latest_tracking_hud();
        if (hud.ready && pipeline->total_t1_frames() >= 3 && pipeline->total_t2_ticks() >= 6) {
            hud_ready = true;
            break;
        }
        timer.sleep_us(20'000ULL);
    }
    REQUIRE(hud_ready);

    const auto hud = pipeline->latest_tracking_hud();
    CHECK(hud.enabled);
    CHECK(hud.ready);
    CHECK(hud.frames_processed >= 3ULL);
    CHECK(std::string(hud.active_ep_label).size() > 0U);

    aether::app::SkinningSnapshot snap{};
    (void)pipeline->acquire_skinning(snap);
    CHECK(snap.frame.bone_count >= AETHER_HUMANOID_BONE_COUNT);
    CHECK(snap.frame.morph_count >= 52U);

    aether::app::OutputWorkItem work{};
    work.frame_sequence = snap.frame_sequence;
    work.t1_capture_us = snap.t1_capture_us;
    work.t2_start_us = snap.t2_start_us;
    work.t2_done_us = snap.t2_done_us;
    work.t3_start_us = aether::now_us();
    work.t3_submit_us = work.t3_start_us + 200;
    pipeline->submit_rendered_frame(work);

    timer.sleep_us(30'000ULL);
    CHECK(pipeline->total_t4_frames() >= 1ULL);

    pipeline->stop();
    CHECK(aether::HotPathGuard::total_violations() == 0);
}
