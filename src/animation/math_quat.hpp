#pragma once

#include "aether_ffi_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace aether::animation {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr float kRadToDeg = 180.0f / kPi;

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr Vec3() noexcept = default;
    constexpr Vec3(float x_, float y_, float z_) noexcept : x(x_), y(y_), z(z_) {}
};

[[nodiscard]] inline constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] inline constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] inline constexpr Vec3 operator-(Vec3 a) noexcept {
    return {-a.x, -a.y, -a.z};
}
[[nodiscard]] inline constexpr Vec3 operator*(Vec3 a, float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] inline constexpr Vec3 operator*(float s, Vec3 a) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] inline constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] inline constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}
[[nodiscard]] inline float length_sq(Vec3 v) noexcept {
    return dot(v, v);
}
[[nodiscard]] inline float length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}
[[nodiscard]] inline Vec3 normalize_safe(Vec3 v, Vec3 fallback = {0.0f, 1.0f, 0.0f}) noexcept {
    const float len2 = dot(v, v);
    if (len2 > 1e-12f) {
        const float inv = 1.0f / std::sqrt(len2);
        return {v.x * inv, v.y * inv, v.z * inv};
    }
    return fallback;
}
[[nodiscard]] inline Vec3 lerp(Vec3 a, Vec3 b, float t) noexcept {
    return a + (b - a) * t;
}

/** Unit quaternion `[x, y, z, w]` (`§6.3`, `§9`). */
struct Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};

    constexpr Quat() noexcept = default;
    constexpr Quat(float x_, float y_, float z_, float w_) noexcept : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr Quat identity() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
};

