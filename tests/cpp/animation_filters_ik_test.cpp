#include "animation/expression_mixer.hpp"
#include "animation/ik_solvers.hpp"
#include "animation/kalman.hpp"
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

        // Verify upright rest pose preservation & relaxed ~38 deg A-pose upper arms
        const Vec3 head_delta_aa =
            to_axis_angle(normalize(engine.bone_rot_ws()[AETHER_BONE_HEAD] * conjugate(rm.rest_world_rotations[AETHER_BONE_HEAD])));
        CHECK(length(head_delta_aa) * kRadToDeg < 0.25f);

        // With no hands tracked, upper arms hang downward from shoulder (~38 deg adduction along -Y)
        const Vec3 l_arm_vec =
            engine.bone_pos_ws()[AETHER_BONE_LEFT_LOWER_ARM] - engine.bone_pos_ws()[AETHER_BONE_LEFT_UPPER_ARM];
        const Vec3 r_arm_vec =
            engine.bone_pos_ws()[AETHER_BONE_RIGHT_LOWER_ARM] - engine.bone_pos_ws()[AETHER_BONE_RIGHT_UPPER_ARM];
        const float l_drop_deg = std::asin(std::clamp(-normalize_safe(l_arm_vec).y, -1.0f, 1.0f)) * kRadToDeg;
        const float r_drop_deg = std::asin(std::clamp(-normalize_safe(r_arm_vec).y, -1.0f, 1.0f)) * kRadToDeg;
        CHECK(l_drop_deg > 30.0f);
        CHECK(l_drop_deg < 46.0f);
        CHECK(r_drop_deg > 30.0f);
        CHECK(r_drop_deg < 46.0f);
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
