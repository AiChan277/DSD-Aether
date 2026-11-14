#include "core/hw_enum.hpp"
#include "core/log.hpp"
#include "core/triple_buffer.hpp"
#include "tracking/camera_source.hpp"
#include "tracking/landmark_solver.hpp"
#include "tracking/ort_router.hpp"
#include "tracking/tracking_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace aether::tracking;

namespace {

std::filesystem::path repo_manifest_path() {
    return std::filesystem::path(AETHER_SOURCE_DIR) / "models" / "manifest.toml";
}

std::filesystem::path temp_ov_cache_dir() {
    auto p = std::filesystem::temp_directory_path() / "aether_test_ov_cache";
    std::filesystem::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("AetherTrackingRouter falls back cleanly through every EP tier and enforces 120-frame health demotion",
          "[tracking][router]") {
    const auto hw = aether::enumerate_hardware();
    const auto manifest = repo_manifest_path();
    REQUIRE(std::filesystem::exists(manifest));

    // Bitmask bits match EpTier enum:
    // 0=Cpu, 1=NpuOpenVino, 2=DirectMlIgpu, 3=DirectMlDgpu, 4=TensorRt, 5=GpuOpenVino
    // Force TRT + NPU + OV-iGPU + DML-iGPU + DML-dGPU to fail -> must cleanly land on CPU without crashing or leaking
    {
        AetherTrackingRouter router;
        TrackingRouterConfig cfg{};
        cfg.manifest_path = manifest;
        cfg.ov_cache_dir = temp_ov_cache_dir();
        cfg.preferred_ep = EpTier::TensorRt;
        cfg.forced_fail_ep_mask =
            (1U << static_cast<uint32_t>(EpTier::TensorRt)) | (1U << static_cast<uint32_t>(EpTier::NpuOpenVino)) |
            (1U << static_cast<uint32_t>(EpTier::GpuOpenVino)) | (1U << static_cast<uint32_t>(EpTier::DirectMlIgpu)) |
            (1U << static_cast<uint32_t>(EpTier::DirectMlDgpu));
        std::string err;
        REQUIRE(router.initialize(cfg, hw, &err) == AETHER_OK);
        CHECK(router.active_ep() == EpTier::Cpu);
        CHECK(router.run_m0_face_detector() == AETHER_OK);
        CHECK(router.run_m1_body() == AETHER_OK);
        CHECK(router.run_m1_hands() == AETHER_OK);
        CHECK(router.run_m2_face_landmarks() == AETHER_OK);
        CHECK(router.run_m3_blendshapes() == AETHER_OK);
        router.shutdown();
    }

    // Health monitor: force Tier 2 (iGPU DML) and inject 120 slow frames > 9.0 ms -> auto-demotes to dGPU or CPU!
    {
        AetherTrackingRouter router;
        TrackingRouterConfig cfg{};
        cfg.manifest_path = manifest;
        cfg.ov_cache_dir = temp_ov_cache_dir();
        cfg.preferred_ep = EpTier::DirectMlIgpu;
        cfg.forced_fail_ep_mask = (1U << static_cast<uint32_t>(EpTier::TensorRt)) |
                                  (1U << static_cast<uint32_t>(EpTier::NpuOpenVino)) |
                                  (1U << static_cast<uint32_t>(EpTier::GpuOpenVino));
        cfg.health_window_frames = 120;
        cfg.health_budget_ms = 9.0;
        std::string err;
        REQUIRE(router.initialize(cfg, hw, &err) == AETHER_OK);
        const auto start_ep = router.active_ep();
        if (start_ep != EpTier::Cpu) {
            std::string msg;
            for (int f = 0; f < 120; ++f) {
                router.record_frame_latency(12.5, &msg);
            }
            CHECK(router.demotion_count() >= 1);
            CHECK(router.active_ep() != start_ep);
            CHECK_FALSE(msg.empty());
        }
        router.shutdown();
    }
}

TEST_CASE("AetherTrackingRouter real host NPU/EP warm inference meets latency budget (PERF-002)",
          "[tracking][router][perf]") {
    const auto hw = aether::enumerate_hardware();
    const auto manifest = repo_manifest_path();
    REQUIRE(std::filesystem::exists(manifest));

    AetherTrackingRouter router;
    TrackingRouterConfig cfg{};
    cfg.manifest_path = manifest;
    cfg.ov_cache_dir = temp_ov_cache_dir();
    cfg.preferred_ep = EpTier::NpuOpenVino;
    cfg.enable_npu_fast_compile = true;
    std::string err;
    REQUIRE(router.initialize(cfg, hw, &err) == AETHER_OK);
    std::string drained_logs;
    aether::LogSystem::drain(
        [](const aether::LogRecord& r, void* ud) {
            auto* out = static_cast<std::string*>(ud);
            *out += r.message;
            *out += " | ";
        },
        &drained_logs);
    INFO("Active EP: " << router.active_ep_label() << " (enum=" << static_cast<int>(router.active_ep())
                       << ") npu.available=" << hw.npu.available << " logs=" << drained_logs);

    if (hw.npu.available) {
        CHECK((router.active_ep() == EpTier::NpuOpenVino || router.active_ep() == EpTier::DirectMlIgpu ||
               router.active_ep() == EpTier::DirectMlDgpu));
    }

    // Fill deterministic normalized input buffers
    std::fill(router.buffers().m1_body_in.begin(), router.buffers().m1_body_in.end(), 0.45f);
    std::fill(router.buffers().m2_face_in.begin(), router.buffers().m2_face_in.end(), 0.50f);
    std::fill(router.buffers().m1_hands_in.begin(), router.buffers().m1_hands_in.end(), 0.40f);
    std::fill(router.buffers().m3_in.begin(), router.buffers().m3_in.end(), 100.0f);

    // Warmup
    for (int w = 0; w < 15; ++w) {
        REQUIRE(router.run_m1_body() == AETHER_OK);
        REQUIRE(router.run_m2_face_landmarks() == AETHER_OK);
        REQUIRE(router.run_m3_blendshapes() == AETHER_OK);
    }

    std::vector<double> samples_ms;
    std::vector<double> m1_ms;
    std::vector<double> m2_ms;
    std::vector<double> m3_ms;
    std::vector<double> m23_ms;
    samples_ms.reserve(50);
    m1_ms.reserve(50);
    m2_ms.reserve(50);
    m3_ms.reserve(50);
    m23_ms.reserve(50);
    for (int i = 0; i < 50; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(router.run_m1_body() == AETHER_OK);
        const auto t1_m1 = std::chrono::steady_clock::now();
        REQUIRE(router.run_m2_face_landmarks() == AETHER_OK);
        const auto t2_m2 = std::chrono::steady_clock::now();
        REQUIRE(router.run_m3_blendshapes() == AETHER_OK);
        const auto t3 = std::chrono::steady_clock::now();
        m1_ms.push_back(std::chrono::duration<double, std::milli>(t1_m1 - t0).count());
        m2_ms.push_back(std::chrono::duration<double, std::milli>(t2_m2 - t1_m1).count());
        m3_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t2_m2).count());
        m23_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t1_m1).count());
        samples_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t0).count());
    }
    std::sort(samples_ms.begin(), samples_ms.end());
    std::sort(m1_ms.begin(), m1_ms.end());
    std::sort(m2_ms.begin(), m2_ms.end());
    std::sort(m3_ms.begin(), m3_ms.end());
    std::sort(m23_ms.begin(), m23_ms.end());
    const double p95_ms = samples_ms[(samples_ms.size() * 95) / 100];
    const double p95_m1 = m1_ms[(m1_ms.size() * 95) / 100];
    const double p95_m2 = m2_ms[(m2_ms.size() * 95) / 100];
    const double p95_m3 = m3_ms[(m3_ms.size() * 95) / 100];
    const double p95_m23 = m23_ms[(m23_ms.size() * 95) / 100];
    INFO("Face+Body+Blendshapes p95=" << p95_ms << " ms (M1_body=" << p95_m1 << ", M2_face=" << p95_m2 << ", M3_bs="
                                      << p95_m3 << ", M2+M3_p95=" << p95_m23 << ") on " << router.active_ep_label());
    REQUIRE(router.buffers().m3_blendshapes.size() == 52);
