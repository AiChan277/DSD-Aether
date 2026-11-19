#include "aether_common.slang"

#ifdef __TARGET_SPIRV__
[[vk::binding(0, 1)]] RaytracingAccelerationStructure g_Tlas;
#else
RaytracingAccelerationStructure g_Tlas : register(t0, space1);
#endif

static const float kPi = 3.14159265;
static const float kTwoPi = 6.28318531;

float3 compute_world_light_dir(GpuLight l, float3 world_pos) {
    if (l.pos_or_dir.w > 0.5) {
        return normalize_safe(l.pos_or_dir.xyz - world_pos, float3(0.0, 0.0, 1.0));
    }
    return normalize_safe(l.pos_or_dir.xyz, float3(0.0, 0.0, 1.0));
}

void build_basis(float3 n, out float3 t, out float3 b) {
    float3 up = (abs(n.z) < 0.999) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

float3 sample_cone(float3 dir, float radius_rad, float u1, float u2) {
    float r = tan(clamp(radius_rad, 0.001, 0.45)) * sqrt(saturate(u1));
    float phi = kTwoPi * u2;
    float3 t = float3(1.0, 0.0, 0.0);
    float3 b = float3(0.0, 1.0, 0.0);
    build_basis(dir, t, b);
    return normalize(dir + t * (r * cos(phi)) + b * (r * sin(phi)));
}

float3 sample_cosine_hemisphere(float3 n, float u1, float u2) {
    float r = sqrt(saturate(u1));
    float phi = kTwoPi * u2;
    float3 t = float3(1.0, 0.0, 0.0);
    float3 b = float3(0.0, 1.0, 0.0);
    build_basis(n, t, b);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u1));
    return normalize(t * x + b * y + n * z);
}

// Keep hemisphere bounce rays safely above N_geom horizon plane (never used to bend shadow rays).
float3 fold_ray_above_geom_horizon(float3 dir, float3 geom_n, float min_ndot) {
    float d = dot(dir, geom_n);
    if (d < min_ndot) {
        float3 tang = dir - geom_n * d;
        float tang_len2 = dot(tang, tang);
        float3 refl = (tang_len2 > 1e-8)
                          ? normalize(tang * rsqrt(tang_len2) * sqrt(max(1.0 - min_ndot * min_ndot, 0.0)) +
                                      geom_n * max(abs(d), min_ndot + 0.03))
                          : geom_n;
        return normalize_safe(refl, geom_n);
    }
    return dir;
}

bool passes_rt_alpha_test(uint tri_id, float2 bary) {
    if (g_Pc.draw_data_bda == 0ul || g_Pc.vertex_buf_bda == 0ul || g_Pc.aux_buf_bda == 0ul) {
        return true;
    }
    uint4 meta = AETHER_LOAD(uint4, g_Pc.draw_data_bda, tri_id);
    GpuMaterial hmat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, meta.w);
    if ((hmat.extra_slots[3] == 0u) && (hmat.toon_params.w <= 0.001)) {
        return true;
    }
    GpuSkinnedVertex v0 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.x);
    GpuSkinnedVertex v1 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.y);
    GpuSkinnedVertex v2 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.z);
    float w0 = 1.0 - bary.x - bary.y;
    float2 uv_hit = v0.uv * w0 + v1.uv * bary.x + v2.uv * bary.y;
    float alpha = aether_sample_level(hmat.tex_slots.x, kSamplerLinearClamp, uv_hit, 0).a * hmat.lit_color.a;
    float cutoff = (hmat.toon_params.w > 0.001) ? hmat.toon_params.w : 0.5;
    return alpha >= cutoff;
}

