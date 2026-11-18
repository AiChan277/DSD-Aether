#include "aether_common.slang"

struct FsOut {
    float4 clip_pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Pass 13A: Studio viewport composition (`8 px` #15171C/#1A1D23 checkerboard + OUT_RGBA16F -> viewport).
[shader("vertex")]
FsOut vs_fullscreen(uint vid : SV_VertexID) {
    FsOut o = {};
#ifdef __TARGET_SPIRV__
    float2 quad[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    o.clip_pos = float4(quad[vid % 3u], 0.0, 1.0);
    o.uv = o.clip_pos.xy * 0.5 + 0.5;
#else
    // DX12 positive viewport maps NDC +Y -> screen top (uv.y = 0)
    float2 quad[3] = {float2(-1.0, 1.0), float2(3.0, 1.0), float2(-1.0, -3.0)};
    o.clip_pos = float4(quad[vid % 3u], 0.0, 1.0);
    o.uv = float2(o.clip_pos.x * 0.5 + 0.5, 0.5 - o.clip_pos.y * 0.5);
#endif
    return o;
}

[shader("fragment")]
float4 ps_compose(FsOut i) : SV_Target0 {
    float x0 = g_Pc.fparam0;
    float y0 = g_Pc.fparam1;
    float x1 = max(x0 + 1e-4, g_Pc.fparam2);
    float y1 = max(y0 + 1e-4, g_Pc.fparam3);
    if (i.uv.x < x0 || i.uv.x > x1 || i.uv.y < y0 || i.uv.y > y1) {
        if ((g_Pc.param1 & 2u) != 0u) {
            return float4(11.0 / 255.0, 13.0 / 255.0, 18.0 / 255.0, 1.0); // #0B0D12 broadcast matte
        }
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float du = asfloat(g_Pc.param2);
    float dv = asfloat(g_Pc.param3);
    if (du <= 0.0) du = 1.0;
    if (dv <= 0.0) dv = 1.0;
    float2 frame_uv = float2((i.uv.x - x0) / (x1 - x0), (i.uv.y - y0) / (y1 - y0));
    float2 src_uv = float2(0.5, 0.5) + (frame_uv - float2(0.5, 0.5)) * float2(du, dv);
    float4 fg = aether_sample(g_Pc.param0, kSamplerLinearClamp, src_uv);
    float a = saturate(fg.a);
    float3 bg_lin = float3(0.0, 0.0, 0.0);
    if ((g_Pc.param1 & 1u) != 0u) {
        uint2 cell = uint2(i.clip_pos.xy) >> 3u;
        bool alt = ((cell.x + cell.y) & 1u) != 0u;
        // Precision Dark viewport checkerboard (#15171C and #1A1D23 converted to linear)
        float3 col_a = srgb_to_linear_rgb(float3(21.0, 23.0, 28.0) / 255.0);
        float3 col_b = srgb_to_linear_rgb(float3(26.0, 29.0, 35.0) / 255.0);
        bg_lin = alt ? col_b : col_a;
    }
    float3 comp_lin = fg.rgb + (1.0 - a) * bg_lin;
    return float4(linear_to_srgb(comp_lin), 1.0);
}

struct HeatmapVsOut {
    float4 clip_pos : SV_Position;
    float4 color : TEXCOORD0;
    float2 edge_uv : TEXCOORD1;
    nointerpolation uint category : TEXCOORD2;
};

// Pass 13B: CVD-safe (Alt+C) Okabe-Ito instanced skeleton heatmap overlay (`#199E70` / `#C98500` / `#3987E5`).
[shader("vertex")]
HeatmapVsOut vs_heatmap(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    HeatmapVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuHeatmapBone b = AETHER_LOAD(GpuHeatmapBone, g_Pc.draw_data_bda, iid);

    static const float2 kTriCorners[6] = {
        float2(0.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
        float2(0.0, -1.0), float2(1.0, 1.0),  float2(0.0, 1.0),
    };
    float2 c = kTriCorners[vid % 6u];

    float2 p0 = b.p0_ndc_weight.xy;
    float2 p1 = b.p1_ndc_pad.xy;
    float2 delta = p1 - p0;
    float len = max(length(delta), 1e-4);
    float2 dir = delta / len;
    float2 perp = float2(-dir.y, dir.x);
    uint cat = uint(b.p1_ndc_pad.z);
    float min_w = (cat >= 3u) ? 1.5 : 3.0;
    float width_px = max(b.p0_ndc_weight.w, min_w);
    float2 ndc_offset = perp * (c.y * width_px * fg.viewport_size.zw);

    o.clip_pos = float4(lerp(p0, p1, c.x) + ndc_offset, 0.0, 1.0);
    if (cat >= 6u) {
        o.color = float4(251.0 / 255.0, 146.0 / 255.0, 60.0 / 255.0, 1.0); // Spring chain active contact (#FB923C)
    } else if (cat == 5u) {
        o.color = float4(56.0 / 255.0, 189.0 / 255.0, 248.0 / 255.0, 0.92); // Spring chain rest (#38BDF8)
    } else if (cat == 4u) {
        o.color = float4(249.0 / 255.0, 115.0 / 255.0, 22.0 / 255.0, 1.0); // Collider contact Orange/Red (#F97316)
    } else if (cat == 3u) {
        o.color = float4(6.0 / 255.0, 182.0 / 255.0, 212.0 / 255.0, 1.0); // Collider rest Cyan (#06B6D4)
    } else if (cat == 2u) {
        o.color = float4(57.0 / 255.0, 135.0 / 255.0, 229.0 / 255.0, 1.0); // #3987E5
    } else if (cat == 1u) {
        o.color = float4(201.0 / 255.0, 133.0 / 255.0, 0.0 / 255.0, 1.0); // #C98500
    } else {
        o.color = float4(25.0 / 255.0, 158.0 / 255.0, 112.0 / 255.0, 1.0); // #199E70
    }
    o.edge_uv = c;
    o.category = cat;
    return o;
}

[shader("fragment")]
float4 ps_heatmap(HeatmapVsOut i) : SV_Target0 {
    if (i.category == 1u) {
        if (frac(i.edge_uv.x * 6.0) > 0.62) {
            discard;
        }
    } else if (i.category == 2u) {
        if (frac(i.edge_uv.x * 12.0) > 0.42) {
            discard;
        }
    }
    return i.color;
}