#if defined(NDEBUG)
    // §8.4 cadence: M2_face + M3_bs run every frame (<= 6.0 ms on NPU / GPU), while M1_body runs every 2nd frame
    // above 60 Hz; 13-TOPS Arrow Lake-HX NPU runs all 3 sequentially in ~9-10.5 ms vs 48-TOPS RM-A <= 6.0 ms.
    const double face_bs_budget = (router.active_ep() == EpTier::Cpu) ? 15.0 : 6.0;
    CHECK(p95_m23 <= face_bs_budget);
    const double full_budget = (router.active_ep() == EpTier::Cpu) ? 25.0 : 12.0;
    CHECK(p95_ms <= full_budget);
#endif
    router.shutdown();

    // Benchmark Tier 2A OpenVINO iGPU if present cleanly
    {
        AetherTrackingRouter igpu_router;
        TrackingRouterConfig igpu_cfg{};
        igpu_cfg.manifest_path = manifest;
        igpu_cfg.ov_cache_dir = temp_ov_cache_dir();
        igpu_cfg.preferred_ep = EpTier::GpuOpenVino;
        if (igpu_router.initialize(igpu_cfg, hw, &err) == AETHER_OK && igpu_router.active_ep() == EpTier::GpuOpenVino) {
            std::fill(igpu_router.buffers().m1_body_in.begin(), igpu_router.buffers().m1_body_in.end(), 0.45f);
            std::fill(igpu_router.buffers().m2_face_in.begin(), igpu_router.buffers().m2_face_in.end(), 0.50f);
            std::fill(igpu_router.buffers().m3_in.begin(), igpu_router.buffers().m3_in.end(), 100.0f);
            for (int w = 0; w < 15; ++w) {
                REQUIRE(igpu_router.run_m1_body() == AETHER_OK);
                REQUIRE(igpu_router.run_m2_face_landmarks() == AETHER_OK);
                REQUIRE(igpu_router.run_m3_blendshapes() == AETHER_OK);
            }
            std::vector<double> g_samples;
            std::vector<double> g_m23_samples;
            g_samples.reserve(50);
            g_m23_samples.reserve(50);
            for (int i = 0; i < 50; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                REQUIRE(igpu_router.run_m1_body() == AETHER_OK);
                const auto t1_m1 = std::chrono::steady_clock::now();
                REQUIRE(igpu_router.run_m2_face_landmarks() == AETHER_OK);
                REQUIRE(igpu_router.run_m3_blendshapes() == AETHER_OK);
                const auto t1 = std::chrono::steady_clock::now();
                g_m23_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t1_m1).count());
                g_samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            std::sort(g_samples.begin(), g_samples.end());
            std::sort(g_m23_samples.begin(), g_m23_samples.end());
            const double g_p50 = g_samples[g_samples.size() / 2];
            const double g_p95 = g_samples[(g_samples.size() * 95) / 100];
            const double g_m23_p95 = g_m23_samples[(g_m23_samples.size() * 95) / 100];
            INFO("Tier 2A OpenVINO iGPU Face+Body+Blendshapes p50=" << g_p50 << " ms, p95=" << g_p95
                                                                    << " ms, M2+M3 p95=" << g_m23_p95 << " ms");
#if defined(NDEBUG)
            CHECK(g_p50 <= 15.0);
            CHECK(g_p95 <= 35.0);
#endif
            igpu_router.shutdown();
        }
    }
}

