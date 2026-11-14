#include "aether_common.slang"

struct GpuSourceSurface {
    float2 uv0;
    float2 uv1;
    uint color_rgba8;
    uint tangent_oct;
    uint flags;
    uint reserved;
};

// Pass 1: Vertex stream pack & temporal previous-position capture (`§10.2`, `P4-02`).
// PushConstants layout:
//   frame_globals_bda : GpuFrameGlobals*
//   draw_data_bda     : AetherVertex25D* (current posed stream)
//   vertex_buf_bda    : GpuSkinnedVertex* (destination 96 B buffer)
//   aux_buf_bda       : AetherVertexSurface* (32 B surface stream, optional)
//   param0            : vertex_count
//   param1            : bit 0 = preserve previous frame positions into prev_pos_sdf.xyz
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint v_idx = tid.x;
    if (v_idx >= g_Pc.param0) {
        return;
    }

    GpuSourceVertex src = AETHER_LOAD(GpuSourceVertex, g_Pc.draw_data_bda, v_idx);
    float3 prev_pos = src.pos_out.xyz;
    if ((g_Pc.param1 & 1u) != 0u) {
        GpuSkinnedVertex prev_v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, v_idx);
        prev_pos = prev_v.pos_zflat.xyz;
    }

    float2 uv0 = float2(0.0, 0.0);
    uint surf_flags = 0u;
    if (g_Pc.aux_buf_bda != 0ul) {
        GpuSourceSurface surf = AETHER_LOAD(GpuSourceSurface, g_Pc.aux_buf_bda, v_idx);
        uv0 = surf.uv0;
        surf_flags = surf.flags;
    }

    GpuSkinnedVertex dst = {};
    dst.pos_zflat = float4(src.pos_out.xyz, clamp(src.norm_zflat.w, 0.0, 0.85));
    dst.prev_pos_sdf = float4(prev_pos, src.proxy_sdf.w);
    dst.true_norm_hair = float4(normalize_safe(src.norm_zflat.xyz, float3(0.0, 0.0, 1.0)), 0.0);
    dst.proxy_norm_out = float4(normalize_safe(src.proxy_sdf.xyz, float3(0.0, 0.0, 1.0)), src.pos_out.w);
    dst.tangent_crease = float4(normalize_safe(src.smooth_tan.xyz, float3(0.0, 0.0, 1.0)), 0.0);
    dst.uv = uv0;
    dst.material_id = (surf_flags >> 8u) & 0xFFu;
    dst.flags = surf_flags;

    AETHER_STORE(GpuSkinnedVertex, g_Pc.vertex_buf_bda, v_idx, dst);
}
