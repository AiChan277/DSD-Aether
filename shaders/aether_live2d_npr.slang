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

    // Mask emissive factor with tex_emissive (extra_slots.y) when bound; unflagged materials (0xFFFFFFFFu)
    // preserve direct analytical emissive composition (Invariant 5).
    float3 emissive_map = float3(1.0, 1.0, 1.0);
    if (mat.extra_slots.y != 0xFFFFFFFFu) {
        emissive_map = g_SampledImages[mat.extra_slots.y].Sample(g_Samplers[kSamplerLinearClamp], i.uv).rgb;
    }
    float3 emissive_term = mat.emission_and_shift.rgb * emissive_map;

    // Unlit MToon materials preserve exact analytical emissive + base premul composite (Invariants 1, 4, 5)
    if ((mat_flags & 32u) != 0u) {
        float3 hdr_rgb = (lit_rgba.rgb + emissive_term) * alpha;
        return float4(hdr_rgb, alpha);
    }

    float face_norm_flatten = saturate(fg.npr_style_params.x);
    float cel_feather = max(fg.npr_style_params.y, 0.008);
    float shadow_shift = saturate(fg.npr_style_params.z);
    float hair_hl_intensity = max(fg.npr_style_params2.y, 0.0);
    float rim_step_strength = max(fg.npr_style_params2.z, 0.0);

    bool is_face = ((mat_flags & 2u) != 0u) || ((i.submesh_flags & 1u) != 0u);
    bool is_skin = is_face || ((mat_flags & 8u) != 0u);
    bool is_hair = ((mat_flags & 4u) != 0u) || ((i.submesh_flags & 6u) != 0u);

    float3 true_n = normalize_safe(i.true_normal, float3(0.0, 0.0, 1.0));
    float3 proxy_n = normalize_safe(i.proxy_normal, true_n);
    float3 view_v = normalize_safe(fg.camera_pos_world.xyz - i.world_pos, float3(0.0, 0.0, 1.0));
    // Flatten face/skin lighting normals strongly toward camera so 3D nose/neck triangles never shadow harshly
    float norm_flat_k = is_skin ? face_norm_flatten : (face_norm_flatten * 0.22);
    float3 n_lit = normalize_safe(lerp(proxy_n, view_v, norm_flat_k), proxy_n);

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

    // Key light & quantized 2D Live2D/MToon cel ramp + 0-360° SDF face shadow (§10.5)
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
    float deep_shadow = 0.0;
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
        float feather = clamp(min(max(mat.toon_params.z, cel_feather * 2.0), fg.light_params.x), 0.015, 0.12);
        cel_lit = smoothstep(thresh - feather, thresh + feather, sdf_val);
    } else {
        float ndl = dot(n_lit, key_dir);
        float shift = mat.emission_and_shift.w;
        float half_soft = clamp(cel_feather * max(1.35 - mat.toon_params.x * 0.55, 0.45), 0.008, 0.12);
        float band1 = smoothstep(shift - half_soft, shift + half_soft, ndl) * rt_shadow;
        float band2 = smoothstep(shift - 0.34 - half_soft, shift - 0.34 + half_soft, ndl);
        cel_lit = band1;
        deep_shadow = (1.0 - band2) * (1.0 - band1);
    }

    // Razor-crisp D32_S8 stencil forehead bang drop shadow (§10.4, §10.5)
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
            cel_lit = min(cel_lit, saturate(1.0 - fg.head_right_world.w * 1.15));
        }
    }

    // Energy-conserving Fill, bounce & Live2D/MToon chromatic cel-shading (§10.5):
    float3 indirect = float3(0.08, 0.08, 0.11);
    if (light_count > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u) {
        float3 fill_dir = normalize_safe(fg.lights[1].pos_or_dir.xyz, float3(0.0, 1.0, 0.0));
        float fill_w = saturate(dot(n_lit, fill_dir) * 0.5 + 0.5);
        indirect += fg.lights[1].color_intensity.rgb * (fill_w * 0.30);
    }
    indirect += bounce_rgb * (mat.hair_specular_params.w * 0.35);

    // Chromatic Anime Shadow Tinting (never flat muddy gray):
    // Skin -> warm coral/peach-pink; bright/white clothes -> cool periwinkle/lavender; colored cloth/hair -> rich cool shift
    float lit_luma = max(luminance(lit_rgba.rgb), 1e-4);
    float sh_luma = luminance(shade_col);
    bool warm_skin_hue = is_skin || (lit_rgba.r > lit_rgba.b * 1.12 && lit_rgba.r > 0.22);
    bool bright_neutral = (lit_luma > 0.34) && ((lit_rgba.r - lit_rgba.b) < 0.14);
    float3 skin_sh = lit_rgba.rgb * float3(0.95, 0.72, 0.68);
    float3 cool_sh = lit_rgba.rgb * float3(0.76, 0.78, 0.94);
    float3 deep_sh = pow(max(lit_rgba.rgb, float3(1e-4, 1e-4, 1e-4)), float3(1.16, 1.22, 1.10)) *
                     float3(0.84, 0.80, 0.94);
    float3 chroma_tint = warm_skin_hue ? skin_sh : (bright_neutral ? cool_sh : deep_sh);

    float3 eff_shade = shade_col;
    if (sh_luma > lit_luma * 0.82) {
        eff_shade = lerp(shade_col, chroma_tint, shadow_shift);
    } else {
        eff_shade = lerp(shade_col, min(shade_col * float3(0.98, 0.96, 1.02), chroma_tint), shadow_shift * 0.45);
    }
    eff_shade *= lerp(1.0, 0.86, deep_shadow);

    float3 raw_lit = key_col + indirect * 0.22;
    float lit_peak = max(raw_lit.r, max(raw_lit.g, raw_lit.b));
    float over = max(lit_peak - 0.95, 0.0);
    float capped_peak = min(lit_peak, 0.95) + 0.18 * (over / (0.40 + over));
    float3 lit_irradiance = raw_lit * (capped_peak / max(lit_peak, 1e-5));
    float3 shade_irradiance = min(indirect * 0.62 + key_col * 0.36, float3(0.90, 0.90, 0.92));

    float3 direct_term = lerp(eff_shade * shade_irradiance, lit_rgba.rgb * lit_irradiance, cel_lit);

    // 2D Stepped Rim Light (sharp graphic anime edge along light direction + subtle base rim)
    float ndv = saturate(dot(true_n, view_v));
    float fresnel_soft = pow(1.0 - ndv, max(mat.rim_color_power.w, 1.0));
    float rim_step = smoothstep(0.68, 0.72, 1.0 - ndv);
    float rim_light_dir = saturate(dot(n_lit, key_dir) * 0.5 + 0.55) * lerp(0.35, 1.0, rt_shadow);
    float3 rim_col_base = max(mat.rim_color_power.rgb, key_col * 0.28);
    float3 rim_term = (rim_col_base * (rim_step * rim_occ * rim_light_dir * rim_step_strength)) +
                      (mat.rim_color_power.rgb * (fresnel_soft * rim_occ * 0.25));

    // Matcap & Stylized 2D Stepped Hair Angel-Ring highlight (glides smoothly with head tilt)
    float3 matcap_term = float3(0.0, 0.0, 0.0);
    if (mat.toon_params.y > 0.001 && mat.tex_slots.w != 0xFFFFFFFFu) {
        float3 vn = normalize_safe(mul_col3(fg.view, true_n), float3(0.0, 0.0, 1.0));
        float2 mc_uv = vn.xy * 0.5 + 0.5;
        matcap_term = g_SampledImages[mat.tex_slots.w].Sample(g_Samplers[kSamplerLinearClamp], mc_uv).rgb * mat.toon_params.y;
    }

    float3 angel_term = float3(0.0, 0.0, 0.0);
    if (is_hair && light_count > 3u && (fg.lights[3].role_and_flags.y & 1u) != 0u && hair_hl_intensity > 0.001) {
        float3 vn = normalize_safe(mul_col3(fg.view, true_n), float3(0.0, 0.0, 1.0));
        float3 l3v = normalize_safe(mul_col3(fg.view, normalize_safe(fg.lights[3].pos_or_dir.xyz, float3(0.0, 1.0, 0.0))),
                                    float3(0.0, 1.0, 0.0));
        float tilt_x = vn.x - fg.head_fwd_world.x * 0.30;
        float arc_y = vn.y + 0.045 * cos(tilt_x * 16.0);
        float3 band_n = normalize_safe(float3(vn.x * 0.28, arc_y, vn.z), vn);
        float3 band_l = normalize_safe(float3(l3v.x * 0.25, l3v.y + 0.18, max(l3v.z, 0.75)), float3(0.0, 0.45, 0.89));
        float ring_sig = saturate(dot(band_n, band_l));
        float ring_step = smoothstep(0.935, 0.945, ring_sig);
        float3 hl_tint = lerp(float3(1.0, 1.0, 1.0), lit_rgba.rgb * 1.45 + 0.25, 0.35);
        angel_term = fg.lights[3].color_intensity.rgb * hl_tint *
                     (ring_step * 0.32 * hair_hl_intensity * lerp(0.45, 1.0, cel_lit));
    }

    float3 hdr = direct_term + rim_term + matcap_term + angel_term + emissive_term;
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