TEST_CASE("Hand ROI 5% border margin and >= 0.30 confidence gating", "[tracking][roi]") {
    std::array<float, 39 * 5> pose{};
    const float pose_flag = 0.95f;

    // MediaPipe pose indices: 15=left_wrist, 17=left_pinky, 19=left_index; 16=right_wrist, 18=right_pinky,
    // 20=right_index Place left hand safely inside frame with high visibility
    for (int idx : {15, 17, 19}) {
        pose[idx * 5 + 0] = 0.30f * 256.0f;
        pose[idx * 5 + 1] = 0.55f * 256.0f;
        pose[idx * 5 + 3] = 5.0f; // high logit -> sigmoid > 0.99
        pose[idx * 5 + 4] = 5.0f;
    }
    // Place right wrist near bottom-edge (within 5% border margin -> must gate OFF!)
    for (int idx : {16, 18, 20}) {
        pose[idx * 5 + 0] = 0.80f * 256.0f;
        pose[idx * 5 + 1] = 0.97f * 256.0f;
        pose[idx * 5 + 3] = 5.0f;
        pose[idx * 5 + 4] = 5.0f;
    }

    const HandRoiDecision d1 = compute_hand_rois_from_pose(pose.data(), pose_flag, 0.30f, 0.05f);
    CHECK(d1.run_left);
    CHECK_FALSE(d1.run_right);

    // Drop left wrist visibility below 0.30 -> must gate OFF!
    pose[15 * 5 + 3] = -2.0f; // sigmoid(-2) ~= 0.119 < 0.30
    const HandRoiDecision d2 = compute_hand_rois_from_pose(pose.data(), pose_flag, 0.30f, 0.05f);
    CHECK_FALSE(d2.run_left);
}

