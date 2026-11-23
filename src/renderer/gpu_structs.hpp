#pragma once

#include "aether_ffi_contract.h"

#include <cstddef>
#include <cstdint>

#if defined(__clang__)
#define AETHER_OFFSETOF(type, member) __builtin_offsetof(type, member)
#else
#define AETHER_OFFSETOF(type, member) offsetof(type, member)
#endif

namespace aether::renderer {

struct alignas(16) GpuMat4 {
    float col0[4]{1.0f, 0.0f, 0.0f, 0.0f};
    float col1[4]{0.0f, 1.0f, 0.0f, 0.0f};
    float col2[4]{0.0f, 0.0f, 1.0f, 0.0f};
    float col3[4]{0.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(GpuMat4) == 64);

struct alignas(16) GpuPushConstants {
    uint64_t frame_globals_bda{0};
    uint64_t draw_data_bda{0};
    uint64_t vertex_buf_bda{0};
    uint64_t aux_buf_bda{0};
    uint32_t param0{0};
    uint32_t param1{0};
    uint32_t param2{0};
    uint32_t param3{0};
    float fparam0{0.0f};
    float fparam1{0.0f};
    float fparam2{0.0f};
    float fparam3{0.0f};
};
static_assert(sizeof(GpuPushConstants) == 64);
static_assert(sizeof(GpuPushConstants) <= 128);
static_assert(AETHER_OFFSETOF(GpuPushConstants, param0) == 32);
static_assert(AETHER_OFFSETOF(GpuPushConstants, fparam0) == 48);

struct alignas(16) GpuSkinnedVertex {
    float pos_zflat[4]{};
    float prev_pos_sdf[4]{};
    float true_norm_hair[4]{};
    float proxy_norm_out[4]{};
    float tangent_crease[4]{};
    float uv[2]{};
    uint32_t material_id{0};
    uint32_t flags{0};
};
static_assert(sizeof(GpuSkinnedVertex) == 96);
static_assert(AETHER_OFFSETOF(GpuSkinnedVertex, pos_zflat) == 0);
static_assert(AETHER_OFFSETOF(GpuSkinnedVertex, uv) == 80);

// Legacy GpuLight removed (Rev 3.1 Single Lighting Authority: StudioLightGPU[16] only)

struct alignas(16) GpuMaterial {
    float lit_color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float shade_color[4]{0.72f, 0.75f, 0.85f, 1.0f};
    float rim_color_power[4]{0.2f, 0.25f, 0.35f, 3.0f};
    float outline_color_width[4]{0.08f, 0.07f, 0.10f, 1.2f};
    float emission_and_shift[4]{0.0f, 0.0f, 0.0f, -0.05f};
    float toon_params[4]{0.85f, 0.0f, 0.08f, 0.5f};
    float hair_specular_params[4]{0.0f, 0.35f, 0.35f, 0.35f};
    uint32_t tex_slots[4]{};
    uint32_t extra_slots[4]{};
    float bounce_tint_zflat[4]{1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(GpuMaterial) == 160);

struct alignas(16) GpuMaterialGrade {
    float uv_scale_offset[4]{1.0f, 1.0f, 0.0f, 0.0f}; // [scale_x, scale_y, offset_x, offset_y]
    float grade_hsbc[4]{0.0f, 1.0f, 0.0f, 1.0f};      // [hue_shift_deg, saturation, brightness, contrast]
    float grade_tint[4]{1.0f, 1.0f, 1.0f, 1.0f};      // [tint_r, tint_g, tint_b, tint_strength]
    float dynamic_params[4]{0.0f, 0.0f, 0.0f, 0.0f};  // [emissive_intensity, blush_alpha, rim_boost, flags]
};
static_assert(sizeof(GpuMaterialGrade) == 64);

struct alignas(16) GpuSubmeshDraw {
    uint32_t index_offset{0};
    uint32_t index_count{0};
    uint32_t vertex_offset{0};
    uint32_t material_index{0};
    uint32_t stencil_ref{0x01};
    uint32_t cull_mode{0};
    uint32_t alpha_mode{0};
    uint32_t flags{0};
};
static_assert(sizeof(GpuSubmeshDraw) == 32);

struct alignas(16) GpuHeatmapBone {
    float p0_ndc_weight[4]{};
    float p1_ndc_pad[4]{};
};
static_assert(sizeof(GpuHeatmapBone) == 32);

struct alignas(16) GpuStageOverlayElem {
    float p0_ws[4]{};          // xyz = world start, w = line width px / aux
    float p1_ws[4]{};          // xyz = world end, w = dash frequency
    float p2_ws[4]{};          // xyz = world tri vertex 2 (when mode >= 1.5), w = alpha/fade
    float color_and_mode[4]{}; // rgb = sRGB/linear color, w = 0:solid line, 1:dashed line, 2:translucent quad tri
};
static_assert(sizeof(GpuStageOverlayElem) == 64);

struct alignas(16) GpuFrameGlobals {
    GpuMat4 view{};
    GpuMat4 proj{};
    GpuMat4 view_proj{};
    GpuMat4 inv_proj{};
    GpuMat4 inv_view{};
    GpuMat4 prev_view_proj{};
    GpuMat4 shadow_view_proj[4]{};
    float viewport_size[4]{1920.0f, 1080.0f, 1.0f / 1920.0f, 1.0f / 1080.0f};
    float head_pivot_view_and_flatness[4]{0.0f, 1.35f, -2.0f, 0.75f};
    float head_fwd_world[4]{0.0f, 0.0f, 1.0f, 6.0f};
    float head_right_world[4]{1.0f, 0.0f, 0.0f, 0.60f};
    float light_params[4]{0.08f, 0.45f, 0.25f, 0.35f};
    float camera_pos_world[4]{0.0f, 1.35f, 2.0f, 0.05f};
    float post_params[4]{1.05f, 0.28f, 1.0f, 0.5f};
    float bg_color_premul[4]{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t state_u32[4]{};
    uint32_t gbuffer_ids[8]{};
    uint32_t gbuffer_extra_ids[8]{};
    uint32_t history_ids[4]{};
    uint32_t output_storage_ids[4]{};
    float avatar_pos_scale[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float avatar_rot_sincos[4]{1.0f, 0.0f, 0.0f, 1.0f};
    float npr_style_params[4]{0.85f, 0.025f, 0.75f, 0.85f};
    float npr_style_params2[4]{0.75f, 0.85f, 0.65f, 0.0f};
    // xyz = smoothed SpringBone hair velocity (m/s), w = pre-quantization GI bleed strength k_gi
    float hair_velocity_ws[4]{0.0f, 0.0f, 0.0f, 0.75f};
    // x = Kuwahara/painterly flatten, y = DoG ink boost, z = anime paper warmth, w = rim depth offset (px)
    float filter_pro_params[4]{0.65f, 0.70f, 0.22f, 3.0f};
    // x = fixture_id (0..5), y = cos_inner, z = cos_outer, w = emitter_aspect_or_ring_r
    float photometric_params0[4]{0.0f, -1.0f, -1.0f, 2.15f};
    // x = barndoor_softness, y = cie_sky_strength, z = ground_bounce_strength, w = exposure_flux_norm
    float photometric_params1[4]{0.0f, 0.0f, 0.0f, 1.0f};
    // x = eye_catchlight_intensity, y = eye_catchlight_size, z = crease_disguise_eff, w = coaxial_flag
    float eye_catchlight_params[4]{0.85f, 1.00f, 0.0f, 0.0f};
    // xyz = world position of eye sphere center, w = cornea_radius (m)
    float eye_left_center_r[4]{-0.032f, 1.40f, 0.025f, 0.014f};
    float eye_right_center_r[4]{0.032f, 1.40f, 0.025f, 0.014f};
    // xyz = animated world-space Neck joint center, w = horizontal neck radius (m)
    float neck_seam_origin_r[4]{0.0f, 1.26f, 0.0f, 0.095f};
    // xyz = animated world-space Head joint center (neck->head axis), w = neck_seam_blend strength (0..1)
    float neck_seam_head_str[4]{0.0f, 1.33f, 0.0f, 1.0f};
    // rgb = sampled linear Face chin/lower-cheek reference skin RGB, w = valid flag (1.0 when sampled)
    float face_skin_ref_rgb[4]{0.85f, 0.68f, 0.58f, 0.0f};
    // xyz = animated world-space Chest/Clavicle center, w = clavicle radius (m) for Unified Head-Neck-Chest field
    float neck_chest_ws[4]{0.0f, 1.15f, 0.0f, 0.145f};
    // xyz = animated world-space Chin anchor, w = valid flag
    float chin_pos_ws[4]{0.0f, 1.32f, 0.04f, 0.0f};
    // Split-Layer Holo-Grading (Avatar): x=contrast, y=saturation, z=warmth, w=ev_bias
    float split_avatar_grade[4]{1.04f, 1.05f, 0.0f, 0.0f};
    // Split-Layer Holo-Grading (Background): x=contrast, y=saturation, z=exposure, w=master_grade
    float split_bg_grade[4]{1.02f, 0.96f, 1.0f, 1.0f};
    // Satsuei optical composite: x=light_wrap, y=halation_str, z=halation_thresh, w=backscatter_str
    float satsuei_params[4]{0.22f, 0.18f, 0.95f, 0.55f};
    // Hillaire (2020) 3-band SH9 sky environment dome RGB coefficients
    float sky_sh9[9][4]{};
    // x=ltc_inv_slot, y=ltc_amp_slot, z=ies_lut_slot, w=studio_light_count
    uint32_t studio_slots0[4]{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u};
    // x=aov_direct_vis_slot, y=aov_indirect_gi_slot, z=aov_backscatter_slot, w=aov_cornea_spec_slot
    uint32_t studio_slots1[4]{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    // x=aov_direct_vis_uav, y=aov_indirect_gi_uav, z=aov_backscatter_uav, w=aov_cornea_spec_uav
    uint32_t studio_uav_slots[4]{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t studio_lights_bda{0};
    uint64_t reservoir_curr_bda{0};
    uint64_t reservoir_prev_bda{0};
    uint32_t studio_aux_u32[2]{0u, 0u};
    // x = is_orthographic (0/1), y = ortho_height (m), z = stage_diag_mode (0=Lit, 1=Clay 18% Gray, 2=False Color), w =
    // reserved
    float stage_camera_params[4]{0.0f, 2.20f, 0.0f, 0.0f};
    uint64_t joint_mats_bda{0};
    uint64_t pmx_sdef_bda{0};
    uint64_t skin_buf_bda{0};
    uint64_t uv_delta_bda{0};
    uint64_t material_grade_bda{0};
    uint64_t _phase3_pad_bda{0};
    // Active sub-rect UV mapping inside physical render targets: xy = active_size / buf_size, zw = max_valid_uv
    float target_uv_scale[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float target_half_uv_scale[4]{1.0f, 1.0f, 1.0f, 1.0f};
    // VRM-to-MMD 2.5D Parity (Rev 3.1 §8.3)
    //   vrm_parity_params0: x=auto_toon_harmonizer (0/1), y=cel_crispness_boost, z=shadow_chroma_lift,
    //   w=pre_cel_clarity vrm_parity_params1: x=hair_angel_ring_boost, y=unified_surface_group_crease (0/1),
    //   z=auto_edge_scale (0/1), w=pmx_strict_order
    float vrm_parity_params0[4]{1.0f, 0.85f, 0.22f, 0.25f};
    float vrm_parity_params1[4]{0.75f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(GpuFrameGlobals) % 16 == 0);
static_assert(sizeof(GpuFrameGlobals) <= 4096);

struct alignas(64) GpuReSTIRReservoir {
    float sample_pos_len[4]{};  // xyz = hit point world pos, w = W_sum * p_hat^-1
    float sample_norm_lum[4]{}; // xyz = hit normal, w = target_pdf / M
    float radiance_w[4]{};      // rgb = incoming radiance, w = sample count M
    float age_pad[4]{};         // x = age/history valid, yzw = reserved
};
static_assert(sizeof(GpuReSTIRReservoir) == 64);

static_assert(sizeof(AetherVertex25D) == 64);
static_assert(sizeof(AetherNprMaterial) == 192);
static_assert(sizeof(AetherStudioLightGpu) == 128);
static_assert(alignof(AetherStudioLightGpu) == 64);

} // namespace aether::renderer
