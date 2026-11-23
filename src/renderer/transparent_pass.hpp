#pragma once

#include "aether_ffi_contract.h"
#include "renderer/gpu_structs.hpp"

#include <cstdint>

namespace aether::renderer {

enum class TransparentBlendMode : uint8_t {
    AlphaBlend = 0,
    AdditiveOverlay = 1,
};

[[nodiscard]] constexpr bool is_additive_submesh(const GpuSubmeshDraw& d, const GpuMaterial& gm) noexcept {
    return d.alpha_mode == AETHER_ALPHA_MODE_ADDITIVE || (d.flags & AETHER_SUBMESH_FLAG_ADDITIVE) != 0u ||
           (gm.extra_slots[2] & AETHER_MAT_FLAG_ADDITIVE) != 0u;
}

[[nodiscard]] constexpr bool is_transparent_submesh(const GpuSubmeshDraw& d, const GpuMaterial& gm) noexcept {
    return d.alpha_mode == AETHER_ALPHA_MODE_BLEND || (d.flags & AETHER_SUBMESH_FLAG_TRANSPARENT) != 0u ||
           is_additive_submesh(d, gm);
}

[[nodiscard]] constexpr TransparentBlendMode transparent_blend_mode(const GpuSubmeshDraw& d,
                                                                    const GpuMaterial& gm) noexcept {
    return is_additive_submesh(d, gm) ? TransparentBlendMode::AdditiveOverlay : TransparentBlendMode::AlphaBlend;
}

[[nodiscard]] constexpr bool is_shadow_caster_submesh(const GpuSubmeshDraw& d, const GpuMaterial& gm) noexcept {
    if (is_transparent_submesh(d, gm)) {
        return false;
    }
    return (gm.extra_slots[2] & AETHER_MAT_FLAG_NO_SHADOW_CAST) == 0u;
}

[[nodiscard]] constexpr bool is_outline_enabled_submesh(const GpuSubmeshDraw& d, const GpuMaterial& gm) noexcept {
    if (is_transparent_submesh(d, gm)) {
        return false;
    }
    if ((gm.extra_slots[2] & AETHER_MAT_FLAG_NO_OUTLINE) != 0u) {
        return false;
    }
    return gm.outline_color_width[3] > 0.0001f;
}

} // namespace aether::renderer
