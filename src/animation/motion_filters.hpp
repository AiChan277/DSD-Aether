#pragma once

#include "animation/math_quat.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace aether::animation {

// ---------------------------------------------------------------------------------------------------------------
// Warudo / Unity-style SmoothDamp: critically damped spring parameterised by `smooth_time` (omega = 2 / smooth_time).
// Uses the exact analytic decay `exp(-omega * dt)` (Unity's Pade approximation of the same solution), so stepping
// 4 x 1/240 s lands on the same trajectory as 1 x 1/60 s for a constant target. Overshoot is clamped like Unity.
// ---------------------------------------------------------------------------------------------------------------
[[nodiscard]] inline float smooth_damp(float current, float target, float& velocity, float smooth_time,
                                       float dt) noexcept {
    const float st = std::max(smooth_time, 1e-4f);
    const float h = std::clamp(dt, 0.0f, 0.1f);
    if (h <= 0.0f) {
        return current;
    }
    const float omega = 2.0f / st;
    const float decay = std::exp(-omega * h);
    const float change = current - target;
    const float temp = (velocity + omega * change) * h;
    velocity = (velocity - omega * temp) * decay;
    float out = target + (change + temp) * decay;
    if ((target - current > 0.0f) == (out > target)) {
        out = target;
        velocity = 0.0f;
    }
    if (std::abs(out - target) < 1e-7f && std::abs(velocity) < 1e-6f) {
        out = target;
        velocity = 0.0f;
    }
    return out;
}

[[nodiscard]] inline Vec3 smooth_damp(Vec3 current, Vec3 target, Vec3& velocity, float smooth_time,
                                      float dt) noexcept {
    return {
        smooth_damp(current.x, target.x, velocity.x, smooth_time, dt),
        smooth_damp(current.y, target.y, velocity.y, smooth_time, dt),
        smooth_damp(current.z, target.z, velocity.z, smooth_time, dt),
    };
}

/** Warudo smooth times (seconds) for each driven channel. */
struct SmoothTimes {
    float head{0.075f};
    float neck{0.105f};
    float chest{0.145f};
    float spine{0.185f};
    float blink{0.028f};
    float mouth{0.038f};
};

