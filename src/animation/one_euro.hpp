#pragma once

#include "aether_ffi_contract.h"
#include "animation/math_quat.hpp"

#include <array>

namespace aether::animation {

struct OneEuroParams {
    float fc_min{1.2f};
    float beta{0.45f};
    float dcutoff{1.0f};
};

/** Scalar 1-Euro low-pass filter (`§9.2`, Casiez et al.) with measured capture `Te`. */
class OneEuroScalar {
  public:
    void reset(float initial = 0.0f) noexcept;
    void rebase_tangent_zero() noexcept {
        x_hat_ = 0.0f;
        initialized_ = true;
    }
    [[nodiscard]] float filter(float x, float dt_s, const OneEuroParams& params) noexcept;
    [[nodiscard]] float value() const noexcept { return x_hat_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    float x_hat_{0.0f};
    float dx_hat_{0.0f};
    bool initialized_{false};
};

/** Tangent-space quaternion 1-Euro filter (`§9.2`) with hemisphere dot guard (`q . q_prev >= 0`). */
class OneEuroQuat {
  public:
    void reset(Quat initial = Quat::identity()) noexcept;
    [[nodiscard]] Quat filter(Quat q, float dt_s, const OneEuroParams& params) noexcept;
    [[nodiscard]] Quat value() const noexcept { return q_hat_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    Quat q_hat_{Quat::identity()};
    std::array<OneEuroScalar, 3> tangent_filters_{};
    bool initialized_{false};
};

struct FilteredTrackingState {
    uint64_t capture_timestamp_us{0};
    uint64_t inference_complete_us{0};
    uint32_t active_execution_provider{AETHER_EP_CPU};
    uint32_t frame_sequence{0};
    uint32_t valid_flags{0};
    float upper_body_confidence{0.0f};
    float left_hand_confidence{0.0f};
    float right_hand_confidence{0.0f};
    float lower_body_confidence{0.0f};
    float dt_capture_s{1.0f / 60.0f};
    std::array<float, 52> blendshapes{};
    std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> joint_positions_ws{};
    std::array<float, AETHER_HUMANOID_BONE_COUNT> joint_confidence{};
    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> joint_rotations{};
};

/** Per-group parameterised 1-Euro filter bank matching §9.2 & Appendix C defaults. */
class OneEuroPoseBank {
  public:
    void reset() noexcept;
    void update(const AetherTrackingFrame& raw, FilteredTrackingState& out) noexcept;

  private:
    [[nodiscard]] static OneEuroParams params_for_blendshape(size_t slot) noexcept;
    [[nodiscard]] static OneEuroParams params_for_bone(size_t bone_idx) noexcept;

    uint64_t last_capture_us_{0};
    std::array<OneEuroScalar, 52> bs_filters_{};
    std::array<std::array<OneEuroScalar, 3>, AETHER_HUMANOID_BONE_COUNT> pos_filters_{};
    std::array<OneEuroQuat, AETHER_HUMANOID_BONE_COUNT> rot_filters_{};
};

} // namespace aether::animation
