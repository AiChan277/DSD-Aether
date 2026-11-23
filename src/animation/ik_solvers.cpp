#include "animation/ik_solvers.hpp"
#include "animation/hand_collision.hpp"
#include "animation/visibility_first.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

RestSkeletonMetrics build_default_humanoid_rest_metrics() noexcept {
    RestSkeletonMetrics m{};
    m.parent_indices.fill(-1);
    m.rest_local_rotations.fill(Quat::identity());
    m.rest_world_rotations.fill(Quat::identity());
    m.bone_lengths.fill(0.06f);

    // Core spine/neck/head chain (0..6)
    m.parent_indices[AETHER_BONE_HIPS] = -1;
    m.rest_local_positions[AETHER_BONE_HIPS] = {0.0f, 0.90f, 0.0f};
    m.parent_indices[AETHER_BONE_SPINE] = AETHER_BONE_HIPS;
    m.rest_local_positions[AETHER_BONE_SPINE] = {0.0f, 0.12f, 0.0f};
    m.parent_indices[AETHER_BONE_CHEST] = AETHER_BONE_SPINE;
    m.rest_local_positions[AETHER_BONE_CHEST] = {0.0f, 0.14f, 0.0f};
    m.parent_indices[AETHER_BONE_UPPER_CHEST] = AETHER_BONE_CHEST;
    m.rest_local_positions[AETHER_BONE_UPPER_CHEST] = {0.0f, 0.14f, 0.0f};
    m.parent_indices[AETHER_BONE_NECK] = AETHER_BONE_UPPER_CHEST;
    m.rest_local_positions[AETHER_BONE_NECK] = {0.0f, 0.10f, 0.0f};
    m.parent_indices[AETHER_BONE_HEAD] = AETHER_BONE_NECK;
    m.rest_local_positions[AETHER_BONE_HEAD] = {0.0f, 0.09f, 0.0f};
    m.parent_indices[AETHER_BONE_JAW] = AETHER_BONE_HEAD;
    m.rest_local_positions[AETHER_BONE_JAW] = {0.0f, -0.02f, 0.04f};
    m.parent_indices[AETHER_BONE_LEFT_EYE] = AETHER_BONE_HEAD;
    m.rest_local_positions[AETHER_BONE_LEFT_EYE] = {0.032f, 0.06f, 0.08f};
    m.parent_indices[AETHER_BONE_RIGHT_EYE] = AETHER_BONE_HEAD;
    m.rest_local_positions[AETHER_BONE_RIGHT_EYE] = {-0.032f, 0.06f, 0.08f};

    // Left / Right Arm T-pose
    m.parent_indices[AETHER_BONE_LEFT_SHOULDER] = AETHER_BONE_UPPER_CHEST;
    m.rest_local_positions[AETHER_BONE_LEFT_SHOULDER] = {0.04f, 0.06f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_UPPER_ARM] = AETHER_BONE_LEFT_SHOULDER;
    m.rest_local_positions[AETHER_BONE_LEFT_UPPER_ARM] = {0.11f, 0.0f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_LOWER_ARM] = AETHER_BONE_LEFT_UPPER_ARM;
    m.rest_local_positions[AETHER_BONE_LEFT_LOWER_ARM] = {0.26f, 0.0f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_HAND] = AETHER_BONE_LEFT_LOWER_ARM;
    m.rest_local_positions[AETHER_BONE_LEFT_HAND] = {0.24f, 0.0f, 0.0f};

    m.parent_indices[AETHER_BONE_RIGHT_SHOULDER] = AETHER_BONE_UPPER_CHEST;
    m.rest_local_positions[AETHER_BONE_RIGHT_SHOULDER] = {-0.04f, 0.06f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_UPPER_ARM] = AETHER_BONE_RIGHT_SHOULDER;
    m.rest_local_positions[AETHER_BONE_RIGHT_UPPER_ARM] = {-0.11f, 0.0f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_LOWER_ARM] = AETHER_BONE_RIGHT_UPPER_ARM;
    m.rest_local_positions[AETHER_BONE_RIGHT_LOWER_ARM] = {-0.26f, 0.0f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_HAND] = AETHER_BONE_RIGHT_LOWER_ARM;
    m.rest_local_positions[AETHER_BONE_RIGHT_HAND] = {-0.24f, 0.0f, 0.0f};

    // Legs
    m.parent_indices[AETHER_BONE_LEFT_UPPER_LEG] = AETHER_BONE_HIPS;
    m.rest_local_positions[AETHER_BONE_LEFT_UPPER_LEG] = {0.095f, -0.05f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_LOWER_LEG] = AETHER_BONE_LEFT_UPPER_LEG;
    m.rest_local_positions[AETHER_BONE_LEFT_LOWER_LEG] = {0.0f, -0.40f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_FOOT] = AETHER_BONE_LEFT_LOWER_LEG;
    m.rest_local_positions[AETHER_BONE_LEFT_FOOT] = {0.0f, -0.40f, 0.0f};
    m.parent_indices[AETHER_BONE_LEFT_TOES] = AETHER_BONE_LEFT_FOOT;
    m.rest_local_positions[AETHER_BONE_LEFT_TOES] = {0.0f, -0.04f, 0.12f};

    m.parent_indices[AETHER_BONE_RIGHT_UPPER_LEG] = AETHER_BONE_HIPS;
    m.rest_local_positions[AETHER_BONE_RIGHT_UPPER_LEG] = {-0.095f, -0.05f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_LOWER_LEG] = AETHER_BONE_RIGHT_UPPER_LEG;
    m.rest_local_positions[AETHER_BONE_RIGHT_LOWER_LEG] = {0.0f, -0.40f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_FOOT] = AETHER_BONE_RIGHT_LOWER_LEG;
    m.rest_local_positions[AETHER_BONE_RIGHT_FOOT] = {0.0f, -0.40f, 0.0f};
    m.parent_indices[AETHER_BONE_RIGHT_TOES] = AETHER_BONE_RIGHT_FOOT;
    m.rest_local_positions[AETHER_BONE_RIGHT_TOES] = {0.0f, -0.04f, 0.12f};

    // Fingers (15 left, 15 right)
    for (int f = 0; f < 5; ++f) {
        const float z_off = 0.03f - 0.015f * static_cast<float>(f);
        const uint32_t lb = AETHER_BONE_LEFT_THUMB_PROXIMAL + static_cast<uint32_t>(f * 3);
        m.parent_indices[lb + 0] = AETHER_BONE_LEFT_HAND;
        m.rest_local_positions[lb + 0] = {0.06f, 0.0f, z_off};
        m.parent_indices[lb + 1] = static_cast<int32_t>(lb + 0);
        m.rest_local_positions[lb + 1] = {0.035f, 0.0f, 0.0f};
        m.parent_indices[lb + 2] = static_cast<int32_t>(lb + 1);
        m.rest_local_positions[lb + 2] = {0.025f, 0.0f, 0.0f};

        const uint32_t rb = AETHER_BONE_RIGHT_THUMB_PROXIMAL + static_cast<uint32_t>(f * 3);
        m.parent_indices[rb + 0] = AETHER_BONE_RIGHT_HAND;
        m.rest_local_positions[rb + 0] = {-0.06f, 0.0f, z_off};
        m.parent_indices[rb + 1] = static_cast<int32_t>(rb + 0);
        m.rest_local_positions[rb + 1] = {-0.035f, 0.0f, 0.0f};
        m.parent_indices[rb + 2] = static_cast<int32_t>(rb + 1);
        m.rest_local_positions[rb + 2] = {-0.025f, 0.0f, 0.0f};
    }

    for (size_t i = 1; i < AETHER_HUMANOID_BONE_COUNT; ++i) {
        m.bone_lengths[i] = std::max(length(m.rest_local_positions[i]), 0.02f);
    }
    m.valid = true;
    return m;
}

