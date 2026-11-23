#pragma once

#include "aether_ffi_contract.h"
#include "animation/math_quat.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aether::renderer {

/**
 * Explicit 4-bit RT geometry visibility masks for 2.5D anime ray tracing:
 * - FaceSkin (0x01): Face, nose, mouth, ears, eyes; never self-shadows, traces Hair | HandsProps | BodyCloth.
 * - Hair (0x02): Front bangs, strands, and back hair (alpha-tested via non-opaque RT candidate checks).
 * - HandsProps (0x04): Hands and forearms extracted by humanoid joint weights.
 * - BodyCloth (0x08): Neck, torso, garments, and legs; traces all masks (0xFF).
 */
enum class RtInstanceMask : uint8_t {
    FaceSkin = 0x01,
    Hair = 0x02,
    HandsProps = 0x04,
    BodyCloth = 0x08,
    All = 0xFF,
};

enum class RtMaskBucket : uint8_t {
    FaceSkin = 0,
    Hair = 1,
    HandsProps = 2,
    BodyCloth = 3,
};

struct alignas(16) RtTriMeta {
    uint32_t i0{0};
    uint32_t i1{0};
    uint32_t i2{0};
    uint32_t mat_id{0};
};
static_assert(sizeof(RtTriMeta) == 16);

inline constexpr uint32_t kMatExtraRtClassShift = 10u;
inline constexpr uint32_t kMatExtraRtClassMask = 0xC00u;
inline constexpr uint32_t kMatExtraEyeIrisBit = (1u << 12u);
inline constexpr uint32_t kMatExtraBodySkinAtlasBit = (1u << 13u);
// Set only when the VRM itself ships a UV-space face shadow map (or bake_and_apply_face_sdf activates one).
inline constexpr uint32_t kMatExtraAuthoredSdfBit = (1u << 15u);

[[nodiscard]] inline std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

