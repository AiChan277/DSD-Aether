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
    float4 center = aether_load(raw_slot, p);

    bool has_fg = (g_Pc.frame_globals_bda != 0ul);
    uint gb_n_slot = 0u;
    uint gb_mv_slot = 0u;
    uint depth_slot = 0u;
    float3 center_n = float3(0.0, 0.0, 1.0);
    float center_d = 0.5;
    float2 mv = float2(0.0, 0.0);
    bool is_path_trace = false;
    if (has_fg) {
        GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
        gb_n_slot = fg.gbuffer_ids[0].x;
        gb_mv_slot = fg.gbuffer_ids[0].y;
        depth_slot = fg.gbuffer_extra_ids[0].x;
        is_path_trace = ((fg.state_u32.w & 0xC00u) == 0x800u);
        center_d = aether_load(depth_slot, p).x;
        if (center_d >= 0.99995) {
            aether_store(dst_slot, tid.xy, center);
            return;
        }
        center_n = oct_decode(aether_load(gb_n_slot, p).xy);
        mv = aether_load(gb_mv_slot, p).xy;
    }

    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;
    int radius_step = is_path_trace ? 2 : 1;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 qp = clamp(p + int2(dx * radius_step, dy * radius_step), int2(0, 0), max_p);
            float4 sv = aether_load(raw_slot, qp);
            if (is_path_trace) {
                sv = clamp(sv, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 2.5, 2.5, 2.5));
            }
            m1 += sv;
            m2 += sv * sv;

            float wk = exp(-0.4 * float(dx * dx + dy * dy));
            if (has_fg) {
                float3 qn = oct_decode(aether_load(gb_n_slot, qp).xy);
                float qd = aether_load(depth_slot, qp).x;
                float n_exp = is_path_trace ? 12.0 : 18.0;
                float d_scale = is_path_trace ? 180.0 : 300.0;
                wk *= pow(saturate(dot(center_n, qn)), n_exp) * exp(-abs(center_d - qd) * d_scale);
            }
            acc += sv * wk;
            wsum += wk;
        }
    }

    float4 filtered = (wsum > 1e-5) ? (acc / wsum) : center;
    float4 mu = m1 / 9.0;
    float4 std_dev = sqrt(max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0)));
    float box_scale = is_path_trace ? 2.25 : 1.45;
    float box_pad = is_path_trace ? 0.015 : 0.0;
    float4 box_min = mu - box_scale * std_dev - box_pad;
    float4 box_max = mu + box_scale * std_dev + box_pad;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = aether_sample_level(hist_slot, kSamplerLinearClamp, prev_uv, 0.0);
            if (is_path_trace) {
                float motion_px = length(mv * float2(float(w), float(h)));
                float pt_alpha = lerp(0.045, 0.22, saturate(motion_px * 0.15));
                if (motion_px > 0.25) {
                    int2 prev_p = clamp(int2(prev_uv * float2(float(w), float(h))), int2(0, 0), max_p);
                    float3 prev_n = oct_decode(aether_load(gb_n_slot, prev_p).xy);
                    float prev_d = aether_load(depth_slot, prev_p).x;
                    if (dot(center_n, prev_n) < 0.70 || abs(center_d - prev_d) > 0.02) {
                        pt_alpha = max(pt_alpha, 0.45);
                    }
                }
                out_val = lerp(clamp(hist, box_min, box_max), filtered, clamp(pt_alpha, 0.045, 1.0));
            } else {
                out_val = lerp(clamp(hist, box_min, box_max), filtered, clamp(g_Pc.fparam0, 0.05, 1.0));
            }
        }
    }

    aether_store(dst_slot, tid.xy, out_val);
}