// ---------------------------------------------------------------------------------------------------------------
// Trickster Webcam Tracker 2-stage stabiliser: 3-sample median spike rejection followed by a smooth centre deadzone
// on the frame-to-frame step (`step *= smoothstep(0, deadzone, |step|)` below the deadzone, untouched above it).
// ---------------------------------------------------------------------------------------------------------------
[[nodiscard]] inline constexpr float median3(float a, float b, float c) noexcept {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

[[nodiscard]] inline float smoothstep01(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-9f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class TricksterChannel {
  public:
    void reset(float v) noexcept {
        win_[0] = win_[1] = win_[2] = v;
        out_ = v;
        primed_ = true;
    }

    [[nodiscard]] float push(float v, float deadzone) noexcept {
        if (!primed_) {
            reset(v);
            return v;
        }
        win_[0] = win_[1];
        win_[1] = win_[2];
        win_[2] = v;
        const float med = median3(win_[0], win_[1], win_[2]);
        const float step = med - out_;
        const float mag = std::abs(step);
        out_ += (mag < deadzone) ? step * smoothstep01(0.0f, deadzone, mag) : step;
        return out_;
    }

    [[nodiscard]] float value() const noexcept { return out_; }

  private:
    float win_[3]{};
    float out_{0.0f};
    bool primed_{false};
};

/**
 * Applies the Trickster stabiliser to the face channels of a fresh camera frame (head rotation as a rotation vector
 * in degrees, head translation in metres, the 52 ARKit blendshapes and the 5 webcam vowels). Body/hand channels are
 * left to the per-joint One-Euro filters. Any ROI reset or face loss re-primes the windows so no stale sample leaks.
 */
class TricksterFrameStabilizer {
  public:
    static constexpr float kRotDeadzoneDeg = 0.18f;
    static constexpr float kPosDeadzoneM = 0.0015f;
    static constexpr float kBlendDeadzone = 0.015f;

    void reset() noexcept { primed_ = false; }

    void apply(AetherTrackingFrame& f) noexcept {
        const bool face = (f.valid_flags & 0x01U) != 0U;
        const bool roi_reset = (f.valid_flags & 0x10U) != 0U;
        if (!face || roi_reset) {
            primed_ = false;
            return;
        }
        auto* hq = f.joint_rotations_quat[AETHER_BONE_HEAD];
        const Vec3 rv = quat_to_rotvec(normalize(Quat{hq[0], hq[1], hq[2], hq[3]})) * kRadToDeg;
        auto* hp = f.joint_positions_ws[AETHER_BONE_HEAD];
        const float rv_in[3] = {rv.x, rv.y, rv.z};
        if (!primed_) {
            for (size_t k = 0; k < 3; ++k) {
                rot_[k].reset(rv_in[k]);
                pos_[k].reset(hp[k]);
            }
            for (size_t i = 0; i < bs_.size(); ++i) {
                bs_[i].reset(f.arkit_blendshapes[i]);
            }
            for (size_t i = 0; i < vis_.size(); ++i) {
                vis_[i].reset(f.webcam_visemes[i]);
            }
            primed_ = true;
            return;
        }
        float rv_out[3]{};
        for (size_t k = 0; k < 3; ++k) {
            rv_out[k] = rot_[k].push(rv_in[k], kRotDeadzoneDeg);
            hp[k] = pos_[k].push(hp[k], kPosDeadzoneM);
        }
        const Quat q = rotvec_to_quat(Vec3{rv_out[0], rv_out[1], rv_out[2]} * kDegToRad);
        hq[0] = q.x;
        hq[1] = q.y;
        hq[2] = q.z;
        hq[3] = q.w;
        for (size_t i = 0; i < bs_.size(); ++i) {
            f.arkit_blendshapes[i] = std::clamp(bs_[i].push(f.arkit_blendshapes[i], kBlendDeadzone), 0.0f, 1.0f);
        }
        for (size_t i = 0; i < vis_.size(); ++i) {
            f.webcam_visemes[i] = std::clamp(vis_[i].push(f.webcam_visemes[i], kBlendDeadzone), 0.0f, 1.0f);
        }
    }

  private:
    std::array<TricksterChannel, 3> rot_{};
    std::array<TricksterChannel, 3> pos_{};
    std::array<TricksterChannel, 52> bs_{};
    std::array<TricksterChannel, 5> vis_{};
    bool primed_{false};
};

// ---------------------------------------------------------------------------------------------------------------
// Kalidokit face rig math (calcEyes / stabilizeBlink / calcMouth), pure and allocation-free.
// ---------------------------------------------------------------------------------------------------------------

/** Eye openness from lid-height / corner-width ratio remapped `[0.14, 0.28] -> [0, 1]`.
 *  The ratio is first normalised to the user's calibrated open-eye ratio so different eye shapes reach 1.0. */
[[nodiscard]] inline float kalido_eye_openness(float eye_ratio, float neutral_open_ratio) noexcept {
    const float scale = std::clamp(0.28f / std::max(neutral_open_ratio, 0.05f), 0.65f, 1.6f);
    const float r = eye_ratio * scale;
    return std::clamp((r - 0.14f) / (0.28f - 0.14f), 0.0f, 1.0f);
}

/**
 * Kalidokit `stabilizeBlink`, extended for head yaw.
 * - Facing the camera (`|yaw| < 15 deg`) and eyes nearly equal (`|dL - dR| < 0.25`): both eyes take the average,
 *   so webcam noise never produces asymmetric twitching.
 * - A decisive wink (`>= 0.35` difference) keeps both eyes independent.
 * - In between the two thresholds the eyes blend smoothly from the average toward independence.
 * - With the head turned (`>= 15 deg`) the far eye is foreshortened and unreliable, so the camera-facing eye drives
 *   both (unless winking). `near_is_right` says which eye faces the camera.
 */
inline void kalido_stabilize_blink(float& blink_l, float& blink_r, float head_yaw_rad, bool near_is_right) noexcept {
    const float d = std::abs(blink_l - blink_r);
    if (d >= 0.35f) {
        return;
    }
    if (std::abs(head_yaw_rad) >= 15.0f * kDegToRad) {
        const float near_eye = near_is_right ? blink_r : blink_l;
        blink_l = near_eye;
        blink_r = near_eye;
        return;
    }
    const float avg = 0.5f * (blink_l + blink_r);
    if (d < 0.25f) {
        blink_l = avg;
        blink_r = avg;
        return;
    }
    const float t = smoothstep01(0.25f, 0.35f, d);
    blink_l = avg + (blink_l - avg) * t;
    blink_r = avg + (blink_r - avg) * t;
}

struct MouthVowels {
    float a{0.0f};
    float i{0.0f};
    float u{0.0f};
    float e{0.0f};
    float o{0.0f};
};

/** Kalidokit `calcMouth`: `mouth_y` = inner-lip height / IOD, `mouth_x` = lip-corner width / IOD,
 *  `mouth_x0` = calibrated closed-mouth width / IOD. */
[[nodiscard]] inline MouthVowels kalido_calc_mouth(float mouth_y, float mouth_x, float mouth_x0) noexcept {
    const float r = mouth_x / std::max(mouth_x0, 0.05f);
    MouthVowels v{};
    v.a = std::clamp((mouth_y - 0.04f) * 3.2f, 0.0f, 1.0f) * std::clamp(r, 0.6f, 1.0f);
    v.i = std::clamp((r - 1.05f) * 4.0f, 0.0f, 1.0f) * (1.0f - 0.6f * v.a);
    v.u = std::clamp((0.92f - r) * 4.5f, 0.0f, 1.0f) * std::clamp(mouth_y * 4.0f, 0.2f, 1.0f);
    v.e = std::clamp(mouth_y * 2.2f, 0.0f, 0.7f) * std::clamp((r - 1.0f) * 3.0f, 0.0f, 1.0f);
    v.o = std::clamp((mouth_y - 0.08f) * 3.0f, 0.0f, 1.0f) * std::clamp((0.98f - r) * 3.5f, 0.2f, 1.0f);
    return v;
}

} // namespace aether::animation
