#pragma once

#include "core/expected.hpp"
#include "renderer/accel_struct.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/denoise/denoiser_manager.hpp"
#include "renderer/gpu_structs.hpp"
#include "renderer/lighting_rig.hpp"
#include "renderer/preview_pass.hpp"
#include "rhi/bindless.hpp"
#include "rhi/gpu_timing.hpp"
#include "rhi/rebar_ring.hpp"
#include "rhi/rhi_device.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace aether::renderer {

struct SdfBakeOutput;

enum class ShadingDebugView : uint8_t {
    Final = 0,
    Albedo = 1,
    TrueNormals = 2,
    ProxyNormals = 3,
    ZFlattenWeight = 4,
    SdfFace = 5,
    HairStencil = 6,
    RtShadowRaw = 7,
    RtShadowDenoised = 8,
    RimOcclusion = 9,
    Bounce = 10,
    OutlineMask = 11,
    MotionVectors = 12,
    Overdraw = 13,
    Count = 14,
};

[[nodiscard]] inline const char* shading_debug_view_name(ShadingDebugView view) noexcept {
    static constexpr const char* kNames[] = {
        "Final",    "Albedo",       "True Normals",   "Proxy Normals",      "Z-Flatten Weight",
        "SDF Face", "Hair Stencil", "RT Shadow Raw",  "RT Shadow Denoised", "Rim Occlusion",
        "Bounce",   "Outline Mask", "Motion Vectors", "Overdraw",
    };
    const auto idx = static_cast<size_t>(view);
    return idx < std::size(kNames) ? kNames[idx] : "Final";
}

struct RendererConfig {
    uint32_t width{1280};
    uint32_t height{720};
    uint32_t output_width{0};  // 0 => match width (backwards compatible)
    uint32_t output_height{0}; // 0 => match height
    bool enable_rt{true};
    bool allow_disk_shader_reload{false};
    DenoiserKind denoiser{DenoiserKind::Auto};
    ShadingDebugView debug_view{ShadingDebugView::Final};
    bool premul_alpha{true};
};

struct FrameRenderRequest {
    const GpuAvatar* avatar{nullptr};
    const LightingRig* rig{nullptr};
    PreviewCamera camera{};
    const float (*bone_matrices)[12]{nullptr};
    uint32_t bone_count{0};
    const float* joint_stress{nullptr};
    uint32_t joint_stress_count{0};
    uint64_t frame_index{0};
    uint64_t completed_frame_index{0};
    uint32_t frame_slot{0};
    ShadingDebugView debug_view{ShadingDebugView::Final};
    bool rt_enabled{true};
    bool premul_alpha{true};
    bool show_checkerboard{true};
    bool show_skeleton_heatmap{false};
    bool show_orbit_orb{true};
    bool render_csm_shadow{true};
    float outline_width_scale{1.0f};
    std::array<float, 3> non_alpha_bg_rgb{0.0f, 177.0f / 255.0f, 64.0f / 255.0f};
    bool need_straight_alpha{false};
    bool need_bottom_up{false};
    bool clear_swapchain{true};
    VkImageView swapchain_view{VK_NULL_HANDLE};
    VkExtent2D swapchain_extent{};

    // Alternate render backend (ADR-0016): when `external_color_image` is set, Passes 1-11 are skipped and the
    // premultiplied linear RGBA8 image is blitted into OUT_RGBA16F, so Pass 12 encode and every output still run.
    VkImage external_color_image{VK_NULL_HANDLE};
    uint32_t external_width{0};
    uint32_t external_height{0};
    // Layout the image is in when this frame starts (`UNDEFINED` on first use: the frame shows a clear instead).
    VkImageLayout external_layout{VK_IMAGE_LAYOUT_UNDEFINED};
    // true: image memory is owned by a D3D11 device (acquire/release with `VK_QUEUE_FAMILY_EXTERNAL`).
    bool external_is_foreign{false};
    // Optional CPU-readback path: tightly packed RGBA8 rows copied into the image before the blit.
    VkBuffer external_staging_buffer{VK_NULL_HANDLE};
};

