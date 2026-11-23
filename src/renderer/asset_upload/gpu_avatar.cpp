#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/asset_upload/vrm_parity_prep.hpp"

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

void build_joint_spring_sources(const std::vector<AetherJointDesc>& joints,
                                const std::vector<AetherSpringJointDesc>& springs, std::vector<int32_t>& out_sources) {
    out_sources.assign(joints.size(), -1);
    constexpr uint32_t kCustomSpringBoneOffset = 64U;
    constexpr uint32_t kMaxCustomSprings = 160U;
    uint32_t dst_idx = 0U;
    for (const AetherSpringJointDesc& sp : springs) {
        if (dst_idx >= kMaxCustomSprings) {
            break;
        }
        if (sp.joint_index < joints.size()) {
            // Only tag the first spring descriptor referencing a given joint (matches SimEngine slot order)
            if (out_sources[sp.joint_index] < 0) {
                out_sources[sp.joint_index] = static_cast<int32_t>(kCustomSpringBoneOffset + dst_idx);
            }
            ++dst_idx;
        }
    }
    // Propagate spring parent ownership down to unsimulated terminal/accessory leaf joints
    for (size_t i = 0; i < joints.size(); ++i) {
        if (out_sources[i] >= 0 || joints[i].humanoid_bone < AETHER_HUMANOID_BONE_COUNT) {
            continue;
        }
        size_t cur = i;
        for (size_t hops = 0; hops < joints.size(); ++hops) {
            const int32_t p = joints[cur].parent_index;
            if (p < 0 || static_cast<size_t>(p) >= joints.size() || static_cast<size_t>(p) == cur) {
                break;
            }
            cur = static_cast<size_t>(p);
            if (joints[cur].humanoid_bone < AETHER_HUMANOID_BONE_COUNT) {
                break;
            }
            if (out_sources[cur] >= 0) {
                out_sources[i] = out_sources[cur];
                break;
            }
        }
    }
}

