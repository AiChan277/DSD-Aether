#include "animation/one_euro.hpp"
#include "animation/motion_filters.hpp"

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
    q_anchor_ = q_hat_;
    q_vel_ref_ = q_hat_;
    edx_ = Vec3{};
    vel_ema_ = 0.0f;
    for (auto& f : tangent_filters_) {
        f.reset(0.0f);
        f.rebase_tangent_zero();
    }
    initialized_ = false;
}

Quat OneEuroQuat::filter(Quat q, float dt_s, const OneEuroParams& params) noexcept {
    Quat q_norm = normalize(q);
    if (!initialized_ || !(dt_s > 0.0f) || !std::isfinite(dt_s)) {
        q_hat_ = q_norm;
        q_anchor_ = q_norm;
        q_vel_ref_ = q_norm;
        edx_ = Vec3{};
        vel_ema_ = 0.0f;
        for (auto& f : tangent_filters_) {
            f.reset(0.0f);
            f.rebase_tangent_zero();
        }
        initialized_ = true;
        return q_hat_;
    }

    // Hemisphere guard (`§9.2`): align raw, anchor, and velocity reference to q_hat_.
    if (dot(q_norm, q_hat_) < 0.0f) {
        q_norm = {-q_norm.x, -q_norm.y, -q_norm.z, -q_norm.w};
    }
    if (dot(q_anchor_, q_hat_) < 0.0f) {
        q_anchor_ = {-q_anchor_.x, -q_anchor_.y, -q_anchor_.z, -q_anchor_.w};
    }
    if (dot(q_vel_ref_, q_hat_) < 0.0f) {
        q_vel_ref_ = {-q_vel_ref_.x, -q_vel_ref_.y, -q_vel_ref_.z, -q_vel_ref_.w};
    }

    // Task 5: Warudo Asymmetric Velocity-Adaptive 1-Euro + Stationary-Anchor Soft Cubic Deadzone
    if (params.vel_threshold_rad_s > 0.0f) {
        const auto lp_alpha = [&](float cutoff_hz) noexcept {
            const float tau = 1.0f / (kTwoPi * std::max(cutoff_hz, 1e-4f));
            return dt_s / (tau + dt_s);
        };

        // 1. Pre-smooth reference (6 Hz) + vector derivative filter so zero-mean Gaussian noise cancels
        // while real head turns wake up cleanly on Frame 1.
        const float alpha_ref = lp_alpha(6.0f);
        const Vec3 r_vel_step = quat_to_rotvec(normalize(conjugate(q_vel_ref_) * q_norm));
        q_vel_ref_ = normalize(q_vel_ref_ * rotvec_to_quat(r_vel_step * alpha_ref));
        const Vec3 v_inst = r_vel_step * (alpha_ref / dt_s);
        const float alpha_d = lp_alpha(params.dcutoff);
        edx_ = edx_ + (v_inst - edx_) * alpha_d;

        // 2. Asymmetric attack/release speed envelope (10 ms fast onset, 75 ms silky settle).
        const float speed_inst = length(edx_);
        const float alpha_v = (speed_inst > vel_ema_) ? (dt_s / (0.010f + dt_s)) : (dt_s / (0.075f + dt_s));
        vel_ema_ += alpha_v * (speed_inst - vel_ema_);

        // 3. Stationary anchor update + soft cubic deadzone around anchor (< 1.5 deg/s).
        const Vec3 r_anchor = quat_to_rotvec(normalize(conjugate(q_anchor_) * q_norm));
        const float d_anchor = length(r_anchor);
        if (vel_ema_ >= params.vel_threshold_rad_s) {
            const float anchor_alpha = std::clamp(dt_s / (0.030f + dt_s), 0.0f, 1.0f);
            q_anchor_ = slerp(q_anchor_, q_hat_, anchor_alpha);
        } else if (d_anchor > params.deadband) {
            const float creep_alpha = std::clamp(dt_s / (0.20f + dt_s), 0.0f, 1.0f);
            q_anchor_ = slerp(q_anchor_, q_hat_, creep_alpha);
        }

        const float dz_fade =
            1.0f - smoothstep01(params.vel_threshold_rad_s, 2.5f * params.vel_threshold_rad_s, vel_ema_);
        const float dz_eff = params.deadband * dz_fade;
        Quat q_target = q_norm;
        if (dz_eff > 1e-6f && d_anchor > 1e-7f && d_anchor < dz_eff) {
            const float u = d_anchor / dz_eff;
            const float d_mapped = dz_eff * (u * u * u);
            q_target = normalize(q_anchor_ * rotvec_to_quat(r_anchor * (d_mapped / d_anchor)));
        }

        // 4. Velocity-adaptive cutoff: pinned to fc_min below threshold, smoothly ramped above.
        const float v_excess = std::max(0.0f, vel_ema_ - params.vel_threshold_rad_s);
        const float beta_ramp = smoothstep01(0.0f, 3.0f * params.vel_threshold_rad_s, v_excess);
        const float fc = params.fc_min + params.beta * v_excess * beta_ramp;
        q_hat_ = slerp(q_hat_, q_target, lp_alpha(fc));
        return q_hat_;
    }

    // Map r = log(q_hat_prev^-1 * q) into SO(3) tangent space, smooth, then exp back (`§9.2`).
    const Quat q_delta = conjugate(q_hat_) * q_norm;
    const Vec3 r_delta = quat_to_rotvec(q_delta);
    const float ang = length(r_delta);
    OneEuroParams tangent_params = params;
    tangent_params.deadband = 0.0f;
    float scale = 1.0f;
    if (params.deadband > 0.0f && ang < params.deadband) {
        const float u = ang / params.deadband;
        scale = u * u; // d * u^2 = dz * u^3 soft cubic response
    }
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
        return {0.45f, 16.0f, 0.85f, 0.30f * kDegToRad, 1.5f * kDegToRad};
    }
    if (bone_idx == AETHER_BONE_LEFT_EYE || bone_idx == AETHER_BONE_RIGHT_EYE) {
        return {3.0f, 1.00f, 1.0f, 0.0035f, 0.0f};
    }
    // Finger bones: 25..54
    if (bone_idx >= AETHER_BONE_LEFT_THUMB_PROXIMAL) {
        return {1.0f, 1.2f, 0.8f, 0.0040f, 0.0f};
    }
    // Body / arms / shoulders / hips
    return {0.4f, 6.0f, 0.7f, 0.0022f, 0.0f};
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
    for (size_t v = 0; v < out.webcam_visemes.size(); ++v) {
        out.webcam_visemes[v] = face_tracked ? std::clamp(raw.webcam_visemes[v], 0.0f, 1.0f) : 0.0f;
    }

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

    auto is_pre_stabilized_eye_slot = [](size_t s) noexcept -> bool {
        return s == 0U || s == 7U || (s >= 1U && s <= 4U) || (s >= 8U && s <= 11U);
    };
    for (size_t i = 0; i < 52U; ++i) {
        OneEuroParams p = params_for_blendshape(i);
        float bs_target = raw.arkit_blendshapes[i];
        if (!face_tracked) {
            bs_target = 0.0f;
            p.deadband = 0.0f;
            p.fc_min = std::max(p.fc_min, 2.5f);
        } else if (is_pre_stabilized_eye_slot(i)) {
            bs_filters_[i].reset(bs_target);
            out.blendshapes[i] = std::clamp(bs_target, 0.0f, 1.0f);
            continue;
        }
        out.blendshapes[i] = std::clamp(bs_filters_[i].filter(bs_target, dt_s, p), 0.0f, 1.0f);
    }

    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const OneEuroParams base_params = params_for_bone(b);
        bool bone_tracked = false;
        if (b == AETHER_BONE_HEAD || b == AETHER_BONE_NECK || b == AETHER_BONE_LEFT_EYE || b == AETHER_BONE_RIGHT_EYE) {
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
            const OneEuroParams pos_params{0.24f, 7.5f, 0.65f, 0.0006f, 0.0f};
            out.joint_positions_ws[b] = {
                pos_filters_[b][0].filter(p_raw.x, dt_s, pos_params),
                pos_filters_[b][1].filter(p_raw.y, dt_s, pos_params),
                pos_filters_[b][2].filter(p_raw.z, dt_s, pos_params),
            };
            if (b == AETHER_BONE_LEFT_EYE || b == AETHER_BONE_RIGHT_EYE) {
                const Quat q_eye = normalize(q_raw);
                rot_filters_[b].reset(q_eye);
                out.joint_rotations[b] = q_eye;
            } else {
                out.joint_rotations[b] = rot_filters_[b].filter(q_raw, dt_s, base_params);
            }
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
