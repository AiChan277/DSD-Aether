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
    float4 center = aether_load(raw_slot, p);

    bool has_fg = (g_Pc.frame_globals_bda != 0ul);
    uint gb_n_slot = 0u;
    uint gb_mv_slot = 0u;
    uint depth_slot = 0u;
    float3 center_n = float3(0.0, 0.0, 1.0);
    float center_d = 0.5;
    float2 mv = float2(0.0, 0.0);
    bool is_path_trace = false;
    float2 vp_size = float2(float(w), float(h));
    if (has_fg) {
        GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
        gb_n_slot = fg.gbuffer_ids[0].x;
        gb_mv_slot = fg.gbuffer_ids[0].y;
        depth_slot = fg.gbuffer_extra_ids[0].x;
        is_path_trace = ((fg.state_u32.w & 0xC00u) == 0x800u);
        vp_size = max(fg.viewport_size.xy, float2(1.0, 1.0));
        center_d = aether_load(depth_slot, p).x;
        if (center_d >= 0.99995) {
            aether_store(dst_slot, tid.xy, center);
            return;
        }
        center_n = oct_decode(aether_load(gb_n_slot, p).xy);
        mv = aether_load(gb_mv_slot, p).xy;
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
            float4 s_val = aether_load(raw_slot, sp);
            m1 += s_val;
            m2 += s_val * s_val;

            int2 ap = clamp(p + int2(dx * 2, dy * 2), int2(0, 0), max_p);
            float4 a_val = aether_load(raw_slot, ap);
            float w_edge = kGauss[dx + 1] * kGauss[dy + 1];
            if (has_fg) {
                float3 sn = oct_decode(aether_load(gb_n_slot, ap).xy);
                float sd = aether_load(depth_slot, ap).x;
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

    // Path tracing stability (Task 3): suppress single-sample fireflies before temporal lock
    if (is_path_trace) {
        filtered = clamp(filtered, max(mu - 2.0 * std_dev, float4(0.0, 0.0, 0.0, 0.0)), mu + 2.0 * std_dev);
    }

    float px_speed = length(mv * vp_size);
    float gamma = is_path_trace ? lerp(1.15, 1.65, saturate(px_speed * 0.65)) : 1.50;
    float4 box_min = mu - gamma * std_dev;
    float4 box_max = mu + gamma * std_dev;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = aether_sample_level(hist_slot, kSamplerLinearClamp, prev_uv, 0.0);
            float4 hist_clamped = clamp(hist, box_min, box_max);
            float base_alpha = clamp(g_Pc.fparam0, 0.04, 1.0);
            float pt_alpha = lerp(0.045, max(base_alpha, 0.24), saturate(px_speed * 0.55));
            float alpha = is_path_trace ? pt_alpha : base_alpha;
            out_val = lerp(hist_clamped, filtered, alpha);
        }
    }

    aether_store(dst_slot, tid.xy, out_val);
}
