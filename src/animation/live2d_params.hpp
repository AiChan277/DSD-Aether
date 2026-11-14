#pragma once

#include "aether_ffi_contract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aether::animation {

/**
 * Standard Live2D Cubism / VTube Studio tracking parameter manifold (`Param*`),
 * driving head/body angles, eyes, eyebrows, and the orthogonal 2D mouth grid -> `A, I, U, E, O`.
 */
struct Live2DParameters {
    // Core Live2D Head & Body angles (degrees)
    float param_angle_x{0.0f};      // [-30, +30] Yaw
    float param_angle_y{0.0f};      // [-30, +30] Pitch
    float param_angle_z{0.0f};      // [-30, +30] Roll
    float param_body_angle_x{0.0f}; // [-10, +10]
    float param_body_angle_y{0.0f}; // [-10, +10]
    float param_body_angle_z{0.0f}; // [-10, +10]

    // Eye openness & eyeball gaze
    float param_eye_l_open{1.0f}; // [0, 1]
    float param_eye_r_open{1.0f}; // [0, 1]
    float param_eye_ball_x{0.0f}; // [-1, +1]
    float param_eye_ball_y{0.0f}; // [-1, +1]

    // Full Live2D eyebrow parameter set
    float param_brow_l_y{0.0f};     // [-1, +1] vertical height
    float param_brow_r_y{0.0f};     // [-1, +1] vertical height
    float param_brow_l_angle{0.0f}; // [-1, +1] inner-vs-outer slant (+ arched/sad, - furrowed/angry)
    float param_brow_r_angle{0.0f}; // [-1, +1]
    float param_brow_l_form{0.0f};  // [-1, +1] shape curvature (- furrowed/frown, + lifted/cheerful)
    float param_brow_r_form{0.0f};  // [-1, +1]

    // 2D orthogonal mouth manifold -> continuous A, I, U, E, O
    float param_mouth_form{0.0f};   // [-1, +1] (-1 pucker/oo .. 0 neutral .. +1 smile)
    float param_mouth_open_y{0.0f}; // [0, 1]   (closed gate < 0.06 -> 0)

    // Resulting normalized vowel blend weights (sum <= 1.0)
    float vowel_a{0.0f};
    float vowel_i{0.0f};
    float vowel_u{0.0f};
    float vowel_e{0.0f};
    float vowel_o{0.0f};
};

