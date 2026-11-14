#include "animation/one_euro.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

namespace {

[[nodiscard]] float smoothing_alpha(float dt_s, float cutoff_hz) noexcept {
    const float rc = 1.0f / std::max(kTwoPi * std::max(cutoff_hz, 1e-4f), 1e-4f);
    return 1.0f / (1.0f + rc / std::max(dt_s, 1e-5f));
}

} // namespace

void OneEuroScalar::reset(float initial) noexcept {
    x_hat_ = initial;
    dx_hat_ = 0.0f;
    initialized_ = false;
}

float OneEuroScalar::filter(float x, float dt_s, const OneEuroParams& params) noexcept {
    if (!initialized_) {
        x_hat_ = x;
        dx_hat_ = 0.0f;
        initialized_ = true;
        return x;
    }
    const float safe_dt = std::clamp(dt_s, 1e-4f, 0.25f);
    const float dx = (x - x_hat_) / safe_dt;
    const float a_d = smoothing_alpha(safe_dt, params.dcutoff);
    dx_hat_ = a_d * dx + (1.0f - a_d) * dx_hat_;
    const float cutoff = params.fc_min + params.beta * std::abs(dx_hat_);
    const float a = smoothing_alpha(safe_dt, cutoff);
    x_hat_ = a * x + (1.0f - a) * x_hat_;
    return x_hat_;
}

void OneEuroQuat::reset(Quat initial) noexcept {
    q_hat_ = normalize(initial);
    for (auto& f : tangent_filters_) {
        f.reset(0.0f);
        f.rebase_tangent_zero();
    }
    initialized_ = false;
}

Quat OneEuroQuat::filter(Quat q, float dt_s, const OneEuroParams& params) noexcept {
    Quat q_norm = normalize(q);
    if (!initialized_) {
        q_hat_ = q_norm;
        for (auto& f : tangent_filters_) {
            f.reset(0.0f);
            f.rebase_tangent_zero();
        }
        initialized_ = true;
        return q_hat_;
    }

    // Hemisphere guard (`§9.2`): q . q_prev >= 0
    if (dot(q_norm, q_hat_) < 0.0f) {
        q_norm = {-q_norm.x, -q_norm.y, -q_norm.z, -q_norm.w};
    }

    // Map r = log(q_hat_prev^-1 * q) into SO(3) tangent space, smooth, then exp back (`§9.2`).
    const Quat q_delta = conjugate(q_hat_) * q_norm;
    const Vec3 r_delta = quat_to_rotvec(q_delta);
    const Vec3 r_filt{
        tangent_filters_[0].filter(r_delta.x, dt_s, params),
        tangent_filters_[1].filter(r_delta.y, dt_s, params),
        tangent_filters_[2].filter(r_delta.z, dt_s, params),
    };
    const Quat step_q = rotvec_to_quat(r_filt);
    q_hat_ = normalize(q_hat_ * step_q);

    // Rebase tangent reference to 0 in the updated frame while keeping dx_hat_ initialized and continuous.
    tangent_filters_[0].rebase_tangent_zero();
    tangent_filters_[1].rebase_tangent_zero();
    tangent_filters_[2].rebase_tangent_zero();
    return q_hat_;
}

void OneEuroPoseBank::reset() noexcept {
    last_capture_us_ = 0;
    for (auto& f : bs_filters_) {
        f.reset(0.0f);
    }
    for (auto& p : pos_filters_) {
        for (auto& axis : p) {
            axis.reset(0.0f);
        }
    }
    for (auto& r : rot_filters_) {
        r.reset(Quat::identity());
    }
}

OneEuroParams OneEuroPoseBank::params_for_blendshape(size_t slot) noexcept {
    // Eye blinks & fast eye saccades: slots 0..13 (Appendix B)
    if (slot <= 13U) {
        return {2.8f, 0.90f, 1.0f};
    }
    // Jaw / Mouth speech articulation: slots 14..40
    if (slot <= 40U) {
        return {1.8f, 0.60f, 1.0f};
    }
    // Brows / Cheeks / Nose: slots 41..51
    return {1.4f, 0.50f, 1.0f};
}

