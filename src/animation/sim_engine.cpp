#include "animation/sim_engine.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

void write_bone_skin_matrix(const Vec3& world_pos, const Quat& world_rot, const Vec3& rest_pos,
                            const Quat& rest_world_rot, float out_mat[12]) noexcept {
    // 3x4 row-major skinning matrix M = T(world_pos) * R(world_rot * conj(rest_world_rot)) * T(-rest_pos)
    const float rw2 = rest_world_rot.w * rest_world_rot.w + rest_world_rot.x * rest_world_rot.x +
                      rest_world_rot.y * rest_world_rot.y + rest_world_rot.z * rest_world_rot.z;
    const Quat rw_unit = (rw2 > 1e-6f) ? normalize(rest_world_rot) : Quat::identity();
    const Quat delta_rot = normalize(normalize(world_rot) * conjugate(rw_unit));

    const float xx = delta_rot.x * delta_rot.x;
    const float yy = delta_rot.y * delta_rot.y;
    const float zz = delta_rot.z * delta_rot.z;
    const float xy = delta_rot.x * delta_rot.y;
    const float xz = delta_rot.x * delta_rot.z;
    const float yz = delta_rot.y * delta_rot.z;
    const float wx = delta_rot.w * delta_rot.x;
    const float wy = delta_rot.w * delta_rot.y;
    const float wz = delta_rot.w * delta_rot.z;

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

void init_neutral_pose_and_springs(const RestSkeletonMetrics& rm, VisibilityFirstSolver& vis_solver,
                                   FullBodyIkSolver& ik_solver, SpringBoneSimulator& spring,
                                   SolvedBodyPose& solved_body,
                                   std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& ik_world_pos,
                                   std::array<Quat, AETHER_HUMANOID_BONE_COUNT>& ik_world_rot,
                                   std::array<Vec3, kMaxBones>& bone_pos_ws,
                                   std::array<Quat, kMaxBones>& bone_rot_ws) noexcept {
    // Place skeleton in neutral upright A-pose BEFORE initializing SpringBone lengths/colliders.
    FilteredTrackingState neutral{};
    AetherGraphOutputs empty_graph{};
    VisibilityFirstConfig vcfg{};
    vis_solver.solve(neutral, empty_graph, 1.0f / 60.0f, vcfg, solved_body);
    ik_solver.solve(solved_body, empty_graph, solved_body.local_rotations, ik_world_pos, ik_world_rot);
    bone_pos_ws.fill(rm.rest_local_positions[AETHER_BONE_HEAD]);
    bone_rot_ws.fill(Quat::identity());
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        bone_pos_ws[b] = ik_world_pos[b];
        bone_rot_ws[b] = ik_world_rot[b];
    }

    spring.configure_default_skirt_and_hair(rm);
    if (rm.custom_spring_count > 0U) {
        const uint32_t custom_n = std::min<uint32_t>(rm.custom_spring_count, kMaxRestSprings);
        for (uint32_t si = 0; si < custom_n; ++si) {
            const auto& sp = rm.custom_springs[si];
            const size_t bi = static_cast<size_t>(kCustomSpringBoneOffset + si);
            if (bi < kMaxBones) {
                bone_pos_ws[bi] = sp.rest_world_pos;
                const uint32_t hb = static_cast<uint32_t>(sp.skin_base_humanoid);
                bone_rot_ws[bi] =
                    (hb < AETHER_HUMANOID_BONE_COUNT) ? rm.rest_world_rotations[hb] : Quat::identity();
            }
        }
    }
    const uint32_t total_bones =
        std::min(kMaxBones, (spring.joint_count() > 0) ? (64U + spring.joint_count()) : AETHER_HUMANOID_BONE_COUNT);
    spring.reset_state(bone_pos_ws, bone_rot_ws, total_bones);
    spring.warmup_settle(bone_pos_ws, bone_rot_ws, total_bones, 60U);
}

} // namespace

SimEngine::SimEngine() noexcept {
    reset();
}

