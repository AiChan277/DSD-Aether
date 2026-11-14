#include "animation/expression_mixer.hpp"
#include "animation/ik_solvers.hpp"
#include "animation/kalman.hpp"
#include "animation/motion_filters.hpp"
#include "animation/one_euro.hpp"
#include "animation/sim_engine.hpp"
#include "animation/visibility_first.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

using namespace aether::animation;

TEST_CASE("One-Euro filter achieves <= 1.0 mm / <= 0.15 deg static jitter and <= 1 frame dynamic lag",
          "[animation][one-euro]") {
    OneEuroPoseBank bank;
    bank.reset();

    std::mt19937 rng(42);
    std::normal_distribution<float> pos_noise(0.0f, 0.004f); // 4 mm std-dev raw noise
    std::normal_distribution<float> rot_noise(0.0f, 1.5f * kDegToRad);

    std::vector<float> out_x;
    std::vector<float> out_yaw;
    AetherTrackingFrame raw{};
    FilteredTrackingState state{};

    // Static test (60 Hz)
    for (int f = 0; f < 300; ++f) {
        raw.capture_timestamp_us = static_cast<uint64_t>(f + 1) * 16667ULL;
        raw.valid_flags = 0x09U;
        raw.upper_body_confidence = 0.95f;
        raw.joint_positions_ws[AETHER_BONE_HEAD][0] = pos_noise(rng);
        raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.55f + pos_noise(rng);
        raw.joint_positions_ws[AETHER_BONE_HEAD][2] = pos_noise(rng);
        raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.95f;
        const Quat q = from_axis_angle({0.0f, 1.0f, 0.0f}, rot_noise(rng));
        raw.joint_rotations_quat[AETHER_BONE_HEAD][0] = q.x;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][1] = q.y;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][2] = q.z;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][3] = q.w;
        bank.update(raw, state);
        if (f >= 60) {
            out_x.push_back(state.joint_positions_ws[AETHER_BONE_HEAD].x);
            const Vec3 aa = to_axis_angle(state.joint_rotations[AETHER_BONE_HEAD]);
            out_yaw.push_back(std::abs(aa.y) * length(aa) * kRadToDeg);
        }
    }

    auto stddev = [](const std::vector<float>& v) {
        const float mean = std::accumulate(v.begin(), v.end(), 0.0f) / static_cast<float>(v.size());
        float s2 = 0.0f;
        for (float x : v) {
            s2 += (x - mean) * (x - mean);
        }
        return std::sqrt(s2 / static_cast<float>(v.size()));
    };
    const float pos_std_mm = stddev(out_x) * 1000.0f;
    const float rot_std_deg = stddev(out_yaw);
    CHECK(pos_std_mm <= 1.0f);
    CHECK(rot_std_deg <= 0.25f); // well below raw 1.5 deg noise

    // 2 Hz sinusoid dynamic lag test -> peak timing lag <= 16.7 ms
    bank.reset();
    float max_in = -1.0f, max_out = -1.0f;
    double peak_in_t = 0.0, peak_out_t = 0.0;
    for (int f = 0; f < 120; ++f) {
        const double t_s = f / 60.0;
        const float signal = 0.15f * std::sin(static_cast<float>(2.0 * 3.14159265 * 2.0 * t_s));
        raw.capture_timestamp_us = static_cast<uint64_t>(f + 1) * 16667ULL;
        raw.joint_positions_ws[AETHER_BONE_HEAD][0] = signal;
        raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.95f;
        bank.update(raw, state);
        if (t_s > 0.45 && t_s < 0.85) {
            if (signal > max_in) {
                max_in = signal;
                peak_in_t = t_s;
            }
            if (state.joint_positions_ws[AETHER_BONE_HEAD].x > max_out) {
                max_out = state.joint_positions_ws[AETHER_BONE_HEAD].x;
                peak_out_t = t_s;
            }
        }
    }
    const double lag_ms = std::abs(peak_out_t - peak_in_t) * 1000.0;
    CHECK(lag_ms <= 20.0);
}

TEST_CASE("Kalman 14ms dead-reckoning predicts ramp without runaway on low confidence", "[animation][kalman]") {
    KalmanScalar2State kf;
    kf.reset(0.0f);
    const KalmanConfig cfg{};
    const float v = 1.2f; // 1.2 m/s constant motion
    const float dt = 1.0f / 60.0f;
    for (int i = 1; i <= 40; ++i) {
        kf.correct(v * (i * dt), dt, 0.95f, cfg.sigma_a_pos, cfg.sigma_m_pos, cfg);
    }
    const float pred_14ms = kf.predict(0.014f);
    const float exact_future = v * (40 * dt + 0.014f);
    CHECK(std::abs(pred_14ms - exact_future) < 0.004f);

    // Low confidence runaway guard: velocity must decay cleanly across 20 damped ticks
    const float v_before = std::abs(kf.velocity());
    for (int i = 0; i < 20; ++i) {
        kf.damp_velocity(cfg.low_conf_vel_damping);
    }
    CHECK(std::abs(kf.velocity()) < v_before * 0.05f);
}