JointLimitEulerRad joint_limit_for_bone(AetherHumanoidBone bone) noexcept {
    switch (bone) {
    case AETHER_BONE_NECK:
    case AETHER_BONE_HEAD:
        return {-40.0f * kDegToRad, 40.0f * kDegToRad,  -55.0f * kDegToRad,
                55.0f * kDegToRad,  -35.0f * kDegToRad, 35.0f * kDegToRad};
    case AETHER_BONE_SPINE:
    case AETHER_BONE_CHEST:
    case AETHER_BONE_UPPER_CHEST:
        return {-25.0f * kDegToRad, 35.0f * kDegToRad,  -35.0f * kDegToRad,
                35.0f * kDegToRad,  -25.0f * kDegToRad, 25.0f * kDegToRad};
    case AETHER_BONE_LEFT_SHOULDER:
    case AETHER_BONE_RIGHT_SHOULDER:
        return {-20.0f * kDegToRad, 20.0f * kDegToRad,  -20.0f * kDegToRad,
                20.0f * kDegToRad,  -30.0f * kDegToRad, 30.0f * kDegToRad};
    case AETHER_BONE_LEFT_UPPER_ARM:
    case AETHER_BONE_RIGHT_UPPER_ARM:
        return {-95.0f * kDegToRad, 150.0f * kDegToRad, -95.0f * kDegToRad,
                95.0f * kDegToRad,  -95.0f * kDegToRad, 95.0f * kDegToRad};
    case AETHER_BONE_LEFT_LOWER_ARM:
    case AETHER_BONE_RIGHT_LOWER_ARM:
        // Hinge elbow flexes without hyper-extension
        return {-10.0f * kDegToRad, 15.0f * kDegToRad,  -148.0f * kDegToRad,
                148.0f * kDegToRad, -15.0f * kDegToRad, 15.0f * kDegToRad};
    case AETHER_BONE_LEFT_UPPER_LEG:
    case AETHER_BONE_RIGHT_UPPER_LEG:
        return {-110.0f * kDegToRad, 45.0f * kDegToRad,  -45.0f * kDegToRad,
                45.0f * kDegToRad,   -45.0f * kDegToRad, 45.0f * kDegToRad};
    case AETHER_BONE_LEFT_LOWER_LEG:
    case AETHER_BONE_RIGHT_LOWER_LEG:
        // Anatomical knee: pitch >= 0 only (no backwards bend!)
        return {0.0f, 145.0f * kDegToRad, -10.0f * kDegToRad, 10.0f * kDegToRad, -10.0f * kDegToRad, 10.0f * kDegToRad};
    default:
        return {-85.0f * kDegToRad, 85.0f * kDegToRad,  -85.0f * kDegToRad,
                85.0f * kDegToRad,  -85.0f * kDegToRad, 85.0f * kDegToRad};
    }
}

