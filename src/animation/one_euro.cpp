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
    const float a_d = smoothing_alpha(safe_dt, params.dcutoff);
    const float diff = x - x_hat_;
    if (params.deadband > 0.0f && std::abs(diff) <= params.deadband) {
        dx_hat_ *= (1.0f - a_d);
        return x_hat_;
    }
    const float x_eff = (params.deadband > 0.0f) ? (x - std::copysign(params.deadband, diff)) : x;
    const float dx = (x_eff - x_hat_) / safe_dt;
    dx_hat_ = a_d * dx + (1.0f - a_d) * dx_hat_;
    const float cutoff = params.fc_min + params.beta * std::abs(dx_hat_);
    const float a = smoothing_alpha(safe_dt, cutoff);
    x_hat_ = a * x_eff + (1.0f - a) * x_hat_;
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
    const float ang = length(r_delta);
    OneEuroParams tangent_params = params;
    tangent_params.deadband = 0.0f;
    if (params.deadband > 0.0f && ang <= params.deadband) {
        for (size_t k = 0; k < 3U; ++k) {
            (void)tangent_filters_[k].filter(0.0f, dt_s, tangent_params);
            tangent_filters_[k].rebase_tangent_zero();
        }
        return q_hat_;
    }
    const float scale = (params.deadband > 0.0f && ang > 1e-6f) ? ((ang - params.deadband) / ang) : 1.0f;
    const Vec3 r_eff = r_delta * scale;
    const Vec3 r_filt{
        tangent_filters_[0].filter(r_eff.x, dt_s, tangent_params),
        tangent_filters_[1].filter(r_eff.y, dt_s, tangent_params),
        tangent_filters_[2].filter(r_eff.z, dt_s, tangent_params),
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
        return {2.8f, 0.90f, 1.0f, 0.004f};
    }
    // Jaw / Mouth speech articulation: slots 14..40
    if (slot <= 40U) {
        return {1.8f, 0.60f, 1.0f, 0.004f};
    }
    // Brows / Cheeks / Nose: slots 41..51
    return {1.4f, 0.50f, 1.0f, 0.004f};
}

OneEuroParams OneEuroPoseBank::params_for_bone(size_t bone_idx) noexcept {
    if (bone_idx == AETHER_BONE_HEAD || bone_idx == AETHER_BONE_NECK) {
        return {0.25f, 6.5f, 0.65f, 0.0022f};
    }
    if (bone_idx == AETHER_BONE_LEFT_EYE || bone_idx == AETHER_BONE_RIGHT_EYE) {
        return {3.0f, 1.00f, 1.0f, 0.0035f};
    }
    // Finger bones: 25..54
    if (bone_idx >= AETHER_BONE_LEFT_THUMB_PROXIMAL) {
        return {1.0f, 1.2f, 0.8f, 0.0040f};
    }
    // Body / arms / shoulders / hips
    return {0.4f, 6.0f, 0.7f, 0.0022f};
}

