#include "aether_common.slang"

// Pass 10: 6-mip Dual-Kawase premultiplied bloom (`rgb + glow`, `a + (1 - a) * lum(glow)`).
// fparam0 > 0 on mip 0 threshold extraction pass; (fparam1, fparam2) = src texel size (1/w, 1/h).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_down(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
    float2 hp = float2(g_Pc.fparam1, g_Pc.fparam2);

    float4 s = aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv, 0) * 4.0;
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(-hp.x, -hp.y), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(hp.x, -hp.y), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(-hp.x, hp.y), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(hp.x, hp.y), 0);
    s *= (1.0 / 8.0);

    if (g_Pc.fparam0 > 0.0) {
        float lum = max(max(s.r, s.g), s.b);
        float thresh = g_Pc.fparam0;
        float soft = 0.25;
        float rq = clamp(lum - thresh + soft, 0.0, 2.0 * soft);
        float excess = max(lum - thresh, (rq * rq) / max(4.0 * soft, 1e-4));
        float gain = (lum > 1e-4) ? saturate(excess / lum) : 0.0;
        s *= gain;
    }

    aether_store(g_Pc.param1, tid.xy, max(s, float4(0.0, 0.0, 0.0, 0.0)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_up(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
    float2 hp = float2(g_Pc.fparam1, g_Pc.fparam2);

    float4 s = float4(0.0, 0.0, 0.0, 0.0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(-hp.x * 2.0, 0.0), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(hp.x * 2.0, 0.0), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(0.0, -hp.y * 2.0), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(0.0, hp.y * 2.0), 0);
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(-hp.x, -hp.y), 0) * 2.0;
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(hp.x, -hp.y), 0) * 2.0;
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(-hp.x, hp.y), 0) * 2.0;
    s += aether_sample_level(g_Pc.param0, kSamplerLinearClamp, uv + float2(hp.x, hp.y), 0) * 2.0;
    s *= (1.0 / 12.0);

    aether_store(g_Pc.param1, tid.xy, max(s, float4(0.0, 0.0, 0.0, 0.0)));
}

float3 visualize_debug_mode(GpuFrameGlobals fg, uint mode, int2 p, float2 uv) {
    if (mode == 2u) {
        float3 n = oct_decode(aether_load(fg.gbuffer_ids[0].x, p).xy);
        return n * 0.5 + 0.5;
    }
    if (mode == 3u) {
        float d = aether_load(fg.gbuffer_extra_ids[0].x, p).x;
        return float3(d, 1.0 - d, 0.35);
    }
    if (mode == 4u) {
        float zf = aether_load(fg.gbuffer_ids[0].w, p).x;
        return float3(zf, zf * 0.65, 1.0 - zf);
    }
    if (mode == 5u) {
        float m = aether_load(fg.gbuffer_ids[0].w, p).y;
        return float3(m, m * 0.85, 0.25);
    }
    if (mode == 6u) {
        uint st = aether_uload(fg.gbuffer_extra_ids[0].y, p).x;
        float cut = ((st & 2u) != 0u) ? 0.95 : 0.06;
        float hair = ((st & 4u) != 0u) ? 0.90 : 0.06;
        float base = ((st & 1u) != 0u) ? 0.45 : 0.06;
        return float3(hair, max(base, cut * 0.55), cut);
    }
    if (mode == 7u) {
        return aether_sample_level(fg.gbuffer_ids[1].x, kSamplerLinearClamp, uv, 0).xxx;
    }
    if (mode == 8u) {
        return aether_load(fg.gbuffer_extra_ids[0].z, p).xxx;
    }
    if (mode == 9u) {
        return aether_load(fg.gbuffer_extra_ids[0].w, p).xxx;
    }
    if (mode == 10u) {
        return aether_sample_level(fg.gbuffer_extra_ids[1].x, kSamplerLinearClamp, uv, 0).rgb;
    }
    if (mode == 11u) {
        float cr = aether_load(fg.gbuffer_ids[1].y, p).x;
        return float3(cr, cr * 0.4, 0.1);
    }
    if (mode == 12u) {
        float2 mv = aether_load(fg.gbuffer_ids[0].y, p).xy;
        return float3(abs(mv.x) * 40.0, abs(mv.y) * 40.0, 0.25);
    }
    return float3(0.0, 0.0, 0.0);
}