Quat clamp_rotation_to_limits(Quat q, const JointLimitEulerRad& lim) noexcept {
    const Vec3 e = to_euler_xyz(normalize(q));
    return from_euler_xyz(std::clamp(e.x, lim.min_x, lim.max_x), std::clamp(e.y, lim.min_y, lim.max_y),
                          std::clamp(e.z, lim.min_z, lim.max_z));
}

TwoBoneIkResult solve_two_bone_ik(Vec3 root_ws, Vec3 target_ws, Vec3 pole_dir_ws, float len_upper, float len_lower,
                                  Vec3 upper_rest_dir, Vec3 lower_rest_dir, Quat upper_rest_world,
                                  Quat lower_rest_world, float weight) noexcept {
    TwoBoneIkResult res{};
    const Vec3 to_target = target_ws - root_ws;
    const float max_reach = 0.998f * (len_upper + len_lower);
    const float min_reach = std::max(0.02f, std::abs(len_upper - len_lower) + 0.01f);
    const float dist = std::clamp(length(to_target), min_reach, max_reach);
    const Vec3 dir = normalize_safe(to_target, upper_rest_dir);

    // Law of cosines for interior triangle angle at root
    const float cos_a = std::clamp(
        (len_upper * len_upper + dist * dist - len_lower * len_lower) / (2.0f * len_upper * dist), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));

    // Project pole direction perpendicular to dir
    Vec3 bend = pole_dir_ws - dir * dot(pole_dir_ws, dir);
    if (length_sq(bend) < 1e-6f) {
        bend = cross(dir, {0.0f, 1.0f, 0.0f});
        if (length_sq(bend) < 1e-6f) {
            bend = cross(dir, {0.0f, 0.0f, 1.0f});
        }
    }
    bend = normalize_safe(bend);

    const Vec3 upper_dir = normalize_safe(dir * cos_a + bend * sin_a);
    res.mid_joint_ws = root_ws + upper_dir * len_upper;
    const Vec3 lower_dir = normalize_safe((root_ws + dir * dist) - res.mid_joint_ws);
    res.end_effector_ws = res.mid_joint_ws + lower_dir * len_lower;

    const Quat solved_u = normalize(quat_from_to(upper_rest_dir, upper_dir) * upper_rest_world);
    const Quat solved_l = normalize(quat_from_to(lower_rest_dir, lower_dir) * lower_rest_world);

    const float w = std::clamp(weight, 0.0f, 1.0f);
    res.upper_world_rot = slerp(upper_rest_world, solved_u, w);
    res.lower_world_rot = slerp(lower_rest_world, solved_l, w);
    return res;
}

void FullBodyIkSolver::set_rest_metrics(const RestSkeletonMetrics& metrics) noexcept {
    metrics_ = metrics.valid ? metrics : build_default_humanoid_rest_metrics();
}