void SimEngine::reset(const RestSkeletonMetrics& rest) noexcept {
    trickster_.reset();
    one_euro_.reset();
    kalman_.reset();
    ik_solver_.set_rest_metrics(rest);
    const auto& rm = ik_solver_.rest_metrics();
    const Vec3 hips_rest = rm.rest_local_positions[AETHER_BONE_HIPS];
    const Vec3 l_foot_rest = hips_rest + rm.rest_local_positions[AETHER_BONE_LEFT_UPPER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_LEFT_LOWER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_LEFT_FOOT];
    const Vec3 r_foot_rest = hips_rest + rm.rest_local_positions[AETHER_BONE_RIGHT_UPPER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_RIGHT_LOWER_LEG] +
                             rm.rest_local_positions[AETHER_BONE_RIGHT_FOOT];
    vis_solver_.reset(hips_rest, l_foot_rest, r_foot_rest);
    vis_solver_.set_rest_metrics(rm);
    expr_mixer_.reset();

    filtered_state_ = {};
    predicted_state_ = {};
    solved_body_ = {};
    ik_world_pos_.fill({});
    ik_world_rot_.fill(Quat::identity());

    init_neutral_pose_and_springs(rm, vis_solver_, ik_solver_, spring_, solved_body_, ik_world_pos_, ik_world_rot_,
                                  bone_pos_ws_, bone_rot_ws_);

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
    vis_solver_.set_rest_metrics(rm);

    init_neutral_pose_and_springs(rm, vis_solver_, ik_solver_, spring_, solved_body_, ik_world_pos_, ik_world_rot_,
                                  bone_pos_ws_, bone_rot_ws_);
}

void SimEngine::tick(const AetherTrackingFrame& raw_tracking, const AetherDspSyncPacket& dsp,
                     const AetherGraphOutputs& graph_out, uint64_t now_us, float horizon_override_s,
                     const SimEngineConfig& cfg, SkinningFrame& out_skinning,
                     const AudioLipSyncFrame* audio_lipsync, float audio_assist_weight) noexcept {
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
        // Trickster stage A/B (3-frame median spike rejection + smooth centre deadzone) before 1-Euro/Kalman.
        stabilized_frame_ = raw_tracking;
        trickster_.apply(stabilized_frame_);
        one_euro_.update(stabilized_frame_, filtered_state_);
        kalman_.correct(filtered_state_, cfg.kalman);
    } else if (has_tracking_frame_) {
        kalman_.tick_low_conf_damping(filtered_state_, cfg.kalman);
    }

    // 2. Dead-reckoning prediction horizon: exact linear up to max_horizon_s (14 ms), then
    // smoothly damped velocity continuation across 30 Hz camera intervals up to max_extrapolated_s (42 ms).
    float horizon_s = 0.0f;
    if (horizon_override_s >= 0.0f) {
        horizon_s = std::clamp(horizon_override_s, 0.0f, cfg.kalman.max_extrapolated_s);
    } else if (filtered_state_.inference_complete_us > 0 && now_us > filtered_state_.inference_complete_us) {
        horizon_s = std::clamp(static_cast<float>(now_us - filtered_state_.inference_complete_us) * 1e-6f, 0.0f,
                               cfg.kalman.max_extrapolated_s);
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

    // 5. Fixed-timestep SpringBone Verlet (preserve previous virtual bone poses on render-only frames)
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
        write_bone_skin_matrix(bone_pos_ws_[b], bone_rot_ws_[b], rest_world_pos[b], rm.rest_world_rotations[b],
                               out_skinning.bone_matrices[b]);
    }
    const uint32_t custom_n =
        (rm.custom_spring_count > 0U) ? std::min<uint32_t>(rm.custom_spring_count, kMaxRestSprings) : 0U;
    for (size_t b = AETHER_HUMANOID_BONE_COUNT; b < total_bones; ++b) {
        if (b >= kCustomSpringBoneOffset && b - kCustomSpringBoneOffset < custom_n) {
            const auto& sp = rm.custom_springs[b - kCustomSpringBoneOffset];
            const uint32_t hb = static_cast<uint32_t>(sp.skin_base_humanoid);
            const Quat base_rest_rot =
                (hb < AETHER_HUMANOID_BONE_COUNT) ? rm.rest_world_rotations[hb] : Quat::identity();
            write_bone_skin_matrix(bone_pos_ws_[b], bone_rot_ws_[b], sp.rest_world_pos, base_rest_rot,
                                   out_skinning.bone_matrices[b]);
        } else {
            // Virtual hair/cloth fallback chain bones keep bind identity relative to animated spring poses
            write_bone_skin_matrix(bone_pos_ws_[b], bone_rot_ws_[b], bone_pos_ws_[b], bone_rot_ws_[b],
                                   out_skinning.bone_matrices[b]);
        }
    }

    // 7. Blendshapes + Live2D parameters + 60 ms DSP mouth-ownership transition
    Live2DParameters raw_l2d{};
    unpack_live2d_from_tracking_frame(stabilized_frame_, raw_l2d);
    const Live2DParameters* const opt_raw_l2d = has_packed_live2d(stabilized_frame_) ? &raw_l2d : nullptr;
    out_skinning.live2d = Live2DParameters{};
    expr_mixer_.update(predicted_state_.blendshapes.data(), dsp, graph_out, dt_s, out_skinning.morph_weights,
                       out_skinning.mouth_owner_blend, predicted_state_.webcam_visemes.data(),
                       rm.has_split_brow_morphs, opt_raw_l2d, &out_skinning.live2d, audio_lipsync,
                       audio_assist_weight);

    const auto world_delta_euler_deg = [&](uint32_t bone_id) noexcept -> Vec3 {
        const Quat rw = rm.rest_world_rotations[bone_id];
        const float rw2 = rw.w * rw.w + rw.x * rw.x + rw.y * rw.y + rw.z * rw.z;
        const Quat rest_world = (rw2 > 1e-6f) ? normalize(rw) : Quat::identity();
        const Quat q_delta = normalize(bone_rot_ws_[bone_id] * conjugate(rest_world));
        return to_euler_xyz(q_delta) * kRadToDeg;
    };
    const Vec3 head_deg = world_delta_euler_deg(AETHER_BONE_HEAD);
    const Vec3 chest_deg = world_delta_euler_deg(AETHER_BONE_CHEST);
    out_skinning.live2d.param_angle_x = std::clamp(head_deg.y, -30.0f, 30.0f);
    out_skinning.live2d.param_angle_y = std::clamp(head_deg.x, -30.0f, 30.0f);
    out_skinning.live2d.param_angle_z = std::clamp(head_deg.z, -30.0f, 30.0f);
    out_skinning.live2d.param_body_angle_x = std::clamp(chest_deg.y, -10.0f, 10.0f);
    out_skinning.live2d.param_body_angle_y = std::clamp(chest_deg.x, -10.0f, 10.0f);
    out_skinning.live2d.param_body_angle_z = std::clamp(chest_deg.z, -10.0f, 10.0f);

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