// Single-pass directional 4-quadrant Kuwahara / bilateral toon flattener (Task 6).
float3 apply_2d_filter_pro_kuwahara(uint src_slot, int2 p, int2 max_p, float3 center_rgb, float center_a,
                                    float crease_edge, float strength) {
    if (center_a <= 0.02 || strength <= 0.001) {
        return center_rgb;
    }
    static const int2 kQuadStarts[4] = {int2(-1, -1), int2(0, -1), int2(-1, 0), int2(0, 0)};
    float3 best_mean = center_rgb;
    float best_var = 1e9;
    float center_l = luminance(center_rgb);
    float edge_protect = 1.0;

    for (int qi = 0; qi < 4; ++qi) {
        int2 qs = kQuadStarts[qi];
        float3 sum_c = float3(0.0, 0.0, 0.0);
        float3 sum_c2 = float3(0.0, 0.0, 0.0);
        float wsum = 0.0;
        for (int dy = 0; dy <= 1; ++dy) {
            for (int dx = 0; dx <= 1; ++dx) {
                int2 sp = clamp(p + qs + int2(dx, dy), int2(0, 0), max_p);
                float4 s = aether_load(src_slot, sp);
                float dl = abs(luminance(s.rgb) - center_l);
                float da = abs(s.a - center_a);
                edge_protect = min(edge_protect, 1.0 - smoothstep(0.06, 0.20, dl + da * 2.0));
                float w = (s.a > 0.02) ? exp(-dl * 8.0) : 0.0;
                sum_c += s.rgb * w;
                sum_c2 += (s.rgb * s.rgb) * w;
                wsum += w;
            }
        }
        if (wsum > 1e-4) {
            float3 mean = sum_c / wsum;
            float3 var3 = max(sum_c2 / wsum - mean * mean, float3(0.0, 0.0, 0.0));
            float var_l = var3.r + var3.g + var3.b;
            if (var_l < best_var) {
                best_var = var_l;
                best_mean = mean;
            }
        }
    }
    float apply_w = saturate(strength) * edge_protect * (1.0 - saturate(crease_edge * 2.5));
    return lerp(center_rgb, best_mean, apply_w);
}

