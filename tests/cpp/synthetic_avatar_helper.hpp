#pragma once

#include "aether_ffi_contract.h"
#include "core/expected.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "rhi/rhi_device.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace aether::test {

struct SyntheticMeshSpec {
    std::vector<AetherVertex25D> vertices{};
    std::vector<AetherVertexSurface> surfaces{};
    std::vector<AetherVertexSkin> skins{};
    std::vector<uint32_t> indices{};
    std::vector<AetherSubmeshDesc> submeshes{};
    std::vector<AetherNprMaterial> materials{};
    std::vector<AetherTextureDesc> tex_descs{};
    std::vector<uint8_t> tex_pixels{};
    std::vector<AetherJointDesc> joints{};
};

[[nodiscard]] inline uint64_t align_up_256(uint64_t v) noexcept {
    return (v + 255ULL) & ~255ULL;
}

[[nodiscard]] inline AetherNprMaterial make_basic_npr_material(float r, float g, float b, float a = 1.0f,
                                                               uint32_t flags = 0,
                                                               float outline_width = 0.0f) noexcept {
    AetherNprMaterial m{};
    m.base_color[0] = r;
    m.base_color[1] = g;
    m.base_color[2] = b;
    m.base_color[3] = a;
    m.shade_color_gi[0] = r * 0.45f;
    m.shade_color_gi[1] = g * 0.45f;
    m.shade_color_gi[2] = b * 0.50f;
    m.shade_color_gi[3] = 0.0f;
    m.rim_color_mix[0] = 0.0f;
    m.rim_color_mix[1] = 0.0f;
    m.rim_color_mix[2] = 0.0f;
    m.rim_color_mix[3] = 0.0f;
    m.outline_color_mix[0] = 0.05f;
    m.outline_color_mix[1] = 0.05f;
    m.outline_color_mix[2] = 0.08f;
    m.outline_color_mix[3] = 0.0f;
    m.toon_params[0] = -0.05f;
    m.toon_params[1] = 0.85f;
    m.toon_params[2] = 3.0f;
    m.toon_params[3] = 0.0f;
    m.uv_outline_alpha[3] = 0.5f;
    m.outline_width = outline_width;
    m.alpha_mode = AETHER_ALPHA_MODE_OPAQUE;
    m.flags = flags;
    m.tex_base = AETHER_INVALID_INDEX;
    m.tex_shade = AETHER_INVALID_INDEX;
    m.tex_shading_shift = AETHER_INVALID_INDEX;
    m.tex_normal = AETHER_INVALID_INDEX;
    m.tex_emissive = AETHER_INVALID_INDEX;
    m.tex_rim = AETHER_INVALID_INDEX;
    m.tex_matcap = AETHER_INVALID_INDEX;
    m.tex_outline_width = AETHER_INVALID_INDEX;
    m.tex_uv_mask = AETHER_INVALID_INDEX;
    m.tex_ramp = AETHER_INVALID_INDEX;
    m.tex_sdf_face = AETHER_INVALID_INDEX;
    return m;
}

inline void append_quad(SyntheticMeshSpec& spec, const float p0[3], const float p1[3], const float p2[3],
                        const float p3[3], const float nrm[3], const float tangent[3], float z_flatten_w,
                        float sdf_mask, float outline_scale, uint32_t material_id, uint32_t submesh_flags) {
    const uint32_t base_v = static_cast<uint32_t>(spec.vertices.size());
    const uint32_t base_i = static_cast<uint32_t>(spec.indices.size());
    const float* pts[4] = {p0, p1, p2, p3};
    const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    for (int k = 0; k < 4; ++k) {
        AetherVertex25D v{};
        v.position[0] = pts[k][0];
        v.position[1] = pts[k][1];
        v.position[2] = pts[k][2];
        v.outline_width_scale = outline_scale;
        v.true_normal[0] = nrm[0];
        v.true_normal[1] = nrm[1];
        v.true_normal[2] = nrm[2];
        v.z_flatten_weight = z_flatten_w;
        v.proxy_normal[0] = nrm[0];
        v.proxy_normal[1] = nrm[1];
        v.proxy_normal[2] = nrm[2];
        v.sdf_face_mask = sdf_mask;
        v.smooth_tangent[0] = tangent[0];
        v.smooth_tangent[1] = tangent[1];
        v.smooth_tangent[2] = tangent[2];
        v.smooth_tangent[3] = 1.0f;
        spec.vertices.push_back(v);

        AetherVertexSurface s{};
        s.uv0[0] = uvs[k][0];
        s.uv0[1] = uvs[k][1];
        s.uv1[0] = uvs[k][0];
        s.uv1[1] = uvs[k][1];
        s.color_rgba8 = 0xFFFFFFFFu;
        s.flags = (material_id & 0xFFu) << 8u;
        spec.surfaces.push_back(s);

        AetherVertexSkin sk{};
        sk.joints[0] = 0;
        sk.weights_unorm16[0] = 65535u;
        spec.skins.push_back(sk);
    }

    const uint32_t idx[6] = {base_v + 0, base_v + 1, base_v + 2, base_v + 0, base_v + 2, base_v + 3};
    spec.indices.insert(spec.indices.end(), idx, idx + 6);

    AetherSubmeshDesc sm{};
    sm.index_offset = base_i;
    sm.index_count = 6;
    sm.vertex_offset = base_v;
    sm.vertex_count = 4;
    sm.material_index = material_id;
    sm.flags = submesh_flags;
    spec.submeshes.push_back(sm);
}

