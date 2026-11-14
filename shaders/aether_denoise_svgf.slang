#include "aether_common.slang"

// Built-in SVGF-Lite spatio-temporal denoiser (§10.9, ADR-0009).
// PushConstants layout:
//   param0: low 16 = raw_sampled_slot, high 16 = history_sampled_slot
//   param1: low 16 = dst_storage_slot, bit 16 = has_valid_history (0 or 1)
//   param2: width, param3: height
//   fparam0: temporal_alpha (default 0.20)
//   fparam1: filter_scale (default 1.0)
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

    // 3x3 statistical moments for temporal variance clamping + edge-stopping spatial blur (à-trous stride 2)
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float4 spatial_sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight_sum = 0.0;

    static const float kGauss[3] = {0.25, 0.50, 0.25};
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                m1 += center;
                m2 += center * center;
                float w0 = kGauss[1] * kGauss[1];
                spatial_sum += center * w0;
                weight_sum += w0;
                continue;
            }
            int2 sp = clamp(p + int2(dx, dy), int2(0, 0), max_p);
            float4 s_val = g_SampledImages[raw_slot].Load(int3(sp, 0));
            m1 += s_val;
            m2 += s_val * s_val;

            int2 ap = clamp(p + int2(dx * 2, dy * 2), int2(0, 0), max_p);
            float4 a_val = g_SampledImages[raw_slot].Load(int3(ap, 0));
            float w_edge = kGauss[dx + 1] * kGauss[dy + 1];
            if (has_fg) {
                float3 sn = oct_decode(g_SampledImages[gb_n_slot].Load(int3(ap, 0)).xy);
                float sd = g_SampledImages[depth_slot].Load(int3(ap, 0)).x;
                float wn = pow(saturate(dot(center_n, sn)), 16.0);
                float wz = exp(-abs(center_d - sd) * 280.0);
                w_edge *= wn * wz;
            }
            spatial_sum += a_val * w_edge;
            weight_sum += w_edge;
        }
    }

    float4 filtered = (weight_sum > 1e-5) ? (spatial_sum / weight_sum) : center;
    float4 mu = m1 / 9.0;
    float4 variance = max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0));
    float4 std_dev = sqrt(variance);
    float4 box_min = mu - 1.5 * std_dev;
    float4 box_max = mu + 1.5 * std_dev;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = g_SampledImages[hist_slot].SampleLevel(g_Samplers[kSamplerLinearClamp], prev_uv, 0.0);
            float4 hist_clamped = clamp(hist, box_min, box_max);
            float alpha = clamp(g_Pc.fparam0, 0.05, 1.0);
            out_val = lerp(hist_clamped, filtered, alpha);
        }
    }

    g_StorageImages[dst_slot][tid.xy] = out_val;
}