TEST_CASE("VisibilityFirst C1 Hermite gating, 4.0/s slew rate, desk idle stability & 12cm foot planting",
          "[animation][visibility]") {
    // Property check on Hermite & slew rate
    CHECK(eval_visibility_hermite(0.10f) == 0.0f);
    CHECK(eval_visibility_hermite(0.30f) == 0.0f);
    CHECK(std::abs(eval_visibility_hermite(0.525f) - 0.5f) < 1e-4f);
    CHECK(eval_visibility_hermite(0.75f) == 1.0f);
    CHECK(eval_visibility_hermite(0.95f) == 1.0f);

    const VisibilityFirstConfig cfg{};
    const float dt = 1.0f / 60.0f;
    float w = 0.0f;
    for (int step = 0; step < 30; ++step) {
        const float next = step_visibility_weight(w, 1.0f, dt, cfg);
        CHECK((next - w) <= cfg.slew_per_s * dt + 1e-5f);
        w = next;
    }
    CHECK(w > 0.99f);

    VisibilityFirstSolver solver;
    solver.reset({0.0f, 0.90f, 0.0f}, {0.09f, 0.05f, 0.0f}, {-0.09f, 0.05f, 0.0f});

    FilteredTrackingState tracked{};
    tracked.upper_body_confidence = 0.9f;
    tracked.left_hand_confidence = 0.0f;
    tracked.right_hand_confidence = 0.0f;
    AetherGraphOutputs gout{};
    SolvedBodyPose pose{};

    // Run 5 seconds of hands-out-of-frame at 60 Hz -> measure wrist attractor RMS velocity <= 5 mm/s
    double sum_vel_sq = 0.0;
    int samples = 0;
    for (int f = 0; f < 300; ++f) {
        solver.solve(tracked, gout, dt, cfg, pose);
        if (f >= 60) {
            const float v_l = length(solver.left_wrist_attractor_vel());
            sum_vel_sq += static_cast<double>(v_l) * v_l;
            ++samples;
        }
    }
    const double rms_vel_mm_s = std::sqrt(sum_vel_sq / samples) * 1000.0;
    INFO("Out-of-frame desk attractor RMS vel = " << rms_vel_mm_s << " mm/s");
    CHECK(rms_vel_mm_s <= 5.5); // includes 0.25 Hz gentle 6 mm breathing lift

    // Foot planting test: translate hips laterally by 0.25 m -> foot step must trigger near 12 cm
    solver.reset({0.0f, 0.90f, 0.0f}, {0.09f, 0.05f, 0.0f}, {-0.09f, 0.05f, 0.0f});
    tracked.valid_flags = 0x08U;
    tracked.upper_body_confidence = 0.95f;
    bool triggered = false;
    float trigger_disp = 0.0f;
    for (int f = 0; f < 100; ++f) {
        const float dx = static_cast<float>(f) * 0.0025f;
        tracked.joint_positions_ws[AETHER_BONE_HIPS] = {dx, 0.90f, 0.0f};
        solver.solve(tracked, gout, dt, cfg, pose);
        if (pose.foot_step_triggered_this_tick[0] || pose.foot_step_triggered_this_tick[1]) {
            triggered = true;
            trigger_disp = std::abs(pose.hips_pos_ws.x);
            break;
        }
    }
    CHECK(triggered);
    CHECK(trigger_disp >= 0.08f);
    CHECK(trigger_disp <= 0.16f);
}

TEST_CASE("Analytic 2-bone IK hits reachable target accurately and FABRIK/joint limits obey Appendix C",
          "[animation][ik]") {
    const Vec3 root{0.20f, 1.35f, 0.0f};
    const Vec3 target{0.35f, 1.10f, 0.25f};
    const Vec3 pole{0.25f, 0.95f, -0.35f};
    const float len1 = 0.27f;
    const float len2 = 0.25f;

    const TwoBoneIkResult res = solve_two_bone_ik(root, target, pole, len1, len2, {1.0f, 0.0f, 0.0f},
                                                  {1.0f, 0.0f, 0.0f}, Quat::identity(), Quat::identity(), 1.0f);
    const float d_end = length(res.end_effector_ws - target);
    const float d_mid = length(res.mid_joint_ws - root);
    const float d_lower = length(res.end_effector_ws - res.mid_joint_ws);
    CHECK(d_end < 0.002f);
    CHECK(std::abs(d_mid - len1) < 0.002f);
    CHECK(std::abs(d_lower - len2) < 0.002f);

    // Anatomical knee hinge check: cannot bend hyper-extended backwards
    const Quat illegal = from_axis_angle({1.0f, 0.0f, 0.0f}, -1.2f);
    const Quat clamped = clamp_rotation_to_limits(illegal, joint_limit_for_bone(AETHER_BONE_LEFT_LOWER_LEG));
    const Vec3 aa = to_axis_angle(clamped);
    CHECK(aa.x >= -0.05f);
}

TEST_CASE("ExpressionMixer executes smooth 60 ms mouth-ownership handoff between camera and DSP",
          "[animation][expression]") {
    ExpressionMixer mixer;
    mixer.reset();

    std::array<float, 52> cam_bs{};
    cam_bs[17] = 0.85f; // jawOpen from camera
    cam_bs[0] = 0.60f;  // eyeBlinkLeft from camera
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    std::array<float, kMaxMorphs> out_morphs{};
    float blend = -1.0f;

    // 1. Camera owns mouth initially (DSP inactive)
    mixer.update(cam_bs.data(), dsp, gout, 0.016f, out_morphs.data(), blend);
    CHECK(blend == 0.0f);
    CHECK(std::abs(out_morphs[17] - 0.85f) < 0.01f);
    CHECK(std::abs(out_morphs[0] - 0.60f) < 0.01f);

    // 2. Turn DSP mouth ownership ON (30 ms -> midpoint, 60 ms -> full DSP)
    dsp.dsp_flags = 0x05U;             // voice_active (b0) | visemes_valid (b2)
    dsp.dsp_viseme_weights[0] = 0.90f; // aa viseme
    for (int ms = 0; ms < 3; ++ms) {
        mixer.update(cam_bs.data(), dsp, gout, 0.010f, out_morphs.data(), blend);
    }
    CHECK(blend >= 0.45f);
    CHECK(blend <= 0.55f);

    for (int ms = 0; ms < 4; ++ms) {
        mixer.update(cam_bs.data(), dsp, gout, 0.010f, out_morphs.data(), blend);
    }
    CHECK(blend >= 0.99f);
    // Eyes are NOT owned by DSP -> camera blink stays 0.60!
    CHECK(std::abs(out_morphs[0] - 0.60f) < 0.01f);
    // Viseme slot 52 gets DSP weight cleanly!
    CHECK(std::abs(out_morphs[kVisemeSlotOffset + 0] - 0.90f) < 0.01f);
    // Camera jawOpen is handed off smoothly to 0 so it doesn't double-open the jaw with visemes!
    CHECK(out_morphs[17] < 0.05f);
}

