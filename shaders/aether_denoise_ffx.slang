#include "aether_common.slang"

// AMD/Intel FidelityFX-style variance-guided spatiotemporal compute denoiser (§10.9, ADR-0009).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    uint raw_slot = g_Pc.param0 & 0xFFFFu;
    uint hist_slot = (g_Pc.param0 >> 16u) & 0xFFFFu;
    uint dst_slot = g_Pc.param1 & 0xFFFFu;
    bool has_hist = ((g_Pc.param1 >> 16u) & 1u) != 0u;

    int2 p = int2(tid.xy);
    int2 max_p = int2(int(w) - 1, int(h) - 1);
    float4 center = g_SampledImages[raw_slot].Load(int3(p, 0));

    bool has_fg = (g_Pc.frame_globals_bda != 0ul);
    uint gb_n_slot = 0u;
    uint gb_mv_slot = 0u;
    uint depth_slot = 0u;
    float3 center_n = float3(0.0, 0.0, 1.0);
    float center_d = 0.5;
    float2 mv = float2(0.0, 0.0);
    if (has_fg) {
        GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
        gb_n_slot = fg.gbuffer_ids[0].x;
        gb_mv_slot = fg.gbuffer_ids[0].y;
        depth_slot = fg.gbuffer_extra_ids[0].x;
        center_n = oct_decode(g_SampledImages[gb_n_slot].Load(int3(p, 0)).xy);
        mv = g_SampledImages[gb_mv_slot].Load(int3(p, 0)).xy;
        center_d = g_SampledImages[depth_slot].Load(int3(p, 0)).x;
    }

    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 qp = clamp(p + int2(dx, dy), int2(0, 0), max_p);
            float4 sv = g_SampledImages[raw_slot].Load(int3(qp, 0));
            m1 += sv;
            m2 += sv * sv;

            float wk = exp(-0.4 * float(dx * dx + dy * dy));
            if (has_fg) {
                float3 qn = oct_decode(g_SampledImages[gb_n_slot].Load(int3(qp, 0)).xy);
                float qd = g_SampledImages[depth_slot].Load(int3(qp, 0)).x;
                wk *= pow(saturate(dot(center_n, qn)), 18.0) * exp(-abs(center_d - qd) * 300.0);
            }
            acc += sv * wk;
            wsum += wk;
        }
    }

    float4 filtered = (wsum > 1e-5) ? (acc / wsum) : center;
    float4 mu = m1 / 9.0;
    float4 std_dev = sqrt(max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0)));
    float4 box_min = mu - 1.45 * std_dev;
    float4 box_max = mu + 1.45 * std_dev;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = g_SampledImages[hist_slot].SampleLevel(g_Samplers[kSamplerLinearClamp], prev_uv, 0.0);
            out_val = lerp(clamp(hist, box_min, box_max), filtered, clamp(g_Pc.fparam0, 0.05, 1.0));
        }
    }

    g_StorageImages[dst_slot][tid.xy] = out_val;
}
