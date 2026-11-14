#include "animation/visibility_first.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

[[nodiscard]] Vec3 step_critical_spring(Vec3 x, Vec3& v, Vec3 target, float wn, float zeta, float dt_s) noexcept {
    // Semi-implicit Euler critically damped second-order spring (zeta = 1.0, unconditional stability for dt <= 1/30)
    const Vec3 dx = x - target;
    const Vec3 a = (-2.0f * zeta * wn) * v - (wn * wn) * dx;
    v = v + a * dt_s;
    x = x + v * dt_s;
    return x;
}

[[nodiscard]] Quat quat_from_two_vectors(Vec3 from, Vec3 to) noexcept {
    const Vec3 f = normalize_safe(from);
    const Vec3 t = normalize_safe(to);
    const float c = std::clamp(dot(f, t), -1.0f, 1.0f);
    if (c < -0.9995f) {
        Vec3 ortho = cross({1.0f, 0.0f, 0.0f}, f);
        if (length_sq(ortho) < 1e-4f) {
            ortho = cross({0.0f, 0.0f, 1.0f}, f);
        }
        return from_axis_angle(normalize_safe(ortho), kPi);
    }
    const Vec3 v = cross(f, t);
    return normalize(Quat{v.x, v.y, v.z, 1.0f + c});
}

} // namespace

