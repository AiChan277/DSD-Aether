#include "tracking/blink_gaze_filter.hpp"

#include <algorithm>
#include <cmath>

namespace aether::tracking {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

[[nodiscard]] float percentile85_stack(const std::array<float, BlinkGazeStabilizer::kEarWindowSize>& buf,
                                       uint32_t count) noexcept {
    if (count == 0U) {
        return 0.28f;
    }
    std::array<float, BlinkGazeStabilizer::kEarWindowSize> scratch{};
    const size_t n = std::min<size_t>(count, scratch.size());
    for (size_t i = 0; i < n; ++i) {
        scratch[i] = buf[i];
    }
    const long rounded = std::lround(static_cast<float>(n - 1U) * 0.85f);
    const size_t k = std::min<size_t>(n - 1U, rounded > 0L ? static_cast<size_t>(rounded) : 0U);
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<ptrdiff_t>(k),
                     scratch.begin() + static_cast<ptrdiff_t>(n));
    return scratch[k];
}

} // namespace

void BlinkGazeStabilizer::EarTracker::reset(float hint) noexcept {
    open_window.fill(0.0f);
    count = 0U;
    write_idx = 0U;
    open_baseline = std::clamp(std::isfinite(hint) ? hint : 0.28f, 0.16f, 0.44f);
    closed_floor = std::clamp(open_baseline * 0.32f, 0.05f, 0.13f);
    floor_seeded = false;
}

float BlinkGazeStabilizer::EarTracker::evaluate_openness(float ear_raw, float pitch_down_rad, float hint) noexcept {
    const float safe_hint = std::clamp(std::isfinite(hint) ? hint : 0.28f, 0.16f, 0.44f);
    if (!floor_seeded) {
        open_baseline = safe_hint;
        closed_floor = std::clamp(safe_hint * 0.32f, 0.05f, 0.13f);
        floor_seeded = true;
    }
    const float pitch_comp = BlinkGazeStabilizer::kPitchCompPerRad * std::clamp(pitch_down_rad, 0.0f, 0.70f);
    const float safe_raw = std::isfinite(ear_raw) ? ear_raw : open_baseline;
    const float ear_corr = std::clamp(safe_raw + pitch_comp, 0.02f, 0.65f);

    // Protect open-eye 85th percentile from being dragged down during prolonged eye closure or winks
    if (count < 8U || ear_corr >= std::max(0.14f, 0.62f * open_baseline)) {
        open_window[write_idx] = ear_corr;
        write_idx = (write_idx + 1U) % static_cast<uint32_t>(kEarWindowSize);
        if (count < static_cast<uint32_t>(kEarWindowSize)) {
            ++count;
        }
        if (count >= 4U) {
            const float p85 = percentile85_stack(open_window, count);
            open_baseline = std::clamp(p85, 0.16f, 0.44f);
        }
    }

    if (ear_corr < 0.58f * open_baseline) {
        const float target_floor = std::clamp(ear_corr, 0.04f, std::max(0.05f, open_baseline - 0.08f));
        closed_floor = 0.85f * closed_floor + 0.15f * target_floor;
    }
    closed_floor = std::min(closed_floor, open_baseline - 0.08f);

    const float denom = std::max(open_baseline - closed_floor, 0.08f);
    return std::clamp((ear_corr - closed_floor) / denom, 0.0f, 1.0f);
}

namespace {

[[nodiscard]] float smooth_damp_c2(float current, float target, float& velocity, float smooth_time_s,
                                   float dt_s) noexcept {
    const float st = std::max(0.0005f, smooth_time_s);
    const float omega = 2.0f / st;
    const float x = omega * dt_s;
    const float exp_f = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = current - target;
    const float temp = (velocity + omega * change) * dt_s;
    velocity = (velocity - omega * temp) * exp_f;
    float out = target + (change + temp) * exp_f;
    if ((target - current > 0.0f) == (out > target)) {
        out = target;
        velocity = (out - target) / std::max(dt_s, 1e-4f);
    }
    return out;
}

} // namespace

void BlinkGazeStabilizer::ChannelState::reset() noexcept {
    phase = BlinkPhase::Open;
    phase_time = 0.0f;
    blink = 0.0f;
    blink_vel = 0.0f;
    prev_open = 1.0f;
    has_prev = false;
}

