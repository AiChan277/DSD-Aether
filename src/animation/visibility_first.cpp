#include "animation/visibility_first.hpp"
#include "animation/motion_filters.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

[[nodiscard]] Vec3 step_critical_spring(Vec3 x, Vec3& v, Vec3 target, float wn, float zeta, float dt_s) noexcept {
    // Semi-implicit Euler critically damped second-order spring (zeta = 1.0)
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
    spine_rot_spring_ = {};
    chest_rot_spring_ = {};
    neck_rot_spring_ = {};
    head_rot_spring_ = {};
    hips_lean_spring_ = {};
    chest_lean_spring_ = {};
    prev_target_head_rotvec_ = {};
    head_vel_ema_rad_s_ = {};
    has_prev_target_head_ = false;
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
    // Graph IK overrides are per-tick: never let a weight latched by an earlier graph evaluation keep the
    // FABRIK head chain (or wrist orientation) pinned after the graph stops driving it.
    out.head_target_weight = 0.0f;
    out.left_wrist_rot_ws = Quat::identity();
    out.right_wrist_rot_ws = Quat::identity();

    // 1. Evaluate optional ultra-smooth 0.22 Hz chest breathing oscillator (`enable_idle_breathing`)
    const float phase = kTwoPi * cfg.breathing_hz * phase_time_s_;
    const float breath_s = std::sin(phase);
    const float breath_01 = cfg.enable_idle_breathing ? (0.5f * (breath_s + 1.0f)) : 0.0f;
    const float breath_chest_rad = cfg.enable_idle_breathing ? (cfg.chest_pitch_rad * breath_01) : 0.0f;
    const float breath_shoulder_rad =
        cfg.enable_idle_breathing ? (cfg.shoulder_breath_rad * 0.5f * (breath_s + 1.0f)) : 0.0f;

    const float upper_conf =
        (tracked.valid_flags == 0U || (tracked.valid_flags & 0x08U) != 0U) ? tracked.upper_body_confidence : 0.0f;
    const float w_upper = step_visibility_weight(prev_weights_[AETHER_BONE_SPINE], upper_conf, safe_dt, cfg);
    prev_weights_[AETHER_BONE_SPINE] = w_upper;

    // Head tracking visibility weight & 60 Hz analytical critically-damped chain springs
    const float face_conf = (tracked.valid_flags & 0x01U) ? 1.0f : 0.0f;
    const float w_face = step_visibility_weight(prev_weights_[AETHER_BONE_HEAD], face_conf, safe_dt, cfg);
    prev_weights_[AETHER_BONE_HEAD] = w_face;

    const Quat tracked_head_q = normalize(tracked.joint_rotations[AETHER_BONE_HEAD]);
    const Vec3 raw_target_head_rv = quat_to_rotvec(tracked_head_q) * w_face;

    // Warudo per-channel SmoothDamp (exact analytic critically-damped, dt-invariant) with staggered smooth times so
    // the body follows the head organically: Spine 15% @ 185 ms, Chest/UpperChest 20% @ 145 ms, Neck 30% @ 105 ms,
    // Head 35% @ 75 ms (= 100% of the tracked head angle). Stillness is handled upstream by the Trickster deadzone.
    const SmoothTimes smooth{};
    const Vec3 spine_rv = smooth_damp(spine_rot_spring_.pos, raw_target_head_rv * 0.15f, spine_rot_spring_.vel,
                                      smooth.spine, safe_dt);
    spine_rot_spring_.pos = spine_rv;
    const Vec3 chest_rv = smooth_damp(chest_rot_spring_.pos, raw_target_head_rv * 0.20f, chest_rot_spring_.vel,
                                      smooth.chest, safe_dt);
    chest_rot_spring_.pos = chest_rv;
    const Vec3 neck_rv =
        smooth_damp(neck_rot_spring_.pos, raw_target_head_rv * 0.30f, neck_rot_spring_.vel, smooth.neck, safe_dt);
    neck_rot_spring_.pos = neck_rv;
    const Vec3 head_rv =
        smooth_damp(head_rot_spring_.pos, raw_target_head_rv * 0.35f, head_rot_spring_.vel, smooth.head, safe_dt);
    head_rot_spring_.pos = head_rv;

    // Head translation (camera space, metres relative to the calibrated neutral) -> pendulum lean of the torso.
    const Vec3 head_ws_sample = tracked.joint_positions_ws[AETHER_BONE_HEAD];
    const float head_dy = (head_ws_sample.y > 0.5f) ? (head_ws_sample.y - 1.50f) : head_ws_sample.y;
    const Vec3 target_lean =
        Vec3{std::clamp(head_ws_sample.x, -0.18f, 0.18f), std::clamp(head_dy, -0.08f, 0.08f),
             std::clamp(head_ws_sample.z, -0.14f, 0.14f)} *
        w_face;
    const Vec3 hips_lean = smooth_damp(hips_lean_spring_.pos, target_lean, hips_lean_spring_.vel, smooth.spine, safe_dt);
    hips_lean_spring_.pos = hips_lean;
    const Vec3 chest_lean =
        smooth_damp(chest_lean_spring_.pos, target_lean, chest_lean_spring_.vel, smooth.chest, safe_dt);
    chest_lean_spring_.pos = chest_lean;
    // Normalised lateral offset `dx` (1.0 ~= one face width = 15 cm) drives the Warudo roll-lean cascade.
    const float dx_spine = std::clamp(hips_lean.x / 0.15f, -1.5f, 1.5f);
    const float dx_chest = std::clamp(chest_lean.x / 0.15f, -1.5f, 1.5f);
    const float spine_roll_lean = -dx_spine * 12.0f * kDegToRad;
    const float chest_roll_lean = -dx_chest * 15.0f * kDegToRad;
    const float neck_roll_lean = -dx_chest * 8.0f * kDegToRad;

    const Vec3 tracked_hips = tracked.joint_positions_ws[AETHER_BONE_HIPS];
    Vec3 hips_pos = lerp(rest_hips_pos_, tracked_hips, w_upper);
    if (tracked.lower_body_confidence < 0.50f) {
        hips_pos.y = rest_hips_pos_.y;
    }
    hips_pos.x += hips_lean.x * 0.35f;
    hips_pos.z += hips_lean.z * 0.35f * 0.35f;
    out.hips_pos_ws = hips_pos;
    out.lower_body_track_weight = (tracked.lower_body_confidence >= 0.50f) ? tracked.lower_body_confidence : 0.0f;

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
    const bool l_body_target_ok = length_sq(tracked.joint_positions_ws[AETHER_BONE_LEFT_HAND]) > 1e-4f;
    const bool r_body_target_ok = length_sq(tracked.joint_positions_ws[AETHER_BONE_RIGHT_HAND]) > 1e-4f;
    const float l_arm_conf = (body_flag_ok && l_body_target_ok)
                                 ? std::min(tracked.upper_body_confidence, tracked.joint_confidence[AETHER_BONE_LEFT_HAND])
                                 : 0.0f;
    const float r_arm_conf = (body_flag_ok && r_body_target_ok)
                                 ? std::min(tracked.upper_body_confidence, tracked.joint_confidence[AETHER_BONE_RIGHT_HAND])
                                 : 0.0f;

    // Disable arm IK completely when hand_conf < 0.5 so arms stay in relaxed VTuber standing A-pose.
    const float l_hand_raw = std::max(l_arm_conf, tracked.left_hand_confidence);
    const float r_hand_raw = std::max(r_arm_conf, tracked.right_hand_confidence);
    const float l_hand_gated = (l_hand_raw >= 0.50f) ? l_hand_raw : 0.0f;
    const float r_hand_gated = (r_hand_raw >= 0.50f) ? r_hand_raw : 0.0f;

    const float w_l_arm = step_visibility_weight(prev_weights_[AETHER_BONE_LEFT_HAND], l_hand_gated, safe_dt, cfg);
    const float w_r_arm = step_visibility_weight(prev_weights_[AETHER_BONE_RIGHT_HAND], r_hand_gated, safe_dt, cfg);
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
    out.left_wrist_track_weight = w_l_arm;
    out.right_wrist_track_weight = w_r_arm;

    // Clavicle secondary lag (`tau = 0.15 s`): only active when arms are lifted/tracked (`w_l_arm > 0`)
    const float l_arm_elev =
        w_l_arm * std::clamp((out.left_wrist_target_ws.y - (hips_pos.y + 0.35f)) * 0.6f, -0.15f, 0.35f);
    const float r_arm_elev =
        w_r_arm * std::clamp((out.right_wrist_target_ws.y - (hips_pos.y + 0.35f)) * 0.6f, -0.15f, 0.35f);
    const float lag_alpha = safe_dt / (cfg.clavicle_lag_s + safe_dt);
    l_clavicle_lag_ += (l_arm_elev - l_clavicle_lag_) * lag_alpha;
    r_clavicle_lag_ += (r_arm_elev - r_clavicle_lag_) * lag_alpha;

    // 4. Populate local bone rotations strictly as rest-relative deltas (`world_delta_to_local_pose`)
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        out.local_rotations[b] = rest_metrics_.world_delta_to_local_pose(static_cast<uint32_t>(b), Quat::identity());
        out.joint_blend_weights[b] = prev_weights_[b];
    }

    // Layered kinematic chain deltas in character world frame (rest-relative via world_delta_to_local_pose):
    // rotation share + Warudo roll lean (-dx * 12/15/8 deg) + forward lean from head depth + additive breathing.
    // Living idle: a very slow, sub-degree torso sway (two incommensurate frequencies so it never looks looped).
    const float sway_amp = cfg.enable_idle_breathing ? cfg.idle_sway_deg * kDegToRad : 0.0f;
    const Vec3 idle_sway{0.0f, sway_amp * std::sin(kTwoPi * 0.071f * phase_time_s_),
                         0.7f * sway_amp * std::sin(kTwoPi * 0.113f * phase_time_s_ + 1.3f)};
    const Quat spine_delta =
        rotvec_to_quat(spine_rv + idle_sway + Vec3{hips_lean.z * 0.06f, 0.0f, spine_roll_lean});
    out.local_rotations[AETHER_BONE_SPINE] = rest_metrics_.world_delta_to_local_pose(AETHER_BONE_SPINE, spine_delta);

    const Quat chest_delta =
        rotvec_to_quat(chest_rv * 0.50f + Vec3{chest_lean.z * 0.10f + breath_chest_rad * 0.60f, 0.0f, chest_roll_lean});
    const Quat upper_chest_delta = rotvec_to_quat(chest_rv * 0.50f + Vec3{breath_chest_rad * 0.40f, 0.0f, 0.0f});
    out.local_rotations[AETHER_BONE_CHEST] = rest_metrics_.world_delta_to_local_pose(AETHER_BONE_CHEST, chest_delta);
    out.local_rotations[AETHER_BONE_UPPER_CHEST] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_UPPER_CHEST, upper_chest_delta);

    // Shoulder subtle counter-roll (-12% of torso roll) + gentle breathing lift (0.8 deg at 0.22 Hz)
    const float shoulder_counter_roll =
        -0.12f * (spine_rv.z + chest_rv.z + spine_roll_lean + chest_roll_lean);
    const Quat l_shoulder_delta =
        from_euler_xyz(0.0f, 0.0f, l_clavicle_lag_ + breath_shoulder_rad + shoulder_counter_roll);
    const Quat r_shoulder_delta =
        from_euler_xyz(0.0f, 0.0f, -r_clavicle_lag_ - breath_shoulder_rad + shoulder_counter_roll);
    out.local_rotations[AETHER_BONE_LEFT_SHOULDER] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_LEFT_SHOULDER, l_shoulder_delta);
    out.local_rotations[AETHER_BONE_RIGHT_SHOULDER] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_RIGHT_SHOULDER, r_shoulder_delta);

    // Natural relaxed standing pose when hands are untracked (not a stiff winged A-pose): upper arms hang
    // `idle_arm_drop_deg` below horizontal and swing slightly forward, elbows keep a soft bend, wrists relax.
    // The delta is measured from the avatar's actual rest upper-arm vector so T-pose and A-pose binds both land here.
    const Vec3 l_rest_u_dir =
        normalize_safe(rest_metrics_.rest_local_positions[AETHER_BONE_LEFT_LOWER_ARM], {1.0f, 0.0f, 0.0f});
    const Vec3 r_rest_u_dir =
        normalize_safe(rest_metrics_.rest_local_positions[AETHER_BONE_RIGHT_LOWER_ARM], {-1.0f, 0.0f, 0.0f});
    const float drop = cfg.idle_arm_drop_deg * kDegToRad;
    const float fwd = cfg.idle_arm_forward_deg * kDegToRad;
    const Vec3 l_target_u_dir{std::cos(drop) * std::cos(fwd), -std::sin(drop), std::cos(drop) * std::sin(fwd)};
    const Vec3 r_target_u_dir{-std::cos(drop) * std::cos(fwd), -std::sin(drop), std::cos(drop) * std::sin(fwd)};
    const Quat l_a_delta = quat_from_to(l_rest_u_dir, l_target_u_dir);
    const Quat r_a_delta = quat_from_to(r_rest_u_dir, r_target_u_dir);
    // Elbow flexion swings the forearm forward (+Z) about the arm's rest-frame Y axis.
    const float elbow = cfg.idle_elbow_bend_deg * kDegToRad;
    const Quat l_upper_a = slerp(l_a_delta, Quat::identity(), w_l_arm);
    const Quat l_lower_a = slerp(from_euler_xyz(0.0f, -elbow, 0.0f), Quat::identity(), w_l_arm);
    const Quat r_upper_a = slerp(r_a_delta, Quat::identity(), w_r_arm);
    const Quat r_lower_a = slerp(from_euler_xyz(0.0f, elbow, 0.0f), Quat::identity(), w_r_arm);
    // Relaxed wrists: slight palm-side flexion (palm faces -Y in the rest frame).
    const float wrist = cfg.idle_wrist_flex_deg * kDegToRad;
    out.local_rotations[AETHER_BONE_LEFT_HAND] = rest_metrics_.world_delta_to_local_pose(
        AETHER_BONE_LEFT_HAND, slerp(from_euler_xyz(0.0f, 0.0f, -wrist), Quat::identity(), w_l_arm));
    out.local_rotations[AETHER_BONE_RIGHT_HAND] = rest_metrics_.world_delta_to_local_pose(
        AETHER_BONE_RIGHT_HAND, slerp(from_euler_xyz(0.0f, 0.0f, wrist), Quat::identity(), w_r_arm));
    out.local_rotations[AETHER_BONE_LEFT_UPPER_ARM] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_LEFT_UPPER_ARM, l_upper_a);
    out.local_rotations[AETHER_BONE_LEFT_LOWER_ARM] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_LEFT_LOWER_ARM, l_lower_a);
    out.local_rotations[AETHER_BONE_RIGHT_UPPER_ARM] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_RIGHT_UPPER_ARM, r_upper_a);
    out.local_rotations[AETHER_BONE_RIGHT_LOWER_ARM] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_RIGHT_LOWER_ARM, r_lower_a);

    // Neck & Head follow critically-damped face Procrustes pose cleanly when face valid
    out.joint_blend_weights[AETHER_BONE_HEAD] = w_face;
    out.local_rotations[AETHER_BONE_NECK] = rest_metrics_.world_delta_to_local_pose(
        AETHER_BONE_NECK, rotvec_to_quat(neck_rv + Vec3{0.0f, 0.0f, neck_roll_lean}));
    // Head counter-stabilisation: cancel the torso/neck lean roll so the head keeps exactly the tracked orientation.
    const float lean_roll_sum = spine_roll_lean + chest_roll_lean + neck_roll_lean;
    out.local_rotations[AETHER_BONE_HEAD] = rest_metrics_.world_delta_to_local_pose(
        AETHER_BONE_HEAD, rotvec_to_quat(head_rv + Vec3{0.0f, 0.0f, -lean_roll_sum}));

    // Eye saccade rotations relative to eye rest orientation
    const Quat l_eye_delta = slerp(Quat::identity(), normalize(tracked.joint_rotations[AETHER_BONE_LEFT_EYE]), w_face);
    const Quat r_eye_delta = slerp(Quat::identity(), normalize(tracked.joint_rotations[AETHER_BONE_RIGHT_EYE]), w_face);
    out.local_rotations[AETHER_BONE_LEFT_EYE] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_LEFT_EYE, l_eye_delta);
    out.local_rotations[AETHER_BONE_RIGHT_EYE] =
        rest_metrics_.world_delta_to_local_pose(AETHER_BONE_RIGHT_EYE, r_eye_delta);

    // Hand finger curl/spread (`bones 25..54`)
    const float w_lhand = step_visibility_weight(prev_weights_[AETHER_BONE_LEFT_INDEX_PROXIMAL],
                                                 (tracked.left_hand_confidence >= 0.50f) ? tracked.left_hand_confidence : 0.0f,
                                                 safe_dt, cfg);
    const float w_rhand = step_visibility_weight(prev_weights_[AETHER_BONE_RIGHT_INDEX_PROXIMAL],
                                                 (tracked.right_hand_confidence >= 0.50f) ? tracked.right_hand_confidence : 0.0f,
                                                 safe_dt, cfg);
    prev_weights_[AETHER_BONE_LEFT_INDEX_PROXIMAL] = w_lhand;
    prev_weights_[AETHER_BONE_RIGHT_INDEX_PROXIMAL] = w_rhand;

    // Relaxed natural finger curl when untracked, graded per joint (proximal < intermediate > distal, thumb lighter)
    // so the hand reads as soft and alive instead of a flat paddle.
    const auto idle_curl_rad = [&](size_t joint_in_hand) noexcept {
        const size_t finger = joint_in_hand / 3U; // 0 = thumb
        const size_t seg = joint_in_hand % 3U;    // 0 proximal, 1 intermediate, 2 distal
        static constexpr float kSeg[3] = {0.80f, 1.25f, 0.90f};
        const float finger_scale = (finger == 0U) ? 0.45f : (0.85f + 0.08f * static_cast<float>(finger));
        return cfg.idle_finger_curl_rad * kSeg[seg] * finger_scale;
    };
    for (size_t b = AETHER_BONE_LEFT_THUMB_PROXIMAL; b <= AETHER_BONE_LEFT_LITTLE_DISTAL; ++b) {
        const Quat idle = from_euler_xyz(0.0f, 0.0f, -idle_curl_rad(b - AETHER_BONE_LEFT_THUMB_PROXIMAL));
        const Quat finger_delta = slerp(idle, normalize(tracked.joint_rotations[b]), w_lhand);
        out.local_rotations[b] = rest_metrics_.world_delta_to_local_pose(static_cast<uint32_t>(b), finger_delta);
        out.joint_blend_weights[b] = w_lhand;
    }
    for (size_t b = AETHER_BONE_RIGHT_THUMB_PROXIMAL; b <= AETHER_BONE_RIGHT_LITTLE_DISTAL; ++b) {
        const Quat idle = from_euler_xyz(0.0f, 0.0f, idle_curl_rad(b - AETHER_BONE_RIGHT_THUMB_PROXIMAL));
        const Quat finger_delta = slerp(idle, normalize(tracked.joint_rotations[b]), w_rhand);
        out.local_rotations[b] = rest_metrics_.world_delta_to_local_pose(static_cast<uint32_t>(b), finger_delta);
        out.joint_blend_weights[b] = w_rhand;
    }

    // Apply Rust node graph per-joint rotation offsets (`graph_out.bone_rotation_offsets[55][4]`, `§5.2`).
    // Offsets are character-frame deltas (like tracking), so map them through the bone's rest frame instead of
    // right-multiplying in raw bone-local axes (which differ per VRM rig).
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const float gx = graph_out.bone_rotation_offsets[b][0];
        const float gy = graph_out.bone_rotation_offsets[b][1];
        const float gz = graph_out.bone_rotation_offsets[b][2];
        const float gw = graph_out.bone_rotation_offsets[b][3];
        const float g2 = gx * gx + gy * gy + gz * gz + gw * gw;
        if (!(g2 > 0.25f) || !std::isfinite(g2)) {
            continue;
        }
        const Quat gq = normalize(Quat{gx, gy, gz, gw});
        if (std::abs(gq.w) >= 0.9999995f) {
            continue; // identity offset (default pass-through graph)
        }
        const Quat rw = rest_metrics_.rest_world_rotations[b];
        const float rw2 = rw.w * rw.w + rw.x * rw.x + rw.y * rw.y + rw.z * rw.z;
        const Quat rest_world = (rw2 > 1e-6f) ? normalize(rw) : Quat::identity();
        const Quat delta_local = normalize(conjugate(rest_world) * gq * rest_world);
        out.local_rotations[b] = normalize(out.local_rotations[b] * delta_local);
    }

    // Check head IK target override (`AETHER_IK_HEAD = 0`, `AETHER_IK_LEFT_HAND = 1`, `AETHER_IK_RIGHT_HAND = 2`)
    const auto apply_ik_override = [&](uint32_t slot, Vec3& io_pos, Quat& io_rot, float* opt_weight) {
        const float iw = std::clamp(graph_out.ik_targets[slot][7], 0.0f, 1.0f);
        const Vec3 gp{
            graph_out.ik_targets[slot][0],
            graph_out.ik_targets[slot][1],
            graph_out.ik_targets[slot][2],
        };
        // An unset slot (origin / non-finite) would drag the chain to the floor: ignore it.
        const bool target_ok = std::isfinite(gp.x) && std::isfinite(gp.y) && std::isfinite(gp.z) &&
                               length_sq(gp) > 1e-6f;
        if (iw > 0.0f && target_ok) {
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
    apply_ik_override(1U, out.left_wrist_target_ws, out.left_wrist_rot_ws, &out.left_wrist_track_weight);
    apply_ik_override(2U, out.right_wrist_target_ws, out.right_wrist_rot_ws, &out.right_wrist_track_weight);

    (void)&quat_from_two_vectors;
}

} // namespace aether::animation
