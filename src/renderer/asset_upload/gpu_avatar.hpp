#pragma once

#include "aether_ffi_contract.h"
#include "animation/live2d_params.hpp"
#include "animation/pmx_physics.hpp"
#include "core/expected.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aether::renderer {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct GpuUploadStats {
    double open_query_ms{0.0};
    double fill_ms{0.0};
    double gpu_upload_ms{0.0};
    uint64_t staging_bytes{0};
    bool pointer_identity_verified{false};
};

struct BoneLineVertex {
    float pos[3]{};
    float color[4]{};
};

struct GpuAvatar;

/**
 * Staged host-visible CPU+VMA arena prepared on the async loader thread cleanly (`§5.6`).
 * Holds zero-copy decoded streams directly inside `staging_buf` ready for single-batch DMA upload.
 */
struct StagedAvatarUpload {
    rhi::RhiDevice* device{nullptr};
    rhi::RhiBuffer staging_buf{};
    std::array<uint64_t, AETHER_STREAM_COUNT> stream_offsets{};
    std::array<uint64_t, AETHER_STREAM_COUNT> lent_ptrs{};
    uint64_t fallback_white_offset{0};

    AetherAssetRequirements requirements{};
    AetherAvatarDesc desc{};
    double open_query_ms{0.0};
    double fill_ms{0.0};
    bool pointer_identity_verified{false};
    std::string source_path;

    AetherPmxAuxHeader pmx_aux_header{};
    std::vector<AetherPmxSdefVertex> pmx_sdef_vertices;
    std::vector<AetherPmxBoneExt> pmx_bone_exts;
    std::vector<AetherPmxIkLink> pmx_ik_links;
    std::vector<AetherPmxMorphChannelDesc> pmx_morph_channels;
    std::vector<AetherPmxGroupMorphChild> pmx_group_children;
    std::vector<AetherPmxUvMorphDelta> pmx_uv_morphs;
    std::vector<AetherPmxMaterialMorphDelta> pmx_mat_morphs;
    std::vector<AetherPmxBoneMorphOffset> pmx_bone_morphs;
    std::vector<AetherPmxRigidBodyDesc> pmx_rigid_bodies;
    std::vector<AetherPmxConstraint6DofDesc> pmx_constraints_6dof;
    std::vector<uint8_t> cpu_texture_pixels;
    std::unique_ptr<GpuAvatar> prebuilt_avatar;

    StagedAvatarUpload() noexcept = default;
    ~StagedAvatarUpload() noexcept;
    StagedAvatarUpload(const StagedAvatarUpload&) = delete;
    StagedAvatarUpload& operator=(const StagedAvatarUpload&) = delete;
    StagedAvatarUpload(StagedAvatarUpload&&) = delete;
    StagedAvatarUpload& operator=(StagedAvatarUpload&&) = delete;

    void release() noexcept;
};

/**
 * Resident Vulkan 1.3 GPU avatar assets & normalised skeleton/material metadata.
 */
struct GpuAvatar {
    static constexpr uint32_t kPoseSlotCount = 2;

    rhi::RhiDevice* device{nullptr};

    rhi::RhiBuffer vertex25d_buf{};
    rhi::RhiBuffer surface_buf{};
    rhi::RhiBuffer skin_buf{};
    rhi::RhiBuffer index_buf{};
    rhi::RhiBuffer morph_delta_buf{};
    rhi::RhiBuffer bone_line_buf{};
    uint32_t bone_line_vertex_count{0};

    std::array<rhi::RhiBuffer, kPoseSlotCount> dynamic_vertex25d_bufs{};
    std::array<rhi::RhiBuffer, kPoseSlotCount> dynamic_bone_line_bufs{};
    std::array<uint64_t, kPoseSlotCount> slot_applied_pose_seq{};

    // Pre-sized rest geometry & zero-alloc live preview scratch (`HR-01`, `P3-07`)
    std::vector<AetherVertex25D> rest_vertices;
    std::vector<AetherVertexSurface> rest_surfaces;
    std::vector<uint32_t> rest_indices;
    std::vector<AetherVertexSkin> skin_vertices;
    std::vector<AetherMorphDelta> morph_deltas;
    std::vector<uint8_t> cpu_texture_pixels; // Retained for instant Vulkan <-> DX12 live backend switch
    std::vector<AetherVertex25D> skinned_vertices_scratch;
    std::vector<float> morph_weights_scratch;
    std::vector<std::array<float, 3>> joint_rest_world_pos;
    std::vector<std::array<float, 3>> joint_anim_world_pos;
    std::vector<int32_t> joint_humanoid_source;
    std::vector<int32_t> joint_spring_source;
    std::vector<std::array<float, 12>> joint_skin_matrices;
    std::vector<BoneLineVertex> bone_line_scratch;

    std::vector<rhi::RhiImage> textures;
    rhi::RhiImage fallback_white{};

    AetherAssetRequirements requirements{};
    AetherAvatarDesc desc{};
    GpuUploadStats stats{};
    std::string source_path;