[[nodiscard]] inline constexpr float dot(Quat a, Quat b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] inline constexpr Quat conjugate(Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}
[[nodiscard]] inline Quat normalize(Quat q) noexcept {
    const float n2 = dot(q, q);
    if (n2 > 1e-12f) {
        const float inv = 1.0f / std::sqrt(n2);
        return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    }
    return Quat::identity();
}
[[nodiscard]] inline constexpr Quat operator*(Quat a, Quat b) noexcept {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}
[[nodiscard]] inline Vec3 rotate(Quat q, Vec3 v) noexcept {
    const Vec3 qv{q.x, q.y, q.z};
    const Vec3 t = 2.0f * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

[[nodiscard]] inline Vec3 rotate_vec(Quat q, Vec3 v) noexcept {
    return rotate(q, v);
}

/** Quaternion tangent-space logarithm `r = log(q) = axis * half_angle` (`§9.2`: 2*r = rotation vector). */
[[nodiscard]] inline Vec3 quat_log(Quat q_in) noexcept {
    Quat q = normalize(q_in);
    if (q.w < 0.0f) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    const float v_len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (v_len < 1e-7f) {
        return {q.x, q.y, q.z};
    }
    const float theta = std::atan2(v_len, std::clamp(q.w, -1.0f, 1.0f));
    const float scale = theta / v_len;
    return {q.x * scale, q.y * scale, q.z * scale};
}

/** Quaternion exponential `exp(r)` matching `quat_log` (`§9.2`, `§9.3`). */
[[nodiscard]] inline Quat quat_exp(Vec3 r) noexcept {
    const float theta = length(r);
    if (theta < 1e-6f) {
        return normalize({r.x, r.y, r.z, 1.0f - 0.5f * theta * theta});
    }
    const float s = std::sin(theta) / theta;
    return {r.x * s, r.y * s, r.z * s, std::cos(theta)};
}

[[nodiscard]] inline Vec3 quat_to_rotvec(Quat q) noexcept {
    return quat_log(q) * 2.0f;
}
[[nodiscard]] inline Quat rotvec_to_quat(Vec3 r) noexcept {
    return quat_exp(r * 0.5f);
}
[[nodiscard]] inline Vec3 to_axis_angle(Quat q) noexcept {
    return quat_to_rotvec(q);
}

[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t_in) noexcept {
    const float t = std::clamp(t_in, 0.0f, 1.0f);
    Quat q1 = normalize(a);
    Quat q2 = normalize(b);
    float d = dot(q1, q2);
    if (d < 0.0f) {
        q2 = {-q2.x, -q2.y, -q2.z, -q2.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return normalize({
            q1.x + (q2.x - q1.x) * t,
            q1.y + (q2.y - q1.y) * t,
            q1.z + (q2.z - q1.z) * t,
            q1.w + (q2.w - q1.w) * t,
        });
    }
    const float theta0 = std::acos(std::clamp(d, -1.0f, 1.0f));
    const float sin0 = std::sin(theta0);
    const float w0 = std::sin((1.0f - t) * theta0) / sin0;
    const float w1 = std::sin(t * theta0) / sin0;
    return normalize({
        w0 * q1.x + w1 * q2.x,
        w0 * q1.y + w1 * q2.y,
        w0 * q1.z + w1 * q2.z,
        w0 * q1.w + w1 * q2.w,
    });
}

[[nodiscard]] inline Quat quat_from_axis_angle(Vec3 axis, float rad) noexcept {
    const Vec3 a = normalize_safe(axis, {0.0f, 1.0f, 0.0f});
    const float half = 0.5f * rad;
    const float s = std::sin(half);
    return {a.x * s, a.y * s, a.z * s, std::cos(half)};
}

[[nodiscard]] inline Quat from_axis_angle(Vec3 axis, float rad) noexcept {
    return quat_from_axis_angle(axis, rad);
}

[[nodiscard]] inline Quat from_euler_xyz(float rx, float ry, float rz) noexcept {
    return normalize(quat_from_axis_angle({0.0f, 0.0f, 1.0f}, rz) * quat_from_axis_angle({0.0f, 1.0f, 0.0f}, ry) *
                     quat_from_axis_angle({1.0f, 0.0f, 0.0f}, rx));
}

[[nodiscard]] inline Vec3 to_euler_xyz(Quat q_in) noexcept {
    const Quat q = normalize(q_in);
    const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    const float y = (std::abs(sinp) >= 1.0f) ? std::copysign(1.57079632679f, sinp) : std::asin(sinp);
    const float x = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float z = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    return {x, y, z};
}

[[nodiscard]] inline Quat quat_from_to(Vec3 from_v, Vec3 to_v) noexcept {
    const Vec3 u = normalize_safe(from_v, {0.0f, 1.0f, 0.0f});
    const Vec3 v = normalize_safe(to_v, u);
    const float d = std::clamp(dot(u, v), -1.0f, 1.0f);
    if (d > 0.99999f) {
        return Quat::identity();
    }
    if (d < -0.99999f) {
        Vec3 ortho = cross({1.0f, 0.0f, 0.0f}, u);
        if (length_sq(ortho) < 1e-6f) {
            ortho = cross({0.0f, 0.0f, 1.0f}, u);
        }
        ortho = normalize_safe(ortho);
        return {ortho.x, ortho.y, ortho.z, 0.0f};
    }
    const Vec3 c = cross(u, v);
    return normalize({c.x, c.y, c.z, 1.0f + d});
}

[[nodiscard]] inline float angle_between(Quat a, Quat b) noexcept {
    Quat qa = normalize(a);
    Quat qb = normalize(b);
    if (dot(qa, qb) < 0.0f) {
        qb = {-qb.x, -qb.y, -qb.z, -qb.w};
    }
    const float dx = qa.x - qb.x;
    const float dy = qa.y - qb.y;
    const float dz = qa.z - qb.z;
    const float dw = qa.w - qb.w;
    const float half_chord = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
    return 4.0f * std::asin(std::clamp(half_chord, 0.0f, 1.0f));
}

/** Row-major 3x4 affine transform matching `SkinningFrame::bone_matrices[i]` (`§9.8`). */
struct Mat3x4RowMajor {
    std::array<float, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

    static Mat3x4RowMajor from_trs(Vec3 t, Quat q_in, Vec3 s = {1.0f, 1.0f, 1.0f}) noexcept {
        const Quat q = normalize(q_in);
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat3x4RowMajor r{};
        r.m[0] = (1.0f - 2.0f * (yy + zz)) * s.x;
        r.m[1] = 2.0f * (xy - wz) * s.y;
        r.m[2] = 2.0f * (xz + wy) * s.z;
        r.m[3] = t.x;
        r.m[4] = 2.0f * (xy + wz) * s.x;
        r.m[5] = (1.0f - 2.0f * (xx + zz)) * s.y;
        r.m[6] = 2.0f * (yz - wx) * s.z;
        r.m[7] = t.y;
        r.m[8] = 2.0f * (xz - wy) * s.x;
        r.m[9] = 2.0f * (yz + wx) * s.y;
        r.m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;
        r.m[11] = t.z;
        return r;
    }

    [[nodiscard]] Vec3 transform_point(Vec3 p) const noexcept {
        return {
            m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11],
        };
    }

    [[nodiscard]] Vec3 transform_dir(Vec3 v) const noexcept {
        return {
            m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[4] * v.x + m[5] * v.y + m[6] * v.z,
            m[8] * v.x + m[9] * v.y + m[10] * v.z,
        };
    }

    [[nodiscard]] Vec3 translation() const noexcept { return {m[3], m[7], m[11]}; }
};

[[nodiscard]] inline Mat3x4RowMajor mul_affine(const Mat3x4RowMajor& a, const Mat3x4RowMajor& b) noexcept {
    Mat3x4RowMajor r{};
    for (int row = 0; row < 3; ++row) {
        const int o = row * 4;
        for (int col = 0; col < 3; ++col) {
            r.m[o + col] = a.m[o + 0] * b.m[0 + col] + a.m[o + 1] * b.m[4 + col] + a.m[o + 2] * b.m[8 + col];
        }
        r.m[o + 3] = a.m[o + 0] * b.m[3] + a.m[o + 1] * b.m[7] + a.m[o + 2] * b.m[11] + a.m[o + 3];
    }
    return r;
}

/** Extracts quaternion from upper-3x3 orthonormal rotation columns of `Mat3x4RowMajor`. */
[[nodiscard]] inline Quat mat3x4_to_quat(const Mat3x4RowMajor& a) noexcept {
    const float m00 = a.m[0], m01 = a.m[1], m02 = a.m[2];
    const float m10 = a.m[4], m11 = a.m[5], m12 = a.m[6];
    const float m20 = a.m[8], m21 = a.m[9], m22 = a.m[10];
    const float tr = m00 + m11 + m22;
    Quat q{};
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return normalize(q);
}

/** Appendix A canonical humanoid bone indices (0..54). */
namespace bone {
inline constexpr uint16_t Hips = 0;
inline constexpr uint16_t Spine = 1;
inline constexpr uint16_t Chest = 2;
inline constexpr uint16_t UpperChest = 3;
inline constexpr uint16_t Neck = 4;
inline constexpr uint16_t Head = 5;
inline constexpr uint16_t LeftEye = 6;
inline constexpr uint16_t RightEye = 7;
inline constexpr uint16_t Jaw = 8;
inline constexpr uint16_t LeftUpperLeg = 9;
inline constexpr uint16_t LeftLowerLeg = 10;
inline constexpr uint16_t LeftFoot = 11;
inline constexpr uint16_t LeftToes = 12;
inline constexpr uint16_t RightUpperLeg = 13;
inline constexpr uint16_t RightLowerLeg = 14;
inline constexpr uint16_t RightFoot = 15;
inline constexpr uint16_t RightToes = 16;
inline constexpr uint16_t LeftShoulder = 17;
inline constexpr uint16_t LeftUpperArm = 18;
inline constexpr uint16_t LeftLowerArm = 19;
inline constexpr uint16_t LeftHand = 20;
inline constexpr uint16_t RightShoulder = 21;
inline constexpr uint16_t RightUpperArm = 22;
inline constexpr uint16_t RightLowerArm = 23;
inline constexpr uint16_t RightHand = 24;
inline constexpr uint16_t LeftThumbMetacarpal = 25;
inline constexpr uint16_t RightThumbMetacarpal = 40;
inline constexpr uint16_t FingerEnd = 55;
} // namespace bone

/** Expected canonical Appendix A parent index (`-1` = hips root). */
inline constexpr std::array<int8_t, AETHER_HUMANOID_BONE_COUNT> kCanonicalParent = {{
    -1, 0,  1,  2,  3, 4, 5, 5, 5, // 0..8
    0,  9,  10, 11,                // 9..12 left leg
    0,  13, 14, 15,                // 13..16 right leg
    3,  17, 18, 19,                // 17..20 left arm
    3,  21, 22, 23,                // 21..24 right arm
    20, 25, 26,                    // 25..27 left thumb
    20, 28, 29,                    // 28..30 left index
    20, 31, 32,                    // 31..33 left middle
    20, 34, 35,                    // 34..36 left ring
    20, 37, 38,                    // 37..39 left little
    24, 40, 41,                    // 40..42 right thumb
    24, 43, 44,                    // 43..45 right index
    24, 46, 47,                    // 46..48 right middle
    24, 49, 50,                    // 49..51 right ring
    24, 52, 53,                    // 52..54 right little
}};

using AetherHumanoidBone = uint16_t;

} // namespace aether::animation

