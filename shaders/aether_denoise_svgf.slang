#include "aether_common.slang"

// Spatiotemporal Variance-Guided Filter (SVGF) denoiser with normal, depth, material-ID, and variance weights.
// Supports both full-res (RT shadow + blocker hit_t + backscatter) and half-res (indirect bounce GI).
// PushConstants layout:
//   param0: low 16 = raw_sampled_slot, high 16 = history_sampled_slot
//   param1: low 16 = dst_storage_slot, bit 16 = has_valid_history (0 or 1)
//   param2: width, param3: height
//   fparam0: temporal_alpha (default 0.10 in PT, 0.20 in Hybrid)
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
    uint gb_mat_slot = 0u;
    uint depth_slot = 0u;
    float3 center_n = float3(0.0, 0.0, 1.0);
    float center_d = 0.5;
    uint center_m = 0u;
    float2 mv = float2(0.0, 0.0);
    bool is_path_trace = false;
    bool fast_pan = false;
    float2 vp_size = float2(float(w), float(h));
    int2 max_gp = max_p;
    float2 gb_scale = float2(1.0, 1.0);
    float2 hist_uv_scale = float2(1.0, 1.0);
    float2 hist_uv_max = float2(1.0, 1.0);

    if (has_fg) {
        GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
        gb_n_slot = fg.gbuffer_ids[0].x;
        gb_mv_slot = fg.gbuffer_ids[0].y;
        gb_mat_slot = fg.gbuffer_ids[0].z;
        depth_slot = fg.gbuffer_extra_ids[0].x;
        is_path_trace = ((fg.state_u32.w & 0xC00u) == 0x800u);
        fast_pan = (fg.stage_camera_params.w > 0.5);
        vp_size = max(fg.viewport_size.xy, float2(1.0, 1.0));
        max_gp = max(int2(vp_size) - 1, int2(0, 0));
        gb_scale = vp_size / float2(float(w), float(h));
        bool is_half_res = (w < uint(vp_size.x * 0.75 + 0.5));
        float4 hscale = is_half_res ? fg.target_half_uv_scale : fg.target_uv_scale;
        hist_uv_scale = hscale.xy;
        hist_uv_max = hscale.zw;

        int2 gp0 = clamp(int2((float2(p) + 0.5) * gb_scale), int2(0, 0), max_gp);
        center_d = aether_load(depth_slot, gp0).x;
        if (center_d >= 0.99995) {
            aether_store(dst_slot, tid.xy, center);
            return;
        }
        center_n = oct_decode(aether_load(gb_n_slot, gp0).xy);
        center_m = aether_uload(gb_mat_slot, gp0).x & 0x7FFFu;
        mv = aether_load(gb_mv_slot, gp0).xy;
    }

    // 1) Local 3x3 statistical variance estimation for variance-guided à-trous spatial weighting
    float4 m1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 m2 = float4(0.0, 0.0, 0.0, 0.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 sp = clamp(p + int2(dx, dy), int2(0, 0), max_p);
            float4 sv = aether_load(raw_slot, sp);
            m1 += sv;
            m2 += sv * sv;
        }
    }
    float4 mu = m1 / 9.0;
    float4 variance = max(m2 / 9.0 - mu * mu, float4(0.0, 0.0, 0.0, 0.0));
    float4 std_dev = sqrt(variance);
    float center_l = luminance(center.rgb);
    float sigma_var = max(luminance(std_dev.rgb) * 1.5, 0.04);

    // 2) 3x3 variance-guided à-trous wavelet filter with normal, depth, and material-ID edge stopping
    float4 spatial_sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    static const float kGauss[3] = {0.25, 0.50, 0.25};
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 ap = clamp(p + int2(dx * 2, dy * 2), int2(0, 0), max_p);
            float4 a_val = aether_load(raw_slot, ap);
            float w_edge = kGauss[dx + 1] * kGauss[dy + 1];
            if (has_fg && !(dx == 0 && dy == 0)) {
                int2 gp = clamp(int2((float2(ap) + 0.5) * gb_scale), int2(0, 0), max_gp);
                float3 sn = oct_decode(aether_load(gb_n_slot, gp).xy);
                float sd = aether_load(depth_slot, gp).x;
                uint raw_m = aether_uload(gb_mat_slot, gp).x;
                uint sm = raw_m & 0x7FFFu;
                uint sclass = (raw_m >> 10u) & 0x0Fu;
                uint cclass = (center_m >> 10u) & 0x0Fu;

                float wn = pow(saturate(dot(center_n, sn)), 16.0);
                float wz = exp(-abs(center_d - sd) * 280.0);
                float wm = (center_m == sm && cclass == sclass) ? 1.0 : 0.02;
                float wl = exp(-abs(center_l - luminance(a_val.rgb)) / (2.5 * sigma_var));
                w_edge *= wn * wz * wm * wl;
            }
            spatial_sum += a_val * w_edge;
            weight_sum += w_edge;
        }
    }

    float4 filtered = (weight_sum > 1e-5) ? (spatial_sum / weight_sum) : center;
    if (is_path_trace) {
        filtered = clamp(filtered, max(mu - 2.0 * std_dev, float4(0.0, 0.0, 0.0, 0.0)), mu + 2.0 * std_dev);
    }

    float px_speed = length(mv * vp_size);
    float gamma = is_path_trace ? lerp(1.15, 1.65, saturate(px_speed * 0.65)) : 1.50;
    float4 box_min = max(mu - gamma * std_dev, float4(0.0, 0.0, 0.0, 0.0));
    float4 box_max = mu + gamma * std_dev;

    // 3) Motion-vector reprojected temporal accumulation (bypassed during Stage fast camera pan)
    float4 out_val = filtered;
    if (has_hist && !fast_pan) {
        float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
        float2 prev_uv = uv - mv;
        if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
            float2 hist_uv = clamp(prev_uv * hist_uv_scale, float2(0.0, 0.0), hist_uv_max);
            float4 hist = aether_sample_level(hist_slot, kSamplerLinearClamp, hist_uv, 0.0);
            float4 hist_clamped = clamp(hist, box_min, box_max);
            float base_alpha = clamp(g_Pc.fparam0, 0.04, 1.0);
            float pt_alpha = lerp(0.10, max(base_alpha, 0.24), saturate(px_speed * 0.55));
            float alpha = is_path_trace ? pt_alpha : base_alpha;
            if (has_fg && px_speed > 0.25) {
                int2 prev_gp = clamp(int2(prev_uv * vp_size), int2(0, 0), max_gp);
                float3 prev_n = oct_decode(aether_load(gb_n_slot, prev_gp).xy);
                float prev_d = aether_load(depth_slot, prev_gp).x;
                uint prev_m = aether_uload(gb_mat_slot, prev_gp).x & 0x7FFFu;
                if (dot(center_n, prev_n) < 0.72 || abs(center_d - prev_d) > 0.025 || center_m != prev_m) {
                    alpha = max(alpha, 0.45);
                }
            }
            out_val = lerp(hist_clamped, filtered, alpha);
        }
    }

    aether_store(dst_slot, tid.xy, out_val);
}
