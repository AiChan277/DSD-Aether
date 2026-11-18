#include "tracking/npu_postprocess.hpp"

#include "animation/math_quat.hpp"

#include <algorithm>
#include <cmath>

namespace aether::tracking {

namespace {

constexpr float kDegToRad = 3.1415926535f / 180.0f;
constexpr float kRadToDeg = 180.0f / 3.1415926535f;

[[nodiscard]] bool finite3(const float v[3]) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

[[nodiscard]] std::array<float, 4> quat_normalize_safe(const float q[4]) noexcept {
    if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2]) || !std::isfinite(q[3])) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n2 < 1e-10f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / std::sqrt(n2);
    return {q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv};
}

// Converts unit quaternion (x, y, z, w) -> Euler (pitch_x, yaw_y, roll_z) in degrees.
[[nodiscard]] std::array<float, 3> quat_to_euler_deg(const std::array<float, 4>& q) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float sinr_cosp = 2.0f * (w * x + y * z);
    const float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
    const float pitch = std::atan2(sinr_cosp, cosr_cosp);

    const float sinp = std::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
    const float yaw = std::asin(sinp);

    const float siny_cosp = 2.0f * (w * z + x * y);
    const float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
    const float roll = std::atan2(siny_cosp, cosy_cosp);

    return {pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg};
}

[[nodiscard]] std::array<float, 4> euler_deg_to_quat(const std::array<float, 3>& deg) noexcept {
    const float hx = 0.5f * deg[0] * kDegToRad;
    const float hy = 0.5f * deg[1] * kDegToRad;
    const float hz = 0.5f * deg[2] * kDegToRad;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);
    const float q[4] = {
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    };
    return quat_normalize_safe(q);
}

