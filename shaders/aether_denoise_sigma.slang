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

    // Contact-hardening kernel: small radius for close hits (y -> 0), wider for soft penumbras (y -> 1).
    // In Path Tracing mode (.yzw holds 1-bounce GI irradiance), always use wide kernel to stabilize Monte Carlo GI.
    int radius_step = is_path_trace ? 2 : ((center.y < 0.20) ? 1 : 2);
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                float4 c_val = is_path_trace ? clamp(center, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 2.5, 2.5, 2.5)) : center;
                m1 += c_val;
                m2 += c_val * c_val;
                acc += c_val;
                wsum += 1.0;
                continue;
            }
            int2 qp = clamp(p + int2(dx * radius_step, dy * radius_step), int2(0, 0), max_p);
            float4 sv = aether_load(raw_slot, qp);
            if (is_path_trace) {
                sv = clamp(sv, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 2.5, 2.5, 2.5));
            }
            if (radius_step == 1) {
                m1 += sv;
                m2 += sv * sv;
            } else {
                int2 q3 = clamp(p + int2(dx, dy), int2(0, 0), max_p);
                float4 s3 = aether_load(raw_slot, q3);
                if (is_path_trace) {
                    s3 = clamp(s3, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 2.5, 2.5, 2.5));
                }
                m1 += s3;
                m2 += s3 * s3;
            }
            float dist2 = float(dx * dx + dy * dy);
            float wk = exp(-0.45 * dist2);
            if (has_fg) {
                float3 qn = oct_decode(aether_load(gb_n_slot, qp).xy);
                float qd = aether_load(depth_slot, qp).x;
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
        float4 sv = aether_load(raw_slot, qp);
        if (is_path_trace) {
            sv = clamp(sv, float4(0.0, 0.0, 0.0, 0.0), float4(1.0, 2.5, 2.5, 2.5));
        }
        float wk = 0.25;
        if (has_fg) {
            float3 qn = oct_decode(aether_load(gb_n_slot, qp).xy);
            float qd = aether_load(depth_slot, qp).x;
            wk *= pow(saturate(dot(center_n, qn)), 6.0) * exp(-abs(center_d - qd) * 80.0);
        }
        acc += sv * wk;
        wsum += wk;
    }

    float4 filtered = (wsum > 1e-5) ? (acc / wsum) : center;
    float4 mu = m1 / 9.0;
    float4 std_dev = sqrt(max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0)));
    float box_scale = is_path_trace ? 2.25 : 1.35;
    float box_pad = is_path_trace ? 0.015 : 0.0;
    float4 box_min = mu - box_scale * std_dev - box_pad;
    float4 box_max = mu + box_scale * std_dev + box_pad;

    float4 out_val = filtered;
    if (has_hist) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float4 hist = aether_sample_level(hist_slot, kSamplerLinearClamp, prev_uv, 0.0);
            float4 clamped_hist = clamp(hist, box_min, box_max);
            float adapt_alpha = lerp(max(g_Pc.fparam0, 0.30), g_Pc.fparam0, center.y);
            if (is_path_trace) {
                float motion_px = length(mv * float2(float(w), float(h)));
                adapt_alpha = lerp(0.045, 0.22, saturate(motion_px * 0.15));
                if (motion_px > 0.25) {
                    int2 prev_p = clamp(int2(prev_uv * float2(float(w), float(h))), int2(0, 0), max_p);
                    float3 prev_n = oct_decode(aether_load(gb_n_slot, prev_p).xy);
                    float prev_d = aether_load(depth_slot, prev_p).x;
                    if (dot(center_n, prev_n) < 0.70 || abs(center_d - prev_d) > 0.02) {
                        adapt_alpha = max(adapt_alpha, 0.45);
                    }
                }
                out_val = lerp(clamped_hist, filtered, clamp(adapt_alpha, 0.045, 1.0));
            } else {
                out_val = lerp(clamped_hist, filtered, clamp(adapt_alpha, 0.05, 1.0));
            }
        }
    }

    aether_store(dst_slot, tid.xy, out_val);
}