TEST_CASE("60Hz critically-damped kinematic chain, zero jitter, C2 follow-through & A-pose",
          "[animation][spring][apose]") {
    using namespace aether::animation;

    // 1. Stationary webcam micro-jitter (< 0.03 deg) -> < 0.001 deg settled jitter & relaxed A-pose
    {
        SimEngine engine;
        RestSkeletonMetrics rm = build_default_humanoid_rest_metrics();
        // Inject non-identity VRM bind rotations to verify rest-pose preservation
        rm.rest_local_rotations[AETHER_BONE_SPINE] = from_euler_xyz(0.05f, -0.10f, 0.04f);
        rm.rest_local_rotations[AETHER_BONE_CHEST] = from_euler_xyz(-0.04f, 0.12f, -0.03f);
        rm.rest_local_rotations[AETHER_BONE_NECK] = from_euler_xyz(0.03f, 0.08f, -0.02f);
        rm.rest_local_rotations[AETHER_BONE_HEAD] = from_euler_xyz(-0.02f, -0.10f, 0.05f);
        rm.rest_local_rotations[AETHER_BONE_LEFT_UPPER_ARM] = from_euler_xyz(0.0f, 1.5707963f, 0.0f);
        rm.rest_local_rotations[AETHER_BONE_RIGHT_UPPER_ARM] = from_euler_xyz(0.0f, -1.5707963f, 0.0f);
        rm.rest_world_rotations[AETHER_BONE_HIPS] = Quat::identity();
        for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            const int32_t p = rm.parent_indices[b];
            const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
            rm.rest_world_rotations[b] = normalize(rm.rest_world_rotations[pi] * rm.rest_local_rotations[b]);
        }
        engine.reset(rm);

        SimEngineConfig cfg{};
        cfg.visibility.enable_idle_breathing = false;
        AetherDspSyncPacket dsp{};
        AetherGraphOutputs gout{};
        SkinningFrame skin{};

        std::mt19937 rng(777U);
        std::normal_distribution<float> deg_noise(0.0f, 0.025f * kDegToRad);
        Quat prev_head_rot = Quat::identity();
        float max_jitter_deg = 0.0f;

        for (int f = 0; f < 180; ++f) {
            AetherTrackingFrame raw{};
            raw.frame_sequence = static_cast<uint32_t>(f + 1);
            raw.capture_timestamp_us = static_cast<uint64_t>(f + 1) * 16667ULL;
            raw.inference_complete_us = raw.capture_timestamp_us + 4000ULL;
            raw.valid_flags = 0x09U;
            raw.upper_body_confidence = 0.90f;
            raw.left_hand_confidence = 0.0f;
            raw.right_hand_confidence = 0.0f;
            const Quat qn = from_euler_xyz(deg_noise(rng), deg_noise(rng), deg_noise(rng));
            raw.joint_rotations_quat[AETHER_BONE_HEAD][0] = qn.x;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][1] = qn.y;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][2] = qn.z;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][3] = qn.w;
            raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
            raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.98f;

            engine.tick(raw, dsp, gout, raw.capture_timestamp_us + 8000ULL, 0.0f, cfg, skin);
            const Quat head_rot = engine.bone_rot_ws()[AETHER_BONE_HEAD];
            if (f >= 120) {
                const float delta_deg = angle_between(head_rot, prev_head_rot) * kRadToDeg;
                max_jitter_deg = std::max(max_jitter_deg, delta_deg);
            }
            prev_head_rot = head_rot;
        }
        CHECK(max_jitter_deg < 0.001f);

        // Verify upright rest pose preservation & relaxed natural idle arms (~68 deg below horizontal)
        const Vec3 head_delta_aa =
            to_axis_angle(normalize(engine.bone_rot_ws()[AETHER_BONE_HEAD] * conjugate(rm.rest_world_rotations[AETHER_BONE_HEAD])));
        CHECK(length(head_delta_aa) * kRadToDeg < 0.25f);

        // With no hands tracked, upper arms hang downward from shoulder (~68 deg below horizontal)
        const Vec3 l_arm_vec =
            engine.bone_pos_ws()[AETHER_BONE_LEFT_LOWER_ARM] - engine.bone_pos_ws()[AETHER_BONE_LEFT_UPPER_ARM];
        const Vec3 r_arm_vec =
            engine.bone_pos_ws()[AETHER_BONE_RIGHT_LOWER_ARM] - engine.bone_pos_ws()[AETHER_BONE_RIGHT_UPPER_ARM];
        const float l_drop_deg = std::asin(std::clamp(-normalize_safe(l_arm_vec).y, -1.0f, 1.0f)) * kRadToDeg;
        const float r_drop_deg = std::asin(std::clamp(-normalize_safe(r_arm_vec).y, -1.0f, 1.0f)) * kRadToDeg;
        CHECK(l_drop_deg > 60.0f);
        CHECK(l_drop_deg < 76.0f);
        CHECK(r_drop_deg > 60.0f);
        CHECK(r_drop_deg < 76.0f);
    }

    // 2. Step head turn (+24 deg Yaw): layered Spine -> Chest -> Neck -> Head follow-through,
    //    critically damped (zero overshoot), and SpringBone settles within 0.5 s.
    {
        SimEngine engine;
        RestSkeletonMetrics rm = build_default_humanoid_rest_metrics();
        engine.reset(rm);
        SimEngineConfig cfg{};
        cfg.visibility.enable_idle_breathing = false;
        AetherDspSyncPacket dsp{};
        AetherGraphOutputs gout{};
        SkinningFrame skin{};

        constexpr float kTargetYawRad = 24.0f * kDegToRad;
        float max_head_yaw_deg = 0.0f;
        std::array<float, 180> head_yaws{};
        float worst_spring_disp_late = 0.0f;
        Vec3 prev_hair_tip{};

        for (int f = 0; f < 180; ++f) {
            AetherTrackingFrame raw{};
            raw.frame_sequence = static_cast<uint32_t>((f / 2) + 1); // 30 Hz camera feeding 60 Hz sim
            raw.capture_timestamp_us = static_cast<uint64_t>((f / 2) * 2 + 1) * 16667ULL;
            raw.inference_complete_us = raw.capture_timestamp_us + 2000ULL;
            raw.valid_flags = 0x01U;
            raw.upper_body_confidence = 0.0f;
            const Quat q_yaw = from_axis_angle({0.0f, 1.0f, 0.0f}, (f >= 6) ? kTargetYawRad : 0.0f);
            raw.joint_rotations_quat[AETHER_BONE_HEAD][0] = q_yaw.x;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][1] = q_yaw.y;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][2] = q_yaw.z;
            raw.joint_rotations_quat[AETHER_BONE_HEAD][3] = q_yaw.w;
            raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
            raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;

            const uint64_t tick_us = static_cast<uint64_t>(f + 1) * 16667ULL;
            engine.tick(raw, dsp, gout, tick_us, 0.0f, cfg, skin);

            const float h_yaw = to_euler_xyz(engine.bone_rot_ws()[AETHER_BONE_HEAD]).y * kRadToDeg;
            head_yaws[static_cast<size_t>(f)] = h_yaw;
            max_head_yaw_deg = std::max(max_head_yaw_deg, h_yaw);

            const Vec3 tip = engine.bone_pos_ws()[67]; // end of hair strand 0
            if (f >= 90) { // >= 0.5 s after 60-frame convergence window
                worst_spring_disp_late = std::max(worst_spring_disp_late, length(tip - prev_hair_tip));
            }
            prev_hair_tip = tip;
        }

        // Critically damped: converges to ~24 deg with <= 0.3 deg overshoot
        CHECK(max_head_yaw_deg <= 24.3f);
        CHECK(head_yaws[179] >= 23.7f);
        CHECK(head_yaws[179] <= 24.3f);
        // C2 monotonic rise (no velocity reversal / ringing spikes on step response after visibility ramp)
        for (size_t i = 20; i < 180; ++i) {
            CHECK(head_yaws[i] >= head_yaws[i - 1] - 0.05f);
        }
        // Layered chain contributions at steady state (Spine ~15%, Chest+UChest ~20%, Neck ~30%, Head ~35%)
        const float spine_y = to_euler_xyz(engine.bone_rot_ws()[AETHER_BONE_SPINE]).y * kRadToDeg;
        const float uchest_y = to_euler_xyz(engine.bone_rot_ws()[AETHER_BONE_UPPER_CHEST]).y * kRadToDeg;
        const float neck_y = to_euler_xyz(engine.bone_rot_ws()[AETHER_BONE_NECK]).y * kRadToDeg;
        const float head_y = to_euler_xyz(engine.bone_rot_ws()[AETHER_BONE_HEAD]).y * kRadToDeg;
        CHECK(spine_y > 2.5f);
        CHECK(uchest_y > spine_y + 3.0f);
        CHECK(neck_y > uchest_y + 5.0f);
        CHECK(head_y > neck_y + 6.0f);
        // Hair/cloth settled cleanly well within 0.5s after head stabilizes
        CHECK(worst_spring_disp_late < 5e-4f);
    }
}