#ifndef AETHER_BONE_HIPS
#define AETHER_BONE_HIPS 0U
#define AETHER_BONE_SPINE 1U
#define AETHER_BONE_CHEST 2U
#define AETHER_BONE_UPPER_CHEST 3U
#define AETHER_BONE_NECK 4U
#define AETHER_BONE_HEAD 5U
#define AETHER_BONE_LEFT_EYE 6U
#define AETHER_BONE_RIGHT_EYE 7U
#define AETHER_BONE_JAW 8U
#define AETHER_BONE_LEFT_UPPER_LEG 9U
#define AETHER_BONE_LEFT_LOWER_LEG 10U
#define AETHER_BONE_LEFT_FOOT 11U
#define AETHER_BONE_LEFT_TOES 12U
#define AETHER_BONE_RIGHT_UPPER_LEG 13U
#define AETHER_BONE_RIGHT_LOWER_LEG 14U
#define AETHER_BONE_RIGHT_FOOT 15U
#define AETHER_BONE_RIGHT_TOES 16U
#define AETHER_BONE_LEFT_SHOULDER 17U
#define AETHER_BONE_LEFT_UPPER_ARM 18U
#define AETHER_BONE_LEFT_LOWER_ARM 19U
#define AETHER_BONE_LEFT_HAND 20U
#define AETHER_BONE_RIGHT_SHOULDER 21U
#define AETHER_BONE_RIGHT_UPPER_ARM 22U
#define AETHER_BONE_RIGHT_LOWER_ARM 23U
#define AETHER_BONE_RIGHT_HAND 24U
#define AETHER_BONE_LEFT_THUMB_PROXIMAL 25U
#define AETHER_BONE_LEFT_INDEX_PROXIMAL 28U
#define AETHER_BONE_LEFT_LITTLE_DISTAL 39U
#define AETHER_BONE_RIGHT_THUMB_PROXIMAL 40U
#define AETHER_BONE_RIGHT_INDEX_PROXIMAL 43U
#define AETHER_BONE_RIGHT_LITTLE_DISTAL 54U
#endif