void BlinkGazeStabilizer::ChannelState::step(float open_raw, float dt_s) noexcept {
    const float safe_dt = std::clamp(dt_s, 1.0f / 240.0f, 0.25f);
    // Wide-Open Lock Zone (> 0.68): snap openness to 1.0 before velocity checks so micro-jitter never triggers blinks.
    const float open_locked = (open_raw > BlinkGazeStabilizer::kWideOpenLock) ? 1.0f : std::clamp(open_raw, 0.0f, 1.0f);
    const float vel = has_prev ? ((open_locked - prev_open) / safe_dt) : 0.0f;
    prev_open = open_locked;
    has_prev = true;

    const bool fast_trigger =
        (open_locked < BlinkGazeStabilizer::kBlinkTriggerLow) ||
        (vel < BlinkGazeStabilizer::kBlinkVelTriggerPerS && open_locked <= BlinkGazeStabilizer::kWideOpenLock);

    if (phase == BlinkPhase::Open) {
        if (fast_trigger) {
            phase = BlinkPhase::Closing;
            phase_time = safe_dt;
        } else {
            // C2 settle to 0.0 when open so any residual velocity decays smoothly
            if (blink <= 0.002f && std::abs(blink_vel) < 0.05f) {
                blink = 0.0f;
                blink_vel = 0.0f;
            } else {
                const float tail = std::clamp((0.15f - blink) / 0.15f, 0.0f, 1.0f);
                const float tau_open = BlinkGazeStabilizer::kOpeningSpringTauS * (1.0f + 0.65f * tail);
                blink = std::clamp(smooth_damp_c2(blink, 0.0f, blink_vel, tau_open, safe_dt), 0.0f, 1.0f);
            }
            return;
        }
    } else if (phase == BlinkPhase::Opening && fast_trigger && phase_time >= 0.020f) {
        // Immediate re-blink trigger preserving continuous velocity (C2 smooth reversal)
        phase = BlinkPhase::Closing;
        phase_time = safe_dt;
    } else {
        phase_time += safe_dt;
    }

    if (phase == BlinkPhase::Closing) {
        blink = std::clamp(smooth_damp_c2(blink, 1.0f, blink_vel, BlinkGazeStabilizer::kClosingSpringTauS, safe_dt),
                           0.0f, 1.0f);
        if ((blink >= 0.96f && phase_time >= BlinkGazeStabilizer::kClosingDurS) || blink >= 0.992f) {
            phase = BlinkPhase::ClosedHold;
            phase_time = 0.0f;
            blink = 1.0f;
            blink_vel = 0.0f;
        } else {
            return;
        }
    }

    if (phase == BlinkPhase::ClosedHold) {
        blink = 1.0f;
        blink_vel = 0.0f;
        if (phase_time >= BlinkGazeStabilizer::kClosedHoldDurS && open_raw >= 0.45f) {
            phase = BlinkPhase::Opening;
            phase_time = 0.0f;
        } else {
            return;
        }
    }

    if (phase == BlinkPhase::Opening) {
        // Dual-stiffness opening spring (55 ms) with soft cushion over the final 15% of lid recovery
        const float tail = std::clamp((0.15f - blink) / 0.15f, 0.0f, 1.0f);
        const float tau_open = BlinkGazeStabilizer::kOpeningSpringTauS * (1.0f + 0.20f * tail);
        blink = std::clamp(smooth_damp_c2(blink, 0.0f, blink_vel, tau_open, safe_dt), 0.0f, 1.0f);
        if (blink <= 0.006f && phase_time >= BlinkGazeStabilizer::kOpeningDurS) {
            phase = BlinkPhase::Open;
            phase_time = 0.0f;
            blink = 0.0f;
            blink_vel = 0.0f;
        }
    }
}

void BlinkGazeStabilizer::reset() noexcept {
    ear_l_.reset(0.28f);
    ear_r_.reset(0.28f);
    ch_l_.reset();
    ch_r_.reset();
    ch_sync_.reset();
    wink_hold_s_ = 0.0f;
    gaze_x_ = 0.0f;
    gaze_y_ = 0.0f;
    gaze_vel_x_ = 0.0f;
    gaze_vel_y_ = 0.0f;
    gaze_anchor_x_ = 0.0f;
    gaze_anchor_y_ = 0.0f;
    squint_l_ = 0.0f;
    squint_r_ = 0.0f;
    cheek_l_ = 0.0f;
    cheek_r_ = 0.0f;
    gaze_saccade_active_ = false;
    has_prev_yaw_ = false;
    prev_yaw_rad_ = 0.0f;
}