void SimEngine::export_vmc_pose(const SkinningFrame& skin, float time_s, VmcPoseFrame& out) const noexcept {
    const auto& rm = ik_solver_.rest_metrics();
    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> delta{};
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const Quat rw = rm.rest_world_rotations[b];
        const float rw2 = rw.w * rw.w + rw.x * rw.x + rw.y * rw.y + rw.z * rw.z;
        const Quat rest_world = (rw2 > 1e-6f) ? normalize(rw) : Quat::identity();
        delta[b] = normalize(bone_rot_ws_[b] * conjugate(rest_world));
    }
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const bool has_parent = (b != AETHER_BONE_HIPS) && p >= 0 && p < static_cast<int32_t>(b);
        Quat q = has_parent ? normalize(conjugate(delta[static_cast<size_t>(p)]) * delta[b]) : delta[b];
        if (q.w < 0.0f) {
            q = Quat{-q.x, -q.y, -q.z, -q.w};
        }
        const Vec3 lp = has_parent ? rm.rest_local_positions[b] : bone_pos_ws_[AETHER_BONE_HIPS];
        // glTF right-handed -> Unity left-handed (mirror X): p = (-x, y, z), q = (x, -y, -z, w)
        out.local_pos[b] = {-lp.x, lp.y, lp.z};
        out.local_rot[b] = {q.x, -q.y, -q.z, q.w};
    }
    for (size_t v = 0; v < out.visemes.size(); ++v) {
        out.visemes[v] = skin.morph_weights[kVisemeSlotOffset + v];
    }
    out.blink_l = skin.morph_weights[0];
    out.blink_r = skin.morph_weights[7];
    out.time_s = time_s;
    out.valid = true;
}

void SimEngine::export_spring_overlay(SpringColliderOverlay* out_cols, uint32_t max_cols, uint32_t& out_col_count,
                                      SpringSegmentOverlay* out_segs, uint32_t max_segs,
                                      uint32_t& out_seg_count) const noexcept {
    spring_.export_overlay(out_cols, max_cols, out_col_count, out_segs, max_segs, out_seg_count, bone_pos_ws_);
}

} // namespace aether::animation