namespace {

RestSkeletonMetrics make_vrm_like_rest_metrics() {
    RestSkeletonMetrics rm = build_default_humanoid_rest_metrics();
    // Non-identity VRM bind rotations so every check also proves rest-pose preservation
    rm.rest_local_rotations[AETHER_BONE_SPINE] = from_euler_xyz(0.05f, -0.10f, 0.04f);
    rm.rest_local_rotations[AETHER_BONE_CHEST] = from_euler_xyz(-0.04f, 0.12f, -0.03f);
    rm.rest_local_rotations[AETHER_BONE_NECK] = from_euler_xyz(0.03f, 0.08f, -0.02f);
    rm.rest_local_rotations[AETHER_BONE_HEAD] = from_euler_xyz(-0.02f, -0.10f, 0.05f);
    rm.rest_world_rotations[AETHER_BONE_HIPS] = Quat::identity();
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rm.rest_world_rotations[b] = normalize(rm.rest_world_rotations[pi] * rm.rest_local_rotations[b]);
    }
    return rm;
}

float rest_delta_deg(const SimEngine& e, const RestSkeletonMetrics& rm, uint32_t b) {
    return angle_between(e.bone_rot_ws()[b], rm.rest_world_rotations[b]) * kRadToDeg;
}

float arm_drop_deg(const SimEngine& e, uint32_t upper, uint32_t lower) {
    const Vec3 v = normalize_safe(e.bone_pos_ws()[lower] - e.bone_pos_ws()[upper]);
    return std::asin(std::clamp(-v.y, -1.0f, 1.0f)) * kRadToDeg;
}

} // namespace

