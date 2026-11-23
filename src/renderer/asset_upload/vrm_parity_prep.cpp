#include "renderer/asset_upload/vrm_parity_prep.hpp"

#include "renderer/avatar_rt_classifier.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace aether::renderer {

namespace {

[[nodiscard]] std::array<float, 3> lin_srgb_to_oklab_cpu(float r, float g, float b) noexcept {
    r = std::max(r, 0.0f);
    g = std::max(g, 0.0f);
    b = std::max(b, 0.0f);
    const float l = std::cbrtf(std::max(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b, 1e-6f));
    const float m = std::cbrtf(std::max(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b, 1e-6f));
    const float s = std::cbrtf(std::max(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b, 1e-6f));
    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    };
}

[[nodiscard]] std::array<float, 3> oklab_to_lin_srgb_cpu(const std::array<float, 3>& lab) noexcept {
    const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    return {
        std::clamp(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s, 0.0f, 1.0f),
        std::clamp(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s, 0.0f, 1.0f),
        std::clamp(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s, 0.0f, 1.0f),
    };
}

[[nodiscard]] bool sample_rgba8_green(const AetherTextureDesc& td, const std::vector<uint8_t>& pixels, float u, float v,
                                      float& out_g) noexcept {
    if ((td.format != AETHER_TEX_FORMAT_RGBA8_UNORM && td.format != AETHER_TEX_FORMAT_RGBA8_SRGB) || td.width == 0U ||
        td.height == 0U) {
        return false;
    }
    const uint64_t row_bytes = static_cast<uint64_t>(td.width) * 4ULL;
    const uint64_t needed = row_bytes * static_cast<uint64_t>(td.height);
    if (td.pixel_offset + needed > pixels.size()) {
        return false;
    }
    auto wrap01 = [](float t) noexcept {
        const float f = std::fmod(t, 1.0f);
        return (f < 0.0f) ? (f + 1.0f) : f;
    };
    const uint32_t tx =
        std::min<uint32_t>(td.width - 1U, static_cast<uint32_t>(wrap01(u) * static_cast<float>(td.width)));
    const uint32_t ty =
        std::min<uint32_t>(td.height - 1U, static_cast<uint32_t>(wrap01(v) * static_cast<float>(td.height)));
    const size_t idx = static_cast<size_t>(td.pixel_offset + (static_cast<uint64_t>(ty) * row_bytes) + tx * 4ULL);
    out_g = static_cast<float>(pixels[idx + 1U]) / 255.0f;
    return true;
}

[[nodiscard]] bool is_eye_or_lash_detail_name(std::string_view m_low, std::string_view s_low) noexcept {
    return contains_any(m_low, {"highlight", "eyelash", "lash", "brow", "eyeline", "sclera", "tear"}) ||
           contains_any(s_low, {"highlight", "eyelash", "lash", "brow", "eyeline", "sclera", "tear"});
}

} // namespace

