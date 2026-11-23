#include "animation/hand_collision.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

[[nodiscard]] Vec3 closest_point_on_segment(Vec3 p, Vec3 a, Vec3 b) noexcept {
    const Vec3 ab = b - a;
    const float ab2 = length_sq(ab);
    if (ab2 <= 1e-10f) {
        return a;
    }
    const float t = std::clamp(dot(p - a, ab) / ab2, 0.0f, 1.0f);
    return a + ab * t;
}

[[nodiscard]] Vec3 default_outward_dir(float side_hint_x) noexcept {
    const float sx = (side_hint_x >= 0.0f) ? 1.0f : -1.0f;
    return normalize_safe({sx * 0.45f, 0.08f, 0.89f}, {0.0f, 0.0f, 1.0f});
}

[[nodiscard]] float signed_dist_point_capsule(Vec3 p, float query_radius, const CollisionCapsule& cap,
                                              float side_hint_x, Vec3& out_normal, Vec3& out_closest) noexcept {
    out_closest = closest_point_on_segment(p, cap.a, cap.b);
    const Vec3 delta = p - out_closest;
    const float dist = length(delta);
    if (dist > 1e-5f) {
        // Keep push-out biased in front of torso when very near midline behind chest
        if (cap.kind == CollisionCapsuleKind::TorsoChest && delta.z < -0.02f && std::abs(delta.x) < 0.04f) {
            out_normal = default_outward_dir(side_hint_x);
        } else {
            out_normal = delta * (1.0f / dist);
        }
    } else {
        out_normal = default_outward_dir(side_hint_x);
    }
    return dist - (cap.radius + query_radius);
}

} // namespace

void BodySelfCollisionField::add_capsule(Vec3 a, Vec3 b, float radius, CollisionCapsuleKind kind) noexcept {
    if (count_ >= kMaxCapsules) {
        return;
    }
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) || !std::isfinite(b.x) ||
        !std::isfinite(b.y) || !std::isfinite(b.z) || !std::isfinite(radius) || radius <= 0.0f) {
        return;
    }
    capsules_[count_++] = CollisionCapsule{a, b, radius, kind};
}

void BodySelfCollisionField::build_from_fk(const std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& world_pos,
                                           bool include_left_arm, bool include_right_arm) noexcept {
    count_ = 0U;
    const Vec3 hips = world_pos[AETHER_BONE_HIPS];
    const Vec3 spine = world_pos[AETHER_BONE_SPINE];
    const Vec3 chest = world_pos[AETHER_BONE_CHEST];
    const Vec3 upper_chest = world_pos[AETHER_BONE_UPPER_CHEST];
    const Vec3 neck = world_pos[AETHER_BONE_NECK];
    const Vec3 head = world_pos[AETHER_BONE_HEAD];

    // Estimate shoulder half-width cleanly so chest width fits petite and broad VRMs without clipping rest A-pose
    const float shoulder_span = length(world_pos[AETHER_BONE_LEFT_UPPER_ARM] - world_pos[AETHER_BONE_RIGHT_UPPER_ARM]);
    const float half_sh = (shoulder_span > 0.08f && shoulder_span < 1.2f) ? (0.5f * shoulder_span) : 0.16f;
    const float torso_core_r = std::clamp(0.45f * half_sh, 0.055f, 0.105f);
    const float chest_r = torso_core_r + kChestSafetyMarginM;

    // 1. Torso / Chest / Abdomen capsules with explicit 3.5 cm chest clearance margin
    add_capsule(hips, spine, torso_core_r * 0.95f + kChestSafetyMarginM, CollisionCapsuleKind::TorsoChest);
    add_capsule(spine, chest, chest_r, CollisionCapsuleKind::TorsoChest);
    add_capsule(chest, upper_chest, chest_r, CollisionCapsuleKind::TorsoChest);

    // 2. Neck & Head skull capsules
    const Vec3 head_up = normalize_safe(head - neck, {0.0f, 1.0f, 0.0f});
    const float neck_len = std::max(0.05f, length(head - neck));
    add_capsule(upper_chest, neck + head_up * (0.25f * neck_len), std::clamp(0.32f * half_sh, 0.042f, 0.065f),
                CollisionCapsuleKind::NeckHead);
    add_capsule(head + head_up * 0.02f, head + head_up * 0.11f, std::clamp(0.58f * half_sh, 0.085f, 0.115f),
                CollisionCapsuleKind::NeckHead);

    // 3. Bilateral forearm + hand capsules for two-hand collision avoidance
    if (include_left_arm) {
        const Vec3 l_elbow = world_pos[AETHER_BONE_LEFT_LOWER_ARM];
        const Vec3 l_wrist = world_pos[AETHER_BONE_LEFT_HAND];
        const Vec3 l_dir = normalize_safe(l_wrist - l_elbow, {1.0f, 0.0f, 0.0f});
        add_capsule(l_elbow + l_dir * 0.02f, l_wrist + l_dir * 0.05f, kForearmRadiusM, CollisionCapsuleKind::LeftArm);
    }
    if (include_right_arm) {
        const Vec3 r_elbow = world_pos[AETHER_BONE_RIGHT_LOWER_ARM];
        const Vec3 r_wrist = world_pos[AETHER_BONE_RIGHT_HAND];
        const Vec3 r_dir = normalize_safe(r_wrist - r_elbow, {-1.0f, 0.0f, 0.0f});
        add_capsule(r_elbow + r_dir * 0.02f, r_wrist + r_dir * 0.05f, kForearmRadiusM, CollisionCapsuleKind::RightArm);
    }
}

