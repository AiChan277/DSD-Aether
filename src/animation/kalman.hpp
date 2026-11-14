#pragma once

#include "animation/one_euro.hpp"

#include <array>

namespace aether::animation {

struct KalmanConfig {
    float max_horizon_s{0.014f};       // Appendix C: 14 ms
    float sigma_a_pos{8.0f};           // m/s^2
    float sigma_a_bs{35.0f};           // blendshape units/s^2
    float sigma_m_pos{0.002f};         // 2 mm
    float sigma_m_bs{0.02f};           // 0.02 blendshape
    float low_conf_threshold{0.30f};   // runaway guard
    float low_conf_vel_damping{0.85f}; // v <- 0.85 v per tick
};

/** Scalar 2-state `[p, v]` constant-velocity linear Kalman filter (`§9.3`). */
class KalmanScalar2State {
  public:
    void reset(float initial_pos = 0.0f) noexcept;
    void correct(float z, float dt_s, float confidence, float sigma_a, float sigma_m, const KalmanConfig& cfg) noexcept;
    void damp_velocity(float factor) noexcept { v_ *= factor; }
    [[nodiscard]] float predict(float horizon_s) const noexcept { return p_ + v_ * horizon_s; }
    [[nodiscard]] float position() const noexcept { return p_; }
    [[nodiscard]] float velocity() const noexcept { return v_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    float p_{0.0f};
    float v_{0.0f};
    float p00_{0.01f};
    float p01_{0.0f};
    float p11_{1.0f};
    bool initialized_{false};
};

/** Quaternion angular-velocity predictor (`ω = 2 * log(qk * qk-1^-1) / Te`, `§9.3`). */
class KalmanQuatPredictor {
  public:
    void reset(Quat initial = Quat::identity()) noexcept;
    void update(Quat q_meas, float dt_s, float confidence, const KalmanConfig& cfg) noexcept;
    void damp_velocity(float factor) noexcept { omega_ = omega_ * factor; }
    [[nodiscard]] Quat predict(float horizon_s, bool roi_reset) const noexcept;
    [[nodiscard]] Vec3 angular_velocity() const noexcept { return omega_; }

  private:
    Quat q_cur_{Quat::identity()};
    Vec3 omega_{};
    bool initialized_{false};
};

class KalmanPosePredictor {
  public:
    void reset() noexcept;
    void correct(const FilteredTrackingState& meas, const KalmanConfig& cfg = {}) noexcept;
    void tick_low_conf_damping(const FilteredTrackingState& latest, const KalmanConfig& cfg = {}) noexcept;
    void predict(float horizon_s, const FilteredTrackingState& base, FilteredTrackingState& out,
                 const KalmanConfig& cfg = {}) const noexcept;

  private:
    std::array<KalmanScalar2State, 52> bs_kf_{};
    std::array<std::array<KalmanScalar2State, 3>, AETHER_HUMANOID_BONE_COUNT> pos_kf_{};
    std::array<KalmanQuatPredictor, AETHER_HUMANOID_BONE_COUNT> rot_kf_{};
};

} // namespace aether::animation
