#include "pmx_physics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::animation {

namespace {

constexpr float kEps = 1e-6f;
constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] PmxVec3 v3_add(PmxVec3 a, PmxVec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] PmxVec3 v3_sub(PmxVec3 a, PmxVec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] PmxVec3 v3_scale(PmxVec3 a, float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}

[[nodiscard]] float v3_dot(PmxVec3 a, PmxVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] PmxVec3 v3_cross(PmxVec3 a, PmxVec3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

[[nodiscard]] float v3_len_sq(PmxVec3 a) noexcept {
    return v3_dot(a, a);
}

[[nodiscard]] float v3_len(PmxVec3 a) noexcept {
    return std::sqrt(v3_len_sq(a));
}

[[nodiscard]] PmxVec3 v3_normalize(PmxVec3 a, PmxVec3 fallback = {0.0f, 1.0f, 0.0f}) noexcept {
    const float l2 = v3_len_sq(a);
    if (l2 <= 1e-12f) {
        return fallback;
    }
    const float inv = 1.0f / std::sqrt(l2);
    return {a.x * inv, a.y * inv, a.z * inv};
}

[[nodiscard]] PmxVec3 v3_clamp_len(PmxVec3 v, float max_len) noexcept {
    const float l2 = v3_len_sq(v);
    if (l2 > max_len * max_len && l2 > 1e-12f) {
        const float s = max_len / std::sqrt(l2);
        return v3_scale(v, s);
    }
    return v;
}

[[nodiscard]] PmxQuat q_normalize(PmxQuat q) noexcept {
    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (n2 <= 1e-12f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / std::sqrt(n2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

[[nodiscard]] PmxQuat q_conj(PmxQuat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] PmxQuat q_mul(PmxQuat a, PmxQuat b) noexcept {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

[[nodiscard]] PmxVec3 q_rotate(PmxQuat q, PmxVec3 v) noexcept {
    const PmxVec3 u{q.x, q.y, q.z};
    const PmxVec3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v, v3_add(v3_scale(t, q.w), v3_cross(u, t)));
}

[[nodiscard]] PmxQuat q_from_axis_angle(PmxVec3 axis_unit, float angle_rad) noexcept {
    const float half = 0.5f * angle_rad;
    const float s = std::sin(half);
    const float c = std::cos(half);
    return q_normalize({axis_unit.x * s, axis_unit.y * s, axis_unit.z * s, c});
}

[[nodiscard]] PmxQuat q_slerp(PmxQuat a, PmxQuat b, float t) noexcept {
    float cos_half = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cos_half < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cos_half = -cos_half;
    }
    if (cos_half > 0.9995f) {
        return q_normalize({
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t,
        });
    }
    const float theta = std::acos(std::clamp(cos_half, -1.0f, 1.0f));
    const float sin_theta = std::sin(theta);
    if (std::abs(sin_theta) < 1e-6f) {
        return a;
    }
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    return q_normalize({
        a.x * wa + b.x * wb,
        a.y * wa + b.y * wb,
        a.z * wa + b.z * wb,
        a.w * wa + b.w * wb,
    });
}

[[nodiscard]] PmxQuat q_from_euler_yxz(PmxVec3 e) noexcept {
    // PMX / Bullet convention uses Y-X-Z intrinsic or Z-X-Y extrinsic Euler angles
    const PmxQuat qx = q_from_axis_angle({1.0f, 0.0f, 0.0f}, e.x);
    const PmxQuat qy = q_from_axis_angle({0.0f, 1.0f, 0.0f}, e.y);
    const PmxQuat qz = q_from_axis_angle({0.0f, 0.0f, 1.0f}, e.z);
    return q_normalize(q_mul(qy, q_mul(qx, qz)));
}

[[nodiscard]] PmxVec3 q_to_euler_yxz(PmxQuat q) noexcept {
    q = q_normalize(q);
    // Rotation matrix elements for R = Ry * Rx * Rz:
    // R12 = -sin(x)
    const float sin_x = std::clamp(-2.0f * (q.y * q.z - q.w * q.x), -1.0f, 1.0f);
    const float rx = std::asin(sin_x);
    float ry = 0.0f;
    float rz = 0.0f;
    if (std::abs(sin_x) < 0.9999f) {
        // R02 = sin(y)cos(x), R22 = cos(y)cos(x)
        const float r02 = 2.0f * (q.x * q.z + q.w * q.y);
        const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        ry = std::atan2(r02, r22);
        // R10 = cos(x)sin(z), R11 = cos(x)cos(z)
        const float r10 = 2.0f * (q.x * q.y + q.w * q.z);
        const float r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
        rz = std::atan2(r10, r11);
    } else {
        // Gimbal lock fallback
        const float r01 = 2.0f * (q.x * q.y - q.w * q.z);
        const float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
        ry = std::atan2(-r01, r00);
        rz = 0.0f;
    }
    return {rx, ry, rz};
}

void compose_trs_mat4(const float t[3], const float r[4], const float s[3], float out[16]) noexcept {
    const float x = r[0];
    const float y = r[1];
    const float z = r[2];
    const float w = r[3];
    const float x2 = x + x;
    const float y2 = y + y;
    const float z2 = z + z;
    const float xx = x * x2;
    const float xy = x * y2;
    const float xz = x * z2;
    const float yy = y * y2;
    const float yz = y * z2;
    const float zz = z * z2;
    const float wx = w * x2;
    const float wy = w * y2;
    const float wz = w * z2;

    out[0] = (1.0f - (yy + zz)) * s[0];
    out[1] = (xy + wz) * s[0];
    out[2] = (xz - wy) * s[0];
    out[3] = 0.0f;

    out[4] = (xy - wz) * s[1];
    out[5] = (1.0f - (xx + zz)) * s[1];
    out[6] = (yz + wx) * s[1];
    out[7] = 0.0f;

    out[8] = (xz + wy) * s[2];
    out[9] = (yz - wx) * s[2];
    out[10] = (1.0f - (xx + yy)) * s[2];
    out[11] = 0.0f;

    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0f;
}

void mat4_mul(const float a[16], const float b[16], float out[16]) noexcept {
    float tmp[16]{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            tmp[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

[[nodiscard]] PmxVec3 mat4_get_pos(const float m[16]) noexcept {
    return {m[12], m[13], m[14]};
}

[[nodiscard]] PmxQuat mat4_get_rot(const float m[16]) noexcept {
    PmxVec3 c0 = v3_normalize({m[0], m[1], m[2]}, {1.0f, 0.0f, 0.0f});
    PmxVec3 c1 = v3_normalize({m[4], m[5], m[6]}, {0.0f, 1.0f, 0.0f});
    PmxVec3 c2 = v3_normalize({m[8], m[9], m[10]}, {0.0f, 0.0f, 1.0f});

    const float trace = c0.x + c1.y + c2.z;
    PmxQuat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (c1.z - c2.y) / s;
        q.y = (c2.x - c0.z) / s;
        q.z = (c0.y - c1.x) / s;
    } else if (c0.x > c1.y && c0.x > c2.z) {
        const float s = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2.0f;
        q.w = (c1.z - c2.y) / s;
        q.x = 0.25f * s;
        q.y = (c1.x + c0.y) / s;
        q.z = (c2.x + c0.z) / s;
    } else if (c1.y > c2.z) {
        const float s = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2.0f;
        q.w = (c2.x - c0.z) / s;
        q.x = (c1.x + c0.y) / s;
        q.y = 0.25f * s;
        q.z = (c2.y + c1.z) / s;
    } else {
        const float s = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2.0f;
        q.w = (c0.y - c1.x) / s;
        q.x = (c2.x + c0.z) / s;
        q.y = (c2.y + c1.z) / s;
        q.z = 0.25f * s;
    }
    return q_normalize(q);
}

[[nodiscard]] PmxVec3 mat4_transform_point(const float m[16], PmxVec3 p) noexcept {
    return {
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
}

[[nodiscard]] PmxVec3 mat4_transform_dir(const float m[16], PmxVec3 d) noexcept {
    return {
        m[0] * d.x + m[4] * d.y + m[8] * d.z,
        m[1] * d.x + m[5] * d.y + m[9] * d.z,
        m[2] * d.x + m[6] * d.y + m[10] * d.z,
    };
}

[[nodiscard]] float body_bounding_radius(const AetherPmxRigidBodyDesc& rb) noexcept {
    const float sx = std::max(rb.size[0], 0.002f);
    const float sy = std::max(rb.size[1], 0.002f);
    const float sz = std::max(rb.size[2], 0.002f);
    switch (rb.shape_kind) {
        case AETHER_PMX_SHAPE_SPHERE:
            return sx;
        case AETHER_PMX_SHAPE_BOX:
            return std::sqrt(sx * sx + sy * sy + sz * sz) * 0.65f;
        case AETHER_PMX_SHAPE_CAPSULE:
        default:
            return sx + 0.5f * sy;
    }
}

void closest_points_on_body_pair(const AetherPmxRigidBodyDesc& desc_a,
                                 const PmxRigidBodyRuntime& state_a,
                                 const AetherPmxRigidBodyDesc& desc_b,
                                 const PmxRigidBodyRuntime& state_b,
                                 PmxVec3& pt_a,
                                 PmxVec3& pt_b,
                                 float& radius_a,
                                 float& radius_b) noexcept {
    auto eval_segment = [](const AetherPmxRigidBodyDesc& d,
                           const PmxRigidBodyRuntime& s,
                           PmxVec3& p0,
                           PmxVec3& p1,
                           float& r) noexcept {
        const float sx = std::max(d.size[0], 0.002f);
        const float sy = std::max(d.size[1], 0.002f);
        const float sz = std::max(d.size[2], 0.002f);
        if (d.shape_kind == AETHER_PMX_SHAPE_CAPSULE) {
            const PmxVec3 up = q_rotate(s.rot, {0.0f, 0.5f * sy, 0.0f});
            p0 = v3_sub(s.pos, up);
            p1 = v3_add(s.pos, up);
            r = sx;
        } else if (d.shape_kind == AETHER_PMX_SHAPE_BOX) {
            p0 = s.pos;
            p1 = s.pos;
            r = std::min({sx, sy, sz});
        } else {
            p0 = s.pos;
            p1 = s.pos;
            r = sx;
        }
    };

    PmxVec3 a0{}, a1{}, b0{}, b1{};
    eval_segment(desc_a, state_a, a0, a1, radius_a);
    eval_segment(desc_b, state_b, b0, b1, radius_b);

    // Closest point between segments [a0, a1] and [b0, b1]
    const PmxVec3 d1 = v3_sub(a1, a0);
    const PmxVec3 d2 = v3_sub(b1, b0);
    const PmxVec3 r = v3_sub(a0, b0);
    const float a = v3_dot(d1, d1);
    const float e = v3_dot(d2, d2);
    const float f = v3_dot(d2, r);

    float s = 0.0f;
    float t = 0.0f;
    if (a <= kEps && e <= kEps) {
        pt_a = a0;
        pt_b = b0;
        return;
    }
    if (a <= kEps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = v3_dot(d1, r);
        if (e <= kEps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = v3_dot(d1, d2);
            const float denom = a * e - b * b;
            if (denom != 0.0f) {
                s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    pt_a = v3_add(a0, v3_scale(d1, s));
    pt_b = v3_add(b0, v3_scale(d2, t));
}

}  // namespace

void PmxSkeletonAndPhysicsSolver::init(
    std::span<const AetherJointDesc> bones,
    std::span<const AetherPmxBoneExt> bone_exts,
    std::span<const AetherPmxIkLink> ik_links,
    std::span<const AetherPmxRigidBodyDesc> rigid_bodies,
    std::span<const AetherPmxConstraint6DofDesc> constraints_6dof) noexcept {
    bone_exts_.assign(bone_exts.begin(), bone_exts.end());
    ik_links_.assign(ik_links.begin(), ik_links.end());
    rigid_bodies_.assign(rigid_bodies.begin(), rigid_bodies.end());
    constraints_6dof_.assign(constraints_6dof.begin(), constraints_6dof.end());

    base_local_t_.resize(bones.size());
    base_local_r_.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        base_local_t_[i] = {
            bones[i].bind_translation[0],
            bones[i].bind_translation[1],
            bones[i].bind_translation[2],
        };
        base_local_r_[i] = q_normalize({
            bones[i].bind_rotation_quat[0],
            bones[i].bind_rotation_quat[1],
            bones[i].bind_rotation_quat[2],
            bones[i].bind_rotation_quat[3],
        });
    }

    rb_states_.assign(rigid_bodies_.size(), PmxRigidBodyRuntime{});
    rest_clearances_.clear();
    rest_clearances_.reserve(rigid_bodies_.size() * 2);

    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        const auto& rb = rigid_bodies_[i];
        auto& st = rb_states_[i];
        st.pos = {rb.pos_ms[0], rb.pos_ms[1], rb.pos_ms[2]};
        st.rot = q_from_euler_yxz({rb.rot_euler_rh[0], rb.rot_euler_rh[1], rb.rot_euler_rh[2]});
        st.prev_pos = st.pos;
        st.prev_rot = st.rot;
        st.lin_vel = {0.0f, 0.0f, 0.0f};
        st.ang_vel = {0.0f, 0.0f, 0.0f};
        if (rb.phys_mode == AETHER_PMX_RB_KINEMATIC || rb.mass <= 1e-5f) {
            st.inv_mass = 0.0f;
            st.inv_inertia = 0.0f;
        } else {
            st.inv_mass = 1.0f / std::max(rb.mass, 0.01f);
            const float r = std::max(body_bounding_radius(rb), 0.02f);
            st.inv_inertia = 2.5f * st.inv_mass / (r * r);
        }
    }

    // Record rest-pose overlap clearances so overlapping colliders at T-pose
    // never violently explode on frame 0.
    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        for (size_t j = i + 1; j < rigid_bodies_.size(); ++j) {
            const auto& a = rigid_bodies_[i];
            const auto& b = rigid_bodies_[j];
            const bool mask_ab = (a.collision_mask & (1u << (b.group_index & 15u))) != 0u;
            const bool mask_ba = (b.collision_mask & (1u << (a.group_index & 15u))) != 0u;
            if (!mask_ab || !mask_ba) {
                continue;
            }
            PmxVec3 pa{}, pb{};
            float ra = 0.0f, rb = 0.0f;
            closest_points_on_body_pair(a, rb_states_[i], b, rb_states_[j], pa, pb, ra, rb);
            const float dist = v3_len(v3_sub(pb, pa));
            const float sum_r = ra + rb;
            if (dist < sum_r) {
                rest_clearances_.push_back(PmxJointClearancePair{
                    static_cast<uint32_t>(i),
                    static_cast<uint32_t>(j),
                    std::max(0.0f, dist * 0.95f),
                });
            }
        }
    }

    accumulator_sec_ = 0.0f;
    initialized_from_pose_ = false;
}

void PmxSkeletonAndPhysicsSolver::reset_to_rest_pose(
    std::span<const AetherJointDesc> bones,
    std::span<const float> rest_world_mats16) noexcept {
    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        const auto& rb = rigid_bodies_[i];
        auto& st = rb_states_[i];
        st.pos = {rb.pos_ms[0], rb.pos_ms[1], rb.pos_ms[2]};
        st.rot = q_from_euler_yxz({rb.rot_euler_rh[0], rb.rot_euler_rh[1], rb.rot_euler_rh[2]});
        st.prev_pos = st.pos;
        st.prev_rot = st.rot;
        st.lin_vel = {0.0f, 0.0f, 0.0f};
        st.ang_vel = {0.0f, 0.0f, 0.0f};

        if (rb.bone_index >= 0 &&
            static_cast<size_t>(rb.bone_index) < bones.size() &&
            static_cast<size_t>(rb.bone_index) * 16 + 16 <= rest_world_mats16.size()) {
            const float* bm = &rest_world_mats16[static_cast<size_t>(rb.bone_index) * 16];
            const PmxVec3 b_pos = mat4_get_pos(bm);
            const PmxQuat b_rot = mat4_get_rot(bm);
            const PmxQuat b_rot_inv = q_conj(b_rot);
            st.bone_local_offset = q_rotate(b_rot_inv, v3_sub(st.pos, b_pos));
            st.bone_local_rot = q_normalize(q_mul(b_rot_inv, st.rot));
        } else {
            st.bone_local_offset = {0.0f, 0.0f, 0.0f};
            st.bone_local_rot = {0.0f, 0.0f, 0.0f, 1.0f};
        }
    }
    accumulator_sec_ = 0.0f;
    initialized_from_pose_ = true;
}

void PmxSkeletonAndPhysicsSolver::recompute_world_matrices(
    std::span<const AetherJointDesc> bones,
    std::span<const float> local_t,
    std::span<const float> local_r,
    std::span<const float> local_s,
    std::span<float> world_mats16) const noexcept {
    const size_t n = bones.size();
    for (size_t i = 0; i < n; ++i) {
        float local_m[16];
        compose_trs_mat4(&local_t[i * 3], &local_r[i * 4], &local_s[i * 3], local_m);
        const int32_t parent = bones[i].parent_index;
        if (parent >= 0 && static_cast<size_t>(parent) < i) {
            mat4_mul(&world_mats16[static_cast<size_t>(parent) * 16], local_m, &world_mats16[i * 16]);
        } else {
            std::memcpy(&world_mats16[i * 16], local_m, sizeof(local_m));
        }
    }
}

void PmxSkeletonAndPhysicsSolver::solve_ccd_ik(
    std::span<const AetherJointDesc> bones,
    std::span<const float> local_t,
    std::span<float> local_r,
    std::span<const float> local_s,
    std::span<float> world_mats16) const noexcept {
    const size_t n = std::min(bones.size(), bone_exts_.size());
    if (n == 0 || ik_links_.empty()) {
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        const auto& ext = bone_exts_[i];
        if (ext.ik_target < 0 ||
            static_cast<size_t>(ext.ik_target) >= n ||
            ext.ik_link_count == 0) {
            continue;
        }
        const size_t effector_idx = static_cast<size_t>(ext.ik_target);
        const uint32_t loops = std::min<uint32_t>(static_cast<uint32_t>(std::max(ext.ik_loops, 0)), 40u);
        const float max_step_angle = std::max(ext.ik_limit_rad, 0.001f);
        const PmxVec3 goal_pos = mat4_get_pos(&world_mats16[i * 16]);

        for (uint32_t iter = 0; iter < loops; ++iter) {
            for (uint32_t l = 0; l < ext.ik_link_count; ++l) {
                const uint32_t link_flat_idx = ext.ik_link_offset + l;
                if (link_flat_idx >= ik_links_.size()) {
                    break;
                }
                const auto& link = ik_links_[link_flat_idx];
                if (static_cast<size_t>(link.bone_index) >= n) {
                    continue;
                }
                const size_t lb = static_cast<size_t>(link.bone_index);
                const PmxVec3 eff_pos = mat4_get_pos(&world_mats16[effector_idx * 16]);
                const PmxVec3 link_pos = mat4_get_pos(&world_mats16[lb * 16]);
                const PmxVec3 to_eff = v3_sub(eff_pos, link_pos);
                const PmxVec3 to_goal = v3_sub(goal_pos, link_pos);
                if (v3_len_sq(to_eff) < 1e-8f || v3_len_sq(to_goal) < 1e-8f) {
                    continue;
                }
                if (v3_len_sq(v3_sub(goal_pos, eff_pos)) < 1e-7f) {
                    break;
                }

                const PmxQuat link_world_rot = mat4_get_rot(&world_mats16[lb * 16]);
                const PmxQuat link_world_inv = q_conj(link_world_rot);
                const PmxVec3 local_eff = v3_normalize(q_rotate(link_world_inv, to_eff));
                const PmxVec3 local_goal = v3_normalize(q_rotate(link_world_inv, to_goal));
                const float cos_ang = std::clamp(v3_dot(local_eff, local_goal), -1.0f, 1.0f);
                float angle = std::acos(cos_ang);
                if (angle < 1e-4f) {
                    continue;
                }
                angle = std::min(angle, max_step_angle);
                const PmxVec3 axis = v3_cross(local_eff, local_goal);
                if (v3_len_sq(axis) < 1e-10f) {
                    continue;
                }
                const PmxQuat delta_q = q_from_axis_angle(v3_normalize(axis), angle);
                PmxQuat cur_q{
                    local_r[lb * 4 + 0],
                    local_r[lb * 4 + 1],
                    local_r[lb * 4 + 2],
                    local_r[lb * 4 + 3],
                };
                cur_q = q_normalize(q_mul(cur_q, delta_q));

                if (link.has_limit != 0u) {
                    PmxVec3 euler = q_to_euler_yxz(cur_q);
                    euler.x = std::clamp(euler.x, link.limit_min_rad[0], link.limit_max_rad[0]);
                    euler.y = std::clamp(euler.y, link.limit_min_rad[1], link.limit_max_rad[1]);
                    euler.z = std::clamp(euler.z, link.limit_min_rad[2], link.limit_max_rad[2]);
                    cur_q = q_from_euler_yxz(euler);
                }

                local_r[lb * 4 + 0] = cur_q.x;
                local_r[lb * 4 + 1] = cur_q.y;
                local_r[lb * 4 + 2] = cur_q.z;
                local_r[lb * 4 + 3] = cur_q.w;
                recompute_world_matrices(bones, local_t, local_r, local_s, world_mats16);
            }
        }
    }
}

void PmxSkeletonAndPhysicsSolver::solve_append_transforms(
    std::span<const AetherJointDesc> bones,
    std::span<float> local_t,
    std::span<float> local_r,
    std::span<const float> local_s,
    std::span<float> world_mats16) noexcept {
    const size_t n = std::min(bones.size(), bone_exts_.size());
    if (n == 0) {
        return;
    }

    const PmxQuat identity_q{0.0f, 0.0f, 0.0f, 1.0f};
    bool any_changed = false;

    for (size_t i = 0; i < n; ++i) {
        const auto& ext = bone_exts_[i];
        const uint32_t flags = ext.append_flags;
        if ((flags & (AETHER_PMX_APPEND_ROT | AETHER_PMX_APPEND_MOVE)) == 0u) {
            continue;
        }
        if (ext.append_parent < 0 || static_cast<size_t>(ext.append_parent) >= n) {
            continue;
        }
        const size_t src = static_cast<size_t>(ext.append_parent);
        const float ratio = ext.append_ratio;

        if ((flags & AETHER_PMX_APPEND_ROT) != 0u) {
            const PmxQuat src_cur{
                local_r[src * 4 + 0],
                local_r[src * 4 + 1],
                local_r[src * 4 + 2],
                local_r[src * 4 + 3],
            };
            const PmxQuat src_base = (src < base_local_r_.size()) ? base_local_r_[src] : identity_q;
            const PmxQuat src_delta = q_normalize(q_mul(q_conj(src_base), src_cur));
            const float abs_r = std::clamp(std::abs(ratio), 0.0f, 2.0f);
            PmxQuat scaled = q_slerp(identity_q, src_delta, std::min(abs_r, 1.0f));
            if (abs_r > 1.0f) {
                const PmxQuat extra = q_slerp(identity_q, src_delta, abs_r - 1.0f);
                scaled = q_normalize(q_mul(scaled, extra));
            }
            if (ratio < 0.0f) {
                scaled = q_conj(scaled);
            }
            const PmxQuat dst_cur{
                local_r[i * 4 + 0],
                local_r[i * 4 + 1],
                local_r[i * 4 + 2],
                local_r[i * 4 + 3],
            };
            const PmxQuat dst_new = q_normalize(q_mul(dst_cur, scaled));
            local_r[i * 4 + 0] = dst_new.x;
            local_r[i * 4 + 1] = dst_new.y;
            local_r[i * 4 + 2] = dst_new.z;
            local_r[i * 4 + 3] = dst_new.w;
            any_changed = true;
        }

        if ((flags & AETHER_PMX_APPEND_MOVE) != 0u) {
            const PmxVec3 src_base = (src < base_local_t_.size()) ? base_local_t_[src] : PmxVec3{};
            const PmxVec3 src_cur{
                local_t[src * 3 + 0],
                local_t[src * 3 + 1],
                local_t[src * 3 + 2],
            };
            const PmxVec3 delta = v3_scale(v3_sub(src_cur, src_base), ratio);
            local_t[i * 3 + 0] += delta.x;
            local_t[i * 3 + 1] += delta.y;
            local_t[i * 3 + 2] += delta.z;
            any_changed = true;
        }
    }

    if (any_changed) {
        recompute_world_matrices(bones, local_t, local_r, local_s, world_mats16);
    }
}

void PmxSkeletonAndPhysicsSolver::step_physics_fixed(
    float fixed_dt,
    std::span<const AetherJointDesc> bones,
    std::span<const float> world_mats16) noexcept {
    if (rigid_bodies_.empty()) {
        return;
    }

    // 1. Sync kinematic bodies to their current animated bone poses;
    //    integrate gravity & exponential damping on dynamic bodies.
    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        const auto& rb = rigid_bodies_[i];
        auto& st = rb_states_[i];
        st.prev_pos = st.pos;
        st.prev_rot = st.rot;

        if (rb.phys_mode == AETHER_PMX_RB_KINEMATIC || st.inv_mass <= 0.0f) {
            if (rb.bone_index >= 0 && static_cast<size_t>(rb.bone_index) < bones.size()) {
                const float* bm = &world_mats16[static_cast<size_t>(rb.bone_index) * 16];
                const PmxVec3 b_pos = mat4_get_pos(bm);
                const PmxQuat b_rot = mat4_get_rot(bm);
                st.pos = v3_add(b_pos, q_rotate(b_rot, st.bone_local_offset));
                st.rot = q_normalize(q_mul(b_rot, st.bone_local_rot));
            }
            st.lin_vel = v3_scale(v3_sub(st.pos, st.prev_pos), 1.0f / fixed_dt);
            st.ang_vel = {0.0f, 0.0f, 0.0f};
            continue;
        }

        const float lin_damp = std::exp(-std::clamp(rb.linear_damping, 0.0f, 0.999f) * 4.0f * fixed_dt);
        const float ang_damp = std::exp(-std::clamp(rb.angular_damping, 0.0f, 0.999f) * 4.0f * fixed_dt);
        st.lin_vel = v3_scale(st.lin_vel, lin_damp);
        st.ang_vel = v3_scale(st.ang_vel, ang_damp);
        st.lin_vel.y += kGravityY * fixed_dt;

        st.lin_vel = v3_clamp_len(st.lin_vel, kMaxLinearVel);
        st.ang_vel = v3_clamp_len(st.ang_vel, kMaxAngularVel);

        st.pos = v3_add(st.pos, v3_scale(st.lin_vel, fixed_dt));
        const float w_len = v3_len(st.ang_vel);
        if (w_len > 1e-6f) {
            const PmxQuat dq = q_from_axis_angle(v3_scale(st.ang_vel, 1.0f / w_len), w_len * fixed_dt);
            st.rot = q_normalize(q_mul(dq, st.rot));
        }
    }

    // 2. Solve 6-DOF Spring Joints (linear limits + spring pull + angular limits)
    constexpr int kConstraintIterations = 4;
    for (int iter = 0; iter < kConstraintIterations; ++iter) {
        for (const auto& c : constraints_6dof_) {
            if (c.rb_a < 0 || c.rb_b < 0 ||
                static_cast<size_t>(c.rb_a) >= rigid_bodies_.size() ||
                static_cast<size_t>(c.rb_b) >= rigid_bodies_.size()) {
                continue;
            }
            const size_t ia = static_cast<size_t>(c.rb_a);
            const size_t ib = static_cast<size_t>(c.rb_b);
            auto& a = rb_states_[ia];
            auto& b = rb_states_[ib];
            const float w_sum = a.inv_mass + b.inv_mass;
            if (w_sum <= 1e-8f) {
                continue;
            }

            // Compute desired relative anchor offset from rest configuration
            const auto& rba = rigid_bodies_[ia];
            const auto& rbb = rigid_bodies_[ib];
            const PmxVec3 rest_a{rba.pos_ms[0], rba.pos_ms[1], rba.pos_ms[2]};
            const PmxVec3 rest_b{rbb.pos_ms[0], rbb.pos_ms[1], rbb.pos_ms[2]};
            const PmxQuat rest_rot_a = q_from_euler_yxz({rba.rot_euler_rh[0], rba.rot_euler_rh[1], rba.rot_euler_rh[2]});
            const PmxQuat rest_rot_b = q_from_euler_yxz({rbb.rot_euler_rh[0], rbb.rot_euler_rh[1], rbb.rot_euler_rh[2]});
            const PmxVec3 rest_local_diff = q_rotate(q_conj(rest_rot_a), v3_sub(rest_b, rest_a));

            const PmxVec3 target_b_in_a = v3_add(a.pos, q_rotate(a.rot, rest_local_diff));
            const PmxVec3 err_world = v3_sub(b.pos, target_b_in_a);
            PmxVec3 err_local = q_rotate(q_conj(a.rot), err_world);

            // Clamp linear translation to [pos_min, pos_max] plus spring restoration
            PmxVec3 corr_local{0.0f, 0.0f, 0.0f};
            const float lin_min[3] = {c.pos_min[0], c.pos_min[1], c.pos_min[2]};
            const float lin_max[3] = {c.pos_max[0], c.pos_max[1], c.pos_max[2]};
            const float lin_k[3] = {c.spring_pos[0], c.spring_pos[1], c.spring_pos[2]};
            float err_arr[3] = {err_local.x, err_local.y, err_local.z};
            float corr_arr[3] = {0.0f, 0.0f, 0.0f};
            for (int axis = 0; axis < 3; ++axis) {
                const float lo = std::min(lin_min[axis], lin_max[axis]);
                const float hi = std::max(lin_min[axis], lin_max[axis]);
                if (err_arr[axis] < lo) {
                    corr_arr[axis] = err_arr[axis] - lo;
                } else if (err_arr[axis] > hi) {
                    corr_arr[axis] = err_arr[axis] - hi;
                } else if (lin_k[axis] > 0.0f) {
                    const float spring_alpha = std::clamp(lin_k[axis] * fixed_dt * 0.25f, 0.0f, 0.45f);
                    corr_arr[axis] = err_arr[axis] * spring_alpha;
                }
            }
            corr_local = {corr_arr[0], corr_arr[1], corr_arr[2]};
            const PmxVec3 corr_world = q_rotate(a.rot, corr_local);
            const float wa = a.inv_mass / w_sum;
            const float wb = b.inv_mass / w_sum;
            a.pos = v3_add(a.pos, v3_scale(corr_world, wa));
            b.pos = v3_sub(b.pos, v3_scale(corr_world, wb));

            // Angular limits + spring restoration in local joint frame
            const PmxQuat rest_rel_q = q_normalize(q_mul(q_conj(rest_rot_a), rest_rot_b));
            const PmxQuat cur_rel_q = q_normalize(q_mul(q_conj(a.rot), b.rot));
            const PmxQuat diff_q = q_normalize(q_mul(q_conj(rest_rel_q), cur_rel_q));
            PmxVec3 eul = q_to_euler_yxz(diff_q);
            float e_arr[3] = {eul.x, eul.y, eul.z};
            for (int axis = 0; axis < 3; ++axis) {
                const float lo = std::min(c.rot_min[axis], c.rot_max[axis]);
                const float hi = std::max(c.rot_min[axis], c.rot_max[axis]);
                e_arr[axis] = std::clamp(e_arr[axis], lo, hi);
                if (c.spring_rot[axis] > 0.0f) {
                    const float k = std::clamp(c.spring_rot[axis] * fixed_dt * 0.15f, 0.0f, 0.45f);
                    e_arr[axis] *= (1.0f - k);
                }
            }
            const PmxQuat clamped_rel = q_normalize(q_mul(rest_rel_q, q_from_euler_yxz({e_arr[0], e_arr[1], e_arr[2]})));
            const PmxQuat target_rot_b = q_normalize(q_mul(a.rot, clamped_rel));
            if (b.inv_mass > 0.0f) {
                b.rot = q_slerp(b.rot, target_rot_b, 0.65f);
            }
        }

        // 3. Collider-vs-collider depenetration (respecting group mask & rest clearance)
        for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
            for (size_t j = i + 1; j < rigid_bodies_.size(); ++j) {
                const auto& rba = rigid_bodies_[i];
                const auto& rbb = rigid_bodies_[j];
                auto& sa = rb_states_[i];
                auto& sb = rb_states_[j];
                const float w_sum = sa.inv_mass + sb.inv_mass;
                if (w_sum <= 1e-8f) {
                    continue;
                }
                const bool mask_ab = (rba.collision_mask & (1u << (rbb.group_index & 15u))) != 0u;
                const bool mask_ba = (rbb.collision_mask & (1u << (rba.group_index & 15u))) != 0u;
                if (!mask_ab || !mask_ba) {
                    continue;
                }

                PmxVec3 pa{}, pb{};
                float ra = 0.0f, rb = 0.0f;
                closest_points_on_body_pair(rba, sa, rbb, sb, pa, pb, ra, rb);
                float min_sep = ra + rb;
                for (const auto& rc : rest_clearances_) {
                    if (rc.body_a == i && rc.body_b == j) {
                        min_sep = std::min(min_sep, rc.rest_clear_dist);
                        break;
                    }
                }

                const PmxVec3 delta = v3_sub(pb, pa);
                const float dist = v3_len(delta);
                if (dist < min_sep) {
                    const PmxVec3 n = v3_normalize(delta, {0.0f, 1.0f, 0.0f});
                    const float pen = (min_sep - dist) * 0.55f;
                    const float wa = sa.inv_mass / w_sum;
                    const float wb = sb.inv_mass / w_sum;
                    sa.pos = v3_sub(sa.pos, v3_scale(n, pen * wa));
                    sb.pos = v3_add(sb.pos, v3_scale(n, pen * wb));
                }
            }
        }
    }

    // 4. Reconstruct velocities from position/rotation change and clamp
    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        const auto& rb = rigid_bodies_[i];
        auto& st = rb_states_[i];
        if (rb.phys_mode == AETHER_PMX_RB_KINEMATIC || st.inv_mass <= 0.0f) {
            continue;
        }
        st.lin_vel = v3_clamp_len(v3_scale(v3_sub(st.pos, st.prev_pos), 1.0f / fixed_dt), kMaxLinearVel);

        PmxQuat dq = q_normalize(q_mul(st.rot, q_conj(st.prev_rot)));
        if (dq.w < 0.0f) {
            dq = {-dq.x, -dq.y, -dq.z, -dq.w};
        }
        const float sin_half = std::sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
        if (sin_half > 1e-5f) {
            const float angle = 2.0f * std::atan2(sin_half, dq.w);
            const PmxVec3 axis{dq.x / sin_half, dq.y / sin_half, dq.z / sin_half};
            st.ang_vel = v3_clamp_len(v3_scale(axis, angle / fixed_dt), kMaxAngularVel);
        } else {
            st.ang_vel = {0.0f, 0.0f, 0.0f};
        }
    }
}

void PmxSkeletonAndPhysicsSolver::write_dynamic_bodies_to_bones(
    std::span<const AetherJointDesc> bones,
    std::span<float> local_t,
    std::span<float> local_r,
    std::span<const float> local_s,
    std::span<float> world_mats16) const noexcept {
    if (rigid_bodies_.empty()) {
        return;
    }

    bool any_written = false;
    for (size_t i = 0; i < rigid_bodies_.size(); ++i) {
        const auto& rb = rigid_bodies_[i];
        const auto& st = rb_states_[i];
        if (rb.phys_mode == AETHER_PMX_RB_KINEMATIC ||
            rb.bone_index < 0 ||
            static_cast<size_t>(rb.bone_index) >= bones.size()) {
            continue;
        }
        const size_t bi = static_cast<size_t>(rb.bone_index);
        const PmxQuat target_world_rot = q_normalize(q_mul(st.rot, q_conj(st.bone_local_rot)));
        const PmxVec3 target_world_pos =
            (rb.phys_mode == AETHER_PMX_RB_DYNAMIC)
                ? v3_sub(st.pos, q_rotate(target_world_rot, st.bone_local_offset))
                : mat4_get_pos(&world_mats16[bi * 16]);

        const int32_t parent = bones[bi].parent_index;
        if (parent >= 0 && static_cast<size_t>(parent) < bones.size()) {
            const float* pm = &world_mats16[static_cast<size_t>(parent) * 16];
            const PmxVec3 p_pos = mat4_get_pos(pm);
            const PmxQuat p_rot = mat4_get_rot(pm);
            const PmxQuat p_rot_inv = q_conj(p_rot);
            const PmxQuat new_local_r = q_normalize(q_mul(p_rot_inv, target_world_rot));
            local_r[bi * 4 + 0] = new_local_r.x;
            local_r[bi * 4 + 1] = new_local_r.y;
            local_r[bi * 4 + 2] = new_local_r.z;
            local_r[bi * 4 + 3] = new_local_r.w;

            if (rb.phys_mode == AETHER_PMX_RB_DYNAMIC) {
                const PmxVec3 new_local_t = q_rotate(p_rot_inv, v3_sub(target_world_pos, p_pos));
                local_t[bi * 3 + 0] = new_local_t.x;
                local_t[bi * 3 + 1] = new_local_t.y;
                local_t[bi * 3 + 2] = new_local_t.z;
            }
        } else {
            local_r[bi * 4 + 0] = target_world_rot.x;
            local_r[bi * 4 + 1] = target_world_rot.y;
            local_r[bi * 4 + 2] = target_world_rot.z;
            local_r[bi * 4 + 3] = target_world_rot.w;
            if (rb.phys_mode == AETHER_PMX_RB_DYNAMIC) {
                local_t[bi * 3 + 0] = target_world_pos.x;
                local_t[bi * 3 + 1] = target_world_pos.y;
                local_t[bi * 3 + 2] = target_world_pos.z;
            }
        }
        any_written = true;
    }

    if (any_written) {
        recompute_world_matrices(bones, local_t, local_r, local_s, world_mats16);
    }
}

void PmxSkeletonAndPhysicsSolver::step(
    float dt_sec,
    std::span<const AetherJointDesc> bones,
    std::span<float> local_t,
    std::span<float> local_r,
    std::span<const float> local_s,
    std::span<float> world_mats16) noexcept {
    if (bones.empty() || empty()) {
        return;
    }

    recompute_world_matrices(bones, local_t, local_r, local_s, world_mats16);
    if (!initialized_from_pose_) {
        reset_to_rest_pose(bones, world_mats16);
    }

    // 1. CCD-IK
    solve_ccd_ik(bones, local_t, local_r, local_s, world_mats16);

    // 2. Append Transform (Rotate + Move)
    solve_append_transforms(bones, local_t, local_r, local_s, world_mats16);

    // 3. Fixed-timestep 60 Hz RigidBody + 6-DOF Spring Joint simulation
    if (!rigid_bodies_.empty()) {
        const float clamped_dt = std::clamp(dt_sec, 0.0f, kFixedDt * static_cast<float>(kMaxSubsteps));
        accumulator_sec_ += clamped_dt;
        uint32_t substeps = 0;
        while (accumulator_sec_ >= kFixedDt && substeps < kMaxSubsteps) {
            step_physics_fixed(kFixedDt, bones, world_mats16);
            accumulator_sec_ -= kFixedDt;
            ++substeps;
        }
        if (substeps == 0 && clamped_dt > 0.0f) {
            step_physics_fixed(kFixedDt, bones, world_mats16);
            accumulator_sec_ = 0.0f;
        }
        write_dynamic_bodies_to_bones(bones, local_t, local_r, local_s, world_mats16);
    }
}

void PmxSkeletonAndPhysicsSolver::evaluate_sdef_vertex(
    const float in_pos[3],
    const float in_nrm[3],
    const uint16_t joints[4],
    const float weights[4],
    const AetherPmxSdefVertex& sdef,
    std::span<const float> skin_mats16,
    std::span<const float> world_mats16,
    std::span<const float> inv_bind_mats16,
    float out_pos[3],
    float out_nrm[3]) noexcept {
    const size_t num_bones = skin_mats16.size() / 16;
    if ((sdef.flags & 1u) != 0u && num_bones > 0) {
        const size_t b0 = std::min<size_t>(joints[0], num_bones - 1);
        const size_t b1 = std::min<size_t>(joints[1], num_bones - 1);
        const float w0 = std::clamp(weights[0], 0.0f, 1.0f);
        const float w1 = 1.0f - w0;

        const float* m0 = &skin_mats16[b0 * 16];
        const float* m1 = &skin_mats16[b1 * 16];
        const PmxQuat q0 = mat4_get_rot(m0);
        const PmxQuat q1 = mat4_get_rot(m1);
        const PmxQuat q_blend = q_slerp(q0, q1, w1);

        const PmxVec3 c{sdef.c[0], sdef.c[1], sdef.c[2]};
        const PmxVec3 r0{sdef.r0[0], sdef.r0[1], sdef.r0[2]};
        const PmxVec3 r1{sdef.r1[0], sdef.r1[1], sdef.r1[2]};
        const PmxVec3 p{in_pos[0], in_pos[1], in_pos[2]};
        const PmxVec3 n{in_nrm[0], in_nrm[1], in_nrm[2]};

        // SDEF volume-preserving spherical interpolation around pivot C:
        //   r_mix = w0 * r0 + w1 * r1
        //   p_out = w0*(M0 * r0) + w1*(M1 * r1) + R_slerp(q0, q1, w1) * (p - c)
        //Wait: in standard PMX SDEF:
        //   rw = w0 * r0 + w1 * r1
        //   c0 = c + 0.5 * (r0 - rw), c1 = c + 0.5 * (r1 - rw)
        //   p' = w0 * (M0 * c0) + w1 * (M1 * c1) + q_blend * (p - c)
        (void)world_mats16;
        (void)inv_bind_mats16;
        const PmxVec3 rw = v3_add(v3_scale(r0, w0), v3_scale(r1, w1));
        const PmxVec3 c0 = v3_add(c, v3_scale(v3_sub(r0, rw), 0.5f));
        const PmxVec3 c1 = v3_add(c, v3_scale(v3_sub(r1, rw), 0.5f));

        const PmxVec3 mc0 = mat4_transform_point(m0, c0);
        const PmxVec3 mc1 = mat4_transform_point(m1, c1);
        const PmxVec3 center_def = v3_add(v3_scale(mc0, w0), v3_scale(mc1, w1));
        const PmxVec3 rot_offset = q_rotate(q_blend, v3_sub(p, c));
        const PmxVec3 final_p = v3_add(center_def, rot_offset);
        const PmxVec3 final_n = v3_normalize(q_rotate(q_blend, n), {0.0f, 1.0f, 0.0f});

        out_pos[0] = final_p.x;
        out_pos[1] = final_p.y;
        out_pos[2] = final_p.z;
        out_nrm[0] = final_n.x;
        out_nrm[1] = final_n.y;
        out_nrm[2] = final_n.z;
        return;
    }

    // Fallback Linear Blend Skinning (BDEF1/2/4)
    PmxVec3 acc_p{0.0f, 0.0f, 0.0f};
    PmxVec3 acc_n{0.0f, 0.0f, 0.0f};
    const PmxVec3 p{in_pos[0], in_pos[1], in_pos[2]};
    const PmxVec3 n{in_nrm[0], in_nrm[1], in_nrm[2]};
    float w_total = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const float w = weights[k];
        if (w <= 0.0f || num_bones == 0) {
            continue;
        }
        const size_t bi = std::min<size_t>(joints[k], num_bones - 1);
        const float* m = &skin_mats16[bi * 16];
        acc_p = v3_add(acc_p, v3_scale(mat4_transform_point(m, p), w));
        acc_n = v3_add(acc_n, v3_scale(mat4_transform_dir(m, n), w));
        w_total += w;
    }
    if (w_total <= 1e-6f) {
        out_pos[0] = in_pos[0];
        out_pos[1] = in_pos[1];
        out_pos[2] = in_pos[2];
        out_nrm[0] = in_nrm[0];
        out_nrm[1] = in_nrm[1];
        out_nrm[2] = in_nrm[2];
    } else {
        out_pos[0] = acc_p.x;
        out_pos[1] = acc_p.y;
        out_pos[2] = acc_p.z;
        const PmxVec3 nn = v3_normalize(acc_n, {0.0f, 1.0f, 0.0f});
        out_nrm[0] = nn.x;
        out_nrm[1] = nn.y;
        out_nrm[2] = nn.z;
    }
}

}  // namespace aether::animation
