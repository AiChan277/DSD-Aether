#include "aether_common.slang"

struct NprVsOut {
    float4 clip_pos : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 view_pos : TEXCOORD1;
    float3 true_normal : TEXCOORD2;
    float3 proxy_normal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    nointerpolation uint material_id : TEXCOORD5;
    nointerpolation uint submesh_flags : TEXCOORD6;
};

static const float kPi = 3.14159265;

// Pass 6 & 8: Live2D-grade 2.5D NPR shading (Opaque -> Cutout -> Blend) + Pass 13 Orbit Orb (§10.5, §10.7).
[shader("vertex")]
NprVsOut vs_main(uint vid : SV_VertexID) {
    NprVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuSkinnedVertex v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, vid);
    GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, g_Pc.param0);

    float z_flat_weight = saturate(v.pos_zflat.w * mat.bounce_tint_zflat.w);
    float3 view_pos = mul_col(fg.view, float4(v.pos_zflat.xyz, 1.0)).xyz;
    o.clip_pos = project_live2d_flat(fg.proj, view_pos, fg.head_pivot_view_and_flatness.z,
                                     z_flat_weight, fg.head_pivot_view_and_flatness.w);
    o.world_pos = v.pos_zflat.xyz;
    o.view_pos = view_pos;
    o.true_normal = normalize_safe(v.true_norm_hair.xyz, float3(0.0, 0.0, 1.0));
    o.proxy_normal = normalize_safe(v.proxy_norm_out.xyz, o.true_normal);
    o.uv = v.uv;
    o.material_id = g_Pc.param0;
    o.submesh_flags = g_Pc.param1;
    return o;
}

