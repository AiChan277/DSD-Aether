#pragma once

struct GpuMat4 {
    float4 col0;
    float4 col1;
    float4 col2;
    float4 col3;
};

struct GpuPushConstants {
    uint64_t frame_globals_bda;
    uint64_t draw_data_bda;
    uint64_t vertex_buf_bda;
    uint64_t aux_buf_bda;
    uint param0;
    uint param1;
    uint param2;
    uint param3;
    float fparam0;
    float fparam1;
    float fparam2;
    float fparam3;
};

struct GpuSkinnedVertex {
    float4 pos_zflat;       // xyz = world/model pos, w = z_flatten_weight
    float4 prev_pos_sdf;    // xyz = prev pos, w = sdf_face_mask
    float4 true_norm_hair;  // xyz = true_normal, w = hair_shadow_mask
    float4 proxy_norm_out;  // xyz = proxy_normal, w = outline_width_scale
    float4 tangent_crease;  // xyz = smooth_tangent, w = crease_bias
    float2 uv;
    uint material_id;
    uint flags;
};

struct GpuSourceVertex {
    float4 pos_out;      // xyz = position[3], w = outline_width_scale (matches AetherVertex25D 64 B)
    float4 norm_zflat;   // xyz = true_normal[3], w = z_flatten_weight
    float4 proxy_sdf;    // xyz = proxy_normal[3], w = sdf_face_mask
    float4 smooth_tan;   // xyzw = smooth_tangent[4]
};

struct GpuLight {
    float4 pos_or_dir;       // xyz, w=0 directional, w=1 point/spot
    float4 color_intensity;  // rgb=linear color*intensity, w=cone_angle_rad
    float4 direction_radius; // xyz=dir, w=penumbra_or_radius_rad
    uint4 role_and_flags;    // x=role, y=flags (bit0 enabled, bit1 cast_rt), z=layer_mask, w=pad
};

struct GpuMaterial {
    float4 lit_color;
    float4 shade_color;
    float4 rim_color_power;
    float4 outline_color_width;
    float4 emission_and_shift;
    float4 toon_params;         // x=shade_toony, y=matcap_strength, z=sdf_feather, w=alpha_cutoff
    float4 hair_specular_params;// x=angel_ring_intensity, y=angel_ring_width, z=gi_intensity, w=bounce_weight
    uint4 tex_slots;            // x=albedo, y=shade, z=sdf_face, w=matcap
    uint4 extra_slots;          // x=normal, y=mask, z=flags, w=material_id
    float4 bounce_tint_zflat;   // xyz=bounce_albedo, w=z_flatten_scale
};

struct GpuSubmeshDraw {
    uint index_offset;
    uint index_count;
    uint vertex_offset;
    uint material_index;
    uint stencil_ref;
    uint cull_mode;
    uint alpha_mode;
    uint flags;
};

struct GpuHeatmapBone {
    float4 p0_ndc_weight; // xy = start clip/ndc, z = stress 0..1, w = bone_width_px
    float4 p1_ndc_pad;    // xy = end clip/ndc, z = category (0=ok,1=warn,2=over), w = bone_id
};

// 128-byte aligned Studio Luminaire + Optical Modifiers (matches AetherStudioLightGpu bit-for-bit)
struct StudioLightGPU {
    float3 position_ws;
    uint flags_and_type;
    float3 direction_ws;
    float intensity_ev100;
    float3 tangent_u_ws;
    float kelvin_cct;
    float3 color_rgb_linear;
    uint light_channel_mask;
    float2 size_xy_radius;
    float2 inner_outer_cos;
    float4 barn_door_cuts;
    float grid_collimation_cos;
    float diffusion_softness;
    float ies_profile_index;
    float catchlight_weight;
    float shadow_bias_scale;
    float padding0;
    float padding1;
    float padding2;
};

// 64-byte ReSTIR GI Spatiotemporal Reservoir
struct GpuReSTIRReservoir {
    float4 sample_pos_len;
    float4 sample_norm_lum;
    float4 radiance_w;
    float4 age_pad;
};

static const uint STUDIO_FIXTURE_RECT_SOFTBOX = 0u;
static const uint STUDIO_FIXTURE_OCTABOX = 1u;
static const uint STUDIO_FIXTURE_STRIPBOX = 2u;
static const uint STUDIO_FIXTURE_PARABOLIC = 3u;
static const uint STUDIO_FIXTURE_LANTERN = 4u;
static const uint STUDIO_FIXTURE_ARRI_SKYPANEL = 5u;
static const uint STUDIO_FIXTURE_ARRI_FRESNEL = 6u;
static const uint STUDIO_FIXTURE_DEDOLIGHT = 7u;
static const uint STUDIO_FIXTURE_ELLIPSOIDAL_LEKO = 8u;
static const uint STUDIO_FIXTURE_BROADCAST_RING = 9u;
static const uint STUDIO_FIXTURE_LED_TUBE = 10u;
static const uint STUDIO_FIXTURE_NATURAL_SUN_SKY = 11u;

static const uint STUDIO_FLAG_ENABLED = 1u << 4;
static const uint STUDIO_FLAG_SOLO = 1u << 5;
static const uint STUDIO_FLAG_CAST_SHADOW = 1u << 6;
static const uint STUDIO_FLAG_SOFTBOX_ON = 1u << 7;
static const uint STUDIO_FLAG_GRID_ON = 1u << 8;
static const uint STUDIO_FLAG_BARN_DOORS_ON = 1u << 9;
static const uint STUDIO_FLAG_GEL_ON = 1u << 10;
static const uint STUDIO_FLAG_AIM_LOCK = 1u << 11;
static const uint STUDIO_FLAG_NEG_FILL = 1u << 12;
static const uint STUDIO_FLAG_BOUNCE_CARD = 1u << 13;
static const uint STUDIO_FLAG_COAXIAL_LOCK = 1u << 14;
static const uint STUDIO_FLAG_DIFFUSION_ON = 1u << 15;
static const uint STUDIO_FLAG_WINDOW_PORTAL = 1u << 16;

