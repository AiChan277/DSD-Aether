#include "aether_common.slang"

struct NprVsOut {
    float4 clip_pos : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 view_pos : TEXCOORD1;
    float4 true_norm_and_crease : TEXCOORD2; // xyz = true_normal, w = crease_weight
    float3 proxy_normal : TEXCOORD3;
    float2 uv : TEXCOORD4;
    nointerpolation uint material_id : TEXCOORD5;
    nointerpolation uint submesh_flags : TEXCOORD6;
};

static const float kPi = 3.14159265;

float eval_cornea_catchlight(float fix_id, float2 lv, float emitter_aspect, float coaxial) {
    // lv is reflection offset in normalized fixture-angular space (divided by eye_catchlight_size)
    if (fix_id < 0.5) {
        // 0: ARRI SkyPanel S60-C (64.5 cm x 30.0 cm soft diffusion rectangle)
        float hw = 0.135;
        float hh = hw / max(emitter_aspect, 1.0);
        float d = max(abs(lv.x) / hw, abs(lv.y) / hh);
        return 1.0 - smoothstep(0.75, 1.02, d);
    }
    if (fix_id < 1.5) {
        // 1: ARRI True Blue T2 / Orbiter Fresnel Spot (circular soft-edged lens)
        return 1.0 - smoothstep(0.075, 0.105, length(lv));
    }
    if (fix_id < 2.5) {
        // 2: Dedolight DLED7 Precision Accent Spot (crisp pinpoint spot)
        return 1.0 - smoothstep(0.040, 0.062, length(lv));
    }
    if (fix_id < 3.5 || coaxial > 0.5) {
        // 3: Broadcast Ring Light (annular ring reflection around optical eye axis)
        float r = length(lv);
        return 1.0 - smoothstep(0.024, 0.048, abs(r - 0.21));
    }
    if (fix_id < 4.5) {
        // 4: Natural Sunlight + CIE Sky Dome (bright solar disc + soft sky shimmer)
        float r = length(lv);
        return saturate((1.0 - smoothstep(0.032, 0.055, r)) + 0.18 * (1.0 - smoothstep(0.06, 0.16, r)));
    }
    // 5: Natural Indoor Window Portal (tall 1.2 m x 1.8 m window pane)
    float hw = 0.085;
    float hh = hw / clamp(emitter_aspect, 0.5, 0.85);
    float d = max(abs(lv.x) / hw, abs(lv.y) / hh);
    return (1.0 - smoothstep(0.78, 1.05, d)) * 0.90;
}

// Pass 6 & 8: Live2D-grade 2.5D NPR shading (Opaque -> Cutout -> Blend) + Pass 13 Orbit Orb (§10.5, §10.7).
[shader("vertex")]
NprVsOut vs_main(uint vid : SV_VertexID) {
    NprVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuSkinnedVertex v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, vid);
    GpuMaterial mat = {};
    if (g_Pc.aux_buf_bda != 0ul) {
        mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, g_Pc.param0);
    } else {
        mat.bounce_tint_zflat = float4(1.0, 1.0, 1.0, 1.0);
    }

    float z_flat_weight = saturate(v.pos_zflat.w * mat.bounce_tint_zflat.w);
    float3 view_pos = mul_col(fg.view, float4(v.pos_zflat.xyz, 1.0)).xyz;
    o.clip_pos = project_live2d_flat(fg.proj, view_pos, fg.head_pivot_view_and_flatness.z,
                                     z_flat_weight, fg.head_pivot_view_and_flatness.w);
    o.world_pos = v.pos_zflat.xyz;
    o.view_pos = view_pos;
    float3 t_norm = normalize_safe(v.true_norm_hair.xyz, float3(0.0, 0.0, 1.0));
    o.true_norm_and_crease = float4(t_norm, saturate(v.tangent_crease.w));
    o.proxy_normal = normalize_safe(v.proxy_norm_out.xyz, t_norm);
    o.uv = v.uv;
    o.material_id = g_Pc.param0;
    o.submesh_flags = g_Pc.param1;
    return o;
}

