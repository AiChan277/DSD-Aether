#pragma once

#include "aether_ffi_contract.h"

#include <array>
#include <cstdint>

namespace aether::tracking {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

/**
 * Configurable 3D NPU tracking safe-zone bounding box & predictive anti-clipping configuration (`Task 8`).
 * 64-byte aligned for lock-free snapshot transfer between UI/T1 without heap allocations (`HR-01`, `HR-02`).
 */
struct alignas(64) NpuMovementBoundsConfig {
    std::array<float, 3> bounds_min{-0.35f, -0.22f, -0.30f}; // Min relative translation (m) around anchor
    std::array<float, 3> bounds_max{0.35f, 0.22f, 0.30f};    // Max relative translation (m) around anchor
    float soft_wall_elasticity{0.35f};                       // [0.05..0.95] C^1 tanh soft-wall knee width
    float max_head_pitch_deg{42.0f};                         // Extreme downward/upward pitch clamp
    float max_head_yaw_deg{62.0f};
    float max_head_roll_deg{40.0f};
    float anti_clip_margin_m{0.035f}; // Extra SDF capsule clearance around head/torso/hair
    bool enable_bounds{true};
    bool enable_anti_clip{true};
    bool show_safe_box_preview{false};
    uint8_t _pad[5]{};
};

static_assert(alignof(NpuMovementBoundsConfig) == 64);

struct NpuPostProcessDiagnostics {
    bool translation_compressed{false};
    bool collision_projected{false};
    bool orientation_clamped{false};
    float max_penetration_m{0.0f};      // Deepest incoming limb penetration BEFORE projection
    float residual_penetration_m{0.0f}; // Deepest penetration AFTER projection (must be 0 for a clean pose)
    float bound_saturation{0.0f};       // [0..1+] normalized excursion before tanh compression
};

/**
 * NPU-stage continuous `tanh` soft-boundary tensor operator + predictive SDF anti-clipping filter (`Task 8`).
 * Executes on T1 inside `LandmarkPoseSolver` BEFORE 60 Hz / 240 Hz `SimEngine` dampers (`HR-01`, `HR-02`).
 */
class alignas(64) NpuPostProcessor {
  public:
    void reset() noexcept;

    /**
     * C^1 continuous soft-boundary `tanh` tensor operator mapping arbitrary relative 3D offset `p_rel`
     * strictly inside `[bounds_min, bounds_max]` with zero distortion in the interior `[0, 1 - elasticity]`.
     */
    [[nodiscard]] static std::array<float, 3> compress_translation_tanh(const std::array<float, 3>& p_rel,
                                                                        const std::array<float, 3>& bounds_min,
                                                                        const std::array<float, 3>& bounds_max,
                                                                        float elasticity) noexcept;

    /**
     * Applies finite sanity checks, extreme pitch/yaw/roll clamping, C^1 `tanh` translation boundary
     * compression, and signed-distance capsule anti-clipping (head/torso/hair) directly to `frame`.
     */
    [[nodiscard]] NpuPostProcessDiagnostics process(AetherTrackingFrame& frame,
                                                    const NpuMovementBoundsConfig& cfg) noexcept;

  private:
    std::array<float, 3> last_safe_head_euler_{};
    bool has_prev_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aether::tracking