static const uint LIGHT_CH_FACE = 1u << 0;
static const uint LIGHT_CH_HAIR = 1u << 1;
static const uint LIGHT_CH_BODY = 1u << 2;
static const uint LIGHT_CH_CATCHLIGHT = 1u << 3;

struct GpuFrameGlobals {
    GpuMat4 view;
    GpuMat4 proj;
    GpuMat4 view_proj;
    GpuMat4 inv_proj;
    GpuMat4 inv_view;
    GpuMat4 prev_view_proj;
    GpuMat4 shadow_view_proj[4];
    float4 viewport_size;
    float4 head_pivot_view_and_flatness;
    float4 head_fwd_world;
    float4 head_right_world;
    float4 light_params;     // x=sdf_feather, y=rim_t_max, z=bounce_t_max, w=bounce_strength
    float4 camera_pos_world; // xyz, w=near_plane
    float4 post_params;      // x=bloom_threshold, y=bloom_intensity, z=tone_mode, w=dither_lsb
    float4 bg_color_premul;
    uint4 state_u32;         // x=frame_index, y=light_count, z=debug_view, w=flags
    uint4 gbuffer_ids[2];
    uint4 gbuffer_extra_ids[2];
    uint4 history_ids;
    uint4 output_storage_ids;
    float4 avatar_pos_scale;  // xyz = AvatarRoot position, w = uniform scale
    float4 avatar_rot_sincos; // x = cos(yaw), y = sin(yaw), z = yaw_rad, w = 1
    float4 npr_style_params;  // x=face_norm_flatten, y=cel_band_sharpness, z=shadow_warm_cool_shift, w=iro_tsuke_strength
    float4 npr_style_params2; // x=crease_strength, y=hair_highlight_intensity, z=rim_step_strength, w=reserved
    float4 hair_velocity_ws;  // xyz=smoothed SpringBone hair velocity (m/s), w=indirect_bleed_strength k_gi
    float4 filter_pro_params;     // x=kuwahara_flatten, y=dog_ink_boost, z=paper_warmth, w=rim_depth_offset_px
    float4 photometric_params0;   // x=fixture_id, y=cos_inner, z=cos_outer, w=emitter_aspect_or_ring_r
    float4 photometric_params1;   // x=barndoor_softness, y=cie_sky_strength, z=ground_bounce_strength, w=exposure_flux_norm
    float4 eye_catchlight_params; // x=intensity, y=size, z=crease_disguise_eff, w=coaxial_flag
    float4 eye_left_center_r;     // xyz=left eye world pos, w=cornea_radius
    float4 eye_right_center_r;    // xyz=right eye world pos, w=cornea_radius
    float4 neck_seam_origin_r;    // xyz=animated Neck joint world pos, w=horizontal neck radius (m)
    float4 neck_seam_head_str;    // xyz=animated Head joint world pos, w=neck_seam_blend strength (0..1)
    float4 face_skin_ref_rgb;     // rgb=sampled linear Face chin/lower-cheek reference RGB, w=valid flag
    float4 neck_chest_ws;         // xyz=animated Chest world pos, w=clavicle radius
    float4 chin_pos_ws;           // xyz=animated Chin anchor, w=valid flag
    float4 split_avatar_grade;    // x=contrast, y=saturation, z=warmth, w=ev_bias
    float4 split_bg_grade;        // x=contrast, y=saturation, z=exposure, w=master_grade
    float4 satsuei_params;        // x=light_wrap, y=halation_str, z=halation_thresh, w=backscatter_str
    float4 sky_sh9[9];            // Hillaire (2020) SH9 atmosphere coefficients
    uint4 studio_slots0;          // x=ltc_inv_slot, y=ltc_amp_slot, z=ies_lut_slot, w=studio_light_count
    uint4 studio_slots1;          // x=aov_direct_vis, y=aov_indirect_gi, z=aov_backscatter, w=aov_cornea_spec
    uint4 studio_uav_slots;       // x=aov_direct_vis_uav, y=aov_indirect_gi_uav, z=aov_backscatter_uav, w=aov_cornea_spec_uav
    uint64_t studio_lights_bda;
    uint64_t reservoir_curr_bda;
    uint64_t reservoir_prev_bda;
    uint2 studio_aux_u32;
    GpuLight lights[8];
};

#ifdef __TARGET_SPIRV__
[[vk::push_constant]] ConstantBuffer<GpuPushConstants> g_Pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_SampledImages[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(2, 0)]] RWTexture2D<float4> g_StorageImages[];
[[vk::binding(3, 0)]] Texture2D<uint4> g_UintSampledImages[];

#define AETHER_LOAD(T, addr, idx) (((T*)(addr))[idx])
#define AETHER_STORE(T, addr, idx, val) (((T*)(addr))[idx] = (val))

float4 aether_sample(uint slot, uint samp, float2 uv) {
    return g_SampledImages[slot].Sample(g_Samplers[samp], uv);
}
float4 aether_sample_nu(uint slot, uint samp, float2 uv) {
    return g_SampledImages[NonUniformResourceIndex(slot)].Sample(g_Samplers[samp], uv);
}
float4 aether_sample_level(uint slot, uint samp, float2 uv, float lod) {
    return g_SampledImages[slot].SampleLevel(g_Samplers[samp], uv, lod);
}
float4 aether_sample_level_nu(uint slot, uint samp, float2 uv, float lod) {
    return g_SampledImages[NonUniformResourceIndex(slot)].SampleLevel(g_Samplers[samp], uv, lod);
}
float4 aether_load(uint slot, int2 coord) {
    return g_SampledImages[slot].Load(int3(coord, 0));
}
uint4 aether_uload(uint slot, int2 coord) {
    return g_UintSampledImages[slot].Load(int3(coord, 0));
}
void aether_store(uint slot, uint2 coord, float4 val) {
    g_StorageImages[slot][coord] = val;
}
#else
ConstantBuffer<GpuPushConstants> g_Pc : register(b0, space0);

