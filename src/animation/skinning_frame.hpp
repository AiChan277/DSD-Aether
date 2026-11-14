#pragma once

#include "core/telemetry.hpp"

#include <array>
#include <cstdint>

namespace aether::animation {

inline constexpr uint32_t kMaxBones = 1024;
inline constexpr uint32_t kMaxMorphs = 1024;
inline constexpr uint32_t kVisemeSlotOffset = 52;
inline constexpr uint32_t kVisemeCount = 5;

/**
 * Full T2 -> T3 skinning and morph snapshot (`AETHER-TS-001` §9.8).
 * C++ internal (not FFI); triple-buffered via `LockFreeTripleBuffer<SkinningFrame>` (~53 KB/slot).
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct alignas(64) SkinningFrame {
    FrameStamps stamps{};
    uint32_t bone_count{0};
    uint32_t morph_count{0};
    float bone_matrices[kMaxBones][12]{}; // 3x4 row-major object-space skinning matrices
    float morph_weights[kMaxMorphs]{};
    float mouth_owner_blend{0.0f}; // 0.0 = camera/NPU owns mouth, 1.0 = DSP owns mouth (§9.7)
    float _pad0[3]{};
    float head_pivot_ws[4]{0.0f, 1.45f, 0.0f, 1.0f};
    float head_forward_ws[4]{0.0f, 0.0f, 1.0f, 0.0f};
    float head_right_ws[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float light_overrides[4][8]{};
    // Phase 3 debug/HUD view-model fields for Dev Shell (strictly scalar POD, zero alloc):
    float head_pos_ws[3]{0.0f, 1.45f, 0.0f};
    float sim_tick_ms{0.0f};
    float spring_sim_ms{0.0f};
    float tracking_confidences[5]{}; // face, upper, l_hand, r_hand, lower
    uint32_t active_ep{3};
    uint32_t tracking_valid_flags{0};
    float arkit_preview[52]{};
    float _reserved_tail[12]{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(alignof(SkinningFrame) == 64);
static_assert(sizeof(SkinningFrame) >= 53248 && sizeof(SkinningFrame) <= 57344,
              "SkinningFrame matches §9.8 ~53 KB per slot");

} // namespace aether::animation