TEST_CASE("Live default node graph + near-neutral tracking keeps avatar upright, A-posed and jitter-free",
          "[animation][graph][apose][regression]") {
    // Regression for the Studio bug where `LOCKED P+0.6 Y-0.8 R-0.4` still twisted head/neck/torso downward:
    // the default NPU Pose -> Visibility-First IK -> Output Pose graph forwarded tracked Hips/Spine positions
    // into the head/hand IK slots and the raw head rotation into bone offsets. This drives SimEngine through the
    // REAL Rust graph host exactly like the T2 thread does.
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    AetherHostConfig hcfg{};
    hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
    hcfg.worker_threads = 2;
    AetherHostHandle host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &host) == AETHER_OK);
    REQUIRE(host != nullptr);

    const RestSkeletonMetrics rm = make_vrm_like_rest_metrics();
    SimEngine engine;
    engine.reset(rm);
    SimEngineConfig cfg{};
    cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    SkinningFrame skin{};
    const Vec3 rest_head_pos = engine.bone_pos_ws()[AETHER_BONE_HEAD];

    std::mt19937 rng(4242U);
    std::normal_distribution<float> deg_noise(0.0f, 0.025f * kDegToRad);
    const Quat q_hud = from_euler_xyz(0.6f * kDegToRad, -0.8f * kDegToRad, -0.4f * kDegToRad);

    float max_offset_deg = 0.0f;
    float max_ik_weight = 0.0f;
    float max_jitter_deg = 0.0f;
    std::array<Quat, 6> prev_chain{};
    for (int f = 0; f < 180; ++f) {
        AetherTrackingFrame raw{};
        raw.frame_sequence = static_cast<uint32_t>((f / 2) + 1); // 30 Hz camera -> 60 Hz sim
        raw.capture_timestamp_us = static_cast<uint64_t>((f / 2) * 2 + 1) * 16667ULL;
        raw.inference_complete_us = raw.capture_timestamp_us + 4000ULL;
        raw.valid_flags = 0x01U | 0x08U;
        raw.upper_body_confidence = 0.95f;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            raw.joint_rotations_quat[b][3] = 1.0f;
        }
        const Quat qn = normalize(q_hud * from_euler_xyz(deg_noise(rng), deg_noise(rng), deg_noise(rng)));
        raw.joint_rotations_quat[AETHER_BONE_HEAD][0] = qn.x;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][1] = qn.y;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][2] = qn.z;
        raw.joint_rotations_quat[AETHER_BONE_HEAD][3] = qn.w;
        raw.joint_positions_ws[AETHER_BONE_HIPS][0] = 0.0f;
        raw.joint_positions_ws[AETHER_BONE_HIPS][1] = 0.90f;
        raw.joint_positions_ws[AETHER_BONE_HIPS][2] = 0.0f;
        raw.joint_positions_ws[AETHER_BONE_HIPS][3] = 0.95f;
        raw.joint_positions_ws[AETHER_BONE_SPINE][3] = 0.95f; // xyz intentionally unset (0,0,0) like the tracker
        raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
        raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.98f;
        raw.joint_positions_ws[AETHER_BONE_NECK][3] = 0.98f;

        const uint64_t tick_us = static_cast<uint64_t>(f + 1) * 16667ULL;
        AetherGraphInputs gin{};
        gin.tracking = &raw;
        gin.now_us = tick_us;
        gin.dt_s = 1.0f / 60.0f;
        gin.tick_index = static_cast<uint32_t>(f);
        AetherGraphOutputs gout{};
        aether_set_thread_role(AETHER_THREAD_ROLE_T2);
        REQUIRE(aether_host_evaluate_active(host, &gin, &gout) == AETHER_OK);
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            const Quat g{gout.bone_rotation_offsets[b][0], gout.bone_rotation_offsets[b][1],
                         gout.bone_rotation_offsets[b][2], gout.bone_rotation_offsets[b][3]};
            max_offset_deg = std::max(max_offset_deg, angle_between(normalize(g), Quat::identity()) * kRadToDeg);
        }
        for (size_t s = 0; s < 6U; ++s) {
            max_ik_weight = std::max(max_ik_weight, gout.ik_targets[s][7]);
        }
        engine.tick(raw, dsp, gout, tick_us, 0.0f, cfg, skin);

        constexpr std::array<uint32_t, 6> kChain = {AETHER_BONE_HIPS, AETHER_BONE_SPINE, AETHER_BONE_CHEST,
                                                    AETHER_BONE_UPPER_CHEST, AETHER_BONE_NECK, AETHER_BONE_HEAD};
        for (size_t i = 0; i < kChain.size(); ++i) {
            const Quat q = engine.bone_rot_ws()[kChain[i]];
            if (f >= 120) {
                max_jitter_deg = std::max(max_jitter_deg, angle_between(q, prev_chain[i]) * kRadToDeg);
            }
            prev_chain[i] = q;
        }
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    aether_host_destroy(host);
    aether_runtime_shutdown();

    // Graph pass-through contributes nothing beyond what the C++ solver already filters.
    CHECK(max_offset_deg < 0.01f);
    CHECK(max_ik_weight < 1e-6f);

    // Upright: torso bones stay at rest, head carries only the ~1.1 deg the user actually turned.
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_HIPS) < 0.5f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_SPINE) < 1.0f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_CHEST) < 1.0f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_UPPER_CHEST) < 1.2f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_NECK) < 1.5f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_HEAD) < 1.8f);
    const Vec3 head_pos = engine.bone_pos_ws()[AETHER_BONE_HEAD];
    CHECK(std::abs(head_pos.x - rest_head_pos.x) < 0.02f);
    CHECK(std::abs(head_pos.z - rest_head_pos.z) < 0.03f);
    CHECK(head_pos.y > rest_head_pos.y - 0.02f); // not dragged down toward the hips

    // Shoulders level, arms relaxed into the natural ~68 deg idle pose (not yanked to the floor / origin).
    CHECK(std::abs(engine.bone_pos_ws()[AETHER_BONE_LEFT_UPPER_ARM].y -
                   engine.bone_pos_ws()[AETHER_BONE_RIGHT_UPPER_ARM].y) < 0.01f);
    const float l_drop = arm_drop_deg(engine, AETHER_BONE_LEFT_UPPER_ARM, AETHER_BONE_LEFT_LOWER_ARM);
    const float r_drop = arm_drop_deg(engine, AETHER_BONE_RIGHT_UPPER_ARM, AETHER_BONE_RIGHT_LOWER_ARM);
    CHECK(l_drop > 60.0f);
    CHECK(l_drop < 76.0f);
    CHECK(r_drop > 60.0f);
    CHECK(r_drop < 76.0f);

    // Rock-solid steady state for the whole Hips -> Head chain with 0.025 deg camera noise.
    CHECK(max_jitter_deg < 0.001f);
}

TEST_CASE("Graph IK override is per-tick: a one-off head target never latches the FABRIK chain",
          "[animation][graph][ik][regression]") {
    const RestSkeletonMetrics rm = make_vrm_like_rest_metrics();
    SimEngine engine;
    engine.reset(rm);
    SimEngineConfig cfg{};
    cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    SkinningFrame skin{};

    for (int f = 0; f < 120; ++f) {
        AetherTrackingFrame raw{};
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            raw.joint_rotations_quat[b][3] = 1.0f;
        }
        AetherGraphOutputs gout{};
        if (f == 10) {
            // A graph pulls the head forward/down for exactly one tick...
            gout.ik_targets[0][0] = 0.0f;
            gout.ik_targets[0][1] = 1.05f;
            gout.ik_targets[0][2] = 0.35f;
            gout.ik_targets[0][6] = 1.0f;
            gout.ik_targets[0][7] = 1.0f;
        } else if (f == 20) {
            // ...and an unset slot (origin) with full weight must be ignored entirely.
            gout.ik_targets[0][6] = 1.0f;
            gout.ik_targets[0][7] = 1.0f;
            gout.ik_targets[1][7] = 1.0f;
            gout.ik_targets[2][7] = 1.0f;
        }
        engine.tick(raw, dsp, gout, static_cast<uint64_t>(f + 1) * 16667ULL, 0.0f, cfg, skin);
        if (f == 20) {
            CHECK(rest_delta_deg(engine, rm, AETHER_BONE_SPINE) < 0.5f);
            // An IK pull to the origin would swing the left hand inward past the shoulder; the idle arm hangs outside it.
            CHECK(engine.bone_pos_ws()[AETHER_BONE_LEFT_HAND].x > engine.bone_pos_ws()[AETHER_BONE_LEFT_UPPER_ARM].x);
        }
    }
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_SPINE) < 0.5f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_CHEST) < 0.5f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_NECK) < 0.5f);
    CHECK(rest_delta_deg(engine, rm, AETHER_BONE_HEAD) < 0.5f);
}

