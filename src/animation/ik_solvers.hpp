#pragma once

#include "animation/visibility_first.hpp"

#include <array>
#include <cstdint>

namespace aether::animation {

struct JointLimitEulerRad {
    float min_x{-3.14159f}, max_x{3.14159f};
    float min_y{-3.14159f}, max_y{3.14159f};
    float min_z{-3.14159f}, max_z{3.14159f};
};

struct RestColliderDef {
    int32_t attached_bone{AETHER_BONE_HIPS};
    uint8_t shape{0}; // 0 = sphere, 1 = capsule
    uint8_t pad[3]{};
    Vec3 local_a{};
    Vec3 local_b{};
    float radius{0.05f};
};

struct RestSkeletonMetrics {
    std::array<int32_t, AETHER_HUMANOID_BONE_COUNT> parent_indices{};
    std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> rest_local_positions{};
    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> rest_local_rotations{};
    std::array<float, AETHER_HUMANOID_BONE_COUNT> bone_lengths{};
    std::array<RestColliderDef, 32> custom_colliders{};
    uint32_t custom_collider_count{0};
    bool valid{false};
};

/** Builds canonical standard-T-pose humanoid rest metrics when only partial skeleton metadata exists. */
[[nodiscard]] RestSkeletonMetrics build_default_humanoid_rest_metrics() noexcept;

/** Returns `AETHER-TS-001` Appendix C anatomical joint angle bounds (radians) for the humanoid bone. */
[[nodiscard]] JointLimitEulerRad joint_limit_for_bone(AetherHumanoidBone bone) noexcept;

/** Clamps a bone-local quaternion rotation cleanly inside anatomical XYZ Euler limits. */
[[nodiscard]] Quat clamp_rotation_to_limits(Quat q, const JointLimitEulerRad& lim) noexcept;

/**
 * Analytic 2-bone IK solver (`§9.5`) with pole/swivel hint and soft full-extension clamping
 * (99.8% reach limit so elbows/knees never hyper-extend or snap).
 */
struct TwoBoneIkResult {
    Quat upper_world_rot{Quat::identity()};
    Quat lower_world_rot{Quat::identity()};
    Vec3 mid_joint_ws{};
    Vec3 end_effector_ws{};
};

[[nodiscard]] TwoBoneIkResult solve_two_bone_ik(Vec3 root_ws, Vec3 target_ws, Vec3 pole_dir_ws, float len_upper,
                                                float len_lower, Vec3 upper_rest_dir, Vec3 lower_rest_dir,
                                                Quat upper_rest_world, Quat lower_rest_world,
                                                float weight = 1.0f) noexcept;

/**
 * Evaluates FABRIK for head-to-chest (4 joints, `<= 4` iterations) and analytic 2-bone arm/leg IK
 * with Appendix C anatomical joint limits, producing clean object-space transforms + local quats.
 */
class FullBodyIkSolver {
  public:
    void set_rest_metrics(const RestSkeletonMetrics& metrics) noexcept;
    [[nodiscard]] const RestSkeletonMetrics& rest_metrics() const noexcept { return metrics_; }

    void solve(const SolvedBodyPose& body_pose, const AetherGraphOutputs& graph_out,
               std::array<Quat, AETHER_HUMANOID_BONE_COUNT>& io_local_rots,
               std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& out_world_positions,
               std::array<Quat, AETHER_HUMANOID_BONE_COUNT>& out_world_rotations) const noexcept;

  private:
    RestSkeletonMetrics metrics_{build_default_humanoid_rest_metrics()};
};

} // namespace aether::animation
