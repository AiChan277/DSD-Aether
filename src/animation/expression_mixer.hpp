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
                float dt_s, float out_morphs[kMaxMorphs], float& out_mouth_owner_blend) noexcept;

    [[nodiscard]] float current_mouth_owner_blend() const noexcept { return mouth_owner_blend_; }
    [[nodiscard]] static bool is_mouth_arkit_slot(size_t slot) noexcept;

  private:
    float mouth_owner_blend_{0.0f}; // 0.0 = camera owns mouth, 1.0 = DSP owns mouth
    std::array<float, kMaxExpressionSlots> smoothed_overrides_{};
};

} // namespace aether::animation