/** Column-major view-projection used by every pass (shared with the DirectX 11 backend). */
[[nodiscard]] std::array<float, 16> compute_view_proj(const PreviewCamera& cam, float aspect) noexcept;

struct FrameGraphOutputs {
    VkImage rgba16f_image{VK_NULL_HANDLE};
    VkImageView rgba16f_view{VK_NULL_HANDLE};
    VkImage bgra8_image{VK_NULL_HANDLE};
    VkImageView bgra8_view{VK_NULL_HANDLE};
    VkImage bgra8_straight_image{VK_NULL_HANDLE};
    VkImage bottom_up_bgra8_image{VK_NULL_HANDLE};
    VkImage nv12_y_image{VK_NULL_HANDLE};
    VkImage nv12_uv_image{VK_NULL_HANDLE};
    VkImage viewport_image{VK_NULL_HANDLE};
    VkImageView viewport_view{VK_NULL_HANDLE};
    VkImage orbit_orb_image{VK_NULL_HANDLE};
    VkImageView orbit_orb_view{VK_NULL_HANDLE};
    VkImage rt_shadow_raw_image{VK_NULL_HANDLE};
    VkImage rt_shadow_denoised_image{VK_NULL_HANDLE};
    VkImage hdr_scene_image{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t output_width{0};
    uint32_t output_height{0};
    uint32_t output_slot{0};
};

/**
 * 14-Pass Vulkan 1.3 FrameGraph (`§10.2–§10.11`).
 * Strict split: `record_critical_passes` runs Passes 1–12 into `OUT_RGBA16F`, `OUT_BGRA8`, `OUT_NV12`
 * before signaling `frame_tl` for T4; `record_ui_passes` runs Pass 13 (`Viewport Compose` + `128^2 Orbit Orb`)
 * and leaves the swapchain attachment open/prepared for ImGui Pass 14.
 * Zero per-frame heap allocations (`HR-01`) and clean `sync2` validation (`HR-08`).
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class FrameGraph {
  public:
    static constexpr uint32_t kBloomMipCount = 6;
    static constexpr uint32_t kOrbitOrbSize = 128;
    static constexpr uint32_t kCsmSize = 2048;
    static constexpr uint32_t kOutputRingSlots = 3;

    FrameGraph() = default;
    ~FrameGraph() { destroy(); }

    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device, const RendererConfig& cfg);
    [[nodiscard]] Expected<void> resize(uint32_t viewport_w, uint32_t viewport_h, uint32_t output_w = UINT32_MAX,
                                        uint32_t output_h = UINT32_MAX);
    void destroy() noexcept;

    [[nodiscard]] Expected<void> bind_avatar(const GpuAvatar* avatar);

    /** Uploads an offline-baked face SDF R8 map into the bindless heap and enables it on face materials. */
    [[nodiscard]] Expected<void> apply_face_sdf(const SdfBakeOutput& baked);

    /** Records Passes 1–12 (Compute Skin/Morph, BLAS/TLAS, Prepass, RT/CSM, Denoise, NPR, Hull, Crease, Bloom,
     * Composite, Encode). */
    void record_critical_passes(VkCommandBuffer cmd_critical, const FrameRenderRequest& req) noexcept;

    /** Records Pass 13 (Viewport Checker/Debug/Heatmap + 128² Light Orbit Orb). */
    void record_ui_passes(VkCommandBuffer cmd_ui, const FrameRenderRequest& req) noexcept;

    /** Reloads SPIR-V modules when disk `.spv` timestamps advance in Debug builds (`P4-02`). */
    [[nodiscard]] uint32_t poll_shader_hot_reload();

    /** Synchronous test/verification readback helper for offscreen images (`B8G8R8A8_UNORM` -> RGBA8). */
    [[nodiscard]] Expected<std::vector<uint8_t>> readback_image_rgba8(VkImage image, VkImageLayout current_layout,
                                                                      uint32_t w, uint32_t h) const;

