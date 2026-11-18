#pragma once

#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/gpu_structs.hpp"
#include "renderer/lighting_rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace aether::renderer::detail {

inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kDegToRad = kPi / 180.0f;

struct Mat4Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

[[nodiscard]] inline Mat4Vec3 normalize3(Mat4Vec3 v) noexcept {
    const float l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 > 1e-12f) {
        const float inv = 1.0f / std::sqrt(l2);
        return {v.x * inv, v.y * inv, v.z * inv};
    }
    return {0.0f, 0.0f, 1.0f};
}

[[nodiscard]] inline Mat4Vec3 cross3(Mat4Vec3 a, Mat4Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline float dot3(Mat4Vec3 a, Mat4Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline GpuMat4 mul_mat4(const GpuMat4& a, const GpuMat4& b) noexcept {
    auto mul_col = [&](const float c[4], float out[4]) {
        for (int r = 0; r < 4; ++r) {
            out[r] = a.col0[r] * c[0] + a.col1[r] * c[1] + a.col2[r] * c[2] + a.col3[r] * c[3];
        }
    };
    GpuMat4 out{};
    mul_col(b.col0, out.col0);
    mul_col(b.col1, out.col1);
    mul_col(b.col2, out.col2);
    mul_col(b.col3, out.col3);
    return out;
}

inline void build_camera_matrices(const PreviewCamera& cam, float aspect, GpuMat4& out_view, GpuMat4& out_inv_view,
                                  GpuMat4& out_proj, GpuMat4& out_inv_proj, GpuMat4& out_vp,
                                  Mat4Vec3& out_eye) noexcept {
    const float cp = std::cos(cam.pitch_rad);
    const float sp = std::sin(cam.pitch_rad);
    const float cy = std::cos(cam.yaw_rad);
    const float sy = std::sin(cam.yaw_rad);

    out_eye = {
        cam.target_x + cam.distance * cp * sy,
        cam.target_y + cam.distance * sp,
        cam.target_z + cam.distance * cp * cy,
    };
    const Mat4Vec3 target{cam.target_x, cam.target_y, cam.target_z};
    const Mat4Vec3 f = normalize3({target.x - out_eye.x, target.y - out_eye.y, target.z - out_eye.z});
    const Mat4Vec3 world_up{0.0f, 1.0f, 0.0f};
    const Mat4Vec3 s = normalize3(cross3(f, world_up));
    const Mat4Vec3 u = cross3(s, f);

    out_view.col0[0] = s.x;
    out_view.col0[1] = u.x;
    out_view.col0[2] = -f.x;
    out_view.col0[3] = 0.0f;
    out_view.col1[0] = s.y;
    out_view.col1[1] = u.y;
    out_view.col1[2] = -f.y;
    out_view.col1[3] = 0.0f;
    out_view.col2[0] = s.z;
    out_view.col2[1] = u.z;
    out_view.col2[2] = -f.z;
    out_view.col2[3] = 0.0f;
    out_view.col3[0] = -dot3(s, out_eye);
    out_view.col3[1] = -dot3(u, out_eye);
    out_view.col3[2] = dot3(f, out_eye);
    out_view.col3[3] = 1.0f;

    out_inv_view.col0[0] = s.x;
    out_inv_view.col0[1] = s.y;
    out_inv_view.col0[2] = s.z;
    out_inv_view.col0[3] = 0.0f;
    out_inv_view.col1[0] = u.x;
    out_inv_view.col1[1] = u.y;
    out_inv_view.col1[2] = u.z;
    out_inv_view.col1[3] = 0.0f;
    out_inv_view.col2[0] = -f.x;
    out_inv_view.col2[1] = -f.y;
    out_inv_view.col2[2] = -f.z;
    out_inv_view.col2[3] = 0.0f;
    out_inv_view.col3[0] = out_eye.x;
    out_inv_view.col3[1] = out_eye.y;
    out_inv_view.col3[2] = out_eye.z;
    out_inv_view.col3[3] = 1.0f;

    const float near_p = 0.05f;
    const float far_p = 60.0f;
    const float fovy = std::clamp(cam.fovy_rad, 0.1f, 2.5f);
    const float fy = 1.0f / std::tan(fovy * 0.5f);
    const float fx = fy / std::max(aspect, 0.1f);
    const float a = -far_p / (far_p - near_p);
    const float b = -(far_p * near_p) / (far_p - near_p);

    out_proj = {};
    out_proj.col0[0] = fx;
    out_proj.col1[1] = fy;
    out_proj.col2[2] = a;
    out_proj.col2[3] = -1.0f;
    out_proj.col3[2] = b;
    out_proj.col3[3] = 0.0f;

    out_inv_proj = {};
    out_inv_proj.col0[0] = 1.0f / fx;
    out_inv_proj.col1[1] = 1.0f / fy;
    out_inv_proj.col2[3] = 1.0f / b;
    out_inv_proj.col3[2] = -1.0f;
    out_inv_proj.col3[3] = -a / b;

    out_vp = mul_mat4(out_proj, out_view);
}

[[nodiscard]] inline GpuMat4 build_csm_ortho_matrix(const Mat4Vec3& target, const Mat4Vec3& light_dir_to_light) noexcept {
    const Mat4Vec3 fwd = normalize3({-light_dir_to_light.x, -light_dir_to_light.y, -light_dir_to_light.z});
    const Mat4Vec3 up_cand = (std::abs(fwd.y) < 0.99f) ? Mat4Vec3{0.0f, 1.0f, 0.0f} : Mat4Vec3{0.0f, 0.0f, 1.0f};
    const Mat4Vec3 right = normalize3(cross3(fwd, up_cand));
    const Mat4Vec3 up = cross3(right, fwd);
    const float dist = 3.0f;
    const Mat4Vec3 eye{target.x - fwd.x * dist, target.y - fwd.y * dist, target.z - fwd.z * dist};

    GpuMat4 v{};
    v.col0[0] = right.x;
    v.col0[1] = up.x;
    v.col0[2] = -fwd.x;
    v.col0[3] = 0.0f;
    v.col1[0] = right.y;
    v.col1[1] = up.y;
    v.col1[2] = -fwd.y;
    v.col1[3] = 0.0f;
    v.col2[0] = right.z;
    v.col2[1] = up.z;
    v.col2[2] = -fwd.z;
    v.col2[3] = 0.0f;
    v.col3[0] = -dot3(right, eye);
    v.col3[1] = -dot3(up, eye);
    v.col3[2] = dot3(fwd, eye);
    v.col3[3] = 1.0f;

    const float half_ext = 1.6f;
    const float zn = 0.1f;
    const float zf = 7.0f;
    GpuMat4 p{};
    p.col0[0] = 1.0f / half_ext;
    p.col1[1] = 1.0f / half_ext;
    p.col2[2] = -1.0f / (zf - zn);
    p.col3[2] = -zn / (zf - zn);
    p.col3[3] = 1.0f;
    return mul_mat4(p, v);
}

[[nodiscard]] inline Mat4Vec3 scene_light_dir_to_light(const SceneLight& l) noexcept {
    if (l.use_2d_orbit_angle) {
        const float az = l.orbit_azimuth_deg * kDegToRad;
        const float el = l.orbit_elevation_deg * kDegToRad;
        const float ce = std::cos(el);
        return normalize3({ce * std::sin(az), std::sin(el), ce * std::cos(az)});
    }
    return normalize3({l.direction_or_pos[0], l.direction_or_pos[1], l.direction_or_pos[2]});
}

/**
 * Shared Vulkan / DirectX 12 `GpuFrameGlobals` assembly guaranteeing identical projection,
 * lighting rig, Live2D flattening pivot, and Tasks 3-6 shader parameters (`anti-flip contract`).
 */
[[nodiscard]] inline uint8_t populate_shared_frame_globals(const FrameRenderRequest& req, uint32_t w, uint32_t h,
                                                           bool has_avatar, bool rt_hardware_ready,
                                                           bool have_prev_view_proj, const GpuMat4& prev_view_proj,
                                                           GpuFrameGlobals& fg, GpuMat4& out_next_prev_vp) noexcept {
    const LightingRig& rig = *req.rig;
    fg = {};
    Mat4Vec3 eye{};
    build_camera_matrices(req.camera, static_cast<float>(w) / static_cast<float>(h), fg.view, fg.inv_view, fg.proj,
                          fg.inv_proj, fg.view_proj, eye);
    fg.prev_view_proj = have_prev_view_proj ? prev_view_proj : fg.view_proj;
    out_next_prev_vp = fg.view_proj;

    const float av_scale = std::clamp(req.avatar_scale, 0.25f, 4.0f);
    const float av_yaw_rad = req.avatar_rot_y_deg * kDegToRad;
    const float av_cos = std::cos(av_yaw_rad);
    const float av_sin = std::sin(av_yaw_rad);
    fg.avatar_pos_scale[0] = req.avatar_pos[0];
    fg.avatar_pos_scale[1] = req.avatar_pos[1];
    fg.avatar_pos_scale[2] = req.avatar_pos[2];
    fg.avatar_pos_scale[3] = av_scale;
    fg.avatar_rot_sincos[0] = av_cos;
    fg.avatar_rot_sincos[1] = av_sin;
    fg.avatar_rot_sincos[2] = av_yaw_rad;
    fg.avatar_rot_sincos[3] = 1.0f;

    Mat4Vec3 pivot_ws{req.camera.target_x, req.camera.target_y, req.camera.target_z};
    if (has_avatar && req.avatar->desc.head_joint < req.avatar->joint_anim_world_pos.size()) {
        const auto& hp = req.avatar->joint_anim_world_pos[req.avatar->desc.head_joint];
        pivot_ws = {
            (av_cos * hp[0] + av_sin * hp[2]) * av_scale + req.avatar_pos[0],
            hp[1] * av_scale + req.avatar_pos[1],
            (-av_sin * hp[0] + av_cos * hp[2]) * av_scale + req.avatar_pos[2],
        };
    }
    const float pivot_vx =
        fg.view.col0[0] * pivot_ws.x + fg.view.col1[0] * pivot_ws.y + fg.view.col2[0] * pivot_ws.z + fg.view.col3[0];
    const float pivot_vy =
        fg.view.col0[1] * pivot_ws.x + fg.view.col1[1] * pivot_ws.y + fg.view.col2[1] * pivot_ws.z + fg.view.col3[1];
    const float pivot_vz =
        fg.view.col0[2] * pivot_ws.x + fg.view.col1[2] * pivot_ws.y + fg.view.col2[2] * pivot_ws.z + fg.view.col3[2];

    fg.viewport_size[0] = static_cast<float>(w);
    fg.viewport_size[1] = static_cast<float>(h);
    fg.viewport_size[2] = 1.0f / static_cast<float>(w);
    fg.viewport_size[3] = 1.0f / static_cast<float>(h);
    fg.head_pivot_view_and_flatness[0] = pivot_vx;
    fg.head_pivot_view_and_flatness[1] = pivot_vy;
    fg.head_pivot_view_and_flatness[2] = pivot_vz;
    fg.head_pivot_view_and_flatness[3] = rig.global_live2d_flatness;

    fg.head_fwd_world[0] = av_sin;
    fg.head_fwd_world[1] = 0.0f;
    fg.head_fwd_world[2] = av_cos;
    fg.head_fwd_world[3] = rig.hair_shadow_offset_px;
    fg.head_right_world[0] = av_cos;
    fg.head_right_world[1] = 0.0f;
    fg.head_right_world[2] = -av_sin;
    fg.head_right_world[3] = rig.hair_shadow_strength;

    fg.light_params[0] = rig.sdf_feather;
    fg.light_params[1] = rig.rim_t_max;
    fg.light_params[2] = rig.bounce_t_max;
    fg.light_params[3] = rig.bounce_strength;
    fg.camera_pos_world[0] = eye.x;
    fg.camera_pos_world[1] = eye.y;
    fg.camera_pos_world[2] = eye.z;
    fg.camera_pos_world[3] = 0.05f;
    fg.post_params[0] = rig.bloom_threshold;
    fg.post_params[1] = rig.bloom_intensity;
    fg.post_params[2] = static_cast<float>(rig.tone_mode);
    fg.post_params[3] = 0.5f;
    for (int c = 0; c < 3; ++c) {
        fg.bg_color_premul[c] = rig.bg_rgba[c] * rig.bg_rgba[3];
    }
    fg.bg_color_premul[3] = rig.bg_rgba[3];
    fg.npr_style_params[0] = std::clamp(rig.face_normal_flatten, 0.0f, 1.0f);
    fg.npr_style_params[1] = std::clamp(rig.cel_band_sharpness, 0.008f, 0.35f);
    fg.npr_style_params[2] = std::clamp(rig.shadow_warm_cool_shift, 0.0f, 1.0f);
    fg.npr_style_params[3] = std::clamp(rig.iro_tsuke_strength, 0.0f, 1.0f);
    fg.npr_style_params2[0] = std::clamp(rig.crease_strength, 0.0f, 1.0f);
    fg.npr_style_params2[1] = std::clamp(rig.hair_highlight_intensity, 0.0f, 2.0f);
    fg.npr_style_params2[2] = std::clamp(rig.rim_step_strength, 0.0f, 2.0f);
    fg.npr_style_params2[3] = rig.enable_2d_filter_pro ? std::clamp(rig.painterly_flatten_strength, 0.0f, 1.0f) : 0.0f;

    fg.hair_velocity_ws[0] = req.hair_velocity_ws[0];
    fg.hair_velocity_ws[1] = req.hair_velocity_ws[1];
    fg.hair_velocity_ws[2] = req.hair_velocity_ws[2];
    fg.hair_velocity_ws[3] = std::clamp(rig.indirect_bleed_strength, 0.0f, 2.0f);
    fg.filter_pro_params[0] = std::clamp(rig.painterly_flatten_strength, 0.0f, 1.0f);
    fg.filter_pro_params[1] = std::clamp(rig.dog_ink_boost, 0.0f, 2.0f);
    fg.filter_pro_params[2] = std::clamp(rig.paper_warmth_strength, 0.0f, 1.0f);
    fg.filter_pro_params[3] = std::clamp(rig.rim_depth_offset_px, 1.0f, 6.0f);

    const uint8_t req_mode =
        (req.lighting_mode <= 2u) ? req.lighting_mode : static_cast<uint8_t>(req.rt_enabled ? 1 : 0);
    const uint8_t eff_mode = (req_mode == 0u || !req.rt_enabled || !rt_hardware_ready) ? uint8_t{0} : req_mode;
    const bool use_rt = (eff_mode != 0u);
    fg.state_u32[0] = static_cast<uint32_t>(req.frame_index & 0xFFFFFFFFull);
    fg.state_u32[1] = std::min<uint32_t>(rig.active_count, LightingRig::kMaxLights);
    fg.state_u32[2] = static_cast<uint32_t>(req.debug_view);
    fg.state_u32[3] = (use_rt ? 1u : 0u) | (eff_mode == 2u ? 2u : 0u) | (rig.enable_2d_filter_pro ? 4u : 0u) |
                      (rig.enable_paper_warmth ? 8u : 0u) | ((static_cast<uint32_t>(req.aa_mode) & 3u) << 8u) |
                      ((static_cast<uint32_t>(eff_mode) & 3u) << 10u);

    for (uint32_t li = 0; li < LightingRig::kMaxLights; ++li) {
        const auto& sl = rig.lights[li];
        auto& gl = fg.lights[li];
        const Mat4Vec3 ldir = scene_light_dir_to_light(sl);
        gl.pos_or_dir[0] = (sl.type == LightType::Directional) ? ldir.x : sl.direction_or_pos[0];
        gl.pos_or_dir[1] = (sl.type == LightType::Directional) ? ldir.y : sl.direction_or_pos[1];
        gl.pos_or_dir[2] = (sl.type == LightType::Directional) ? ldir.z : sl.direction_or_pos[2];
        gl.pos_or_dir[3] = (sl.type == LightType::Directional) ? 0.0f : 1.0f;
        gl.color_intensity[0] = sl.color[0] * sl.intensity;
        gl.color_intensity[1] = sl.color[1] * sl.intensity;
        gl.color_intensity[2] = sl.color[2] * sl.intensity;
        gl.color_intensity[3] = sl.cone_angle_rad;
        gl.direction_radius[0] = ldir.x;
        gl.direction_radius[1] = ldir.y;
        gl.direction_radius[2] = ldir.z;
        gl.direction_radius[3] = sl.angular_radius_rad;
        gl.role_and_flags[0] = static_cast<uint32_t>(sl.role);
        gl.role_and_flags[1] = (sl.enabled ? 1u : 0u) | (sl.cast_rt_shadow ? 2u : 0u);
        gl.role_and_flags[2] = 0xFFu;
    }
    fg.shadow_view_proj[0] = build_csm_ortho_matrix(pivot_ws, scene_light_dir_to_light(rig.lights[0]));
    return eff_mode;
}

/**
 * Shared material table + opaque/cutout/transparent bucket builder used by both Vulkan `FrameGraph`
 * and DX12 `Dx12FrameGraph` so material flags, SDF slots, and stencil refs stay 100% consistent.
 */
inline void populate_materials_and_submeshes(const GpuAvatar& avatar, std::span<const uint32_t> tex_slots,
                                             uint32_t fallback_white_slot, uint32_t face_sdf_slot,
                                             std::vector<GpuMaterial>& out_materials,
                                             std::vector<GpuSubmeshDraw>& out_submeshes,
                                             std::vector<uint32_t>& out_opaque, std::vector<uint32_t>& out_cutout,
                                             std::vector<uint32_t>& out_transparent) {
    auto resolve_tex = [&](uint32_t idx, uint32_t fallback) -> uint32_t {
        if (idx == AETHER_INVALID_INDEX || idx >= tex_slots.size()) {
            return fallback;
        }
        return tex_slots[idx];
    };

    const size_t mat_count = std::max<size_t>(avatar.materials.size(), 1U);
    out_materials.assign(mat_count, GpuMaterial{});
    for (size_t i = 0; i < mat_count; ++i) {
        GpuMaterial& gm = out_materials[i];
        if (i < avatar.materials.size()) {
            const auto& m = avatar.materials[i];
            std::memcpy(gm.lit_color, m.base_color, sizeof(gm.lit_color));
            gm.shade_color[0] = m.shade_color_gi[0];
            gm.shade_color[1] = m.shade_color_gi[1];
            gm.shade_color[2] = m.shade_color_gi[2];
            gm.shade_color[3] = 1.0f;
            gm.rim_color_power[0] = m.rim_color_mix[0];
            gm.rim_color_power[1] = m.rim_color_mix[1];
            gm.rim_color_power[2] = m.rim_color_mix[2];
            gm.rim_color_power[3] = std::max(m.toon_params[2], 1.0f);
            gm.outline_color_width[0] = m.outline_color_mix[0];
            gm.outline_color_width[1] = m.outline_color_mix[1];
            gm.outline_color_width[2] = m.outline_color_mix[2];
            gm.outline_color_width[3] = m.outline_width;
            gm.emission_and_shift[0] = m.emissive_and_pad[0];
            gm.emission_and_shift[1] = m.emissive_and_pad[1];
            gm.emission_and_shift[2] = m.emissive_and_pad[2];
            gm.emission_and_shift[3] = m.toon_params[0];
            gm.toon_params[0] = m.toon_params[1];
            gm.toon_params[1] = m.emissive_and_pad[3];
            gm.toon_params[2] = 0.08f;
            gm.toon_params[3] = m.uv_outline_alpha[3];
            gm.hair_specular_params[0] = 0.0f;
            gm.hair_specular_params[1] = 0.35f;
            gm.hair_specular_params[2] = m.shade_color_gi[3];
            gm.hair_specular_params[3] = 0.35f;
            const uint32_t base_slot = resolve_tex(m.tex_base, fallback_white_slot);
            gm.tex_slots[0] = base_slot;
            gm.tex_slots[1] = resolve_tex(m.tex_shade, base_slot);
            gm.tex_slots[2] = resolve_tex(m.tex_sdf_face, face_sdf_slot);
            gm.tex_slots[3] = resolve_tex(m.tex_matcap, UINT32_MAX);
            gm.extra_slots[0] = resolve_tex(m.tex_normal, UINT32_MAX);
            gm.extra_slots[1] = resolve_tex(m.tex_emissive, UINT32_MAX);
            gm.extra_slots[2] = m.flags;
            gm.extra_slots[3] = m.alpha_mode;
            gm.bounce_tint_zflat[0] = m.base_color[0];
            gm.bounce_tint_zflat[1] = m.base_color[1];
            gm.bounce_tint_zflat[2] = m.base_color[2];
            gm.bounce_tint_zflat[3] = 1.0f;
        } else {
            gm.tex_slots[0] = fallback_white_slot;
            gm.tex_slots[1] = fallback_white_slot;
            gm.tex_slots[2] = face_sdf_slot;
            gm.tex_slots[3] = UINT32_MAX;
            gm.extra_slots[0] = UINT32_MAX;
            gm.extra_slots[1] = UINT32_MAX;
        }
    }

    out_submeshes.clear();
    out_opaque.clear();
    out_cutout.clear();
    out_transparent.clear();
    if (avatar.submeshes.empty()) {
        GpuSubmeshDraw d{};
        d.index_offset = 0;
        d.index_count = avatar.requirements.index_count;
        d.vertex_offset = 0;
        d.material_index = 0;
        d.stencil_ref = 0x01u;
        d.flags = AETHER_SUBMESH_FLAG_OPAQUE;
        out_submeshes.push_back(d);
    } else {
        out_submeshes.reserve(avatar.submeshes.size());
        for (const auto& sm : avatar.submeshes) {
            GpuSubmeshDraw d{};
            d.index_offset = sm.index_offset;
            d.index_count = sm.index_count;
            d.vertex_offset = sm.vertex_offset;
            d.material_index = std::min<uint32_t>(sm.material_index, static_cast<uint32_t>(out_materials.size() - 1));
            const auto& gm = out_materials[d.material_index];
            uint32_t st = 0x01u;
            if ((gm.extra_slots[2] & AETHER_MAT_FLAG_HAIR_FRONT_STENCIL) != 0u ||
                (sm.flags & AETHER_SUBMESH_FLAG_HAIR_FRONT) != 0u) {
                st |= 0x02u;
            }
            if ((gm.extra_slots[2] & AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW) != 0u ||
                (sm.flags & AETHER_SUBMESH_FLAG_FACE) != 0u) {
                st |= 0x04u;
            }
            d.stencil_ref = st;
            d.cull_mode = ((gm.extra_slots[2] & AETHER_MAT_FLAG_DOUBLE_SIDED) != 0u) ? 1u : 0u;
            d.alpha_mode = gm.extra_slots[3];
            d.flags = sm.flags;
            out_submeshes.push_back(d);
        }
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(out_submeshes.size()); ++i) {
        const auto& d = out_submeshes[i];
        if (d.alpha_mode == AETHER_ALPHA_MODE_BLEND || (d.flags & AETHER_SUBMESH_FLAG_TRANSPARENT) != 0u) {
            out_transparent.push_back(i);
        } else if (d.alpha_mode == AETHER_ALPHA_MODE_MASK || (d.flags & AETHER_SUBMESH_FLAG_ALPHA_CUTOUT) != 0u) {
            out_cutout.push_back(i);
        } else {
            out_opaque.push_back(i);
        }
    }
}

} // namespace aether::renderer::detail
