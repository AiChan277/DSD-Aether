#pragma once

#include "animation/ik_solvers.hpp"
#include "animation/skinning_frame.hpp"

#include <array>
#include <cstdint>

namespace aether::animation {

inline constexpr uint32_t kMaxSpringJoints = 512;
inline constexpr uint32_t kMaxSpringColliders = 64;

enum class ColliderShape : uint8_t {
    Sphere = 0,
    Capsule = 1,
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct alignas(32) SpringColliderSoA {
    uint32_t count{0};
    alignas(32) uint8_t shape[kMaxSpringColliders]{};
    alignas(32) int32_t attached_bone[kMaxSpringColliders]{};
    alignas(32) float local_ax[kMaxSpringColliders]{};
    alignas(32) float local_ay[kMaxSpringColliders]{};
    alignas(32) float local_az[kMaxSpringColliders]{};
    alignas(32) float local_bx[kMaxSpringColliders]{};
    alignas(32) float local_by[kMaxSpringColliders]{};
    alignas(32) float local_bz[kMaxSpringColliders]{};
    alignas(32) float radius[kMaxSpringColliders]{};
    // Per-tick evaluated object/center-space endpoints:
    alignas(32) float center_ax[kMaxSpringColliders]{};
    alignas(32) float center_ay[kMaxSpringColliders]{};
    alignas(32) float center_az[kMaxSpringColliders]{};
    alignas(32) float center_bx[kMaxSpringColliders]{};
    alignas(32) float center_by[kMaxSpringColliders]{};
    alignas(32) float center_bz[kMaxSpringColliders]{};
    uint32_t group_bit[kMaxSpringColliders]{};
};

struct SpringJointDesc {
    int32_t bone_index{-1};
    int32_t parent_joint_index{-1}; // -1 for root of chain
    int32_t parent_bone_index{-1};  // Avatar parent bone driving anchor
    int32_t center_bone_index{-1};  // VRM springCenter bone (-1 = root space)
    Vec3 local_bone_offset{0.0f, -0.08f, 0.0f};
    Vec3 rest_dir{0.0f, 0.0f, 0.0f}; // Optional explicit rest direction (defaults to normalize(local_bone_offset))
    float bone_length{0.08f};
    float hit_radius{0.02f};
    float stiffness{0.45f};
    float drag_force{0.40f};
    float gravity_power{0.08f};
    Vec3 gravity_dir{0.0f, -1.0f, 0.0f};
    uint32_t collider_mask{0};
    bool filter_colliders_by_mask{false};
};

/** Zero-alloc collider snapshot for T3 Rigging wireframe overlay (`#06B6D4` rest / `#F97316` contact). */
inline constexpr uint32_t kMaxOverlayColliders = 32;
inline constexpr uint32_t kMaxOverlaySpringSegments = kMaxSpringJoints;

struct SpringColliderOverlay {
    float p0[3]{};
    float radius{0.0f};
    float p1[3]{};
    uint8_t shape{0};      // 0 = sphere, 1 = capsule
    uint8_t in_contact{0}; // 1 when active collision contact occurred during the last substep
    uint8_t pad[2]{};
};

/** Zero-alloc SpringBone chain segment snapshot (head -> tail in avatar model space). */
struct SpringSegmentOverlay {
    float head[3]{};
    float hit_radius{0.015f};
    float tail[3]{};
    uint8_t in_contact{0};
    uint8_t pad[3]{};
};

/**
 * Cache-lined SoA SpringBone Verlet simulator (`§9.6`, 60/120 Hz fixed sub-step).
 * Dispatches AVX-512 (16-wide), AVX2+FMA (8-wide), or exact scalar fallback (`<= 1e-5 m` parity).
 */
class alignas(64) SpringBoneSimulator {
  public:
    enum class SimdPath : uint8_t {
        Scalar = 0,
        Avx2 = 1,
        Avx512 = 2,
    };

    SpringBoneSimulator() noexcept;

    void clear() noexcept;
    bool add_joint(const SpringJointDesc& desc) noexcept;
    bool add_sphere_collider(int32_t attached_bone, Vec3 offset, float radius, uint32_t group_bit = 0) noexcept;
    bool add_capsule_collider(int32_t attached_bone, Vec3 head_offset, Vec3 tail_offset, float radius,
                              uint32_t group_bit = 0) noexcept;
    void configure_default_skirt_and_hair(const RestSkeletonMetrics& rest) noexcept;

    void reset_state(const std::array<Vec3, kMaxBones>& bone_pos_ws, const std::array<Quat, kMaxBones>& bone_rot_ws,
                     uint32_t bone_count) noexcept;

