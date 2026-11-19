#include "aether_common.slang"

struct PrepassVsOut {
    float4 clip_pos : SV_Position;
    float3 view_normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    nointerpolation uint packed_mat : TEXCOORD2;
    nointerpolation uint material_id : TEXCOORD3;
    float4 misc : TEXCOORD4;
    float4 cur_clip : TEXCOORD5;
    float4 prev_clip : TEXCOORD6;
    float3 world_pos : TEXCOORD7;
    float3 world_true_normal : TEXCOORD8;
};

struct PrepassPsOut {
    float2 gb_n : SV_Target0;
    float2 gb_mv : SV_Target1;
    uint gb_mat : SV_Target2;
    float4 gb_misc : SV_Target3;
};

// Pass 3: Depth + Stencil + G-Buffer prepass with camera-facing Z-flattening (§10.4) & Dual-Normal output.
// gb_n     = oct(N_proxy in view space)
// gb_misc  = (k_eff, sdf_mask, oct(N_geom in view space))
// gb_mat   = (mat_id & 0x3FF) | (mat_cat << 10) | (rt_class << 13) | (is_hair ? 0x8000 : 0)
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
    uint mat_flags = mat.extra_slots.z;
    uint mat_id = g_Pc.param0 & 0x3FFu;
    uint rt_class = (mat_flags >> 10u) & 3u;
    bool is_hair = ((g_Pc.param1 & 6u) != 0u) || ((mat_flags & 4u) != 0u) || (rt_class == 1u);
    bool is_face_mat = ((mat_flags & 2u) != 0u) || ((g_Pc.param1 & 1u) != 0u) || (v.prev_pos_sdf.w > 0.5) || (rt_class == 0u);
    bool is_body_skin_atlas = ((mat_flags & (1u << 13u)) != 0u) && !is_face_mat;

    float3 true_n_ws = normalize_safe(v.true_norm_hair.xyz, float3(0.0, 0.0, 1.0));
    float3 proxy_n_ws = normalize_safe(v.proxy_norm_out.xyz, true_n_ws);
    float crease_eff = saturate(fg.eye_catchlight_params.z);
    float3 shade_n_ws = is_face_mat
                            ? proxy_n_ws
                            : normalize_safe(lerp(true_n_ws, proxy_n_ws, crease_eff), true_n_ws);
    if (is_face_mat) {
        float3 unified_n = compute_unified_skin_normal(v.pos_zflat.xyz, true_n_ws, fg);
        float chin_y = (fg.chin_pos_ws.w > 0.5) ? fg.chin_pos_ws.y : (fg.neck_seam_head_str.y - 0.045);
        float jaw_w = 1.0 - smoothstep(chin_y - 0.01, chin_y + 0.045, v.pos_zflat.y);
        shade_n_ws = normalize_safe(lerp(shade_n_ws, unified_n, max(0.35, jaw_w)), shade_n_ws);
    } else if (is_body_skin_atlas) {
        float neck_w = compute_neck_seam_blend(v.pos_zflat.xyz, fg);
        float3 unified_n = compute_unified_skin_normal(v.pos_zflat.xyz, true_n_ws, fg);
        shade_n_ws = normalize_safe(lerp(shade_n_ws, unified_n, max(neck_w, 0.25)), shade_n_ws);
    }

    o.view_normal = normalize_safe(mul_col3(fg.view, shade_n_ws), float3(0.0, 0.0, 1.0));
    o.world_pos = v.pos_zflat.xyz;
    o.world_true_normal = true_n_ws;
    o.uv = v.uv;

    uint crease_q8 = is_face_mat ? 0u : (uint(round(saturate(v.tangent_crease.w) * 255.0)) & 0xFFu);
    bool suppress_edge = (v.proxy_norm_out.w <= 0.05);
    o.material_id = g_Pc.param0;
    o.packed_mat = mat_id | (rt_class << 10u) | (is_face_mat ? 0x1000u : 0u) | (is_body_skin_atlas ? 0x2000u : 0u) |
                   (suppress_edge ? 0x4000u : 0u) | (is_hair ? 0x8000u : 0u) | (crease_q8 << 16u);

    float k_eff = saturate(z_flat_weight * fg.head_pivot_view_and_flatness.w);
    float sdf_mask = max(v.prev_pos_sdf.w, ((mat_flags & 2u) != 0u) ? 1.0 : 0.0);
    o.misc = float4(k_eff, sdf_mask, 0.0, 0.0);

    o.cur_clip = mul_col(fg.view_proj, float4(v.pos_zflat.xyz, 1.0));
    o.prev_clip = mul_col(fg.prev_view_proj, float4(v.prev_pos_sdf.xyz, 1.0));
    return o;
}

[shader("fragment")]
PrepassPsOut ps_main(PrepassVsOut i, bool is_front_face : SV_IsFrontFace) {
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);
    if (g_Pc.aux_buf_bda != 0ul) {
        GpuMaterial mat = AETHER_LOAD(GpuMaterial, g_Pc.aux_buf_bda, i.material_id);
        if ((g_Pc.param1 & 32u) != 0u && mat.toon_params.w > 0.001) {
            float tex_a = aether_sample(mat.tex_slots.x, kSamplerLinearClamp, i.uv).a;
            float alpha = tex_a * mat.lit_color.a;
            float aa_alpha = saturate((alpha - mat.toon_params.w) / max(fwidth(alpha), 1e-4) + 0.5);
            if (aa_alpha < 0.05) {
                discard;
            }
        }
    }

    // Exact per-triangle geometric normal N_geom oriented safely via SV_IsFrontFace & vertex true normal
    float3 world_ref_n = normalize_safe(i.world_true_normal, float3(0.0, 0.0, 1.0));
    if (!is_front_face) {
        world_ref_n = -world_ref_n;
    }
    float3 tri_cross = cross(ddx(i.world_pos), ddy(i.world_pos));
    float3 world_geom_n = world_ref_n;
    if (dot(tri_cross, tri_cross) > 1e-12) {
        float3 tri_n = normalize(tri_cross);
        if (dot(tri_n, world_ref_n) < 0.0) {
            tri_n = -tri_n;
        }
        world_geom_n = tri_n;
    }
    float3 view_geom_n = normalize_safe(mul_col3(fg.view, world_geom_n), i.view_normal);

    float2 cur_uv = ndc_to_screen_uv(i.cur_clip.xy / max(abs(i.cur_clip.w), 1e-5));
    float2 prev_uv = ndc_to_screen_uv(i.prev_clip.xy / max(abs(i.prev_clip.w), 1e-5));

    PrepassPsOut o = {};
    float3 view_n = normalize_safe(i.view_normal, float3(0.0, 0.0, 1.0));
    if (!is_front_face) {
        view_n = -view_n;
    }
    o.gb_n = oct_encode(view_n);
    o.gb_mv = cur_uv - prev_uv;
    o.gb_mat = i.packed_mat;
    o.gb_misc = float4(i.misc.x, i.misc.y, oct_encode(view_geom_n));
    return o;
}
