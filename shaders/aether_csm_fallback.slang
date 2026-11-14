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
    int3 p = int3(int2(tid.xy), 0);
    uint depth_slot = fg.gbuffer_extra_ids[0].x;
    float hw_depth = g_SampledImages[depth_slot].Load(p).x;
    uint csm_slot = g_Pc.param3 & 0xFFFFu;

    if (hw_depth >= 0.99998 || csm_slot == 0xFFFFu) {
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

    float true_view_z = -fg.camera_pos_world.w;
    if (hw_depth + fg.proj.col2.z < 0.0) {
        true_view_z = -fg.proj.col3.z / (hw_depth + fg.proj.col2.z);
    }
    float2 uv = (float2(tid.xy) + 0.5) * fg.viewport_size.zw;
    float3 view_pos = reconstruct_view_pos_from_zflat(
        fg.proj, uv, true_view_z, fg.head_pivot_view_and_flatness.z, misc.x);
    float3 world_pos = mul_col(fg.inv_view, float4(view_pos, 1.0)).xyz;
    float3 world_n = normalize_safe(mul_col3(fg.inv_view, view_n), float3(0.0, 0.0, 1.0));

    float4 sc = mul_col(fg.shadow_view_proj[0], float4(world_pos + world_n * 0.005, 1.0));
    float2 suv = sc.xy / max(abs(sc.w), 1e-5) * 0.5 + 0.5;
    float sz = sc.z / max(abs(sc.w), 1e-5);

    float vis = 1.0;
    if (suv.x > 0.01 && suv.x < 0.99 && suv.y > 0.01 && suv.y < 0.99 && sz > 0.0 && sz < 1.0 && misc.y < 0.5) {
        float texel = 1.0 / 2048.0;
        float occ = 0.0;
        for (int dy = -1; dy <= 1; dy += 2) {
            for (int dx = -1; dx <= 1; dx += 2) {
                float d = g_SampledImages[csm_slot].SampleLevel(
                    g_Samplers[kSamplerNearestClamp], suv + float2(float(dx), float(dy)) * texel, 0).x;
                occ += (sz - 0.003 > d) ? 0.0 : 1.0;
            }
        }
        vis = occ * 0.25;
    }

    float rim_occ = saturate(0.5 + 0.5 * (-view_n.z));
    g_StorageImages[g_Pc.param0][tid.xy] = float4(vis, 1.0, 0.0, 1.0);
    g_StorageImages[g_Pc.param1][tid.xy] = float4(rim_occ, 1.0, 0.0, 1.0);
    if (write_half) {
        g_StorageImages[g_Pc.param2][tid.xy >> 1u] = float4(0.0, 0.0, 0.0, 1.0);
    }
}