    std::vector<AetherSubmeshDesc> submeshes;
    std::vector<AetherNprMaterial> materials;
    std::vector<AetherJointDesc> joints;
    std::vector<AetherSpringJointDesc> springs;
    std::vector<AetherColliderDesc> colliders;
    std::vector<AetherMorphRangeDesc> morph_ranges;
    std::vector<AetherExpressionBindingDesc> expressions;
    std::vector<AetherExpressionBindingDesc> deduped_expressions;
    enum class MorphRegion : uint8_t {
        Other = 0,
        EyeL = 1,
        EyeR = 2,
        Brow = 3,
        Mouth = 4,
        Emotion = 5,
    };
    struct MorphMeta {
        MorphRegion region{MorphRegion::Other};
        bool affects_eyelids{false};
        bool is_blink{false};
    };
    std::vector<MorphMeta> morph_meta;
    struct BrowBoneBinding {
        uint32_t joint_index{0};
        uint8_t side{0}; // 0=left, 1=right, 2=center
    };

    std::vector<AetherTextureDesc> texture_descs;
    std::string strings;
    std::vector<BrowBoneBinding> brow_bones;
    bool has_split_brow_morphs{false};
    // Calibrated linear RGB of lower-cheek / chin Face skin sampled once on upload (w = 1.0 when valid).
    std::array<float, 4> face_skin_ref_linear{0.85f, 0.68f, 0.58f, 0.0f};

    // Phase 3: Native PMX 2.0/2.1 auxiliary data (SDEF, Bone Ext, CCD-IK, Morphs, 60 Hz Physics)
    AetherPmxAuxHeader pmx_aux_header{};
    std::vector<AetherPmxSdefVertex> pmx_sdef_vertices;
    std::vector<AetherPmxBoneExt> pmx_bone_exts;
    std::vector<AetherPmxIkLink> pmx_ik_links;
    std::vector<AetherPmxMorphChannelDesc> pmx_morph_channels;
    std::vector<AetherPmxGroupMorphChild> pmx_group_children;
    std::vector<AetherPmxUvMorphDelta> pmx_uv_morphs;
    std::vector<AetherPmxMaterialMorphDelta> pmx_mat_morphs;
    std::vector<AetherPmxBoneMorphOffset> pmx_bone_morphs;
    std::vector<AetherPmxRigidBodyDesc> pmx_rigid_bodies;
    std::vector<AetherPmxConstraint6DofDesc> pmx_constraints_6dof;

    rhi::RhiBuffer pmx_sdef_buf{};
    std::array<rhi::RhiBuffer, kPoseSlotCount> dynamic_uv_delta_bufs{};
    std::array<rhi::RhiBuffer, kPoseSlotCount> dynamic_joint_mat_bufs{};

    std::vector<std::array<float, 2>> uv_deltas_scratch;
    std::vector<float> pmx_channel_weights_scratch;
    std::vector<AetherNprMaterial> morphed_materials_scratch;
    std::vector<float> pmx_local_t_scratch;
    std::vector<float> pmx_local_r_scratch;
    std::vector<float> pmx_local_s_scratch;
    std::vector<float> pmx_world_mats_scratch;
    std::vector<float> pmx_rest_world_mats;
    std::vector<float> pmx_rest_inv_mats;
    animation::PmxSkeletonAndPhysicsSolver pmx_solver;

    ~GpuAvatar() { destroy(); }
    GpuAvatar() = default;
    GpuAvatar(const GpuAvatar&) = delete;
    GpuAvatar& operator=(const GpuAvatar&) = delete;

    void destroy() noexcept;

    /**
     * Updates double-buffered preview vertex & skeleton overlay buffers for `frame_slot` with
     * zero heap allocations (`HR-01`, `P3-07`).
     */
    void update_pose(uint32_t frame_slot, uint64_t pose_seq, const float (*bone_matrices)[12], uint32_t bone_count,
                     const float* expr_weights, uint32_t expr_count, bool update_mesh_vertices = true,
                     const animation::Live2DParameters* opt_live2d = nullptr) noexcept;

    [[nodiscard]] std::string string_slice(uint32_t offset, uint32_t len) const;
    [[nodiscard]] uint32_t mapped_humanoid_count() const noexcept;
};

/**
 * Builds CPU geometry/skeleton/parity/morph/bone metadata into `staged.prebuilt_avatar` off the main thread.
 */
void prepare_cpu_avatar_payload(StagedAvatarUpload& staged);

/**
 * Uploads a completed `StagedAvatarUpload` (zero-copy VMA host-visible staging) into GPU memory
 * using Synchronization2 barriers in a single submitted command buffer (`HR-08`, `<= 50 ms`).
 * Frees `staged->staging_buf` immediately after the upload fence signals.
 */
[[nodiscard]] Expected<std::unique_ptr<GpuAvatar>> upload_staged_avatar(rhi::RhiDevice& rhi,
                                                                        std::unique_ptr<StagedAvatarUpload> staged);

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace aether::renderer
