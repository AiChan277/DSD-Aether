#include "animation/sim_engine.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

void write_bone_skin_matrix(const Vec3& world_pos, const Quat& world_rot, const Vec3& rest_pos,
                            float out_mat[12]) noexcept {
    // 3x4 row-major skinning matrix M = T(world_pos) * R(world_rot) * T(-rest_pos)
    const float xx = world_rot.x * world_rot.x;
    const float yy = world_rot.y * world_rot.y;
    const float zz = world_rot.z * world_rot.z;
    const float xy = world_rot.x * world_rot.y;
    const float xz = world_rot.x * world_rot.z;
    const float yz = world_rot.y * world_rot.z;
    const float wx = world_rot.w * world_rot.x;
    const float wy = world_rot.w * world_rot.y;
    const float wz = world_rot.w * world_rot.z;

    const float r00 = 1.0f - 2.0f * (yy + zz);
    const float r01 = 2.0f * (xy - wz);
    const float r02 = 2.0f * (xz + wy);

    const float r10 = 2.0f * (xy + wz);
    const float r11 = 1.0f - 2.0f * (xx + zz);
    const float r12 = 2.0f * (yz - wx);

    const float r20 = 2.0f * (xz - wy);
    const float r21 = 2.0f * (yz + wx);
    const float r22 = 1.0f - 2.0f * (xx + yy);

    const float tx = world_pos.x - (r00 * rest_pos.x + r01 * rest_pos.y + r02 * rest_pos.z);
    const float ty = world_pos.y - (r10 * rest_pos.x + r11 * rest_pos.y + r12 * rest_pos.z);
    const float tz = world_pos.z - (r20 * rest_pos.x + r21 * rest_pos.y + r22 * rest_pos.z);

    out_mat[0] = r00;
    out_mat[1] = r01;
    out_mat[2] = r02;
    out_mat[3] = tx;
    out_mat[4] = r10;
    out_mat[5] = r11;
    out_mat[6] = r12;
    out_mat[7] = ty;
    out_mat[8] = r20;
    out_mat[9] = r21;
    out_mat[10] = r22;
    out_mat[11] = tz;
}

} // namespace

SimEngine::SimEngine() noexcept {
    reset();
}

void SimEngine::reset(const RestSkeletonMetrics& rest) noexcept {
    one_euro_.reset();
    kalman_.reset();
    ik_solver_.set_rest_metrics(rest);
    const auto& rm = ik_solver_.rest_metrics();
    vis_solver_.reset(rm.rest_local_positions[AETHER_BONE_HIPS], {0.095f, 0.05f, 0.0f}, {-0.095f, 0.05f, 0.0f});
    spring_.configure_default_skirt_and_hair(rm);
    expr_mixer_.reset();

    filtered_state_ = {};
    predicted_state_ = {};
    solved_body_ = {};
    ik_world_pos_.fill({});
    ik_world_rot_.fill(Quat::identity());
    last_seen_frame_seq_ = 0;
    last_tick_us_ = 0;
    spring_accum_s_ = 0.0f;
    has_tracking_frame_ = false;
}

void SimEngine::set_rest_metrics(const RestSkeletonMetrics& rest) noexcept {
    ik_solver_.set_rest_metrics(rest);
    const auto& rm = ik_solver_.rest_metrics();
    const Vec3 hips_rest = rm.rest_local_positions[AETHER_BONE_HIPS];
    const Vec3 l_foot_rest = hips_rest + rm.rest_local_positions[AETHER_BONE_LEFT_UPPER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_LEFT_LOWER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_LEFT_FOOT];
    const Vec3 r_foot_rest = hips_rest + rm.rest_local_positions[AETHER_BONE_RIGHT_UPPER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_RIGHT_LOWER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_RIGHT_FOOT];
    vis_solver_.set_rest_anchors(hips_rest, l_foot_rest, r_foot_rest);
    spring_.configure_default_skirt_and_hair(rm);
}

