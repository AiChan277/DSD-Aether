#include "aether_common.slang"

// NVIDIA SIGMA-inspired penumbra-aware compute denoiser (§10.9, ADR-0009).
// Uses .y normalized ray hit distance to keep contact shadows tight and blur wide penumbras cleanly.
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
        center_d = g_SampledImages[depth_slot].Load(int3(p, 0)).x;
        if (center_d >= 0.99995) {
            g_StorageImages[dst_slot][tid.xy] = center;
            return;
        }
        center_n = oct_decode(g_SampledImages[gb_n_slot].Load(int3(p, 0)).xy);
        mv = g_SampledImages[gb_mv_slot].Load(int3(p, 0)).xy;
    }

    // Contact-hardening kernel: small radius for close hits (y -> 0), wider for soft penumbras (y -> 1)
    int radius_step = (center.y < 0.20) ? 1 : 2;
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                m1 += center;
                m2 += center * center;
                acc += center;
                wsum += 1.0;
                continue;
            }
            int2 qp = clamp(p + int2(dx * radius_step, dy * radius_step), int2(0, 0), max_p);
            float4 sv = g_SampledImages[raw_slot].Load(int3(qp, 0));
            if (radius_step == 1) {
                m1 += sv;
                m2 += sv * sv;
            } else {
                int2 q3 = clamp(p + int2(dx, dy), int2(0, 0), max_p);
                float4 s3 = g_SampledImages[raw_slot].Load(int3(q3, 0));
                m1 += s3;
                m2 += s3 * s3;
            }
            float dist2 = float(dx * dx + dy * dy);
            float wk = exp(-0.45 * dist2);
            if (has_fg) {
                float3 qn = oct_decode(g_SampledImages[gb_n_slot].Load(int3(qp, 0)).xy);
                float qd = g_SampledImages[depth_slot].Load(int3(qp, 0)).x;
                wk *= pow(saturate(dot(center_n, qn)), 6.0) * exp(-abs(center_d - qd) * 80.0);
            }
            acc += sv * wk;
            wsum += wk;
        }
    }

    // 4 outer cardinal penumbra taps when filtering soft shadows so 1-2 spp penumbras smooth cleanly
    static const int2 kOuterOffsets[4] = {int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2)};
    for (int oi = 0; oi < 4; ++oi) {
        int2 qp = clamp(p + kOuterOffsets[oi] * radius_step, int2(0, 0), max_p);
        float4 sv = g_SampledImages[raw_slot].Load(int3(qp, 0));
        float wk = 0.25;
        if (has_fg) {
            float3 qn = oct_decode(g_SampledImages[gb_n_slot].Load(int3(qp, 0)).xy);
            float qd = g_SampledImages[depth_slot].Load(int3(qp, 0)).x;
            wk *= pow(saturate(dot(center_n, qn)), 6.0) * exp(-abs(center_d - qd) * 80.0);
        }
        acc += sv * wk;
        wsum += wk;
    }

    float4 filtered = (wsum > 1e-5) ? (acc / wsum) : center;
    float4 mu = m1 / 9.0;
    float4 std_dev = sqrt(max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0)));
    float4 box_min = mu - 1.35 * std_dev;
    float4 box_max = mu + 1.35 * std_dev;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = g_SampledImages[hist_slot].SampleLevel(g_Samplers[kSamplerLinearClamp], prev_uv, 0.0);
            float4 clamped_hist = clamp(hist, box_min, box_max);
            // Faster response on sharp contact edges, calmer blend in smooth penumbra
            float adapt_alpha = lerp(max(g_Pc.fparam0, 0.30), g_Pc.fparam0, center.y);
            out_val = lerp(clamped_hist, filtered, clamp(adapt_alpha, 0.05, 1.0));
        }
    }

    g_StorageImages[dst_slot][tid.xy] = out_val;
}
