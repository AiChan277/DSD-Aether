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

    float4 s = g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv, 0) * 4.0;
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(-hp.x, -hp.y), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(hp.x, -hp.y), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(-hp.x, hp.y), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(hp.x, hp.y), 0);
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

    g_StorageImages[g_Pc.param1][tid.xy] = max(s, float4(0.0, 0.0, 0.0, 0.0));
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
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(-hp.x * 2.0, 0.0), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(hp.x * 2.0, 0.0), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(0.0, -hp.y * 2.0), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(0.0, hp.y * 2.0), 0);
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(-hp.x, -hp.y), 0) * 2.0;
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(hp.x, -hp.y), 0) * 2.0;
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(-hp.x, hp.y), 0) * 2.0;
    s += g_SampledImages[g_Pc.param0].SampleLevel(g_Samplers[kSamplerLinearClamp], uv + float2(hp.x, hp.y), 0) * 2.0;
    s *= (1.0 / 12.0);

    g_StorageImages[g_Pc.param1][tid.xy] = max(s, float4(0.0, 0.0, 0.0, 0.0));
}

float3 visualize_debug_mode(GpuFrameGlobals fg, uint mode, int2 p, float2 uv) {
    if (mode == 2u) {
        float3 n = oct_decode(g_SampledImages[fg.gbuffer_ids[0].x].Load(int3(p, 0)).xy);
        return n * 0.5 + 0.5;
    }
    if (mode == 3u) {
        float d = g_SampledImages[fg.gbuffer_extra_ids[0].x].Load(int3(p, 0)).x;
        return float3(d, 1.0 - d, 0.35);
    }
    if (mode == 4u) {
        float zf = g_SampledImages[fg.gbuffer_ids[0].w].Load(int3(p, 0)).x;
        return float3(zf, zf * 0.65, 1.0 - zf);
    }
    if (mode == 5u) {
        float m = g_SampledImages[fg.gbuffer_ids[0].w].Load(int3(p, 0)).y;
        return float3(m, m * 0.85, 0.25);
    }
    if (mode == 6u) {
        uint st = g_UintSampledImages[fg.gbuffer_extra_ids[0].y].Load(int3(p, 0)).x;
        return float3(((st & 2u) != 0u) ? 0.95 : 0.08, ((st & 1u) != 0u) ? 0.65 : 0.08, 0.25);
    }
    if (mode == 7u) {
        return g_SampledImages[fg.gbuffer_ids[1].x].SampleLevel(g_Samplers[kSamplerLinearClamp], uv, 0).xxx;
    }
    if (mode == 8u) {
        return g_SampledImages[fg.gbuffer_extra_ids[0].z].Load(int3(p, 0)).xxx;
    }
    if (mode == 9u) {
        return g_SampledImages[fg.gbuffer_extra_ids[0].w].Load(int3(p, 0)).xxx;
    }
    if (mode == 10u) {
        return g_SampledImages[fg.gbuffer_extra_ids[1].x].SampleLevel(g_Samplers[kSamplerLinearClamp], uv, 0).rgb;
    }
    if (mode == 11u) {
        float cr = g_SampledImages[fg.gbuffer_ids[1].y].Load(int3(p, 0)).x;
        return float3(cr, cr * 0.4, 0.1);
    }
    if (mode == 12u) {
        float2 mv = g_SampledImages[fg.gbuffer_ids[0].y].Load(int3(p, 0)).xy;
        return float3(abs(mv.x) * 40.0, abs(mv.y) * 40.0, 0.25);
    }
    return float3(0.0, 0.0, 0.0);
}

// Pass 11: Bloom + Crease + Tone Map + Transparent Background Composition -> OUT_RGBA16F (§10.9, §10.10).
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
    float2 uv = (float2(tid.xy) + 0.5) / float2(float(w), float(h));
    uint debug_view = fg.state_u32.z;

    if (debug_view >= 2u && debug_view <= 12u) {
        float3 dbg = visualize_debug_mode(fg, debug_view, p, uv);
        g_StorageImages[g_Pc.param1][tid.xy] = float4(dbg, 1.0);
        return;
    }

    float4 hdr = g_SampledImages[g_Pc.param0].Load(int3(p, 0));
    if (debug_view == 1u || debug_view == 13u) {
        float3 out_rgb = hdr.rgb + (1.0 - saturate(hdr.a)) * fg.bg_color_premul.rgb;
        float out_a = saturate(hdr.a + (1.0 - saturate(hdr.a)) * fg.bg_color_premul.a);
        g_StorageImages[g_Pc.param1][tid.xy] = float4(out_rgb, out_a);
        return;
    }

    // Apply interior crease lines gently over lit geometry (masked to foreground alpha)
    uint crease_slot = fg.gbuffer_ids[1].y;
    if (crease_slot != 0xFFFFFFFFu && hdr.a > 0.01) {
        float cr = g_SampledImages[crease_slot].Load(int3(p, 0)).x;
        hdr.rgb *= saturate(1.0 - 0.65 * cr);
    }

    float3 glow = float3(0.0, 0.0, 0.0);
    uint bloom_slot = fg.gbuffer_ids[1].z;
    if (bloom_slot != 0xFFFFFFFFu && fg.post_params.y > 0.0001) {
        glow = g_SampledImages[bloom_slot].SampleLevel(g_Samplers[kSamplerLinearClamp], uv, 0).rgb * fg.post_params.y;
    }

    float a0 = saturate(hdr.a);
    float3 premul_hdr = hdr.rgb + glow;
    float a_out = saturate(a0 + (1.0 - a0) * saturate(luminance(glow)));

    // Tonemap foreground unpremultiplied so AgX preserves Porter-Duff background invariance (Invariant 5)
    float3 mapped_premul = float3(0.0, 0.0, 0.0);
    if (a_out > 1e-4) {
        float3 unpre = premul_hdr / a_out;
        float3 mapped = (fg.post_params.z > 0.5) ? agx_tonemap_soft(unpre) : saturate(unpre);
        mapped_premul = mapped * a_out;
    }

    float3 final_rgb = mapped_premul + (1.0 - a_out) * fg.bg_color_premul.rgb;
    float final_a = saturate(a_out + (1.0 - a_out) * fg.bg_color_premul.a);
    g_StorageImages[g_Pc.param1][tid.xy] = float4(final_rgb, final_a);
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
    float4 lin = g_SampledImages[g_Pc.param0].Load(int3(int2(tid.xy), 0));
    float alpha = saturate(lin.a);
    float dither = (ign_blue_noise(tid.xy, fg.state_u32.x) - 0.5) * (fg.post_params.w / 255.0);

    float3 srgb = float3(0.0, 0.0, 0.0);
    if (alpha > 1e-5) {
        float3 lin_rgb = (g_Pc.fparam0 > 0.5) ? saturate(lin.rgb / alpha) : clamp(lin.rgb, 0.0, alpha);
        srgb = saturate(linear_to_srgb(lin_rgb) + dither);
    }

    uint2 dst_xy = (g_Pc.fparam1 > 0.5) ? uint2(tid.x, h - 1u - tid.y) : tid.xy;
    g_StorageImages[g_Pc.param1][dst_xy] = float4(srgb, alpha);
}

float3 nv12_composite_srgb(uint src_slot, int2 coord, float3 bg_linear) {
    float4 lin4 = g_SampledImages[src_slot].Load(int3(coord, 0));
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
    g_StorageImages[g_Pc.param1][tid.xy] = float4(y, 0.0, 0.0, 1.0);

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
        g_StorageImages[fg.output_storage_ids.w][tid.xy >> 1u] = float4(cb, cr, 0.0, 1.0);
    }
}