void build_deduped_expressions_and_morph_meta(const std::vector<AetherExpressionBindingDesc>& expressions,
                                              const std::vector<AetherMorphRangeDesc>& morph_ranges,
                                              const std::vector<AetherMorphDelta>& morph_deltas, size_t vertex_count,
                                              std::vector<AetherExpressionBindingDesc>& out_deduped,
                                              std::vector<GpuAvatar::MorphMeta>& out_meta) {
    out_deduped.clear();
    out_meta.assign(morph_ranges.size(), GpuAvatar::MorphMeta{});
    if (expressions.empty()) {
        return;
    }

    // Keep only the canonical (lowest preset_index) preset per emotion slot (>= 57) so Joy/Fun never stack.
    std::array<uint32_t, 64> min_preset_for_slot{};
    min_preset_for_slot.fill(UINT32_MAX);
    for (const AetherExpressionBindingDesc& b : expressions) {
        if (b.input_slot >= 57U && b.input_slot < min_preset_for_slot.size()) {
            min_preset_for_slot[b.input_slot] = std::min(min_preset_for_slot[b.input_slot], b.preset_index);
        }
    }

    out_deduped.reserve(expressions.size());
    for (const AetherExpressionBindingDesc& b : expressions) {
        if (b.input_slot >= 57U && b.input_slot < min_preset_for_slot.size() &&
            b.preset_index != min_preset_for_slot[b.input_slot]) {
            continue;
        }
        bool dup = false;
        for (const AetherExpressionBindingDesc& prev : out_deduped) {
            if (prev.input_slot == b.input_slot && prev.morph_index == b.morph_index) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out_deduped.push_back(b);
        }
    }

    std::vector<uint8_t> blink_verts(vertex_count, 0U);
    auto classify_slot = [](uint32_t slot) noexcept -> GpuAvatar::MorphRegion {
        if (slot <= 6U) {
            return GpuAvatar::MorphRegion::EyeL;
        }
        if (slot >= 7U && slot <= 13U) {
            return GpuAvatar::MorphRegion::EyeR;
        }
        if (slot >= 41U && slot <= 45U) {
            return GpuAvatar::MorphRegion::Brow;
        }
        if ((slot >= 14U && slot <= 37U) || (slot >= 52U && slot <= 56U)) {
            return GpuAvatar::MorphRegion::Mouth;
        }
        if (slot >= 57U) {
            return GpuAvatar::MorphRegion::Emotion;
        }
        return GpuAvatar::MorphRegion::Other;
    };

    for (const AetherExpressionBindingDesc& b : out_deduped) {
        if (b.morph_index >= out_meta.size()) {
            continue;
        }
        GpuAvatar::MorphMeta& mm = out_meta[b.morph_index];
        const GpuAvatar::MorphRegion reg = classify_slot(b.input_slot);
        if (b.input_slot == 0U || b.input_slot == 7U) {
            mm.is_blink = true;
            mm.region = reg;
            if (b.morph_index < morph_ranges.size() && vertex_count > 0U) {
                const AetherMorphRangeDesc& mr = morph_ranges[b.morph_index];
                const size_t start = mr.delta_offset;
                const size_t end = std::min(start + mr.delta_count, morph_deltas.size());
                for (size_t di = start; di < end; ++di) {
                    const AetherMorphDelta& d = morph_deltas[di];
                    const float d2 = d.d_position[0] * d.d_position[0] + d.d_position[1] * d.d_position[1] +
                                     d.d_position[2] * d.d_position[2];
                    if (d.vertex_index < vertex_count && d2 > 1e-6f) {
                        blink_verts[d.vertex_index] = 1U;
                    }
                }
            }
        } else if (mm.region == GpuAvatar::MorphRegion::Other ||
                   (mm.region == GpuAvatar::MorphRegion::Emotion && reg != GpuAvatar::MorphRegion::Other)) {
            mm.region = reg;
        }
    }

    if (vertex_count > 0U) {
        for (size_t mi = 0; mi < morph_ranges.size(); ++mi) {
            const AetherMorphRangeDesc& mr = morph_ranges[mi];
            const size_t start = mr.delta_offset;
            const size_t end = std::min(start + mr.delta_count, morph_deltas.size());
            for (size_t di = start; di < end; ++di) {
                const AetherMorphDelta& d = morph_deltas[di];
                const float d2 = d.d_position[0] * d.d_position[0] + d.d_position[1] * d.d_position[1] +
                                 d.d_position[2] * d.d_position[2];
                if (d.vertex_index < vertex_count && d2 > 1e-6f && blink_verts[d.vertex_index] != 0U) {
                    out_meta[mi].affects_eyelids = true;
                    break;
                }
            }
        }
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

float srgb_u8_to_lin(uint8_t u) noexcept {
    const float c = static_cast<float>(u) / 255.0f;
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::string to_lower_str(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch);
    }
    return out;
}

std::string read_cstr_from_pool(const std::string& pool, uint32_t offset) {
    if (offset >= pool.size()) {
        return {};
    }
    const char* p = pool.data() + offset;
    const size_t max_n = pool.size() - offset;
    size_t len = 0;
    while (len < max_n && p[len] != '\0') {
        ++len;
    }
    return std::string(p, len);
}

bool sample_avatar_texel_rgba8(const AetherTextureDesc& td, const std::vector<uint8_t>& cpu_pixels, uint32_t tx,
                               uint32_t ty, uint8_t out_rgba[4]) noexcept {
    if (td.width == 0U || td.height == 0U) {
        return false;
    }
    tx = std::min(tx, td.width - 1U);
    ty = std::min(ty, td.height - 1U);
    if (!is_bc7_format(td.format)) {
        const uint64_t expected_bytes = static_cast<uint64_t>(td.width) * static_cast<uint64_t>(td.height) * 4ULL;
        if (td.pixel_size < expected_bytes || td.pixel_offset + expected_bytes > cpu_pixels.size()) {
            return false;
        }
        const uint8_t* px = cpu_pixels.data() + td.pixel_offset + (static_cast<size_t>(ty) * td.width + tx) * 4U;
        out_rgba[0] = px[0];
        out_rgba[1] = px[1];
        out_rgba[2] = px[2];
        out_rgba[3] = px[3];
        return true;
    }

    // Decode BC7 Mode 6 block (produced by `bc7_cache.rs` for warm disk cache hits).
    const uint32_t bw = (td.width + 3U) / 4U;
    const uint32_t bh = (td.height + 3U) / 4U;
    const uint64_t expected_bytes = static_cast<uint64_t>(bw) * static_cast<uint64_t>(bh) * 16ULL;
    if (td.pixel_size < expected_bytes || td.pixel_offset + expected_bytes > cpu_pixels.size()) {
        return false;
    }
    const size_t block_idx = static_cast<size_t>(ty / 4U) * bw + (tx / 4U);
    const uint8_t* blk = cpu_pixels.data() + td.pixel_offset + block_idx * 16U;
    uint64_t lo = 0ULL;
    uint64_t hi = 0ULL;
    std::memcpy(&lo, blk, 8U);
    std::memcpy(&hi, blk + 8U, 8U);
    if ((lo & 0x7FULL) != 0x40ULL) {
        return false; // Non-Mode-6 block
    }
    const uint32_t p0 = static_cast<uint32_t>((lo >> 63U) & 1ULL);
    const uint32_t p1 = static_cast<uint32_t>(hi & 1ULL);
    const uint32_t pi = (ty & 3U) * 4U + (tx & 3U);
    const uint32_t idx = (pi == 0U) ? static_cast<uint32_t>((hi >> 1U) & 0x7ULL)
                                    : static_cast<uint32_t>((hi >> (4U + (pi - 1U) * 4U)) & 0xFULL);
    static constexpr uint32_t kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    const uint32_t w = kWeights[idx & 15U];
    for (uint32_t ch = 0U; ch < 4U; ++ch) {
        const uint32_t q0 = static_cast<uint32_t>((lo >> (7U + ch * 14U)) & 0x7FULL);
        const uint32_t q1 = static_cast<uint32_t>((lo >> (14U + ch * 14U)) & 0x7FULL);
        const uint32_t c0 = (q0 << 1U) | p0;
        const uint32_t c1 = (q1 << 1U) | p1;
        out_rgba[ch] = static_cast<uint8_t>(((64U - w) * c0 + w * c1 + 32U) >> 6U);
    }
    return true;
}

/**
 * Samples the avatar's lower-cheek / chin `Face_00_SKIN` linear RGB once at upload time
 * so the upper-neck baked-shadow normalizer matches ANY avatar skin tone automatically.
 */
void compute_face_skin_reference(GpuAvatar& avatar) {
    avatar.face_skin_ref_linear = {0.85f, 0.68f, 0.58f, 0.0f};
    if (avatar.submeshes.empty() || avatar.materials.empty() || avatar.rest_vertices.empty() ||
        avatar.rest_surfaces.size() < avatar.rest_vertices.size() || avatar.cpu_texture_pixels.empty()) {
        return;
    }

    float head_x = 0.0f;
    float head_y = avatar.desc.aabb_min[1] + (avatar.desc.aabb_max[1] - avatar.desc.aabb_min[1]) * 0.88f;
    float neck_y = head_y - 0.07f;
    const uint16_t head_ji = avatar.desc.humanoid_to_joint[AETHER_BONE_HEAD];
    const uint16_t neck_ji = avatar.desc.humanoid_to_joint[AETHER_BONE_NECK];
    if (head_ji != 0xFFFFU && head_ji < avatar.joint_rest_world_pos.size()) {
        head_x = avatar.joint_rest_world_pos[head_ji][0];
        head_y = avatar.joint_rest_world_pos[head_ji][1];
    }
    if (neck_ji != 0xFFFFU && neck_ji < avatar.joint_rest_world_pos.size()) {
        neck_y = avatar.joint_rest_world_pos[neck_ji][1];
    }
    const float neck_len = std::max(head_y - neck_y, 0.045f);
    const float y_min = neck_y - 0.15f * neck_len;
    const float y_max = head_y + 0.55f * neck_len;

    struct SkinSample {
        float r;
        float g;
        float b;
        float luma;
    };
    std::vector<SkinSample> samples;
    samples.reserve(512);

    auto collect_from_submesh = [&](const AetherSubmeshDesc& sm, bool strict_chin) {
        if (sm.material_index >= avatar.materials.size()) {
            return;
        }
        const AetherNprMaterial& mat = avatar.materials[sm.material_index];
        if (mat.tex_base >= avatar.texture_descs.size()) {
            return;
        }
        const AetherTextureDesc& td = avatar.texture_descs[mat.tex_base];
        if (td.width == 0U || td.height == 0U) {
            return;
        }
        const bool is_srgb = (td.format == AETHER_TEX_FORMAT_RGBA8_SRGB || td.format == AETHER_TEX_FORMAT_BC7_SRGB);
        const size_t idx_end =
            std::min<size_t>(static_cast<size_t>(sm.index_offset) + sm.index_count, avatar.rest_indices.size());
        const size_t stride = std::max<size_t>(1U, (idx_end - sm.index_offset) / 2400U);
        for (size_t ii = sm.index_offset; ii < idx_end; ii += stride) {
            const uint32_t vi = avatar.rest_indices[ii];
            if (vi >= avatar.rest_vertices.size()) {
                continue;
            }
            const AetherVertex25D& v = avatar.rest_vertices[vi];
            if (strict_chin) {
                if (v.position[1] < y_min || v.position[1] > y_max) {
                    continue;
                }
                if (std::abs(v.position[0] - head_x) > 0.075f) {
                    continue;
                }
                if (v.true_normal[2] < 0.25f) {
                    continue;
                }
            }
            const AetherVertexSurface& s = avatar.rest_surfaces[vi];
            const float u = std::clamp(s.uv0[0], 0.0f, 1.0f);
            const float v_coord = std::clamp(s.uv0[1], 0.0f, 1.0f);
            const uint32_t tx =
                std::min<uint32_t>(td.width - 1U, static_cast<uint32_t>(u * static_cast<float>(td.width)));
            const uint32_t ty =
                std::min<uint32_t>(td.height - 1U, static_cast<uint32_t>(v_coord * static_cast<float>(td.height)));
            uint8_t px[4]{};
            if (!sample_avatar_texel_rgba8(td, avatar.cpu_texture_pixels, tx, ty, px)) {
                continue;
            }
            if (px[3] < 180U) {
                continue;
            }
            const float r =
                (is_srgb ? srgb_u8_to_lin(px[0]) : (static_cast<float>(px[0]) / 255.0f)) * mat.base_color[0];
            const float g =
                (is_srgb ? srgb_u8_to_lin(px[1]) : (static_cast<float>(px[1]) / 255.0f)) * mat.base_color[1];
            const float b =
                (is_srgb ? srgb_u8_to_lin(px[2]) : (static_cast<float>(px[2]) / 255.0f)) * mat.base_color[2];
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum < 0.04f) {
                continue;
            }
            samples.push_back({r, g, b, lum});
        }
    };

    auto is_face_skin_submesh = [&](const AetherSubmeshDesc& sm) -> bool {
        if (sm.material_index >= avatar.materials.size()) {
            return false;
        }
        const std::string m_low =
            to_lower_str(read_cstr_from_pool(avatar.strings, avatar.materials[sm.material_index].name_offset));
        const std::string s_low = to_lower_str(avatar.string_slice(sm.name_offset, sm.name_len));
        auto has_bad = [](const std::string& t) {
            return t.find("eye") != std::string::npos || t.find("mouth") != std::string::npos ||
                   t.find("brow") != std::string::npos || t.find("lash") != std::string::npos ||
                   t.find("highlight") != std::string::npos || t.find("sclera") != std::string::npos ||
                   t.find("tooth") != std::string::npos || t.find("tongue") != std::string::npos ||
                   t.find("hair") != std::string::npos;
        };
        if (has_bad(m_low) || has_bad(s_low)) {
            return false;
        }
        const bool named_face_skin =
            (m_low.find("face") != std::string::npos && m_low.find("skin") != std::string::npos);
        const bool flagged_face = ((sm.flags & AETHER_SUBMESH_FLAG_FACE) != 0U) &&
                                  (m_low.find("face") != std::string::npos || s_low.find("face") != std::string::npos ||
                                   m_low.find("skin") != std::string::npos);
        return named_face_skin || flagged_face;
    };

    for (const auto& sm : avatar.submeshes) {
        if (is_face_skin_submesh(sm)) {
            collect_from_submesh(sm, true);
        }
    }
    if (samples.size() < 16U) {
        for (const auto& sm : avatar.submeshes) {
            if (is_face_skin_submesh(sm)) {
                collect_from_submesh(sm, false);
            }
        }
    }
    if (samples.empty()) {
        return;
    }

    std::sort(samples.begin(), samples.end(), [](const SkinSample& a, const SkinSample& b) { return a.luma < b.luma; });
    // Trimmed mean over the 45th..70th luminance percentile avoids dark lip lines & bright stickers.
    const size_t lo = (samples.size() * 45U) / 100U;
    const size_t hi = std::max(lo + 1U, (samples.size() * 70U) / 100U);
    double sr = 0.0;
    double sg = 0.0;
    double sb = 0.0;
    for (size_t i = lo; i < hi && i < samples.size(); ++i) {
        sr += samples[i].r;
        sg += samples[i].g;
        sb += samples[i].b;
    }
    const double denom = static_cast<double>(std::min(hi, samples.size()) - lo);
    avatar.face_skin_ref_linear = {
        static_cast<float>(sr / denom),
        static_cast<float>(sg / denom),
        static_cast<float>(sb / denom),
        1.0f,
    };
}

} // namespace

