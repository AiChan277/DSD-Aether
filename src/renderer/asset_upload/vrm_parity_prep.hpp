#pragma once

#include "renderer/asset_upload/gpu_avatar.hpp"

#include <cstdint>

namespace aether::renderer {

struct VrmParityStats {
    uint32_t outline_tex_modulated_vertices{0};
    uint32_t procedural_edge_vertices{0};
    uint32_t oklab_harmonized_materials{0};
};

/**
 * Normalizes VRM-to-MMD visual parity once at avatar upload:
 *  1) Samples MToon `_OutlineWidthTexture` green channel into per-vertex `outline_width_scale`.
 *  2) Applies procedural `edge_scale` tapering on VRM silhouettes/folds while preserving seam masks.
 *  3) Harmonizes muddy or missing VRM `shade_color_gi` in OKLab space.
 */
VrmParityStats prepare_vrm_and_pmx_parity(GpuAvatar& avatar, bool vrm_auto_edge_scale_enabled = true);

} // namespace aether::renderer
