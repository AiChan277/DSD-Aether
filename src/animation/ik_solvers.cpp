#include "animation/ik_solvers.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

RestSkeletonMetrics build_default_humanoid_rest_metrics() noexcept {
    RestSkeletonMetrics m{};
    m.parent_indices.fill(-1);
    m.rest_local_rotations.fill(Quat::identity());
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
                io_local_rots[b] = clamp_rotation_to_limits(io_local_rots[b],
                                                            joint_limit_for_bone(static_cast<AetherHumanoidBone>(b)));
            }
            out_world_rotations[b] = normalize(out_world_rotations[pi] * io_local_rots[b]);
            out_world_positions[b] =
                out_world_positions[pi] + rotate_vec(out_world_rotations[pi], metrics_.rest_local_positions[b]);
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
        // Map FABRIK chain directions back to Spine/Chest/Neck local rotations
        for (size_t i = 0; i < 3; ++i) {
            const size_t b = kChain[i];
            const size_t child = kChain[i + 1];
            const int32_t pi = metrics_.parent_indices[b];
            const Quat p_world = (pi >= 0) ? out_world_rotations[static_cast<size_t>(pi)] : Quat::identity();
            const Vec3 cur_dir = normalize_safe(out_world_positions[child] - out_world_positions[b]);
            const Vec3 new_dir = normalize_safe(pts[i + 1] - pts[i]);
            const Quat new_world = normalize(quat_from_to(cur_dir, new_dir) * out_world_rotations[b]);
            io_local_rots[b] = clamp_rotation_to_limits(normalize(conjugate(p_world) * new_world),
                                                        joint_limit_for_bone(static_cast<AetherHumanoidBone>(b)));
        }
        compute_fk();
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
    const Vec3 l_elbow_pole = rotate_vec(out_world_rotations[AETHER_BONE_UPPER_CHEST], {0.35f, -0.45f, -0.82f});
    const Vec3 r_elbow_pole = rotate_vec(out_world_rotations[AETHER_BONE_UPPER_CHEST], {-0.35f, -0.45f, -0.82f});
    apply_arm_ik(AETHER_BONE_LEFT_SHOULDER, AETHER_BONE_LEFT_UPPER_ARM, AETHER_BONE_LEFT_LOWER_ARM,
                 AETHER_BONE_LEFT_HAND, body_pose.left_wrist_target_ws, l_elbow_pole,
                 body_pose.left_wrist_track_weight);
    apply_arm_ik(AETHER_BONE_RIGHT_SHOULDER, AETHER_BONE_RIGHT_UPPER_ARM, AETHER_BONE_RIGHT_LOWER_ARM,
                 AETHER_BONE_RIGHT_HAND, body_pose.right_wrist_target_ws, r_elbow_pole,
                 body_pose.right_wrist_track_weight);

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
    apply_leg_ik(AETHER_BONE_LEFT_UPPER_LEG, AETHER_BONE_LEFT_LOWER_LEG, AETHER_BONE_LEFT_FOOT,
                 body_pose.left_foot_target_ws);
    apply_leg_ik(AETHER_BONE_RIGHT_UPPER_LEG, AETHER_BONE_RIGHT_LOWER_LEG, AETHER_BONE_RIGHT_FOOT,
                 body_pose.right_foot_target_ws);

    compute_fk();
}

} // namespace aether::animation