void FullBodyIkSolver::solve(const SolvedBodyPose& body_pose, const AetherGraphOutputs& /*graph_out*/,
                             std::array<Quat, AETHER_HUMANOID_BONE_COUNT>& io_local_rots,
                             std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& out_world_positions,
                             std::array<Quat, AETHER_HUMANOID_BONE_COUNT>& out_world_rotations) const noexcept {
    // 1. Forward kinematics pass from Visibility-First local rotations (with Appendix C limits)
    const auto compute_fk = [&]() {
        out_world_positions[AETHER_BONE_HIPS] = body_pose.hips_pos_ws;
        out_world_rotations[AETHER_BONE_HIPS] = normalize(io_local_rots[AETHER_BONE_HIPS]);
        for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            const int32_t p = metrics_.parent_indices[b];
            const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
            if (b >= AETHER_BONE_SPINE) {
                const Quat rl = metrics_.rest_local_rotations[b];
                const float rl2 = rl.w * rl.w + rl.x * rl.x + rl.y * rl.y + rl.z * rl.z;
                const Quat rest_unit = (rl2 > 1e-6f) ? normalize(rl) : Quat::identity();

                const Quat rw = metrics_.rest_world_rotations[b];
                const float rw2 = rw.w * rw.w + rw.x * rw.x + rw.y * rw.y + rw.z * rw.z;
                const Quat rw_unit = (rw2 > 1e-6f) ? normalize(rw) : Quat::identity();

                // Express local delta in character anatomical rest axes before applying anatomical Euler limits.
                const Quat delta_local = normalize(conjugate(rest_unit) * io_local_rots[b]);
                const Quat delta_anatomical = normalize(rw_unit * delta_local * conjugate(rw_unit));
                const Quat clamped_anatomical = clamp_rotation_to_limits(
                    delta_anatomical, joint_limit_for_bone(static_cast<AetherHumanoidBone>(b)));
                const Quat clamped_local = normalize(conjugate(rw_unit) * clamped_anatomical * rw_unit);
                io_local_rots[b] = normalize(rest_unit * clamped_local);
            }
            out_world_rotations[b] = normalize(out_world_rotations[pi] * io_local_rots[b]);
            const Quat rpw = metrics_.rest_world_rotations[pi];
            const float rpw2 = rpw.w * rpw.w + rpw.x * rpw.x + rpw.y * rpw.y + rpw.z * rpw.z;
            const Quat rest_parent_world = (rpw2 > 1e-6f) ? normalize(rpw) : Quat::identity();
            const Quat parent_delta_world = normalize(out_world_rotations[pi] * conjugate(rest_parent_world));
            out_world_positions[b] =
                out_world_positions[pi] + rotate_vec(parent_delta_world, metrics_.rest_local_positions[b]);
        }
    };
    compute_fk();

    // 2. Optional 4-joint FABRIK spine-neck-head chain (`<= 4` iterations) when graph head IK target active
    if (body_pose.head_target_weight > 0.01f) {
        constexpr std::array<size_t, 4> kChain = {AETHER_BONE_SPINE, AETHER_BONE_CHEST, AETHER_BONE_NECK,
                                                  AETHER_BONE_HEAD};
        Vec3 pts[4]{};
        float lens[3]{};
        for (size_t i = 0; i < 4; ++i) {
            pts[i] = out_world_positions[kChain[i]];
        }
        for (size_t i = 0; i < 3; ++i) {
            lens[i] = std::max(length(pts[i + 1] - pts[i]), 0.05f);
        }
        const Vec3 base_anchor = pts[0];
        const Vec3 target =
            lerp(pts[3], body_pose.head_target_pos_ws, std::clamp(body_pose.head_target_weight, 0.0f, 1.0f));
        for (int iter = 0; iter < 4; ++iter) {
            // Backward
            pts[3] = target;
            for (int i = 2; i >= 0; --i) {
                const Vec3 d = normalize_safe(pts[i] - pts[i + 1], {0.0f, -1.0f, 0.0f});
                pts[i] = pts[i + 1] + d * lens[i];
            }
            // Forward
            pts[0] = base_anchor;
            for (size_t i = 0; i < 3; ++i) {
                const Vec3 d = normalize_safe(pts[i + 1] - pts[i], {0.0f, 1.0f, 0.0f});
                pts[i + 1] = pts[i] + d * lens[i];
            }
            if (length(pts[3] - target) < 0.002f) {
                break;
            }
        }
        // Map FABRIK chain directions back to Spine/Chest/Neck local rotations. Re-run FK after each joint so the
        // next joint sees its parent's updated world rotation; anatomical limits are applied rest-relative inside
        // compute_fk() (clamping the full local quaternion here would break VRM 1.0 rigs with non-identity rest).
        for (size_t i = 0; i < 3; ++i) {
            const size_t b = kChain[i];
            const size_t child = kChain[i + 1];
            const int32_t pi = metrics_.parent_indices[b];
            const Quat p_world = (pi >= 0) ? out_world_rotations[static_cast<size_t>(pi)] : Quat::identity();
            const Vec3 cur_dir = normalize_safe(out_world_positions[child] - out_world_positions[b]);
            const Vec3 new_dir = normalize_safe(pts[i + 1] - pts[i]);
            const Quat new_world = normalize(quat_from_to(cur_dir, new_dir) * out_world_rotations[b]);
            io_local_rots[b] = normalize(conjugate(p_world) * new_world);
            compute_fk();
        }
    }

    // 3. Analytic 2-bone IK for Left & Right Arms + Legs (`§9.5`)
    const auto apply_arm_ik = [&](AetherHumanoidBone sh_b, AetherHumanoidBone up_b, AetherHumanoidBone lo_b,
                                  AetherHumanoidBone hand_b, Vec3 target_ws, Vec3 pole_hint, float weight) {
        const Vec3 root_ws = out_world_positions[up_b];
        const float l1 = metrics_.bone_lengths[lo_b];
        const float l2 = metrics_.bone_lengths[hand_b];
        const Vec3 rest_u = normalize_safe(out_world_positions[lo_b] - out_world_positions[up_b]);
        const Vec3 rest_l = normalize_safe(out_world_positions[hand_b] - out_world_positions[lo_b]);
        const TwoBoneIkResult ik = solve_two_bone_ik(root_ws, target_ws, pole_hint, l1, l2, rest_u, rest_l,
                                                     out_world_rotations[up_b], out_world_rotations[lo_b], weight);
        const Quat sh_world = out_world_rotations[sh_b];
        io_local_rots[up_b] =
            clamp_rotation_to_limits(normalize(conjugate(sh_world) * ik.upper_world_rot), joint_limit_for_bone(up_b));
        const Quat actual_up_world = normalize(sh_world * io_local_rots[up_b]);
        io_local_rots[lo_b] = clamp_rotation_to_limits(normalize(conjugate(actual_up_world) * ik.lower_world_rot),
                                                       joint_limit_for_bone(lo_b));
    };

    const Vec3 forward_chest = rotate_vec(out_world_rotations[AETHER_BONE_UPPER_CHEST], {0.0f, 0.0f, 1.0f});
    Vec3 l_elbow_pole = rotate_vec(out_world_rotations[AETHER_BONE_UPPER_CHEST], {0.35f, -0.45f, -0.82f});
    Vec3 r_elbow_pole = rotate_vec(out_world_rotations[AETHER_BONE_UPPER_CHEST], {-0.35f, -0.45f, -0.82f});
    const bool l_active = (body_pose.left_wrist_track_weight >= 0.05f);
    const bool r_active = (body_pose.right_wrist_track_weight >= 0.05f);
    if (l_active || r_active) {
        Vec3 l_target = body_pose.left_wrist_target_ws;
        Vec3 r_target = body_pose.right_wrist_target_ws;

        // Stage 1: Pre-IK tangential wrist slide against chest (+3.5 cm margin), neck/head, and passive/bilateral arm
        BodySelfCollisionField field{};
        field.build_from_fk(out_world_positions, !l_active, !r_active);
        field.resolve_pre_ik_wrists(l_active, l_target, r_active, r_target);

        for (int iter = 0; iter < 4; ++iter) {
            if (l_active) {
                apply_arm_ik(AETHER_BONE_LEFT_SHOULDER, AETHER_BONE_LEFT_UPPER_ARM, AETHER_BONE_LEFT_LOWER_ARM,
                             AETHER_BONE_LEFT_HAND, l_target, l_elbow_pole, body_pose.left_wrist_track_weight);
            }
            if (r_active) {
                apply_arm_ik(AETHER_BONE_RIGHT_SHOULDER, AETHER_BONE_RIGHT_UPPER_ARM, AETHER_BONE_RIGHT_LOWER_ARM,
                             AETHER_BONE_RIGHT_HAND, r_target, r_elbow_pole, body_pose.right_wrist_track_weight);
            }
            compute_fk();

            // Stage 2: Post-IK forearm/elbow push-out + 2-bone IK refinement until min signed distance >= 0
            field.build_from_fk(out_world_positions, true, true);
            bool any_penetration = false;
            if (l_active) {
                Vec3 n_fore{};
                Vec3 n_hand{};
                const float sd_fore = field.query_segment(
                    out_world_positions[AETHER_BONE_LEFT_LOWER_ARM], out_world_positions[AETHER_BONE_LEFT_HAND],
                    BodySelfCollisionField::kForearmRadiusM, CollisionCapsuleKind::LeftArm, 1.0f, &n_fore);
                const float sd_hand = field.query_point(out_world_positions[AETHER_BONE_LEFT_HAND],
                                                        BodySelfCollisionField::kHandSphereRadiusM,
                                                        CollisionCapsuleKind::LeftArm, 1.0f, &n_hand);
                const float worst = std::min(sd_fore, sd_hand);
                if (worst < 0.0f) {
                    const Vec3 push_n = (sd_fore < sd_hand) ? n_fore : n_hand;
                    l_target = l_target + push_n * (-worst + 0.004f);
                    l_elbow_pole = normalize_safe(l_elbow_pole + push_n * 0.45f, l_elbow_pole);
                    any_penetration = true;
                }
            }
            if (r_active) {
                Vec3 n_fore{};
                Vec3 n_hand{};
                const float sd_fore = field.query_segment(
                    out_world_positions[AETHER_BONE_RIGHT_LOWER_ARM], out_world_positions[AETHER_BONE_RIGHT_HAND],
                    BodySelfCollisionField::kForearmRadiusM, CollisionCapsuleKind::RightArm, -1.0f, &n_fore);
                const float sd_hand = field.query_point(out_world_positions[AETHER_BONE_RIGHT_HAND],
                                                        BodySelfCollisionField::kHandSphereRadiusM,
                                                        CollisionCapsuleKind::RightArm, -1.0f, &n_hand);
                const float worst = std::min(sd_fore, sd_hand);
                if (worst < 0.0f) {
                    const Vec3 push_n = (sd_fore < sd_hand) ? n_fore : n_hand;
                    r_target = r_target + push_n * (-worst + 0.004f);
                    r_elbow_pole = normalize_safe(r_elbow_pole + push_n * 0.45f, r_elbow_pole);
                    any_penetration = true;
                }
            }
            if (!any_penetration) {
                break;
            }
        }
    }

    const Vec3 knee_pole = normalize_safe(Vec3{forward_chest.x, -0.05f, std::max(0.5f, forward_chest.z)});
    const auto apply_leg_ik = [&](AetherHumanoidBone up_b, AetherHumanoidBone lo_b, AetherHumanoidBone ft_b,
                                  Vec3 target_ws) {
        const Vec3 root_ws = out_world_positions[up_b];
        const float l1 = metrics_.bone_lengths[lo_b];
        const float l2 = metrics_.bone_lengths[ft_b];
        const Vec3 rest_u = normalize_safe(out_world_positions[lo_b] - out_world_positions[up_b]);
        const Vec3 rest_l = normalize_safe(out_world_positions[ft_b] - out_world_positions[lo_b]);
        const TwoBoneIkResult ik = solve_two_bone_ik(root_ws, target_ws, knee_pole, l1, l2, rest_u, rest_l,
                                                     out_world_rotations[up_b], out_world_rotations[lo_b], 1.0f);
        const Quat hips_w = out_world_rotations[AETHER_BONE_HIPS];
        io_local_rots[up_b] =
            clamp_rotation_to_limits(normalize(conjugate(hips_w) * ik.upper_world_rot), joint_limit_for_bone(up_b));
        const Quat up_w = normalize(hips_w * io_local_rots[up_b]);
        io_local_rots[lo_b] =
            clamp_rotation_to_limits(normalize(conjugate(up_w) * ik.lower_world_rot), joint_limit_for_bone(lo_b));
    };
    if (body_pose.lower_body_track_weight >= 0.05f) {
        apply_leg_ik(AETHER_BONE_LEFT_UPPER_LEG, AETHER_BONE_LEFT_LOWER_LEG, AETHER_BONE_LEFT_FOOT,
                     body_pose.left_foot_target_ws);
        apply_leg_ik(AETHER_BONE_RIGHT_UPPER_LEG, AETHER_BONE_RIGHT_LOWER_LEG, AETHER_BONE_RIGHT_FOOT,
                     body_pose.right_foot_target_ws);
    }

    compute_fk();
}

