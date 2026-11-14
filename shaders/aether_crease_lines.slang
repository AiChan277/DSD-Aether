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
    uint gb_misc_slot = fg.gbuffer_ids[0].w;
    uint depth_slot = fg.gbuffer_extra_ids[0].x;

    float d0 = g_SampledImages[depth_slot].Load(int3(p, 0)).x;
    if (d0 >= 0.99998) {
        g_StorageImages[g_Pc.param0][tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float z0 = abs(fg.proj.col3.z / max(abs(d0 + fg.proj.col2.z), 1e-5));
    float3 n0 = oct_decode(g_SampledImages[gb_n_slot].Load(int3(p, 0)).xy);
    uint m0 = g_UintSampledImages[gb_mat_slot].Load(int3(p, 0)).x;
    // Mask crease ink cleanly off flat face planes (`gb_misc.y` = sdf_mask) so cheeks/nose stay 2D clean
    float face_mask = saturate(g_SampledImages[gb_misc_slot].Load(int3(p, 0)).y);
    float plane_keep = 1.0 - 0.88 * face_mask;
    float cos_thresh = cos(radians(max(g_Pc.fparam0, 5.0)));

    // 3x3 Sobel gradient operator over view-space normals and linearized view depth
    float3 sob_nx = float3(0.0, 0.0, 0.0);
    float3 sob_ny = float3(0.0, 0.0, 0.0);
    float sob_zx = 0.0;
    float sob_zy = 0.0;
    float max_n_ang = 0.0;
    float max_mat_edge = 0.0;

    static const float kSx[9] = {-1.0, 0.0, 1.0, -2.0, 0.0, 2.0, -1.0, 0.0, 1.0};
    static const float kSy[9] = {-1.0, -2.0, -1.0, 0.0, 0.0, 0.0, 1.0, 2.0, 1.0};
    int k = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 np = clamp(p + int2(dx, dy), int2(0, 0), max_p);
            float d1 = g_SampledImages[depth_slot].Load(int3(np, 0)).x;
            float z1 = (d1 >= 0.99998) ? (z0 + 0.5) : abs(fg.proj.col3.z / max(abs(d1 + fg.proj.col2.z), 1e-5));
            float3 n1 = (d1 >= 0.99998) ? n0 : oct_decode(g_SampledImages[gb_n_slot].Load(int3(np, 0)).xy);
            uint m1 = g_UintSampledImages[gb_mat_slot].Load(int3(np, 0)).x;

            float kx = kSx[k];
            float ky = kSy[k];
            sob_nx += n1 * kx;
            sob_ny += n1 * ky;
            float rel_z = clamp((z1 - z0) / max(z0, 0.05), -0.5, 0.5);
            sob_zx += rel_z * kx;
            sob_zy += rel_z * ky;

            if (abs(dx) + abs(dy) == 1 && d1 < 0.99998) {
                if (dot(n0, n1) < cos_thresh) {
                    max_n_ang = 1.0;
                }
                if (m0 != m1) {
                    max_mat_edge = 0.55;
                }
            }
            ++k;
        }
    }

    float n_sobel = sqrt(dot(sob_nx, sob_nx) + dot(sob_ny, sob_ny));
    float z_sobel = sqrt(sob_zx * sob_zx + sob_zy * sob_zy);
    float n_edge = max(smoothstep(0.65, 1.35, n_sobel), max_n_ang * 0.80) * plane_keep;
    float z_edge = smoothstep(0.06, 0.16, z_sobel) * plane_keep;
    float max_edge = max(n_edge * 0.85, max(z_edge * 0.95, max_mat_edge));

    g_StorageImages[g_Pc.param0][tid.xy] = float4(saturate(max_edge), 0.0, 0.0, 1.0);
}