    void step(float fixed_dt_s, Vec3 external_accel_ws, float external_drag_scale,
              std::array<Vec3, kMaxBones>& io_bone_pos_ws, std::array<Quat, kMaxBones>& io_bone_rot_ws,
              uint32_t bone_count, SimdPath force_path = SimdPath::Avx2) noexcept;

    [[nodiscard]] uint32_t joint_count() const noexcept { return count_; }
    [[nodiscard]] uint32_t collider_count() const noexcept { return colliders_.count; }
    [[nodiscard]] bool collider_in_contact(uint32_t idx) const noexcept {
        return idx < colliders_.count && active_contact_[idx] != 0;
    }
    [[nodiscard]] SimdPath active_simd_path() const noexcept { return detected_path_; }
    [[nodiscard]] uint32_t teleport_reset_count() const noexcept { return teleport_resets_; }

    /** Exports world/avatar-space collider and physics chain snapshots without heap allocation (`HR-01`). */
    void export_overlay(SpringColliderOverlay* out_cols, uint32_t max_cols, uint32_t& out_col_count,
                        SpringSegmentOverlay* out_segs, uint32_t max_segs, uint32_t& out_seg_count,
                        const std::array<Vec3, kMaxBones>& bone_pos_ws) const noexcept;

    // Direct accessors for unit-testing tail position parity:
    [[nodiscard]] Vec3 joint_tail_pos(uint32_t idx) const noexcept {
        return {curr_x_[idx], curr_y_[idx], curr_z_[idx]};
    }

  private:
    void evaluate_colliders(const std::array<Vec3, kMaxBones>& bone_pos_ws,
                            const std::array<Quat, kMaxBones>& bone_rot_ws) noexcept;
    void step_scalar(float dt_s, float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                     std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                     std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept;
    void step_avx2(float dt_s, float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                   std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                   std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept;
    void step_avx512(float dt_s, float dt_sq, Vec3 ext_accel, float ext_drag_scale,
                     std::array<Vec3, kMaxBones>& io_bone_pos_ws,
                     std::array<Quat, kMaxBones>& io_bone_rot_ws) noexcept;

    uint32_t count_{0};
    uint32_t teleport_resets_{0};
    bool state_initialized_{false};
    Vec3 prev_root_pos_{};
    Vec3 last_center_pos_{};
    SimdPath detected_path_{SimdPath::Scalar};
    std::array<uint8_t, kMaxSpringColliders> active_contact_{};
    std::array<uint8_t, kMaxSpringJoints> joint_contact_{};

    // SoA joint arrays aligned for 512-bit SIMD loads:
    alignas(64) float curr_x_[kMaxSpringJoints]{};
    alignas(64) float curr_y_[kMaxSpringJoints]{};
    alignas(64) float curr_z_[kMaxSpringJoints]{};
    alignas(64) float prev_x_[kMaxSpringJoints]{};
    alignas(64) float prev_y_[kMaxSpringJoints]{};
    alignas(64) float prev_z_[kMaxSpringJoints]{};
    alignas(64) float head_x_[kMaxSpringJoints]{};
    alignas(64) float head_y_[kMaxSpringJoints]{};
    alignas(64) float head_z_[kMaxSpringJoints]{};
    alignas(64) float rest_dir_x_[kMaxSpringJoints]{};
    alignas(64) float rest_dir_y_[kMaxSpringJoints]{};
    alignas(64) float rest_dir_z_[kMaxSpringJoints]{};
    alignas(64) float grav_x_[kMaxSpringJoints]{};
    alignas(64) float grav_y_[kMaxSpringJoints]{};
    alignas(64) float grav_z_[kMaxSpringJoints]{};
    alignas(64) float stiffness_[kMaxSpringJoints]{};
    alignas(64) float drag_[kMaxSpringJoints]{};
    alignas(64) float bone_len_[kMaxSpringJoints]{};
    alignas(64) float hit_radius_[kMaxSpringJoints]{};
    alignas(64) uint32_t collider_mask_[kMaxSpringJoints]{};
    alignas(64) uint8_t filter_by_mask_[kMaxSpringJoints]{};

    alignas(64) int32_t bone_idx_[kMaxSpringJoints]{};
    alignas(64) int32_t parent_joint_idx_[kMaxSpringJoints]{};
    alignas(64) int32_t parent_bone_idx_[kMaxSpringJoints]{};
    alignas(64) int32_t center_bone_idx_[kMaxSpringJoints]{};
    alignas(64) float local_offset_x_[kMaxSpringJoints]{};
    alignas(64) float local_offset_y_[kMaxSpringJoints]{};
    alignas(64) float local_offset_z_[kMaxSpringJoints]{};

    SpringColliderSoA colliders_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aether::animation