OneEuroParams OneEuroPoseBank::params_for_bone(size_t bone_idx) noexcept {
    if (bone_idx == AETHER_BONE_HEAD || bone_idx == AETHER_BONE_NECK) {
        return {0.25f, 6.5f, 0.65f};
    }
    if (bone_idx == AETHER_BONE_LEFT_EYE || bone_idx == AETHER_BONE_RIGHT_EYE) {
        return {3.0f, 1.00f, 1.0f};
    }
    // Finger bones: 25..54
    if (bone_idx >= AETHER_BONE_LEFT_THUMB_PROXIMAL) {
        return {1.0f, 1.2f, 0.8f};
    }
    // Body / arms / shoulders / hips
    return {0.4f, 6.0f, 0.7f};
}

void OneEuroPoseBank::update(const AetherTrackingFrame& raw, FilteredTrackingState& out) noexcept {
    float dt_s = 1.0f / 60.0f;
    if (last_capture_us_ > 0 && raw.capture_timestamp_us > last_capture_us_) {
        dt_s = static_cast<float>(raw.capture_timestamp_us - last_capture_us_) * 1e-6f;
        dt_s = std::clamp(dt_s, 1.0f / 240.0f, 0.2f);
    }
    last_capture_us_ = raw.capture_timestamp_us;

    const bool roi_reset = (raw.valid_flags & 0x10U) != 0U;
    out.capture_timestamp_us = raw.capture_timestamp_us;
    out.inference_complete_us = raw.inference_complete_us;
    out.active_execution_provider = raw.active_execution_provider;
    out.frame_sequence = raw.frame_sequence;
    out.valid_flags = raw.valid_flags;
    out.upper_body_confidence = raw.upper_body_confidence;
    out.left_hand_confidence = raw.left_hand_confidence;
    out.right_hand_confidence = raw.right_hand_confidence;
    out.lower_body_confidence = raw.lower_body_confidence;
    out.dt_capture_s = dt_s;

    if (roi_reset) {
        for (size_t i = 0; i < 52U; ++i) {
            bs_filters_[i].reset(raw.arkit_blendshapes[i]);
            out.blendshapes[i] = bs_filters_[i].filter(raw.arkit_blendshapes[i], dt_s, params_for_blendshape(i));
        }
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            const Vec3 p{
                raw.joint_positions_ws[b][0],
                raw.joint_positions_ws[b][1],
                raw.joint_positions_ws[b][2],
            };
            pos_filters_[b][0].reset(p.x);
            pos_filters_[b][1].reset(p.y);
            pos_filters_[b][2].reset(p.z);
            out.joint_positions_ws[b] = p;

            Quat q{
                raw.joint_rotations_quat[b][0],
                raw.joint_rotations_quat[b][1],
                raw.joint_rotations_quat[b][2],
                raw.joint_rotations_quat[b][3],
            };
            q = normalize(q);
            rot_filters_[b].reset(q);
            out.joint_rotations[b] = q;
            out.joint_confidence[b] = raw.joint_positions_ws[b][3];
        }
        return;
    }

    for (size_t i = 0; i < 52U; ++i) {
        out.blendshapes[i] =
            std::clamp(bs_filters_[i].filter(raw.arkit_blendshapes[i], dt_s, params_for_blendshape(i)), 0.0f, 1.0f);
    }

    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const OneEuroParams params = params_for_bone(b);
        const Vec3 p_raw{
            raw.joint_positions_ws[b][0],
            raw.joint_positions_ws[b][1],
            raw.joint_positions_ws[b][2],
        };
        out.joint_positions_ws[b] = {
            pos_filters_[b][0].filter(p_raw.x, dt_s, params),
            pos_filters_[b][1].filter(p_raw.y, dt_s, params),
            pos_filters_[b][2].filter(p_raw.z, dt_s, params),
        };

        const Quat q_raw{
            raw.joint_rotations_quat[b][0],
            raw.joint_rotations_quat[b][1],
            raw.joint_rotations_quat[b][2],
            raw.joint_rotations_quat[b][3],
        };
        out.joint_rotations[b] = rot_filters_[b].filter(q_raw, dt_s, params);
        out.joint_confidence[b] = raw.joint_positions_ws[b][3];
    }
}

} // namespace aether::animation