// Pass 11: Bloom + Crease + 2D Filter Pro + Tone Map + Transparent Background Composition -> OUT_RGBA16F.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_composite(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    int2 p = int2(tid.xy);
    int2 max_p = int2(int(w) - 1, int(h) - 1);
    float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
    uint debug_view = fg.state_u32.z;

    if (debug_view >= 2u && debug_view <= 12u) {
        float3 dbg = visualize_debug_mode(fg, debug_view, p, uv);
        aether_store(g_Pc.param1, tid.xy, float4(dbg, 1.0));
        return;
    }

    float4 hdr = aether_load(g_Pc.param0, p);
    uint aa_mode = (fg.state_u32.w >> 8u) & 3u;
    if (aa_mode != 0u && debug_view == 0u) {
        float4 c_n = aether_load(g_Pc.param0, clamp(p + int2(0, -1), int2(0, 0), max_p));
        float4 c_s = aether_load(g_Pc.param0, clamp(p + int2(0, 1), int2(0, 0), max_p));
        float4 c_w = aether_load(g_Pc.param0, clamp(p + int2(-1, 0), int2(0, 0), max_p));
        float4 c_e = aether_load(g_Pc.param0, clamp(p + int2(1, 0), int2(0, 0), max_p));
        float l_c = dot(hdr.rgb, float3(0.2126, 0.7152, 0.0722));
        float l_n = dot(c_n.rgb, float3(0.2126, 0.7152, 0.0722));
        float l_s = dot(c_s.rgb, float3(0.2126, 0.7152, 0.0722));
        float l_w = dot(c_w.rgb, float3(0.2126, 0.7152, 0.0722));
        float l_e = dot(c_e.rgb, float3(0.2126, 0.7152, 0.0722));
        float l_min = min(l_c, min(min(l_n, l_s), min(l_w, l_e)));
        float l_max = max(l_c, max(max(l_n, l_s), max(l_w, l_e)));
        float a_min = min(hdr.a, min(min(c_n.a, c_s.a), min(c_w.a, c_e.a)));
        float a_max = max(hdr.a, max(max(c_n.a, c_s.a), max(c_w.a, c_e.a)));
        float contrast = max(l_max - l_min, a_max - a_min);
        if (contrast > max(0.035, l_max * 0.10)) {
            float dir_x = -((l_n + l_n) - (l_s + l_s)) + ((c_n.a - c_s.a) * 0.5);
            float dir_y = ((l_w + l_w) - (l_e + l_e)) + ((c_e.a - c_w.a) * 0.5);
            float dir_len = max(length(float2(dir_x, dir_y)), 1e-4);
            float2 dir = float2(dir_x, dir_y) / dir_len;
            float2 off = clamp(dir * (aa_mode >= 2u ? 0.75 : 0.5), float2(-1.0, -1.0), float2(1.0, 1.0));
            int2 p1 = clamp(int2(round(float2(p) + off)), int2(0, 0), max_p);
            int2 p2 = clamp(int2(round(float2(p) - off)), int2(0, 0), max_p);
            float4 edge_avg = 0.5 * (aether_load(g_Pc.param0, p1) + aether_load(g_Pc.param0, p2));
            float4 box_avg = 0.25 * (c_n + c_s + c_w + c_e);
            float aa_w = (aa_mode >= 2u) ? saturate(contrast * 1.35) * 0.65 : saturate(contrast * 1.15) * 0.50;
            float4 target = (aa_mode >= 2u) ? lerp(edge_avg, box_avg, 0.35) : edge_avg;
            hdr = lerp(hdr, target, aa_w);
        }
    }
    if (debug_view == 1u || debug_view == 13u) {
        float3 out_rgb = hdr.rgb + (1.0 - saturate(hdr.a)) * fg.bg_color_premul.rgb;
        float out_a = saturate(hdr.a + (1.0 - saturate(hdr.a)) * fg.bg_color_premul.a);
        aether_store(g_Pc.param1, tid.xy, float4(out_rgb, out_a));
        return;
    }

    uint crease_slot = fg.gbuffer_ids[1].y;
    float raw_crease = (crease_slot != 0xFFFFFFFFu) ? aether_load(crease_slot, p).x : 0.0;

    // Task 6A: Edge-Preserving Kuwahara / Bilateral Toon Flattening before ink application
    bool filter_pro = ((fg.state_u32.w & 4u) != 0u);
    if (filter_pro) {
        hdr.rgb = apply_2d_filter_pro_kuwahara(g_Pc.param0, p, max_p, hdr.rgb, hdr.a, raw_crease,
                                               fg.filter_pro_params.x);
    }

    // Apply sub-pixel AA Iro-Tsuke tinted interior crease ink lines over lit geometry
    float crease_str = saturate(fg.npr_style_params2.x);
    if (crease_slot != 0xFFFFFFFFu && hdr.a > 0.01 && crease_str > 0.001) {
        float aa_cr = raw_crease;
        if (filter_pro) {
            float c0 = aether_load(crease_slot, clamp(p + int2(1, 0), int2(0, 0), max_p)).x;
            float c1 = aether_load(crease_slot, clamp(p + int2(0, 1), int2(0, 0), max_p)).x;
            aa_cr = smoothstep(0.18, 0.78, lerp(raw_crease, 0.5 * (c0 + c1), 0.30));
        }
        float cr = aa_cr * crease_str;
        float3 base_norm = max(hdr.rgb / max(hdr.a, 0.05), float3(0.01, 0.01, 0.01));
        float3 iro_ink = clamp(pow(base_norm, float3(1.18, 1.24, 1.12)) * 0.28,
                               float3(0.03, 0.025, 0.045), float3(0.40, 0.35, 0.48));
        float3 ink_mul = lerp(float3(0.24, 0.22, 0.26), iro_ink / max(base_norm, float3(0.08, 0.08, 0.08)),
                              saturate(fg.npr_style_params.w));
        hdr.rgb *= lerp(float3(1.0, 1.0, 1.0), saturate(ink_mul), saturate(cr * 0.72));
    }

    float3 glow = float3(0.0, 0.0, 0.0);
    uint bloom_slot = fg.gbuffer_ids[1].z;
    if (bloom_slot != 0xFFFFFFFFu && fg.post_params.y > 0.0001) {
        glow = aether_sample_level(bloom_slot, kSamplerLinearClamp, uv, 0).rgb * fg.post_params.y;
    }

    float a0 = saturate(hdr.a);
    float3 premul_hdr = hdr.rgb + glow;
    float a_out = saturate(a0 + (1.0 - a0) * saturate(luminance(glow)));

    // Tonemap foreground unpremultiplied so AgX preserves Porter-Duff background invariance (Invariant 5)
    float3 mapped_premul = float3(0.0, 0.0, 0.0);
    if (a_out > 1e-4) {
        float3 unpre = premul_hdr / a_out;
        float3 mapped = (fg.post_params.z > 0.5) ? agx_tonemap_soft(unpre) : saturate(unpre);
        // Task 6B: Optional subtle 2D anime paper / cel warmth grading curve
        if (filter_pro && ((fg.state_u32.w & 8u) != 0u) && fg.filter_pro_params.z > 0.001) {
            float warm_w = saturate(fg.filter_pro_params.z);
            float3 cel_warm = mapped * float3(1.03, 1.005, 0.96) + float3(0.012, 0.008, 0.004) * (1.0 - mapped);
            mapped = saturate(lerp(mapped, cel_warm, warm_w));
        }
        mapped_premul = mapped * a_out;
    }

    float3 final_rgb = mapped_premul + (1.0 - a_out) * fg.bg_color_premul.rgb;
    float final_a = saturate(a_out + (1.0 - a_out) * fg.bg_color_premul.a);
    aether_store(g_Pc.param1, tid.xy, float4(final_rgb, final_a));
}