void OneEuroPoseBank::update(const AetherTrackingFrame& raw, FilteredTrackingState& out) noexcept {
    float dt_s = 1.0f / 60.0f;
    if (last_capture_us_ > 0 && raw.capture_timestamp_us > last_capture_us_) {
        dt_s = static_cast<float>(raw.capture_timestamp_us - last_capture_us_) * 1e-6f;
        dt_s = std::clamp(dt_s, 1.0f / 240.0f, 0.2f);
    }
    last_capture_us_ = raw.capture_timestamp_us;

    const bool roi_reset = (raw.valid_flags & 0x10U) != 0U;
    const bool face_tracked = (raw.valid_flags & 0x01U) != 0U;
    const bool body_tracked = (raw.valid_flags & 0x08U) != 0U && raw.upper_body_confidence >= 0.30f;
    const bool lhand_tracked = (raw.valid_flags & 0x02U) != 0U && raw.left_hand_confidence >= 0.30f;
    const bool rhand_tracked = (raw.valid_flags & 0x04U) != 0U && raw.right_hand_confidence >= 0.30f;

    out.capture_timestamp_us = raw.capture_timestamp_us;
    out.inference_complete_us = raw.inference_complete_us;
    out.active_execution_provider = raw.active_execution_provider;
    out.frame_sequence = raw.frame_sequence;
    out.valid_flags = raw.valid_flags;
    out.upper_body_confidence = body_tracked ? raw.upper_body_confidence : 0.0f;
    out.left_hand_confidence = lhand_tracked ? raw.left_hand_confidence : 0.0f;
    out.right_hand_confidence = rhand_tracked ? raw.right_hand_confidence : 0.0f;
    out.lower_body_confidence = (body_tracked && raw.lower_body_confidence >= 0.30f) ? raw.lower_body_confidence : 0.0f;
    out.dt_capture_s = dt_s;

    if (roi_reset) {
        for (size_t i = 0; i < 52U; ++i) {
            const float bs_target = face_tracked ? raw.arkit_blendshapes[i] : 0.0f;
            bs_filters_[i].reset(bs_target);
            OneEuroParams p = params_for_blendshape(i);
            p.deadband = 0.0f;
            out.blendshapes[i] = std::clamp(bs_filters_[i].filter(bs_target, dt_s, p), 0.0f, 1.0f);
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
        OneEuroParams p = params_for_blendshape(i);
        float bs_target = raw.arkit_blendshapes[i];
        if (!face_tracked) {
            bs_target = 0.0f;
            p.deadband = 0.0f;
            p.fc_min = std::max(p.fc_min, 2.5f);
        }
        out.blendshapes[i] = std::clamp(bs_filters_[i].filter(bs_target, dt_s, p), 0.0f, 1.0f);
    }

    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const OneEuroParams base_params = params_for_bone(b);
        bool bone_tracked = false;
        if (b == AETHER_BONE_HEAD || b == AETHER_BONE_NECK || b == AETHER_BONE_LEFT_EYE ||
            b == AETHER_BONE_RIGHT_EYE) {
            bone_tracked = face_tracked;
        } else if (b >= AETHER_BONE_LEFT_THUMB_PROXIMAL && b <= AETHER_BONE_LEFT_LITTLE_DISTAL) {
            bone_tracked = lhand_tracked;
        } else if (b >= AETHER_BONE_RIGHT_THUMB_PROXIMAL && b <= AETHER_BONE_RIGHT_LITTLE_DISTAL) {
            bone_tracked = rhand_tracked;
        } else {
            bone_tracked = body_tracked || (raw.joint_positions_ws[b][3] >= 0.30f);
        }

        const Vec3 p_raw{
            raw.joint_positions_ws[b][0],
            raw.joint_positions_ws[b][1],
            raw.joint_positions_ws[b][2],
        };
        const Quat q_raw{
            raw.joint_rotations_quat[b][0],
            raw.joint_rotations_quat[b][1],
            raw.joint_rotations_quat[b][2],
            raw.joint_rotations_quat[b][3],
        };

        if (bone_tracked) {
            OneEuroParams pos_params = base_params;
            pos_params.deadband = 0.0006f;
            out.joint_positions_ws[b] = {
                pos_filters_[b][0].filter(p_raw.x, dt_s, pos_params),
                pos_filters_[b][1].filter(p_raw.y, dt_s, pos_params),
                pos_filters_[b][2].filter(p_raw.z, dt_s, pos_params),
            };
            out.joint_rotations[b] = rot_filters_[b].filter(q_raw, dt_s, base_params);
            out.joint_confidence[b] = raw.joint_positions_ws[b][3];
        } else {
            // Critically damp untracked joints back to calm neutral rest pose with zero deadband stickiness
            OneEuroParams decay_params = base_params;
            decay_params.fc_min = std::max(base_params.fc_min, 2.2f);
            decay_params.beta = 0.0f;
            decay_params.deadband = 0.0f;
            out.joint_positions_ws[b] = {
                pos_filters_[b][0].filter(p_raw.x, dt_s, decay_params),
                pos_filters_[b][1].filter(p_raw.y, dt_s, decay_params),
                pos_filters_[b][2].filter(p_raw.z, dt_s, decay_params),
            };
            out.joint_rotations[b] = rot_filters_[b].filter(Quat::identity(), dt_s, decay_params);
            out.joint_confidence[b] = 0.0f;
        }
    }
}

} // namespace aether::animation