BlinkGazeOutput BlinkGazeStabilizer::update(const BlinkGazeInput& in) noexcept {
    BlinkGazeOutput out{};
    const float safe_dt = std::clamp(in.dt_s, 1.0f / 240.0f, 0.25f);
    if (in.roi_reset || !in.has_landmarks) {
        ch_l_.reset();
        ch_r_.reset();
        ch_sync_.reset();
        wink_hold_s_ = 0.0f;
        gaze_x_ = 0.0f;
        gaze_y_ = 0.0f;
        gaze_vel_x_ = 0.0f;
        gaze_vel_y_ = 0.0f;
        gaze_anchor_x_ = 0.0f;
        gaze_anchor_y_ = 0.0f;
        squint_l_ = 0.0f;
        squint_r_ = 0.0f;
        cheek_l_ = 0.0f;
        cheek_r_ = 0.0f;
        gaze_saccade_active_ = false;
        has_prev_yaw_ = false;
        return out;
    }

    const float open_l = ear_l_.evaluate_openness(in.ear_left, in.pitch_down_rad, in.neutral_ear_left_hint);
    const float open_r = ear_r_.evaluate_openness(in.ear_right, in.pitch_down_rad, in.neutral_ear_right_hint);
    out.open_left = open_l;
    out.open_right = open_r;
    out.ear_open_base_l = ear_l_.open_baseline;
    out.ear_open_base_r = ear_r_.open_baseline;
    out.ear_closed_floor_l = ear_l_.closed_floor;
    out.ear_closed_floor_r = ear_r_.closed_floor;

    // Bilateral Blink Link unless deliberate sustained one-eye wink (> 0.48 diff for > 120 ms at |Yaw| < 15 deg)
    const bool facing_cam = std::abs(in.head_yaw_rad) < (kWinkMaxYawDeg * kDegToRad);
    const float diff = std::abs(open_l - open_r);
    if (facing_cam && diff > kWinkDiffThreshold) {
        wink_hold_s_ = std::min(wink_hold_s_ + safe_dt, 1.0f);
    } else if (diff < 0.25f || !facing_cam) {
        wink_hold_s_ = 0.0f;
    } else {
        wink_hold_s_ = std::max(0.0f, wink_hold_s_ - 2.0f * safe_dt);
    }

    const bool deliberate_wink = (wink_hold_s_ >= kWinkMinDurationS) && facing_cam && (diff > kWinkDiffThreshold);
    out.wink_active = deliberate_wink;

    // Use near camera-facing eye when head is turned (>= 8 deg) so far-eye foreshortening never biases sync
    const bool head_turned = std::abs(in.head_yaw_rad) >= (8.0f * kDegToRad);
    const float synced_open = head_turned ? ((in.head_yaw_rad >= 0.0f) ? open_r : open_l) : (0.5f * (open_l + open_r));

    ch_l_.step(open_l, safe_dt);
    ch_r_.step(open_r, safe_dt);
    ch_sync_.step(synced_open, safe_dt);

    float raw_blink_l = 0.0f;
    float raw_blink_r = 0.0f;
    if (deliberate_wink) {
        raw_blink_l = ch_l_.blink;
        raw_blink_r = ch_r_.blink;
        out.phase_left = ch_l_.phase;
        out.phase_right = ch_r_.phase;
    } else {
        raw_blink_l = ch_sync_.blink;
        raw_blink_r = ch_sync_.blink;
        out.phase_left = ch_sync_.phase;
        out.phase_right = ch_sync_.phase;
    }

    // Pursuit & Saccadic Iris Gaze Filter: 0.020 deadband + 32 ms C2 spring, recenter during blinks
    const float raw_gx = head_turned ? ((in.head_yaw_rad >= 0.0f) ? in.gaze_gx_right : in.gaze_gx_left)
                                     : (0.5f * (in.gaze_gx_left + in.gaze_gx_right));
    const float raw_gy = head_turned ? ((in.head_yaw_rad >= 0.0f) ? in.gaze_gy_right : in.gaze_gy_left)
                                     : (0.5f * (in.gaze_gy_left + in.gaze_gy_right));
    const float clamped_gx = std::clamp(std::isfinite(raw_gx) ? raw_gx : 0.0f, -1.0f, 1.0f);
    const float clamped_gy = std::clamp(std::isfinite(raw_gy) ? raw_gy : 0.0f, -1.0f, 1.0f);

    const float yaw_rate = has_prev_yaw_ ? (std::abs(in.head_yaw_rad - prev_yaw_rad_) / safe_dt) : 0.0f;
    prev_yaw_rad_ = in.head_yaw_rad;
    has_prev_yaw_ = true;

    const float blink_max = std::max(raw_blink_l, raw_blink_r);
    if (blink_max > 0.5f) {
        gaze_x_ = smooth_damp_c2(gaze_x_, 0.0f, gaze_vel_x_, 0.045f, safe_dt);
        gaze_y_ = smooth_damp_c2(gaze_y_, 0.0f, gaze_vel_y_, 0.045f, safe_dt);
        gaze_anchor_x_ = gaze_x_;
        gaze_anchor_y_ = gaze_y_;
        gaze_saccade_active_ = false;
    } else {
        const float err = std::hypot(clamped_gx - gaze_anchor_x_, clamped_gy - gaze_anchor_y_);
        if (err > kGazeDeadband || yaw_rate >= 0.26f) {
            gaze_saccade_active_ = true;
        }
        if (gaze_saccade_active_) {
            gaze_x_ = smooth_damp_c2(gaze_x_, clamped_gx, gaze_vel_x_, kGazeSpringTauS, safe_dt);
            gaze_y_ = smooth_damp_c2(gaze_y_, clamped_gy, gaze_vel_y_, kGazeSpringTauS, safe_dt);
            gaze_anchor_x_ = gaze_x_;
            gaze_anchor_y_ = gaze_y_;
            if (std::hypot(clamped_gx - gaze_x_, clamped_gy - gaze_y_) <= 0.008f &&
                std::hypot(gaze_vel_x_, gaze_vel_y_) < 0.08f) {
                gaze_x_ = clamped_gx;
                gaze_y_ = clamped_gy;
                gaze_vel_x_ = 0.0f;
                gaze_vel_y_ = 0.0f;
                gaze_anchor_x_ = gaze_x_;
                gaze_anchor_y_ = gaze_y_;
                gaze_saccade_active_ = false;
            }
        } else {
            gaze_x_ = gaze_anchor_x_;
            gaze_y_ = gaze_anchor_y_;
            gaze_vel_x_ = 0.0f;
            gaze_vel_y_ = 0.0f;
        }
    }

    out.gaze_gx = gaze_x_;
    out.gaze_gy = gaze_y_;

    // Live2D Eyelid-Follows-Gaze (-22% EyeOpen on down gaze, +12% eyeWide/brow lift hint on up gaze)
    const float down_gaze = std::max(0.0f, out.gaze_gy);
    const float up_gaze = std::max(0.0f, -out.gaze_gy);
    out.blink_left = std::clamp(raw_blink_l + kGazeDownLidFollow * down_gaze * (1.0f - raw_blink_l), 0.0f, 1.0f);
    out.blink_right = std::clamp(raw_blink_r + kGazeDownLidFollow * down_gaze * (1.0f - raw_blink_r), 0.0f, 1.0f);
    out.brow_lift_from_gaze = kGazeUpLidFollow * up_gaze * (1.0f - blink_max);

    // Separated EyeSquint & CheekRaise smoothing independent of binary/ballistic EyeOpen blinks
    const auto filter_sq = [](float val) noexcept -> float {
        const float c = std::clamp(std::isfinite(val) ? val : 0.0f, 0.0f, 1.0f);
        return (c < 0.03f) ? 0.0f : c;
    };
    const float a_sq = 1.0f - std::exp(-safe_dt / 0.045f);
    const float a_chk = 1.0f - std::exp(-safe_dt / 0.055f);
    squint_l_ += (filter_sq(in.raw_squint_left) * (1.0f - 0.50f * raw_blink_l) - squint_l_) * a_sq;
    squint_r_ += (filter_sq(in.raw_squint_right) * (1.0f - 0.50f * raw_blink_r) - squint_r_) * a_sq;
    cheek_l_ += (std::clamp(in.raw_cheek_squint_left, 0.0f, 1.0f) - cheek_l_) * a_chk;
    cheek_r_ += (std::clamp(in.raw_cheek_squint_right, 0.0f, 1.0f) - cheek_r_) * a_chk;
    out.eye_squint_left = std::clamp(squint_l_, 0.0f, 1.0f);
    out.eye_squint_right = std::clamp(squint_r_, 0.0f, 1.0f);
    out.cheek_raise_left = std::clamp(cheek_l_, 0.0f, 1.0f);
    out.cheek_raise_right = std::clamp(cheek_r_, 0.0f, 1.0f);
    return out;
}

} // namespace aether::tracking
