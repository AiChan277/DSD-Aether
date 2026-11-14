#pragma once

#include "animation/ik_solvers.hpp"
#include "animation/one_euro.hpp"

#include <array>

namespace aether::animation {

enum class IdleAttractorMode : uint8_t {
    Thigh = 0,
    Desk = 1,
};

struct VisibilityFirstConfig {
    float gate_low{0.30f};     // Appendix C: 0.30
    float gate_high{0.75f};    // Appendix C: 0.75
    float slew_per_s{4.0f};    // Appendix C: 4.0 /s (>= 250 ms full transition)
    float breathing_hz{0.22f}; // Ultra-smooth 0.22 Hz optional breathing cycle
    float chest_pitch_rad{1.2f * kDegToRad};
    float shoulder_breath_rad{0.8f * kDegToRad};
    float shoulder_lift_m{0.004f};
    float attractor_lift_m{0.006f};
    float proc_spring_wn{kTwoPi * 1.5f}; // 2*pi*1.5 Hz
    float proc_spring_zeta{1.0f};        // critically damped
    float clavicle_lag_s{0.15f};         // Appendix C: 0.15 s
    float k_sway{0.35f};                 // Appendix C: 0.35
    float foot_step_threshold_m{0.12f};  // Appendix C: 12 cm
    float foot_step_duration_s{0.35f};   // Appendix C: 0.35 s
    float foot_step_apex_m{0.04f};       // Appendix C: 4 cm
    IdleAttractorMode left_arm_idle{IdleAttractorMode::Desk};
    IdleAttractorMode right_arm_idle{IdleAttractorMode::Desk};
    // Natural relaxed idle pose for untracked arms/hands (degrees / radians).
    float idle_arm_drop_deg{68.0f};     // upper arm below horizontal (a stiff A-pose is ~35-40)
    float idle_arm_forward_deg{10.0f};  // arms hang slightly in front of the body
    float idle_elbow_bend_deg{16.0f};   // soft elbow flex
    float idle_wrist_flex_deg{8.0f};    // relaxed wrist
    float idle_finger_curl_rad{0.26f};  // base per-joint finger curl
    float idle_sway_deg{0.45f};         // slow torso sway amplitude (with breathing)
    bool enable_idle_breathing{true};   // Warudo additive idle breathing (0.22 Hz, 1.2 deg chest, 0.8 deg shoulders)
    bool enable_hand_ik{false};         // Hard-bypass hand tracking & arm IK; lock clean relaxed standing arm pose
};

struct FootPlantState {
    Vec3 planted_pos{};
    Vec3 step_start_pos{};
    Vec3 step_target_pos{};
    float step_elapsed_s{0.0f};
    bool stepping{false};
};

struct SolvedBodyPose {
    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> local_rotations{};
    Vec3 hips_pos_ws{0.0f, 0.90f, 0.0f};
    Vec3 left_wrist_target_ws{};
    Vec3 right_wrist_target_ws{};
    Quat left_wrist_rot_ws{Quat::identity()};
    Quat right_wrist_rot_ws{Quat::identity()};
    float left_wrist_track_weight{0.0f};
    float right_wrist_track_weight{0.0f};
    float lower_body_track_weight{0.0f};
    Vec3 left_foot_target_ws{};
    Vec3 right_foot_target_ws{};
    Vec3 head_target_pos_ws{};
    float head_target_weight{0.0f};
    std::array<float, AETHER_HUMANOID_BONE_COUNT> joint_blend_weights{};
    std::array<bool, 2> foot_step_triggered_this_tick{};
};

/** Evaluates Hermite C1 smoothstep weight (`t^2 * (3 - 2t)`) from `c in [0.30, 0.75]`. */
[[nodiscard]] float eval_visibility_hermite(float confidence, float c_low = 0.30f, float c_high = 0.75f) noexcept;

/** Evaluates slew-limited visibility weight (`|w - w_prev| <= max_slew * dt`). */
[[nodiscard]] float step_visibility_weight(float prev_w, float confidence, float dt_s,
                                           const VisibilityFirstConfig& cfg = {}) noexcept;

/**
 * Visibility-First confidence-gated procedural & tracking posture solver (`§9.4`).
 * Keeps out-of-frame limbs anchored smoothly at breathing-coupled thigh/desk attractors,
 * drives hips sway + auto foot planting, and respects graph IK/bone overrides.
 */
class VisibilityFirstSolver {
  public:
    void reset(Vec3 root_hips_pos = {0.0f, 0.90f, 0.0f}, Vec3 l_foot_rest = {0.09f, 0.05f, 0.0f},
               Vec3 r_foot_rest = {-0.09f, 0.05f, 0.0f}) noexcept;

    /** Updates rest-pose hips & foot anchors when switching avatars while preserving filter weights. */
    void set_rest_anchors(Vec3 root_hips_pos, Vec3 l_foot_rest, Vec3 r_foot_rest) noexcept;

    /** Sets full avatar rest metrics (`rest_local_rotations`, `rest_world_rotations`) for relative deltas. */
    void set_rest_metrics(const RestSkeletonMetrics& metrics) noexcept { rest_metrics_ = metrics; }

    void solve(const FilteredTrackingState& tracked, const AetherGraphOutputs& graph_out, float dt_s,
               const VisibilityFirstConfig& cfg, SolvedBodyPose& out) noexcept;

    [[nodiscard]] Vec3 left_wrist_attractor_pos() const noexcept { return l_wrist_pos_; }
    [[nodiscard]] Vec3 right_wrist_attractor_pos() const noexcept { return r_wrist_pos_; }
    [[nodiscard]] Vec3 left_wrist_attractor_vel() const noexcept { return l_wrist_vel_; }
    [[nodiscard]] Vec3 right_wrist_attractor_vel() const noexcept { return r_wrist_vel_; }

  private:
    float phase_time_s_{0.0f};
    Vec3 rest_hips_pos_{0.0f, 0.90f, 0.0f};
    std::array<float, AETHER_HUMANOID_BONE_COUNT> prev_weights_{};

    // Critically damped wrist procedural springs + clavicle secondary lag
    Vec3 l_wrist_pos_{};
    Vec3 l_wrist_vel_{};
    Vec3 r_wrist_pos_{};
    Vec3 r_wrist_vel_{};
    float l_clavicle_lag_{0.0f};
    float r_clavicle_lag_{0.0f};

    // 60 Hz analytical critically-damped kinematic chain springs (Spine -> Chest -> Neck -> Head)
    struct SpringVec3State {
        Vec3 pos{};
        Vec3 vel{};
    };
    SpringVec3State spine_rot_spring_{};
    SpringVec3State chest_rot_spring_{};
    SpringVec3State neck_rot_spring_{};
    SpringVec3State head_rot_spring_{};
    SpringVec3State hips_lean_spring_{};
    SpringVec3State chest_lean_spring_{};
    Vec3 prev_target_head_rotvec_{};
    Vec3 head_vel_ema_rad_s_{};
    bool has_prev_target_head_{false};
    RestSkeletonMetrics rest_metrics_{build_default_humanoid_rest_metrics()};

    std::array<Quat, AETHER_HUMANOID_BONE_COUNT> proc_rot_smooth_{};
    std::array<FootPlantState, 2> feet_{};
    bool initialized_{false};
};

} // namespace aether::animation