[[nodiscard]] inline float live2d_smoothstep(float edge0, float edge1, float x) noexcept {
    const float denom = edge1 - edge0;
    if (std::abs(denom) < 1e-6f) {
        return (x >= edge1) ? 1.0f : 0.0f;
    }
    const float t = std::clamp((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/**
 * Evaluates the continuous Live2D 2D vowel manifold (`ParamMouthForm` x `ParamMouthOpenY` -> `A, I, U, E, O`).
 * Closed-mouth noise gate (`open_y < 0.06`) forces all vowels cleanly to `0.0`.
 */
inline void solve_live2d_vowels(float mouth_form, float mouth_open_y, float& out_a, float& out_i, float& out_u,
                                float& out_e, float& out_o) noexcept {
    const float open_y = std::clamp(std::isfinite(mouth_open_y) ? mouth_open_y : 0.0f, 0.0f, 1.0f);
    const float form = std::clamp(std::isfinite(mouth_form) ? mouth_form : 0.0f, -1.0f, 1.0f);
    if (open_y < 0.06f) {
        out_a = 0.0f;
        out_i = 0.0f;
        out_u = 0.0f;
        out_e = 0.0f;
        out_o = 0.0f;
        return;
    }

    float a = live2d_smoothstep(0.25f, 0.85f, open_y) * live2d_smoothstep(-0.35f, 0.15f, form);
    float i = live2d_smoothstep(0.05f, 0.30f, open_y) * (1.0f - live2d_smoothstep(0.35f, 0.65f, open_y)) *
              live2d_smoothstep(0.20f, 0.85f, form);
    float u = live2d_smoothstep(0.06f, 0.45f, open_y) * (1.0f - live2d_smoothstep(0.45f, 0.80f, open_y)) *
              live2d_smoothstep(-0.10f, -0.75f, form);
    float e = live2d_smoothstep(0.12f, 0.45f, open_y) * (1.0f - live2d_smoothstep(0.50f, 0.80f, open_y)) *
              live2d_smoothstep(0.00f, 0.60f, form);
    float o = live2d_smoothstep(0.35f, 0.85f, open_y) * live2d_smoothstep(-0.05f, -0.65f, form);

    const float sum = a + i + u + e + o;
    if (sum > 1.0f) {
        const float inv = 1.0f / sum;
        a *= inv;
        i *= inv;
        u *= inv;
        e *= inv;
        o *= inv;
    }

    out_a = std::clamp(a, 0.0f, 1.0f);
    out_i = std::clamp(i, 0.0f, 1.0f);
    out_u = std::clamp(u, 0.0f, 1.0f);
    out_e = std::clamp(e, 0.0f, 1.0f);
    out_o = std::clamp(o, 0.0f, 1.0f);
}

/**
 * Packs Live2D brow & mouth manifold scalars into `AetherTrackingFrame::_reserved[16]`
 * without altering the frozen 2048 B FFI struct layout.
 */
inline void pack_live2d_into_tracking_frame(const Live2DParameters& p, AetherTrackingFrame& frame) noexcept {
    auto q15 = [](float v) noexcept -> int16_t {
        const float c = std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f);
        return static_cast<int16_t>(std::lround(c * 32767.0f));
    };
    const uint16_t open_word = static_cast<uint16_t>(
        0x8000U | static_cast<uint16_t>(std::lround(std::clamp(p.param_mouth_open_y, 0.0f, 1.0f) * 16383.0f)));
    std::array<int16_t, 8> words{};
    words[0] = static_cast<int16_t>(open_word);
    words[1] = q15(p.param_mouth_form);
    words[2] = q15(p.param_brow_l_y);
    words[3] = q15(p.param_brow_r_y);
    words[4] = q15(p.param_brow_l_angle);
    words[5] = q15(p.param_brow_r_angle);
    words[6] = q15(p.param_brow_l_form);
    words[7] = q15(p.param_brow_r_form);
    std::memcpy(frame._reserved, words.data(), sizeof(words));
}

[[nodiscard]] inline bool has_packed_live2d(const AetherTrackingFrame& frame) noexcept {
    uint16_t word0 = 0;
    std::memcpy(&word0, frame._reserved, sizeof(word0));
    return (word0 & 0x8000U) != 0U;
}

inline void unpack_live2d_from_tracking_frame(const AetherTrackingFrame& frame, Live2DParameters& out) noexcept {
    std::array<int16_t, 8> words{};
    std::memcpy(words.data(), frame._reserved, sizeof(words));
    const uint16_t word0 = static_cast<uint16_t>(words[0]);
    constexpr float kInvQ14 = 1.0f / 16383.0f;
    constexpr float kInvQ15 = 1.0f / 32767.0f;

    if ((word0 & 0x8000U) != 0U) {
        out.param_mouth_open_y = std::clamp(static_cast<float>(word0 & 0x3FFFU) * kInvQ14, 0.0f, 1.0f);
        out.param_mouth_form = std::clamp(static_cast<float>(words[1]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_l_y = std::clamp(static_cast<float>(words[2]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_r_y = std::clamp(static_cast<float>(words[3]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_l_angle = std::clamp(static_cast<float>(words[4]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_r_angle = std::clamp(static_cast<float>(words[5]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_l_form = std::clamp(static_cast<float>(words[6]) * kInvQ15, -1.0f, 1.0f);
        out.param_brow_r_form = std::clamp(static_cast<float>(words[7]) * kInvQ15, -1.0f, 1.0f);
    } else {
        // Fallback reconstruction from ARKit blendshapes + webcam_visemes
        const float brow_down_l = std::clamp(frame.arkit_blendshapes[41], 0.0f, 1.0f);
        const float brow_down_r = std::clamp(frame.arkit_blendshapes[42], 0.0f, 1.0f);
        const float brow_in_up = std::clamp(frame.arkit_blendshapes[43], 0.0f, 1.0f);
        const float brow_out_l = std::clamp(frame.arkit_blendshapes[44], 0.0f, 1.0f);
        const float brow_out_r = std::clamp(frame.arkit_blendshapes[45], 0.0f, 1.0f);
        out.param_brow_l_y = std::clamp(0.6f * brow_in_up + 0.6f * brow_out_l - brow_down_l, -1.0f, 1.0f);
        out.param_brow_r_y = std::clamp(0.6f * brow_in_up + 0.6f * brow_out_r - brow_down_r, -1.0f, 1.0f);
        out.param_brow_l_angle = std::clamp(brow_in_up - 0.5f * brow_down_l - 0.5f * brow_out_l, -1.0f, 1.0f);
        out.param_brow_r_angle = std::clamp(brow_in_up - 0.5f * brow_down_r - 0.5f * brow_out_r, -1.0f, 1.0f);
        out.param_brow_l_form = std::clamp(0.5f * (brow_in_up + brow_out_l) - brow_down_l, -1.0f, 1.0f);
        out.param_brow_r_form = std::clamp(0.5f * (brow_in_up + brow_out_r) - brow_down_r, -1.0f, 1.0f);

        const float smile = 0.5f * (frame.arkit_blendshapes[23] + frame.arkit_blendshapes[24]);
        const float pucker = std::max(frame.arkit_blendshapes[20], frame.arkit_blendshapes[19] * 0.7f);
        out.param_mouth_form = std::clamp(smile - pucker, -1.0f, 1.0f);
        out.param_mouth_open_y = std::clamp(frame.arkit_blendshapes[17], 0.0f, 1.0f);
    }

    out.param_eye_l_open = std::clamp(1.0f - frame.arkit_blendshapes[0], 0.0f, 1.0f);
    out.param_eye_r_open = std::clamp(1.0f - frame.arkit_blendshapes[7], 0.0f, 1.0f);
    const float look_in_l = frame.arkit_blendshapes[2];
    const float look_out_l = frame.arkit_blendshapes[3];
    const float look_in_r = frame.arkit_blendshapes[9];
    const float look_out_r = frame.arkit_blendshapes[10];
    const float look_down = 0.5f * (frame.arkit_blendshapes[1] + frame.arkit_blendshapes[8]);
    const float look_up = 0.5f * (frame.arkit_blendshapes[4] + frame.arkit_blendshapes[11]);
    out.param_eye_ball_x = std::clamp(0.5f * ((look_out_r - look_in_r) + (look_in_l - look_out_l)), -1.0f, 1.0f);
    out.param_eye_ball_y = std::clamp(look_up - look_down, -1.0f, 1.0f);

    out.vowel_a = std::clamp(frame.webcam_visemes[0], 0.0f, 1.0f);
    out.vowel_i = std::clamp(frame.webcam_visemes[1], 0.0f, 1.0f);
    out.vowel_u = std::clamp(frame.webcam_visemes[2], 0.0f, 1.0f);
    out.vowel_e = std::clamp(frame.webcam_visemes[3], 0.0f, 1.0f);
    out.vowel_o = std::clamp(frame.webcam_visemes[4], 0.0f, 1.0f);
}

} // namespace aether::animation
