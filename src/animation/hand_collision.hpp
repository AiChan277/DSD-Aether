#pragma once

#include "animation/math_quat.hpp"

#include <array>
#include <cstdint>

namespace aether::animation {

enum class CollisionCapsuleKind : uint8_t {
    TorsoChest = 0,
    NeckHead = 1,
    LeftArm = 2,
    RightArm = 3,
};

struct CollisionCapsule {
    Vec3 a{};
    Vec3 b{};
    float radius{0.05f};
    CollisionCapsuleKind kind{CollisionCapsuleKind::TorsoChest};
};

/**
 * Signed-distance capsule collision field covering torso/chest (`+3.5 cm` safety margin),
 * neck/head, and bilateral forearm/hand capsules. Strictly zero heap allocations.
 */
class BodySelfCollisionField {
  public:
    static constexpr float kChestSafetyMarginM = 0.035f;
    static constexpr float kHandSphereRadiusM = 0.042f;
    static constexpr float kForearmRadiusM = 0.036f;
    static constexpr size_t kMaxCapsules = 12U;

    /** Populates torso, neck/head, and optionally left/right forearm+hand capsules from FK bone world positions. */
    void build_from_fk(const std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& world_pos, bool include_left_arm = true,
                       bool include_right_arm = true) noexcept;

    /**
     * Evaluates minimum signed distance from point `p` (with sphere radius `query_radius`) to active capsules,
     * optionally filtering out self arm capsules (`ignore_kind`) and returning the unit outward push normal.
     */
    [[nodiscard]] float query_point(Vec3 p, float query_radius, CollisionCapsuleKind ignore_kind, float side_hint_x,
                                    Vec3* out_normal = nullptr, Vec3* out_closest = nullptr) const noexcept;

    /**
     * Evaluates shortest signed distance between segment `[seg_a, seg_b]` (radius `seg_radius`) and active capsules
     * excluding `ignore_kind`, returning the unit outward push normal at the worst penetration site.
     */
    [[nodiscard]] float query_segment(Vec3 seg_a, Vec3 seg_b, float seg_radius, CollisionCapsuleKind ignore_kind,
                                      float side_hint_x, Vec3* out_normal = nullptr,
                                      float* out_worst_t = nullptr) const noexcept;

    /**
     * Computes minimum signed distance (`>= 0.0f` when collision-free) for active arm(s) against torso/head and
     * opposite forearm/hand capsules.
     */
    [[nodiscard]] float min_signed_distance(const std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& world_pos,
                                            bool check_left = true, bool check_right = true) const noexcept;

    /**
     * Stage 1 Pre-IK tangential slide: pushes wrist targets smoothly onto collision-free tangent surface outside
     * torso (`+3.5 cm`), neck/head, and maintains bilateral hand separation when both arms are active.
     */
    void resolve_pre_ik_wrists(bool left_active, Vec3& io_left_wrist, bool right_active,
                               Vec3& io_right_wrist) const noexcept;

    [[nodiscard]] uint32_t capsule_count() const noexcept { return count_; }

  private:
    void add_capsule(Vec3 a, Vec3 b, float radius, CollisionCapsuleKind kind) noexcept;

    std::array<CollisionCapsule, kMaxCapsules> capsules_{};
    uint32_t count_{0};
};

} // namespace aether::animation