VrmParityStats prepare_vrm_and_pmx_parity(GpuAvatar& avatar, bool vrm_auto_edge_scale_enabled) {
    VrmParityStats stats{};
    const size_t v_count = avatar.rest_vertices.size();
    if (v_count == 0U || avatar.materials.empty()) {
        return stats;
    }

    // 1) Modulate per-vertex outline_width_scale from MToon _OutlineWidthTexture when authored.
    if (avatar.rest_surfaces.size() == v_count && !avatar.cpu_texture_pixels.empty()) {
        std::vector<uint8_t> modulated(v_count, 0U);
        for (const auto& sm : avatar.submeshes) {
            if (sm.material_index >= avatar.materials.size()) {
                continue;
            }
            const auto& mat = avatar.materials[sm.material_index];
            if (mat.tex_outline_width >= avatar.texture_descs.size()) {
                continue;
            }
            const auto& td = avatar.texture_descs[mat.tex_outline_width];
            const size_t i_end =
                std::min<size_t>(static_cast<size_t>(sm.index_offset) + sm.index_count, avatar.rest_indices.size());
            for (size_t ii = sm.index_offset; ii < i_end; ++ii) {
                const uint32_t vi = avatar.rest_indices[ii];
                if (vi >= v_count || modulated[vi] != 0U) {
                    continue;
                }
                modulated[vi] = 1U;
                float g = 1.0f;
                if (sample_rgba8_green(td, avatar.cpu_texture_pixels, avatar.rest_surfaces[vi].uv0[0],
                                       avatar.rest_surfaces[vi].uv0[1], g)) {
                    avatar.rest_vertices[vi].outline_width_scale *= std::clamp(g, 0.0f, 1.0f);
                    ++stats.outline_tex_modulated_vertices;
                }
            }
        }
    }

    // 2) Procedural edge_scale tapering on VRM/glTF avatars when no PMX edge_scale or outline texture is authored.
    const bool is_vrm_source = (avatar.desc.source_format == AETHER_SOURCE_FORMAT_VRM0 ||
                                avatar.desc.source_format == AETHER_SOURCE_FORMAT_VRM1 ||
                                avatar.desc.source_format == AETHER_SOURCE_FORMAT_GLTF);
    if (vrm_auto_edge_scale_enabled && is_vrm_source && avatar.pmx_aux_header.is_pmx == 0U &&
        !avatar.rest_indices.empty()) {
        std::vector<uint8_t> processed(v_count, 0U);
        for (const auto& sm : avatar.submeshes) {
            if (sm.material_index >= avatar.materials.size()) {
                continue;
            }
            const auto& mat = avatar.materials[sm.material_index];
            if (mat.tex_outline_width < avatar.texture_descs.size()) {
                continue;
            }
            const std::string m_low = to_lower_ascii(read_material_cstr(avatar, mat.name_offset));
            const std::string s_low = to_lower_ascii(avatar.string_slice(sm.name_offset, sm.name_len));
            const bool suppress_detail = is_eye_or_lash_detail_name(m_low, s_low);
            const bool is_face =
                ((sm.flags & AETHER_SUBMESH_FLAG_FACE) != 0U) || contains_any(m_low, {"face", "skin", "hada"});
            const size_t i_end =
                std::min<size_t>(static_cast<size_t>(sm.index_offset) + sm.index_count, avatar.rest_indices.size());
            for (size_t ii = sm.index_offset; ii < i_end; ++ii) {
                const uint32_t vi = avatar.rest_indices[ii];
                if (vi >= v_count || processed[vi] != 0U) {
                    continue;
                }
                processed[vi] = 1U;
                auto& rv = avatar.rest_vertices[vi];
                if (rv.outline_width_scale <= 0.001f) {
                    continue; // Preserve bake.rs jaw-to-neck seam suppression
                }
                if (suppress_detail) {
                    rv.outline_width_scale = 0.0f;
                    ++stats.procedural_edge_vertices;
                    continue;
                }
                const float dot_ns = rv.true_normal[0] * rv.smooth_tangent[0] +
                                     rv.true_normal[1] * rv.smooth_tangent[1] +
                                     rv.true_normal[2] * rv.smooth_tangent[2];
                if (dot_ns > 0.10f) {
                    const float taper = std::clamp((std::clamp(dot_ns, 0.0f, 1.0f) - 0.55f) / 0.40f, 0.0f, 1.0f);
                    const float min_scale = is_face ? 0.65f : 0.72f;
                    rv.outline_width_scale *= (min_scale + (1.0f - min_scale) * taper);
                    ++stats.procedural_edge_vertices;
                }
            }
        }
    }

    // 3) OKLab Shade Color Harmonizer for VRM materials with unharmonized / neutral shade tints.
    if (avatar.pmx_aux_header.is_pmx == 0U) {
        std::vector<uint32_t> mat_sm_flags(avatar.materials.size(), 0U);
        std::vector<std::string> mat_sm_names(avatar.materials.size());
        for (const auto& sm : avatar.submeshes) {
            if (sm.material_index < avatar.materials.size()) {
                mat_sm_flags[sm.material_index] |= sm.flags;
                if (mat_sm_names[sm.material_index].empty() && sm.name_len > 0U) {
                    mat_sm_names[sm.material_index] = avatar.string_slice(sm.name_offset, sm.name_len);
                }
            }
        }
        for (size_t mi = 0; mi < avatar.materials.size(); ++mi) {
            auto& mat = avatar.materials[mi];
            if ((mat.flags & AETHER_MAT_FLAG_UNLIT) != 0U) {
                continue;
            }
            const std::string m_name = read_material_cstr(avatar, mat.name_offset);
            const RtMaskBucket bucket = classify_rt_bucket(m_name, mat_sm_names[mi], mat.flags, mat_sm_flags[mi]);
            const std::array<float, 3> lab_lit =
                lin_srgb_to_oklab_cpu(mat.base_color[0], mat.base_color[1], mat.base_color[2]);
            std::array<float, 3> lab_sh =
                lin_srgb_to_oklab_cpu(mat.shade_color_gi[0], mat.shade_color_gi[1], mat.shade_color_gi[2]);
            const float sh_chroma = std::hypot(lab_sh[1], lab_sh[2]);
            const float lit_diff = std::abs(mat.shade_color_gi[0] - mat.base_color[0]) +
                                   std::abs(mat.shade_color_gi[1] - mat.base_color[1]) +
                                   std::abs(mat.shade_color_gi[2] - mat.base_color[2]);
            const bool is_unharmonized =
                (mat.tex_shade == AETHER_INVALID_INDEX && (lit_diff < 0.04f || sh_chroma < 0.014f));
            if (!is_unharmonized) {
                continue;
            }
            const bool is_skin =
                (bucket == RtMaskBucket::FaceSkin) || is_body_skin_atlas_name(m_name, mat_sm_names[mi]);
            if (is_skin) {
                lab_sh[0] = std::clamp(lab_lit[0] * 0.87f, 0.25f, 0.92f);
                lab_sh[1] = lab_lit[1] + 0.024f;
                lab_sh[2] = lab_lit[2] + 0.010f;
            } else {
                lab_sh[0] = std::clamp(lab_lit[0] * 0.78f, 0.12f, 0.88f);
                lab_sh[1] = lab_lit[1] - 0.005f;
                lab_sh[2] = lab_lit[2] - 0.024f;
            }
            const auto rgb_out = oklab_to_lin_srgb_cpu(lab_sh);
            mat.shade_color_gi[0] = std::max(rgb_out[0], 0.015f);
            mat.shade_color_gi[1] = std::max(rgb_out[1], 0.015f);
            mat.shade_color_gi[2] = std::max(rgb_out[2], 0.015f);
            ++stats.oklab_harmonized_materials;
        }
    }

    return stats;
}

} // namespace aether::renderer
