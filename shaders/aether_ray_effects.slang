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
        if (write_half) {
            aether_store(g_Pc.param2, tid.xy >> 1u, float4(0.0, 0.0, 0.0, 1.0));
        }
        return;
    }

    uint gb_n_slot = fg.gbuffer_ids[0].x;
    uint gb_mat_slot = fg.gbuffer_ids[0].z;
    uint gb_misc_slot = fg.gbuffer_ids[0].w;

    float3 view_n = oct_decode(aether_load(gb_n_slot, ip).xy);
    float3 view_nr = oct_decode(aether_load(gb_n_slot, clamp(ip + int2(1, 0), int2(0, 0), max_p)).xy);
    float3 view_nd = oct_decode(aether_load(gb_n_slot, clamp(ip + int2(0, 1), int2(0, 0), max_p)).xy);
    float curvature = saturate(length(view_nr - view_n) + length(view_nd - view_n));

    float4 misc = aether_load(gb_misc_slot, ip);
    uint packed_mat = aether_uload(gb_mat_slot, ip).x;
    uint rt_class = (packed_mat >> 10u) & 3u;
    bool is_face_px = (rt_class == 0u) || (misc.y > 0.5);

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

    // Dual-Normal Ray Launching: offset RayOrigin along true N_geom
    float curvature_bias = 0.0020 + 0.0014 * abs(true_view_z) + 0.006 * curvature;
    float3 ray_origin = world_pos + world_geom_n * max(0.0015, curvature_bias);

    uint frame_idx = fg.state_u32.x;
    uint lighting_mode = (fg.state_u32.w >> 10u) & 3u;
    bool is_path_trace = (lighting_mode >= 2u);
    float j0 = ign_blue_noise(tid.xy, frame_idx);
    float j1 = ign_blue_noise(tid.xy ^ uint2(31u, 17u), frame_idx + 17u);

    // FaceSkin (0x01) excludes self-intersection from facial polygons; traces Hair (0x02) | HandsProps (0x04) | BodyCloth (0x08).
    uint shadow_mask = is_face_px ? 0xFEu : 0xFFu;

    // 1) Alpha-tested soft cone directional RT shadows
    float shadow_vis = 1.0;
    float min_t_norm = 1.0;
    float weight_sum = 0.0;
    float vis_accum = 0.0;

    uint light_count = min(fg.state_u32.y, 4u);
    float3 primary_key_dir = float3(0.35, 0.55, 0.75);
    if (light_count > 0u) {
        primary_key_dir = compute_world_light_dir(fg.lights[0], world_pos);
    }

    for (uint li = 0u; li < light_count; ++li) {
        GpuLight l = fg.lights[li];
        bool enabled = (l.role_and_flags.y & 1u) != 0u;
        bool cast_rt = (l.role_and_flags.y & 2u) != 0u;
        if (!enabled || !cast_rt || l.role_and_flags.x == 2u) {
            continue;
        }
        float3 ldir = compute_world_light_dir(l, world_pos);
        float ndl_geom = dot(world_geom_n, ldir);
        float lw = max(luminance(l.color_intensity.rgb), 0.1);

        // Smooth terminator factor on non-face surfaces: facing-away triangles enter shadow (0.0), NEVER force 1.0!
        float geom_face = is_face_px ? 1.0 : smoothstep(-0.20, 0.06, ndl_geom);
        if (geom_face <= 0.001) {
            weight_sum += lw;
            continue;
        }

        // Respect true fixture angular radius (0.53 deg sun, 0.8 deg Dedolight, soft SkyPanel/Window)
        float cone_r = max(l.direction_radius.w, 0.0045);
        float tmin_bias = is_face_px ? 0.004 : (0.006 + 0.012 * (1.0 - saturate(ndl_geom)) + 0.010 * curvature);
        float3 light_origin = ray_origin + world_geom_n * (0.0015 + 0.003 * curvature);

        uint s_count = (cone_r >= 0.05) ? 4u : 2u;
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
                min_t_norm = min(min_t_norm, clamp(q.CommittedRayT() / 0.45, 0.0, 1.0));
            } else {
                s_accum += 1.0;
            }
        }

        float vis = (s_accum / float(s_count)) * geom_face;
        vis_accum += lw * vis;
        weight_sum += lw;
    }
    if (weight_sum > 1e-5) {
        shadow_vis = vis_accum / weight_sum;
    }

    aether_store(g_Pc.param0, tid.xy, float4(shadow_vis, min_t_norm, 0.0, 1.0));

    // 2) Occluded Fresnel rim trace (only traced where Fresnel > 0.2)
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

    // 3) True-Albedo 1-Bounce Indirect Color Bleeding + Path Tracing GI (half-res -> denoised by SVGF)
    if (write_half) {
        float3 key_col = (light_count > 0u) ? fg.lights[0].color_intensity.rgb : float3(1.0, 0.96, 0.92);
        float3 fill_col = (light_count > 1u && (fg.lights[1].role_and_flags.y & 1u) != 0u)
                              ? fg.lights[1].color_intensity.rgb
                              : float3(0.45, 0.56, 0.78);
        float3 bounce_rgb = float3(0.0, 0.0, 0.0);

        uint sample_n = is_path_trace ? 2u : 1u;
        for (uint si = 0u; si < sample_n; ++si) {
            float u1 = frac(j0 + 0.381966 * float(si));
            float u2 = frac(j1 + 0.618034 * float(si));
            float3 bdir = fold_ray_above_geom_horizon(sample_cosine_hemisphere(world_n, u1, u2), world_geom_n, 0.05);
            RayDesc rb = {};
            rb.Origin = ray_origin;
            rb.Direction = bdir;
            rb.TMin = 0.005 + 0.008 * curvature;
            rb.TMax = max(fg.light_params.z, is_path_trace ? 0.45 : 0.28);
            RayQuery<RAY_FLAG_NONE> qb;
            qb.TraceRayInline(g_Tlas, RAY_FLAG_NONE, 0xFFu, rb);
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
                float3 hit_world = rb.Origin + rb.Direction * hit_t;
                float3 hit_chroma = eval_bounce_hit_radiance(
                    qb.CommittedInstanceID() + qb.CommittedPrimitiveIndex(),
                    qb.CommittedTriangleBarycentrics(),
                    hit_world, bdir, primary_key_dir, key_col, fill_col, fg, max_p);
                bounce_rgb += min(hit_chroma, float3(1.5, 1.5, 1.5)) * (atten * fg.light_params.w);
            } else if (is_path_trace) {
                float sky_t = saturate(bdir.y * 0.5 + 0.5);
                bounce_rgb += lerp(key_col * 0.04, fill_col * 0.16, sky_t) * fg.light_params.w;
            }
        }
        bounce_rgb /= float(sample_n);
        aether_store(g_Pc.param2, tid.xy >> 1u, float4(bounce_rgb, 1.0));
    }
}
