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
        uint st = aether_uload(stencil_slot, p).x;
        if ((st & 1u) == 0u) {
            aether_store(g_Pc.param0, tid.xy, float4(0.0, 0.0, 0.0, 1.0));
            return;
        }
    }

    uint gb_n_slot = fg.gbuffer_ids[0].x;
    uint gb_mat_slot = fg.gbuffer_ids[0].z;
    uint gb_misc_slot = fg.gbuffer_ids[0].w;
    uint depth_slot = fg.gbuffer_extra_ids[0].x;

    float d0 = aether_load(depth_slot, p).x;
    if (d0 >= 0.99998) {
        aether_store(g_Pc.param0, tid.xy, float4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    float z0 = abs(fg.proj.col3.z / max(abs(d0 + fg.proj.col2.z), 1e-5));
    float3 n0 = oct_decode(aether_load(gb_n_slot, p).xy);
    uint m0 = aether_uload(gb_mat_slot, p).x;
    // Mask crease ink cleanly off flat face planes (`gb_misc.y` = sdf_mask) so cheeks/nose stay 2D clean
    float face_mask = saturate(aether_load(gb_misc_slot, p).y);
    float plane_keep = 1.0 - 0.88 * face_mask;
    float cos_thresh = cos(radians(max(g_Pc.fparam0, 5.0)));

    // 3x3 Sobel + Flow-guided DoG curvature ink operator (Task 6)
    float3 sob_nx = float3(0.0, 0.0, 0.0);
    float3 sob_ny = float3(0.0, 0.0, 0.0);
    float sob_zx = 0.0;
    float sob_zy = 0.0;
    float dog_n = 0.0;
    float dog_z = 0.0;
    float max_n_ang = 0.0;
    float max_mat_edge = 0.0;

    static const float kSx[9] = {-1.0, 0.0, 1.0, -2.0, 0.0, 2.0, -1.0, 0.0, 1.0};
    static const float kSy[9] = {-1.0, -2.0, -1.0, 0.0, 0.0, 0.0, 1.0, 2.0, 1.0};
    int k = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 np = clamp(p + int2(dx, dy), int2(0, 0), max_p);
            float d1 = aether_load(depth_slot, np).x;
            float z1 = (d1 >= 0.99998) ? (z0 + 0.5) : abs(fg.proj.col3.z / max(abs(d1 + fg.proj.col2.z), 1e-5));
            float3 n1 = (d1 >= 0.99998) ? n0 : oct_decode(aether_load(gb_n_slot, np).xy);
            uint m1 = aether_uload(gb_mat_slot, np).x;

            float kx = kSx[k];
            float ky = kSy[k];
            sob_nx += n1 * kx;
            sob_ny += n1 * ky;
            float rel_z = clamp((z1 - z0) / max(z0, 0.05), -0.5, 0.5);
            sob_zx += rel_z * kx;
            sob_zy += rel_z * ky;
            dog_z += max(rel_z, 0.0);
            dog_n += (1.0 - saturate(dot(n0, n1)));

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
    bool filter_pro = ((fg.state_u32.w & 4u) != 0u);
    float dog_boost = filter_pro ? max(fg.filter_pro_params.y, 0.0) : 0.0;
    float dist_taper = clamp(1.4 / max(z0, 0.5), 0.55, 1.35);

    float n_edge = max(smoothstep(0.65, 1.32, n_sobel + dog_n * 0.25 * dog_boost), max_n_ang * 0.80) * plane_keep;
    float z_edge = smoothstep(0.055, 0.145, z_sobel + dog_z * 0.04 * dog_boost) * plane_keep * dist_taper;
    float max_edge = max(n_edge * 0.85, max(z_edge, max_mat_edge));

    aether_store(g_Pc.param0, tid.xy, float4(saturate(max_edge), 0.0, 0.0, 1.0));
}
