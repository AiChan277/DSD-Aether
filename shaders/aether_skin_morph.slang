#include "aether_common.slang"

struct GpuSourceSurface {
    float2 uv0;
    float2 uv1;
    uint color_rgba8;
    uint tangent_oct;
    uint flags;
    uint reserved;
};

float4 quat_from_3x4_rows(float4 r0, float4 r1, float4 r2) {
    float trace = r0.x + r1.y + r2.z;
    float4 q = float4(0.0, 0.0, 0.0, 1.0);
    if (trace > 0.0) {
        float s = sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (r2.y - r1.z) / s;
        q.y = (r0.z - r2.x) / s;
        q.z = (r1.x - r0.y) / s;
    } else if (r0.x > r1.y && r0.x > r2.z) {
        float s = sqrt(1.0 + r0.x - r1.y - r2.z) * 2.0;
        q.w = (r2.y - r1.z) / s;
        q.x = 0.25 * s;
        q.y = (r0.y + r1.x) / s;
        q.z = (r0.z + r2.x) / s;
    } else if (r1.y > r2.z) {
        float s = sqrt(1.0 + r1.y - r0.x - r2.z) * 2.0;
        q.w = (r0.z - r2.x) / s;
        q.x = (r0.y + r1.x) / s;
        q.y = 0.25 * s;
        q.z = (r1.z + r2.y) / s;
    } else {
        float s = sqrt(1.0 + r2.z - r0.x - r1.y) * 2.0;
        q.w = (r1.x - r0.y) / s;
        q.x = (r0.z + r2.x) / s;
        q.y = (r1.z + r2.y) / s;
        q.z = 0.25 * s;
    }
    return normalize(q);
}

float4 quat_slerp_sdef(float4 a, float4 b, float t) {
    float d = dot(a, b);
    if (d < 0.0) {
        b = -b;
        d = -d;
    }
    if (d > 0.9995) {
        return normalize(lerp(a, b, t));
    }
    float theta = acos(clamp(d, -1.0, 1.0));
    float sin_t = sin(theta);
    if (abs(sin_t) < 1e-6) {
        return a;
    }
    float wa = sin((1.0 - t) * theta) / sin_t;
    float wb = sin(t * theta) / sin_t;
    return normalize(a * wa + b * wb);
}