TEST_CASE("FileCamera synthetic stream drives TrackingEngine into LockFreeTripleBuffer cleanly", "[tracking][e2e]") {
    const auto hw = aether::enumerate_hardware();
    const auto manifest = repo_manifest_path();
    REQUIRE(std::filesystem::exists(manifest));

    auto cam = create_file_camera_source({}, true, 640, 480, 60, 12);
    TrackingRouterConfig cfg{};
    cfg.manifest_path = manifest;
    cfg.ov_cache_dir = temp_ov_cache_dir();
    cfg.preferred_ep = EpTier::NpuOpenVino;

    TrackingEngine engine;
    std::string err;
    REQUIRE(engine.initialize(cfg, hw, std::move(cam), &err) == AETHER_OK);

    aether::LockFreeTripleBuffer<AetherTrackingFrame> tb;
    uint32_t valid_frames = 0;
    uint64_t prev_ts = 0;
    for (int i = 0; i < 6; ++i) {
        if (engine.step_once(tb, 100, false)) {
            REQUIRE(tb.acquire());
            const auto& f = tb.latest();
            CHECK(f.frame_sequence > valid_frames);
            CHECK(f.capture_timestamp_us > prev_ts);
            prev_ts = f.capture_timestamp_us;
            ++valid_frames;
        }
    }
    CHECK(valid_frames >= 5);
    engine.shutdown();
}

TEST_CASE("Task 1: Hand tracking is disabled by default and emits zero hand confidence",
          "[tracking][router][no-hands]") {
    TrackingRouterConfig cfg{};
    CHECK_FALSE(cfg.enable_hand_tracking);

    const auto hw = aether::enumerate_hardware();
    const auto manifest = repo_manifest_path();
    REQUIRE(std::filesystem::exists(manifest));
    cfg.manifest_path = manifest;
    cfg.ov_cache_dir = temp_ov_cache_dir();
    cfg.preferred_ep = EpTier::Cpu;

    auto cam = create_file_camera_source({}, true, 640, 480, 60, 12);
    REQUIRE(cam != nullptr);
    TrackingEngine engine;
    std::string err;
    REQUIRE(engine.initialize(cfg, hw, std::move(cam), &err) == AETHER_OK);

    aether::LockFreeTripleBuffer<AetherTrackingFrame> tb;
    bool got_frame = false;
    for (int i = 0; i < 6; ++i) {
        if (engine.step_once(tb, 100, true) && tb.acquire()) {
            const auto& f = tb.latest();
            CHECK(f.left_hand_confidence == 0.0f);
            CHECK(f.right_hand_confidence == 0.0f);
            CHECK((f.valid_flags & 0x06U) == 0U);
            got_frame = true;
        }
    }
    REQUIRE(got_frame);
    const auto stats = engine.snapshot_stats();
    CHECK(stats.left_hand_confidence == 0.0f);
    CHECK(stats.right_hand_confidence == 0.0f);

    engine.shutdown();
}
