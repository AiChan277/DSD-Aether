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
    const size_t k = std::min(n - 1U, static_cast<size_t>(static_cast<float>(n - 1U) * 0.85f + 0.5f));
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

void BlinkGazeStabilizer::ChannelState::reset() noexcept {
    phase = BlinkPhase::Open;
    phase_time = 0.0f;
    blink = 0.0f;
    start_blink = 0.0f;
    prev_open = 1.0f;
    has_prev = false;
}

void BlinkGazeStabilizer::ChannelState::step(float open_raw, float dt_s) noexcept {
    const float safe_dt = std::clamp(dt_s, 1.0f / 240.0f, 0.25f);
    // Wide-Open Lock Zone (> 0.68): snap openness to 1.0 before velocity checks so micro-jitter never triggers blinks.
    const float open_locked =
        (open_raw > BlinkGazeStabilizer::kWideOpenLock) ? 1.0f : std::clamp(open_raw, 0.0f, 1.0f);
    const float vel = has_prev ? ((open_locked - prev_open) / safe_dt) : 0.0f;
    prev_open = open_locked;
    has_prev = true;

    const bool fast_trigger =
        (open_locked < BlinkGazeStabilizer::kBlinkTriggerLow) ||
        (vel < BlinkGazeStabilizer::kBlinkVelTriggerPerS && open_locked <= BlinkGazeStabilizer::kWideOpenLock);

    if (phase == BlinkPhase::Open) {
        if (fast_trigger) {
            phase = BlinkPhase::Closing;
            start_blink = blink;
            phase_time = safe_dt;
        } else {
            // Wide-Open Lock (> 0.68 -> 0.0) & Schmitt hysteresis across [0.42, 0.68] hold Open at Blink = 0.0
            blink = 0.0f;
            return;
        }
    } else if (phase == BlinkPhase::Opening && fast_trigger && phase_time >= 0.020f) {
        // Immediate re-blink trigger if eyelids drop again mid-recovery
        phase = BlinkPhase::Closing;
        start_blink = blink;
        phase_time = safe_dt;
    } else {
        phase_time += safe_dt;
    }

    if (phase == BlinkPhase::Closing) {
        if (phase_time >= BlinkGazeStabilizer::kClosingDurS) {
            const float carry = phase_time - BlinkGazeStabilizer::kClosingDurS;
            phase = BlinkPhase::ClosedHold;
            phase_time = carry;
            blink = 1.0f;
        } else {
            const float u = std::clamp(phase_time / BlinkGazeStabilizer::kClosingDurS, 0.0f, 1.0f);
            blink = std::clamp(start_blink + (1.0f - start_blink) * (u * u * u), 0.0f, 1.0f);
            return;
        }
    }

    if (phase == BlinkPhase::ClosedHold) {
        blink = 1.0f;
        if (phase_time >= BlinkGazeStabilizer::kClosedHoldDurS && open_raw >= 0.45f) {
            const float carry = phase_time - BlinkGazeStabilizer::kClosedHoldDurS;
            phase = BlinkPhase::Opening;
            phase_time = carry;
        } else {
            return;
        }
    }

    if (phase == BlinkPhase::Opening) {
        if (phase_time >= BlinkGazeStabilizer::kOpeningDurS) {
            phase = BlinkPhase::Open;
            phase_time = 0.0f;
            blink = 0.0f;
        } else {
            // Smooth cubic ease-out openness: 1 - (1 - t)^3 -> blink = (1 - t)^3
            const float u = std::clamp(phase_time / BlinkGazeStabilizer::kOpeningDurS, 0.0f, 1.0f);
            const float inv = 1.0f - u;
            blink = std::clamp(inv * inv * inv, 0.0f, 1.0f);
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
    gaze_anchor_x_ = 0.0f;
    gaze_anchor_y_ = 0.0f;
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
        gaze_anchor_x_ = 0.0f;
        gaze_anchor_y_ = 0.0f;
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

    if (deliberate_wink) {
        out.blink_left = ch_l_.blink;
        out.blink_right = ch_r_.blink;
        out.phase_left = ch_l_.phase;
        out.phase_right = ch_r_.phase;
    } else {
        out.blink_left = ch_sync_.blink;
        out.blink_right = ch_sync_.blink;
        out.phase_left = ch_sync_.phase;
        out.phase_right = ch_sync_.phase;
    }

    // Saccadic Iris Gaze Filter: deadband 0.035, smooth saccade steps with head turns, recenter during blinks
    const float raw_gx = head_turned ? ((in.head_yaw_rad >= 0.0f) ? in.gaze_gx_right : in.gaze_gx_left)
                                     : (0.5f * (in.gaze_gx_left + in.gaze_gx_right));
    const float raw_gy = head_turned ? ((in.head_yaw_rad >= 0.0f) ? in.gaze_gy_right : in.gaze_gy_left)
                                     : (0.5f * (in.gaze_gy_left + in.gaze_gy_right));
    const float clamped_gx = std::clamp(std::isfinite(raw_gx) ? raw_gx : 0.0f, -1.0f, 1.0f);
    const float clamped_gy = std::clamp(std::isfinite(raw_gy) ? raw_gy : 0.0f, -1.0f, 1.0f);

    const float yaw_rate = has_prev_yaw_ ? (std::abs(in.head_yaw_rad - prev_yaw_rad_) / safe_dt) : 0.0f;
    prev_yaw_rad_ = in.head_yaw_rad;
    has_prev_yaw_ = true;

    const float blink_max = std::max(out.blink_left, out.blink_right);
    if (blink_max > 0.5f) {
        // Recenter eyeballs cleanly behind closed eyelids
        const float recenter_a = 1.0f - std::exp(-safe_dt / 0.040f);
        gaze_x_ += (0.0f - gaze_x_) * recenter_a;
        gaze_y_ += (0.0f - gaze_y_) * recenter_a;
        gaze_anchor_x_ = gaze_x_;
        gaze_anchor_y_ = gaze_y_;
        gaze_saccade_active_ = false;
    } else {
        const float err = std::hypot(clamped_gx - gaze_anchor_x_, clamped_gy - gaze_anchor_y_);
        if (err > kGazeDeadband || yaw_rate >= 0.26f) {
            gaze_saccade_active_ = true;
        }
        if (gaze_saccade_active_) {
            const float sac_a = 1.0f - std::exp(-safe_dt / 0.025f);
            gaze_x_ += (clamped_gx - gaze_x_) * sac_a;
            gaze_y_ += (clamped_gy - gaze_y_) * sac_a;
            gaze_anchor_x_ = gaze_x_;
            gaze_anchor_y_ = gaze_y_;
            if (std::hypot(clamped_gx - gaze_x_, clamped_gy - gaze_y_) <= 0.012f) {
                gaze_x_ = clamped_gx;
                gaze_y_ = clamped_gy;
                gaze_anchor_x_ = gaze_x_;
                gaze_anchor_y_ = gaze_y_;
                gaze_saccade_active_ = false;
            }
        } else {
            gaze_x_ = gaze_anchor_x_;
            gaze_y_ = gaze_anchor_y_;
        }
    }

    out.gaze_gx = gaze_x_;
    out.gaze_gy = gaze_y_;
    return out;
}

} // namespace aether::tracking