[shader("fragment")]
float4 ps_main(NprVsOut i, bool is_front_face : SV_IsFrontFace) : SV_Target0 {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuMaterial mat = {};
    if (g_Pc.aux_buf_bda != 0ul) {
        mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, i.material_id);
    }

    float4 tex_lit = aether_sample(mat.tex_slots.x, kSamplerLinearClamp, i.uv);
    float4 lit_rgba = tex_lit * mat.lit_color;
    float alpha = saturate(lit_rgba.a);

    bool is_cutout = (i.submesh_flags & 32u) != 0u;
    bool is_transparent = (i.submesh_flags & 64u) != 0u;
    if (is_cutout && mat.toon_params.w > 0.001) {
        // Derivative-scaled analytical alpha AA for cutout lace, ribbons, and eyelashes
        alpha = saturate((alpha - mat.toon_params.w) / max(fwidth(alpha), 1e-4) + 0.5);
        if (alpha < 0.05) {
            discard;
        }
    }

    uint mat_flags = mat.extra_slots.z;
    uint debug_view = fg.state_u32.z;
    if (debug_view == 1u) {
        return float4(lit_rgba.rgb * alpha, alpha);
    }
    if (debug_view == 5u) {
        return float4(0.18, 0.06, 0.02, 0.25);
    }

    float3 emissive_map = float3(1.0, 1.0, 1.0);
    if (mat.extra_slots.y != 0xFFFFFFFFu) {
        emissive_map = aether_sample(mat.extra_slots.y, kSamplerLinearClamp, i.uv).rgb;
    }
    float3 emissive_term = mat.emission_and_shift.rgb * emissive_map;

    // Unlit MToon materials preserve exact analytical emissive + base premul composite
    if ((mat_flags & 32u) != 0u) {
        float3 hdr_rgb = (lit_rgba.rgb + emissive_term) * alpha;
        return float4(hdr_rgb, alpha);
    }

    float face_norm_flatten = saturate(fg.npr_style_params.x);
    float cel_feather = clamp(fg.npr_style_params.y, 0.008, 0.20);
    float shadow_shift = saturate(fg.npr_style_params.z);
    float hair_hl_intensity = max(fg.npr_style_params2.y, 0.0);
    float rim_step_strength = max(fg.npr_style_params2.z, 0.0);
    uint lighting_mode = (fg.state_u32.w >> 10u) & 3u;
    float k_gi = max(fg.hair_velocity_ws.w, 0.0);

    uint rt_class = (mat_flags >> 10u) & 3u;
    bool is_face = ((mat_flags & 2u) != 0u) || ((i.submesh_flags & 1u) != 0u) || (rt_class == 0u);
    bool is_hair = ((mat_flags & 4u) != 0u) || ((i.submesh_flags & 6u) != 0u) || (rt_class == 1u);
    bool is_eye_iris = ((mat_flags & (1u << 12u)) != 0u);
    bool is_body_skin = !is_face && ((mat_flags & (1u << 13u)) != 0u);
    float skin_chroma_w = is_body_skin ? compute_body_skin_texel_weight(lit_rgba.rgb, fg.face_skin_ref_rgb) : 0.0;
    float neck_w = is_body_skin ? compute_neck_seam_blend(i.world_pos, fg) : 0.0;
    float neck_skin_w = neck_w * skin_chroma_w;
    bool is_skin_pixel = is_face || (is_body_skin && (skin_chroma_w > 0.35));

    // Orient double-sided backfaces correctly so inner back-hair and hems never ignite false silhouette rims.
    float3 true_n = normalize_safe(i.true_norm_and_crease.xyz, float3(0.0, 0.0, 1.0));
    float3 proxy_n = normalize_safe(i.proxy_normal, true_n);
    if (!is_front_face) {
        true_n = -true_n;
        proxy_n = -proxy_n;
    }

    // Rule A & C ([TESTING] Disguise Low-Poly Creases) + Unified Head-Neck-Chest C-inf Normal Field
    float crease_disguise = saturate(fg.eye_catchlight_params.z);
    float crease_w = saturate(i.true_norm_and_crease.w);
    float3 shade_n = (!is_face && crease_disguise > 0.001)
                         ? normalize_safe(lerp(true_n, proxy_n, crease_disguise), true_n)
                         : true_n;
    float3 unified_skin_n = compute_unified_skin_normal(i.world_pos, true_n, fg);
    if (is_skin_pixel) {
        if (is_face) {
            float chin_y = (fg.chin_pos_ws.w > 0.5) ? fg.chin_pos_ws.y : (fg.neck_seam_head_str.y - 0.045);
            float jaw_w = 1.0 - smoothstep(chin_y - 0.015, chin_y + 0.055, i.world_pos.y);
            shade_n = normalize_safe(lerp(proxy_n, unified_skin_n, max(0.35, jaw_w)), proxy_n);
        } else {
            float seam_or_skin = max(neck_skin_w, 0.55);
            shade_n = normalize_safe(lerp(shade_n, unified_skin_n, seam_or_skin), shade_n);
        }
    }

    float3 view_v = normalize_safe(fg.camera_pos_world.xyz - i.world_pos, float3(0.0, 0.0, 1.0));
    float norm_flat_k = is_skin_pixel ? face_norm_flatten : 0.0;
    float3 n_lit_base = is_skin_pixel ? unified_skin_n : shade_n;
    if (is_face) {
        float chin_y = (fg.chin_pos_ws.w > 0.5) ? fg.chin_pos_ws.y : (fg.neck_seam_head_str.y - 0.045);
        float upper_face_w = smoothstep(chin_y - 0.01, chin_y + 0.05, i.world_pos.y);
        n_lit_base = normalize_safe(lerp(unified_skin_n, proxy_n, 0.65 * upper_face_w), unified_skin_n);
    }
    float3 n_lit = normalize_safe(lerp(n_lit_base, view_v, norm_flat_k), n_lit_base);

    // Sample authentic MToon _ShadeTexture * _ShadeColor
    float3 tex_shade_mul = aether_sample(mat.tex_slots.y, kSamplerLinearClamp, i.uv).rgb;
    float3 shade_col = mat.shade_color.rgb * tex_shade_mul;

    // VRoid baked upper-neck texture shadow compensation (strictly gated on upper-neck skin chroma)
    if (is_body_skin && neck_skin_w > 0.001 && fg.face_skin_ref_rgb.w > 0.5) {
        float3 face_ref = fg.face_skin_ref_rgb.rgb;
        float ref_luma = max(luminance(face_ref), 1e-4);
        float px_luma = max(luminance(lit_rgba.rgb), 1e-4);
        float darken_ratio = saturate((ref_luma - px_luma) / max(ref_luma, 0.05));
        float chroma_shift = saturate(length(lit_rgba.rgb - face_ref) * 1.5);
        float lift_w =
            neck_skin_w * saturate(max(smoothstep(0.01, 0.14, darken_ratio), smoothstep(0.04, 0.18, chroma_shift)));
        float3 de_shadowed = lerp(lit_rgba.rgb, max(lit_rgba.rgb, face_ref), lift_w);
        float3 ratio = de_shadowed / max(lit_rgba.rgb, float3(0.02, 0.02, 0.02));
        lit_rgba.rgb = de_shadowed;
        shade_col = min(shade_col * ratio, max(shade_col, face_ref));
    }

    int2 pix_i = clamp(int2(i.clip_pos.xy), int2(0, 0),
                       int2(max(int(fg.viewport_size.x) - 1, 0), max(int(fg.viewport_size.y) - 1, 0)));
    float rt_shadow = 1.0;
    float blocker_dist_m = 0.45;
    float rim_occ = 1.0;
    float3 bounce_rgb = float3(0.0, 0.0, 0.0);
    float3 aov_backscatter = float3(0.0, 0.0, 0.0);
    float3 aov_cornea_rt = float3(0.0, 0.0, 0.0);
    bool rt_shadow_active = ((fg.state_u32.w & 1u) != 0u);

    if (!is_transparent) {
        uint sh_slot = fg.gbuffer_extra_ids[0].z;
        uint rim_slot = fg.gbuffer_extra_ids[0].w;
        uint bounce_slot = fg.gbuffer_extra_ids[1].x;
        if (sh_slot != 0xFFFFFFFFu) {
            float4 sh4 = aether_load(sh_slot, pix_i);
            rt_shadow = sh4.x;
            blocker_dist_m = (sh4.z > 0.001) ? sh4.z : 0.45;
        }
        if (rim_slot != 0xFFFFFFFFu) {
            rim_occ = aether_load(rim_slot, pix_i).x;
        }
        float2 vp_uv = (float2(pix_i) + 0.5) * fg.viewport_size.zw;
        if (bounce_slot != 0xFFFFFFFFu) {
            bounce_rgb = aether_sample_level(bounce_slot, kSamplerLinearClamp, vp_uv, 0).rgb;
        }
        if (rt_shadow_active && fg.studio_slots1.z != 0xFFFFFFFFu) {
            aov_backscatter = aether_sample_level(fg.studio_slots1.z, kSamplerLinearClamp, vp_uv, 0).rgb;
        }
        if (rt_shadow_active && fg.studio_slots1.w != 0xFFFFFFFFu) {
            aov_cornea_rt = aether_sample_level(fg.studio_slots1.w, kSamplerLinearClamp, vp_uv, 0).rgb;
        }
    }

    uint light_count = fg.state_u32.y;
    uint studio_count = (fg.studio_lights_bda != 0ul) ? min(fg.studio_slots0.w, 16u) : 0u;
    GpuLight key_l = fg.lights[0];
    float3 key_dir = float3(0.0, 0.0, 1.0);
    float3 key_col = float3(1.0, 1.0, 1.0);
    float key_diff_soft = 0.5;
    bool key_softbox_on = true;
    uint surface_ch_bit = is_face ? LIGHT_CH_FACE : (is_hair ? LIGHT_CH_HAIR : LIGHT_CH_BODY);

    if (studio_count > 0u) {
        bool found_key = false;
        key_col = float3(0.0, 0.0, 0.0);
        for (uint ki = 0u; ki < studio_count; ++ki) {
            StudioLightGPU ksl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, ki);
            if ((ksl.flags_and_type & STUDIO_FLAG_ENABLED) != 0u &&
                (ksl.flags_and_type & (STUDIO_FLAG_NEG_FILL | STUDIO_FLAG_BOUNCE_CARD)) == 0u &&
                !found_key) {
                float kd, kmod;
                eval_studio_light_modifiers(ksl, fg, i.world_pos, key_dir, kd, kmod);
                key_diff_soft = ksl.diffusion_softness;
                key_softbox_on = (ksl.flags_and_type & STUDIO_FLAG_SOFTBOX_ON) != 0u;
                key_col = ksl.color_rgb_linear * ksl.intensity_ev100;
                found_key = true;
                break;
            }
        }
    } else if (light_count > 0u && (key_l.role_and_flags.y & 1u) != 0u) {
        key_dir = (key_l.pos_or_dir.w > 0.5)
                      ? normalize_safe(key_l.pos_or_dir.xyz - i.world_pos, float3(0.0, 0.0, 1.0))
                      : normalize_safe(key_l.pos_or_dir.xyz, float3(0.0, 0.0, 1.0));
        key_col = key_l.color_intensity.rgb;
    }

    // Photometric fixture cone & 4-leaf barn-door shaping centered at avatar anchor
    float fix_id = fg.photometric_params0.x;
    float flux_norm = fg.photometric_params1.w;
    float beam_atten = 1.0;
    if (studio_count > 0u) {
        StudioLightGPU ksl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, 0u);
        if ((ksl.flags_and_type & STUDIO_FLAG_ENABLED) != 0u &&
            (ksl.light_channel_mask & surface_ch_bit) != 0u) {
            float3 d_tmp;
            float dist_tmp, mod_tmp;
            eval_studio_light_modifiers(ksl, fg, i.world_pos, d_tmp, dist_tmp, mod_tmp);
            beam_atten = lerp(0.18, 1.0, clamp(mod_tmp, 0.0, 1.35));
        } else if ((ksl.light_channel_mask & surface_ch_bit) == 0u) {
            beam_atten = 0.0;
        }
    } else if (fg.photometric_params0.y > -0.5) {
        float3 anchor_ws = mul_col(fg.inv_view, float4(fg.head_pivot_view_and_flatness.xyz, 1.0)).xyz;
        float3 spot_origin = anchor_ws + key_dir * 1.80;
        float3 to_px = normalize_safe(i.world_pos - spot_origin, -key_dir);
        float radial = smoothstep(fg.photometric_params0.z, fg.photometric_params0.y, dot(to_px, -key_dir));
        float bd_soft = fg.photometric_params1.x;
        if (bd_soft > 0.01) {
            float3 spot_r = normalize_safe(cross(float3(0.0, 1.0, 0.0), key_dir), float3(1.0, 0.0, 0.0));
            float3 spot_u = normalize_safe(cross(key_dir, spot_r), float3(0.0, 1.0, 0.0));
            float3 rel = i.world_pos - anchor_ws;
            float bd_x = 1.0 - smoothstep(0.45, 0.90, abs(dot(rel, spot_r)));
            float bd_y = 1.0 - smoothstep(0.60, 1.15, abs(dot(rel, spot_u)));
            radial *= lerp(1.0, bd_x * bd_y, bd_soft);
        }
        beam_atten = lerp(0.18, 1.0, radial);
    }

    // Single-Pass Studio Multi-Light Accumulation (up to 16 StudioLightGPU via LTC + Modifiers)
    float3 primary_diff_acc = float3(0.0, 0.0, 0.0);
    float3 accent_diff_acc = float3(0.0, 0.0, 0.0);
    float3 accent_spec_acc = float3(0.0, 0.0, 0.0);
    float3 multi_catchlight_acc = float3(0.0, 0.0, 0.0);

    if (studio_count > 0u) {
        for (uint li = 0u; li < studio_count; ++li) {
            StudioLightGPU sl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, li);
            bool enabled = (sl.flags_and_type & STUDIO_FLAG_ENABLED) != 0u;
            bool is_passive = (sl.flags_and_type & (STUDIO_FLAG_NEG_FILL | STUDIO_FLAG_BOUNCE_CARD)) != 0u;
            if (!enabled || is_passive || (sl.light_channel_mask & surface_ch_bit) == 0u) {
                continue;
            }
            float3 ldir;
            float ldist, lmod;
            eval_studio_light_modifiers(sl, fg, i.world_pos, ldir, ldist, lmod);
            if (lmod <= 0.002) {
                continue;
            }
            float diff_ltc = 0.0;
            float spec_ltc = 0.0;
            float mat_rough = is_eye_iris ? 0.04 : (is_hair ? 0.24 : (is_skin_pixel ? 0.65 : 0.42));
            eval_ltc_studio_radiance(sl, fg, i.world_pos, n_lit, view_v, mat_rough, diff_ltc, spec_ltc);

            float ndl_s = saturate(dot(n_lit, ldir));
            float geom_term = is_skin_pixel ? 1.0 : disney_shadow_terminator(true_n, n_lit, ldir);
            float e_area = lerp(ndl_s, max(diff_ltc * 1.45, ndl_s * 0.65),
                                ((sl.flags_and_type & STUDIO_FLAG_SOFTBOX_ON) != 0u) ? 0.72 : 0.22);
            float3 l_energy = sl.color_rgb_linear * (sl.intensity_ev100 * lmod * geom_term);

            if (li == 0u) {
                float key_w = is_skin_pixel ? lerp(0.92, 1.0, saturate(e_area)) : max(e_area, 0.35);
                primary_diff_acc += l_energy * key_w;
            } else {
                accent_diff_acc += l_energy * e_area * 0.55;
                if (!is_skin_pixel && !is_eye_iris) {
                    accent_spec_acc += l_energy * spec_ltc * (is_hair ? 0.35 : 0.22);
                }
            }
            if (is_eye_iris && ((sl.light_channel_mask & LIGHT_CH_CATCHLIGHT) != 0u) && sl.catchlight_weight > 0.001) {
                float3 eye_c = (dot(i.world_pos - fg.eye_left_center_r.xyz, i.world_pos - fg.eye_left_center_r.xyz) <=
                                dot(i.world_pos - fg.eye_right_center_r.xyz, i.world_pos - fg.eye_right_center_r.xyz))
                                   ? fg.eye_left_center_r.xyz
                                   : fg.eye_right_center_r.xyz;
                float3 n_radial = normalize_safe(i.world_pos - eye_c, true_n);
                float3 n_cornea = normalize_safe(lerp(true_n, n_radial, 0.72), true_n);
                float c_diff, c_spec;
                float c_rough = ((sl.flags_and_type & STUDIO_FLAG_SOFTBOX_ON) != 0u) ? 0.055 : 0.025;
                eval_ltc_studio_radiance(sl, fg, i.world_pos, n_cornea, view_v, c_rough, c_diff, c_spec);
                multi_catchlight_acc += sl.color_rgb_linear * (c_spec * sl.intensity_ev100 * sl.catchlight_weight * 0.65);
            }
        }
    }

    // Hillaire (2020) SH9 Atmosphere + 3-band ambient + Pre-Cel GI Color Bleed
    float3 sh_ambient = eval_hillaire_sky_sh9(n_lit, fg);
    float3 fill_indirect = float3(0.0, 0.0, 0.0);
    if (light_count > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u) {
        float3 fill_dir = normalize_safe(fg.lights[1].pos_or_dir.xyz, float3(0.0, 1.0, 0.0));
        float fill_w = saturate(dot(n_lit, fill_dir) * 0.5 + 0.5);
        fill_indirect = fg.lights[1].color_intensity.rgb * (fill_w * 0.25);
    }
    float3 cie_sky = lerp(float3(0.75, 0.84, 0.96), float3(0.45, 0.72, 1.00), saturate(shade_n.y * 0.65 + 0.35)) *
                     (0.24 * fg.photometric_params1.y);
    float3 earth_bounce = float3(0.56, 0.46, 0.38) * saturate(-shade_n.y * 0.55 + 0.20) *
                          (0.18 * fg.photometric_params1.z);
    float3 e_bounce = bounce_rgb * max(mat.hair_specular_params.w, 0.35);
    float3 indirect = sh_ambient * ((lighting_mode == 0u) ? 0.44 : 0.30) + fill_indirect + cie_sky + earth_bounce +
                      e_bounce * 0.65 + accent_diff_acc * 0.45;
    if (is_skin_pixel && lighting_mode >= 1u) {
        indirect = max(indirect, sh_ambient * 0.36 + fill_indirect + cie_sky);
    }

    float gi_luma = luminance(e_bounce);
    float l_gi_lift = k_gi * gi_luma * 0.45;
    float ndl_unshadowed = dot(n_lit, key_dir);
    float extra_feather = 0.02 * crease_w * crease_disguise + (key_softbox_on ? 0.025 * key_diff_soft : 0.0);
    float rt_soft = clamp((key_softbox_on ? 0.16 : 0.06) + fg.lights[0].direction_radius.w * 2.1, 0.06, 0.44);

    float cel_lit = 1.0;
    float deep_shadow = 0.0;
    bool use_sdf_face = ((mat_flags & (1u << 15u)) != 0u) && (mat.tex_slots.z != 0xFFFFFFFFu);
    if (is_face && !use_sdf_face) {
        float3 h_fwd = normalize_safe(fg.head_fwd_world.xyz, float3(0.0, 0.0, 1.0));
        float3 h_right = normalize_safe(fg.head_right_world.xyz, float3(1.0, 0.0, 0.0));
        float2 n_h = normalize_safe(float3(dot(n_lit_base, h_right), dot(n_lit_base, h_fwd), 0.0),
                                    float3(0.0, 1.0, 0.0)).xy;
        float2 l_h = float2(dot(key_dir, h_right), dot(key_dir, h_fwd));
        float a_l = (dot(l_h, l_h) > 1e-6) ? atan2(l_h.x, l_h.y) : 0.0;
        static const float kFaceProxySpread = 0.55;
        float a_n = asin(clamp(n_h.x / kFaceProxySpread, -1.0, 1.0));
        float a_face = a_n * (1.0 - 0.5 * face_norm_flatten);
        float d_face = cos(a_face - a_l);
        float feather = clamp(cel_feather + extra_feather, 0.008, 0.055);
        float face_lit = quantize_cel(d_face + l_gi_lift * 0.25, 0.0, feather);
        float face_rt = rt_shadow_active ? smoothstep(0.50 - rt_soft, 0.50 + rt_soft, rt_shadow) : 1.0;
        cel_lit = min(face_lit, face_rt) * beam_atten;
    } else if (use_sdf_face) {
        float2 head_fwd_xz = normalize_safe(float3(fg.head_fwd_world.x, 0.0, fg.head_fwd_world.z),
                                            float3(0.0, 0.0, 1.0)).xz;
        float2 head_right_xz = normalize_safe(float3(fg.head_right_world.x, 0.0, fg.head_right_world.z),
                                              float3(1.0, 0.0, 0.0)).xz;
        float2 l_xz = normalize_safe(float3(key_dir.x, 0.0, key_dir.z), float3(0.0, 0.0, 1.0)).xz;
        float lx = dot(l_xz, head_right_xz);
        float lz = clamp(dot(l_xz, head_fwd_xz), -1.0, 1.0);
        float2 mir_uv = (lx < 0.0) ? float2(1.0 - i.uv.x, i.uv.y) : i.uv;
        float sdf_val = aether_sample(mat.tex_slots.z, kSamplerLinearClamp, mir_uv).r;
        float thresh = max(0.0, acos(lz) / kPi - l_gi_lift * 0.25);
        float feather = clamp(min(max(mat.toon_params.z, cel_feather * 2.0), fg.light_params.x) + extra_feather,
                              0.015, 0.12);
        float sdf_lit = quantize_cel(sdf_val, thresh, feather);
        float face_rt = rt_shadow_active ? smoothstep(0.50 - rt_soft, 0.50 + rt_soft, rt_shadow) : 1.0;
        cel_lit = min(sdf_lit, face_rt) * beam_atten;
    } else {
        float mtoon_shift = mat.emission_and_shift.w;
        float default_vroid_shift = is_skin_pixel ? -0.18 : 0.12;
        float shift = (mtoon_shift < -0.65) ? default_vroid_shift : mtoon_shift;
        float toony = clamp(mat.toon_params.x, 0.0, 1.0);
        float half_soft = clamp(cel_feather * max(1.35 - toony * 0.55, 0.45) + extra_feather, 0.008, 0.15);
        float g_term = is_skin_pixel ? 1.0 : disney_shadow_terminator(true_n, n_lit, key_dir);
        float l_form = ndl_unshadowed * g_term + l_gi_lift * 0.25;
        float form_lit = quantize_cel(l_form, shift, half_soft);
        if (neck_skin_w > 0.001) {
            float3 h_fwd = normalize_safe(fg.head_fwd_world.xyz, float3(0.0, 0.0, 1.0));
            float3 h_right = normalize_safe(fg.head_right_world.xyz, float3(1.0, 0.0, 0.0));
            float2 n_h = normalize_safe(float3(dot(unified_skin_n, h_right), dot(unified_skin_n, h_fwd), 0.0),
                                        float3(0.0, 1.0, 0.0)).xy;
            float2 l_h = float2(dot(key_dir, h_right), dot(key_dir, h_fwd));
            float a_l = (dot(l_h, l_h) > 1e-6) ? atan2(l_h.x, l_h.y) : 0.0;
            static const float kFaceProxySpread = 0.55;
            float a_n = asin(clamp(n_h.x / kFaceProxySpread, -1.0, 1.0));
            float a_face = a_n * (1.0 - 0.5 * face_norm_flatten);
            float d_face = cos(a_face - a_l);
            float face_feather = clamp(cel_feather + extra_feather, 0.008, 0.055);
            float neck_face_lit = quantize_cel(d_face + l_gi_lift * 0.25, 0.0, face_feather);
            form_lit = lerp(form_lit, neck_face_lit, neck_skin_w);
        }
        float cast_lit = rt_shadow_active ? smoothstep(0.50 - rt_soft, 0.50 + rt_soft, rt_shadow) : 1.0;
        cel_lit = min(form_lit, cast_lit) * beam_atten;
        deep_shadow = (1.0 - quantize_cel(l_form, shift - 0.34, half_soft)) * (1.0 - cel_lit) * (1.0 - neck_skin_w);
    }

    // Stencil forehead bang drop-shadow fallback only when RT shadow pass is inactive
    uint depth_slot = fg.gbuffer_extra_ids[0].x;
    uint stencil_slot = fg.gbuffer_extra_ids[0].y;
    bool recv_hair_shadow = ((mat_flags & 8u) != 0u || is_face) && !is_hair && ((mat_flags & 4u) == 0u);
    if (recv_hair_shadow && !rt_shadow_active && !is_transparent && stencil_slot != 0xFFFFFFFFu &&
        fg.head_fwd_world.w > 0.0) {
        float3 l_view = mul_col3(fg.view, key_dir);
        float ref_dist = max(-fg.head_pivot_view_and_flatness.z, 0.25);
        float cur_dist = max(-i.view_pos.z, 0.25);
        float depth_scale = clamp(ref_dist / cur_dist, 0.35, 1.8);
        float off_px = fg.head_fwd_world.w * depth_scale;
        int2 caster_p = pix_i + int2(round(float2(l_view.x, -l_view.y) * off_px));
        caster_p = clamp(caster_p, int2(0, 0), int2(int(fg.viewport_size.x) - 1, int(fg.viewport_size.y) - 1));
        uint caster_stencil = aether_uload(stencil_slot, caster_p).x;
        if ((caster_stencil & 4u) != 0u) {
            bool caster_in_front = true;
            if (depth_slot != 0xFFFFFFFFu) {
                float d_c = aether_load(depth_slot, caster_p).x;
                float z_c = (d_c >= 0.99998) ? (cur_dist + 2.0) : abs(fg.proj.col3.z / max(abs(d_c + fg.proj.col2.z), 1e-5));
                caster_in_front = (z_c < cur_dist - 0.002);
            }
            if (caster_in_front) {
                cel_lit = min(cel_lit, saturate(1.0 - fg.head_right_world.w * 1.15));
            }
        }
    }

    // Authentic MToon _ShadeColor * _ShadeTexture, with chromatic anime harmonic tint only when _ShadeColor == 1
    float lit_luma = max(luminance(lit_rgba.rgb), 1e-4);
    float sh_luma = luminance(shade_col);
    bool has_authored_shade = (sh_luma < lit_luma * 0.88) || (length(shade_col - lit_rgba.rgb) > 0.06);

    float3 base_shade = shade_col;
    if (!has_authored_shade) {
        if (is_skin_pixel) {
            float3 skin_mul = lerp(float3(0.95, 0.87, 0.83), float3(0.89, 0.74, 0.70), shadow_shift);
            base_shade = shade_col * skin_mul;
        } else if (lit_luma > 0.55) {
            // Cream cardigan & bright cloth: clean cool-periwinkle fold tone preserving stitch contrast
            float3 light_mul = lerp(float3(0.84, 0.85, 0.91), float3(0.70, 0.74, 0.87), shadow_shift);
            base_shade = shade_col * light_mul;
        } else {
            // Colored & dark cloth/hair/ribbons: rich chromatic attenuation without grey washout
            float3 dark_pow = pow(max(lit_rgba.rgb, float3(1e-4, 1e-4, 1e-4)), float3(1.10, 1.13, 1.07));
            float3 dark_mul = lerp(float3(0.88, 0.88, 0.94), float3(0.76, 0.78, 0.90), shadow_shift);
            base_shade = min(shade_col, dark_pow) * dark_mul;
        }
    } else {
        float3 sub_cool = lerp(float3(1.0, 1.0, 1.0), float3(0.92, 0.92, 1.02), shadow_shift * 0.35);
        base_shade = shade_col * sub_cool;
    }

    float3 bleed_chroma = normalize_chroma(e_bounce);
    float gi_tint_w = saturate(gi_luma * k_gi * 1.25) * 0.45;
    float3 eff_shade = lerp(base_shade, base_shade * bleed_chroma, gi_tint_w) * lerp(1.0, 0.90, deep_shadow);
    // Boost cloth shadow chroma by +20% in OKLab so bright knitwear / outfits retain rich illustration color
    if (!is_skin_pixel && !is_eye_iris && !is_hair) {
        float3 lab_sh = lin_srgb_to_oklab(eff_shade);
        lab_sh.yz *= 1.20;
        eff_shade = oklab_to_lin_srgb(lab_sh);
    }

    // Energy-conserving COVER Corp Soft-Knee Multi-Light Compression + OKLab Secondary Accent Hue Protection
    float3 primary_base = (studio_count > 0u) ? primary_diff_acc : (key_col * flux_norm);
    float3 raw_lit = primary_base + accent_diff_acc + indirect * 0.14;
    float3 lit_irradiance = compress_soft_knee_diffuse(raw_lit, primary_base, accent_diff_acc);
    float3 shade_irradiance = min(key_col * (0.28 * flux_norm) + indirect * 0.52, float3(0.68, 0.70, 0.76));

    // Pixar RenderMan Shadow Fringe SSS Terminator Band (4 * cel * (1 - cel))
    // Driven by blocker distance (AOV_DirectVis.z) + softbox diffusion; warm coral/amber bleed on skin/thin cloth
    float fringe_band = 4.0 * cel_lit * (1.0 - cel_lit);
    float blocker_soft = saturate(blocker_dist_m * 2.5) * (key_softbox_on ? 1.0 : 0.45);
    float3 fringe_tint = is_skin_pixel ? float3(0.44, 0.14, 0.08) : (is_hair ? float3(0.18, 0.09, 0.06) : float3(0.22, 0.11, 0.07));
    float fringe_str = is_skin_pixel ? (0.36 + 0.25 * blocker_soft) : (0.14 * blocker_soft);
    float3 shadow_fringe_sss = fringe_tint * (fringe_band * fringe_str * luminance(key_col));

    float3 direct_term = lerp(eff_shade * shade_irradiance, lit_rgba.rgb * lit_irradiance, cel_lit) +
                         lit_rgba.rgb * shadow_fringe_sss;

    // Silhouette 2D stepped rim light (restrained inside back hair & !is_front_face interior faces)
    float3 rim_dir_ws = (light_count > 2u && (fg.lights[2].role_and_flags.y & 1u) != 0u)
                            ? normalize_safe(fg.lights[2].pos_or_dir.xyz, key_dir)
                            : key_dir;
    float3 rim_dir_vs = normalize_safe(mul_col3(fg.view, rim_dir_ws), float3(0.4, 0.6, 0.2));
    float2 light_scr = normalize_safe(float3(rim_dir_vs.x, -rim_dir_vs.y, 0.0), float3(1.0, 0.0, 0.0)).xy;
    float depth_edge_rim = 0.0;
    if (!is_transparent && depth_slot != 0xFFFFFFFFu) {
        int2 max_vp = int2(int(fg.viewport_size.x) - 1, int(fg.viewport_size.y) - 1);
        float rim_px = clamp(fg.filter_pro_params.w, 2.0, 4.5);
        int2 rim_p = clamp(pix_i + int2(round(light_scr * rim_px)), int2(0, 0), max_vp);
        float d_off = aether_load(depth_slot, rim_p).x;
        float z_cur = abs(fg.proj.col3.z / max(abs(i.clip_pos.z + fg.proj.col2.z), 1e-5));
        float z_off = (d_off >= 0.99998) ? (z_cur + 2.0) : abs(fg.proj.col3.z / max(abs(d_off + fg.proj.col2.z), 1e-5));
        float rel_jump = (z_off - z_cur) / max(z_cur, 0.15);
        depth_edge_rim = smoothstep(0.016, 0.052, rel_jump);
    }

    float ndv = saturate(dot(shade_n, view_v));
    float fresnel_soft = pow(1.0 - ndv, mat.rim_color_power.w);
    float rim_signal = max(1.0 - ndv, depth_edge_rim * 0.92);
    float rim_step = smoothstep(0.70, 0.76, rim_signal);
    float rim_light_dir = saturate(dot(n_lit, rim_dir_ws) * 0.45 + 0.55) * lerp(0.25, 1.0, rt_shadow);
    float backface_atten = is_front_face ? 1.0 : 0.12;
    float dark_albedo_atten = saturate(lit_luma * 2.5 + 0.35);
    float3 rim_col_base = lerp(mat.rim_color_power.rgb, key_col * 0.28, 0.65);
    float3 rim_term = ((rim_col_base * (rim_step * rim_occ * rim_light_dir * rim_step_strength * 0.65)) +
                       (mat.rim_color_power.rgb * (fresnel_soft * rim_occ * 0.22))) *
                      backface_atten * dark_albedo_atten;

    // Matcap term
    float3 matcap_term = float3(0.0, 0.0, 0.0);
    if (mat.toon_params.y > 0.001 && mat.tex_slots.w != 0xFFFFFFFFu) {
        float3 vn = normalize_safe(mul_col3(fg.view, shade_n), float3(0.0, 0.0, 1.0));
        float2 mc_uv = vn.xy * 0.48 + 0.5;
        matcap_term = aether_sample(mat.tex_slots.w, kSamplerLinearClamp, mc_uv).rgb * mat.toon_params.y;
    }

    // Hybrid Micro-PBR Differentiation:
    // - Cloth / Knitwear: Charlie Velvet Sheen (1 - ndv)^4.5 attenuated on bright fabrics
    // - Silk / Ribbon / Trim (rt_class == 2): tight Anisotropic GGX Specular highlight
    // - Eyes: Lower-Iris Caustic Crescent Glow + LTC/RT Softbox Cornea Catchlights
    // - Hair: Crown-smoothed Kajiya-Kay dual-lobe angel ring + Thin-Slab Backscatter
    float3 micro_pbr_term = float3(0.0, 0.0, 0.0);
    if (!is_skin_pixel && !is_eye_iris && !is_hair) {
        if (rt_class == 2u) {
            float3 h_rib = normalize_safe(key_dir + view_v, view_v);
            float ndh = saturate(dot(shade_n, h_rib));
            float ggx_tight = pow(ndh, 48.0) * 0.32;
            micro_pbr_term = key_col * lerp(lit_rgba.rgb, float3(1.0, 1.0, 1.0), 0.45) * ggx_tight * cel_lit +
                             accent_spec_acc * 0.65;
        } else {
            float bright_atten = smoothstep(0.95, 0.22, lit_luma);
            float charlie = pow(1.0 - ndv, 4.5) * 0.26 * bright_atten;
            micro_pbr_term = lit_rgba.rgb * key_col * charlie * rim_occ + accent_spec_acc * 0.35;
        }
    }

    // Photometric fixture Cornea Catchlight Reflection + Lower-Iris Caustic Crescent Glow
    float3 catchlight_term = float3(0.0, 0.0, 0.0);
    float catch_int = fg.eye_catchlight_params.x;
    if (is_eye_iris && is_front_face) {
        float3 d_l = i.world_pos - fg.eye_left_center_r.xyz;
        float3 d_r = i.world_pos - fg.eye_right_center_r.xyz;
        float3 eye_c = (dot(d_l, d_l) <= dot(d_r, d_r)) ? fg.eye_left_center_r.xyz : fg.eye_right_center_r.xyz;
        float eye_r = max((dot(d_l, d_l) <= dot(d_r, d_r)) ? fg.eye_left_center_r.w : fg.eye_right_center_r.w, 0.008);
        float3 local_eye = (i.world_pos - eye_c) / eye_r;
        // Lower-Iris Caustic Crescent Glow opposite key elevation
        float3 h_up = normalize_safe(cross(fg.head_fwd_world.xyz, fg.head_right_world.xyz), float3(0.0, 1.0, 0.0));
        float lower_band = smoothstep(-0.05, -0.58, dot(local_eye, h_up));
        float caustic_align = 0.65 + 0.35 * saturate(dot(local_eye, key_dir));
        catchlight_term += lit_rgba.rgb * key_col * (lower_band * caustic_align * 0.28);

        if (catch_int > 0.001) {
            float3 n_radial = normalize_safe(i.world_pos - eye_c, true_n);
            float3 n_cornea = normalize_safe(lerp(true_n, n_radial, 0.72), true_n);
            float3 r_dir = reflect(-view_v, n_cornea);

            float3 l_aim = normalize_safe(key_dir + float3(0.0, 0.16, 0.14), view_v);
            float3 l_right = normalize_safe(cross(float3(0.0, 1.0, 0.0), l_aim), float3(1.0, 0.0, 0.0));
            float3 l_up = normalize_safe(cross(l_aim, l_right), float3(0.0, 1.0, 0.0));
            float forward_gate = step(0.15, dot(r_dir, l_aim));
            float2 lv = float2(dot(r_dir, l_right), dot(r_dir, l_up)) / clamp(fg.eye_catchlight_params.y, 0.25, 2.0);
            float shape = eval_cornea_catchlight(fix_id, lv, fg.photometric_params0.w, fg.eye_catchlight_params.w);
            if (!key_softbox_on) {
                // Narrow pinpoint when softbox attachment disabled
                shape = max(shape * 0.25, 1.0 - smoothstep(0.032, 0.052, length(lv)));
            }
            catchlight_term += key_col * (shape * forward_gate * catch_int * 0.85) +
                               max(multi_catchlight_acc, aov_cornea_rt) * (0.45 * catch_int);
        }
    }

    // Kajiya-Kay / Anisotropic Hair Angel Ring (crown-smoothed tangent flow + thin-slab Backscatter)
    float3 angel_term = float3(0.0, 0.0, 0.0);
    if (is_hair && is_front_face && hair_hl_intensity > 0.001) {
        float3 dp_dx = ddx(i.world_pos);
        float3 dp_dy = ddy(i.world_pos);
        float2 duv_dx = ddx(i.uv);
        float2 duv_dy = ddy(i.uv);
        float det = duv_dx.x * duv_dy.y - duv_dy.x * duv_dx.y;
        float3 h_up = normalize_safe(cross(fg.head_fwd_world.xyz, fg.head_right_world.xyz), float3(0.0, 1.0, 0.0));
        float3 apex = fg.neck_seam_head_str.xyz + h_up * 0.14;
        float3 crown_dir = normalize_safe(apex - i.world_pos, h_up);
        float3 crown_tangent = normalize_safe(crown_dir - true_n * dot(crown_dir, true_n), h_up);
        float3 t_crown_tip = crown_tangent;
        if (abs(det) > 1e-8) {
            float3 bitan = (-duv_dy.x * dp_dx + duv_dx.x * dp_dy) / det;
            float3 t_uv = normalize_safe(bitan - true_n * dot(bitan, true_n), crown_tangent);
            // Smoothly replace jagged UV facets at coarse crown blocks with continuous azimuthal crown flow
            float crown_w = clamp(smoothstep(0.20, 0.72, dot(true_n, h_up)), 0.45, 0.90);
            t_crown_tip = normalize_safe(lerp(t_uv, crown_tangent, crown_w), crown_tangent);
        }
        float strand_shift = 0.085 * sin(i.uv.x * 52.0) + 0.04 * cos(i.uv.x * 104.0);
        float3 hair_vel = clamp(fg.hair_velocity_ws.xyz, float3(-1.5, -1.5, -1.5), float3(1.5, 1.5, 1.5));
        float3 t_shift1 = normalize_safe(t_crown_tip + true_n * strand_shift + hair_vel * 0.18, t_crown_tip);
        float3 t_shift2 = normalize_safe(t_crown_tip + true_n * (strand_shift - 0.14) + hair_vel * 0.24, t_crown_tip);

        float3 hair_l_dir = (light_count > 3u && (fg.lights[3].role_and_flags.y & 1u) != 0u)
                                ? normalize_safe(fg.lights[3].pos_or_dir.xyz, key_dir)
                                : key_dir;
        float3 hair_l_col = (light_count > 3u && (fg.lights[3].role_and_flags.y & 1u) != 0u)
                                ? fg.lights[3].color_intensity.rgb
                                : key_col;

        float3 h1 = normalize_safe(hair_l_dir + view_v, view_v);
        float3 h2 = normalize_safe(rim_dir_ws + view_v, view_v);
        float sin1 = sqrt(max(0.0, 1.0 - dot(t_shift1, h1) * dot(t_shift1, h1)));
        float sin2 = sqrt(max(0.0, 1.0 - dot(t_shift2, h2) * dot(t_shift2, h2)));
        float lobe1 = pow(sin1, 34.0);
        float lobe2 = pow(sin2, 18.0) * 0.52;

        float3 vn = normalize_safe(mul_col3(fg.view, true_n), float3(0.0, 0.0, 1.0));
        float tilt_arch = smoothstep(0.10, 0.75, vn.y + 0.12 * cos((vn.x - fg.head_fwd_world.x * 0.30) * 14.0));
        float spec_val = saturate((lobe1 + lobe2) * (0.45 + 0.55 * tilt_arch));
        float ring_step = smoothstep(0.60, 0.66, spec_val);
        float ring_edge_chroma = smoothstep(0.54, 0.60, spec_val) * (1.0 - ring_step);

        float3 hl_tint = lerp(float3(1.0, 1.0, 1.0), lit_rgba.rgb * 1.45 + 0.22, 0.38);
        float3 edge_col = lerp(hl_tint, hl_tint * float3(1.12, 0.90, 1.08), ring_edge_chroma);
        angel_term = hair_l_col * edge_col *
                     ((ring_step * 0.34 + ring_edge_chroma * 0.14) * hair_hl_intensity * lerp(0.45, 1.0, cel_lit));
    }
    // Thin-slab RT Backscatter (or backlit silhouette fallback on iGPU)
    float3 backscatter_term = float3(0.0, 0.0, 0.0);
    float bs_str = max(fg.satsuei_params.w, 0.0);
    if (bs_str > 0.001 && (is_hair || is_skin_pixel || rt_class == 2u)) {
        if (rt_shadow_active) {
            backscatter_term = aov_backscatter * lit_rgba.rgb * bs_str;
        } else if (is_hair) {
            float rim_back = pow(saturate(1.0 - ndv), 3.0) * saturate(dot(-view_v, key_dir) * 0.5 + 0.5);
            backscatter_term = lit_rgba.rgb * key_col * float3(1.15, 0.75, 0.50) * (rim_back * bs_str * 0.35);
        }
    }

    float3 hdr = direct_term + rim_term + micro_pbr_term + matcap_term + catchlight_term + angel_term +
                 backscatter_term + emissive_term;
    return float4(hdr * alpha, alpha);
}

// Pass 13 helper: 128x128 3D interactive Light Orbit Orb shader (§12.7).
[shader("vertex")]
NprVsOut vs_orb(uint vid : SV_VertexID) {
    NprVsOut o = {};
#ifdef __TARGET_SPIRV__
    float2 quad[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    o.clip_pos = float4(quad[vid % 3u], 0.0, 1.0);
    o.uv = o.clip_pos.xy * 0.5 + 0.5;
#else
    float2 quad[3] = {float2(-1.0, 1.0), float2(3.0, 1.0), float2(-1.0, -3.0)};
    o.clip_pos = float4(quad[vid % 3u], 0.0, 1.0);
    o.uv = float2(o.clip_pos.x * 0.5 + 0.5, 0.5 - o.clip_pos.y * 0.5);
#endif
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
    float dot_dist = length(q - key_dir.xy * 0.82);
    col = lerp(col, float3(0.24, 0.55, 0.96), smoothstep(0.16, 0.10, dot_dist));
    return float4(linear_to_srgb(col) * edge_aa, edge_aa);
}
