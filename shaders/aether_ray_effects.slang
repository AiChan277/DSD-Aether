#include "aether_common.slang"

[[vk::binding(0, 1)]] RaytracingAccelerationStructure g_Tlas;

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

// Pass 4: Inline RayQuery soft directional cone shadows, occluded Fresnel rim, and half-res diffuse bounce (§10.6).
// PushConstants:
//   param0: rt_shadow_raw storage slot
//   param1: rim_occ_raw storage slot
//   param2: bounce_half storage slot
//   param3: (high 16 = half_w | low 16 = flags)
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    uint w = max(uint(fg.viewport_size.x), 1u);
    uint h = max(uint(fg.viewport_size.y), 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    int3 p = int3(int2(tid.xy), 0);
    uint depth_slot = fg.gbuffer_extra_ids[0].x;
    float hw_depth = g_SampledImages[depth_slot].Load(p).x;
    bool write_half = ((tid.x & 1u) == 0u) && ((tid.y & 1u) == 0u);

    if (hw_depth >= 0.99998) {
        g_StorageImages[g_Pc.param0][tid.xy] = float4(1.0, 1.0, 0.0, 1.0);
        g_StorageImages[g_Pc.param1][tid.xy] = float4(1.0, 1.0, 0.0, 1.0);
        if (write_half) {
            g_StorageImages[g_Pc.param2][tid.xy >> 1u] = float4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }

    uint gb_n_slot = fg.gbuffer_ids[0].x;
    uint gb_misc_slot = fg.gbuffer_ids[0].w;
    float3 view_n = oct_decode(g_SampledImages[gb_n_slot].Load(p).xy);
    float4 misc = g_SampledImages[gb_misc_slot].Load(p);

    // Reconstruct un-flattened world position and geometric ray origin (§10.4)
    float true_view_z = -fg.proj.col3.z / max(abs(hw_depth + fg.proj.col2.z), 1e-5);
    if (hw_depth + fg.proj.col2.z < 0.0) {
        true_view_z = -fg.proj.col3.z / (hw_depth + fg.proj.col2.z);
    } else {
        true_view_z = -fg.camera_pos_world.w;
    }
    float2 uv = (float2(tid.xy) + 0.5) * fg.viewport_size.zw;
    float3 view_pos = reconstruct_view_pos_from_zflat(
        fg.proj, uv, true_view_z, fg.head_pivot_view_and_flatness.z, misc.x);
    float3 world_pos = mul_col(fg.inv_view, float4(view_pos, 1.0)).xyz;
    float3 world_n = normalize_safe(mul_col3(fg.inv_view, view_n), float3(0.0, 0.0, 1.0));
    float3 ray_origin = world_pos + world_n * (0.002 + 0.0005 * abs(true_view_z));

    uint frame_idx = fg.state_u32.x;
    float j0 = ign_blue_noise(tid.xy, frame_idx);
    float j1 = ign_blue_noise(tid.xy ^ uint2(31u, 17u), frame_idx + 17u);

    // Exclude HairCast instances (0x04) from face-SDF pixels so anime face shadows stay clean (§10.6)
    uint trace_mask = (misc.y > 0.5) ? 0xFBu : 0xFFu;

    // 1) Soft cone directional RT shadows (Primary Key light + additional RT casters)
    float shadow_vis = 1.0;
    float min_t_norm = 1.0;
    float weight_sum = 0.0;
    float vis_accum = 0.0;

    uint light_count = min(fg.state_u32.y, 4u);
    for (uint li = 0u; li < light_count; ++li) {
        GpuLight l = fg.lights[li];
        bool enabled = (l.role_and_flags.y & 1u) != 0u;
        bool cast_rt = (l.role_and_flags.y & 2u) != 0u;
        if (!enabled || !cast_rt || l.role_and_flags.x == 2u) {
            continue;
        }
        float3 ldir = compute_world_light_dir(l, world_pos);
        float3 rdir = sample_cone(ldir, max(l.direction_radius.w, 0.02), j0, j1);
        float lw = max(luminance(l.color_intensity.rgb), 0.1);

        RayDesc r = {};
        r.Origin = ray_origin;
        r.Direction = rdir;
        r.TMin = 0.003;
        r.TMax = 2.8;

        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_Tlas, RAY_FLAG_NONE, trace_mask, r);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            min_t_norm = min(min_t_norm, clamp(q.CommittedRayT() / 0.45, 0.0, 1.0));
        } else {
            vis_accum += lw;
        }
        weight_sum += lw;
    }
    if (weight_sum > 1e-5) {
        shadow_vis = vis_accum / weight_sum;
    }
    g_StorageImages[g_Pc.param0][tid.xy] = float4(shadow_vis, min_t_norm, 0.0, 1.0);

    // 2) Occluded Fresnel rim trace (only traced where Fresnel > 0.2 per §10.6)
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
        rr.TMin = 0.004;
        rr.TMax = max(fg.light_params.y, 0.45);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> qr;
        qr.TraceRayInline(g_Tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, rr);
        qr.Proceed();
        if (qr.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            rim_vis = 0.0;
            rim_t = clamp(qr.CommittedRayT() / rr.TMax, 0.0, 1.0);
        }
    }
    g_StorageImages[g_Pc.param1][tid.xy] = float4(rim_vis, rim_t, 0.0, 1.0);

    // 3) Half-resolution 1-spp cosine diffuse bounce (`TMax = 0.25 m`, strength `0.35`)
    if (write_half) {
        float3 bounce_rgb = float3(0.0, 0.0, 0.0);
        float3 bdir = sample_cosine_hemisphere(world_n, j0, j1);
        RayDesc rb = {};
        rb.Origin = ray_origin;
        rb.Direction = bdir;
        rb.TMin = 0.004;
        rb.TMax = max(fg.light_params.z, 0.25);
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> qb;
        qb.TraceRayInline(g_Tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFFu, rb);
        qb.Proceed();
        if (qb.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float atten = 1.0 - saturate(qb.CommittedRayT() / rb.TMax);
            float3 key_col = (light_count > 0u) ? fg.lights[0].color_intensity.rgb : float3(1.0, 0.96, 0.92);
            bounce_rgb = key_col * (atten * fg.light_params.w);
        }
        g_StorageImages[g_Pc.param2][tid.xy >> 1u] = float4(bounce_rgb, 1.0);
    }
}