[[nodiscard]] float dist_point_to_segment(const std::array<float, 3>& p, const std::array<float, 3>& a,
                                          const std::array<float, 3>& b, std::array<float, 3>& out_closest) noexcept {
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float ab2 = abx * abx + aby * aby + abz * abz;
    float t = 0.0f;
    if (ab2 > 1e-9f) {
        t = ((p[0] - a[0]) * abx + (p[1] - a[1]) * aby + (p[2] - a[2]) * abz) / ab2;
        t = std::clamp(t, 0.0f, 1.0f);
    }
    out_closest = {a[0] + abx * t, a[1] + aby * t, a[2] + abz * t};
    const float dx = p[0] - out_closest[0], dy = p[1] - out_closest[1], dz = p[2] - out_closest[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void project_out_of_capsule(std::array<float, 3>& p, const std::array<float, 3>& a, const std::array<float, 3>& b,
                            float radius, bool prefer_forward_z, NpuPostProcessDiagnostics& diag) noexcept {
    std::array<float, 3> c{};
    const float d = dist_point_to_segment(p, a, b, c);
    if (d < radius) {
        const float pen = radius - d;
        diag.collision_projected = true;
        diag.max_penetration_m = std::max(diag.max_penetration_m, pen);

        float dx = p[0] - c[0];
        float dy = p[1] - c[1];
        float dz = p[2] - c[2];
        const float len2 = dx * dx + dy * dy + dz * dz;
        if (len2 > 1e-8f) {
            const float inv = 1.0f / std::sqrt(len2);
            dx *= inv;
            dy *= inv;
            dz *= inv;
        } else {
            dx = 0.0f;
            dy = 0.0f;
            dz = prefer_forward_z ? 1.0f : -1.0f;
        }
        p[0] = c[0] + dx * radius;
        p[1] = c[1] + dy * radius;
        p[2] = c[2] + dz * radius;
    }
}

} // namespace

void NpuPostProcessor::reset() noexcept {
    last_safe_head_euler_ = {};
    has_prev_ = false;
}

std::array<float, 3> NpuPostProcessor::compress_translation_tanh(const std::array<float, 3>& p_rel,
                                                                 const std::array<float, 3>& bounds_min,
                                                                 const std::array<float, 3>& bounds_max,
                                                                 float elasticity) noexcept {
    const float knee = std::clamp(elasticity, 0.05f, 0.95f);
    const float lin = 1.0f - knee;
    std::array<float, 3> out{};
    for (size_t i = 0; i < 3u; ++i) {
        const float bmin = std::min(bounds_min[i], bounds_max[i]);
        const float bmax = std::max(bounds_min[i], bounds_max[i]);
        const float center = 0.5f * (bmin + bmax);
        const float half = std::max(0.01f, 0.5f * (bmax - bmin));
        const float val = std::isfinite(p_rel[i]) ? p_rel[i] : center;
        const float u = (val - center) / half;
        const float abs_u = std::abs(u);
        float mapped_u = u;
        if (abs_u > lin) {
            const float s = (u >= 0.0f) ? 1.0f : -1.0f;
            // C^1 smooth knee transition into tanh saturation strictly bounded inside [-1, +1]
            mapped_u = s * (lin + knee * std::tanh((abs_u - lin) / knee));
        }
        out[i] = std::clamp(center + half * mapped_u, bmin, bmax);
    }
    return out;
}

NpuPostProcessDiagnostics NpuPostProcessor::process(AetherTrackingFrame& frame,
                                                    const NpuMovementBoundsConfig& cfg) noexcept {
    NpuPostProcessDiagnostics diag{};
    const float margin = std::clamp(cfg.anti_clip_margin_m, 0.0f, 0.12f);

    // 1. Canonical humanoid rest anchors for NPU relative translation clamping
    const std::array<float, 3> head_anchor{0.0f, 1.50f, 0.0f};
    const std::array<float, 3> hips_anchor{0.0f, 0.92f, 0.0f};

    auto clamp_joint_translation = [&](uint32_t bone_idx, const std::array<float, 3>& anchor) {
        if (!finite3(frame.joint_positions_ws[bone_idx])) {
            frame.joint_positions_ws[bone_idx][0] = anchor[0];
            frame.joint_positions_ws[bone_idx][1] = anchor[1];
            frame.joint_positions_ws[bone_idx][2] = anchor[2];
        }
        const std::array<float, 3> rel{
            frame.joint_positions_ws[bone_idx][0] - anchor[0],
            frame.joint_positions_ws[bone_idx][1] - anchor[1],
            frame.joint_positions_ws[bone_idx][2] - anchor[2],
        };
        if (cfg.enable_bounds) {
            for (size_t i = 0; i < 3u; ++i) {
                const float half = std::max(0.01f, 0.5f * std::abs(cfg.bounds_max[i] - cfg.bounds_min[i]));
                const float center = 0.5f * (cfg.bounds_min[i] + cfg.bounds_max[i]);
                const float sat = std::abs(rel[i] - center) / half;
                diag.bound_saturation = std::max(diag.bound_saturation, sat);
            }
            const auto compressed =
                compress_translation_tanh(rel, cfg.bounds_min, cfg.bounds_max, cfg.soft_wall_elasticity);
            if (std::abs(compressed[0] - rel[0]) > 1e-5f || std::abs(compressed[1] - rel[1]) > 1e-5f ||
                std::abs(compressed[2] - rel[2]) > 1e-5f) {
                diag.translation_compressed = true;
            }
            frame.joint_positions_ws[bone_idx][0] = anchor[0] + compressed[0];
            frame.joint_positions_ws[bone_idx][1] = anchor[1] + compressed[1];
            frame.joint_positions_ws[bone_idx][2] = anchor[2] + compressed[2];
        }
    };

    if (frame.joint_positions_ws[AETHER_BONE_HEAD][3] > 0.0f || (frame.valid_flags & 0x01u) != 0u) {
        clamp_joint_translation(AETHER_BONE_HEAD, head_anchor);
    }
    if (frame.joint_positions_ws[AETHER_BONE_HIPS][3] > 0.0f || (frame.valid_flags & 0x08u) != 0u) {
        clamp_joint_translation(AETHER_BONE_HIPS, hips_anchor);
    }

    // 2. Extreme-pitch / yaw / roll orientation clamping on head & neck before reaching SimEngine dampers
    if (cfg.enable_anti_clip &&
        (frame.joint_positions_ws[AETHER_BONE_HEAD][3] > 0.0f || (frame.valid_flags & 0x01u) != 0u)) {
        const auto q = quat_normalize_safe(frame.joint_rotations_quat[AETHER_BONE_HEAD]);
        auto euler = quat_to_euler_deg(q);
        // Reject instantaneous >60 deg single-frame glitch jumps relative to last valid pose
        if (has_prev_) {
            for (size_t i = 0; i < 3u; ++i) {
                const float diff = euler[i] - last_safe_head_euler_[i];
                if (std::abs(diff) > 65.0f) {
                    euler[i] = last_safe_head_euler_[i] + std::clamp(diff, -25.0f, 25.0f);
                    diag.orientation_clamped = true;
                }
            }
        }
        const std::array<float, 3> clamped{
            std::clamp(euler[0], -std::max(10.0f, cfg.max_head_pitch_deg), std::max(10.0f, cfg.max_head_pitch_deg)),
            std::clamp(euler[1], -std::max(10.0f, cfg.max_head_yaw_deg), std::max(10.0f, cfg.max_head_yaw_deg)),
            std::clamp(euler[2], -std::max(10.0f, cfg.max_head_roll_deg), std::max(10.0f, cfg.max_head_roll_deg)),
        };
        if (std::abs(clamped[0] - euler[0]) > 1e-3f || std::abs(clamped[1] - euler[1]) > 1e-3f ||
            std::abs(clamped[2] - euler[2]) > 1e-3f) {
            diag.orientation_clamped = true;
        }
        const auto q_clamped = euler_deg_to_quat(clamped);
        for (size_t k = 0; k < 4u; ++k) {
            frame.joint_rotations_quat[AETHER_BONE_HEAD][k] = q_clamped[k];
        }
        last_safe_head_euler_ = clamped;
        has_prev_ = true;
    }

    // 3. Predictive signed-distance capsule anti-clipping for limbs/hands/arms vs head/torso/hair
    if (cfg.enable_anti_clip) {
        const bool has_head = frame.joint_positions_ws[AETHER_BONE_HEAD][3] > 0.0f || (frame.valid_flags & 0x01u) != 0u;
        const std::array<float, 3> cur_head{
            has_head ? frame.joint_positions_ws[AETHER_BONE_HEAD][0] : head_anchor[0],
            has_head ? frame.joint_positions_ws[AETHER_BONE_HEAD][1] : head_anchor[1],
            has_head ? frame.joint_positions_ws[AETHER_BONE_HEAD][2] : head_anchor[2],
        };
        const std::array<float, 3> torso_bot{hips_anchor[0], 0.96f, hips_anchor[2]};
        const std::array<float, 3> torso_top{cur_head[0] * 0.45f, 1.38f, cur_head[2] * 0.45f};
        const std::array<float, 3> hair_back_a{cur_head[0], cur_head[1] + 0.04f, cur_head[2] - 0.05f};
        const std::array<float, 3> hair_back_b{cur_head[0] * 0.6f, 1.15f, cur_head[2] - 0.09f};

        const std::array<uint32_t, 4> limb_joints{
            AETHER_BONE_LEFT_LOWER_ARM,
            AETHER_BONE_RIGHT_LOWER_ARM,
            AETHER_BONE_LEFT_HAND,
            AETHER_BONE_RIGHT_HAND,
        };
        for (uint32_t j : limb_joints) {
            if (frame.joint_positions_ws[j][3] <= 0.0f) {
                continue;
            }
            if (!finite3(frame.joint_positions_ws[j])) {
                continue;
            }
            std::array<float, 3> p{frame.joint_positions_ws[j][0], frame.joint_positions_ws[j][1],
                                   frame.joint_positions_ws[j][2]};

            if (cfg.enable_bounds) {
                // Constrain extremity excursion safely within expanded bounding envelope
                const std::array<float, 3> arm_min{cfg.bounds_min[0] - 0.35f, cfg.bounds_min[1] - 0.45f,
                                                   cfg.bounds_min[2] - 0.25f};
                const std::array<float, 3> arm_max{cfg.bounds_max[0] + 0.35f, cfg.bounds_max[1] + 0.45f,
                                                   cfg.bounds_max[2] + 0.45f};
                const std::array<float, 3> hand_anchor{0.0f, 1.15f, 0.0f};
                const std::array<float, 3> rel{p[0] - hand_anchor[0], p[1] - hand_anchor[1], p[2] - hand_anchor[2]};
                const auto comp = compress_translation_tanh(rel, arm_min, arm_max, cfg.soft_wall_elasticity);
                p[0] = hand_anchor[0] + comp[0];
                p[1] = hand_anchor[1] + comp[1];
                p[2] = hand_anchor[2] + comp[2];
            }

            // Keep hands/forearms out of torso capsule, head sphere, and posterior hair curtain capsule.
            // Runs AFTER the soft-bounds envelope so compression can never pull a limb back into the body,
            // and iterates (Gauss-Seidel) because the capsules overlap: escaping one may enter another.
            const float r_torso = 0.145f + margin;
            const float r_head = 0.120f + margin;
            const float r_hair = 0.105f + margin;
            constexpr float kSkin = 1e-4f; // Land strictly outside so the residual check reads exactly zero
            for (int iter = 0; iter < 8; ++iter) {
                const bool before = diag.collision_projected;
                diag.collision_projected = false;
                project_out_of_capsule(p, torso_bot, torso_top, r_torso + kSkin, true, diag);
                project_out_of_capsule(p, cur_head, cur_head, r_head + kSkin, true, diag);
                project_out_of_capsule(p, hair_back_a, hair_back_b, r_hair + kSkin, true, diag);
                const bool moved = diag.collision_projected;
                diag.collision_projected = before || moved;
                if (!moved) {
                    break;
                }
            }

            std::array<float, 3> scratch{};
            const float residual =
                std::max({r_torso - dist_point_to_segment(p, torso_bot, torso_top, scratch),
                          r_head - dist_point_to_segment(p, cur_head, cur_head, scratch),
                          r_hair - dist_point_to_segment(p, hair_back_a, hair_back_b, scratch), 0.0f});
            diag.residual_penetration_m = std::max(diag.residual_penetration_m, residual);

            frame.joint_positions_ws[j][0] = p[0];
            frame.joint_positions_ws[j][1] = p[1];
            frame.joint_positions_ws[j][2] = p[2];
        }
    }

    return diag;
}

} // namespace aether::tracking