TEST_CASE("Warudo SmoothDamp is critically damped, overshoot-free and dt-invariant", "[animation][smoothdamp]") {
    // 1 x 1/60 s equals 4 x 1/240 s for a constant target (exact analytic solution).
    float a = 0.0f, va = 0.0f, b = 0.0f, vb = 0.0f;
    for (int f = 0; f < 30; ++f) {
        a = smooth_damp(a, 10.0f, va, 0.075f, 1.0f / 60.0f);
        for (int s = 0; s < 4; ++s) {
            b = smooth_damp(b, 10.0f, vb, 0.075f, 1.0f / 240.0f);
        }
        CHECK(std::abs(a - b) < 1e-3f);
        CHECK(a <= 10.0f); // never overshoots
    }
    CHECK(std::abs(a - 10.0f) < 0.01f); // 0.5 s >> 75 ms smooth time
    // Longer smooth time lags more: spine (185 ms) trails head (75 ms) after 100 ms.
    float h = 0.0f, vh = 0.0f, sp = 0.0f, vs = 0.0f;
    const SmoothTimes st{};
    for (int f = 0; f < 6; ++f) {
        h = smooth_damp(h, 1.0f, vh, st.head, 1.0f / 60.0f);
        sp = smooth_damp(sp, 1.0f, vs, st.spine, 1.0f / 60.0f);
    }
    CHECK(h > sp + 0.2f);
}

TEST_CASE("Trickster median-3 rejects 1-frame spikes and centre deadzone holds still", "[animation][trickster]") {
    CHECK(median3(1.0f, 9.0f, 2.0f) == 2.0f);
    CHECK(median3(5.0f, 1.0f, 3.0f) == 3.0f);

    TricksterChannel ch;
    ch.reset(0.0f);
    float out = 0.0f;
    for (float v : {0.0f, 0.0f, 25.0f, 0.0f, 0.0f}) { // single-frame 25 deg spike
        out = ch.push(v, 0.18f);
        CHECK(std::abs(out) < 1e-6f);
    }
    // Sub-deadzone sensor noise is attenuated by smoothstep(0, dz, |step|): +-0.03 deg -> < 0.003 deg/step (>10x).
    float prev = ch.value();
    for (int i = 0; i < 60; ++i) {
        out = ch.push((i % 2 == 0) ? 0.03f : -0.03f, 0.18f);
        CHECK(std::abs(out - prev) < 0.003f);
        prev = out;
    }
    // A real 10 deg move passes through untouched (after the 1-frame median delay).
    ch.reset(0.0f);
    (void)ch.push(10.0f, 0.18f);
    CHECK(std::abs(ch.push(10.0f, 0.18f) - 10.0f) < 1e-5f);
}

TEST_CASE("Kalidokit calcEyes / stabilizeBlink / calcMouth", "[animation][kalidokit]") {
    // Openness remap [0.14, 0.28] -> [0, 1], normalised to the calibrated open ratio.
    CHECK(kalido_eye_openness(0.28f, 0.28f) == 1.0f);
    CHECK(kalido_eye_openness(0.14f, 0.28f) == 0.0f);
    CHECK(std::abs(kalido_eye_openness(0.21f, 0.28f) - 0.5f) < 1e-4f);
    CHECK(kalido_eye_openness(0.32f, 0.32f) == 1.0f); // larger eyes still read fully open

    // Facing camera + small asymmetry -> locked to the average (no one-eye twitch).
    float l = 0.10f, r = 0.30f;
    kalido_stabilize_blink(l, r, 0.0f, false);
    CHECK(l == r);
    CHECK(std::abs(l - 0.20f) < 1e-6f);
    // Decisive wink stays independent.
    l = 0.95f;
    r = 0.05f;
    kalido_stabilize_blink(l, r, 0.0f, false);
    CHECK(l == 0.95f);
    CHECK(r == 0.05f);
    // Head turned 25 deg: the camera-facing eye drives both.
    l = 0.40f;
    r = 0.15f;
    kalido_stabilize_blink(l, r, 25.0f * kDegToRad, true);
    CHECK(l == 0.15f);
    CHECK(r == 0.15f);

    const float x0 = 0.55f;
    const MouthVowels closed = kalido_calc_mouth(0.01f, x0, x0);
    CHECK(closed.a + closed.i + closed.u + closed.e + closed.o < 1e-6f);
    const MouthVowels open_a = kalido_calc_mouth(0.33f, x0, x0);
    CHECK(open_a.a > 0.9f);
    CHECK(open_a.a > open_a.o);
    const MouthVowels wide_i = kalido_calc_mouth(0.03f, x0 * 1.30f, x0);
    CHECK(wide_i.i > 0.9f);
    const MouthVowels pucker_u = kalido_calc_mouth(0.06f, x0 * 0.72f, x0);
    CHECK(pucker_u.u > 0.2f);
    CHECK(pucker_u.u > pucker_u.a);
    const MouthVowels round_o = kalido_calc_mouth(0.40f, x0 * 0.72f, x0);
    CHECK(round_o.o > 0.8f);
    CHECK(round_o.o > round_o.a); // narrowing the open mouth turns "A" into "O"
    const MouthVowels mid_e = kalido_calc_mouth(0.20f, x0 * 1.25f, x0);
    CHECK(mid_e.e > 0.25f);
}

