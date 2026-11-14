#include "animation/sim_engine.hpp"
#include "animation/spring_bone.hpp"
#include "core/hot_path_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>

using namespace aether::animation;

namespace {

void populate_stress_spring_rig(SpringBoneSimulator& sim, uint32_t joint_count, uint32_t collider_count) {
    sim.clear();
    for (uint32_t c = 0; c < collider_count; ++c) {
        const float angle = static_cast<float>(c) * (kTwoPi / static_cast<float>(std::max(1U, collider_count)));
        const Vec3 off{0.18f * std::cos(angle), -0.05f, 0.18f * std::sin(angle)};
        if (c % 2U == 0U) {
            REQUIRE(sim.add_sphere_collider(static_cast<int32_t>(AETHER_BONE_HIPS), off, 0.09f));
        } else {
            REQUIRE(sim.add_capsule_collider(static_cast<int32_t>(AETHER_BONE_SPINE), off,
                                             off + Vec3{0.0f, -0.14f, 0.0f}, 0.07f));
        }
    }

    for (uint32_t j = 0; j < joint_count; ++j) {
        SpringJointDesc d{};
        d.bone_index = static_cast<int32_t>(64U + j);
        const bool is_root_of_chain = (j % 4U) == 0U;
        d.parent_joint_index = is_root_of_chain ? -1 : static_cast<int32_t>(j - 1U);
        d.parent_bone_index =
            is_root_of_chain ? static_cast<int32_t>(AETHER_BONE_HEAD) : static_cast<int32_t>(64U + j - 1U);
        d.center_bone_index = static_cast<int32_t>(AETHER_BONE_HIPS);
        const float strand = static_cast<float>(j / 4U);
        d.local_bone_offset = {0.02f * std::cos(strand), -0.06f, 0.02f * std::sin(strand)};
        d.bone_length = 0.07f;
        d.hit_radius = 0.02f;
        d.stiffness = 0.45f;
        d.drag_force = 0.40f;
        d.gravity_power = 0.08f;
        REQUIRE(sim.add_joint(d));
    }
}

} // namespace

TEST_CASE("SpringBone AVX2 matches scalar reference within 1e-5m across 512 joints x 32 colliders",
          "[animation][spring][simd]") {
    auto sim_scalar = std::make_unique<SpringBoneSimulator>();
    auto sim_avx2 = std::make_unique<SpringBoneSimulator>();
    populate_stress_spring_rig(*sim_scalar, 512, 32);
    populate_stress_spring_rig(*sim_avx2, 512, 32);

    std::array<Vec3, kMaxBones> bones_s{};
    std::array<Quat, kMaxBones> rots_s{};
    rots_s.fill(Quat::identity());
    for (uint32_t b = 0; b < kMaxBones; ++b) {
        bones_s[b] = {0.0f, 1.45f - 0.01f * static_cast<float>(b % 16U), 0.0f};
    }
    auto bones_a = bones_s;
    auto rots_a = rots_s;

    sim_scalar->reset_state(bones_s, rots_s, kMaxBones);
    sim_avx2->reset_state(bones_a, rots_a, kMaxBones);

    for (int tick = 0; tick < 60; ++tick) {
        const float t = static_cast<float>(tick) * (1.0f / 60.0f);
        bones_s[AETHER_BONE_HIPS] = {0.08f * std::sin(t * 6.0f), 0.90f, 0.04f * std::cos(t * 4.0f)};
        bones_s[AETHER_BONE_HEAD] = bones_s[AETHER_BONE_HIPS] + Vec3{0.0f, 0.58f, 0.0f};
        bones_a[AETHER_BONE_HIPS] = bones_s[AETHER_BONE_HIPS];
        bones_a[AETHER_BONE_HEAD] = bones_s[AETHER_BONE_HEAD];

        sim_scalar->step(1.0f / 60.0f, {1.5f * std::sin(t), 0.0f, 0.0f}, 1.0f, bones_s, rots_s, kMaxBones,
                         SpringBoneSimulator::SimdPath::Scalar);
        sim_avx2->step(1.0f / 60.0f, {1.5f * std::sin(t), 0.0f, 0.0f}, 1.0f, bones_a, rots_a, kMaxBones,
                       SpringBoneSimulator::SimdPath::Avx2);
    }

    float max_diff = 0.0f;
    for (uint32_t j = 0; j < 512; ++j) {
        const float d = length(sim_scalar->joint_tail_pos(j) - sim_avx2->joint_tail_pos(j));
        max_diff = std::max(max_diff, d);
    }
    INFO("Max AVX2 vs Scalar tail position delta = " << max_diff << " m");
    CHECK(max_diff <= 1e-5f);
}

