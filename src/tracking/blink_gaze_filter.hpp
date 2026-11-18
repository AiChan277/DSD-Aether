#pragma once

#include <array>
#include <cstdint>

namespace aether::tracking {

enum class BlinkPhase : uint8_t {
    Open = 0,
    Closing = 1,
    ClosedHold = 2,
    Opening = 3,
};

struct BlinkGazeInput {
    float ear_left{0.28f};
    float ear_right{0.28f};
    float gaze_gx_left{0.0f};   // [-1, +1] normalized horizontal iris offset (+ image-right)
    float gaze_gy_left{0.0f};   // [-1, +1] normalized vertical iris offset (+ looking down)
    float gaze_gx_right{0.0f};
    float gaze_gy_right{0.0f};
    float pitch_down_rad{0.0f}; // Positive when head pitches downward (+pitch in LandmarkPoseSolver)
    float head_yaw_rad{0.0f};   // Yaw in camera coords (+ right)
    float dt_s{1.0f / 30.0f};
    float neutral_ear_left_hint{0.28f};
    float neutral_ear_right_hint{0.28f};
    float raw_squint_left{0.0f};
    float raw_squint_right{0.0f};
    float raw_cheek_squint_left{0.0f};
    float raw_cheek_squint_right{0.0f};
    bool has_landmarks{true};
    bool roi_reset{false};
};

struct BlinkGazeOutput {
    float blink_left{0.0f};     // [0, 1] (0 = wide open, 1 = fully shut)
    float blink_right{0.0f};    // [0, 1]
    float open_left{1.0f};      // Normalized pitch-compensated openness before lock/envelope
    float open_right{1.0f};
    float ear_open_base_l{0.28f};
    float ear_open_base_r{0.28f};
    float ear_closed_floor_l{0.09f};
    float ear_closed_floor_r{0.09f};
    float gaze_gx{0.0f};        // Stabilized bilateral horizontal gaze [-1, +1]
    float gaze_gy{0.0f};        // Stabilized bilateral vertical gaze [-1, +1] (+ looking down)
    float eye_squint_left{0.0f};
    float eye_squint_right{0.0f};
    float cheek_raise_left{0.0f};
    float cheek_raise_right{0.0f};
    float brow_lift_from_gaze{0.0f};
    BlinkPhase phase_left{BlinkPhase::Open};
    BlinkPhase phase_right{BlinkPhase::Open};
    bool wink_active{false};
};

/**
 * Warudo / VTube Studio Dual-Stiffness C2 Eyelid Spring + Saccadic Iris Gaze Stabilizer.
 * Features pitch-compensated 85th-percentile EAR calibration, Schmitt wide-open lock (> 0.68),
 * C2 dual-stiffness eyelid spring (22 ms Closing / 55 ms Opening + final-15% deceleration cushion),
 * Live2D Eyelid-Follows-Gaze (-22% down gaze, +12% up-gaze lift), separated squint/cheek raise,
 * Bilateral Blink Link (120 ms deliberate wink gate), and 0.02 deadband + 32 ms pursuit/saccade gaze spring.
 * Strictly zero heap allocations on hot path.
 */
class BlinkGazeStabilizer {
  public:
    static constexpr float kPitchCompPerRad = 0.14f;
    static constexpr float kWideOpenLock = 0.68f;
    static constexpr float kBlinkTriggerLow = 0.42f;
    static constexpr float kBlinkVelTriggerPerS = -3.5f;
    static constexpr float kClosingSpringTauS = 0.022f;
    static constexpr float kOpeningSpringTauS = 0.055f;
    static constexpr float kClosingDurS = 0.035f;
    static constexpr float kClosedHoldDurS = 0.035f;
    static constexpr float kOpeningDurS = 0.085f;
    static constexpr float kGazeDownLidFollow = 0.22f;
    static constexpr float kGazeUpLidFollow = 0.12f;
    static constexpr float kWinkDiffThreshold = 0.48f;
    static constexpr float kWinkMinDurationS = 0.120f;
    static constexpr float kWinkMaxYawDeg = 15.0f;
    static constexpr float kGazeDeadband = 0.020f;
    static constexpr float kGazeSpringTauS = 0.032f;
    static constexpr size_t kEarWindowSize = 90U;

    BlinkGazeStabilizer() noexcept { reset(); }

    void reset() noexcept;

    [[nodiscard]] BlinkGazeOutput update(const BlinkGazeInput& in) noexcept;

  private:
    struct EarTracker {
        std::array<float, kEarWindowSize> open_window{};
        uint32_t count{0};
        uint32_t write_idx{0};
        float open_baseline{0.28f};
        float closed_floor{0.09f};
        bool floor_seeded{false};

        void reset(float hint = 0.28f) noexcept;
        [[nodiscard]] float evaluate_openness(float ear_raw, float pitch_down_rad, float hint) noexcept;
    };

    struct ChannelState {
        BlinkPhase phase{BlinkPhase::Open};
        float phase_time{0.0f};
        float blink{0.0f};
        float blink_vel{0.0f};
        float prev_open{1.0f};
        bool has_prev{false};

        void reset() noexcept;
        void step(float open_raw, float dt_s) noexcept;
    };

    EarTracker ear_l_{};
    EarTracker ear_r_{};
    ChannelState ch_l_{};
    ChannelState ch_r_{};
    ChannelState ch_sync_{};

    float wink_hold_s_{0.0f};
    float gaze_x_{0.0f};
    float gaze_y_{0.0f};
    float gaze_vel_x_{0.0f};
    float gaze_vel_y_{0.0f};
    float gaze_anchor_x_{0.0f};
    float gaze_anchor_y_{0.0f};
    float squint_l_{0.0f};
    float squint_r_{0.0f};
    float cheek_l_{0.0f};
    float cheek_r_{0.0f};
    bool gaze_saccade_active_{false};
    bool has_prev_yaw_{false};
    float prev_yaw_rad_{0.0f};
};

} // namespace aether::tracking