// Pass 12A: Encode OUT_RGBA16F (linear) -> OUT_BGRA8 (sRGB premul/straight + +-0.5 LSB IGN dither) (§10.10, §11).
// g_Pc.fparam0 > 0.5 => straight alpha (NDI); g_Pc.fparam1 > 0.5 => bottom-up scanlines (DirectShow RGB).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_encode_bgra(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    float4 lin = aether_load(g_Pc.param0, int2(tid.xy));
    float alpha = saturate(lin.a);
    float dither = (ign_blue_noise(tid.xy, fg.state_u32.x) - 0.5) * (fg.post_params.w / 255.0);

    float3 srgb = float3(0.0, 0.0, 0.0);
    if (alpha > 1e-5) {
        float3 lin_rgb = (g_Pc.fparam0 > 0.5) ? saturate(lin.rgb / alpha) : clamp(lin.rgb, 0.0, alpha);
        srgb = saturate(linear_to_srgb(lin_rgb) + dither);
    }

    uint2 dst_xy = (g_Pc.fparam1 > 0.5) ? uint2(tid.x, h - 1u - tid.y) : tid.xy;
    aether_store(g_Pc.param1, dst_xy, float4(srgb, alpha));
}

float3 nv12_composite_srgb(uint src_slot, int2 coord, float3 bg_linear) {
    float4 lin4 = aether_load(src_slot, coord);
    float a = saturate(lin4.a);
    float3 comp_lin = saturate(lin4.rgb + (1.0 - a) * bg_linear);
    return linear_to_srgb(comp_lin);
}

// Pass 12B: Encode OUT_RGBA16F -> OUT_NV12 (composite over non-alpha BG + BT.709 Y + 2x2 UV) (§10.10, §11.3).
// g_Pc.fparam0..2 => linear RGB background (e.g. Chroma Green #00B140 or Black).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_encode_nv12(uint3 tid : SV_DispatchThreadID) {
    uint w = max(g_Pc.param2, 1u);
    uint h = max(g_Pc.param3, 1u);
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    float3 bg_lin = float3(g_Pc.fparam0, g_Pc.fparam1, g_Pc.fparam2);
    float3 srgb = nv12_composite_srgb(g_Pc.param0, int2(tid.xy), bg_lin);
    float y = saturate(0.2126 * srgb.r + 0.7152 * srgb.g + 0.0722 * srgb.b);
    aether_store(g_Pc.param1, tid.xy, float4(y, 0.0, 0.0, 1.0));

    if (((tid.x & 1u) == 0u) && ((tid.y & 1u) == 0u) && fg.output_storage_ids.w != 0xFFFFFFFFu) {
        int2 max_p = int2(int(w) - 1, int(h) - 1);
        int2 p0 = int2(tid.xy);
        float3 c00 = nv12_composite_srgb(g_Pc.param0, p0, bg_lin);
        float3 c10 = nv12_composite_srgb(g_Pc.param0, min(p0 + int2(1, 0), max_p), bg_lin);
        float3 c01 = nv12_composite_srgb(g_Pc.param0, min(p0 + int2(0, 1), max_p), bg_lin);
        float3 c11 = nv12_composite_srgb(g_Pc.param0, min(p0 + int2(1, 1), max_p), bg_lin);
        float3 avg = (c00 + c10 + c01 + c11) * 0.25;
        float cb = saturate(-0.1146 * avg.r - 0.3854 * avg.g + 0.5000 * avg.b + 0.5);
        float cr = saturate(0.5000 * avg.r - 0.4542 * avg.g - 0.0458 * avg.b + 0.5);
        aether_store(fg.output_storage_ids.w, tid.xy >> 1u, float4(cb, cr, 0.0, 1.0));
    }
}
