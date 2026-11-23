#include "aether_common.slang"

struct ShadowVsOut {
    float4 clip_pos : SV_Position;
};

// Pass 4 (Non-RT fallback): 2048^2 directional CSM depth + PCF compute shadow (`§10.6`).
[shader("vertex")]
ShadowVsOut vs_shadow(uint vid : SV_VertexID) {
    ShadowVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuSkinnedVertex v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, vid);
    o.clip_pos = mul_col(fg.shadow_view_proj[g_Pc.param0 & 3u], float4(v.pos_zflat.xyz, 1.0));
    return o;
}

[shader("fragment")]
void ps_shadow() {}

// PushConstants:
//   param0: rt_shadow_raw storage slot
//   param1: rim_occ_raw storage slot
//   param2: bounce_half storage slot
//   param3: csm_depth sampled slot (0xFFFF = disabled / unshadowed)
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_fallback(uint3 tid : SV_DispatchThreadID) {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    uint w = max(uint(fg.viewport_size.x), 1u);
    uint h = max(uint(fg.viewport_size.y), 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    bool write_half = ((tid.x & 1u) == 0u) && ((tid.y & 1u) == 0u);
    int2 p = int2(tid.xy);
    uint depth_slot = fg.gbuffer_extra_ids[0].x;
    float hw_depth = aether_load(depth_slot, p).x;
    uint csm_slot = g_Pc.param3 & 0xFFFFu;

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
    float3 view_n = oct_decode(aether_load(gb_n_slot, p).xy);
    uint packed_mat = aether_uload(gb_mat_slot, p).x;
    float4 misc = aether_load(gb_misc_slot, p);
    uint rt_class = (packed_mat >> 10u) & 3u;
    bool is_face_px = (rt_class == 0u) || ((packed_mat & 0x1000u) != 0u) || (misc.y > 0.5);
    bool is_body_skin_px = ((packed_mat & 0x2000u) != 0u) && !is_face_px;

    float true_view_z = depth_to_view_z(fg, hw_depth);
    float2 uv = (float2(tid.xy) + 0.5) * fg.viewport_size.zw;
    float3 view_pos = reconstruct_view_pos_from_zflat(
        fg, uv, true_view_z, fg.head_pivot_view_and_flatness.z, misc.x);
    float3 world_pos = mul_col(fg.inv_view, float4(view_pos, 1.0)).xyz;
    float3 world_n = normalize_safe(mul_col3(fg.inv_view, view_n), float3(0.0, 0.0, 1.0));
    float neck_w = is_body_skin_px ? compute_neck_seam_blend(world_pos, fg) : 0.0;

    float vis = 1.0;
    if (csm_slot != 0xFFFFu && !is_face_px && neck_w < 0.99) {
        float4 sc = mul_col(fg.shadow_view_proj[0], float4(world_pos + world_n * 0.006, 1.0));
        float2 sc_ndc = sc.xy / max(abs(sc.w), 1e-5);
#ifdef __TARGET_SPIRV__
        float2 suv = sc_ndc * 0.5 + 0.5;
#else
        float2 suv = ndc_to_screen_uv(sc_ndc);
#endif
        float sz = sc.z / max(abs(sc.w), 1e-5);
        if (suv.x > 0.01 && suv.x < 0.99 && suv.y > 0.01 && suv.y < 0.99 && sz > 0.0 && sz < 1.0) {
            float texel = 1.0 / 2048.0;
            float occ = 0.0;
            for (int dy = -1; dy <= 1; dy += 2) {
                for (int dx = -1; dx <= 1; dx += 2) {
                    float d = aether_sample_level(
                        csm_slot, kSamplerNearestClamp, suv + float2(float(dx), float(dy)) * texel, 0).x;
                    occ += (sz - 0.003 > d) ? 0.0 : 1.0;
                }
            }
            vis = lerp(occ * 0.25, 1.0, neck_w);
        }
    }

    float rim_occ = saturate(0.55 + 0.45 * (-view_n.z));
    aether_store(g_Pc.param0, tid.xy, float4(vis, 1.0, 0.0, 1.0));
    aether_store(g_Pc.param1, tid.xy, float4(rim_occ, 1.0, 0.0, 1.0));
    if (write_half) {
        float3 sh_amb = eval_hillaire_sky_sh9(world_n, fg) * fg.light_params.w;
        aether_store(g_Pc.param2, tid.xy >> 1u, float4(sh_amb, 1.0));
    }
}
