#pragma once

#include "core/expected.hpp"
#include "renderer/asset_upload/gpu_avatar.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/gpu_structs.hpp"
#include "renderer/sdf_generator.hpp"
#include "rhi/d3d12_device.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12GraphicsCommandList4;

namespace aether::renderer {

/**
 * Complete DirectX 12 (`ID3D12Device5` + SM 6.6 DXIL + DXR Tier 1.1) 12-Pass NPR/Path-Tracing FrameGraph.
 * - Matches Vulkan `FrameGraph` coordinate contract (`proj.col1[1] = +fy`, positive D3D12 viewport,
 *   `FrontCounterClockwise = TRUE`, top-left UV `(0,0)`): strictly upright and unflipped.
 * - Supports 3-tier lighting (Traditional CSM+SH, Hybrid RT `TraceRayInline`, 2.5D Path Tracing + SVGF).
 * - Supports UMA zero-copy upload/readback on Intel iGPUs and D3D11On12 + shared NT handle Spout2/VCam export.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class Dx12FrameGraph {
  public:
    static constexpr uint32_t kBloomMipCount = 6u;
    static constexpr uint32_t kCsmSize = 2048u;

    Dx12FrameGraph() = default;
    ~Dx12FrameGraph() { destroy(); }

    Dx12FrameGraph(const Dx12FrameGraph&) = delete;
    Dx12FrameGraph& operator=(const Dx12FrameGraph&) = delete;

    [[nodiscard]] Expected<void> init(rhi::D3D12Device& device, uint32_t width, uint32_t height,
                                      uint32_t max_vertices = 131072u, bool enable_shared_output = true);
    [[nodiscard]] Expected<void> resize(uint32_t width, uint32_t height);
    void destroy() noexcept;

    [[nodiscard]] Expected<void> bind_avatar(const GpuAvatar* avatar);
    [[nodiscard]] Expected<void> ensure_upload_capacity(uint32_t vertex_count);
    [[nodiscard]] Expected<void> apply_face_sdf(const SdfBakeOutput& baked);

    /** Records and submits all DX12 rendering passes for `req` cleanly (zero per-frame heap allocs). */
    [[nodiscard]] Expected<uint64_t> render_frame(const FrameRenderRequest& req, bool wait_for_gpu = true) noexcept;

    /** Copies the rendered premultiplied-sRGB `out_rgba8_` frame into `out_rgba` (`w * h * 4` bytes). */
    [[nodiscard]] bool readback_rgba8(std::span<uint8_t> out_rgba) noexcept;
    /** Copies the rendered `out_bgra8_` frame into `out_bgra` (`w * h * 4` bytes, optional bottom-up flip). */
    [[nodiscard]] bool readback_bgra8(std::span<uint8_t> out_bgra, bool bottom_up = false) noexcept;

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool pipelines_ready() const noexcept { return pipelines_ready_; }
    [[nodiscard]] bool dxr_ready() const noexcept { return dxr_ready_; }
    [[nodiscard]] const GpuAvatar* bound_avatar() const noexcept { return bound_avatar_; }
    [[nodiscard]] uint8_t last_effective_lighting_mode() const noexcept { return last_eff_lighting_mode_; }
    [[nodiscard]] uint64_t rendered_frames() const noexcept { return rendered_frames_; }
    [[nodiscard]] float last_render_ms() const noexcept { return last_render_ms_; }

    [[nodiscard]] const rhi::D3D12Texture& out_bgra8() const noexcept { return out_bgra8_; }
    [[nodiscard]] const rhi::D3D12Texture& out_rgba8() const noexcept { return out_rgba8_; }
    [[nodiscard]] const rhi::D3D12Texture& out_rgba16f() const noexcept { return out_rgba16f_; }
    [[nodiscard]] void* shared_bgra8_nt_handle() const noexcept { return out_bgra8_.shared_nt_handle; }
    [[nodiscard]] void* shared_rgba8_nt_handle() const noexcept { return out_rgba8_.shared_nt_handle; }
    [[nodiscard]] void* shared_rgba16f_nt_handle() const noexcept { return out_rgba16f_.shared_nt_handle; }

  private:
    Expected<void> create_pipelines();
    void destroy_pipelines() noexcept;
    Expected<void> create_size_dependent_targets(uint32_t w, uint32_t h);
    void destroy_size_dependent_targets() noexcept;
    void destroy_avatar_resources() noexcept;
    void destroy_dxr_structures() noexcept;
    Expected<void> prepare_dxr_structures(const GpuAvatar& avatar);
    void record_dxr_build_or_refit(ID3D12GraphicsCommandList4* cmd, bool refit_only) noexcept;

    struct FrameUploadSlice {
        rhi::D3D12Buffer fg_buf{};            // GpuFrameGlobals (UploadCpu / UMA zero-copy)
        rhi::D3D12Buffer pose_buf{};          // AetherVertex25D[vertex_capacity] (UploadCpu / UMA zero-copy)
        rhi::D3D12Buffer tlas_inst{};         // 2 x D3D12_RAYTRACING_INSTANCE_DESC
        rhi::D3D12Buffer studio_lights_buf{}; // StudioLightGPU[16] StructuredBuffer
        uint32_t pose_vertex_cap{0};
    };

    rhi::D3D12Device* device_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t max_vertices_{131072u};
    bool enable_shared_output_{true};
    bool pipelines_ready_{false};
    bool dxr_ready_{false};
    bool dxr_has_built_once_{false};
    uint8_t last_eff_lighting_mode_{0};
    bool have_prev_view_proj_{false};
    GpuMat4 prev_view_proj_{};
    uint64_t rendered_frames_{0};
    float last_render_ms_{0.0f};