StagedAvatarUpload::~StagedAvatarUpload() noexcept = default;

void StagedAvatarUpload::release() noexcept {
    prebuilt_avatar.reset();
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
    device->destroy_buffer(pmx_sdef_buf);
    for (uint32_t s = 0; s < kPoseSlotCount; ++s) {
        device->destroy_buffer(dynamic_vertex25d_bufs[s]);
        device->destroy_buffer(dynamic_bone_line_bufs[s]);
        device->destroy_buffer(dynamic_uv_delta_bufs[s]);
        device->destroy_buffer(dynamic_joint_mat_bufs[s]);
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
        const int32_t spr = (j < joint_spring_source.size()) ? joint_spring_source[j] : -1;
        const int32_t hum = (j < joint_humanoid_source.size()) ? joint_humanoid_source[j] : -1;
        const int32_t src = (spr >= 0 && static_cast<uint32_t>(spr) < bone_count) ? spr : hum;
        if (src >= 0 && static_cast<uint32_t>(src) < bone_count) {
            std::memcpy(joint_skin_matrices[j].data(), bone_matrices[src], sizeof(float) * 12U);
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
                c, -s, 0.0f, px - (c * px - s * py), s, c, 0.0f, py + ty - (s * px + c * py), 0.0f, 0.0f, 1.0f, 0.0f,
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
            const auto& active_exprs = deduped_expressions.empty() ? expressions : deduped_expressions;
            for (const AetherExpressionBindingDesc& b : active_exprs) {
                if (b.input_slot < expr_count && b.morph_index < morph_weights_scratch.size()) {
                    morph_weights_scratch[b.morph_index] += expr_weights[b.input_slot] * b.weight;
                }
            }
            for (float& mw : morph_weights_scratch) {
                mw = std::clamp(mw, 0.0f, 1.0f);
            }
            if (morph_meta.size() == morph_ranges.size()) {
                const float blink_l = std::clamp(expr_weights[0], 0.0f, 1.0f);
                const float blink_r = (expr_count > 7U) ? std::clamp(expr_weights[7], 0.0f, 1.0f) : blink_l;
                const float eyelid_guard = 1.0f - std::max(blink_l, blink_r);
                float eye_l_non_blink = 0.0f;
                float eye_r_non_blink = 0.0f;
                float eye_l_blink = 0.0f;
                float eye_r_blink = 0.0f;
                float brow_sum = 0.0f;
                float mouth_sum = 0.0f;

                for (size_t mi = 0; mi < morph_ranges.size(); ++mi) {
                    float& w = morph_weights_scratch[mi];
                    const MorphMeta& mm = morph_meta[mi];
                    const bool is_eye_brow_emotion = !mm.is_blink && mm.affects_eyelids &&
                                                     mm.region != MorphRegion::Mouth && mm.region != MorphRegion::Other;
                    if (is_eye_brow_emotion) {
                        w *= eyelid_guard;
                    }
                    switch (mm.region) {
                    case MorphRegion::EyeL:
                        if (mm.is_blink) {
                            eye_l_blink += w;
                        } else {
                            eye_l_non_blink += w;
                        }
                        break;
                    case MorphRegion::EyeR:
                        if (mm.is_blink) {
                            eye_r_blink += w;
                        } else {
                            eye_r_non_blink += w;
                        }
                        break;
                    case MorphRegion::Brow:
                        brow_sum += w;
                        break;
                    case MorphRegion::Mouth:
                        mouth_sum += w;
                        break;
                    case MorphRegion::Emotion:
                    case MorphRegion::Other:
                        break;
                    }
                }

                const float eye_l_budget = std::max(0.0f, 1.0f - std::min(eye_l_blink, 1.0f));
                const float eye_r_budget = std::max(0.0f, 1.0f - std::min(eye_r_blink, 1.0f));
                const float eye_l_scale = (eye_l_non_blink > eye_l_budget && eye_l_non_blink > 1e-5f)
                                              ? (eye_l_budget / eye_l_non_blink)
                                              : 1.0f;
                const float eye_r_scale = (eye_r_non_blink > eye_r_budget && eye_r_non_blink > 1e-5f)
                                              ? (eye_r_budget / eye_r_non_blink)
                                              : 1.0f;
                const float blink_l_scale = (eye_l_blink > 1.0f) ? (1.0f / eye_l_blink) : 1.0f;
                const float blink_r_scale = (eye_r_blink > 1.0f) ? (1.0f / eye_r_blink) : 1.0f;
                const float brow_scale = (brow_sum > 0.75f) ? (0.75f / brow_sum) : 1.0f;
                const float mouth_scale = (mouth_sum > 1.0f) ? (1.0f / mouth_sum) : 1.0f;

                float non_blink_eyelid_sum = 0.0f;
                for (size_t mi = 0; mi < morph_ranges.size(); ++mi) {
                    float& w = morph_weights_scratch[mi];
                    const MorphMeta& mm = morph_meta[mi];
                    switch (mm.region) {
                    case MorphRegion::EyeL:
                        w *= mm.is_blink ? blink_l_scale : eye_l_scale;
                        break;
                    case MorphRegion::EyeR:
                        w *= mm.is_blink ? blink_r_scale : eye_r_scale;
                        break;
                    case MorphRegion::Brow:
                        w *= brow_scale;
                        break;
                    case MorphRegion::Mouth:
                        w *= mouth_scale;
                        break;
                    case MorphRegion::Emotion:
                    case MorphRegion::Other:
                        break;
                    }
                    if (!mm.is_blink && mm.affects_eyelids && mm.region != MorphRegion::Mouth &&
                        mm.region != MorphRegion::Other) {
                        non_blink_eyelid_sum += w;
                    }
                }
                if (non_blink_eyelid_sum > eyelid_guard && non_blink_eyelid_sum > 1e-5f) {
                    const float extra = eyelid_guard / non_blink_eyelid_sum;
                    for (size_t mi = 0; mi < morph_ranges.size(); ++mi) {
                        const MorphMeta& mm = morph_meta[mi];
                        if (!mm.is_blink && mm.affects_eyelids && mm.region != MorphRegion::Mouth &&
                            mm.region != MorphRegion::Other) {
                            morph_weights_scratch[mi] *= extra;
                        }
                    }
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

        // Phase 3: Evaluate PMX Group, UV, Material, and Bone morphs + step 60 Hz PMX skeleton/physics solver
        if (pmx_aux_header.is_pmx != 0U) {
            if (!pmx_channel_weights_scratch.empty()) {
                std::fill(pmx_channel_weights_scratch.begin(), pmx_channel_weights_scratch.end(), 0.0f);
                for (size_t ci = 0; ci < pmx_morph_channels.size() && ci < pmx_channel_weights_scratch.size(); ++ci) {
                    const auto& ch = pmx_morph_channels[ci];
                    float w = 0.0f;
                    if (ch.vertex_morph_index != AETHER_INVALID_INDEX &&
                        static_cast<size_t>(ch.vertex_morph_index) < morph_weights_scratch.size()) {
                        w = morph_weights_scratch[ch.vertex_morph_index];
                    } else if (expr_weights != nullptr && ch.appendix_b_slot < expr_count) {
                        w = std::clamp(expr_weights[ch.appendix_b_slot], 0.0f, 1.0f);
                    }
                    pmx_channel_weights_scratch[ci] = w;
                }
                for (int pass = 0; pass < 2; ++pass) {
                    for (size_t ci = 0; ci < pmx_morph_channels.size(); ++ci) {
                        const auto& ch = pmx_morph_channels[ci];
                        if (ch.morph_kind != AETHER_PMX_MORPH_GROUP) {
                            continue;
                        }
                        const float gw = pmx_channel_weights_scratch[ci];
                        if (std::abs(gw) < 1e-4f) {
                            continue;
                        }
                        const uint32_t end = std::min<uint32_t>(ch.child_offset + ch.child_count,
                                                                static_cast<uint32_t>(pmx_group_children.size()));
                        for (uint32_t gi = ch.child_offset; gi < end; ++gi) {
                            const auto& child = pmx_group_children[gi];
                            if (static_cast<size_t>(child.child_morph_index) < pmx_channel_weights_scratch.size() &&
                                static_cast<size_t>(child.child_morph_index) != ci) {
                                pmx_channel_weights_scratch[static_cast<size_t>(child.child_morph_index)] = std::clamp(
                                    pmx_channel_weights_scratch[static_cast<size_t>(child.child_morph_index)] +
                                        gw * child.weight,
                                    0.0f, 1.0f);
                            }
                        }
                    }
                }

                if (!uv_deltas_scratch.empty()) {
                    for (auto& uv : uv_deltas_scratch) {
                        uv[0] = 0.0f;
                        uv[1] = 0.0f;
                    }
                    for (size_t ci = 0; ci < pmx_morph_channels.size(); ++ci) {
                        const auto& ch = pmx_morph_channels[ci];
                        if (ch.morph_kind != AETHER_PMX_MORPH_UV) {
                            continue;
                        }
                        const float w = pmx_channel_weights_scratch[ci];
                        if (std::abs(w) < 1e-4f) {
                            continue;
                        }
                        const uint32_t end =
                            std::min<uint32_t>(ch.uv_offset + ch.uv_count, static_cast<uint32_t>(pmx_uv_morphs.size()));
                        for (uint32_t ui = ch.uv_offset; ui < end; ++ui) {
                            const auto& ud = pmx_uv_morphs[ui];
                            if (ud.vertex_index < uv_deltas_scratch.size()) {
                                uv_deltas_scratch[ud.vertex_index][0] += ud.duv[0] * w;
                                uv_deltas_scratch[ud.vertex_index][1] += ud.duv[1] * w;
                            }
                        }
                    }
                    if (dynamic_uv_delta_bufs[frame_slot].info.pMappedData != nullptr) {
                        const size_t uv_bytes = uv_deltas_scratch.size() * sizeof(float) * 2U;
                        std::memcpy(dynamic_uv_delta_bufs[frame_slot].info.pMappedData, uv_deltas_scratch.data(),
                                    uv_bytes);
                        vmaFlushAllocation(device->allocator(), dynamic_uv_delta_bufs[frame_slot].allocation, 0,
                                           VK_WHOLE_SIZE);
                    }
                }

                if (morphed_materials_scratch.size() == materials.size() && !materials.empty()) {
                    std::memcpy(morphed_materials_scratch.data(), materials.data(),
                                materials.size() * sizeof(AetherNprMaterial));
                    for (size_t ci = 0; ci < pmx_morph_channels.size(); ++ci) {
                        const auto& ch = pmx_morph_channels[ci];
                        if (ch.morph_kind != AETHER_PMX_MORPH_MATERIAL) {
                            continue;
                        }
                        const float w = pmx_channel_weights_scratch[ci];
                        if (std::abs(w) < 1e-4f) {
                            continue;
                        }
                        const uint32_t end = std::min<uint32_t>(ch.mat_offset + ch.mat_count,
                                                                static_cast<uint32_t>(pmx_mat_morphs.size()));
                        for (uint32_t mi = ch.mat_offset; mi < end; ++mi) {
                            const auto& md = pmx_mat_morphs[mi];
                            const size_t mat_lo = (md.material_index < 0) ? 0U : static_cast<size_t>(md.material_index);
                            const size_t mat_hi = (md.material_index < 0)
                                                      ? morphed_materials_scratch.size()
                                                      : std::min(mat_lo + 1U, morphed_materials_scratch.size());
                            for (size_t ti = mat_lo; ti < mat_hi; ++ti) {
                                auto& dst = morphed_materials_scratch[ti];
                                if (md.op_mode == 0U) {
                                    for (int c = 0; c < 4; ++c) {
                                        dst.base_color[c] *= (1.0f + (md.diffuse[c] - 1.0f) * w);
                                    }
                                    for (int c = 0; c < 3; ++c) {
                                        dst.shade_color_gi[c] *= (1.0f + (md.toon_tint[c] - 1.0f) * w);
                                        dst.emissive_and_pad[c] *= (1.0f + (md.ambient[c] - 1.0f) * w);
                                        dst.outline_color_mix[c] *= (1.0f + (md.edge_color[c] - 1.0f) * w);
                                    }
                                    dst.outline_width *= (1.0f + (md.edge_size - 1.0f) * w);
                                } else {
                                    for (int c = 0; c < 4; ++c) {
                                        dst.base_color[c] =
                                            std::clamp(dst.base_color[c] + md.diffuse[c] * w, 0.0f, 4.0f);
                                    }
                                    for (int c = 0; c < 3; ++c) {
                                        dst.shade_color_gi[c] =
                                            std::clamp(dst.shade_color_gi[c] + md.toon_tint[c] * w, 0.0f, 4.0f);
                                        dst.emissive_and_pad[c] =
                                            std::clamp(dst.emissive_and_pad[c] + md.ambient[c] * w, 0.0f, 8.0f);
                                        dst.outline_color_mix[c] =
                                            std::clamp(dst.outline_color_mix[c] + md.edge_color[c] * w, 0.0f, 4.0f);
                                    }
                                    dst.outline_width = std::max(0.0f, dst.outline_width + md.edge_size * w);
                                }
                            }
                        }
                    }
                }
            }

            if (!pmx_solver.empty() && pmx_local_t_scratch.size() == j_count * 3U &&
                pmx_local_r_scratch.size() == j_count * 4U) {
                for (size_t j = 0; j < j_count; ++j) {
                    for (int k = 0; k < 3; ++k) {
                        pmx_local_t_scratch[j * 3U + k] = joints[j].bind_translation[k];
                    }
                    for (int k = 0; k < 4; ++k) {
                        pmx_local_r_scratch[j * 4U + k] = joints[j].bind_rotation_quat[k];
                    }
                }
                for (size_t ci = 0; ci < pmx_morph_channels.size() && ci < pmx_channel_weights_scratch.size(); ++ci) {
                    const auto& ch = pmx_morph_channels[ci];
                    if (ch.morph_kind != AETHER_PMX_MORPH_BONE) {
                        continue;
                    }
                    const float w = pmx_channel_weights_scratch[ci];
                    if (std::abs(w) < 1e-4f) {
                        continue;
                    }
                    const uint32_t end = std::min<uint32_t>(ch.bone_offset + ch.bone_count,
                                                            static_cast<uint32_t>(pmx_bone_morphs.size()));
                    for (uint32_t bi = ch.bone_offset; bi < end; ++bi) {
                        const auto& bm = pmx_bone_morphs[bi];
                        if (static_cast<size_t>(bm.bone_index) < j_count) {
                            const size_t jb = static_cast<size_t>(bm.bone_index);
                            pmx_local_t_scratch[jb * 3U + 0] += bm.translation[0] * w;
                            pmx_local_t_scratch[jb * 3U + 1] += bm.translation[1] * w;
                            pmx_local_t_scratch[jb * 3U + 2] += bm.translation[2] * w;
                        }
                    }
                }
                pmx_solver.step(animation::PmxSkeletonAndPhysicsSolver::kFixedDt, joints, pmx_local_t_scratch,
                                pmx_local_r_scratch, pmx_local_s_scratch, pmx_world_mats_scratch);
                if (pmx_rest_inv_mats.size() == j_count * 12U) {
                    for (size_t j = 0; j < j_count; ++j) {
                        const float* w_col = &pmx_world_mats_scratch[j * 16U];
                        const float* inv_row = &pmx_rest_inv_mats[j * 12U];
                        // Multiply column-major 4x4 w_col by row-major 3x4 inv_row into row-major 3x4 delta
                        std::array<float, 12> w_row{
                            w_col[0], w_col[4],  w_col[8], w_col[12], w_col[1],  w_col[5],
                            w_col[9], w_col[13], w_col[2], w_col[6],  w_col[10], w_col[14],
                        };
                        std::array<float, 12> inv_arr{};
                        std::memcpy(inv_arr.data(), inv_row, sizeof(float) * 12U);
                        std::array<float, 12> pmx_delta{};
                        mul_3x4(w_row, inv_arr, pmx_delta);
                        std::array<float, 12> composed{};
                        mul_3x4(joint_skin_matrices[j], pmx_delta, composed);
                        joint_skin_matrices[j] = composed;
                    }
                }
            }
        }

        if (dynamic_joint_mat_bufs[frame_slot].info.pMappedData != nullptr && !joint_skin_matrices.empty()) {
            const size_t jm_bytes = joint_skin_matrices.size() * sizeof(float) * 12U;
            std::memcpy(dynamic_joint_mat_bufs[frame_slot].info.pMappedData, joint_skin_matrices.data(), jm_bytes);
            vmaFlushAllocation(device->allocator(), dynamic_joint_mat_bufs[frame_slot].allocation, 0, VK_WHOLE_SIZE);
        }

        const size_t v_count = skinned_vertices_scratch.size();
        const bool has_skin = (skin_vertices.size() == v_count && !joint_skin_matrices.empty());
        if (has_skin) {
            constexpr float kInvUnorm16 = 1.0f / 65535.0f;
            const size_t max_j = joint_skin_matrices.size();
            auto quat_from_3x4 = [](const std::array<float, 12>& m) noexcept -> std::array<float, 4> {
                const float m00 = m[0], m01 = m[1], m02 = m[2];
                const float m10 = m[4], m11 = m[5], m12 = m[6];
                const float m20 = m[8], m21 = m[9], m22 = m[10];
                const float trace = m00 + m11 + m22;
                float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
                if (trace > 0.0f) {
                    const float s = std::sqrt(trace + 1.0f) * 2.0f;
                    qw = 0.25f * s;
                    qx = (m21 - m12) / s;
                    qy = (m02 - m20) / s;
                    qz = (m10 - m01) / s;
                } else if (m00 > m11 && m00 > m22) {
                    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                    qw = (m21 - m12) / s;
                    qx = 0.25f * s;
                    qy = (m01 + m10) / s;
                    qz = (m02 + m20) / s;
                } else if (m11 > m22) {
                    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                    qw = (m02 - m20) / s;
                    qx = (m01 + m10) / s;
                    qy = 0.25f * s;
                    qz = (m12 + m21) / s;
                } else {
                    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                    qw = (m10 - m01) / s;
                    qx = (m02 + m20) / s;
                    qy = (m12 + m21) / s;
                    qz = 0.25f * s;
                }
                const float n2 = qx * qx + qy * qy + qz * qz + qw * qw;
                if (n2 > 1e-12f) {
                    const float inv = 1.0f / std::sqrt(n2);
                    return {qx * inv, qy * inv, qz * inv, qw * inv};
                }
                return {0.0f, 0.0f, 0.0f, 1.0f};
            };
            auto slerp_q = [](std::array<float, 4> a, std::array<float, 4> b,
                              float t) noexcept -> std::array<float, 4> {
                float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                if (dot < 0.0f) {
                    b = {-b[0], -b[1], -b[2], -b[3]};
                    dot = -dot;
                }
                float wa = 1.0f - t;
                float wb = t;
                if (dot < 0.9995f) {
                    const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
                    const float sin_t = std::sin(theta);
                    if (std::abs(sin_t) > 1e-6f) {
                        wa = std::sin((1.0f - t) * theta) / sin_t;
                        wb = std::sin(t * theta) / sin_t;
                    }
                }
                const float qx = a[0] * wa + b[0] * wb;
                const float qy = a[1] * wa + b[1] * wb;
                const float qz = a[2] * wa + b[2] * wb;
                const float qw = a[3] * wa + b[3] * wb;
                const float n2 = qx * qx + qy * qy + qz * qz + qw * qw;
                const float inv = (n2 > 1e-12f) ? (1.0f / std::sqrt(n2)) : 1.0f;
                return {qx * inv, qy * inv, qz * inv, qw * inv};
            };
            auto quat_rot = [](const std::array<float, 4>& q, const float v[3], float out[3]) noexcept {
                const float ux = q[0], uy = q[1], uz = q[2], s = q[3];
                const float tx = 2.0f * (uy * v[2] - uz * v[1]);
                const float ty = 2.0f * (uz * v[0] - ux * v[2]);
                const float tz = 2.0f * (ux * v[1] - uy * v[0]);
                out[0] = v[0] + s * tx + (uy * tz - uz * ty);
                out[1] = v[1] + s * ty + (uz * tx - ux * tz);
                out[2] = v[2] + s * tz + (ux * ty - uy * tx);
            };

            for (size_t vi = 0; vi < v_count; ++vi) {
                const AetherVertexSkin& sk = skin_vertices[vi];
                if (sk.weights_unorm16[0] == 0U || sk.joints[0] >= max_j) {
                    continue;
                }
                AetherVertex25D& vtx = skinned_vertices_scratch[vi];
                const float in_pos[3] = {vtx.position[0], vtx.position[1], vtx.position[2]};
                const float in_tn[3] = {vtx.true_normal[0], vtx.true_normal[1], vtx.true_normal[2]};
                const float in_pn[3] = {vtx.proxy_normal[0], vtx.proxy_normal[1], vtx.proxy_normal[2]};

                // SDEF Spherical Deform volume-preserving skinning for PMX vertices with flags & 1
                if (vi < pmx_sdef_vertices.size() && (pmx_sdef_vertices[vi].flags & 1U) != 0U &&
                    sk.weights_unorm16[1] != 0U && sk.joints[1] < max_j) {
                    const auto& sdef = pmx_sdef_vertices[vi];
                    const float w0 = std::clamp(static_cast<float>(sk.weights_unorm16[0]) * kInvUnorm16, 0.0f, 1.0f);
                    const float w1 = 1.0f - w0;
                    const auto& m0 = joint_skin_matrices[sk.joints[0]];
                    const auto& m1 = joint_skin_matrices[sk.joints[1]];
                    const auto q0 = quat_from_3x4(m0);
                    const auto q1 = quat_from_3x4(m1);
                    const auto qb = slerp_q(q0, q1, w1);

                    const float rw[3] = {
                        w0 * sdef.r0[0] + w1 * sdef.r1[0],
                        w0 * sdef.r0[1] + w1 * sdef.r1[1],
                        w0 * sdef.r0[2] + w1 * sdef.r1[2],
                    };
                    const float c0[3] = {
                        sdef.c[0] + 0.5f * (sdef.r0[0] - rw[0]),
                        sdef.c[1] + 0.5f * (sdef.r0[1] - rw[1]),
                        sdef.c[2] + 0.5f * (sdef.r0[2] - rw[2]),
                    };
                    const float c1[3] = {
                        sdef.c[0] + 0.5f * (sdef.r1[0] - rw[0]),
                        sdef.c[1] + 0.5f * (sdef.r1[1] - rw[1]),
                        sdef.c[2] + 0.5f * (sdef.r1[2] - rw[2]),
                    };
                    float mc0[3]{}, mc1[3]{}, rel_p[3]{}, rot_p[3]{};
                    transform_point_3x4(m0, c0, mc0);
                    transform_point_3x4(m1, c1, mc1);
                    rel_p[0] = in_pos[0] - sdef.c[0];
                    rel_p[1] = in_pos[1] - sdef.c[1];
                    rel_p[2] = in_pos[2] - sdef.c[2];
                    quat_rot(qb, rel_p, rot_p);
                    vtx.position[0] = w0 * mc0[0] + w1 * mc1[0] + rot_p[0];
                    vtx.position[1] = w0 * mc0[1] + w1 * mc1[1] + rot_p[1];
                    vtx.position[2] = w0 * mc0[2] + w1 * mc1[2] + rot_p[2];
                    quat_rot(qb, in_tn, vtx.true_normal);
                    quat_rot(qb, in_pn, vtx.proxy_normal);
                    continue;
                }

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

void prepare_cpu_avatar_payload(StagedAvatarUpload& staged) {
    if (staged.prebuilt_avatar || staged.staging_buf.info.pMappedData == nullptr) {
        return;
    }

    auto* mapped_mut = static_cast<uint8_t*>(staged.staging_buf.info.pMappedData);
    const uint8_t* mapped = mapped_mut;

    auto avatar = std::make_unique<GpuAvatar>();
    avatar->requirements = staged.requirements;
    avatar->desc = staged.desc;
    avatar->source_path = std::move(staged.source_path);
    avatar->stats.open_query_ms = staged.open_query_ms;
    avatar->stats.fill_ms = staged.fill_ms;
    avatar->stats.staging_bytes = staged.staging_buf.size;
    avatar->stats.pointer_identity_verified = staged.pointer_identity_verified;

    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_MORPH_RANGES],
                    staged.requirements.stream_bytes[AETHER_STREAM_MORPH_RANGES], avatar->morph_ranges);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_SUBMESHES],
                    staged.requirements.stream_bytes[AETHER_STREAM_SUBMESHES], avatar->submeshes);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_MATERIALS],
                    staged.requirements.stream_bytes[AETHER_STREAM_MATERIALS], avatar->materials);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_JOINTS],
                    staged.requirements.stream_bytes[AETHER_STREAM_JOINTS], avatar->joints);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_SPRINGS],
                    staged.requirements.stream_bytes[AETHER_STREAM_SPRINGS], avatar->springs);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_COLLIDERS],
                    staged.requirements.stream_bytes[AETHER_STREAM_COLLIDERS], avatar->colliders);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_EXPRESSIONS],
                    staged.requirements.stream_bytes[AETHER_STREAM_EXPRESSIONS], avatar->expressions);
    copy_stream_pod(mapped, staged.stream_offsets[AETHER_STREAM_TEXTURE_DESCS],
                    staged.requirements.stream_bytes[AETHER_STREAM_TEXTURE_DESCS], avatar->texture_descs);

    const uint64_t str_bytes = staged.requirements.stream_bytes[AETHER_STREAM_STRINGS];
    if (str_bytes > 0) {
        avatar->strings.assign(reinterpret_cast<const char*>(mapped + staged.stream_offsets[AETHER_STREAM_STRINGS]),
                               static_cast<size_t>(str_bytes));
    }

    // Move PMX 2.0/2.1 auxiliary payload and initialize zero-alloc runtime buffers.
    avatar->pmx_aux_header = staged.pmx_aux_header;
    avatar->pmx_sdef_vertices = std::move(staged.pmx_sdef_vertices);
    avatar->pmx_bone_exts = std::move(staged.pmx_bone_exts);
    avatar->pmx_ik_links = std::move(staged.pmx_ik_links);
    avatar->pmx_morph_channels = std::move(staged.pmx_morph_channels);
    avatar->pmx_group_children = std::move(staged.pmx_group_children);
    avatar->pmx_uv_morphs = std::move(staged.pmx_uv_morphs);
    avatar->pmx_mat_morphs = std::move(staged.pmx_mat_morphs);
    avatar->pmx_bone_morphs = std::move(staged.pmx_bone_morphs);
    avatar->pmx_rigid_bodies = std::move(staged.pmx_rigid_bodies);
    avatar->pmx_constraints_6dof = std::move(staged.pmx_constraints_6dof);
    avatar->morphed_materials_scratch = avatar->materials;

    if (avatar->pmx_aux_header.is_pmx != 0U) {
        avatar->uv_deltas_scratch.assign(avatar->requirements.vertex_count, {0.0f, 0.0f});
        avatar->pmx_channel_weights_scratch.assign(avatar->pmx_morph_channels.size(), 0.0f);
        const size_t jc = avatar->joints.size();
        avatar->pmx_local_t_scratch.resize(jc * 3U, 0.0f);
        avatar->pmx_local_r_scratch.resize(jc * 4U, 0.0f);
        avatar->pmx_local_s_scratch.resize(jc * 3U, 1.0f);
        avatar->pmx_world_mats_scratch.resize(jc * 16U, 0.0f);
        avatar->pmx_rest_world_mats.resize(jc * 16U, 0.0f);
        avatar->pmx_rest_inv_mats.resize(jc * 12U, 0.0f);
        for (size_t j = 0; j < jc; ++j) {
            for (int k = 0; k < 3; ++k) {
                avatar->pmx_local_t_scratch[j * 3U + k] = avatar->joints[j].bind_translation[k];
                avatar->pmx_local_s_scratch[j * 3U + k] = avatar->joints[j].bind_scale[k];
            }
            for (int k = 0; k < 4; ++k) {
                avatar->pmx_local_r_scratch[j * 4U + k] = avatar->joints[j].bind_rotation_quat[k];
            }
            std::memcpy(&avatar->pmx_rest_inv_mats[j * 12U], avatar->joints[j].inverse_bind_matrix_row_major,
                        sizeof(float) * 12U);
        }
        avatar->pmx_solver.init(avatar->joints, avatar->pmx_bone_exts, avatar->pmx_ik_links, avatar->pmx_rigid_bodies,
                                avatar->pmx_constraints_6dof);
    }

    const uint64_t v25_bytes = staged.requirements.stream_bytes[AETHER_STREAM_VERTEX25D];
    const uint64_t surf_bytes = staged.requirements.stream_bytes[AETHER_STREAM_SURFACE];
    const uint64_t skin_bytes = staged.requirements.stream_bytes[AETHER_STREAM_SKIN];
    const uint64_t idx_bytes = staged.requirements.stream_bytes[AETHER_STREAM_INDEX_U32];
    const uint64_t morph_bytes = staged.requirements.stream_bytes[AETHER_STREAM_MORPH_DELTAS];

    auto assign_stream_pod = [&](uint64_t off, uint64_t bytes, auto& out) {
        using Elem = std::decay_t<decltype(out)>::value_type;
        if (bytes >= sizeof(Elem)) {
            const auto* src = reinterpret_cast<const Elem*>(mapped + off);
            out.assign(src, src + (bytes / sizeof(Elem)));
        }
    };
    assign_stream_pod(staged.stream_offsets[AETHER_STREAM_VERTEX25D], v25_bytes, avatar->rest_vertices);
    assign_stream_pod(staged.stream_offsets[AETHER_STREAM_SURFACE], surf_bytes, avatar->rest_surfaces);
    assign_stream_pod(staged.stream_offsets[AETHER_STREAM_INDEX_U32], idx_bytes, avatar->rest_indices);
    assign_stream_pod(staged.stream_offsets[AETHER_STREAM_SKIN], skin_bytes, avatar->skin_vertices);
    assign_stream_pod(staged.stream_offsets[AETHER_STREAM_MORPH_DELTAS], morph_bytes, avatar->morph_deltas);

    const uint64_t tex_pix_bytes = staged.requirements.stream_bytes[AETHER_STREAM_TEXTURE_PIXELS];
    if (!staged.cpu_texture_pixels.empty()) {
        avatar->cpu_texture_pixels = std::move(staged.cpu_texture_pixels);
    } else if (tex_pix_bytes > 0) {
        const uint8_t* tp = mapped + staged.stream_offsets[AETHER_STREAM_TEXTURE_PIXELS];
        avatar->cpu_texture_pixels.assign(tp, tp + static_cast<size_t>(tex_pix_bytes));
    }

    (void)prepare_vrm_and_pmx_parity(*avatar, true);
    if (v25_bytes > 0 && !avatar->rest_vertices.empty()) {
        std::memcpy(mapped_mut + staged.stream_offsets[AETHER_STREAM_VERTEX25D], avatar->rest_vertices.data(),
                    static_cast<size_t>(v25_bytes));
    }

    // Populate fallback 1x1 white RGBA8 texel in the reserved staging tail.
    static constexpr std::array<uint8_t, 4> kWhitePx{255U, 255U, 255U, 255U};
    std::memcpy(mapped_mut + staged.fallback_white_offset, kWhitePx.data(), kWhitePx.size());

    avatar->skinned_vertices_scratch.assign(avatar->rest_vertices.begin(), avatar->rest_vertices.end());
    avatar->morph_weights_scratch.assign(avatar->morph_ranges.size(), 0.0f);

    compute_rest_joint_world_positions(avatar->joints, avatar->joint_rest_world_pos);
    avatar->joint_anim_world_pos = avatar->joint_rest_world_pos;
    compute_face_skin_reference(*avatar);
    build_joint_humanoid_sources(avatar->joints, avatar->desc, avatar->joint_humanoid_source);
    build_joint_spring_sources(avatar->joints, avatar->springs, avatar->joint_spring_source);
    build_deduped_expressions_and_morph_meta(avatar->expressions, avatar->morph_ranges, avatar->morph_deltas,
                                             avatar->rest_vertices.size(), avatar->deduped_expressions,
                                             avatar->morph_meta);
    detect_brow_bone_bindings(avatar->joints, avatar->strings, avatar->joint_humanoid_source,
                              avatar->joint_rest_world_pos, avatar->brow_bones);
    avatar->has_split_brow_morphs = false;
    for (const AetherExpressionBindingDesc& eb : avatar->deduped_expressions) {
        if (eb.input_slot >= 41U && eb.input_slot <= 45U) {
            avatar->has_split_brow_morphs = true;
            break;
        }
    }
    avatar->joint_skin_matrices.assign(avatar->joints.size(),
                                       {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
    avatar->bone_line_scratch.resize(avatar->joints.size() * 4U);
    emit_bone_line_vertices(avatar->joints, avatar->joint_rest_world_pos, avatar->bone_line_scratch);

    staged.prebuilt_avatar = std::move(avatar);
}

Expected<std::unique_ptr<GpuAvatar>> upload_staged_avatar(rhi::RhiDevice& rhi,
                                                          std::unique_ptr<StagedAvatarUpload> staged) {
    if (!staged || staged->staging_buf.handle == VK_NULL_HANDLE || staged->staging_buf.info.pMappedData == nullptr) {
        return fail(ErrorCode::InvalidArg, "Invalid StagedAvatarUpload payload");
    }

    const uint64_t upload_t0 = now_us();
    if (!staged->prebuilt_avatar) {
        prepare_cpu_avatar_payload(*staged);
    }

    auto avatar = std::move(staged->prebuilt_avatar);
    avatar->device = &rhi;

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
            auto dyn_res = rhi.create_uma_zero_copy_buffer(v25_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
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
    if (!avatar->bone_line_scratch.empty()) {
        const VkDeviceSize bone_bytes =
            static_cast<VkDeviceSize>(avatar->bone_line_scratch.size()) * sizeof(BoneLineVertex);
        auto b_res = rhi.create_uma_zero_copy_buffer(bone_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
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
            auto db_res = rhi.create_uma_zero_copy_buffer(bone_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
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

    if (!avatar->pmx_sdef_vertices.empty()) {
        const VkDeviceSize sdef_bytes =
            static_cast<VkDeviceSize>(avatar->pmx_sdef_vertices.size()) * sizeof(AetherPmxSdefVertex);
        auto r = rhi.create_uma_zero_copy_buffer(sdef_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        if (r) {
            avatar->pmx_sdef_buf = r.value();
            std::memcpy(avatar->pmx_sdef_buf.info.pMappedData, avatar->pmx_sdef_vertices.data(),
                        static_cast<size_t>(sdef_bytes));
            vmaFlushAllocation(rhi.allocator(), avatar->pmx_sdef_buf.allocation, 0, VK_WHOLE_SIZE);
        }
    }
    if (!avatar->uv_deltas_scratch.empty()) {
        const VkDeviceSize uv_bytes = static_cast<VkDeviceSize>(avatar->uv_deltas_scratch.size()) * sizeof(float) * 2U;
        for (uint32_t s = 0; s < GpuAvatar::kPoseSlotCount; ++s) {
            auto r = rhi.create_uma_zero_copy_buffer(uv_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            if (r) {
                avatar->dynamic_uv_delta_bufs[s] = r.value();
                std::memset(avatar->dynamic_uv_delta_bufs[s].info.pMappedData, 0, static_cast<size_t>(uv_bytes));
                vmaFlushAllocation(rhi.allocator(), avatar->dynamic_uv_delta_bufs[s].allocation, 0, VK_WHOLE_SIZE);
            }
        }
    }
    if (!avatar->joint_skin_matrices.empty()) {
        const VkDeviceSize jm_bytes =
            static_cast<VkDeviceSize>(avatar->joint_skin_matrices.size()) * sizeof(float) * 12U;
        for (uint32_t s = 0; s < GpuAvatar::kPoseSlotCount; ++s) {
            auto r = rhi.create_uma_zero_copy_buffer(jm_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
            if (r) {
                avatar->dynamic_joint_mat_bufs[s] = r.value();
                std::memcpy(avatar->dynamic_joint_mat_bufs[s].info.pMappedData, avatar->joint_skin_matrices.data(),
                            static_cast<size_t>(jm_bytes));
                vmaFlushAllocation(rhi.allocator(), avatar->dynamic_joint_mat_bufs[s].allocation, 0, VK_WHOLE_SIZE);
            }
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
