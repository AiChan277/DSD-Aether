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

struct alignas(16) GpuLight {
    float pos_or_dir[4]{};
    float color_intensity[4]{};
    float direction_radius[4]{};
    uint32_t role_and_flags[4]{};
};
static_assert(sizeof(GpuLight) == 64);

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
    GpuLight lights[8]{};
};
static_assert(sizeof(GpuFrameGlobals) % 16 == 0);
static_assert(AETHER_OFFSETOF(GpuFrameGlobals, lights) + 8 * sizeof(GpuLight) == sizeof(GpuFrameGlobals));

static_assert(sizeof(AetherVertex25D) == 64);
static_assert(sizeof(AetherNprMaterial) == 192);

} // namespace aether::renderer