    /** Synchronous test/verification readback helper for `OUT_RGBA16F` -> float RGBA32F. */
    [[nodiscard]] Expected<std::vector<float>> readback_rgba16f(uint32_t w, uint32_t h) const;

    [[nodiscard]] FrameGraphOutputs outputs() const noexcept;
    [[nodiscard]] FrameGraphOutputs outputs_for_slot(uint32_t ring_slot) const noexcept;
    [[nodiscard]] uint32_t effective_output_width() const noexcept {
        return cfg_.output_width > 0 ? cfg_.output_width : cfg_.width;
    }
    [[nodiscard]] uint32_t effective_output_height() const noexcept {
        return cfg_.output_height > 0 ? cfg_.output_height : cfg_.height;
    }
    [[nodiscard]] const rhi::GpuTimingRing& timing() const noexcept { return timing_; }
    [[nodiscard]] rhi::GpuTimingRing& timing() noexcept { return timing_; }
    [[nodiscard]] const DenoiserManager& denoiser() const noexcept { return denoiser_; }
    [[nodiscard]] DenoiserManager& denoiser() noexcept { return denoiser_; }
    [[nodiscard]] const AccelStructManager& accel_struct() const noexcept { return accel_; }
    [[nodiscard]] AccelStructManager& accel_struct() noexcept { return accel_; }
    [[nodiscard]] const rhi::BindlessHeap& bindless() const noexcept { return bindless_; }
    [[nodiscard]] rhi::BindlessHeap& bindless() noexcept { return bindless_; }
    [[nodiscard]] bool rt_available() const noexcept { return device_ != nullptr && device_->rt_supported(); }
    [[nodiscard]] bool pipelines_ready() const noexcept { return pipelines_ready_; }

  private:
    void record_external_color_blit(VkCommandBuffer cmd, const FrameRenderRequest& req, uint32_t w,
                                    uint32_t h) noexcept;
    struct RegisteredImage {
        rhi::RhiImage img{};
        uint32_t sampled_slot{UINT32_MAX};
        uint32_t storage_slot{UINT32_MAX};
        uint32_t uint_slot{UINT32_MAX};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    Expected<void> create_size_dependent_targets(uint32_t w, uint32_t h);
    void destroy_size_dependent_targets() noexcept;
    Expected<void> create_static_targets();
    void destroy_static_targets() noexcept;
    Expected<void> create_pipelines(bool reload_only = false);
    void destroy_pipelines() noexcept;

    rhi::RhiDevice* device_{nullptr};
    RendererConfig cfg_{};

    rhi::BindlessHeap bindless_{};
    rhi::RebarRingBuffer rebar_{};
    rhi::GpuTimingRing timing_{};
    AccelStructManager accel_{};
    DenoiserManager denoiser_{};

    // Per-avatar GPU buffers pre-allocated on `bind_avatar()`
    const GpuAvatar* bound_avatar_{nullptr};
    rhi::RhiBuffer skinned_vertices_buf_{};
    rhi::RhiBuffer material_buf_{};
    rhi::RhiBuffer submesh_draw_buf_{};
    rhi::RhiImage fallback_white_tex_{};
    uint32_t fallback_white_slot_{UINT32_MAX};
    rhi::RhiImage baked_face_sdf_img_{};
    uint32_t baked_face_sdf_slot_{UINT32_MAX};
    std::vector<uint32_t> avatar_tex_slots_{};
    uint32_t avatar_face_sdf_slot_{UINT32_MAX};
    std::vector<GpuSubmeshDraw> cached_submeshes_{};
    std::vector<GpuMaterial> cached_materials_{};
    GpuMat4 prev_view_proj_{};
    bool have_prev_view_proj_{false};
    uint32_t last_output_slot_{0};