[shader("fragment")]
float4 ps_main(NprVsOut i) : SV_Target0 {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, i.material_id);

    float4 tex_lit = g_SampledImages[mat.tex_slots.x].Sample(g_Samplers[kSamplerLinearClamp], i.uv);
    float4 lit_rgba = tex_lit * mat.lit_color;
    float alpha = saturate(lit_rgba.a);

    if ((i.submesh_flags & 32u) != 0u && mat.toon_params.w > 0.001) {
        if (alpha < mat.toon_params.w) {
            discard;
        }
    }

    uint mat_flags = mat.extra_slots.z;
    uint debug_view = fg.state_u32.z;
    if (debug_view == 1u) {
        return float4(lit_rgba.rgb * alpha, alpha);
    }
    if (debug_view == 13u) {
        return float4(0.18, 0.06, 0.02, 0.25);
    }

    // Unlit MToon materials preserve exact analytical emissive + base premul composite (Invariants 1, 4, 5)
    if ((mat_flags & 32u) != 0u) {
        float3 hdr_rgb = (lit_rgba.rgb + mat.emission_and_shift.rgb) * alpha;
        return float4(hdr_rgb, alpha);
    }

    float3 true_n = normalize_safe(i.true_normal, float3(0.0, 0.0, 1.0));
    float3 proxy_n = normalize_safe(i.proxy_normal, true_n);
    float3 view_v = normalize_safe(fg.camera_pos_world.xyz - i.world_pos, float3(0.0, 0.0, 1.0));

    float3 tex_shade_mul = g_SampledImages[mat.tex_slots.y].Sample(g_Samplers[kSamplerLinearClamp], i.uv).rgb;
    float3 shade_col = mat.shade_color.rgb * tex_shade_mul;

    int3 pix = int3(int2(i.clip_pos.xy), 0);
    float rt_shadow = 1.0;
    float rim_occ = 1.0;
    float3 bounce_rgb = float3(0.0, 0.0, 0.0);

    bool is_transparent = (i.submesh_flags & 64u) != 0u;
    if (!is_transparent) {
        uint sh_slot = fg.gbuffer_extra_ids[0].z;
        uint rim_slot = fg.gbuffer_extra_ids[0].w;
        uint bounce_slot = fg.gbuffer_extra_ids[1].x;
        if (sh_slot != 0xFFFFFFFFu) {
            rt_shadow = g_SampledImages[sh_slot].Load(pix).x;
        }
        if (rim_slot != 0xFFFFFFFFu) {
            rim_occ = g_SampledImages[rim_slot].Load(pix).x;
        }
        if (bounce_slot != 0xFFFFFFFFu) {
            float2 screen_uv = i.clip_pos.xy * fg.viewport_size.zw;
            bounce_rgb = g_SampledImages[bounce_slot].SampleLevel(g_Samplers[kSamplerLinearClamp], screen_uv, 0).rgb;
        }
    }

    // Key light & MToon cel ramp / 0-360° SDF face shadow (§10.5)
    uint light_count = fg.state_u32.y;
    GpuLight key_l = fg.lights[0];
    float3 key_dir = float3(0.0, 0.0, 1.0);
    float3 key_col = float3(1.0, 0.96, 0.92);
    if (light_count > 0u && (key_l.role_and_flags.y & 1u) != 0u) {
        key_dir = (key_l.pos_or_dir.w > 0.5) ? normalize_safe(key_l.pos_or_dir.xyz - i.world_pos, float3(0.0, 0.0, 1.0))
                                             : normalize_safe(key_l.pos_or_dir.xyz, float3(0.0, 0.0, 1.0));
        key_col = key_l.color_intensity.rgb;
    }

    float cel_lit = 1.0;
    bool use_sdf_face = ((mat_flags & 2u) != 0u) && (mat.tex_slots.z != 0xFFFFFFFFu);
    if (use_sdf_face) {
        float2 head_fwd_xz = normalize_safe(float3(fg.head_fwd_world.x, 0.0, fg.head_fwd_world.z),
                                            float3(0.0, 0.0, 1.0)).xz;
        float2 head_right_xz = normalize_safe(float3(fg.head_right_world.x, 0.0, fg.head_right_world.z),
                                              float3(1.0, 0.0, 0.0)).xz;
        float2 l_xz = key_dir.xz;
        float l_xz_len = length(l_xz);
        float2 l_horiz = (l_xz_len > 1e-5) ? (l_xz / l_xz_len) : head_fwd_xz;

        float lx = dot(l_horiz, head_right_xz);
        float lz = clamp(dot(l_horiz, head_fwd_xz), -1.0, 1.0);
        float2 mir_uv = (lx >= 0.0) ? i.uv : float2(1.0 - i.uv.x, i.uv.y);
        float sdf_val = g_SampledImages[mat.tex_slots.z].Sample(g_Samplers[kSamplerLinearClamp], mir_uv).r;
        float thresh = acos(lz) / kPi;
        float feather = max(mat.toon_params.z, fg.light_params.x);
        cel_lit = smoothstep(thresh - feather, thresh + feather, sdf_val);
    } else {
        float ndl = dot(proxy_n, key_dir);
        float shift = mat.emission_and_shift.w;
        float half_soft = max(1.0 - mat.toon_params.x, 0.04) * 0.5;
        cel_lit = smoothstep(shift - half_soft, shift + half_soft, ndl) * rt_shadow;
    }

    // Read-only D32_S8 stencil hair drop shadow (§10.4, §10.5)
    bool recv_hair_shadow = ((mat_flags & 8u) != 0u) && ((mat_flags & 4u) == 0u);
    uint stencil_slot = fg.gbuffer_extra_ids[0].y;
    if (recv_hair_shadow && !is_transparent && stencil_slot != 0xFFFFFFFFu && fg.head_fwd_world.w > 0.0) {
        float3 l_view = mul_col3(fg.view, key_dir);
        float ref_dist = max(-fg.head_pivot_view_and_flatness.z, 0.25);
        float cur_dist = max(-i.view_pos.z, 0.25);
        float scale_res = max(fg.viewport_size.y / 1080.0, 1.0);
        float off_px = fg.head_fwd_world.w * scale_res * clamp(ref_dist / cur_dist, 0.5, 1.6);
        int2 caster_p = int2(i.clip_pos.xy) + int2(round(float2(l_view.x, -l_view.y) * off_px));
        caster_p = clamp(caster_p, int2(0, 0), int2(int(fg.viewport_size.x) - 1, int(fg.viewport_size.y) - 1));
        uint caster_stencil = g_UintSampledImages[stencil_slot].Load(int3(caster_p, 0)).x;
        if ((caster_stencil & 2u) != 0u) {
            cel_lit *= saturate(1.0 - fg.head_right_world.w);
        }
    }

    // Soft Fill & GI equalization
    float3 indirect = float3(0.08, 0.08, 0.10);
    if (light_count > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u) {
        float3 fill_dir = normalize_safe(fg.lights[1].pos_or_dir.xyz, float3(0.0, 1.0, 0.0));
        float fill_w = saturate(dot(proxy_n, fill_dir) * 0.5 + 0.5);
        indirect += fg.lights[1].color_intensity.rgb * (fill_w * 0.45);
    }
    indirect += bounce_rgb * mat.hair_specular_params.w;

    float3 surf_base = lerp(shade_col, lit_rgba.rgb, cel_lit);
    float3 direct_term = surf_base * (key_col * lerp(0.55, 1.0, cel_lit) + indirect);

    // Occluded Fresnel parametric rim
    float ndv = saturate(dot(true_n, view_v));
    float fresnel = pow(1.0 - ndv, max(mat.rim_color_power.w, 1.0));
    float3 rim_term = mat.rim_color_power.rgb * (fresnel * rim_occ);

    // Matcap & Hair Angel-Ring specular
    float3 matcap_term = float3(0.0, 0.0, 0.0);
    if (mat.toon_params.y > 0.001 && mat.tex_slots.w != 0xFFFFFFFFu) {
        float3 vn = normalize_safe(mul_col3(fg.view, true_n), float3(0.0, 0.0, 1.0));
        float2 mc_uv = vn.xy * 0.5 + 0.5;
        matcap_term = g_SampledImages[mat.tex_slots.w].Sample(g_Samplers[kSamplerLinearClamp], mc_uv).rgb * mat.toon_params.y;
    }

    float3 angel_term = float3(0.0, 0.0, 0.0);
    if ((i.submesh_flags & 6u) != 0u && light_count > 3u && (fg.lights[3].role_and_flags.y & 1u) != 0u) {
        float3 hdir = normalize_safe(normalize_safe(fg.lights[3].pos_or_dir.xyz, float3(0.0, 1.0, 0.0)) + view_v,
                                     float3(0.0, 1.0, 0.0));
        float ndh = saturate(dot(true_n, hdir));
        float ring = smoothstep(0.92, 0.985, ndh);
        angel_term = fg.lights[3].color_intensity.rgb * (ring * 0.35);
    }

    float3 hdr = direct_term + rim_term + matcap_term + angel_term + mat.emission_and_shift.rgb;
    return float4(hdr * alpha, alpha);
}

