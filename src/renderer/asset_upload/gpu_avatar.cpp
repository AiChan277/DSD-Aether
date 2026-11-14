#include "renderer/asset_upload/gpu_avatar.hpp"

#include "animation/math_quat.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace aether::renderer {
namespace {

struct Quatf {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
};

Quatf normalize_quat(Quatf q) noexcept {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-6f) {
        const float inv = 1.0f / n;
        q.x *= inv;
        q.y *= inv;
        q.z *= inv;
        q.w *= inv;
    } else {
        q = Quatf{0.0f, 0.0f, 0.0f, 1.0f};
    }
    return q;
}

Quatf mul_quat(Quatf a, Quatf b) noexcept {
    return Quatf{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

std::array<float, 3> rotate_vec(Quatf q, const float v[3]) noexcept {
    const float qx = q.x;
    const float qy = q.y;
    const float qz = q.z;
    const float qw = q.w;
    const float ux = qy * v[2] - qz * v[1];
    const float uy = qz * v[0] - qx * v[2];
    const float uz = qx * v[1] - qy * v[0];
    const float tx = 2.0f * ux;
    const float ty = 2.0f * uy;
    const float tz = 2.0f * uz;
    return {
        v[0] + qw * tx + (qy * tz - qz * ty),
        v[1] + qw * ty + (qz * tx - qx * tz),
        v[2] + qw * tz + (qx * ty - qy * tx),
    };
}

void compute_rest_joint_world_positions(const std::vector<AetherJointDesc>& joints,
                                        std::vector<std::array<float, 3>>& out_pos) {
    out_pos.assign(joints.size(), {0.0f, 0.0f, 0.0f});
    if (joints.empty()) {
        return;
    }

    std::vector<Quatf> world_rot(joints.size());
    std::vector<uint8_t> visited(joints.size(), 0U);

    // Joints from Rust normaliser are topological/scene-ordered; iterate iteratively to handle any parent order.
    for (size_t pass = 0; pass < joints.size(); ++pass) {
        bool any = false;
        for (size_t i = 0; i < joints.size(); ++i) {
            if (visited[i] != 0U) {
                continue;
            }
            const int32_t p = joints[i].parent_index;
            if (p >= 0 && static_cast<size_t>(p) < joints.size() && visited[static_cast<size_t>(p)] == 0U &&
                static_cast<size_t>(p) != i) {
                continue;
            }
            const Quatf lr = normalize_quat(Quatf{joints[i].bind_rotation_quat[0], joints[i].bind_rotation_quat[1],
                                                  joints[i].bind_rotation_quat[2], joints[i].bind_rotation_quat[3]});
            if (p >= 0 && static_cast<size_t>(p) < joints.size() && static_cast<size_t>(p) != i) {
                const auto& pw = out_pos[static_cast<size_t>(p)];
                const Quatf& pr = world_rot[static_cast<size_t>(p)];
                const auto rotated = rotate_vec(pr, joints[i].bind_translation);
                out_pos[i] = {pw[0] + rotated[0], pw[1] + rotated[1], pw[2] + rotated[2]};
                world_rot[i] = normalize_quat(mul_quat(pr, lr));
            } else {
                out_pos[i] = {joints[i].bind_translation[0], joints[i].bind_translation[1],
                              joints[i].bind_translation[2]};
                world_rot[i] = lr;
            }
            visited[i] = 1U;
            any = true;
        }
        if (!any) {
            break;
        }
    }
}

void emit_bone_line_vertices(const std::vector<AetherJointDesc>& joints,
                             const std::vector<std::array<float, 3>>& world_pos,
                             std::vector<BoneLineVertex>& out_verts) noexcept {
    if (joints.empty() || world_pos.size() != joints.size() || out_verts.size() != joints.size() * 4U) {
        return;
    }

    size_t write_idx = 0;
    for (size_t i = 0; i < joints.size(); ++i) {
        const auto& c = world_pos[i];
        const bool is_humanoid = (joints[i].humanoid_bone < AETHER_HUMANOID_BONE_COUNT);
        const float col[4] = {
            is_humanoid ? 0.22f : 0.85f,
            is_humanoid ? 0.82f : 0.45f,
            is_humanoid ? 0.95f : 0.20f,
            0.95f,
        };
        const int32_t p = joints[i].parent_index;
        if (p >= 0 && static_cast<size_t>(p) < joints.size() && static_cast<size_t>(p) != i) {
            const auto& pp = world_pos[static_cast<size_t>(p)];
            out_verts[write_idx++] = BoneLineVertex{{pp[0], pp[1], pp[2]}, {col[0], col[1], col[2], col[3]}};
            out_verts[write_idx++] = BoneLineVertex{{c[0], c[1], c[2]}, {col[0], col[1], col[2], col[3]}};
        } else {
            const float tip = std::clamp(joints[i].tail_length, 0.03f, 0.12f);
            out_verts[write_idx++] = BoneLineVertex{{c[0], c[1], c[2]}, {col[0], col[1], col[2], col[3]}};
            out_verts[write_idx++] = BoneLineVertex{{c[0], c[1] + tip, c[2]}, {col[0], col[1], col[2], col[3]}};
        }

        const float tick = 0.012f;
        out_verts[write_idx++] = BoneLineVertex{{c[0] - tick, c[1], c[2]}, {col[0], col[1], col[2], 0.85f}};
        out_verts[write_idx++] = BoneLineVertex{{c[0] + tick, c[1], c[2]}, {col[0], col[1], col[2], 0.85f}};
    }
}

inline void mul_3x4(const std::array<float, 12>& a, const std::array<float, 12>& b,
                    std::array<float, 12>& out) noexcept {
    for (int r = 0; r < 3; ++r) {
        const float a0 = a[static_cast<size_t>(r) * 4U + 0U];
        const float a1 = a[static_cast<size_t>(r) * 4U + 1U];
        const float a2 = a[static_cast<size_t>(r) * 4U + 2U];
        const float a3 = a[static_cast<size_t>(r) * 4U + 3U];
        out[static_cast<size_t>(r) * 4U + 0U] = a0 * b[0] + a1 * b[4] + a2 * b[8];
        out[static_cast<size_t>(r) * 4U + 1U] = a0 * b[1] + a1 * b[5] + a2 * b[9];
        out[static_cast<size_t>(r) * 4U + 2U] = a0 * b[2] + a1 * b[6] + a2 * b[10];
        out[static_cast<size_t>(r) * 4U + 3U] = a0 * b[3] + a1 * b[7] + a2 * b[11] + a3;
    }
}

void detect_brow_bone_bindings(const std::vector<AetherJointDesc>& joints, const std::string& strings,
                               const std::vector<int32_t>& joint_humanoid_source,
                               const std::vector<std::array<float, 3>>& rest_world_pos,
                               std::vector<GpuAvatar::BrowBoneBinding>& out_bones) {
    out_bones.clear();
    auto contains_ci = [](std::string_view hay, std::string_view needle) noexcept -> bool {
        if (needle.empty() || hay.size() < needle.size()) {
            return false;
        }
        for (size_t i = 0; i <= hay.size() - needle.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                char a = hay[i + j];
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a + ('a' - 'A'));
                }
                if (a != needle[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    };
    for (size_t j = 0; j < joints.size(); ++j) {
        const AetherJointDesc& jd = joints[j];
        if (jd.humanoid_bone < AETHER_HUMANOID_BONE_COUNT || jd.name_len == 0U ||
            static_cast<uint64_t>(jd.name_offset) + jd.name_len > strings.size()) {
            continue;
        }
        if (j < joint_humanoid_source.size() && joint_humanoid_source[j] != AETHER_BONE_HEAD &&
            joint_humanoid_source[j] >= 0) {
            continue;
        }
        const std::string_view name(strings.data() + jd.name_offset, jd.name_len);
        const bool is_brow = contains_ci(name, "brow") || contains_ci(name, "mayu") ||
                             name.find("\xE7\x9C\x89") != std::string_view::npos ||
                             name.find("\xE3\x81\xBE\xE3\x82\x86") != std::string_view::npos;
        if (!is_brow) {
            continue;
        }
        uint8_t side = 2U;
        if (contains_ci(name, "left") || contains_ci(name, "_l") || contains_ci(name, ".l") ||
            name.find("\xE5\xB7\xA6") != std::string_view::npos) {
            side = 0U;
        } else if (contains_ci(name, "right") || contains_ci(name, "_r") || contains_ci(name, ".r") ||
                   name.find("\xE5\x8F\xB3") != std::string_view::npos) {
            side = 1U;
        } else if (j < rest_world_pos.size()) {
            side = (rest_world_pos[j][0] > 0.004f) ? 0U : ((rest_world_pos[j][0] < -0.004f) ? 1U : 2U);
        }
        out_bones.push_back(GpuAvatar::BrowBoneBinding{static_cast<uint32_t>(j), side});
    }
}

void build_joint_humanoid_sources(const std::vector<AetherJointDesc>& joints, const AetherAvatarDesc& desc,
                                  std::vector<int32_t>& out_sources) {
    out_sources.assign(joints.size(), -1);
    static constexpr uint16_t kHipsBoneIndex = 0U; // Appendix A canonical bone 0 = Hips
    const int32_t fallback_hips =
        (desc.humanoid_to_joint[kHipsBoneIndex] < joints.size()) ? static_cast<int32_t>(kHipsBoneIndex) : -1;
    for (size_t i = 0; i < joints.size(); ++i) {
        size_t cur = i;
        int32_t resolved = -1;
        for (size_t hops = 0; hops < joints.size(); ++hops) {
            if (joints[cur].humanoid_bone < AETHER_HUMANOID_BONE_COUNT) {
                resolved = static_cast<int32_t>(joints[cur].humanoid_bone);
                break;
            }
            const int32_t p = joints[cur].parent_index;
            if (p < 0 || static_cast<size_t>(p) >= joints.size() || static_cast<size_t>(p) == cur) {
                break;
            }
            cur = static_cast<size_t>(p);
        }
        out_sources[i] = (resolved >= 0) ? resolved : fallback_hips;
    }
}

inline void transform_point_3x4(const std::array<float, 12>& m, const float in_p[3], float out_p[3]) noexcept {
    const float x = in_p[0];
    const float y = in_p[1];
    const float z = in_p[2];
    out_p[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    out_p[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    out_p[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
}

inline void transform_dir_3x4(const std::array<float, 12>& m, const float in_d[3], float out_d[3]) noexcept {
    const float x = in_d[0];
    const float y = in_d[1];
    const float z = in_d[2];
    const float rx = m[0] * x + m[1] * y + m[2] * z;
    const float ry = m[4] * x + m[5] * y + m[6] * z;
    const float rz = m[8] * x + m[9] * y + m[10] * z;
    const float len2 = rx * rx + ry * ry + rz * rz;
    if (len2 > 1e-12f) {
        const float inv = 1.0f / std::sqrt(len2);
        out_d[0] = rx * inv;
        out_d[1] = ry * inv;
        out_d[2] = rz * inv;
    } else {
        out_d[0] = x;
        out_d[1] = y;
        out_d[2] = z;
    }
}

template <typename T>
void copy_stream_pod(const uint8_t* base_ptr, uint64_t offset, uint64_t byte_len, std::vector<T>& dst) {
    const size_t count = static_cast<size_t>(byte_len / sizeof(T));
    dst.resize(count);
    if (count > 0) {
        std::memcpy(dst.data(), base_ptr + offset, count * sizeof(T));
    }
}

VkFormat map_vk_format(uint32_t aether_fmt) noexcept {
    switch (aether_fmt) {
    case AETHER_TEX_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case AETHER_TEX_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case AETHER_TEX_FORMAT_BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case AETHER_TEX_FORMAT_BC7_SRGB:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool is_bc7_format(uint32_t aether_fmt) noexcept {
    return aether_fmt == AETHER_TEX_FORMAT_BC7_UNORM || aether_fmt == AETHER_TEX_FORMAT_BC7_SRGB;
}

} // namespace

void StagedAvatarUpload::release() noexcept {
    if (device != nullptr && staging_buf.handle != VK_NULL_HANDLE) {
        device->destroy_buffer(staging_buf);
    }
    device = nullptr;
}

void GpuAvatar::destroy() noexcept {
    if (device == nullptr) {
        return;
    }
    for (auto& img : textures) {
        device->destroy_image(img);
    }
    textures.clear();
    device->destroy_image(fallback_white);
    device->destroy_buffer(vertex25d_buf);
    device->destroy_buffer(surface_buf);
    device->destroy_buffer(skin_buf);
    device->destroy_buffer(index_buf);
    device->destroy_buffer(morph_delta_buf);
    device->destroy_buffer(bone_line_buf);
    for (uint32_t s = 0; s < kPoseSlotCount; ++s) {
        device->destroy_buffer(dynamic_vertex25d_bufs[s]);
        device->destroy_buffer(dynamic_bone_line_bufs[s]);
        slot_applied_pose_seq[s] = 0ULL;
    }
    bone_line_vertex_count = 0;
    device = nullptr;
}

void GpuAvatar::update_pose(uint32_t frame_slot, uint64_t pose_seq, const float (*bone_matrices)[12],
                            uint32_t bone_count, const float* expr_weights, uint32_t expr_count,
                            bool update_mesh_vertices, const animation::Live2DParameters* opt_live2d) noexcept {
    if (device == nullptr || frame_slot >= kPoseSlotCount || pose_seq == 0ULL || bone_matrices == nullptr ||
        bone_count == 0U || slot_applied_pose_seq[frame_slot] == pose_seq) {
        return;
    }

    // 1. Resolve per-joint skinning matrices and animated skeleton overlay (0 alloc)
    const size_t j_count = joints.size();
    for (size_t j = 0; j < j_count; ++j) {
        const int32_t hum = (j < joint_humanoid_source.size()) ? joint_humanoid_source[j] : -1;
        if (hum >= 0 && static_cast<uint32_t>(hum) < bone_count) {
            std::memcpy(joint_skin_matrices[j].data(), bone_matrices[hum], sizeof(float) * 12U);
        } else {
            joint_skin_matrices[j] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        }
    }

    // Task 4(c): Drive VRM eyebrow child bones (`LeftEyeBrow`, `RightEyeBrow`, `mayu`, etc.) when present
    if (!brow_bones.empty()) {
        float brow_l_y = 0.0f, brow_r_y = 0.0f;
        float brow_l_ang = 0.0f, brow_r_ang = 0.0f;
        if (opt_live2d != nullptr) {
            brow_l_y = std::clamp(opt_live2d->param_brow_l_y, -1.0f, 1.0f);
            brow_r_y = std::clamp(opt_live2d->param_brow_r_y, -1.0f, 1.0f);
            brow_l_ang = std::clamp(opt_live2d->param_brow_l_angle, -1.0f, 1.0f);
            brow_r_ang = std::clamp(opt_live2d->param_brow_r_angle, -1.0f, 1.0f);
        } else if (expr_weights != nullptr && expr_count > 45U) {
            brow_l_y = std::clamp(0.5f * expr_weights[43] + 0.5f * expr_weights[44] - expr_weights[41], -1.0f, 1.0f);
            brow_r_y = std::clamp(0.5f * expr_weights[43] + 0.5f * expr_weights[45] - expr_weights[42], -1.0f, 1.0f);
            brow_l_ang = std::clamp(expr_weights[43] - 0.5f * expr_weights[44] - 0.5f * expr_weights[41], -1.0f, 1.0f);
            brow_r_ang = std::clamp(expr_weights[43] - 0.5f * expr_weights[45] - 0.5f * expr_weights[42], -1.0f, 1.0f);
        }
        for (const BrowBoneBinding& bb : brow_bones) {
            const uint32_t j = bb.joint_index;
            if (j >= j_count) {
                continue;
            }
            const float by = (bb.side == 0U) ? brow_l_y : ((bb.side == 1U) ? brow_r_y : 0.5f * (brow_l_y + brow_r_y));
            const float ba =
                (bb.side == 0U) ? brow_l_ang : ((bb.side == 1U) ? brow_r_ang : 0.5f * (brow_l_ang + brow_r_ang));
            const float ty = by * 0.010f;
            const float sign = (bb.side == 1U) ? 1.0f : -1.0f;
            const float rz = ba * 0.14f * sign;
            const float c = std::cos(rz);
            const float s = std::sin(rz);
            const float px = joint_rest_world_pos[j][0];
            const float py = joint_rest_world_pos[j][1];
            const std::array<float, 12> delta{
                c,    -s,   0.0f, px - (c * px - s * py),
                s,    c,    0.0f, py + ty - (s * px + c * py),
                0.0f, 0.0f, 1.0f, 0.0f,
            };
            std::array<float, 12> composed{};
            mul_3x4(joint_skin_matrices[j], delta, composed);
            joint_skin_matrices[j] = composed;
        }
    }

    for (size_t j = 0; j < j_count; ++j) {
        transform_point_3x4(joint_skin_matrices[j], joint_rest_world_pos[j].data(), joint_anim_world_pos[j].data());
    }

    if (dynamic_bone_line_bufs[frame_slot].info.pMappedData != nullptr && !bone_line_scratch.empty()) {
        emit_bone_line_vertices(joints, joint_anim_world_pos, bone_line_scratch);
        const VkDeviceSize bone_bytes = static_cast<VkDeviceSize>(bone_line_scratch.size()) * sizeof(BoneLineVertex);
        std::memcpy(dynamic_bone_line_bufs[frame_slot].info.pMappedData, bone_line_scratch.data(),
                    static_cast<size_t>(bone_bytes));
        vmaFlushAllocation(device->allocator(), dynamic_bone_line_bufs[frame_slot].allocation, 0, VK_WHOLE_SIZE);
    }

    // 2. Apply sparse blendshapes + 8-weight linear blend skinning into host-visible preview VB (0 alloc)
    if (update_mesh_vertices && !rest_vertices.empty() && skinned_vertices_scratch.size() == rest_vertices.size() &&
        dynamic_vertex25d_bufs[frame_slot].info.pMappedData != nullptr) {
        std::memcpy(skinned_vertices_scratch.data(), rest_vertices.data(),
                    rest_vertices.size() * sizeof(AetherVertex25D));

        if (!morph_deltas.empty() && !morph_weights_scratch.empty() && expr_weights != nullptr && expr_count > 0U) {
            std::fill(morph_weights_scratch.begin(), morph_weights_scratch.end(), 0.0f);
            for (const AetherExpressionBindingDesc& b : expressions) {
                if (b.input_slot < expr_count && b.morph_index < morph_weights_scratch.size()) {
                    morph_weights_scratch[b.morph_index] += expr_weights[b.input_slot] * b.weight;
                }
            }
            const size_t v_count = skinned_vertices_scratch.size();
            for (size_t mi = 0; mi < morph_ranges.size(); ++mi) {
                // VRM morphs are authored for [0, 1]; overlapping presets must never overdrive a shape past 100%.
                const float w = std::clamp(morph_weights_scratch[mi], 0.0f, 1.0f);
                if (w <= 0.002f) {
                    continue;
                }
                const AetherMorphRangeDesc& mr = morph_ranges[mi];
                const size_t start = mr.delta_offset;
                const size_t end = std::min(start + mr.delta_count, morph_deltas.size());
                for (size_t di = start; di < end; ++di) {
                    const AetherMorphDelta& d = morph_deltas[di];
                    if (d.vertex_index < v_count) {
                        AetherVertex25D& vtx = skinned_vertices_scratch[d.vertex_index];
                        vtx.position[0] += d.d_position[0] * w;
                        vtx.position[1] += d.d_position[1] * w;
                        vtx.position[2] += d.d_position[2] * w;
                        vtx.true_normal[0] += d.d_normal[0] * w;
                        vtx.true_normal[1] += d.d_normal[1] * w;
                        vtx.true_normal[2] += d.d_normal[2] * w;
                    }
                }
            }
        }

        const size_t v_count = skinned_vertices_scratch.size();
        const bool has_skin = (skin_vertices.size() == v_count && !joint_skin_matrices.empty());
        if (has_skin) {
            constexpr float kInvUnorm16 = 1.0f / 65535.0f;
            const size_t max_j = joint_skin_matrices.size();
            for (size_t vi = 0; vi < v_count; ++vi) {
                const AetherVertexSkin& sk = skin_vertices[vi];
                if (sk.weights_unorm16[0] == 0U || sk.joints[0] >= max_j) {
                    continue;
                }
                AetherVertex25D& vtx = skinned_vertices_scratch[vi];
                std::array<float, 12> blended{};
                if (sk.weights_unorm16[0] == 65535U || sk.weights_unorm16[1] == 0U) {
                    blended = joint_skin_matrices[sk.joints[0]];
                } else {
                    float total_w = 0.0f;
                    for (size_t k = 0; k < 8U; ++k) {
                        const uint16_t wu = sk.weights_unorm16[k];
                        if (wu == 0U) {
                            break;
                        }
                        const uint16_t ji = sk.joints[k];
                        if (ji >= max_j) {
                            continue;
                        }
                        const float wf = static_cast<float>(wu) * kInvUnorm16;
                        total_w += wf;
                        const auto& jm = joint_skin_matrices[ji];
                        for (size_t c = 0; c < 12U; ++c) {
                            blended[c] += jm[c] * wf;
                        }
                    }
                    if (total_w > 1e-4f) {
                        const float inv_w = 1.0f / total_w;
                        for (float& c : blended) {
                            c *= inv_w;
                        }
                    } else {
                        blended = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
                    }
                }
                const float in_pos[3] = {vtx.position[0], vtx.position[1], vtx.position[2]};
                const float in_tn[3] = {vtx.true_normal[0], vtx.true_normal[1], vtx.true_normal[2]};
                const float in_pn[3] = {vtx.proxy_normal[0], vtx.proxy_normal[1], vtx.proxy_normal[2]};
                transform_point_3x4(blended, in_pos, vtx.position);
                transform_dir_3x4(blended, in_tn, vtx.true_normal);
                transform_dir_3x4(blended, in_pn, vtx.proxy_normal);
            }
        }

        const VkDeviceSize v_bytes = static_cast<VkDeviceSize>(v_count) * sizeof(AetherVertex25D);
        std::memcpy(dynamic_vertex25d_bufs[frame_slot].info.pMappedData, skinned_vertices_scratch.data(),
                    static_cast<size_t>(v_bytes));
        vmaFlushAllocation(device->allocator(), dynamic_vertex25d_bufs[frame_slot].allocation, 0, VK_WHOLE_SIZE);
    }

    slot_applied_pose_seq[frame_slot] = pose_seq;
}

std::string GpuAvatar::string_slice(uint32_t offset, uint32_t len) const {
    if (offset >= strings.size() || len == 0) {
        return {};
    }
    const size_t safe_len = std::min<size_t>(len, strings.size() - offset);
    return strings.substr(offset, safe_len);
}

uint32_t GpuAvatar::mapped_humanoid_count() const noexcept {
    uint32_t mapped = 0;
    for (uint16_t j : desc.humanoid_to_joint) {
        if (j != 0xFFFFU) {
            ++mapped;
        }
    }
    return mapped;
}

Expected<std::unique_ptr<GpuAvatar>> upload_staged_avatar(rhi::RhiDevice& rhi,
                                                          std::unique_ptr<StagedAvatarUpload> staged) {
    if (!staged || staged->staging_buf.handle == VK_NULL_HANDLE || staged->staging_buf.info.pMappedData == nullptr) {
        return fail(ErrorCode::InvalidArg, "Invalid StagedAvatarUpload payload");
    }

    const uint64_t upload_t0 = now_us();
    const auto* mapped = static_cast<const uint8_t*>(staged->staging_buf.info.pMappedData);

    auto avatar = std::make_unique<GpuAvatar>();
    avatar->device = &rhi;
    avatar->requirements = staged->requirements;
    avatar->desc = staged->desc;
    avatar->source_path = std::move(staged->source_path);
    avatar->stats.open_query_ms = staged->open_query_ms;
    avatar->stats.fill_ms = staged->fill_ms;
    avatar->stats.staging_bytes = staged->staging_buf.size;
    avatar->stats.pointer_identity_verified = staged->pointer_identity_verified;

    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_MORPH_RANGES],
                    staged->requirements.stream_bytes[AETHER_STREAM_MORPH_RANGES], avatar->morph_ranges);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_SUBMESHES],
                    staged->requirements.stream_bytes[AETHER_STREAM_SUBMESHES], avatar->submeshes);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_MATERIALS],
                    staged->requirements.stream_bytes[AETHER_STREAM_MATERIALS], avatar->materials);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_JOINTS],
                    staged->requirements.stream_bytes[AETHER_STREAM_JOINTS], avatar->joints);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_SPRINGS],
                    staged->requirements.stream_bytes[AETHER_STREAM_SPRINGS], avatar->springs);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_COLLIDERS],
                    staged->requirements.stream_bytes[AETHER_STREAM_COLLIDERS], avatar->colliders);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_EXPRESSIONS],
                    staged->requirements.stream_bytes[AETHER_STREAM_EXPRESSIONS], avatar->expressions);
    copy_stream_pod(mapped, staged->stream_offsets[AETHER_STREAM_TEXTURE_DESCS],
                    staged->requirements.stream_bytes[AETHER_STREAM_TEXTURE_DESCS], avatar->texture_descs);

    const uint64_t str_bytes = staged->requirements.stream_bytes[AETHER_STREAM_STRINGS];
    if (str_bytes > 0) {
        avatar->strings.assign(reinterpret_cast<const char*>(mapped + staged->stream_offsets[AETHER_STREAM_STRINGS]),
                               static_cast<size_t>(str_bytes));
    }

    // Populate fallback 1x1 white RGBA8 texel in the reserved staging tail.
    std::array<uint8_t, 4> white_px{255U, 255U, 255U, 255U};
    std::memcpy(static_cast<uint8_t*>(staged->staging_buf.info.pMappedData) + staged->fallback_white_offset,
                white_px.data(), white_px.size());

    vmaFlushAllocation(rhi.allocator(), staged->staging_buf.allocation, 0, VK_WHOLE_SIZE);

    auto cleanup_staged = [&staged]() {
        if (staged) {
            staged->release();
        }
    };

    const uint64_t v25_bytes = staged->requirements.stream_bytes[AETHER_STREAM_VERTEX25D];
    const uint64_t surf_bytes = staged->requirements.stream_bytes[AETHER_STREAM_SURFACE];
    const uint64_t skin_bytes = staged->requirements.stream_bytes[AETHER_STREAM_SKIN];
    const uint64_t idx_bytes = staged->requirements.stream_bytes[AETHER_STREAM_INDEX_U32];
    const uint64_t morph_bytes = staged->requirements.stream_bytes[AETHER_STREAM_MORPH_DELTAS];

    auto assign_stream_pod = [&](uint64_t off, uint64_t bytes, auto& out) {
        using Elem = std::decay_t<decltype(out)>::value_type;
        if (bytes >= sizeof(Elem)) {
            const auto* src = reinterpret_cast<const Elem*>(mapped + off);
            out.assign(src, src + (bytes / sizeof(Elem)));
        }
    };
    assign_stream_pod(staged->stream_offsets[AETHER_STREAM_VERTEX25D], v25_bytes, avatar->rest_vertices);
    assign_stream_pod(staged->stream_offsets[AETHER_STREAM_INDEX_U32], idx_bytes, avatar->rest_indices);
    assign_stream_pod(staged->stream_offsets[AETHER_STREAM_SKIN], skin_bytes, avatar->skin_vertices);
    assign_stream_pod(staged->stream_offsets[AETHER_STREAM_MORPH_DELTAS], morph_bytes, avatar->morph_deltas);
    avatar->skinned_vertices_scratch.assign(avatar->rest_vertices.begin(), avatar->rest_vertices.end());
    avatar->morph_weights_scratch.assign(avatar->morph_ranges.size(), 0.0f);

    const VkBufferUsageFlags rt_bda_flags =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        (rhi.rt_supported() ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0U);

    if (v25_bytes > 0) {
        auto res = rhi.create_buffer(v25_bytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!res) {
            cleanup_staged();
            return make_unexpected(res.error());
        }
        avatar->vertex25d_buf = res.value();

        for (uint32_t s = 0; s < GpuAvatar::kPoseSlotCount; ++s) {
            auto dyn_res = rhi.create_buffer(v25_bytes,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             VMA_MEMORY_USAGE_AUTO,
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
            if (!dyn_res) {
                cleanup_staged();
                return make_unexpected(dyn_res.error());
            }
            avatar->dynamic_vertex25d_bufs[s] = dyn_res.value();
            if (avatar->dynamic_vertex25d_bufs[s].info.pMappedData != nullptr && !avatar->rest_vertices.empty()) {
                std::memcpy(avatar->dynamic_vertex25d_bufs[s].info.pMappedData, avatar->rest_vertices.data(),
                            static_cast<size_t>(v25_bytes));
                vmaFlushAllocation(rhi.allocator(), avatar->dynamic_vertex25d_bufs[s].allocation, 0, VK_WHOLE_SIZE);
            }
        }
    }
    if (surf_bytes > 0) {
        auto res = rhi.create_buffer(surf_bytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!res) {
            cleanup_staged();
            return make_unexpected(res.error());
        }
        avatar->surface_buf = res.value();
    }
    if (skin_bytes > 0) {
        auto res = rhi.create_buffer(skin_bytes,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!res) {
            cleanup_staged();
            return make_unexpected(res.error());
        }
        avatar->skin_buf = res.value();
    }
    if (idx_bytes > 0) {
        auto res = rhi.create_buffer(idx_bytes,
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_bda_flags,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!res) {
            cleanup_staged();
            return make_unexpected(res.error());
        }
        avatar->index_buf = res.value();
    }
    if (morph_bytes > 0) {
        auto res = rhi.create_buffer(morph_bytes,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        if (!res) {
            cleanup_staged();
            return make_unexpected(res.error());
        }
        avatar->morph_delta_buf = res.value();
    }

    // Build static + double-buffered live skeleton overlay buffers (tiny, zero-sync-hazard).
    compute_rest_joint_world_positions(avatar->joints, avatar->joint_rest_world_pos);
    avatar->joint_anim_world_pos = avatar->joint_rest_world_pos;
    build_joint_humanoid_sources(avatar->joints, avatar->desc, avatar->joint_humanoid_source);
    detect_brow_bone_bindings(avatar->joints, avatar->strings, avatar->joint_humanoid_source,
                              avatar->joint_rest_world_pos, avatar->brow_bones);
    avatar->has_split_brow_morphs = false;
    for (const AetherExpressionBindingDesc& eb : avatar->expressions) {
        if (eb.input_slot >= 41U && eb.input_slot <= 45U) {
            avatar->has_split_brow_morphs = true;
            break;
        }
    }
    avatar->joint_skin_matrices.assign(avatar->joints.size(),
                                       {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
    avatar->bone_line_scratch.resize(avatar->joints.size() * 4U);
    emit_bone_line_vertices(avatar->joints, avatar->joint_rest_world_pos, avatar->bone_line_scratch);
    if (!avatar->bone_line_scratch.empty()) {
        const VkDeviceSize bone_bytes =
            static_cast<VkDeviceSize>(avatar->bone_line_scratch.size()) * sizeof(BoneLineVertex);
        auto b_res = rhi.create_buffer(bone_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!b_res) {
            cleanup_staged();
            return make_unexpected(b_res.error());
        }
        avatar->bone_line_buf = b_res.value();
        std::memcpy(avatar->bone_line_buf.info.pMappedData, avatar->bone_line_scratch.data(),
                    static_cast<size_t>(bone_bytes));
        vmaFlushAllocation(rhi.allocator(), avatar->bone_line_buf.allocation, 0, VK_WHOLE_SIZE);
        avatar->bone_line_vertex_count = static_cast<uint32_t>(avatar->bone_line_scratch.size());

        for (uint32_t s = 0; s < GpuAvatar::kPoseSlotCount; ++s) {
            auto db_res = rhi.create_buffer(bone_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT);
            if (!db_res) {
                cleanup_staged();
                return make_unexpected(db_res.error());
            }
            avatar->dynamic_bone_line_bufs[s] = db_res.value();
            std::memcpy(avatar->dynamic_bone_line_bufs[s].info.pMappedData, avatar->bone_line_scratch.data(),
                        static_cast<size_t>(bone_bytes));
            vmaFlushAllocation(rhi.allocator(), avatar->dynamic_bone_line_bufs[s].allocation, 0, VK_WHOLE_SIZE);
        }
    }

    auto fw_res = rhi.create_image_2d(1, 1, VK_FORMAT_R8G8B8A8_SRGB,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fw_res) {
        cleanup_staged();
        return make_unexpected(fw_res.error());
    }
    avatar->fallback_white = fw_res.value();

    avatar->textures.reserve(avatar->texture_descs.size());
    for (const AetherTextureDesc& td : avatar->texture_descs) {
        if (is_bc7_format(td.format) && !rhi.gpu_info().texture_compression_bc) {
            cleanup_staged();
            return fail(ErrorCode::Unsupported, "BC7 texture uploaded on GPU lacking textureCompressionBC");
        }
        const VkFormat vk_fmt = map_vk_format(td.format);
        if (vk_fmt == VK_FORMAT_UNDEFINED) {
            cleanup_staged();
            return fail(ErrorCode::Unsupported, "Unsupported texture pixel format in AetherTextureDesc");
        }
        const uint32_t w = std::max(1U, td.width);
        const uint32_t h = std::max(1U, td.height);
        auto img_res = rhi.create_image_2d(w, h, vk_fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!img_res) {
            cleanup_staged();
            return make_unexpected(img_res.error());
        }
        avatar->textures.push_back(img_res.value());
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = rhi.graphics_family();
    cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(rhi.device(), &cpci, nullptr, &pool) != VK_SUCCESS) {
        cleanup_staged();
        return fail(ErrorCode::VulkanError, "vkCreateCommandPool(upload) failed");
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(rhi.device(), &cbai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(rhi.device(), pool, nullptr);
        cleanup_staged();
        return fail(ErrorCode::VulkanError, "vkAllocateCommandBuffers(upload) failed");
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(rhi.device(), &fci, nullptr, &fence) != VK_SUCCESS) {
        vkDestroyCommandPool(rhi.device(), pool, nullptr);
        cleanup_staged();
        return fail(ErrorCode::VulkanError, "vkCreateFence(upload) failed");
    }

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) {
        vkDestroyFence(rhi.device(), fence, nullptr);
        vkDestroyCommandPool(rhi.device(), pool, nullptr);
        cleanup_staged();
        return fail(ErrorCode::VulkanError, "vkBeginCommandBuffer(upload) failed");
    }

    const VkBuffer src_buf = staged->staging_buf.handle;
    auto record_copy = [&](const rhi::RhiBuffer& dst, uint32_t stream_idx, uint64_t bytes) {
        if (bytes == 0 || dst.handle == VK_NULL_HANDLE) {
            return;
        }
        VkBufferCopy region{};
        region.srcOffset = staged->stream_offsets[stream_idx];
        region.dstOffset = 0;
        region.size = bytes;
        vkCmdCopyBuffer(cmd, src_buf, dst.handle, 1, &region);
    };

    record_copy(avatar->vertex25d_buf, AETHER_STREAM_VERTEX25D, v25_bytes);
    record_copy(avatar->surface_buf, AETHER_STREAM_SURFACE, surf_bytes);
    record_copy(avatar->skin_buf, AETHER_STREAM_SKIN, skin_bytes);
    record_copy(avatar->index_buf, AETHER_STREAM_INDEX_U32, idx_bytes);
    record_copy(avatar->morph_delta_buf, AETHER_STREAM_MORPH_DELTAS, morph_bytes);

    // Transition fallback_white + all decoded avatar textures -> TRANSFER_DST_OPTIMAL in batch.
    rhi::RhiDevice::cmd_transition_image(cmd, avatar->fallback_white.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    for (const auto& img : avatar->textures) {
        rhi::RhiDevice::cmd_transition_image(cmd, img.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                             0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }

    VkBufferImageCopy fw_copy{};
    fw_copy.bufferOffset = staged->fallback_white_offset;
    fw_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    fw_copy.imageSubresource.layerCount = 1;
    fw_copy.imageExtent = VkExtent3D{1, 1, 1};
    vkCmdCopyBufferToImage(cmd, src_buf, avatar->fallback_white.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &fw_copy);

    const uint64_t tex_pixels_base = staged->stream_offsets[AETHER_STREAM_TEXTURE_PIXELS];
    for (size_t i = 0; i < avatar->texture_descs.size(); ++i) {
        const AetherTextureDesc& td = avatar->texture_descs[i];
        if (td.pixel_size == 0) {
            continue;
        }
        VkBufferImageCopy copy{};
        copy.bufferOffset = tex_pixels_base + td.pixel_offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = VkExtent3D{std::max(1U, td.width), std::max(1U, td.height), 1};
        vkCmdCopyBufferToImage(cmd, src_buf, avatar->textures[i].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
    }

    rhi::RhiDevice::cmd_transition_image(cmd, avatar->fallback_white.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    for (const auto& img : avatar->textures) {
        rhi::RhiDevice::cmd_transition_image(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // Memory barrier making buffer transfer writes visible to vertex/index/shader reads.
    VkMemoryBarrier2 buf_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    buf_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    buf_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    buf_barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    buf_barrier.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &buf_barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkDestroyFence(rhi.device(), fence, nullptr);
        vkDestroyCommandPool(rhi.device(), pool, nullptr);
        cleanup_staged();
        return fail(ErrorCode::VulkanError, "vkEndCommandBuffer(upload) failed");
    }

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    const VkResult sub_rc = vkQueueSubmit2(rhi.graphics_queue(), 1, &submit, fence);
    if (sub_rc == VK_SUCCESS) {
        vkWaitForFences(rhi.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    }

    vkDestroyFence(rhi.device(), fence, nullptr);
    vkDestroyCommandPool(rhi.device(), pool, nullptr);
    cleanup_staged();

    if (sub_rc != VK_SUCCESS) {
        return fail(ErrorCode::VulkanError, "vkQueueSubmit2(upload) failed");
    }

    avatar->stats.gpu_upload_ms = static_cast<double>(now_us() - upload_t0) / 1000.0;
    AETHER_LOG_INFO("renderer",
                    "GPU uploaded avatar (%u verts, %u tris, %zu tex, mapped %u/55) in %.2f ms (open+q=%.2f, "
                    "fill=%.2f)",
                    avatar->requirements.vertex_count, avatar->requirements.index_count / 3U, avatar->textures.size(),
                    avatar->mapped_humanoid_count(), avatar->stats.gpu_upload_ms, avatar->stats.open_query_ms,
                    avatar->stats.fill_ms);
    return avatar;
}

} // namespace aether::renderer
