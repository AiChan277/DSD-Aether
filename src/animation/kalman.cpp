#include "animation/kalman.hpp"

#include <algorithm>
#include <cmath>

namespace aether::animation {

void KalmanScalar2State::reset(float initial_pos) noexcept {
    p_ = initial_pos;
    v_ = 0.0f;
    p00_ = 0.01f;
    p01_ = 0.0f;
    p11_ = 1.0f;
    initialized_ = false;
}

void KalmanScalar2State::correct(float z, float dt_s, float confidence, float sigma_a, float sigma_m,
                                 const KalmanConfig& cfg) noexcept {
    if (!initialized_) {
        p_ = z;
        v_ = 0.0f;
        p00_ = sigma_m * sigma_m;
        p01_ = 0.0f;
        p11_ = 0.25f;
        initialized_ = true;
        return;
    }

    const float dt = std::clamp(dt_s, 1e-4f, 0.2f);
    // Predict state & covariance: x_pred = F x, P_pred = F P F^T + Q
    const float p_pred = p_ + v_ * dt;
    const float v_pred = v_;

    const float dt2 = dt * dt;
    const float dt3 = dt2 * dt;
    const float dt4 = dt2 * dt2;
    const float q_var = sigma_a * sigma_a;

    const float pp00 = p00_ + 2.0f * dt * p01_ + dt2 * p11_ + 0.25f * dt4 * q_var;
    const float pp01 = p01_ + dt * p11_ + 0.5f * dt3 * q_var;
    const float pp11 = p11_ + dt2 * q_var;

    if (confidence < cfg.low_conf_threshold) {
        p_ = p_pred;
        v_ = v_pred * cfg.low_conf_vel_damping;
        p00_ = pp00;
        p01_ = pp01;
        p11_ = pp11;
        return;
    }

    // Measurement update: z = p + noise(R)
    const float conf_scale = 1.0f / std::max(confidence, 0.25f);
    const float r_var = (sigma_m * conf_scale) * (sigma_m * conf_scale);
    const float s = pp00 + r_var;
    const float k0 = pp00 / s;
    const float k1 = pp01 / s;
    const float innov = z - p_pred;

    p_ = p_pred + k0 * innov;
    v_ = v_pred + k1 * innov;
    p00_ = (1.0f - k0) * pp00;
    p01_ = (1.0f - k0) * pp01;
    p11_ = std::max(pp11 - k1 * pp01, 1e-6f);
}

void KalmanQuatPredictor::reset(Quat initial) noexcept {
    q_cur_ = normalize(initial);
    omega_ = {};
    initialized_ = false;
}

void KalmanQuatPredictor::update(Quat q_meas, float dt_s, float confidence, const KalmanConfig& cfg) noexcept {
    Quat qm = normalize(q_meas);
    if (!initialized_) {
        q_cur_ = qm;
        omega_ = {};
        initialized_ = true;
        return;
    }
    if (dot(qm, q_cur_) < 0.0f) {
        qm = {-qm.x, -qm.y, -qm.z, -qm.w};
    }
    if (confidence < cfg.low_conf_threshold) {
        omega_ = omega_ * cfg.low_conf_vel_damping;
        return;
    }
    const float dt = std::clamp(dt_s, 1e-4f, 0.2f);
    const Quat q_diff = qm * conjugate(q_cur_);
    const Vec3 raw_omega = quat_to_rotvec(q_diff) * (1.0f / dt);
    // Blend new angular velocity smoothly and clamp runaway above 20 rad/s
    omega_ = omega_ * 0.35f + raw_omega * 0.65f;
    const float w_len = length(omega_);
    if (w_len > 20.0f) {
        omega_ = omega_ * (20.0f / w_len);
    }
    q_cur_ = qm;
}

Quat KalmanQuatPredictor::predict(float horizon_s, bool roi_reset) const noexcept {
    if (roi_reset || !initialized_) {
        return q_cur_;
    }
    const float h = std::max(0.0f, horizon_s);
    const Quat step = rotvec_to_quat(omega_ * h);
    return normalize(step * q_cur_);
}

void KalmanPosePredictor::reset() noexcept {
    for (auto& f : bs_kf_) {
        f.reset(0.0f);
    }
    for (auto& pos : pos_kf_) {
        for (auto& axis : pos) {
            axis.reset(0.0f);
        }
    }
    for (auto& rk : rot_kf_) {
        rk.reset(Quat::identity());
    }
}

void KalmanPosePredictor::correct(const FilteredTrackingState& meas, const KalmanConfig& cfg) noexcept {
    const bool roi_reset = (meas.valid_flags & 0x10U) != 0U;
    if (roi_reset) {
        for (size_t i = 0; i < 52U; ++i) {
            bs_kf_[i].reset(meas.blendshapes[i]);
            bs_kf_[i].correct(meas.blendshapes[i], meas.dt_capture_s, 1.0f, cfg.sigma_a_bs, cfg.sigma_m_bs, cfg);
        }
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            pos_kf_[b][0].reset(meas.joint_positions_ws[b].x);
            pos_kf_[b][1].reset(meas.joint_positions_ws[b].y);
            pos_kf_[b][2].reset(meas.joint_positions_ws[b].z);
            rot_kf_[b].reset(meas.joint_rotations[b]);
        }
        return;
    }