// Pass 13 helper: 128x128 3D interactive Light Orbit Orb shader (`§12.7`).
[shader("vertex")]
NprVsOut vs_orb(uint vid : SV_VertexID) {
    NprVsOut o = {};
    float2 quad[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    o.clip_pos = float4(quad[vid % 3u], 0.0, 1.0);
    o.uv = o.clip_pos.xy * 0.5 + 0.5;
    return o;
}

[shader("fragment")]
float4 ps_orb(NprVsOut i) : SV_Target0 {
    float2 p = i.uv * 2.0 - 1.0;
    p.y = -p.y;
    float r2 = dot(p, p);
    if (r2 > 0.90) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float radius = 0.88;
    float2 q = p / radius;
    float q2 = dot(q, q);
    float edge_aa = smoothstep(1.0, 0.94, q2);
    if (edge_aa <= 0.001) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float3 n = float3(q.x, q.y, sqrt(max(0.0, 1.0 - q2)));
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    float3 key_dir = normalize_safe(fg.lights[0].pos_or_dir.xyz, float3(0.42, 0.56, 0.71));
    float3 key_col = fg.lights[0].color_intensity.rgb;

    float cel = smoothstep(-0.08, 0.12, dot(n, key_dir));
    float3 col = lerp(float3(0.16, 0.19, 0.26), float3(0.76, 0.80, 0.88) * clamp(key_col, 0.2, 1.4), cel);
    float3 h = normalize(key_dir + float3(0.0, 0.0, 1.0));
    col += key_col * pow(saturate(dot(n, h)), 36.0) * 0.45;
    // Key light directional dot handle
    float dot_dist = length(q - key_dir.xy * 0.82);
    col = lerp(col, float3(0.24, 0.55, 0.96), smoothstep(0.16, 0.10, dot_dist));
    return float4(linear_to_srgb(col) * edge_aa, edge_aa);
}