RestSkeletonMetrics build_rest_metrics_from_avatar_streams(const AetherAvatarDesc& desc, const AetherJointDesc* joints,
                                                           size_t joint_count,
                                                           const std::array<float, 3>* joint_rest_world_pos,
                                                           size_t rest_pos_count, const AetherColliderDesc* colliders,
                                                           size_t collider_count, const AetherSpringJointDesc* springs,
                                                           size_t spring_count, bool has_split_brow_morphs) noexcept {
    RestSkeletonMetrics rm = build_default_humanoid_rest_metrics();
    if (joints == nullptr || joint_rest_world_pos == nullptr || joint_count == 0U || rest_pos_count != joint_count) {
        return rm;
    }

    constexpr size_t kMaxAvatarJoints = 1024U;
    const size_t n_j = std::min(joint_count, kMaxAvatarJoints);
    std::array<Quat, kMaxAvatarJoints> joint_world_rot{};
    std::array<uint8_t, kMaxAvatarJoints> rot_resolved{};
    joint_world_rot.fill(Quat::identity());

    auto resolve_joint_world_rot = [&](auto& self, size_t idx, int depth) noexcept -> Quat {
        if (idx >= n_j || depth > 128) {
            return Quat::identity();
        }
        if (rot_resolved[idx] != 0U) {
            return joint_world_rot[idx];
        }
        const auto& j = joints[idx];
        const float* br = j.bind_rotation_quat;
        const float q2 = br[0] * br[0] + br[1] * br[1] + br[2] * br[2] + br[3] * br[3];
        const Quat q_local = (q2 > 1e-6f) ? normalize(Quat{br[0], br[1], br[2], br[3]}) : Quat::identity();
        Quat q_parent = Quat::identity();
        if (j.parent_index >= 0 && static_cast<size_t>(j.parent_index) < n_j) {
            q_parent = self(self, static_cast<size_t>(j.parent_index), depth + 1);
        }
        joint_world_rot[idx] = normalize(q_parent * q_local);
        rot_resolved[idx] = 1U;
        return joint_world_rot[idx];
    };
    for (size_t i = 0; i < n_j; ++i) {
        (void)resolve_joint_world_rot(resolve_joint_world_rot, i, 0);
    }

    auto find_humanoid_ancestor = [&](uint32_t j) noexcept -> uint16_t {
        uint32_t cur = j;
        for (size_t steps = 0; steps < n_j && cur < n_j; ++steps) {
            const uint16_t candidate = joints[cur].humanoid_bone;
            if (candidate < AETHER_HUMANOID_BONE_COUNT) {
                return candidate;
            }
            const int32_t p_idx = joints[cur].parent_index;
            if (p_idx < 0 || static_cast<size_t>(p_idx) >= n_j || static_cast<uint32_t>(p_idx) == cur) {
                break;
            }
            cur = static_cast<uint32_t>(p_idx);
        }
        return AETHER_BONE_HIPS;
    };

    std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> world{};
    const uint16_t hips_j = desc.humanoid_to_joint[AETHER_BONE_HIPS];
    if (hips_j < n_j) {
        const auto& hp = joint_rest_world_pos[hips_j];
        world[AETHER_BONE_HIPS] = {hp[0], hp[1], hp[2]};
        rm.rest_world_rotations[AETHER_BONE_HIPS] = joint_world_rot[hips_j];
    } else {
        world[AETHER_BONE_HIPS] = rm.rest_local_positions[AETHER_BONE_HIPS];
        rm.rest_world_rotations[AETHER_BONE_HIPS] = Quat::identity();
    }
    rm.rest_local_rotations[AETHER_BONE_HIPS] = rm.rest_world_rotations[AETHER_BONE_HIPS];

    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        const uint16_t j = desc.humanoid_to_joint[b];
        if (j < n_j) {
            const auto& jp = joint_rest_world_pos[j];
            world[b] = {jp[0], jp[1], jp[2]};
            rm.rest_world_rotations[b] = joint_world_rot[j];
        } else {
            world[b] = world[pi] + rm.rest_local_positions[b];
            rm.rest_world_rotations[b] = rm.rest_world_rotations[pi];
        }
        rm.rest_local_rotations[b] = normalize(conjugate(rm.rest_world_rotations[pi]) * rm.rest_world_rotations[b]);
    }

    rm.rest_local_positions[AETHER_BONE_HIPS] = world[AETHER_BONE_HIPS];
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rm.rest_local_positions[b] = world[b] - world[pi];
        rm.bone_lengths[b] = std::max(length(rm.rest_local_positions[b]), 0.02f);
    }

    uint32_t custom_col_cnt = 0;
    if (colliders != nullptr) {
        for (size_t ci = 0; ci < collider_count && custom_col_cnt < kMaxRestColliders; ++ci) {
            const auto& col = colliders[ci];
            uint16_t hb = AETHER_BONE_HIPS;
            Vec3 j_pos{};
            Quat col_j_rot = Quat::identity();
            if (col.joint_index < n_j) {
                const auto& jr = joint_rest_world_pos[col.joint_index];
                j_pos = {jr[0], jr[1], jr[2]};
                col_j_rot = joint_world_rot[col.joint_index];
                hb = find_humanoid_ancestor(col.joint_index);
            }
            auto& dst = rm.custom_colliders[custom_col_cnt++];
            dst.attached_bone = static_cast<int32_t>(hb);
            dst.shape = (col.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE) ? 1U : 0U;
            dst.group_bit = col.group_bit;
            dst.radius = std::clamp(col.radius, 0.005f, 0.50f);
            const Vec3 a_ws = j_pos + rotate_vec(col_j_rot, {col.offset[0], col.offset[1], col.offset[2]});
            const Vec3 b_ws = j_pos + rotate_vec(col_j_rot, {col.tail[0], col.tail[1], col.tail[2]});
            const Quat inv_hb_rot = conjugate(rm.rest_world_rotations[hb]);
            dst.local_a = rotate_vec(inv_hb_rot, a_ws - world[hb]);
            dst.local_b = rotate_vec(inv_hb_rot, b_ws - world[hb]);
        }
    }
    rm.custom_collider_count = custom_col_cnt;

    // Populate authored VRM SpringBone joints anchored relative to each chain's humanoid base rotation.
    uint32_t custom_sp_cnt = 0;
    if (springs != nullptr && spring_count > 0U) {
        std::array<int32_t, kMaxRestSprings> src_to_dst{};
        src_to_dst.fill(-1);
        const size_t n_sp = std::min(spring_count, static_cast<size_t>(kMaxRestSprings));
        for (size_t si = 0; si < n_sp && custom_sp_cnt < kMaxRestSprings; ++si) {
            const auto& sp = springs[si];
            if (sp.joint_index >= n_j) {
                continue;
            }
            const auto& jp = joint_rest_world_pos[sp.joint_index];
            const Vec3 pos_ws{jp[0], jp[1], jp[2]};

            int32_t mapped_ps = -1;
            if (sp.parent_spring_index >= 0 && static_cast<size_t>(sp.parent_spring_index) < n_sp) {
                mapped_ps = src_to_dst[static_cast<size_t>(sp.parent_spring_index)];
            }

            uint16_t hb_base = find_humanoid_ancestor(sp.joint_index);
            if (mapped_ps >= 0 && static_cast<uint32_t>(mapped_ps) < custom_sp_cnt) {
                hb_base = static_cast<uint16_t>(rm.custom_springs[static_cast<size_t>(mapped_ps)].skin_base_humanoid);
            }
            const Quat inv_base_rot = conjugate(rm.rest_world_rotations[hb_base]);

            const uint32_t dst_idx = custom_sp_cnt++;
            src_to_dst[si] = static_cast<int32_t>(dst_idx);
            auto& dst = rm.custom_springs[dst_idx];
            dst.bone_index = static_cast<int32_t>(kCustomSpringBoneOffset + dst_idx);
            dst.skin_base_humanoid = static_cast<int32_t>(hb_base);
            dst.center_bone_index = static_cast<int32_t>(AETHER_BONE_HIPS);
            dst.rest_world_pos = pos_ws;

            const Vec3 raw_rdir = normalize_safe({sp.rest_dir[0], sp.rest_dir[1], sp.rest_dir[2]}, {0.0f, -1.0f, 0.0f});
            dst.rest_dir = normalize_safe(rotate_vec(inv_base_rot, raw_rdir), {0.0f, -1.0f, 0.0f});

            if (mapped_ps >= 0 && static_cast<uint32_t>(mapped_ps) < dst_idx) {
                const auto& p_dst = rm.custom_springs[static_cast<size_t>(mapped_ps)];
                const auto& p_sp = springs[static_cast<size_t>(sp.parent_spring_index)];
                const Vec3 p_dir =
                    normalize_safe({p_sp.rest_dir[0], p_sp.rest_dir[1], p_sp.rest_dir[2]}, {0.0f, -1.0f, 0.0f});
                const Vec3 expected_tail = p_dst.rest_world_pos + p_dir * std::max(p_dst.bone_length, 0.01f);
                if (length(pos_ws - expected_tail) <= 2e-3f) {
                    dst.parent_spring_index = mapped_ps;
                    dst.parent_bone_index = static_cast<int32_t>(kCustomSpringBoneOffset + mapped_ps);
                    dst.local_bone_offset = {};
                } else {
                    // Side fork branch off parent spring joint: anchor via parent spring bone rotation + local offset.
                    dst.parent_spring_index = -1;
                    dst.parent_bone_index = static_cast<int32_t>(kCustomSpringBoneOffset + mapped_ps);
                    dst.local_bone_offset = rotate_vec(inv_base_rot, pos_ws - p_dst.rest_world_pos);
                }
            } else {
                dst.parent_spring_index = -1;
                dst.parent_bone_index = static_cast<int32_t>(hb_base);
                dst.local_bone_offset = rotate_vec(inv_base_rot, pos_ws - world[hb_base]);
            }

            dst.bone_length = std::max(sp.bone_length, 0.01f);
            dst.hit_radius = std::max(sp.hit_radius, 0.005f);
            dst.stiffness = std::clamp(sp.stiffness, 0.01f, 4.0f);
            dst.drag_force = std::clamp(sp.drag_force, 0.0f, 1.0f);
            dst.gravity_power = std::clamp(sp.gravity_power, 0.0f, 5.0f);
            dst.gravity_dir =
                normalize_safe({sp.gravity_dir[0], sp.gravity_dir[1], sp.gravity_dir[2]}, {0.0f, -1.0f, 0.0f});
            dst.collider_group_mask = sp.collider_group_mask;
            dst.filter_colliders_by_mask = true;
        }
    }
    rm.custom_spring_count = custom_sp_cnt;
    rm.has_split_brow_morphs = has_split_brow_morphs;
    rm.valid = true;
    return rm;
}

} // namespace aether::animation
