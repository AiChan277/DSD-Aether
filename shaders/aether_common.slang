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