    std::array<FrameUploadSlice, rhi::kD3D12FrameCount> upload_slices_{};

    // Avatar static + skinned GPU buffers & textures
    const GpuAvatar* bound_avatar_{nullptr};
    uint32_t avatar_vertex_count_{0};
    rhi::D3D12Buffer skinned_vertices_buf_{};
    rhi::D3D12Buffer surface_buf_{};
    rhi::D3D12Buffer index_buf_{};
    rhi::D3D12Buffer material_buf_{};
    rhi::D3D12Texture fallback_white_tex_{};
    rhi::D3D12Texture ltc_inv_m_{};
    rhi::D3D12Texture ltc_amp_{};
    rhi::D3D12Texture ies_lut_{};
    rhi::D3D12Texture baked_face_sdf_tex_{};
    uint32_t avatar_face_sdf_slot_{rhi::kD3D12InvalidDescIndex};
    std::vector<rhi::D3D12Texture> avatar_textures_{};
    std::vector<uint32_t> avatar_tex_slots_{};
    std::vector<GpuMaterial> cached_materials_{};
    std::vector<GpuSubmeshDraw> cached_submeshes_{};
    std::vector<uint32_t> opaque_draws_{};
    std::vector<uint32_t> cutout_draws_{};
    std::vector<uint32_t> transparent_draws_{};

    // DXR Tier 1.1 4-Mask BLAS/TLAS buffers & scratch (FaceSkin 0x01, Hair 0x02, HandsProps 0x04, BodyCloth 0x08)
    struct DxrBlasBucket {
        rhi::D3D12Buffer buf{};
        uint32_t tri_count{0};
        uint32_t tri_offset{0};
        uint32_t geom_count{0};
        std::vector<uint8_t> geom_bytes{};
    };
    std::array<DxrBlasBucket, 4> blas_buckets_{};
    rhi::D3D12Buffer rt_index_buf_{};
    rhi::D3D12Buffer rt_tri_meta_buf_{};
    rhi::D3D12Buffer tlas_buf_{};
    rhi::D3D12Buffer dxr_scratch_buf_{};

    // Size-dependent render targets
    rhi::D3D12Texture depth_stencil_{};
    rhi::D3D12Texture csm_depth_{};
    rhi::D3D12Texture gb_n_{};
    rhi::D3D12Texture gb_mv_{};
    rhi::D3D12Texture gb_mat_{};
    rhi::D3D12Texture gb_misc_{};
    rhi::D3D12Texture prev_normal_{};

    rhi::D3D12Texture rt_shadow_raw_{};
    rhi::D3D12Texture rt_shadow_denoised_{};
    rhi::D3D12Texture prev_rt_shadow_{};
    rhi::D3D12Texture rim_occ_raw_{};
    rhi::D3D12Texture rim_occ_denoised_{};
    rhi::D3D12Texture prev_rim_occ_{};
    rhi::D3D12Texture bounce_half_{};
    rhi::D3D12Texture bounce_denoised_{};
    rhi::D3D12Texture prev_bounce_{};
    rhi::D3D12Texture aov_backscatter_{};
    rhi::D3D12Texture aov_cornea_spec_{};
    std::array<rhi::D3D12Buffer, 2> reservoir_bufs_{};
    std::array<StudioLightGPU, kMaxStudioLights> studio_lights_scratch_{};

    rhi::D3D12Texture hdr_scene_{};
    rhi::D3D12Texture crease_mask_{};
    std::array<rhi::D3D12Texture, kBloomMipCount> bloom_mips_{};
    rhi::D3D12Texture out_rgba16f_{};
    rhi::D3D12Texture out_rgba8_{};
    rhi::D3D12Texture out_bgra8_{};

    // CPU Readback buffers (mapped persistent; UMA zero-copy directly on integrated GPUs)
    rhi::D3D12Buffer readback_rgba8_buf_{};
    rhi::D3D12Buffer readback_bgra8_buf_{};
    uint32_t readback_row_pitch_{0};

    // PSOs (all compiled from SM 6.6 DXIL phase4_shaders_dxil.hpp)
    ID3D12PipelineState* pso_skin_morph_{nullptr};
    ID3D12PipelineState* pso_prepass_cull_{nullptr};
    ID3D12PipelineState* pso_prepass_nocull_{nullptr};
    ID3D12PipelineState* pso_csm_shadow_{nullptr};
    ID3D12PipelineState* pso_csm_fallback_{nullptr};
    ID3D12PipelineState* pso_ray_effects_{nullptr};
    ID3D12PipelineState* pso_denoise_svgf_{nullptr};
    ID3D12PipelineState* pso_denoise_sigma_{nullptr};
    ID3D12PipelineState* pso_denoise_ffx_{nullptr};
    ID3D12PipelineState* pso_npr_opaque_cull_{nullptr};
    ID3D12PipelineState* pso_npr_opaque_nocull_{nullptr};
    ID3D12PipelineState* pso_npr_blend_cull_{nullptr};
    ID3D12PipelineState* pso_npr_blend_nocull_{nullptr};
    ID3D12PipelineState* pso_npr_wire_{nullptr};
    ID3D12PipelineState* pso_outline_hull_{nullptr};
    ID3D12PipelineState* pso_crease_lines_{nullptr};
    ID3D12PipelineState* pso_bloom_down_{nullptr};
    ID3D12PipelineState* pso_bloom_up_{nullptr};
    ID3D12PipelineState* pso_tone_composite_{nullptr};
    ID3D12PipelineState* pso_encode_bgra_{nullptr};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aether::renderer