[[nodiscard]] inline Expected<std::unique_ptr<renderer::GpuAvatar>>
upload_synthetic_avatar(rhi::RhiDevice& rhi, const SyntheticMeshSpec& spec) {
    auto staged = std::make_unique<renderer::StagedAvatarUpload>();
    staged->device = &rhi;

    SyntheticMeshSpec clean = spec;
    if (clean.joints.empty()) {
        AetherJointDesc root{};
        root.parent_index = -1;
        root.humanoid_bone = 0;
        root.region_tag = AETHER_JOINT_REGION_HEAD;
        root.tail_length = 0.20f;
        root.bind_rotation_quat[3] = 1.0f;
        root.bind_scale[0] = 1.0f;
        root.bind_scale[1] = 1.0f;
        root.bind_scale[2] = 1.0f;
        clean.joints.push_back(root);
    }

    staged->requirements.vertex_count = static_cast<uint32_t>(clean.vertices.size());
    staged->requirements.index_count = static_cast<uint32_t>(clean.indices.size());
    staged->requirements.stream_bytes[AETHER_STREAM_VERTEX25D] = clean.vertices.size() * sizeof(AetherVertex25D);
    staged->requirements.stream_bytes[AETHER_STREAM_SURFACE] = clean.surfaces.size() * sizeof(AetherVertexSurface);
    staged->requirements.stream_bytes[AETHER_STREAM_SKIN] = clean.skins.size() * sizeof(AetherVertexSkin);
    staged->requirements.stream_bytes[AETHER_STREAM_INDEX_U32] = clean.indices.size() * sizeof(uint32_t);
    staged->requirements.stream_bytes[AETHER_STREAM_SUBMESHES] = clean.submeshes.size() * sizeof(AetherSubmeshDesc);
    staged->requirements.stream_bytes[AETHER_STREAM_MATERIALS] = clean.materials.size() * sizeof(AetherNprMaterial);
    staged->requirements.stream_bytes[AETHER_STREAM_JOINTS] = clean.joints.size() * sizeof(AetherJointDesc);
    staged->requirements.stream_bytes[AETHER_STREAM_TEXTURE_DESCS] = clean.tex_descs.size() * sizeof(AetherTextureDesc);
    staged->requirements.stream_bytes[AETHER_STREAM_TEXTURE_PIXELS] = clean.tex_pixels.size();

    uint64_t total = 0;
    for (uint32_t s = 0; s < AETHER_STREAM_COUNT; ++s) {
        staged->stream_offsets[s] = total;
        const uint64_t len = staged->requirements.stream_bytes[s];
        if (len > 0) {
            total += align_up_256(len);
        }
    }
    staged->fallback_white_offset = total;
    total += 256ULL;

    auto buf_res = rhi.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buf_res) {
        return make_unexpected(buf_res.error());
    }
    staged->staging_buf = buf_res.value();
    auto* base = static_cast<uint8_t*>(staged->staging_buf.info.pMappedData);
    auto copy_pod = [&](uint32_t stream, const auto& vec) {
        if (!vec.empty()) {
            std::memcpy(base + staged->stream_offsets[stream], vec.data(), vec.size() * sizeof(vec[0]));
        }
    };
    copy_pod(AETHER_STREAM_VERTEX25D, clean.vertices);
    copy_pod(AETHER_STREAM_SURFACE, clean.surfaces);
    copy_pod(AETHER_STREAM_SKIN, clean.skins);
    copy_pod(AETHER_STREAM_INDEX_U32, clean.indices);
    copy_pod(AETHER_STREAM_SUBMESHES, clean.submeshes);
    copy_pod(AETHER_STREAM_MATERIALS, clean.materials);
    copy_pod(AETHER_STREAM_JOINTS, clean.joints);
    copy_pod(AETHER_STREAM_TEXTURE_DESCS, clean.tex_descs);
    copy_pod(AETHER_STREAM_TEXTURE_PIXELS, clean.tex_pixels);

    staged->desc.head_joint = 0;
    for (uint16_t& m : staged->desc.humanoid_to_joint) {
        m = 0xFFFFu;
    }
    staged->desc.humanoid_to_joint[0] = 0;
    staged->pointer_identity_verified = true;

    return renderer::upload_staged_avatar(rhi, std::move(staged));
}

} // namespace aether::test