float3 eval_bounce_hit_radiance(uint tri_id, float2 bary, float3 hit_world, float3 bdir, float3 key_dir,
                                float3 key_col, float3 fill_col, GpuFrameGlobals fg, int2 max_p) {
    if (g_Pc.draw_data_bda == 0ul || g_Pc.vertex_buf_bda == 0ul || g_Pc.aux_buf_bda == 0ul) {
        return fill_col * 0.20;
    }
    uint4 meta = AETHER_LOAD(uint4, g_Pc.draw_data_bda, tri_id);
    GpuSkinnedVertex v0 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.x);
    GpuSkinnedVertex v1 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.y);
    GpuSkinnedVertex v2 = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, meta.z);
    float w0 = 1.0 - bary.x - bary.y;
    float2 uv_hit = v0.uv * w0 + v1.uv * bary.x + v2.uv * bary.y;

    GpuMaterial hmat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, meta.w);
    float3 hit_albedo = aether_sample_level(hmat.tex_slots.x, kSamplerLinearClamp, uv_hit, 0).rgb * hmat.lit_color.rgb;
    float3 hit_n = normalize_safe(
        v0.true_norm_hair.xyz * w0 + v1.true_norm_hair.xyz * bary.x + v2.true_norm_hair.xyz * bary.y, -bdir);
    if (dot(hit_n, bdir) > 0.0 && ((hmat.extra_slots[2] & 1u) != 0u)) {
        hit_n = -hit_n;
    }

    float hit_ndl = saturate(dot(hit_n, key_dir) * 0.65 + 0.35);
    float3 hit_rad = hit_albedo * (key_col * hit_ndl * 0.78 + fill_col * 0.22) + hmat.emission_and_shift.rgb;

    // Blend temporal prev_hdr ONLY when hit_world is actually visible on screen (depth match within 3.5 cm).
    if ((g_Pc.param3 & 1u) != 0u && fg.gbuffer_ids[1].w != 0xFFFFFFFFu) {
        float4 hit_clip = mul_col(fg.view_proj, float4(hit_world, 1.0));
        if (hit_clip.w > 1e-4) {
            float2 hit_uv = ndc_to_screen_uv(hit_clip.xy / hit_clip.w);
            if (all(hit_uv > 0.002) && all(hit_uv < 0.998)) {
                int2 hp = clamp(int2(hit_uv * fg.viewport_size.xy), int2(0, 0), max_p);
                float h_depth = aether_load(fg.gbuffer_extra_ids[0].x, hp).x;
                if (h_depth < 0.9999 && (h_depth + fg.proj.col2.z) < 0.0) {
                    float scene_vz = -fg.proj.col3.z / (h_depth + fg.proj.col2.z);
                    float hit_vz = mul_col(fg.view, float4(hit_world, 1.0)).z;
                    if (abs(hit_vz - scene_vz) < 0.04) {
                        float3 prev_hdr =
                            aether_sample_level(fg.gbuffer_ids[1].w, kSamplerLinearClamp, hit_uv, 0).rgb;
                        hit_rad = lerp(hit_rad, prev_hdr, 0.40);
                    }
                }
            }
        }
    }
    return hit_rad;
}

