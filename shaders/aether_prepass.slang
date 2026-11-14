#include "aether_common.slang"

struct PrepassVsOut {
    float4 clip_pos : SV_Position;
    float3 view_normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    nointerpolation uint material_id : TEXCOORD2;
    float4 misc : TEXCOORD3;
    float4 cur_clip : TEXCOORD4;
    float4 prev_clip : TEXCOORD5;
};

struct PrepassPsOut {
    float2 gb_n : SV_Target0;
    float2 gb_mv : SV_Target1;
    uint gb_mat : SV_Target2;
    float4 gb_misc : SV_Target3;
};

// Pass 3: Depth + Stencil + G-Buffer prepass with camera-facing Z-flattening (§10.4).
// Stencil bit 0x01 marks all valid avatar pixels (crease pass filter);
// Stencil bit 0x02 marks hair-front casters (`AETHER_MAT_FLAG_HAIR_FRONT_STENCIL`).
[shader("vertex")]
PrepassVsOut vs_main(uint vid : SV_VertexID) {
    PrepassVsOut o = {};
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    GpuSkinnedVertex v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, vid);
    GpuMaterial mat = {};
    if (g_Pc.aux_buf_bda != 0ul) {
        mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, g_Pc.param0);
    } else {
        mat.bounce_tint_zflat = float4(1.0, 1.0, 1.0, 1.0);
    }

    float z_flat_weight = saturate(v.pos_zflat.w * mat.bounce_tint_zflat.w);
    float3 view_pos = mul_col(fg.view, float4(v.pos_zflat.xyz, 1.0)).xyz;
    o.clip_pos = project_live2d_flat(fg.proj, view_pos, fg.head_pivot_view_and_flatness.z,
                                     z_flat_weight, fg.head_pivot_view_and_flatness.w);
    o.view_normal = normalize_safe(mul_col3(fg.view, v.true_norm_hair.xyz), float3(0.0, 0.0, 1.0));
    o.uv = v.uv;
    o.material_id = g_Pc.param0;

    float k_eff = saturate(z_flat_weight * fg.head_pivot_view_and_flatness.w);
    uint mat_flags = mat.extra_slots.z;
    float sdf_mask = max(v.prev_pos_sdf.w, ((mat_flags & 2u) != 0u) ? 1.0 : 0.0);
    float hair_recv = ((mat_flags & 8u) != 0u) ? 1.0 : 0.0;
    float hair_cast = ((mat_flags & 4u) != 0u || (g_Pc.param1 & 2u) != 0u) ? 1.0 : 0.0;
    o.misc = float4(k_eff, sdf_mask, hair_recv, hair_cast);

    o.cur_clip = mul_col(fg.view_proj, float4(v.pos_zflat.xyz, 1.0));
    o.prev_clip = mul_col(fg.prev_view_proj, float4(v.prev_pos_sdf.xyz, 1.0));
    return o;
}

[shader("fragment")]
PrepassPsOut ps_main(PrepassVsOut i) {
    if (g_Pc.aux_buf_bda != 0ul) {
        GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, i.material_id);
        if ((g_Pc.param1 & 32u) != 0u && mat.toon_params.w > 0.001) {
            float tex_a = g_SampledImages[mat.tex_slots.x].Sample(g_Samplers[kSamplerLinearClamp], i.uv).a;
            float alpha = tex_a * mat.lit_color.a;
            if (alpha < mat.toon_params.w) {
                discard;
            }
        }
    }

    float2 cur_uv = (i.cur_clip.xy / max(abs(i.cur_clip.w), 1e-5)) * 0.5 + 0.5;
    float2 prev_uv = (i.prev_clip.xy / max(abs(i.prev_clip.w), 1e-5)) * 0.5 + 0.5;

    PrepassPsOut o = {};
    o.gb_n = oct_encode(normalize_safe(i.view_normal, float3(0.0, 0.0, 1.0)));
    o.gb_mv = cur_uv - prev_uv;
    o.gb_mat = i.material_id;
    o.gb_misc = i.misc;
    return o;
}