float BodySelfCollisionField::query_point(Vec3 p, float query_radius, CollisionCapsuleKind ignore_kind,
                                          float side_hint_x, Vec3* out_normal, Vec3* out_closest) const noexcept {
    float best_sd = 1e6f;
    Vec3 best_n = default_outward_dir(side_hint_x);
    Vec3 best_c = p;
    for (uint32_t i = 0U; i < count_; ++i) {
        const auto& cap = capsules_[i];
        if (cap.kind == ignore_kind) {
            continue;
        }
        Vec3 n{};
        Vec3 c{};
        const float sd = signed_dist_point_capsule(p, query_radius, cap, side_hint_x, n, c);
        if (sd < best_sd) {
            best_sd = sd;
            best_n = n;
            best_c = c;
        }
    }
    if (out_normal != nullptr) {
        *out_normal = best_n;
    }
    if (out_closest != nullptr) {
        *out_closest = best_c;
    }
    return best_sd;
}

float BodySelfCollisionField::query_segment(Vec3 seg_a, Vec3 seg_b, float seg_radius, CollisionCapsuleKind ignore_kind,
                                            float side_hint_x, Vec3* out_normal, float* out_worst_t) const noexcept {
    float best_sd = 1e6f;
    Vec3 best_n = default_outward_dir(side_hint_x);
    float best_t = 1.0f;
    constexpr int kSamples = 5;
    for (int s = 1; s <= kSamples; ++s) {
        // Sample along forearm from just past elbow (0.2) to extended wrist tip (1.0)
        const float t = static_cast<float>(s) / static_cast<float>(kSamples);
        const Vec3 pt = lerp(seg_a, seg_b, t);
        Vec3 n{};
        const float sd = query_point(pt, seg_radius, ignore_kind, side_hint_x, &n, nullptr);
        if (sd < best_sd) {
            best_sd = sd;
            best_n = n;
            best_t = t;
        }
    }
    if (out_normal != nullptr) {
        *out_normal = best_n;
    }
    if (out_worst_t != nullptr) {
        *out_worst_t = best_t;
    }
    return best_sd;
}

float BodySelfCollisionField::min_signed_distance(const std::array<Vec3, AETHER_HUMANOID_BONE_COUNT>& world_pos,
                                                  bool check_left, bool check_right) const noexcept {
    float best_sd = 1e6f;
    if (check_left) {
        const Vec3 l_elbow = world_pos[AETHER_BONE_LEFT_LOWER_ARM];
        const Vec3 l_wrist = world_pos[AETHER_BONE_LEFT_HAND];
        const float sd_fore = query_segment(l_elbow, l_wrist, kForearmRadiusM, CollisionCapsuleKind::LeftArm, 1.0f);
        const float sd_hand = query_point(l_wrist, kHandSphereRadiusM, CollisionCapsuleKind::LeftArm, 1.0f);
        best_sd = std::min({best_sd, sd_fore, sd_hand});
    }
    if (check_right) {
        const Vec3 r_elbow = world_pos[AETHER_BONE_RIGHT_LOWER_ARM];
        const Vec3 r_wrist = world_pos[AETHER_BONE_RIGHT_HAND];
        const float sd_fore = query_segment(r_elbow, r_wrist, kForearmRadiusM, CollisionCapsuleKind::RightArm, -1.0f);
        const float sd_hand = query_point(r_wrist, kHandSphereRadiusM, CollisionCapsuleKind::RightArm, -1.0f);
        best_sd = std::min({best_sd, sd_fore, sd_hand});
    }
    return best_sd;
}

void BodySelfCollisionField::resolve_pre_ik_wrists(bool left_active, Vec3& io_left_wrist, bool right_active,
                                                   Vec3& io_right_wrist) const noexcept {
    constexpr float kClearanceEps = 0.004f;
    auto push_out = [&](Vec3& w, CollisionCapsuleKind self_kind, float side_hint) {
        for (int it = 0; it < 3; ++it) {
            Vec3 n{};
            const float sd = query_point(w, kHandSphereRadiusM, self_kind, side_hint, &n, nullptr);
            if (sd >= kClearanceEps) {
                break;
            }
            w = w + n * (kClearanceEps - sd);
        }
    };

    if (left_active) {
        push_out(io_left_wrist, CollisionCapsuleKind::LeftArm, 1.0f);
    }
    if (right_active) {
        push_out(io_right_wrist, CollisionCapsuleKind::RightArm, -1.0f);
    }

    // Bilateral wrist-to-wrist clearance when both arms are tracked simultaneously
    if (left_active && right_active) {
        const Vec3 d = io_left_wrist - io_right_wrist;
        const float min_sep = 2.0f * kHandSphereRadiusM + 0.012f;
        const float dist = length(d);
        if (dist < min_sep) {
            const Vec3 axis = (dist > 1e-5f) ? (d * (1.0f / dist)) : Vec3{1.0f, 0.0f, 0.0f};
            const float push = 0.5f * (min_sep - dist);
            io_left_wrist = io_left_wrist + axis * push;
            io_right_wrist = io_right_wrist - axis * push;
            push_out(io_left_wrist, CollisionCapsuleKind::LeftArm, 1.0f);
            push_out(io_right_wrist, CollisionCapsuleKind::RightArm, -1.0f);
        }
    }
}

} // namespace aether::animation