void SimEngine::tick(const AetherTrackingFrame& raw_tracking, const AetherDspSyncPacket& dsp,
                     const AetherGraphOutputs& graph_out, uint64_t now_us, float horizon_override_s,
                     const SimEngineConfig& cfg, SkinningFrame& out_skinning) noexcept {
    float dt_s = 1.0f / 240.0f;
    if (last_tick_us_ > 0 && now_us > last_tick_us_) {
        dt_s = std::clamp(static_cast<float>(now_us - last_tick_us_) * 1e-6f, 1.0f / 480.0f, 0.05f);
    }
    last_tick_us_ = now_us;

    // 1. Apply 1-Euro & Kalman correction only on fresh capture sequence
    if (raw_tracking.frame_sequence != 0U &&
        (!has_tracking_frame_ || raw_tracking.frame_sequence != last_seen_frame_seq_)) {
        last_seen_frame_seq_ = raw_tracking.frame_sequence;
        has_tracking_frame_ = true;
        one_euro_.update(raw_tracking, filtered_state_);
        kalman_.correct(filtered_state_, cfg.kalman);
    } else if (has_tracking_frame_) {
        kalman_.tick_low_conf_damping(filtered_state_, cfg.kalman);
    }

    // 2. Dead-reckoning prediction horizon (measured capture-to-tick clamped to `<= 14 ms`)
    float horizon_s = 0.0f;
    if (horizon_override_s >= 0.0f) {
        horizon_s = std::clamp(horizon_override_s, 0.0f, cfg.kalman.max_horizon_s);
    } else if (filtered_state_.inference_complete_us > 0 && now_us > filtered_state_.inference_complete_us) {
        horizon_s = std::clamp(static_cast<float>(now_us - filtered_state_.inference_complete_us) * 1e-6f, 0.0f,
                               cfg.kalman.max_horizon_s);
    }
    kalman_.predict(horizon_s, filtered_state_, predicted_state_, cfg.kalman);

    // 3. Visibility-First posture + breathing + desk/thigh attractors + auto foot planting
    vis_solver_.solve(predicted_state_, graph_out, dt_s, cfg.visibility, solved_body_);

    // 4. Full-body IK (2-bone arms/legs + <=4 iter FABRIK head/neck + Appendix C joint limits)
    ik_solver_.solve(solved_body_, graph_out, solved_body_.local_rotations, ik_world_pos_, ik_world_rot_);

    // Compute rest world positions cleanly once from rest metrics
    const auto& rm = ik_solver_.rest_metrics();
    std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> rest_world_pos{};
    rest_world_pos[AETHER_BONE_HIPS] = rm.rest_local_positions[AETHER_BONE_HIPS];
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rest_world_pos[b] = rest_world_pos[pi] + rm.rest_local_positions[b];
    }

    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        bone_pos_ws_[b] = ik_world_pos_[b];
        bone_rot_ws_[b] = ik_world_rot_[b];
    }
    const uint32_t active_spring_bones =
        (spring_.joint_count() > 0) ? (64U + spring_.joint_count()) : AETHER_HUMANOID_BONE_COUNT;
    const uint32_t total_bones =
        std::min(kMaxBones, std::max<uint32_t>(AETHER_HUMANOID_BONE_COUNT, active_spring_bones));
    for (size_t b = AETHER_HUMANOID_BONE_COUNT; b < total_bones; ++b) {
        bone_pos_ws_[b] = bone_pos_ws_[AETHER_BONE_HEAD];
        bone_rot_ws_[b] = Quat::identity();
    }

    // 5. Fixed-timestep SpringBone Verlet (with clamped accumulator + SIMD AVX2/scalar)
    const Vec3 wind_ws{0.0f, 0.0f, 0.0f};
    const float drag_scale = 1.0f;
    (void)graph_out;
    spring_accum_s_ += dt_s;
    const float fdt = std::clamp(cfg.spring_fixed_dt_s, 1.0f / 240.0f, 1.0f / 30.0f);
    uint32_t substeps = 0;
    while (spring_accum_s_ >= fdt && substeps < cfg.spring_substeps_max) {
        spring_.step(fdt, wind_ws, drag_scale, bone_pos_ws_, bone_rot_ws_, total_bones, cfg.spring_simd_path);
        spring_accum_s_ -= fdt;
        ++substeps;
    }
    if (spring_accum_s_ >= fdt * static_cast<float>(cfg.spring_substeps_max)) {
        spring_accum_s_ = 0.0f;
    }

    // 6. Write 3x4 row-major skinning matrices into `SkinningFrame`
    out_skinning.bone_count = total_bones;
    out_skinning.morph_count = AETHER_HUMANOID_BONE_COUNT + 9U; // 52 ARKit + 5 visemes + custom
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        write_bone_skin_matrix(bone_pos_ws_[b], bone_rot_ws_[b], rest_world_pos[b], out_skinning.bone_matrices[b]);
    }
    for (size_t b = AETHER_HUMANOID_BONE_COUNT; b < total_bones; ++b) {
        // Virtual hair/cloth chain bones keep bind identity relative to their animated spring positions
        write_bone_skin_matrix(bone_pos_ws_[b], bone_rot_ws_[b], bone_pos_ws_[b], out_skinning.bone_matrices[b]);
    }

    // 7. Blendshapes + 60 ms DSP mouth-ownership transition
    expr_mixer_.update(predicted_state_.blendshapes.data(), dsp, graph_out, dt_s, out_skinning.morph_weights,
                       out_skinning.mouth_owner_blend);

    // 8. Fill head anchor vectors (render face shadow/SDF light basis) & DevShell HUD telemetry view
    const Vec3 head_fwd = rotate_vec(bone_rot_ws_[AETHER_BONE_HEAD], {0.0f, 0.0f, 1.0f});
    const Vec3 head_rgt = rotate_vec(bone_rot_ws_[AETHER_BONE_HEAD], {1.0f, 0.0f, 0.0f});
    out_skinning.head_pivot_ws[0] = bone_pos_ws_[AETHER_BONE_HEAD].x;
    out_skinning.head_pivot_ws[1] = bone_pos_ws_[AETHER_BONE_HEAD].y;
    out_skinning.head_pivot_ws[2] = bone_pos_ws_[AETHER_BONE_HEAD].z;
    out_skinning.head_pivot_ws[3] = 1.0f;

    out_skinning.head_forward_ws[0] = head_fwd.x;
    out_skinning.head_forward_ws[1] = head_fwd.y;
    out_skinning.head_forward_ws[2] = head_fwd.z;
    out_skinning.head_forward_ws[3] = 0.0f;

    out_skinning.head_right_ws[0] = head_rgt.x;
    out_skinning.head_right_ws[1] = head_rgt.y;
    out_skinning.head_right_ws[2] = head_rgt.z;
    out_skinning.head_right_ws[3] = 0.0f;

    for (size_t l = 0; l < 4; ++l) {
        for (size_t k = 0; k < 8; ++k) {
            out_skinning.light_overrides[l][k] = 0.0f;
        }
    }

    out_skinning.head_pos_ws[0] = bone_pos_ws_[AETHER_BONE_HEAD].x;
    out_skinning.head_pos_ws[1] = bone_pos_ws_[AETHER_BONE_HEAD].y;
    out_skinning.head_pos_ws[2] = bone_pos_ws_[AETHER_BONE_HEAD].z;

    out_skinning.tracking_confidences[0] = (predicted_state_.valid_flags & 0x01U) ? 1.0f : 0.0f;
    out_skinning.tracking_confidences[1] = predicted_state_.upper_body_confidence;
    out_skinning.tracking_confidences[2] = predicted_state_.left_hand_confidence;
    out_skinning.tracking_confidences[3] = predicted_state_.right_hand_confidence;
    out_skinning.tracking_confidences[4] = predicted_state_.lower_body_confidence;
    out_skinning.active_ep = predicted_state_.active_execution_provider;
    out_skinning.tracking_valid_flags = predicted_state_.valid_flags;
    for (size_t i = 0; i < 52U; ++i) {
        out_skinning.arkit_preview[i] = out_skinning.morph_weights[i];
    }

    out_skinning.stamps.frame_sequence = raw_tracking.frame_sequence;
    out_skinning.stamps.t1_capture_us = raw_tracking.capture_timestamp_us;
}

} // namespace aether::animation
