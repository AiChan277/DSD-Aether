#include "aether_common.slang"

// Offline 2D face shadow SDF normal-sweep bake shader (§10.6.2, P4-10).
// aux_buf_bda points to float4[w*h] interpolated object-space face proxy normals (.xyz, w = valid mask).
// param0 = dst storage image slot (R8), param1 = azimuth_steps (default 16), param2 = width, param3 = height.
// fparam0..2 = face forward axis (normalized), fparam3 unused.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    uint idx = tid.y * w + tid.x;
    float4 n_mask = AETHER_LOAD(float4, g_Pc.aux_buf_bda, idx);
    if (n_mask.w < 0.5) {
        // Fallback neutral SDF value outside painted UV island
        g_StorageImages[g_Pc.param0][tid.xy] = float4(0.5, 0.5, 0.5, 1.0);
        return;
    }

    float3 n = normalize(n_mask.xyz);
    float3 fwd = float3(g_Pc.fparam0, g_Pc.fparam1, g_Pc.fparam2);
    float flen = length(fwd);
    fwd = (flen > 1e-4) ? (fwd / flen) : float3(0.0, 0.0, 1.0);
    float3 up = abs(fwd.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 right = normalize(cross(up, fwd));

    uint steps = clamp(g_Pc.param1, 4u, 64u);
    float lit_acc = 0.0;
    for (uint s = 0u; s < steps; ++s) {
        float theta = ((float(s) + 0.5) / float(steps)) * 3.14159265;
        float3 ldir = fwd * cos(theta) + right * sin(theta);
        float ndotl = dot(n, ldir);
        lit_acc += smoothstep(-0.04, 0.04, ndotl);
    }
    float sdf_val = saturate(lit_acc / float(steps));
    g_StorageImages[g_Pc.param0][tid.xy] = float4(sdf_val, sdf_val, sdf_val, 1.0);
}
