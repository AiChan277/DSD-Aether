#include "aether_common.slang"

struct HullVsOut {
    float4 clip_pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint material_id : TEXCOORD1;
    nointerpolation float valid : TEXCOORD2;
};

// Pass 7: Distance-compensated inverted-hull outline (`w^0.65`) along seam-welded `smooth_tangent` (§10.7).
[shader("vertex")]
HullVsOut vs_main(uint vid : SV_VertexID) {
    HullVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuSkinnedVertex v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, vid);
    GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, g_Pc.param0);

    float z_flat_weight = saturate(v.pos_zflat.w * mat.bounce_tint_zflat.w);
    float3 view_pos = mul_col(fg.view, float4(v.pos_zflat.xyz, 1.0)).xyz;
    float4 clip = project_live2d_flat(fg.proj, view_pos, fg.head_pivot_view_and_flatness.z,
                                      z_flat_weight, fg.head_pivot_view_and_flatness.w);

    float raw_width = mat.outline_color_width.w * g_Pc.fparam0 * v.proxy_norm_out.w;
    bool screen_mode = (mat.extra_slots.z & 16u) != 0u;
    if (raw_width <= 0.0001) {
        o.clip_pos = float4(0.0, 0.0, 2.0, 1.0);
        o.valid = 0.0;
        return o;
    }

    float3 tan_w = normalize_safe(v.tangent_crease.xyz, float3(0.0, 0.0, 1.0));
    float3 view_tan = normalize_safe(mul_col3(fg.view, tan_w), float3(0.0, 0.0, 1.0));
    float w_pos = max(clip.w, 1e-4);

    if (screen_mode) {
        // Screen-space thickness with w^0.65 / w = w^-0.35 attenuation across 0.5-3.0 m
        float2 clip_xy = mul_col3(fg.proj, view_tan).xy;
        float len2 = dot(clip_xy, clip_xy);
        float2 clip_n = (len2 > 1e-10) ? (clip_xy * rsqrt(len2)) : float2(0.0, 0.0);
        float2 ndc_step = raw_width * (2.0 * fg.viewport_size.zw) * pow(w_pos, 0.65);
        clip.xy += clip_n * ndc_step;
    } else {
        // World-space metre thickness compressed with w^0.65 perspective scaling
        float3 world_extruded = v.pos_zflat.xyz + tan_w * (raw_width * pow(w_pos, 0.65));
        float3 vp_ext = mul_col(fg.view, float4(world_extruded, 1.0)).xyz;
        clip = project_live2d_flat(fg.proj, vp_ext, fg.head_pivot_view_and_flatness.z,
                                   z_flat_weight, fg.head_pivot_view_and_flatness.w);
    }

    o.clip_pos = clip;
    o.uv = v.uv;
    o.material_id = g_Pc.param0;
    o.valid = 1.0;
    return o;
}

[shader("fragment")]
float4 ps_main(HullVsOut i) : SV_Target0 {
    if (i.valid < 0.5) {
        discard;
    }
    GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, i.material_id);
    float alpha = saturate(g_SampledImages[mat.tex_slots.x].Sample(g_Samplers[kSamplerLinearClamp], i.uv).a * mat.lit_color.a);
    if ((g_Pc.param1 & 32u) != 0u && alpha < max(mat.toon_params.w, 0.001)) {
        discard;
    }
    return float4(mat.outline_color_width.rgb * alpha, alpha);
}