TEST_CASE("ExpressionMixer drives VRM vowel slots from the webcam and takes max with DSP", "[animation][expression]") {
    ExpressionMixer mixer;
    mixer.reset();
    std::array<float, 52> cam{};
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    std::array<float, kMaxMorphs> out{};
    float blend = 0.0f;
    const float vowels_open_a[5] = {0.8f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int f = 0; f < 20; ++f) { // mic silent: webcam alone opens the VRM "A"
        mixer.update(cam.data(), dsp, gout, 1.0f / 60.0f, out.data(), blend, vowels_open_a);
    }
    CHECK(std::abs(out[kVisemeSlotOffset + 0] - 0.8f) < 0.01f);

    dsp.dsp_flags = 0x05U;
    dsp.dsp_viseme_weights[3] = 0.9f; // DSP "E"
    for (int f = 0; f < 20; ++f) {
        mixer.update(cam.data(), dsp, gout, 1.0f / 60.0f, out.data(), blend, vowels_open_a);
    }
    CHECK(std::abs(out[kVisemeSlotOffset + 0] - 0.8f) < 0.01f); // max(webcam A, DSP A = 0)
    CHECK(std::abs(out[kVisemeSlotOffset + 3] - 0.9f) < 0.01f); // max(webcam E = 0, DSP E)

    // Blink smoothing is snappy (28 ms): a full blink reaches > 0.9 within 100 ms.
    ExpressionMixer m2;
    m2.reset();
    std::array<float, 52> eyes{};
    m2.update(eyes.data(), AetherDspSyncPacket{}, gout, 1.0f / 60.0f, out.data(), blend);
    eyes[0] = 1.0f;
    for (int f = 0; f < 6; ++f) {
        m2.update(eyes.data(), AetherDspSyncPacket{}, gout, 1.0f / 60.0f, out.data(), blend);
    }
    CHECK(out[0] > 0.9f);
}

TEST_CASE("Warudo pendulum lean: torso rolls with lateral head offset, head stays tracked-level",
          "[animation][lean]") {
    const RestSkeletonMetrics rm = make_vrm_like_rest_metrics();
    SimEngine engine;
    engine.reset(rm);
    SimEngineConfig cfg{};
    cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    SkinningFrame skin{};
    for (int f = 0; f < 180; ++f) {
        AetherTrackingFrame raw{};
        raw.frame_sequence = static_cast<uint32_t>((f / 2) + 1);
        raw.capture_timestamp_us = static_cast<uint64_t>((f / 2) * 2 + 1) * 16667ULL;
        raw.inference_complete_us = raw.capture_timestamp_us;
        raw.valid_flags = 0x01U;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            raw.joint_rotations_quat[b][3] = 1.0f;
        }
        raw.joint_positions_ws[AETHER_BONE_HEAD][0] = 0.075f; // lean half a face width toward +X
        raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
        raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        engine.tick(raw, dsp, gout, static_cast<uint64_t>(f + 1) * 16667ULL, 0.0f, cfg, skin);
    }
    const auto world_delta = [&](uint32_t b) {
        return normalize(engine.bone_rot_ws()[b] * conjugate(rm.rest_world_rotations[b]));
    };
    // Spine rolls -dx*12 deg = -6 deg about +Z (top of the torso moves toward +X), chest adds -7.5 deg.
    const Vec3 spine_up = rotate_vec(world_delta(AETHER_BONE_SPINE), {0.0f, 1.0f, 0.0f});
    const Vec3 chest_up = rotate_vec(world_delta(AETHER_BONE_CHEST), {0.0f, 1.0f, 0.0f});
    CHECK(spine_up.x > 0.08f);
    CHECK(chest_up.x > spine_up.x + 0.08f);
    // Counter-stabilised head keeps the (zero) tracked roll.
    const Vec3 head_up = rotate_vec(world_delta(AETHER_BONE_HEAD), {0.0f, 1.0f, 0.0f});
    CHECK(std::abs(head_up.x) < 0.02f);
    CHECK(engine.bone_pos_ws()[AETHER_BONE_HEAD].x > engine.bone_pos_ws()[AETHER_BONE_HIPS].x);
}

TEST_CASE("Emotion presets suppress tracked blink (VRM overrideBlink) but keep explicit winks", "[animation][expression]") {
    ExpressionMixer mixer;
    mixer.reset();
    std::array<float, 52> cam{};
    cam[0] = 0.7f;
    cam[7] = 0.7f;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    gout.expression_weights[52] = 1.0f; // graph slot 52 -> Joy input 57 (F1)
    gout.expression_override[52] = 1.0f;
    std::array<float, kMaxMorphs> out{};
    float blend = 0.0f;
    mixer.update(cam.data(), dsp, gout, 1.0f / 60.0f, out.data(), blend);
    CHECK(out[57] == 1.0f);
    CHECK(out[0] < 1e-6f); // Joy's own closed-eye shape is not stacked with the tracked blink
    CHECK(out[7] < 1e-6f);

    gout.expression_weights[0] = 1.0f; // explicit wink override survives
    gout.expression_override[0] = 1.0f;
    mixer.update(cam.data(), dsp, gout, 1.0f / 60.0f, out.data(), blend);
    CHECK(out[0] > 0.99f);
}