float3 quat_rotate_sdef(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float3 mat3x4_point(GpuJointMat3x4 m, float3 p) {
    return float3(
        dot(m.row0.xyz, p) + m.row0.w,
        dot(m.row1.xyz, p) + m.row1.w,
        dot(m.row2.xyz, p) + m.row2.w
    );
}

// Pass 1: Vertex stream pack & temporal previous-position capture (`§10.2`, `P4-02`).
// PushConstants layout:
//   frame_globals_bda : GpuFrameGlobals*
//   draw_data_bda     : AetherVertex25D* (current posed stream)
//   vertex_buf_bda    : GpuSkinnedVertex* (destination 96 B buffer)
//   aux_buf_bda       : AetherVertexSurface* (32 B surface stream, optional)
//   param0            : vertex_count
//   param1            : bit 0 = preserve previous frame positions into prev_pos_sdf.xyz
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint v_idx = tid.x;
    if (v_idx >= g_Pc.param0) {
        return;
    }

    GpuSourceVertex src = AETHER_LOAD(GpuSourceVertex, g_Pc.draw_data_bda, v_idx);
    GpuFrameGlobals fg = AETHER_LOAD(GpuFrameGlobals, g_Pc.frame_globals_bda, 0);

    // Optional GPU Compute SDEF spherical skinning when bit 1 of g_Pc.param1 is set
    if ((g_Pc.param1 & 2u) != 0u && fg.pmx_sdef_bda != 0ul && fg.joint_mats_bda != 0ul && fg.skin_buf_bda != 0ul) {
        GpuPmxSdef sdef = AETHER_LOAD(GpuPmxSdef, fg.pmx_sdef_bda, v_idx);
        GpuVertexSkin sk = AETHER_LOAD(GpuVertexSkin, fg.skin_buf_bda, v_idx);
        uint j0 = sk.joints_packed.x & 0xFFFFu;
        uint j1 = (sk.joints_packed.x >> 16u) & 0xFFFFu;
        uint w0_u = sk.weights_packed.x & 0xFFFFu;
        uint w1_u = (sk.weights_packed.x >> 16u) & 0xFFFFu;
        if ((sdef.flags & 1u) != 0u && w1_u != 0u) {
            float w0 = saturate(float(w0_u) / 65535.0);
            float w1 = 1.0 - w0;
            GpuJointMat3x4 m0 = AETHER_LOAD(GpuJointMat3x4, fg.joint_mats_bda, j0);
            GpuJointMat3x4 m1 = AETHER_LOAD(GpuJointMat3x4, fg.joint_mats_bda, j1);
            float4 q0 = quat_from_3x4_rows(m0.row0, m0.row1, m0.row2);
            float4 q1 = quat_from_3x4_rows(m1.row0, m1.row1, m1.row2);
            float4 qb = quat_slerp_sdef(q0, q1, w1);
            float3 rw = w0 * sdef.r0 + w1 * sdef.r1;
            float3 c0 = sdef.c + 0.5 * (sdef.r0 - rw);
            float3 c1 = sdef.c + 0.5 * (sdef.r1 - rw);
            float3 mc0 = mat3x4_point(m0, c0);
            float3 mc1 = mat3x4_point(m1, c1);
            src.pos_out.xyz = w0 * mc0 + w1 * mc1 + quat_rotate_sdef(qb, src.pos_out.xyz - sdef.c);
            src.norm_zflat.xyz = quat_rotate_sdef(qb, src.norm_zflat.xyz);
            src.proxy_sdf.xyz = quat_rotate_sdef(qb, src.proxy_sdf.xyz);
        }
    }
    float2 yaw_cs = (dot(fg.avatar_rot_sincos.xy, fg.avatar_rot_sincos.xy) > 0.25)
                        ? fg.avatar_rot_sincos.xy
                        : float2(1.0, 0.0);
    float av_scale = (fg.avatar_pos_scale.w > 0.01) ? fg.avatar_pos_scale.w : 1.0;

    float3 pos_rot = float3(yaw_cs.x * src.pos_out.x + yaw_cs.y * src.pos_out.z,
                            src.pos_out.y,
                            -yaw_cs.y * src.pos_out.x + yaw_cs.x * src.pos_out.z);
    float3 world_pos = pos_rot * av_scale + fg.avatar_pos_scale.xyz;

    float3 nrm_rot = float3(yaw_cs.x * src.norm_zflat.x + yaw_cs.y * src.norm_zflat.z,
                            src.norm_zflat.y,
                            -yaw_cs.y * src.norm_zflat.x + yaw_cs.x * src.norm_zflat.z);
    float3 prx_rot = float3(yaw_cs.x * src.proxy_sdf.x + yaw_cs.y * src.proxy_sdf.z,
                            src.proxy_sdf.y,
                            -yaw_cs.y * src.proxy_sdf.x + yaw_cs.x * src.proxy_sdf.z);
    float3 tan_rot = float3(yaw_cs.x * src.smooth_tan.x + yaw_cs.y * src.smooth_tan.z,
                            src.smooth_tan.y,
                            -yaw_cs.y * src.smooth_tan.x + yaw_cs.x * src.smooth_tan.z);

    float3 prev_pos = world_pos;
    if ((g_Pc.param1 & 1u) != 0u) {
        GpuSkinnedVertex prev_v = AETHER_LOAD(GpuSkinnedVertex, g_Pc.vertex_buf_bda, v_idx);
        prev_pos = prev_v.pos_zflat.xyz;
    }

    float2 uv0 = float2(0.0, 0.0);
    uint surf_flags = 0u;
    if (g_Pc.aux_buf_bda != 0ul) {
        GpuSourceSurface surf = AETHER_LOAD(GpuSourceSurface, g_Pc.aux_buf_bda, v_idx);
        uv0 = surf.uv0;
        surf_flags = surf.flags;
    }
    if (fg.uv_delta_bda != 0ul) {
        float2 d_uv = AETHER_LOAD(float2, fg.uv_delta_bda, v_idx);
        uv0 += d_uv;
    }

    float3 n_true_ws = normalize_safe(nrm_rot, float3(0.0, 0.0, 1.0));
    float3 n_proxy_ws = normalize_safe(prx_rot, n_true_ws);
    // For non-face vertices, proxy_normal holds Rule-B safely welded N_healed; compute per-vertex crease weight.
    float crease_w = ((surf_flags & 1u) == 0u) ? saturate((1.0 - dot(n_true_ws, n_proxy_ws)) * 4.0) : 0.0;

    GpuSkinnedVertex dst = {};
    dst.pos_zflat = float4(world_pos, clamp(src.norm_zflat.w, 0.0, 0.85));
    dst.prev_pos_sdf = float4(prev_pos, src.proxy_sdf.w);
    dst.true_norm_hair = float4(n_true_ws, 0.0);
    dst.proxy_norm_out = float4(n_proxy_ws, src.pos_out.w);
    dst.tangent_crease = float4(normalize_safe(tan_rot, float3(0.0, 0.0, 1.0)), crease_w);
    dst.uv = uv0;
    dst.material_id = (surf_flags >> 8u) & 0xFFu;
    dst.flags = surf_flags;

    AETHER_STORE(GpuSkinnedVertex, g_Pc.vertex_buf_bda, v_idx, dst);
}