    const float face_conf = (meas.valid_flags & 0x01U) ? 1.0f : 0.1f;
    for (size_t i = 0; i < 52U; ++i) {
        bs_kf_[i].correct(meas.blendshapes[i], meas.dt_capture_s, face_conf, cfg.sigma_a_bs, cfg.sigma_m_bs, cfg);
    }
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const float conf = std::max(meas.joint_confidence[b], 0.0f);
        pos_kf_[b][0].correct(meas.joint_positions_ws[b].x, meas.dt_capture_s, conf, cfg.sigma_a_pos, cfg.sigma_m_pos,
                              cfg);
        pos_kf_[b][1].correct(meas.joint_positions_ws[b].y, meas.dt_capture_s, conf, cfg.sigma_a_pos, cfg.sigma_m_pos,
                              cfg);
        pos_kf_[b][2].correct(meas.joint_positions_ws[b].z, meas.dt_capture_s, conf, cfg.sigma_a_pos, cfg.sigma_m_pos,
                              cfg);
        rot_kf_[b].update(meas.joint_rotations[b], meas.dt_capture_s, conf, cfg);
    }
}

void KalmanPosePredictor::tick_low_conf_damping(const FilteredTrackingState& latest, const KalmanConfig& cfg) noexcept {
    if ((latest.valid_flags & 0x01U) == 0U) {
        for (auto& bk : bs_kf_) {
            bk.damp_velocity(cfg.low_conf_vel_damping);
        }
    }
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        if (latest.joint_confidence[b] < cfg.low_conf_threshold) {
            pos_kf_[b][0].damp_velocity(cfg.low_conf_vel_damping);
            pos_kf_[b][1].damp_velocity(cfg.low_conf_vel_damping);
            pos_kf_[b][2].damp_velocity(cfg.low_conf_vel_damping);
            rot_kf_[b].damp_velocity(cfg.low_conf_vel_damping);
        }
    }
}

void KalmanPosePredictor::predict(float horizon_s, const FilteredTrackingState& base, FilteredTrackingState& out,
                                  const KalmanConfig& cfg) const noexcept {
    out = base;
    const bool roi_reset = (base.valid_flags & 0x10U) != 0U;
    const float h = roi_reset ? 0.0f : std::clamp(horizon_s, 0.0f, cfg.max_horizon_s);
    for (size_t i = 0; i < 52U; ++i) {
        out.blendshapes[i] = std::clamp(bs_kf_[i].predict(h), 0.0f, 1.0f);
    }
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        out.joint_positions_ws[b] = {
            pos_kf_[b][0].predict(h),
            pos_kf_[b][1].predict(h),
            pos_kf_[b][2].predict(h),
        };
        out.joint_rotations[b] = rot_kf_[b].predict(h, roi_reset);
    }
}

} // namespace aether::animation
