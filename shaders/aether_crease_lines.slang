#include "aether_common.slang"

// Pass 9: Screen-space interior crease lines (`> 35 deg` normal, `> 2%` rel depth, material boundary) (§10.7).
// Masked by D32_S8 read-only stencil `& 0x01` (valid character silhouette only).
// PushConstants:
//   param0: crease_mask storage slot (R8/R16F)
//   param1: (unused)
//   param2: width, param3: height
//   fparam0: normal threshold deg (default 35.0)
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    int2 p = int2(tid.xy);
    int2 max_p = int2(int(w) - 1, int(h) - 1);

    uint stencil_slot = fg.gbuffer_extra_ids[0].y;
    if (stencil_slot != 0xFFFFFFFFu) {
        uint st = g_UintSampledImages[stencil_slot].Load(int3(p, 0)).x;
        if ((st & 1u) == 0u) {
            g_StorageImages[g_Pc.param0][tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
            return;
        }
    }

    uint gb_n_slot = fg.gbuffer_ids[0].x;
    uint gb_mat_slot = fg.gbuffer_ids[0].z;
    uint depth_slot = fg.gbuffer_extra_ids[0].x;

    float d0 = g_SampledImages[depth_slot].Load(int3(p, 0)).x;
    if (d0 >= 0.99998) {
        g_StorageImages[g_Pc.param0][tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float z0 = abs(fg.proj.col3.z / max(abs(d0 + fg.proj.col2.z), 1e-5));
    float3 n0 = oct_decode(g_SampledImages[gb_n_slot].Load(int3(p, 0)).xy);
    uint m0 = g_UintSampledImages[gb_mat_slot].Load(int3(p, 0)).x;
    float cos_thresh = cos(radians(max(g_Pc.fparam0, 5.0)));

    float max_edge = 0.0;
    static const int2 kOffsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    for (int k = 0; k < 4; ++k) {
        int2 np = clamp(p + kOffsets[k], int2(0, 0), max_p);
        float d1 = g_SampledImages[depth_slot].Load(int3(np, 0)).x;
        if (d1 >= 0.99998) {
            continue;
        }
        float z1 = abs(fg.proj.col3.z / max(abs(d1 + fg.proj.col2.z), 1e-5));
        float3 n1 = oct_decode(g_SampledImages[gb_n_slot].Load(int3(np, 0)).xy);
        uint m1 = g_UintSampledImages[gb_mat_slot].Load(int3(np, 0)).x;

        float rel_depth = abs(z0 - z1) / max(z0, 0.05);
        float n_edge = (dot(n0, n1) < cos_thresh) ? 1.0 : 0.0;
        float z_edge = smoothstep(0.02, 0.05, rel_depth);
        float m_edge = (m0 != m1) ? 0.55 : 0.0;
        max_edge = max(max_edge, max(n_edge * 0.75, max(z_edge * 0.85, m_edge)));
    }

    g_StorageImages[g_Pc.param0][tid.xy] = float4(max_edge, 0.0, 0.0, 1.0);
}
