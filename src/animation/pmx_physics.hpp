#pragma once

#include <aether_ffi_contract.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace aether::animation {

struct PmxVec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct PmxQuat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
};

struct PmxRigidBodyRuntime {
    PmxVec3 pos{};
    PmxQuat rot{};
    PmxVec3 prev_pos{};
    PmxQuat prev_rot{};
    PmxVec3 lin_vel{};
    PmxVec3 ang_vel{};
    PmxVec3 bone_local_offset{};
    PmxQuat bone_local_rot{};
    float inv_mass{0.0f};
    float inv_inertia{0.0f};
};

struct PmxJointClearancePair {
    uint32_t body_a{0};
    uint32_t body_b{0};
    float rest_clear_dist{0.0f};
};

/// Zero-allocation (after `init()`) runtime evaluator for:
///   1. PMX Bone Morph local offsets (`translation` + `rotation`)
///   2. PMX CCD-IK (`ik_target`, `ik_loops`, `ik_limit_rad`, per-link Euler limits)
///   3. PMX Append Transform (`AppendRotate` / `AppendMove` / `AppendLocal` with `append_ratio`)
///   4. Fixed-timestep 60 Hz Damped RigidBody + 6-DOF Spring Joint physics
///      with rest-pose penetration clearance, sphere/box/capsule depenetration,
///      exponential damping, and velocity clamping (`max_lin_vel = 6.0 m/s`,
///      `max_ang_vel = 15.0 rad/s`).
class PmxSkeletonAndPhysicsSolver {
public:
    static constexpr float kFixedDt = 1.0f / 60.0f;
    static constexpr float kGravityY = -9.8f;
    static constexpr float kMaxLinearVel = 6.0f;
    static constexpr float kMaxAngularVel = 15.0f;
    static constexpr uint32_t kMaxSubsteps = 4u;

    PmxSkeletonAndPhysicsSolver() = default;

    void init(std::span<const AetherJointDesc> bones,
              std::span<const AetherPmxBoneExt> bone_exts,
              std::span<const AetherPmxIkLink> ik_links,
              std::span<const AetherPmxRigidBodyDesc> rigid_bodies,
              std::span<const AetherPmxConstraint6DofDesc> constraints_6dof) noexcept;

    void reset_to_rest_pose(std::span<const AetherJointDesc> bones,
                            std::span<const float> rest_world_mats16) noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return bone_exts_.empty() && rigid_bodies_.empty();
    }

    /// Evaluates CCD-IK, Append Transform, and fixed-timestep 60 Hz RigidBody +
    /// 6-DOF Spring Joint simulation in-place on `local_t` (N*3), `local_r` (N*4),
    /// and `world_mats16` (N*16 column-major 4x4 matrices).
    /// Zero heap allocations occur inside `step()`.
    void step(float dt_sec,
              std::span<const AetherJointDesc> bones,
              std::span<float> local_t,
              std::span<float> local_r,
              std::span<const float> local_s,
              std::span<float> world_mats16) noexcept;

    /// Evaluates spherical deform (`SDEF`) vertex skinning on CPU for verification
    /// and fallback when needed, returning deformed position and normal.
    static void evaluate_sdef_vertex(const float in_pos[3],
                                     const float in_nrm[3],
                                     const uint16_t joints[4],
                                     const float weights[4],
                                     const AetherPmxSdefVertex& sdef,
                                     std::span<const float> skin_mats16,
                                     std::span<const float> world_mats16,
                                     std::span<const float> inv_bind_mats16,
                                     float out_pos[3],
                                     float out_nrm[3]) noexcept;

    [[nodiscard]] std::span<const PmxRigidBodyRuntime> rigid_body_states() const noexcept {
        return rb_states_;
    }

private:
    void recompute_world_matrices(std::span<const AetherJointDesc> bones,
                                  std::span<const float> local_t,
                                  std::span<const float> local_r,
                                  std::span<const float> local_s,
                                  std::span<float> world_mats16) const noexcept;

    void solve_ccd_ik(std::span<const AetherJointDesc> bones,
                      std::span<const float> local_t,
                      std::span<float> local_r,
                      std::span<const float> local_s,
                      std::span<float> world_mats16) const noexcept;

    void solve_append_transforms(std::span<const AetherJointDesc> bones,
                                 std::span<float> local_t,
                                 std::span<float> local_r,
                                 std::span<const float> local_s,
                                 std::span<float> world_mats16) noexcept;

    void step_physics_fixed(float fixed_dt,
                            std::span<const AetherJointDesc> bones,
                            std::span<const float> world_mats16) noexcept;

    void write_dynamic_bodies_to_bones(std::span<const AetherJointDesc> bones,
                                       std::span<float> local_t,
                                       std::span<float> local_r,
                                       std::span<const float> local_s,
                                       std::span<float> world_mats16) const noexcept;

    std::vector<AetherPmxBoneExt> bone_exts_;
    std::vector<AetherPmxIkLink> ik_links_;
    std::vector<AetherPmxRigidBodyDesc> rigid_bodies_;
    std::vector<AetherPmxConstraint6DofDesc> constraints_6dof_;
    std::vector<PmxRigidBodyRuntime> rb_states_;
    std::vector<PmxJointClearancePair> rest_clearances_;
    std::vector<PmxVec3> base_local_t_;
    std::vector<PmxQuat> base_local_r_;
    float accumulator_sec_{0.0f};
    bool initialized_from_pose_{false};
};

}  // namespace aether::animation