T aether_dxil_load<T>(uint64_t h, uint idx) {
    uint desc = uint(h & 0xFFFFFu);
    uint off = uint(h >> 32u) + idx * sizeof(T);
    if ((uint(h) & 0x100000u) != 0u) {
        RWByteAddressBuffer b = ResourceDescriptorHeap[desc];
        return b.Load<T>(off);
    }
    ByteAddressBuffer b = ResourceDescriptorHeap[desc];
    return b.Load<T>(off);
}
void aether_dxil_store<T>(uint64_t h, uint idx, T val) {
    uint desc = uint(h & 0xFFFFFu);
    uint off = uint(h >> 32u) + idx * sizeof(T);
    RWByteAddressBuffer b = ResourceDescriptorHeap[desc];
    b.Store<T>(off, val);
}

#define AETHER_LOAD(T, addr, idx) aether_dxil_load<T>((addr), (idx))
#define AETHER_STORE(T, addr, idx, val) aether_dxil_store<T>((addr), (idx), (val))

float4 aether_sample(uint slot, uint samp, float2 uv) {
    Texture2D<float4> t = ResourceDescriptorHeap[slot];
    SamplerState s = SamplerDescriptorHeap[samp];
    return t.Sample(s, uv);
}
float4 aether_sample_nu(uint slot, uint samp, float2 uv) {
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(slot)];
    SamplerState s = SamplerDescriptorHeap[samp];
    return t.Sample(s, uv);
}
float4 aether_sample_level(uint slot, uint samp, float2 uv, float lod) {
    Texture2D<float4> t = ResourceDescriptorHeap[slot];
    SamplerState s = SamplerDescriptorHeap[samp];
    return t.SampleLevel(s, uv, lod);
}
float4 aether_sample_level_nu(uint slot, uint samp, float2 uv, float lod) {
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(slot)];
    SamplerState s = SamplerDescriptorHeap[samp];
    return t.SampleLevel(s, uv, lod);
}
float4 aether_load(uint slot, int2 coord) {
    Texture2D<float4> t = ResourceDescriptorHeap[slot];
    return t.Load(int3(coord, 0));
}
uint4 aether_uload(uint slot, int2 coord) {
    Texture2D<uint4> t = ResourceDescriptorHeap[slot];
    return t.Load(int3(coord, 0));
}
void aether_store(uint slot, uint2 coord, float4 val) {
    RWTexture2D<float4> w = ResourceDescriptorHeap[slot];
    w[coord] = val;
}
#endif

static const uint kSamplerLinearClamp = 0;
static const uint kSamplerNearestClamp = 1;
static const uint kSamplerLinearRepeat = 2;
static const uint kSamplerNearestRepeat = 3;

float4 mul_col(GpuMat4 m, float4 v) {
    return m.col0 * v.x + m.col1 * v.y + m.col2 * v.z + m.col3 * v.w;
}

float3 mul_col3(GpuMat4 m, float3 v) {
    return (m.col0.xyz * v.x + m.col1.xyz * v.y + m.col2.xyz * v.z);
}

float ndc_depth_from_view_z(GpuMat4 proj, float view_z) {
    float4 p = mul_col(proj, float4(0.0, 0.0, view_z, 1.0));
    return p.z / max(abs(p.w), 1e-6) * sign(p.w);
}

// Live2D-style camera-facing Z-flattening (§10.4) preserving un-flattened view depth for self-occlusion.
float4 project_live2d_flat(GpuMat4 proj, float3 pos_view, float pivot_z, float z_flatten_weight,
                           float global_flatness) {
    float k = saturate(z_flatten_weight * global_flatness);
    float3 pos_flat = float3(pos_view.xy, lerp(pos_view.z, pivot_z, k));
    float4 clip = mul_col(proj, float4(pos_flat, 1.0));
    float z_ndc = ndc_depth_from_view_z(proj, pos_view.z);
    clip.z = z_ndc * clip.w;
    return clip;
}

// Exact inverse projection for ray origin reconstruction from Z-flattened G-Buffer (§10.4).
float3 reconstruct_view_pos_from_zflat(GpuMat4 proj, float2 uv, float true_view_z, float pivot_z, float k) {
    float z_f = lerp(true_view_z, pivot_z, k);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float p00 = max(abs(proj.col0.x), 1e-5);
    float p11 = max(abs(proj.col1.y), 1e-5);
    float x_view = ndc.x * (-z_f) / p00;
    float y_view = ndc.y * (-z_f) / p11;
    return float3(x_view, y_view, true_view_z);
}

float3 normalize_safe(float3 v, float3 fallback_n) {
    float l2 = dot(v, v);
    return (l2 > 1e-10) ? (v * rsqrt(l2)) : fallback_n;
}

float linear_to_srgb_vec_comp(float u) {
    return (u <= 0.04045) ? (u / 12.92) : pow((u + 0.055) / 1.055, 2.4);
}

float3 srgb_to_linear_rgb(float3 c) {
    return float3(linear_to_srgb_vec_comp(c.x), linear_to_srgb_vec_comp(c.y), linear_to_srgb_vec_comp(c.z));
}

float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-7);
    if (n.z < 0.0) {
        float2 sign_xy = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * sign_xy;
    }
    return n.xy * 0.5 + 0.5;
}