    // Size-dependent offscreen targets
    RegisteredImage gb_n_{};
    RegisteredImage gb_mv_{};
    RegisteredImage gb_mat_{};
    RegisteredImage gb_misc_{};
    rhi::RhiImage depth_stencil_{};
    VkImageView depth_only_view_{VK_NULL_HANDLE};
    VkImageView stencil_only_view_{VK_NULL_HANDLE};
    uint32_t depth_sampled_slot_{UINT32_MAX};
    uint32_t stencil_uint_slot_{UINT32_MAX};
    VkImageLayout depth_layout_{VK_IMAGE_LAYOUT_UNDEFINED};

    RegisteredImage rt_shadow_raw_{};
    RegisteredImage rt_shadow_denoised_{};
    RegisteredImage rim_occ_raw_{};
    RegisteredImage rim_occ_denoised_{};
    RegisteredImage bounce_half_{};
    RegisteredImage crease_mask_{};
    RegisteredImage hdr_scene_{};
    std::array<RegisteredImage, kBloomMipCount> bloom_mips_{};
    struct OutputSlotTargets {
        RegisteredImage bgra8{};
        RegisteredImage bgra8_straight{};
        RegisteredImage bottom_up_bgra8{};
        RegisteredImage nv12_y{};
        RegisteredImage nv12_uv{};
    };

    RegisteredImage out_rgba16f_{};
    RegisteredImage out_rgba8_scratch_{};
    RegisteredImage out_bgra8_{};
    RegisteredImage out_nv12_y_{};
    RegisteredImage out_nv12_uv_{};
    std::array<OutputSlotTargets, kOutputRingSlots> out_ring_{};
    RegisteredImage viewport_out_{};

    RegisteredImage prev_depth_{};
    RegisteredImage prev_normal_{};
    RegisteredImage prev_rt_shadow_{};
    RegisteredImage prev_rim_occ_{};

    // Static targets
    RegisteredImage orbit_orb_out_{};
    rhi::RhiImage orbit_orb_depth_{};
    VkImageLayout orbit_orb_depth_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
    RegisteredImage csm_depth_{};
    RegisteredImage blue_noise_64_{};

    // Shared pipeline layouts (`global` for all passes except RT, `rt` includes Set 1 TLAS)
    VkPipelineLayout pipe_layout_global_{VK_NULL_HANDLE};
    VkPipelineLayout pipe_layout_rt_{VK_NULL_HANDLE};

    VkPipeline pso_skin_morph_{VK_NULL_HANDLE};
    VkPipeline pso_prepass_{VK_NULL_HANDLE};
    VkPipeline pso_ray_effects_{VK_NULL_HANDLE};
    VkPipeline pso_csm_shadow_{VK_NULL_HANDLE};
    VkPipeline pso_csm_fallback_{VK_NULL_HANDLE};
    VkPipeline pso_live2d_opaque_{VK_NULL_HANDLE};
    VkPipeline pso_live2d_double_sided_{VK_NULL_HANDLE};
    VkPipeline pso_live2d_blend_{VK_NULL_HANDLE};
    VkPipeline pso_orbit_orb_{VK_NULL_HANDLE};
    VkPipeline pso_outline_hull_{VK_NULL_HANDLE};
    VkPipeline pso_crease_lines_{VK_NULL_HANDLE};
    VkPipeline pso_bloom_down_{VK_NULL_HANDLE};
    VkPipeline pso_bloom_up_{VK_NULL_HANDLE};
    VkPipeline pso_tone_composite_{VK_NULL_HANDLE};
    VkPipeline pso_encode_bgra_{VK_NULL_HANDLE};
    VkPipeline pso_encode_nv12_{VK_NULL_HANDLE};
    VkPipeline pso_viewport_compose_{VK_NULL_HANDLE};
    VkPipeline pso_heatmap_{VK_NULL_HANDLE};

    std::vector<uint32_t> opaque_draw_indices_{};
    std::vector<uint32_t> cutout_draw_indices_{};
    std::vector<uint32_t> transparent_draw_indices_{};
    std::array<GpuHeatmapBone, 128> heatmap_scratch_{};
    std::array<int64_t, 27> shader_mtimes_{};
    bool bgra8_supports_storage_{false};
    bool pipelines_ready_{false};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace aether::renderer