TEST_CASE("SpringBone teleport > 0.5m resets velocity cleanly without NaNs or explosion", "[animation][spring]") {
    auto sim = std::make_unique<SpringBoneSimulator>();
    populate_stress_spring_rig(*sim, 64, 8);
    std::array<Vec3, kMaxBones> bones{};
    std::array<Quat, kMaxBones> rots{};
    rots.fill(Quat::identity());
    sim->reset_state(bones, rots, kMaxBones);

    bones[AETHER_BONE_HIPS] = {2.0f, 0.90f, 0.0f}; // > 0.5 m jump!
    bones[AETHER_BONE_HEAD] = {2.0f, 1.48f, 0.0f};
    sim->step(1.0f / 60.0f, {}, 1.0f, bones, rots, kMaxBones);
    CHECK(sim->teleport_reset_count() >= 1);
    for (uint32_t j = 0; j < 64; ++j) {
        // `joint_tail_pos(j)` is in center-bone (`HIPS`) space per VRM 1.0 `§9.6`
        const Vec3 p_ws = sim->joint_tail_pos(j) + bones[AETHER_BONE_HIPS];
        CHECK(std::isfinite(p_ws.x));
        CHECK(std::isfinite(p_ws.y));
        CHECK(std::isfinite(p_ws.z));
        CHECK(length(p_ws - bones[AETHER_BONE_HEAD]) < 0.6f);
    }
}

TEST_CASE("SimEngine full T2 tick + 512-joint SpringBone are strictly zero-alloc and meet PERF-003",
          "[animation][perf][hot-path]") {
    auto engine = std::make_unique<SimEngine>();
    engine->reset();
    populate_stress_spring_rig(engine->spring_simulator(), 512, 32);

    auto frame = std::make_unique<SkinningFrame>();
    AetherTrackingFrame raw{};
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    const SimEngineConfig cfg{};

#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_thread_violations();
#endif

    // Warmup tick
    for (int i = 0; i < 8; ++i) {
        raw.frame_sequence = static_cast<uint32_t>(i + 1);
        raw.capture_timestamp_us = static_cast<uint64_t>(i + 1) * 16667ULL;
        engine->tick(raw, dsp, gout, raw.capture_timestamp_us + 2000ULL, -1.0f, cfg, *frame);
    }

    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kTicks = 240;
    {
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        AETHER_HOT_PATH_SCOPE;
#endif
        for (int i = 0; i < kTicks; ++i) {
            raw.frame_sequence = static_cast<uint32_t>(20 + i);
            raw.capture_timestamp_us = static_cast<uint64_t>(20 + i) * 4167ULL;
            engine->tick(raw, dsp, gout, raw.capture_timestamp_us + 2000ULL, -1.0f, cfg, *frame);
        }
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        CHECK(aether::HotPathGuard::thread_violations() == 0);
#endif
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double mean_tick_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kTicks;
    INFO("Full SimEngine::tick (512 spring joints x 32 colliders) mean = " << mean_tick_ms << " ms");
#if defined(NDEBUG)
    CHECK(mean_tick_ms <= 0.80);
#else
    CHECK(mean_tick_ms <= 6.00);
#endif
}