[[nodiscard]] inline bool contains_any(std::string_view hay, std::initializer_list<std::string_view> needles) noexcept {
    for (const std::string_view n : needles) {
        if (!n.empty() && hay.find(n) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::string read_material_cstr(const GpuAvatar& avatar, uint32_t name_offset) {
    if (name_offset >= avatar.strings.size()) {
        return {};
    }
    const char* p = avatar.strings.data() + name_offset;
    const size_t max_len = avatar.strings.size() - name_offset;
    size_t len = 0;
    while (len < max_len && p[len] != '\0') {
        ++len;
    }
    return std::string(p, len);
}

/**
 * Returns true ONLY for dedicated eye-iris primitives (so cornea catchlights never spill onto
 * face skin, sclera, eyebrows, highlights, or clothing in shared atlases).
 */
[[nodiscard]] inline bool is_genuine_eye_iris_name(std::string_view mat_raw, std::string_view sm_raw) {
    const std::string m_low = to_lower_ascii(mat_raw);
    const std::string s_low = to_lower_ascii(sm_raw);
    if (contains_any(m_low, {"highlight", "eyeline", "eyelash", "eyebrow", "brow", "lash", "sclera", "skin", "body",
                             "cloth", "hair", "face"}) ||
        contains_any(s_low, {"highlight", "eyeline", "eyelash", "eyebrow", "brow", "lash", "sclera", "skin", "body",
                             "cloth", "hair"})) {
        return false;
    }
    return contains_any(m_low, {"eyeiris", "iris", "pupil", "hitomi"}) ||
           contains_any(s_low, {"eyeiris", "iris", "pupil", "hitomi"});
}

/**
 * Returns true for body skin primitives/atlases (e.g. `Body_00_SKIN`) that contain exposed skin
 * alongside dark clothing (such as inner black lace), enabling safe per-pixel skin continuity
 * without affecting non-skin regions or separate clothing primitives (`Bottoms_01_CLOTH`).
 */
[[nodiscard]] inline bool is_body_skin_atlas_name(std::string_view mat_raw, std::string_view sm_raw) {
    const std::string m_low = to_lower_ascii(mat_raw);
    const std::string s_low = to_lower_ascii(sm_raw);
    if (contains_any(m_low, {"bottom", "cardigan", "skirt", "coat", "jacket", "hair", "eye"}) ||
        contains_any(s_low, {"bottom", "cardigan", "skirt", "hair", "eye"})) {
        return false;
    }
    return contains_any(m_low, {"body", "skin", "hada", "neck"}) ||
           contains_any(s_low, {"body", "skin", "hada", "neck"});
}

[[nodiscard]] inline RtMaskBucket classify_rt_bucket(std::string_view mat_raw, std::string_view sm_raw,
                                                     uint32_t mat_flags, uint32_t sm_flags) {
    const std::string m_low = to_lower_ascii(mat_raw);
    const std::string s_low = to_lower_ascii(sm_raw);

    const bool is_hair = ((mat_flags & AETHER_MAT_FLAG_HAIR_FRONT_STENCIL) != 0u) ||
                         ((sm_flags & (AETHER_SUBMESH_FLAG_HAIR_FRONT | AETHER_SUBMESH_FLAG_HAIR_BACK)) != 0u) ||
                         contains_any(m_low, {"hair", "bang", "kami", "ahoge"}) ||
                         contains_any(s_low, {"hair", "bang", "kami", "ahoge"}) ||
                         contains_any(mat_raw, {"\xE9\xAB\xAA"}) || contains_any(sm_raw, {"\xE9\xAB\xAA"});
    if (is_hair) {
        return RtMaskBucket::Hair;
    }

    const bool is_face = ((mat_flags & (AETHER_MAT_FLAG_SDF_FACE | AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW)) != 0u) ||
                         ((sm_flags & AETHER_SUBMESH_FLAG_FACE) != 0u) ||
                         contains_any(m_low, {"face", "mouth", "ear", "nose", "tongue", "teeth", "iris", "eye"}) ||
                         contains_any(s_low, {"face", "head", "mouth", "ear", "nose", "iris", "eye"});
    if (is_face) {
        return RtMaskBucket::FaceSkin;
    }

    if (contains_any(m_low, {"hand", "glove", "wrist"}) || contains_any(s_low, {"hand", "glove", "wrist"})) {
        return RtMaskBucket::HandsProps;
    }

    return RtMaskBucket::BodyCloth;
}

[[nodiscard]] inline uint8_t bucket_to_instance_mask(RtMaskBucket b) noexcept {
    switch (b) {
    case RtMaskBucket::FaceSkin:
        return static_cast<uint8_t>(RtInstanceMask::FaceSkin);
    case RtMaskBucket::Hair:
        return static_cast<uint8_t>(RtInstanceMask::Hair);
    case RtMaskBucket::HandsProps:
        return static_cast<uint8_t>(RtInstanceMask::HandsProps);
    case RtMaskBucket::BodyCloth:
    default:
        return static_cast<uint8_t>(RtInstanceMask::BodyCloth);
    }
}

struct RtPartitionedIndices {
    std::vector<uint32_t> sorted_indices{};
    std::vector<RtTriMeta> tri_meta{};
    std::array<uint32_t, 4> bucket_index_offset{};
    std::array<uint32_t, 4> bucket_index_count{};
    std::array<bool, 4> bucket_has_cutout{};
};

/**
 * Partitions opaque and alpha-tested avatar triangles into 4 contiguous RT BLAS buckets
 * (`FaceSkin 0x01`, `Hair 0x02`, `HandsProps 0x04`, `BodyCloth 0x08`) and emits per-triangle
 * `(i0, i1, i2, mat_id)` metadata for RT alpha cutout testing and true-albedo bounce lookup.
 */
[[nodiscard]] inline RtPartitionedIndices build_rt_partitioned_indices(const GpuAvatar& avatar) {
    RtPartitionedIndices out{};
    if (avatar.rest_indices.empty()) {
        return out;
    }

    const size_t joint_count = avatar.joints.size();
    std::vector<uint8_t> joint_region(joint_count, 0u); // 1 = Hand/Forearm
    if (joint_count > 0u) {
        auto mark_branch = [&](int32_t root_j, uint8_t tag) {
            if (root_j < 0 || static_cast<size_t>(root_j) >= joint_count) {
                return;
            }
            joint_region[static_cast<size_t>(root_j)] = tag;
            bool changed = true;
            while (changed) {
                changed = false;
                for (size_t j = 0; j < joint_count; ++j) {
                    if (joint_region[j] == 0u) {
                        const int32_t p = avatar.joints[j].parent_index;
                        if (p >= 0 && static_cast<size_t>(p) < joint_count &&
                            joint_region[static_cast<size_t>(p)] == tag) {
                            joint_region[j] = tag;
                            changed = true;
                        }
                    }
                }
            }
        };

        auto humanoid_idx = [&](uint32_t hb) -> int32_t {
            if (hb >= 55u) {
                return -1;
            }
            const uint16_t ji = avatar.desc.humanoid_to_joint[hb];
            return (ji != 0xFFFFu && ji < joint_count) ? static_cast<int32_t>(ji) : -1;
        };

        mark_branch(humanoid_idx(AETHER_BONE_LEFT_LOWER_ARM), 1u);
        mark_branch(humanoid_idx(AETHER_BONE_LEFT_HAND), 1u);
        mark_branch(humanoid_idx(AETHER_BONE_RIGHT_LOWER_ARM), 1u);
        mark_branch(humanoid_idx(AETHER_BONE_RIGHT_HAND), 1u);
    }

    auto vertex_hand_weight = [&](uint32_t vtx_idx) noexcept -> float {
        if (vtx_idx >= avatar.skin_vertices.size() || joint_count == 0u) {
            return 0.0f;
        }
        const auto& sk = avatar.skin_vertices[vtx_idx];
        float out_w = 0.0f;
        for (size_t k = 0; k < 8u; ++k) {
            const uint16_t ji = sk.joints[k];
            const uint16_t wu = sk.weights_unorm16[k];
            if (wu == 0u || ji >= joint_count) {
                continue;
            }
            if (joint_region[ji] == 1u) {
                out_w += static_cast<float>(wu) * (1.0f / 65535.0f);
            }
        }
        return out_w;
    };

    std::array<std::vector<uint32_t>, 4> tmp_buckets{};
    std::array<std::vector<RtTriMeta>, 4> tmp_meta{};
    const size_t approx_per_bucket = avatar.rest_indices.size() / 2u + 3u;
    for (size_t b = 0; b < 4u; ++b) {
        tmp_buckets[b].reserve(approx_per_bucket);
        tmp_meta[b].reserve(approx_per_bucket / 3u + 1u);
    }

    auto append_range = [&](uint32_t idx_off, uint32_t idx_cnt, uint32_t mat_idx, uint32_t sm_flags,
                            std::string_view sm_name, bool is_cutout) {
        uint32_t m_flags = 0u;
        std::string m_name;
        const uint32_t safe_mat =
            avatar.materials.empty() ? 0u
                                     : std::min<uint32_t>(mat_idx, static_cast<uint32_t>(avatar.materials.size() - 1u));
        if (safe_mat < avatar.materials.size()) {
            m_flags = avatar.materials[safe_mat].flags;
            m_name = read_material_cstr(avatar, avatar.materials[safe_mat].name_offset);
        }
        const RtMaskBucket default_bucket = classify_rt_bucket(m_name, sm_name, m_flags, sm_flags);

        const uint32_t end = std::min<uint32_t>(idx_off + idx_cnt, static_cast<uint32_t>(avatar.rest_indices.size()));
        for (uint32_t t = idx_off; t + 2u < end; t += 3u) {
            const uint32_t i0 = avatar.rest_indices[t + 0u];
            const uint32_t i1 = avatar.rest_indices[t + 1u];
            const uint32_t i2 = avatar.rest_indices[t + 2u];
            RtMaskBucket b = default_bucket;
            if (b == RtMaskBucket::BodyCloth && !avatar.skin_vertices.empty()) {
                const float hand_avg =
                    (vertex_hand_weight(i0) + vertex_hand_weight(i1) + vertex_hand_weight(i2)) * (1.0f / 3.0f);
                if (hand_avg >= 0.45f) {
                    b = RtMaskBucket::HandsProps;
                }
            }
            const size_t bi = static_cast<size_t>(b);
            if (is_cutout) {
                out.bucket_has_cutout[bi] = true;
            }
            tmp_buckets[bi].push_back(i0);
            tmp_buckets[bi].push_back(i1);
            tmp_buckets[bi].push_back(i2);
            tmp_meta[bi].push_back(RtTriMeta{i0, i1, i2, safe_mat});
        }
    };

    if (avatar.submeshes.empty()) {
        append_range(0u, static_cast<uint32_t>(avatar.rest_indices.size()), 0u, AETHER_SUBMESH_FLAG_OPAQUE, {}, false);
    } else {
        for (const auto& sm : avatar.submeshes) {
            uint32_t alpha_mode = AETHER_ALPHA_MODE_OPAQUE;
            uint32_t m_flags = 0u;
            if (sm.material_index < avatar.materials.size()) {
                alpha_mode = avatar.materials[sm.material_index].alpha_mode;
                m_flags = avatar.materials[sm.material_index].flags;
            }
            const bool is_transparent =
                (alpha_mode == AETHER_ALPHA_MODE_BLEND) || (alpha_mode == AETHER_ALPHA_MODE_ADDITIVE) ||
                ((sm.flags & (AETHER_SUBMESH_FLAG_TRANSPARENT | AETHER_SUBMESH_FLAG_ADDITIVE)) != 0u) ||
                ((m_flags & AETHER_MAT_FLAG_NO_SHADOW_CAST) != 0u);
            if (is_transparent) {
                continue;
            }
            const bool is_cutout = (alpha_mode == AETHER_ALPHA_MODE_MASK) ||
                                   ((sm.flags & AETHER_SUBMESH_FLAG_ALPHA_CUTOUT) != 0u) ||
                                   ((m_flags & AETHER_MAT_FLAG_ALPHA_CUTOUT) != 0u);
            const std::string sm_name = avatar.string_slice(sm.name_offset, sm.name_len);
            append_range(sm.index_offset, sm.index_count, sm.material_index, sm.flags, sm_name, is_cutout);
        }
    }

    if (tmp_buckets[0].empty() && tmp_buckets[1].empty() && tmp_buckets[2].empty() && tmp_buckets[3].empty()) {
        const size_t n = avatar.rest_indices.size();
        tmp_buckets[static_cast<size_t>(RtMaskBucket::BodyCloth)] = avatar.rest_indices;
        auto& m_dst = tmp_meta[static_cast<size_t>(RtMaskBucket::BodyCloth)];
        for (size_t t = 0; t + 2 < n; t += 3) {
            m_dst.push_back(
                RtTriMeta{avatar.rest_indices[t], avatar.rest_indices[t + 1], avatar.rest_indices[t + 2], 0u});
        }
    }

    out.sorted_indices.reserve(avatar.rest_indices.size());
    out.tri_meta.reserve(avatar.rest_indices.size() / 3u + 1u);
    for (size_t b = 0; b < 4u; ++b) {
        out.bucket_index_offset[b] = static_cast<uint32_t>(out.sorted_indices.size());
        out.bucket_index_count[b] = static_cast<uint32_t>(tmp_buckets[b].size());
        out.sorted_indices.insert(out.sorted_indices.end(), tmp_buckets[b].begin(), tmp_buckets[b].end());
        out.tri_meta.insert(out.tri_meta.end(), tmp_meta[b].begin(), tmp_meta[b].end());
    }
    return out;
}

} // namespace aether::renderer