float eval_visibility_hermite(float confidence, float c_low, float c_high) noexcept {
    const float span = std::max(c_high - c_low, 1e-4f);
    const float t = std::clamp((confidence - c_low) / span, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float step_visibility_weight(float prev_w, float confidence, float dt_s, const VisibilityFirstConfig& cfg) noexcept {
    const float target = eval_visibility_hermite(confidence, cfg.gate_low, cfg.gate_high);
    const float max_step = std::max(cfg.slew_per_s, 0.01f) * std::clamp(dt_s, 0.0f, 0.1f);
    return std::clamp(target, prev_w - max_step, prev_w + max_step);
}

void VisibilityFirstSolver::reset(Vec3 root_hips_pos, Vec3 l_foot_rest, Vec3 r_foot_rest) noexcept {
    phase_time_s_ = 0.0f;
    rest_hips_pos_ = root_hips_pos;
    prev_weights_.fill(0.0f);
    l_wrist_pos_ = root_hips_pos + Vec3{0.23f, 0.02f, 0.24f};
    l_wrist_vel_ = {};
    r_wrist_pos_ = root_hips_pos + Vec3{-0.23f, 0.02f, 0.24f};
    r_wrist_vel_ = {};
    l_clavicle_lag_ = 0.0f;
    r_clavicle_lag_ = 0.0f;
    proc_rot_smooth_.fill(Quat::identity());

    feet_[0] = FootPlantState{l_foot_rest, l_foot_rest, l_foot_rest, 0.0f, false};
    feet_[1] = FootPlantState{r_foot_rest, r_foot_rest, r_foot_rest, 0.0f, false};
    initialized_ = true;
}

void VisibilityFirstSolver::set_rest_anchors(Vec3 root_hips_pos, Vec3 l_foot_rest, Vec3 r_foot_rest) noexcept {
    if (!initialized_) {
        reset(root_hips_pos, l_foot_rest, r_foot_rest);
        return;
    }
    const Vec3 delta = root_hips_pos - rest_hips_pos_;
    rest_hips_pos_ = root_hips_pos;
    l_wrist_pos_ = l_wrist_pos_ + delta;
    r_wrist_pos_ = r_wrist_pos_ + delta;
    if (!feet_[0].stepping) {
        feet_[0] = FootPlantState{l_foot_rest, l_foot_rest, l_foot_rest, 0.0f, false};
    } else {
        feet_[0].step_target_pos = l_foot_rest;
    }
    if (!feet_[1].stepping) {
        feet_[1] = FootPlantState{r_foot_rest, r_foot_rest, r_foot_rest, 0.0f, false};
    } else {
        feet_[1].step_target_pos = r_foot_rest;
    }
}

void VisibilityFirstSolver::solve(const FilteredTrackingState& tracked, const AetherGraphOutputs& graph_out, float dt_s,
                                  const VisibilityFirstConfig& cfg, SolvedBodyPose& out) noexcept {
    const float safe_dt = std::clamp(dt_s, 1.0f / 480.0f, 0.05f);
    if (!initialized_) {
        reset();
    }
    phase_time_s_ += safe_dt;
    out.foot_step_triggered_this_tick = {false, false};

    // 1. Evaluate optional ultra-smooth 0.2 Hz chest breathing oscillator (`enable_idle_breathing`)
    const float phase = kTwoPi * cfg.breathing_hz * phase_time_s_;
    const float breath_s = std::sin(phase);
    const float breath_01 = cfg.enable_idle_breathing ? (0.5f * (breath_s + 1.0f)) : 0.0f;
    const float breath_chest_rad = cfg.enable_idle_breathing ? (cfg.chest_pitch_rad * breath_01) : 0.0f;

    const float upper_conf =
        (tracked.valid_flags == 0U || (tracked.valid_flags & 0x08U) != 0U) ? tracked.upper_body_confidence : 0.0f;
    const float w_upper = step_visibility_weight(prev_weights_[AETHER_BONE_SPINE], upper_conf, safe_dt, cfg);
    prev_weights_[AETHER_BONE_SPINE] = w_upper;

    const Vec3 tracked_hips = tracked.joint_positions_ws[AETHER_BONE_HIPS];
    const Vec3 hips_pos = lerp(rest_hips_pos_, tracked_hips, w_upper);
    out.hips_pos_ws = hips_pos;

    // Head tracking visibility weight & calm torso sway (zero sway when face untracked)
    const float face_conf = (tracked.valid_flags & 0x01U) ? 1.0f : 0.0f;
    const float w_face = step_visibility_weight(prev_weights_[AETHER_BONE_HEAD], face_conf, safe_dt, cfg);
    prev_weights_[AETHER_BONE_HEAD] = w_face;

    // Head target: head tracking orientation drives subtle torso/hips sway (`k_sway = 0.35`, `§9.4`)
    const Quat tracked_head_q = normalize(tracked.joint_rotations[AETHER_BONE_HEAD]);
    const Vec3 head_aa = to_axis_angle(tracked_head_q) * w_face;
    const Vec3 proc_hips_aa{head_aa.x * 0.10f, head_aa.y * (cfg.k_sway * 0.45f), head_aa.z * (cfg.k_sway * 0.35f)};
    const Quat proc_hips_q = rotvec_to_quat(proc_hips_aa);
    const Quat tracked_hips_q = normalize(tracked.joint_rotations[AETHER_BONE_HIPS]);

    // 2. Auto Foot Planting (trigger step at lateral/planar displacement > `12 cm`, `0.35 s` smooth arc + `4 cm` apex)
    const std::array<Vec3, 2> ideal_foot_ws = {{
        {hips_pos.x + 0.095f, 0.05f, hips_pos.z},
        {hips_pos.x - 0.095f, 0.05f, hips_pos.z},
    }};
    for (size_t f = 0; f < 2U; ++f) {
        auto& st = feet_[f];
        if (!st.stepping) {
            const float dx = ideal_foot_ws[f].x - st.planted_pos.x;
            const float dz = ideal_foot_ws[f].z - st.planted_pos.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const bool other_stepping = feet_[1U - f].stepping;
            if (!other_stepping && dist > cfg.foot_step_threshold_m) {
                st.stepping = true;
                st.step_elapsed_s = 0.0f;
                st.step_start_pos = st.planted_pos;
                st.step_target_pos = ideal_foot_ws[f];
                out.foot_step_triggered_this_tick[f] = true;
            }
        }
        if (st.stepping) {
            st.step_elapsed_s += safe_dt;
            const float u = std::clamp(st.step_elapsed_s / std::max(cfg.foot_step_duration_s, 0.05f), 0.0f, 1.0f);
            const float s = u * u * (3.0f - 2.0f * u);
            Vec3 p = lerp(st.step_start_pos, st.step_target_pos, s);
            p.y += cfg.foot_step_apex_m * std::sin(kPi * u);
            st.planted_pos = p;
            if (u >= 1.0f) {
                st.stepping = false;
                st.planted_pos = st.step_target_pos;
            }
        }
    }
    out.left_foot_target_ws = feet_[0].planted_pos;
    out.right_foot_target_ws = feet_[1].planted_pos;

    // 3. Per-limb Confidence Gating + Critically Damped Desk / Thigh Attractor + Clavicle Secondary Lag
    const auto compute_attractor = [&](bool is_left, IdleAttractorMode mode) -> Vec3 {
        const float sx = is_left ? 1.0f : -1.0f;
        if (mode == IdleAttractorMode::Desk) {
            // Stationary physical desk anchor: only gentle forearm breathing lift (RMS <= 5 mm/s)
            return {
                rest_hips_pos_.x + sx * 0.23f,
                rest_hips_pos_.y + 0.02f + 0.5f * cfg.attractor_lift_m * breath_01,
                rest_hips_pos_.z + 0.24f,
            };
        }
        return {
            hips_pos.x + sx * 0.22f,
            hips_pos.y - 0.16f + cfg.attractor_lift_m * breath_01,
            hips_pos.z + 0.04f,
        };
    };

    const bool body_flag_ok = (tracked.valid_flags == 0U || (tracked.valid_flags & 0x08U) != 0U);
    const float l_arm_conf =
        body_flag_ok ? std::min(tracked.upper_body_confidence, tracked.joint_confidence[AETHER_BONE_LEFT_HAND]) : 0.0f;
    const float r_arm_conf =
        body_flag_ok ? std::min(tracked.upper_body_confidence, tracked.joint_confidence[AETHER_BONE_RIGHT_HAND]) : 0.0f;

    const float w_l_arm = step_visibility_weight(prev_weights_[AETHER_BONE_LEFT_HAND],
                                                 std::max(l_arm_conf, tracked.left_hand_confidence), safe_dt, cfg);
    const float w_r_arm = step_visibility_weight(prev_weights_[AETHER_BONE_RIGHT_HAND],
                                                 std::max(r_arm_conf, tracked.right_hand_confidence), safe_dt, cfg);
    prev_weights_[AETHER_BONE_LEFT_HAND] = w_l_arm;
    prev_weights_[AETHER_BONE_RIGHT_HAND] = w_r_arm;

    const Vec3 l_attr_target = compute_attractor(true, cfg.left_arm_idle);
    const Vec3 r_attr_target = compute_attractor(false, cfg.right_arm_idle);
    const Vec3 l_attr = step_critical_spring(l_wrist_pos_, l_wrist_vel_, l_attr_target, cfg.proc_spring_wn,
                                             cfg.proc_spring_zeta, safe_dt);
    const Vec3 r_attr = step_critical_spring(r_wrist_pos_, r_wrist_vel_, r_attr_target, cfg.proc_spring_wn,
                                             cfg.proc_spring_zeta, safe_dt);
    l_wrist_pos_ = l_attr;
    r_wrist_pos_ = r_attr;

    const Vec3 l_track_pos = tracked.joint_positions_ws[AETHER_BONE_LEFT_HAND];
    const Vec3 r_track_pos = tracked.joint_positions_ws[AETHER_BONE_RIGHT_HAND];
    out.left_wrist_target_ws = lerp(l_attr, l_track_pos, w_l_arm);
    out.right_wrist_target_ws = lerp(r_attr, r_track_pos, w_r_arm);
    out.left_wrist_track_weight = 1.0f; // IK always aims wrist at blended procedural/tracked target smoothly
    out.right_wrist_track_weight = 1.0f;

    // Clavicle secondary lag (`tau = 0.15 s`): lags shoulder lift behind upper arm elevation
    const float l_arm_elev = std::clamp((out.left_wrist_target_ws.y - (hips_pos.y + 0.35f)) * 0.6f, -0.15f, 0.35f);
    const float r_arm_elev = std::clamp((out.right_wrist_target_ws.y - (hips_pos.y + 0.35f)) * 0.6f, -0.15f, 0.35f);
    const float lag_alpha = safe_dt / (cfg.clavicle_lag_s + safe_dt);
    l_clavicle_lag_ += (l_arm_elev - l_clavicle_lag_) * lag_alpha;
    r_clavicle_lag_ += (r_arm_elev - r_clavicle_lag_) * lag_alpha;

    // 4. Populate local bone rotations + blend graph bone/IK overrides cleanly (`§9.4`, `§6.4`)
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        out.local_rotations[b] = Quat::identity();
        out.joint_blend_weights[b] = prev_weights_[b];
    }

    out.local_rotations[AETHER_BONE_HIPS] = slerp(proc_hips_q, tracked_hips_q, w_upper);
    // Optional chest breathing pitch + gentle counter-yaw (zero when untracked & breathing off)
    const Quat proc_chest = from_euler_xyz(breath_chest_rad, -head_aa.y * (cfg.k_sway * 0.15f), 0.0f);
    out.local_rotations[AETHER_BONE_CHEST] =
        slerp(proc_chest, normalize(tracked.joint_rotations[AETHER_BONE_CHEST]), w_upper);
    out.local_rotations[AETHER_BONE_UPPER_CHEST] =
        slerp(from_euler_xyz(breath_chest_rad * 0.5f, 0.0f, 0.0f),
              normalize(tracked.joint_rotations[AETHER_BONE_UPPER_CHEST]), w_upper);

    // Shoulder / clavicle follow + optional breathing rise
    const float shoulder_breath = cfg.enable_idle_breathing ? (0.015f * breath_01) : 0.0f;
    out.local_rotations[AETHER_BONE_LEFT_SHOULDER] = from_euler_xyz(0.0f, 0.0f, l_clavicle_lag_ + shoulder_breath);
    out.local_rotations[AETHER_BONE_RIGHT_SHOULDER] = from_euler_xyz(0.0f, 0.0f, -r_clavicle_lag_ - shoulder_breath);

    // Neck & Head follow face Procrustes pose cleanly when face valid
    out.joint_blend_weights[AETHER_BONE_HEAD] = w_face;

    out.local_rotations[AETHER_BONE_NECK] = slerp(Quat::identity(), rotvec_to_quat(head_aa * 0.35f), w_face);
    out.local_rotations[AETHER_BONE_HEAD] = slerp(Quat::identity(), rotvec_to_quat(head_aa * 0.65f), w_face);

    // Eye saccade rotations
    out.local_rotations[AETHER_BONE_LEFT_EYE] =
        slerp(Quat::identity(), normalize(tracked.joint_rotations[AETHER_BONE_LEFT_EYE]), w_face);
    out.local_rotations[AETHER_BONE_RIGHT_EYE] =
        slerp(Quat::identity(), normalize(tracked.joint_rotations[AETHER_BONE_RIGHT_EYE]), w_face);

    // Hand finger curl/spread (`bones 25..54`)
    const float w_lhand = step_visibility_weight(prev_weights_[AETHER_BONE_LEFT_INDEX_PROXIMAL],
                                                 tracked.left_hand_confidence, safe_dt, cfg);
    const float w_rhand = step_visibility_weight(prev_weights_[AETHER_BONE_RIGHT_INDEX_PROXIMAL],
                                                 tracked.right_hand_confidence, safe_dt, cfg);
    prev_weights_[AETHER_BONE_LEFT_INDEX_PROXIMAL] = w_lhand;
    prev_weights_[AETHER_BONE_RIGHT_INDEX_PROXIMAL] = w_rhand;

    // Gentle relaxed neutral finger curl when untracked
    const Quat idle_finger_curl = from_euler_xyz(0.28f, 0.0f, 0.0f);
    for (size_t b = AETHER_BONE_LEFT_THUMB_PROXIMAL; b <= AETHER_BONE_LEFT_LITTLE_DISTAL; ++b) {
        out.local_rotations[b] = slerp(idle_finger_curl, normalize(tracked.joint_rotations[b]), w_lhand);
        out.joint_blend_weights[b] = w_lhand;
    }
    for (size_t b = AETHER_BONE_RIGHT_THUMB_PROXIMAL; b <= AETHER_BONE_RIGHT_LITTLE_DISTAL; ++b) {
        out.local_rotations[b] = slerp(idle_finger_curl, normalize(tracked.joint_rotations[b]), w_rhand);
        out.joint_blend_weights[b] = w_rhand;
    }

    // Apply Rust node graph per-joint rotation offsets (`graph_out.bone_rotation_offsets[55][4]`, `§5.2`)
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const float gx = graph_out.bone_rotation_offsets[b][0];
        const float gy = graph_out.bone_rotation_offsets[b][1];
        const float gz = graph_out.bone_rotation_offsets[b][2];
        const float gw = graph_out.bone_rotation_offsets[b][3];
        if (gx * gx + gy * gy + gz * gz + gw * gw > 0.25f) {
            const Quat gq = normalize(Quat{gx, gy, gz, gw});
            out.local_rotations[b] = normalize(out.local_rotations[b] * gq);
        }
    }

    // Check head IK target override (`AETHER_IK_HEAD = 0`, `AETHER_IK_LEFT_HAND = 1`, `AETHER_IK_RIGHT_HAND = 2`)
    const auto apply_ik_override = [&](uint32_t slot, Vec3& io_pos, Quat& io_rot, float* opt_weight) {
        const float iw = std::clamp(graph_out.ik_targets[slot][7], 0.0f, 1.0f);
        if (iw > 0.0f) {
            const Vec3 gp{
                graph_out.ik_targets[slot][0],
                graph_out.ik_targets[slot][1],
                graph_out.ik_targets[slot][2],
            };
            const Quat gq = normalize(Quat{
                graph_out.ik_targets[slot][3],
                graph_out.ik_targets[slot][4],
                graph_out.ik_targets[slot][5],
                graph_out.ik_targets[slot][6],
            });
            io_pos = lerp(io_pos, gp, iw);
            io_rot = slerp(io_rot, gq, iw);
            if (opt_weight != nullptr) {
                *opt_weight = std::max(*opt_weight, iw);
            }
        }
    };
    out.head_target_pos_ws = hips_pos + Vec3{0.0f, 0.56f, 0.0f};
    Quat unused_head_q = Quat::identity();
    apply_ik_override(0U, out.head_target_pos_ws, unused_head_q, &out.head_target_weight);
    apply_ik_override(1U, out.left_wrist_target_ws, out.left_wrist_rot_ws, nullptr);
    apply_ik_override(2U, out.right_wrist_target_ws, out.right_wrist_rot_ws, nullptr);

    (void)&quat_from_two_vectors;
}

} // namespace aether::animation
