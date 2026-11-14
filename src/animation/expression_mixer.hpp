#pragma once

#include "aether_ffi_contract.h"
#include "animation/skinning_frame.hpp"

#include <array>
#include <cstdint>

namespace aether::animation {

inline constexpr uint32_t kMaxExpressionSlots = AETHER_EXPRESSION_SLOT_COUNT; // 64
inline constexpr uint32_t kArkitSlotCount = 52;

/**
 * Evaluates ARKit-52 + VRM mouth/expression mixing with 60 ms mouth-ownership hand-off (§9.7)
 * between live camera tracking and DSP visemes (from T4 `AetherDspSyncPacket`), plus
 * VRM binary/exclusive overrides and Rust graph overrides — zero heap allocations.
 */
class ExpressionMixer {
  public:
    void reset() noexcept;

    /**
     * Mixes camera blendshapes (`arkit_cam`), Rust graph expression overrides (`graph_out`),
     * and audio DSP viseme telemetry (`dsp`) over timestep `dt_s`.
     * Writes the 52 ARKit slots + 5 visemes + any override slots into `out_morphs[kMaxMorphs]`.
     */
    void update(const float arkit_cam[52], const AetherDspSyncPacket& dsp, const AetherGraphOutputs& graph_out,
                float dt_s, float out_morphs[kMaxMorphs], float& out_mouth_owner_blend,
                const float* webcam_visemes_5 = nullptr, bool has_split_brow_morphs = true,
                const Live2DParameters* in_live2d = nullptr, Live2DParameters* out_live2d = nullptr) noexcept;

    [[nodiscard]] float current_mouth_owner_blend() const noexcept { return mouth_owner_blend_; }
    [[nodiscard]] static bool is_mouth_arkit_slot(size_t slot) noexcept;
    [[nodiscard]] static bool is_brow_arkit_slot(size_t slot) noexcept;

  private:
    float mouth_owner_blend_{0.0f}; // 0.0 = camera owns mouth, 1.0 = DSP owns mouth
    std::array<float, kMaxExpressionSlots> smoothed_overrides_{};
    // Warudo SmoothDamp state for camera-driven channels (blink 28 ms, mouth / vowels 38 ms, brows 55 ms)
    std::array<float, 52> cam_val_{};
    std::array<float, 52> cam_vel_{};
    std::array<float, 5> vowel_val_{};
    std::array<float, 5> vowel_vel_{};
    std::array<float, 10> live2d_val_{}; // [0..5]=brow L/R Y/Angle/Form, [6..7]=mouth OpenY/Form, [8..9]=eyeBall X/Y
    std::array<float, 10> live2d_vel_{};
    bool primed_{false};
};

} // namespace aether::animation