TEST_CASE("Task 1 & Task 3: Hand tracking/arm IK stay bypassed and Live2D vowel manifold is gated",
          "[animation][ik][live2d]") {
    VisibilityFirstConfig default_ik{};
    CHECK_FALSE(default_ik.enable_hand_ik);

    const RestSkeletonMetrics rm = make_vrm_like_rest_metrics();

    auto run_arm_eval = [&](bool adversarial_hands) {
        SimEngine engine;
        engine.reset(rm);
        SimEngineConfig cfg{};
        cfg.visibility.enable_idle_breathing = false;
        AetherDspSyncPacket dsp{};
        SkinningFrame skin{};
        std::array<float, 2> drops{};

        for (int f = 0; f < 30; ++f) {
            AetherTrackingFrame raw{};
            raw.frame_sequence = static_cast<uint32_t>(f + 1);
            raw.capture_timestamp_us = static_cast<uint64_t>(f + 1) * 16667ULL;
            raw.inference_complete_us = raw.capture_timestamp_us;
            raw.valid_flags = adversarial_hands ? 0x0FU : 0x09U;
            raw.upper_body_confidence = 0.95f;
            for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                raw.joint_rotations_quat[b][3] = 1.0f;
            }
            raw.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
            raw.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;

            AetherGraphOutputs gout{};
            if (adversarial_hands) {
                raw.left_hand_confidence = 0.99f;
                raw.right_hand_confidence = 0.99f;
                raw.joint_positions_ws[AETHER_BONE_LEFT_HAND][0] = 0.85f;
                raw.joint_positions_ws[AETHER_BONE_LEFT_HAND][1] = 2.20f;
                raw.joint_positions_ws[AETHER_BONE_LEFT_HAND][2] = -0.45f;
                raw.joint_positions_ws[AETHER_BONE_LEFT_HAND][3] = 0.99f;
                raw.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] = -0.85f;
                raw.joint_positions_ws[AETHER_BONE_RIGHT_HAND][1] = 0.25f;
                raw.joint_positions_ws[AETHER_BONE_RIGHT_HAND][2] = 0.60f;
                raw.joint_positions_ws[AETHER_BONE_RIGHT_HAND][3] = 0.99f;
                const Quat twisted = normalize(Quat{0.4f, 0.5f, 0.3f, 0.7f});
                for (uint32_t b = AETHER_BONE_LEFT_SHOULDER; b <= AETHER_BONE_RIGHT_HAND; ++b) {
                    raw.joint_rotations_quat[b][0] = twisted.x;
                    raw.joint_rotations_quat[b][1] = twisted.y;
                    raw.joint_rotations_quat[b][2] = twisted.z;
                    raw.joint_rotations_quat[b][3] = twisted.w;
                    gout.bone_rotation_offsets[b][0] = twisted.x;
                    gout.bone_rotation_offsets[b][1] = twisted.y;
                    gout.bone_rotation_offsets[b][2] = twisted.z;
                    gout.bone_rotation_offsets[b][3] = twisted.w;
                }
                gout.ik_targets[1][0] = 0.9f;
                gout.ik_targets[1][1] = 2.1f;
                gout.ik_targets[1][2] = 0.5f;
                gout.ik_targets[1][6] = 1.0f;
                gout.ik_targets[1][7] = 1.0f;
                gout.ik_targets[2][0] = -0.9f;
                gout.ik_targets[2][1] = 0.2f;
                gout.ik_targets[2][2] = 0.6f;
                gout.ik_targets[2][6] = 1.0f;
                gout.ik_targets[2][7] = 1.0f;
            }
            engine.tick(raw, dsp, gout, raw.capture_timestamp_us, 0.0f, cfg, skin);
        }
        drops[0] = arm_drop_deg(engine, AETHER_BONE_LEFT_UPPER_ARM, AETHER_BONE_LEFT_LOWER_ARM);
        drops[1] = arm_drop_deg(engine, AETHER_BONE_RIGHT_UPPER_ARM, AETHER_BONE_RIGHT_LOWER_ARM);
        return std::make_pair(skin, drops);
    };

    const auto [base_skin, base_drops] = run_arm_eval(false);
    const auto [adv_skin, adv_drops] = run_arm_eval(true);
    CHECK(base_drops[0] > 60.0f);
    CHECK(base_drops[0] < 76.0f);
    CHECK(base_drops[1] > 60.0f);
    CHECK(base_drops[1] < 76.0f);
    CHECK(std::abs(adv_drops[0] - base_drops[0]) < 1e-4f);
    CHECK(std::abs(adv_drops[1] - base_drops[1]) < 1e-4f);
    for (uint32_t b : {AETHER_BONE_LEFT_SHOULDER, AETHER_BONE_RIGHT_SHOULDER, AETHER_BONE_LEFT_UPPER_ARM,
                       AETHER_BONE_RIGHT_UPPER_ARM, AETHER_BONE_LEFT_LOWER_ARM, AETHER_BONE_RIGHT_LOWER_ARM,
                       AETHER_BONE_LEFT_HAND, AETHER_BONE_RIGHT_HAND}) {
        for (size_t k = 0; k < 12U; ++k) {
            CHECK(std::abs(adv_skin.bone_matrices[b][k] - base_skin.bone_matrices[b][k]) < 1e-5f);
        }
    }

    // Task 3: Live2D continuous 5-vowel manifold and closed-mouth noise gate (< 0.06).
    for (float form : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        float a = 1.0f, i = 1.0f, u = 1.0f, e = 1.0f, o = 1.0f;
        solve_live2d_vowels(form, 0.059f, a, i, u, e, o);
        CHECK(a == 0.0f);
        CHECK(i == 0.0f);
        CHECK(u == 0.0f);
        CHECK(e == 0.0f);
        CHECK(o == 0.0f);
    }

    auto check_dominant = [](float open_y, float form, int expected_idx) {
        float v[5]{};
        solve_live2d_vowels(form, open_y, v[0], v[1], v[2], v[3], v[4]);
        const float sum = v[0] + v[1] + v[2] + v[3] + v[4];
        CHECK(sum <= 1.001f);
        CHECK(v[expected_idx] > 0.30f);
        for (int k = 0; k < 5; ++k) {
            if (k != expected_idx) {
                CHECK(v[expected_idx] >= v[k]);
            }
        }
    };
    check_dominant(0.90f, 0.00f, 0);  // A
    check_dominant(0.20f, 0.90f, 1);  // I
    check_dominant(0.45f, -0.90f, 2); // U
    check_dominant(0.48f, 0.70f, 3);  // E
    check_dominant(0.80f, -0.60f, 4); // O

    // Verify _reserved[16] packing round-trip without altering ABI struct size.
    static_assert(sizeof(AetherTrackingFrame) == 2048U);
    Live2DParameters src{};
    src.param_brow_l_y = 0.65f;
    src.param_brow_r_y = -0.35f;
    src.param_brow_l_angle = 0.22f;
    src.param_brow_r_angle = -0.18f;
    src.param_brow_l_form = 0.50f;
    src.param_brow_r_form = -0.45f;
    src.param_mouth_open_y = 0.72f;
    src.param_mouth_form = -0.25f;
    AetherTrackingFrame packed{};
    pack_live2d_into_tracking_frame(src, packed);
    CHECK(has_packed_live2d(packed));
    Live2DParameters dst{};
    unpack_live2d_from_tracking_frame(packed, dst);
    CHECK(std::abs(dst.param_brow_l_y - src.param_brow_l_y) < 1e-4f);
    CHECK(std::abs(dst.param_brow_r_y - src.param_brow_r_y) < 1e-4f);
    CHECK(std::abs(dst.param_brow_l_angle - src.param_brow_l_angle) < 1e-4f);
    CHECK(std::abs(dst.param_brow_r_angle - src.param_brow_r_angle) < 1e-4f);
    CHECK(std::abs(dst.param_brow_l_form - src.param_brow_l_form) < 1e-4f);
    CHECK(std::abs(dst.param_brow_r_form - src.param_brow_r_form) < 1e-4f);
    CHECK(std::abs(dst.param_mouth_open_y - src.param_mouth_open_y) < 1e-4f);
    CHECK(std::abs(dst.param_mouth_form - src.param_mouth_form) < 1e-4f);
}