float3 oct_decode(float2 e) {
    float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

float ign_blue_noise(uint2 pix, uint frame_idx) {
    float2 p = float2(pix) + float2(float(frame_idx & 63u) * 5.588238, float(frame_idx & 63u) * 5.588238);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

float3 agx_tonemap_soft(float3 x) {
    // Soft AgX-inspired shoulder preserving stylized anime chroma without hue skew (§10.10).
    x = max(x, 0.0);
    float luma = dot(x, float3(0.2126, 0.7152, 0.0722));
    float mapped_luma = luma / (1.0 + 0.55 * luma);
    float3 scaled = (luma > 1e-5) ? (x * (mapped_luma / luma)) : x;
    return lerp(scaled, saturate(scaled / (1.0 + 0.15 * scaled)), 0.35);
}

// Microfacet Charlie velvet/knitwear sheen NDF (Production 2.5D Hybrid Micro-PBR).
float d_charlie(float roughness, float ndoth) {
    float r = clamp(roughness, 0.12, 1.0);
    float inv_r = 1.0 / r;
    float cos2h = ndoth * ndoth;
    float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + inv_r) * pow(sin2h, inv_r * 0.5) / 6.2831853;
}

float linear_to_srgb_channel(float c) {
    c = saturate(c);
    return (c <= 0.0031308) ? (12.92 * c) : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float3 linear_to_srgb(float3 c) {
    return float3(linear_to_srgb_channel(c.x), linear_to_srgb_channel(c.y), linear_to_srgb_channel(c.z));
}

float luminance(float3 c) {
    return dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float3 normalize_chroma(float3 c) {
    float l = luminance(c);
    if (l <= 1e-4) {
        return float3(1.0, 1.0, 1.0);
    }
    return clamp(max(c, 0.0) / l, float3(0.45, 0.45, 0.45), float3(1.65, 1.65, 1.65));
}

float quantize_cel(float l_eff, float shift, float feather) {
    return smoothstep(shift - feather, shift + feather, l_eff);
}

// 3-Band Spherical Harmonics L0/L1/L2 ambient built from active studio Key/Fill/Rim rig + sky/ground bounce.
float3 eval_3band_sh_ambient(float3 n, GpuFrameGlobals fg) {
    float3 key_col = (fg.state_u32.y > 0u) ? fg.lights[0].color_intensity.rgb : float3(0.95, 0.94, 0.96);
    float3 fill_col = (fg.state_u32.y > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u)
                          ? fg.lights[1].color_intensity.rgb
                          : float3(0.45, 0.56, 0.80);
    float3 fill_dir = (fg.state_u32.y > 1u)
                          ? normalize_safe(fg.lights[1].pos_or_dir.xyz, float3(-0.6, 0.25, 0.7))
                          : float3(-0.6, 0.25, 0.7);
    float3 sky_col = lerp(float3(0.46, 0.58, 0.86), fill_col * 0.85, 0.55);
    float3 ground_col = lerp(float3(0.28, 0.24, 0.27), key_col * 0.26, 0.50);

    // Zonal L0 + L1 directional + L2 quadratic up/down envelope
    float y = n.y;
    float3 l0 = (sky_col + ground_col) * 0.28 + fill_col * 0.14;
    float3 l1 = (sky_col - ground_col) * (0.24 * y) + fill_col * (0.34 * saturate(dot(n, fill_dir) * 0.5 + 0.5));
    float l2_band = 0.08 * (3.0 * y * y - 1.0);
    float3 l2 = lerp(ground_col, sky_col, saturate(y * 0.5 + 0.5)) * max(l2_band, -0.05);
    return max(l0 + l1 + l2, float3(0.06, 0.06, 0.08));
}

float2 ndc_to_screen_uv(float2 ndc) {
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Smooth rotation-invariant skeletal neck-to-head blend weight (1.0 at chin/upper neck, tapering to 0.0 at collarbone).
float compute_neck_seam_blend(float3 world_pos, GpuFrameGlobals fg) {
    float strength = saturate(fg.neck_seam_head_str.w);
    if (strength <= 0.001) {
        return 0.0;
    }
    float3 neck_p = fg.neck_seam_origin_r.xyz;
    float3 head_p = fg.neck_seam_head_str.xyz;
    float3 axis = head_p - neck_p;
    float axis_len2 = max(dot(axis, axis), 1e-4);
    float t = dot(world_pos - neck_p, axis) / axis_len2;
    float vert_w = smoothstep(-0.85, -0.05, t) * (1.0 - smoothstep(1.35, 1.85, t));
    float3 spine_p = neck_p + axis * clamp(t, -0.85, 1.35);
    float r_perp = length(world_pos - spine_p);
    float neck_r = max(fg.neck_seam_origin_r.w, 0.04);
    float radial_w = 1.0 - smoothstep(neck_r * 0.85, neck_r * 1.45, r_perp);
    return vert_w * radial_w * strength;
}

// Forward-flattened head-aligned proxy normal for the upper neck so downward chin bevels don't drop into false shadow.
float3 compute_neck_proxy_normal(float3 true_n, GpuFrameGlobals fg) {
    float3 h_fwd = normalize_safe(fg.head_fwd_world.xyz, float3(0.0, 0.0, 1.0));
    float3 h_right = normalize_safe(fg.head_right_world.xyz, float3(1.0, 0.0, 0.0));
    float3 h_up = normalize_safe(cross(h_fwd, h_right), float3(0.0, 1.0, 0.0));
    float nx = dot(true_n, h_right);
    float ny = dot(true_n, h_up);
    float nz = dot(true_n, h_fwd);
    float front_gate = smoothstep(-0.35, 0.15, nz);
    float3 n_proxy = normalize_safe(h_right * (nx * 0.62) + h_up * (ny * 0.12) + h_fwd * max(nz, 0.55), h_fwd);
    return normalize_safe(lerp(true_n, n_proxy, front_gate), true_n);
}

// Smooth skin-chroma detector for shared Body_00_SKIN atlases (accepts pale/tan/dark skin, rejects black/grey/metal/lace).
float compute_body_skin_texel_weight(float3 lin_rgb, float4 face_ref) {
    float r = lin_rgb.r;
    float g = lin_rgb.g;
    float b = lin_rgb.b;
    float min_r = (face_ref.w > 0.5) ? min(0.10, face_ref.r * 0.30) : 0.10;
    float lum_ok = smoothstep(min_r * 0.6, min_r * 1.35, r);
    float rg_ok = smoothstep(1.01, 1.05, r / max(g, 1e-4));
    float rb_ok = smoothstep(1.09, 1.18, r / max(b, 1e-4));
    float gb_ok = smoothstep(0.95, 1.02, g / max(b, 1e-4));
    float diff_ok = smoothstep(0.035, 0.080, r - b);
    return lum_ok * rg_ok * rb_ok * gb_ok * diff_ok;
}

// ============================================================================
// Phase 1 Core Architecture: C-inf Unified Head-Neck-Chest Normal Field,
// Disney SIGGRAPH 2019 Shadow Terminator G', OKLab, Heitz LTC, Modifiers & SH9
// ============================================================================

float smin_exp(float a, float b, float k) {
    float h = max(k - abs(a - b), 0.0) / max(k, 1e-4);
    return min(a, b) - h * h * k * 0.25;
}

float sd_capsule_field(float3 p, float3 a, float3 b, float r) {
    float3 pa = p - a;
    float3 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-5), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

float unified_head_neck_chest_sdf(float3 p, GpuFrameGlobals fg) {
    float3 neck_p = fg.neck_seam_origin_r.xyz;
    float3 head_p = fg.neck_seam_head_str.xyz;
    float3 chest_p = fg.neck_chest_ws.xyz;
    float r_neck = max(fg.neck_seam_origin_r.w, 0.055);
    float r_chest = max(fg.neck_chest_ws.w, r_neck * 1.35);

    // Smooth torso-to-neck column capsule (blended from chest through neck into jaw base)
    float d_torso = sd_capsule_field(p, chest_p - float3(0.0, 0.06, 0.0), neck_p + float3(0.0, 0.02, 0.0), r_chest * 0.72);
    float d_neck = sd_capsule_field(p, chest_p + float3(0.0, 0.05, 0.0), head_p + float3(0.0, 0.03, 0.0), r_neck * 0.85);
    float d_col = smin_exp(d_torso, d_neck, r_neck * 0.65);

    // Flattened anime facial ellipsoid anchored on head/jaw
    float3 h_fwd = normalize_safe(fg.head_fwd_world.xyz, float3(0.0, 0.0, 1.0));
    float3 h_right = normalize_safe(fg.head_right_world.xyz, float3(1.0, 0.0, 0.0));
    float3 h_up = normalize_safe(cross(h_fwd, h_right), float3(0.0, 1.0, 0.0));
    float3 head_center = head_p + h_up * (r_neck * 0.45);
    float3 q = p - head_center;
    float3 q_loc = float3(dot(q, h_right), dot(q, h_up), dot(q, h_fwd));
    float3 radii = float3(r_neck * 1.15, r_neck * 1.32, r_neck * 1.45); // Gentle Z compression for anime planes
    float d_head = (length(q_loc / radii) - 1.0) * min(radii.x, min(radii.y, radii.z));

    return smin_exp(d_col, d_head, r_neck * 0.60);
}

// C-infinity continuous shading field across Face <-> Jaw <-> Neck <-> Chest/Clavicles.
// Weights per PDF Rev 2.0 §3.1: 0.85 above chin, 0.80 across jawline/neck, tapering to 0.65 at chest/clavicles.
float3 compute_unified_skin_normal(float3 world_pos, float3 true_n, GpuFrameGlobals fg) {
    float blend_master = saturate(fg.neck_seam_head_str.w);
    if (blend_master <= 0.001) {
        return normalize_safe(true_n, float3(0.0, 0.0, 1.0));
    }
    const float eps = 0.003;
    float dx = unified_head_neck_chest_sdf(world_pos + float3(eps, 0.0, 0.0), fg) -
               unified_head_neck_chest_sdf(world_pos - float3(eps, 0.0, 0.0), fg);
    float dy = unified_head_neck_chest_sdf(world_pos + float3(0.0, eps, 0.0), fg) -
               unified_head_neck_chest_sdf(world_pos - float3(0.0, eps, 0.0), fg);
    float dz = unified_head_neck_chest_sdf(world_pos + float3(0.0, 0.0, eps), fg) -
               unified_head_neck_chest_sdf(world_pos - float3(0.0, 0.0, eps), fg);
    float3 n_field = normalize_safe(float3(dx, dy, dz), true_n);

    // Region weighting curve along anatomical height from chest/clavicles through neck/jaw to chin/face
    float chin_y = (fg.chin_pos_ws.w > 0.5) ? fg.chin_pos_ws.y : (fg.neck_seam_head_str.y - 0.045);
    float neck_y = fg.neck_seam_origin_r.y;
    float chest_y = fg.neck_chest_ws.y;

    float t_jaw_to_chin = smoothstep(neck_y - 0.015, chin_y + 0.025, world_pos.y);
    float t_chest_to_neck = smoothstep(chest_y - 0.03, neck_y - 0.005, world_pos.y);
    float w_region = lerp(0.65, lerp(0.80, 0.85, t_jaw_to_chin), t_chest_to_neck);

    // Taper outside upper torso so hands/legs retain crisp silhouette normal detail
    float3 torso_mid = lerp(fg.neck_chest_ws.xyz, fg.neck_seam_head_str.xyz, 0.5);
    float dist_axis = length(world_pos.xz - torso_mid.xz);
    float radial_gate = 1.0 - smoothstep(fg.neck_chest_ws.w * 1.15, fg.neck_chest_ws.w * 2.1, dist_axis);
    float vert_gate = smoothstep(chest_y - 0.14, chest_y - 0.02, world_pos.y);
    float w = w_region * radial_gate * vert_gate * blend_master;

    return normalize_safe(lerp(true_n, n_field, w), true_n);
}

// Disney SIGGRAPH 2019 Analytical Shadow Terminator softening (Chiang, Li & Burley 2019)
// Eliminates jagged polygonal shadow lines where smooth/proxy normal Ns diverges from geometric Ng.
float disney_shadow_terminator(float3 Ng, float3 Ns, float3 L) {
    float ndl_g = dot(Ng, L);
    float ndl_s = dot(Ns, L);
    if (ndl_s <= 0.0) {
        return 0.0;
    }
    if (ndl_g >= 0.0) {
        // Compute ratio on back/glancing transitions safely
        float g_ndns = max(dot(Ng, Ns), 0.15);
        float G = saturate(ndl_g / max(g_ndns * ndl_s, 1e-4));
        return clamp(-G * G * G + G * G + G, 0.0, 1.0);
    }
    // Smooth grazing fade near horizon to prevent abrupt polygon clipping
    float horizon = smoothstep(-0.08, 0.0, ndl_g);
    return horizon * horizon * (3.0 - 2.0 * horizon) * 0.25;
}

// Geometric tangent-plane ray origin offset along true geometric normal Ng (never shaded/proxy normal)
float3 offset_ray_origin_ng(float3 p, float3 ng) {
    float scale = max(1.0, max(abs(p.x), max(abs(p.y), abs(p.z))));
    return p + normalize_safe(ng, float3(0.0, 1.0, 0.0)) * (0.0018 * scale);
}

// Horizon bounce folding: prevent below-surface black dots on 1-bounce GI hemisphere samples
float3 reflect_above_horizon(float3 dir, float3 ng) {
    float3 n = normalize_safe(ng, float3(0.0, 1.0, 0.0));
    float d = dot(dir, n);
    if (d < 0.05) {
        return normalize_safe(dir + n * (0.05 - d * 2.0), n);
    }
    return dir;
}

// Fast exact Björn Ottosson OKLab transforms (D65 linear sRGB <-> OKLab)
float3 lin_srgb_to_oklab(float3 c) {
    float3 cc = max(c, float3(0.0, 0.0, 0.0));
    float l = 0.4122214708 * cc.r + 0.5363325363 * cc.g + 0.0514459929 * cc.b;
    float m = 0.2119034982 * cc.r + 0.6806995451 * cc.g + 0.1073969566 * cc.b;
    float s = 0.0883024619 * cc.r + 0.2817188376 * cc.g + 0.6299787005 * cc.b;
    float l_ = pow(max(l, 1e-6), 1.0 / 3.0);
    float m_ = pow(max(m, 1e-6), 1.0 / 3.0);
    float s_ = pow(max(s, 1e-6), 1.0 / 3.0);
    return float3(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    );
}

float3 oklab_to_lin_srgb(float3 lab) {
    float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;
    return max(float3(
        +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
    ), float3(0.0, 0.0, 0.0));
}

// COVER Corp Soft-Knee Multi-Light Compression (> 0.85 compressed asymptotically below 1.0)
// while preserving secondary colored rim/accent chromaticity in OKLab space.
float3 compress_soft_knee_diffuse(float3 total_diff, float3 primary_diff, float3 accent_diff) {
    float lum = luminance(total_diff);
    float compressed_lum = lum;
    if (lum > 0.85) {
        float excess = lum - 0.85;
        compressed_lum = 0.85 + 0.15 * (excess / (excess + 0.45));
    }
    float ratio = compressed_lum / max(lum, 1e-4);
    float3 base_compressed = total_diff * ratio;

    // Preserve chromaticity of secondary accents in OKLab when strong key compresses luminance
    float accent_lum = luminance(accent_diff);
    if (accent_lum > 0.02 && lum > 0.85) {
        float3 lab_comp = lin_srgb_to_oklab(base_compressed);
        float3 lab_raw = lin_srgb_to_oklab(total_diff);
        float chroma_boost = 1.0 + 0.35 * saturate((lum - 0.85) / 0.65);
        lab_comp.yz = lerp(lab_comp.yz, lab_raw.yz * chroma_boost, 0.60);
        return oklab_to_lin_srgb(lab_comp);
    }
    return base_compressed;
}

float3 eval_hillaire_sky_sh9(float3 n_ws, GpuFrameGlobals fg) {
    float3 n = normalize_safe(n_ws, float3(0.0, 1.0, 0.0));
    float3 sh = fg.sky_sh9[0].xyz * 0.282095;
    sh += fg.sky_sh9[1].xyz * (0.488603 * n.y);
    sh += fg.sky_sh9[2].xyz * (0.488603 * n.z);
    sh += fg.sky_sh9[3].xyz * (0.488603 * n.x);
    sh += fg.sky_sh9[4].xyz * (1.092548 * n.x * n.y);
    sh += fg.sky_sh9[5].xyz * (1.092548 * n.y * n.z);
    sh += fg.sky_sh9[6].xyz * (0.315392 * (3.0 * n.y * n.y - 1.0));
    sh += fg.sky_sh9[7].xyz * (1.092548 * n.x * n.z);
    sh += fg.sky_sh9[8].xyz * (0.546274 * (n.x * n.x - n.y * n.y));
    if (luminance(sh) < 1e-4) {
        return eval_3band_sh_ambient(n, fg);
    }
    return max(sh, float3(0.02, 0.025, 0.035));
}

float eval_studio_gobo(uint gobo_id, float2 p_uv, float blur) {
    if (gobo_id == 0u) {
        return 1.0;
    }
    if (gobo_id == 1u) {
        // Window Blinds horizontal slats
        float stripe = sin(p_uv.y * 28.0);
        return smoothstep(-0.15 - blur * 0.35, 0.15 + blur * 0.35, stripe);
    }
    if (gobo_id == 2u) {
        // Anime Classroom Mullions + Slats
        float mx = abs(frac(p_uv.x * 2.5 + 0.5) - 0.5);
        float my = abs(frac(p_uv.y * 3.5 + 0.5) - 0.5);
        float bar_x = smoothstep(0.05, 0.08 + blur * 0.05, mx);
        float bar_y = smoothstep(0.04, 0.07 + blur * 0.05, my);
        return bar_x * bar_y;
    }
    if (gobo_id == 3u) {
        // Organic Leaf Canopy dapple
        float d1 = sin(p_uv.x * 13.0 + p_uv.y * 9.0) * cos(p_uv.y * 17.0 - p_uv.x * 7.0);
        float d2 = sin(p_uv.x * 23.0 - p_uv.y * 19.0);
        return smoothstep(-0.2 - blur * 0.3, 0.25 + blur * 0.3, d1 * 0.7 + d2 * 0.3);
    }
    // Stage Circle Iris
    float r = length(p_uv);
    return 1.0 - smoothstep(0.42 - blur * 0.1, 0.48 + blur * 0.1, r);
}

// Evaluates modular Studio Light optics: Honeycomb Grid, 4-Leaf Barn Doors, IES LUT, Spot Cone, Gobo
void eval_studio_light_modifiers(StudioLightGPU L_gpu, GpuFrameGlobals fg, float3 P,
                                 out float3 dir_to_light, out float dist, out float mod_atten) {
    uint ftype = L_gpu.flags_and_type & 0x0Fu;
    float3 to_l = L_gpu.position_ws - P;
    dist = max(length(to_l), 0.08);
    if (ftype == STUDIO_FIXTURE_NATURAL_SUN_SKY && (L_gpu.flags_and_type & STUDIO_FLAG_WINDOW_PORTAL) == 0u) {
        dir_to_light = normalize_safe(-L_gpu.direction_ws, float3(0.4, 0.7, 0.5));
        dist = 10.0;
    } else {
        dir_to_light = to_l / dist;
    }

    float3 beam_fwd = normalize_safe(L_gpu.direction_ws, -dir_to_light);
    float3 u_ax = normalize_safe(L_gpu.tangent_u_ws, float3(1.0, 0.0, 0.0));
    float3 v_ax = normalize_safe(cross(beam_fwd, u_ax), float3(0.0, 1.0, 0.0));
    u_ax = normalize_safe(cross(v_ax, beam_fwd), u_ax);

    // Signed vector from luminaire center toward shading point P in fixture local frame
    float3 l_to_p = -dir_to_light;
    float z_proj = max(dot(l_to_p, beam_fwd), 1e-4);
    float x_proj = dot(l_to_p, u_ax);
    float y_proj = dot(l_to_p, v_ax);
    mod_atten = 1.0;

    // 1. Directional cone shaping for Spots / Fresnel / Dedolight / Leko (and collimated fixtures)
    if (ftype == STUDIO_FIXTURE_ARRI_FRESNEL || ftype == STUDIO_FIXTURE_DEDOLIGHT ||
        ftype == STUDIO_FIXTURE_ELLIPSOIDAL_LEKO || ftype == STUDIO_FIXTURE_PARABOLIC) {
        float cos_ang = dot(l_to_p, beam_fwd);
        mod_atten *= smoothstep(L_gpu.inner_outer_cos.y, L_gpu.inner_outer_cos.x + 1e-4, cos_ang);
    }

    // 2. Honeycomb / Egg-Crate Grid (30 / 40 / 60 deg collimation): suppresses lateral spill cleanly
    if ((L_gpu.flags_and_type & STUDIO_FLAG_GRID_ON) != 0u) {
        float cos_g = L_gpu.grid_collimation_cos;
        float theta_g = acos(clamp(cos_g, 0.0, 1.0));
        float cos_inner_g = cos(theta_g * 0.65);
        float cos_ang = dot(l_to_p, beam_fwd);
        mod_atten *= smoothstep(cos_g, cos_inner_g + 1e-4, cos_ang);
    }

    // 3. 4-Leaf Barn Doors local cuts [Top, Bottom, Left, Right]
    if ((L_gpu.flags_and_type & STUDIO_FLAG_BARN_DOORS_ON) != 0u) {
        float ang_y = atan2(y_proj, z_proj);
        float ang_x = atan2(x_proj, z_proj);
        float edge_soft = 0.08 + 0.14 * L_gpu.diffusion_softness;
        float cut_top = 1.0 - smoothstep(L_gpu.barn_door_cuts.x - edge_soft, L_gpu.barn_door_cuts.x + edge_soft, ang_y);
        float cut_bot = 1.0 - smoothstep(L_gpu.barn_door_cuts.y - edge_soft, L_gpu.barn_door_cuts.y + edge_soft, -ang_y);
        float cut_left = 1.0 - smoothstep(L_gpu.barn_door_cuts.z - edge_soft, L_gpu.barn_door_cuts.z + edge_soft, -ang_x);
        float cut_right = 1.0 - smoothstep(L_gpu.barn_door_cuts.w - edge_soft, L_gpu.barn_door_cuts.w + edge_soft, ang_x);
        mod_atten *= cut_top * cut_bot * cut_left * cut_right;
    }

    // 4. ANSI/IES LM-63-19 256x64 LUT lookup
    if (L_gpu.ies_profile_index >= 0.0 && fg.studio_slots0.z != 0xFFFFFFFFu) {
        float polar_angle = acos(clamp(dot(l_to_p, beam_fwd), -1.0, 1.0));
        float u_ies = clamp(polar_angle / 3.14159265, 0.0, 1.0);
        float v_ies = (L_gpu.ies_profile_index + 0.5) / 64.0;
        float ies_val = aether_sample_level(fg.studio_slots0.z, 0u, float2(u_ies, v_ies), 0.0).r;
        mod_atten *= lerp(0.55 + 0.45 * ies_val, ies_val, 0.85);
    }

    // 5. Ellipsoidal / Leko / Window Gobo mask
    uint gobo_id = (L_gpu.flags_and_type >> 17u) & 0x0Fu;
    if (gobo_id > 0u) {
        float2 g_uv = float2(x_proj, y_proj) / max(z_proj, 0.15) * 1.4;
        mod_atten *= eval_studio_gobo(gobo_id, g_uv, L_gpu.diffusion_softness);
    }

    // 6. Inverse-square studio falloff (clamped gracefully around calibration anchor 2.10m so 1.0 EV stays balanced)
    if (ftype != STUDIO_FIXTURE_NATURAL_SUN_SKY) {
        float d_ratio = 2.10 / max(dist, 0.45);
        mod_atten *= clamp(d_ratio * d_ratio, 0.25, 4.0);
    }
}

float ltc_integrate_edge(float3 v1, float3 v2) {
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float theta_sintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2).z * theta_sintheta;
}

float ltc_clip_integrate_quad(float3 P0, float3 P1, float3 P2, float3 P3, float3 N, float3 V, float3 P,
                              float3x3 M_inv) {
    float3 T1 = normalize_safe(V - N * dot(V, N), abs(N.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0));
    float3 T2 = cross(N, T1);
    float3x3 world_to_local = float3x3(T1, T2, N);
    float3x3 M = mul(M_inv, world_to_local);

    float3 L0 = normalize(mul(M, P0 - P));
    float3 L1 = normalize(mul(M, P1 - P));
    float3 L2 = normalize(mul(M, P2 - P));
    float3 L3 = normalize(mul(M, P3 - P));

    float sum = 0.0;
    sum += ltc_integrate_edge(L0, L1);
    sum += ltc_integrate_edge(L1, L2);
    sum += ltc_integrate_edge(L2, L3);
    sum += ltc_integrate_edge(L3, L0);
    return max(0.0, abs(sum) * 0.15915494);
}

// Evaluates polygonal Softbox / Strip / Octa / Tube / Ring LTC response (0% noise!) for both diffuse & specular
void eval_ltc_studio_radiance(StudioLightGPU L_gpu, GpuFrameGlobals fg, float3 P, float3 N, float3 V,
                              float roughness, out float out_diff, out float out_spec) {
    float ndv = clamp(dot(N, V), 0.01, 0.999);
    float2 uv_lut = float2(sqrt(clamp(roughness, 0.04, 1.0)), 1.0 - ndv);
    uv_lut = uv_lut * (63.0 / 64.0) + (0.5 / 64.0);

    float4 minv_d = float4(1.0, 0.0, 1.0, 0.0);
    float4 amp_s = float4(1.0, 0.0, 0.0, 1.0);
    if (fg.studio_slots0.x != 0xFFFFFFFFu && fg.studio_slots0.y != 0xFFFFFFFFu) {
        minv_d = aether_sample_level(fg.studio_slots0.x, 0u, uv_lut, 0.0);
        amp_s = aether_sample_level(fg.studio_slots0.y, 0u, uv_lut, 0.0);
    }

    float3x3 M_inv = float3x3(
        minv_d.x, 0.0, minv_d.y,
        0.0, minv_d.z, 0.0,
        minv_d.w, 0.0, 1.0
    );
    float3x3 M_id = float3x3(
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    );

    float3 f_dir = normalize_safe(L_gpu.direction_ws, float3(0.0, 0.0, -1.0));
    float3 u_ax = normalize_safe(L_gpu.tangent_u_ws, float3(1.0, 0.0, 0.0));
    float3 v_ax = normalize_safe(cross(f_dir, u_ax), float3(0.0, 1.0, 0.0));
    u_ax = normalize_safe(cross(v_ax, f_dir), u_ax);

    float hx = L_gpu.size_xy_radius.x * 0.5;
    float hy = L_gpu.size_xy_radius.y * 0.5;
    float3 c = L_gpu.position_ws;
    float3 P0 = c - u_ax * hx - v_ax * hy;
    float3 P1 = c + u_ax * hx - v_ax * hy;
    float3 P2 = c + u_ax * hx + v_ax * hy;
    float3 P3 = c - u_ax * hx + v_ax * hy;

    uint ftype = L_gpu.flags_and_type & 0x0Fu;
    float diff_area = ltc_clip_integrate_quad(P0, P1, P2, P3, N, V, P, M_id);
    float spec_area = ltc_clip_integrate_quad(P0, P1, P2, P3, N, V, P, M_inv);

    // Octabox 8-sided inner chamfer adjustment when SoftboxOn
    if (ftype == STUDIO_FIXTURE_OCTABOX && (L_gpu.flags_and_type & STUDIO_FLAG_SOFTBOX_ON) != 0u) {
        float3 P0o = c - u_ax * (hx * 0.92) - v_ax * (hy * 0.38);
        float3 P1o = c + u_ax * (hx * 0.38) - v_ax * (hy * 0.92);
        float3 P2o = c + u_ax * (hx * 0.92) + v_ax * (hy * 0.38);
        float3 P3o = c - u_ax * (hx * 0.38) + v_ax * (hy * 0.92);
        spec_area = 0.5 * (spec_area + ltc_clip_integrate_quad(P0o, P1o, P2o, P3o, N, V, P, M_inv));
    }
    // Broadcast Ring Light annular center cutout modulation
    if (ftype == STUDIO_FIXTURE_BROADCAST_RING) {
        float3 P0i = c - u_ax * (hy * 0.5) - v_ax * (hy * 0.5);
        float3 P1i = c + u_ax * (hy * 0.5) - v_ax * (hy * 0.5);
        float3 P2i = c + u_ax * (hy * 0.5) + v_ax * (hy * 0.5);
        float3 P3i = c - u_ax * (hy * 0.5) + v_ax * (hy * 0.5);
        float inner_spec = ltc_clip_integrate_quad(P0i, P1i, P2i, P3i, N, V, P, M_inv);
        spec_area = max(0.0, spec_area - inner_spec * 0.88);
    }

    out_diff = diff_area;
    out_spec = spec_area * amp_s.x;
}