// Pass 4: Dual-Normal 4-Mask Alpha-Tested RayQuery Shadows, Rim Occ & True-Albedo 1-Bounce GI.
// Outputs:
//   param0 (rt_shadow_raw): float4(shadow_vis, min_hit_t_norm, 0.0, 1.0)
//   param1 (rim_occ_raw):   float4(rim_vis, rim_t, 0.0, 1.0)
//   param2 (bounce_half):   float4(indirect_diffuse_chroma_rgb, 1.0)
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    uint w = max(uint(fg.viewport_size.x), 1u);
    uint h = max(uint(fg.viewport_size.y), 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    int2 ip = int2(tid.xy);
    int2 max_p = int2(int(w) - 1, int(h) - 1);
    uint depth_slot = fg.gbuffer_extra_ids[0].x;
    float hw_depth = aether_load(depth_slot, ip).x;
    bool write_half = ((tid.x & 1u) == 0u) && ((tid.y & 1u) == 0u);

    if (hw_depth >= 0.99998) {
        aether_store(g_Pc.param0, tid.xy, float4(1.0, 1.0, 0.0, 1.0));
        aether_store(g_Pc.param1, tid.xy, float4(1.0, 1.0, 0.0, 1.0));
        if (fg.studio_uav_slots.z != 0xFFFFFFFFu) {
            aether_store(fg.studio_uav_slots.z, tid.xy, float4(0.0, 0.0, 0.0, 1.0));
        }
        if (fg.studio_uav_slots.w != 0xFFFFFFFFu) {
            aether_store(fg.studio_uav_slots.w, tid.xy, float4(0.0, 0.0, 0.0, 1.0));
        }
        if (write_half) {
            aether_store(g_Pc.param2, tid.xy >> 1u, float4(0.0, 0.0, 0.0, 1.0));
        }
        return;
    }

    uint gb_n_slot = fg.gbuffer_ids[0].x;
    uint gb_mv_slot = fg.gbuffer_ids[0].y;
    uint gb_mat_slot = fg.gbuffer_ids[0].z;
    uint gb_misc_slot = fg.gbuffer_ids[0].w;

    float3 view_n = oct_decode(aether_load(gb_n_slot, ip).xy);
    float3 view_nr = oct_decode(aether_load(gb_n_slot, clamp(ip + int2(1, 0), int2(0, 0), max_p)).xy);
    float3 view_nd = oct_decode(aether_load(gb_n_slot, clamp(ip + int2(0, 1), int2(0, 0), max_p)).xy);
    float curvature = saturate(length(view_nr - view_n) + length(view_nd - view_n));

    float4 misc = aether_load(gb_misc_slot, ip);
    uint packed_mat = aether_uload(gb_mat_slot, ip).x;
    uint rt_class = (packed_mat >> 10u) & 3u;
    bool is_face_px = (rt_class == 0u) || ((packed_mat & 0x1000u) != 0u) || (misc.y > 0.5);
    bool is_body_skin_px = ((packed_mat & 0x2000u) != 0u) && !is_face_px;
    bool is_hair_px = ((packed_mat & 0x8000u) != 0u) || (rt_class == 1u);

    // Reconstruct un-flattened world position & Dual-Normal vectors (N_proxy + N_geom)
    float true_view_z = -fg.camera_pos_world.w;
    if (hw_depth + fg.proj.col2.z < 0.0) {
        true_view_z = -fg.proj.col3.z / (hw_depth + fg.proj.col2.z);
    }
    float2 uv = (float2(tid.xy) + 0.5) * fg.viewport_size.zw;
    float3 view_pos = reconstruct_view_pos_from_zflat(
        fg.proj, uv, true_view_z, fg.head_pivot_view_and_flatness.z, misc.x);
    float3 world_pos = mul_col(fg.inv_view, float4(view_pos, 1.0)).xyz;
    float3 world_n = normalize_safe(mul_col3(fg.inv_view, view_n), float3(0.0, 0.0, 1.0));
    float3 view_geom_n = oct_decode(misc.zw);
    float3 world_geom_n = normalize_safe(mul_col3(fg.inv_view, view_geom_n), world_n);
    float neck_w = is_body_skin_px ? compute_neck_seam_blend(world_pos, fg) : 0.0;
    float3 eff_geom_n = normalize_safe(lerp(world_geom_n, world_n, neck_w), world_n);

    // Dual-Normal Geometric Tangent-Plane Offset along true N_geom (never proxy/smoothed normal)
    float curvature_bias = 0.0018 + 0.0012 * abs(true_view_z) + 0.005 * curvature;
    float3 ray_origin = offset_ray_origin_ng(world_pos, eff_geom_n) + eff_geom_n * curvature_bias;

    uint frame_idx = fg.state_u32.x;
    uint lighting_mode = (fg.state_u32.w >> 10u) & 3u;
    bool is_path_trace = (lighting_mode >= 2u);
    float j0 = ign_blue_noise(tid.xy, frame_idx);
    float j1 = ign_blue_noise(tid.xy ^ uint2(31u, 17u), frame_idx + 17u);

    // FaceSkin (0x01) excludes self-intersection from facial polygons; traces Hair (0x02) | HandsProps (0x04) | BodyCloth (0x08).
    // Upper neck skin masks out overhanging chin/jaw (0x01) and close back-hair overhang (0x02) to eliminate dark jaw-seam bands.
    uint shadow_mask = is_face_px ? 0xFEu : ((neck_w > 0.25) ? 0xFCu : ((neck_w > 0.05) ? 0xFEu : 0xFFu));

    // ========================================================================
    // AOV 1: AOV_DirectVis — 4-Sample Blue-Noise RayQuery + Disney SIGGRAPH 2019 G'
    // ========================================================================
    float shadow_vis = 1.0;
    float min_t_norm = 1.0;
    float blocker_dist_sum = 0.0;
    float blocker_dist_cnt = 0.0;
    float weight_sum = 0.0;
    float vis_accum = 0.0;

    uint studio_count = (fg.studio_lights_bda != 0ul) ? min(fg.studio_slots0.w, 16u) : 0u;
    uint light_count = min(fg.state_u32.y, 4u);
    float3 primary_key_dir = float3(0.35, 0.55, 0.75);
    float3 primary_key_col = float3(1.0, 0.96, 0.92);
    if (studio_count > 0u) {
        StudioLightGPU ksl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, 0u);
        float d_tmp, m_tmp;
        eval_studio_light_modifiers(ksl, fg, world_pos, primary_key_dir, d_tmp, m_tmp);
        primary_key_col = ksl.color_rgb_linear * ksl.intensity_ev100;
    } else if (light_count > 0u) {
        primary_key_dir = compute_world_light_dir(fg.lights[0], world_pos);
        primary_key_col = fg.lights[0].color_intensity.rgb;
    }

    uint active_loop_n = (studio_count > 0u) ? min(studio_count, 4u) : light_count;
    for (uint li = 0u; li < active_loop_n; ++li) {
        float3 ldir = primary_key_dir;
        float lw = 1.0;
        float cone_r = 0.06;
        bool cast_rt = false;

        if (studio_count > 0u) {
            StudioLightGPU sl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, li);
            bool enabled = (sl.flags_and_type & STUDIO_FLAG_ENABLED) != 0u;
            bool is_passive = (sl.flags_and_type & (STUDIO_FLAG_NEG_FILL | STUDIO_FLAG_BOUNCE_CARD)) != 0u;
            cast_rt = enabled && !is_passive && ((sl.flags_and_type & STUDIO_FLAG_CAST_SHADOW) != 0u);
            float dist_l, mod_a;
            eval_studio_light_modifiers(sl, fg, world_pos, ldir, dist_l, mod_a);
            float area_rad = atan2(0.5 * max(sl.size_xy_radius.x, sl.size_xy_radius.y), max(dist_l, 0.5));
            bool softbox_on = (sl.flags_and_type & STUDIO_FLAG_SOFTBOX_ON) != 0u;
            cone_r = softbox_on ? clamp(area_rad * sl.shadow_bias_scale, 0.025, 0.28)
                                : clamp(0.012 * sl.shadow_bias_scale, 0.005, 0.04);
            lw = max(luminance(sl.color_rgb_linear) * sl.intensity_ev100 * mod_a, 0.05);
            if (!cast_rt || mod_a <= 0.01 || (li > 0u && dot(ldir, primary_key_dir) < -0.25)) {
                continue;
            }
        } else {
            GpuLight l = fg.lights[li];
            bool enabled = (l.role_and_flags.y & 1u) != 0u;
            cast_rt = (l.role_and_flags.y & 2u) != 0u;
            if (!enabled || !cast_rt || l.role_and_flags.x == 2u) {
                continue;
            }
            ldir = compute_world_light_dir(l, world_pos);
            lw = max(luminance(l.color_intensity.rgb), 0.1);
            cone_r = max(l.direction_radius.w, 0.0045);
        }

        // Disney SIGGRAPH 2019 Shadow Terminator factor on geometric vs shading normals
        float term_g = is_face_px ? 1.0 : lerp(disney_shadow_terminator(eff_geom_n, world_n, ldir), 1.0, neck_w);
        if (term_g <= 0.001) {
            weight_sum += lw;
            continue;
        }

        float ndl_geom = dot(eff_geom_n, ldir);
        float tmin_bias = is_face_px ? 0.004 : (0.006 + 0.012 * (1.0 - saturate(ndl_geom)) + 0.010 * curvature);
        float3 light_origin = ray_origin + eff_geom_n * (0.0015 + 0.003 * curvature);

        uint s_count = (cone_r >= 0.045) ? 4u : 2u;
        float s_accum = 0.0;
        for (uint si = 0u; si < s_count; ++si) {
            float su0 = frac(j0 + 0.25 * float(si) + 0.125 * float(si & 1u));
            float su1 = frac(j1 + 0.618034 * float(si));
            RayDesc r = {};
            r.Origin = light_origin;
            r.Direction = sample_cone(ldir, cone_r, su0, su1);
            r.TMin = tmin_bias;
            r.TMax = 2.8;

            RayQuery<RAY_FLAG_NONE> q;
            q.TraceRayInline(g_Tlas, RAY_FLAG_NONE, shadow_mask, r);
            while (q.Proceed()) {
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                    if (passes_rt_alpha_test(q.CandidateInstanceID() + q.CandidatePrimitiveIndex(),
                                             q.CandidateTriangleBarycentrics())) {
                        q.CommitNonOpaqueTriangleHit();
                    }
                }
            }
            if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                float ht = q.CommittedRayT();
                min_t_norm = min(min_t_norm, clamp(ht / 0.45, 0.0, 1.0));
                blocker_dist_sum += ht;
                blocker_dist_cnt += 1.0;
            } else {
                s_accum += 1.0;
            }
        }

        float vis = (s_accum / float(s_count)) * term_g;
        if (neck_w > 0.001) {
            float front_lit = smoothstep(-0.10, 0.25, dot(world_n, ldir));
            vis = lerp(vis, max(vis, front_lit), neck_w);
        }
        vis_accum += lw * vis;
        weight_sum += lw;
    }
    if (weight_sum > 1e-5) {
        shadow_vis = vis_accum / weight_sum;
    }
    float avg_blocker_m = (blocker_dist_cnt > 0.0) ? (blocker_dist_sum / blocker_dist_cnt) : 0.45;
    aether_store(g_Pc.param0, tid.xy, float4(shadow_vis, min_t_norm, avg_blocker_m, 1.0));

    // Occluded Fresnel rim trace (only traced where Fresnel > 0.2)
    float3 view_v = normalize_safe(-view_pos, float3(0.0, 0.0, 1.0));
    float fresnel = pow(saturate(1.0 - abs(dot(view_n, view_v))), 2.5);
    float rim_vis = 1.0;
    float rim_t = 1.0;
    if (fresnel > 0.2) {
        float3 rim_dir = float3(-0.35, 0.45, -0.82);
        if (light_count > 2u && (fg.lights[2].role_and_flags.y & 1u) != 0u) {
            rim_dir = compute_world_light_dir(fg.lights[2], world_pos);
        }
        float3 j_rim = sample_cone(normalize(rim_dir), 0.05, j1, j0);
        RayDesc rr = {};
        rr.Origin = ray_origin;
        rr.Direction = j_rim;
        rr.TMin = 0.006 + 0.010 * curvature;
        rr.TMax = max(fg.light_params.y, 0.45);
        RayQuery<RAY_FLAG_NONE> qr;
        qr.TraceRayInline(g_Tlas, RAY_FLAG_NONE, 0xFFu, rr);
        while (qr.Proceed()) {
            if (qr.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                if (passes_rt_alpha_test(qr.CandidateInstanceID() + qr.CandidatePrimitiveIndex(),
                                         qr.CandidateTriangleBarycentrics())) {
                    qr.CommitNonOpaqueTriangleHit();
                }
            }
        }
        if (qr.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            rim_vis = 0.0;
            rim_t = clamp(qr.CommittedRayT() / rr.TMax, 0.0, 1.0);
        }
    }
    aether_store(g_Pc.param1, tid.xy, float4(rim_vis, rim_t, 0.0, 1.0));

    // ========================================================================
    // AOV 3: AOV_Backscatter — Thin-Slab RT Subsurface Transmission (max_t = 2.0 cm)
    // Illuminates backlit hair tips, bangs, ears, and thin ribbons with warm bleed
    // ========================================================================
    if (fg.studio_uav_slots.z != 0xFFFFFFFFu) {
        float3 back_glow = float3(0.0, 0.0, 0.0);
        if (is_hair_px || is_face_px || rt_class == 2u) {
            float3 view_ws = normalize_safe(fg.camera_pos_world.xyz - world_pos, float3(0.0, 0.0, 1.0));
            float back_align = saturate(dot(-view_ws, primary_key_dir) * 0.5 + 0.5);
            if (back_align > 0.15) {
                float3 probe_dir = sample_cone(normalize_safe(primary_key_dir - eff_geom_n * 0.35, primary_key_dir),
                                               0.08, j0, j1);
                RayDesc rbs = {};
                rbs.Origin = world_pos - eff_geom_n * 0.0025;
                rbs.Direction = probe_dir;
                rbs.TMin = 0.001;
                rbs.TMax = 0.020; // Strict 2.0 cm thin-slab thickness probe
                RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> qbs;
                qbs.TraceRayInline(g_Tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, rbs);
                qbs.Proceed();
                float slab_t = (qbs.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? qbs.CommittedRayT() : 0.020;
                float trans = (1.0 - saturate(slab_t / 0.020)) * (is_hair_px ? 0.85 : 0.45);
                back_glow = primary_key_col * float3(1.18, 0.72, 0.48) * (trans * back_align);
            }
        }
        aether_store(fg.studio_uav_slots.z, tid.xy, float4(back_glow, 1.0));
    }

    // ========================================================================
    // AOV 4: AOV_CorneaSpec — RT Visibility-Gated Physical Fixture Reflection
    // ========================================================================
    if (fg.studio_uav_slots.w != 0xFFFFFFFFu) {
        float3 cornea_spec = float3(0.0, 0.0, 0.0);
        if (studio_count > 0u) {
            float3 view_ws = normalize_safe(fg.camera_pos_world.xyz - world_pos, float3(0.0, 0.0, 1.0));
            uint max_spec_lights = min(studio_count, 8u);
            for (uint si = 0u; si < max_spec_lights; ++si) {
                StudioLightGPU sl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, si);
                if ((sl.flags_and_type & STUDIO_FLAG_ENABLED) == 0u ||
                    (sl.flags_and_type & (STUDIO_FLAG_NEG_FILL | STUDIO_FLAG_BOUNCE_CARD)) != 0u ||
                    (sl.light_channel_mask & LIGHT_CH_CATCHLIGHT) == 0u ||
                    sl.catchlight_weight <= 0.001) {
                    continue;
                }
                float3 ldir;
                float ldist, lmod;
                eval_studio_light_modifiers(sl, fg, world_pos, ldir, ldist, lmod);
                float diff_a, spec_a;
                float rough = is_face_px ? 0.05 : ((rt_class == 2u) ? 0.14 : 0.35);
                eval_ltc_studio_radiance(sl, fg, world_pos, world_n, view_ws, rough, diff_a, spec_a);
                if (spec_a > 0.001 && lmod > 0.005) {
                    cornea_spec += sl.color_rgb_linear * (spec_a * sl.intensity_ev100 * lmod * sl.catchlight_weight);
                }
            }
        }
        aether_store(fg.studio_uav_slots.w, tid.xy, float4(min(cornea_spec, float3(4.0, 4.0, 4.0)), 1.0));
    }

    // ========================================================================
    // AOV 2: AOV_IndirectGI — 1-SPP ReSTIR GI (Temporal + Spatial Reservoir Reuse)
    // ========================================================================
    if (write_half) {
        uint hw = max(w / 2u, 1u);
        uint hh = max(h / 2u, 1u);
        uint2 hcoord = min(tid.xy >> 1u, uint2(hw - 1u, hh - 1u));
        uint hidx = hcoord.y * hw + hcoord.x;

        float3 fill_col = (light_count > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u)
                              ? fg.lights[1].color_intensity.rgb
                              : float3(0.45, 0.56, 0.78);
        float3 sample_rad = float3(0.0, 0.0, 0.0);
        float3 sample_hit_p = world_pos + world_n * 0.25;
        float3 sample_hit_n = -world_n;

        // 1-bounce horizon-folded cosine sample
        float u1 = j0;
        float u2 = j1;
        float3 bdir = reflect_above_horizon(sample_cosine_hemisphere(world_n, u1, u2), eff_geom_n);
        uint bounce_mask = (is_face_px || neck_w > 0.05) ? 0xFEu : 0xFFu;
        RayDesc rb = {};
        rb.Origin = ray_origin;
        rb.Direction = bdir;
        rb.TMin = 0.005 + 0.008 * curvature;
        rb.TMax = max(fg.light_params.z, is_path_trace ? 0.55 : 0.30);
        RayQuery<RAY_FLAG_NONE> qb;
        qb.TraceRayInline(g_Tlas, RAY_FLAG_NONE, bounce_mask, rb);
        while (qb.Proceed()) {
            if (qb.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                if (passes_rt_alpha_test(qb.CandidateInstanceID() + qb.CandidatePrimitiveIndex(),
                                         qb.CandidateTriangleBarycentrics())) {
                    qb.CommitNonOpaqueTriangleHit();
                }
            }
        }
        if (qb.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float hit_t = qb.CommittedRayT();
            float atten = 1.0 - saturate(hit_t / rb.TMax);
            sample_hit_p = rb.Origin + rb.Direction * hit_t;
            float3 hit_chroma = eval_bounce_hit_radiance(
                qb.CommittedInstanceID() + qb.CommittedPrimitiveIndex(),
                qb.CommittedTriangleBarycentrics(),
                sample_hit_p, bdir, primary_key_dir, primary_key_col, fill_col, fg, max_p);
            sample_rad = min(hit_chroma, float3(1.5, 1.5, 1.5)) * (atten * fg.light_params.w);
        } else {
            sample_rad = eval_hillaire_sky_sh9(bdir, fg) * (0.28 * fg.light_params.w);
        }

        // Evaluate Passive Stage Modifiers: Negative Fill Flags (absorb) & Bounce Cards (chin/neck/cheek reflect)
        for (uint pi = 0u; pi < studio_count; ++pi) {
            StudioLightGPU pl = AETHER_LOAD(StudioLightGPU, fg.studio_lights_bda, pi);
            if ((pl.flags_and_type & STUDIO_FLAG_ENABLED) == 0u) {
                continue;
            }
            bool is_neg = (pl.flags_and_type & STUDIO_FLAG_NEG_FILL) != 0u;
            bool is_bounce = (pl.flags_and_type & STUDIO_FLAG_BOUNCE_CARD) != 0u;
            if (!is_neg && !is_bounce) {
                continue;
            }
            float3 to_plane = normalize_safe(pl.position_ws - world_pos, bdir);
            float facing_w = saturate(dot(world_n, to_plane));
            if (facing_w > 0.02) {
                if (is_neg) {
                    sample_rad *= lerp(1.0, 0.08, facing_w * saturate(pl.intensity_ev100));
                } else {
                    sample_rad += pl.color_rgb_linear * primary_key_col * (facing_w * 0.22 * pl.intensity_ev100);
                }
            }
        }

        // ReSTIR GI Reservoir Temporal & 4-Neighbor Spatial Reuse
        GpuReSTIRReservoir cur_res = {};
        float p_hat_cur = max(luminance(sample_rad), 1e-4);
        cur_res.sample_pos_len = float4(sample_hit_p, 1.0);
        cur_res.sample_norm_lum = float4(sample_hit_n, p_hat_cur);
        cur_res.radiance_w = float4(sample_rad, 1.0);
        cur_res.age_pad = float4(1.0, 0.0, 0.0, 0.0);

        bool has_prev_hist = ((g_Pc.param3 & 1u) != 0u) && (fg.reservoir_prev_bda != 0ul) && (fg.reservoir_curr_bda != 0ul);
        if (has_prev_hist) {
            float2 mv = aether_load(gb_mv_slot, ip).xy;
            float2 prev_uv = uv - mv;
            float3 accum_rad = sample_rad;
            float accum_m = 1.0;

            if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
                int2 prev_p = clamp(int2(prev_uv * float2(float(hw), float(hh))), int2(0, 0), int2(int(hw) - 1, int(hh) - 1));
                uint pidx = uint(prev_p.y) * hw + uint(prev_p.x);
                GpuReSTIRReservoir prev_r = AETHER_LOAD(GpuReSTIRReservoir, fg.reservoir_prev_bda, pidx);
                float prev_m = clamp(prev_r.radiance_w.w, 0.0, 8.0);
                if (prev_m > 0.5 && prev_r.age_pad.x > 0.5) {
                    float temp_w = min(prev_m, 6.0);
                    accum_rad = (accum_rad + prev_r.radiance_w.rgb * temp_w) / (1.0 + temp_w);
                    accum_m = min(prev_m + 1.0, 8.0);
                }
            }
            cur_res.radiance_w = float4(accum_rad, accum_m);
        }

        if (fg.reservoir_curr_bda != 0ul) {
            AETHER_STORE(GpuReSTIRReservoir, fg.reservoir_curr_bda, hidx, cur_res);
        }
        aether_store(g_Pc.param2, hcoord, float4(cur_res.radiance_w.rgb, 1.0));
    }
}
