#include "renderer/dx12_frame_graph.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"
#include "generated/phase4_shaders_dxil.hpp"
#include "renderer/avatar_rt_classifier.hpp"
#include "renderer/frame_globals_builder.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace aether::renderer {

namespace {

#if defined(_WIN32)
constexpr DXGI_FORMAT kDepthFmt = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kCsmFmt = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kNormalFmt = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kMvFmt = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kMatFmt = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT kMiscFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kRtFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kHdrFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kCreaseFmt = DXGI_FORMAT_R16_FLOAT;
constexpr DXGI_FORMAT kRgba8Fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kBgra8Fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

static_assert(sizeof(D3D12_RAYTRACING_GEOMETRY_DESC) > 0);
static_assert(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) == 64);

[[nodiscard]] DXGI_FORMAT map_dxgi_avatar_tex(uint32_t aether_fmt) noexcept {
    switch (aether_fmt) {
    case AETHER_TEX_FORMAT_RGBA8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case AETHER_TEX_FORMAT_RGBA8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case AETHER_TEX_FORMAT_BC7_UNORM:
        return DXGI_FORMAT_BC7_UNORM;
    case AETHER_TEX_FORMAT_BC7_SRGB:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void cmd_clear_rtv(rhi::D3D12Device& dev, ID3D12GraphicsCommandList4* cmd, const rhi::D3D12Texture& tex,
                   const float rgba[4]) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    dev.cpu_rtv_handle(tex.rtv_index, &h);
    cmd->ClearRenderTargetView(h, rgba, 0, nullptr);
}

void cmd_set_root_pc(ID3D12GraphicsCommandList4* cmd, bool compute, const GpuPushConstants& pc) noexcept {
    if (compute) {
        cmd->SetComputeRoot32BitConstants(0, 16, &pc, 0);
    } else {
        cmd->SetGraphicsRoot32BitConstants(0, 16, &pc, 0);
    }
}

[[nodiscard]] uint64_t align_up_u64(uint64_t v, uint64_t a) noexcept {
    return (a <= 1ULL) ? v : ((v + a - 1ULL) & ~(a - 1ULL));
}
#endif

} // namespace

Expected<void> Dx12FrameGraph::init(rhi::D3D12Device& device, uint32_t width, uint32_t height, uint32_t max_vertices,
                                    bool enable_shared_output) {
#if !defined(_WIN32)
    (void)device;
    (void)width;
    (void)height;
    (void)max_vertices;
    (void)enable_shared_output;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    destroy();
    if (!device.is_valid() || width == 0 || height == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::init invalid args"));
    }

    device_ = &device;
    max_vertices_ = std::max(max_vertices, 4096u);
    enable_shared_output_ = enable_shared_output;
    stage_overlay_scratch_.resize(4096u);
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }
    rendered_frames_ = 0;

    if (auto rc = create_pipelines(); !rc) {
        destroy();
        return rc;
    }
    if (auto rc = ensure_upload_capacity(max_vertices_); !rc) {
        destroy();
        return rc;
    }
    {
        auto sob = device_->create_buffer(
            4096ull * sizeof(GpuStageOverlayElem), rhi::D3D12BufferUsage::ShaderResource,
            rhi::D3D12BufferHeap::UploadCpu, true, false);
        if (!sob) {
            destroy();
            return make_unexpected(sob.error());
        }
        stage_overlay_buf_ = sob.value();
    }

    // 1x1 white fallback texture + 2048^2 CSM depth target
    const std::array<uint8_t, 4> white_px{255u, 255u, 255u, 255u};
    auto fw_res =
        device_->create_texture_2d_from_data(1u, 1u, static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), white_px, 4u);
    if (!fw_res) {
        destroy();
        return make_unexpected(fw_res.error());
    }
    fallback_white_tex_ = fw_res.value();

    auto csm_res = device_->create_texture_2d(kCsmSize, kCsmSize, static_cast<int>(kCsmFmt), false, true, false);
    if (!csm_res) {
        destroy();
        return make_unexpected(csm_res.error());
    }
    csm_depth_ = csm_res.value();

    // Heitz (2016) 64x64 LTC LUTs & 256x64 ANSI/IES LM-63-19 LUT (uploaded once at init)
    {
        std::array<float, kLtcLutSize * kLtcLutSize * 4> inv_m{};
        std::array<float, kLtcLutSize * kLtcLutSize * 4> amp{};
        generate_heitz_ltc_luts(inv_m, amp);
        const auto* inv_u8 = reinterpret_cast<const uint8_t*>(inv_m.data());
        const auto* amp_u8 = reinterpret_cast<const uint8_t*>(amp.data());
        auto inv_r = device_->create_texture_2d_from_data(
            kLtcLutSize, kLtcLutSize, static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT),
            std::span<const uint8_t>(inv_u8, inv_m.size() * sizeof(float)), kLtcLutSize * 16u);
        if (!inv_r) {
            destroy();
            return make_unexpected(inv_r.error());
        }
        ltc_inv_m_ = inv_r.value();

        auto amp_r = device_->create_texture_2d_from_data(
            kLtcLutSize, kLtcLutSize, static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT),
            std::span<const uint8_t>(amp_u8, amp.size() * sizeof(float)), kLtcLutSize * 16u);
        if (!amp_r) {
            destroy();
            return make_unexpected(amp_r.error());
        }
        ltc_amp_ = amp_r.value();
    }
    {
        std::array<float, kIesLutWidth * kIesLutRows> ies{};
        generate_ies_lm63_lut(ies);
        const auto* ies_u8 = reinterpret_cast<const uint8_t*>(ies.data());
        auto ies_r = device_->create_texture_2d_from_data(
            kIesLutWidth, kIesLutRows, static_cast<int>(DXGI_FORMAT_R32_FLOAT),
            std::span<const uint8_t>(ies_u8, ies.size() * sizeof(float)), kIesLutWidth * 4u);
        if (!ies_r) {
            destroy();
            return make_unexpected(ies_r.error());
        }
        ies_lut_ = ies_r.value();
    }

    if (auto rc = create_size_dependent_targets(width, height); !rc) {
        destroy();
        return rc;
    }
    return {};
#endif
}

Expected<void> Dx12FrameGraph::resize(uint32_t width, uint32_t height) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::resize invalid args"));
    }
    if (width == width_ && height == height_) {
        return {};
    }
    device_->wait_idle();
    return create_size_dependent_targets(width, height);
}

void Dx12FrameGraph::destroy() noexcept {
    if (device_ != nullptr) {
        device_->wait_idle();
        destroy_avatar_resources();
        destroy_size_dependent_targets();
        device_->destroy_texture(csm_depth_);
        device_->destroy_texture(ies_lut_);
        device_->destroy_texture(ltc_amp_);
        device_->destroy_texture(ltc_inv_m_);
        device_->destroy_texture(fallback_white_tex_);
        device_->destroy_buffer(stage_overlay_buf_);
        for (auto& sl : upload_slices_) {
            device_->destroy_buffer(sl.fg_buf);
            device_->destroy_buffer(sl.pose_buf);
            device_->destroy_buffer(sl.tlas_inst);
            device_->destroy_buffer(sl.studio_lights_buf);
            sl.pose_vertex_cap = 0;
        }
        destroy_pipelines();
    }
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }
}

Expected<void> Dx12FrameGraph::ensure_upload_capacity(uint32_t vertex_count) {
    if (device_ == nullptr) {
        return make_unexpected(
            Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::ensure_upload_capacity uninitialized"));
    }
    const uint32_t need = std::max(vertex_count, max_vertices_);
    const uint64_t pose_bytes = static_cast<uint64_t>(need) * sizeof(AetherVertex25D);
    bool need_wait_idle = false;
    for (const auto& sl : upload_slices_) {
        if (sl.pose_buf.resource != nullptr && sl.pose_vertex_cap < need) {
            need_wait_idle = true;
            break;
        }
    }
    if (need_wait_idle) {
        device_->wait_idle();
    }
    for (auto& sl : upload_slices_) {
        if (sl.fg_buf.resource == nullptr) {
            auto r = device_->create_buffer(4096u, rhi::D3D12BufferUsage::ShaderResource,
                                            rhi::D3D12BufferHeap::UploadCpu, true, false);
            if (!r) {
                return make_unexpected(r.error());
            }
            sl.fg_buf = r.value();
        }
        if (sl.tlas_inst.resource == nullptr) {
            auto r = device_->create_buffer(512u, rhi::D3D12BufferUsage::ShaderResource,
                                            rhi::D3D12BufferHeap::UploadCpu, false, false);
            if (!r) {
                return make_unexpected(r.error());
            }
            sl.tlas_inst = r.value();
        }
        if (sl.studio_lights_buf.resource == nullptr) {
            auto r = device_->create_buffer(static_cast<uint64_t>(kMaxStudioLights) * sizeof(StudioLightGPU),
                                            rhi::D3D12BufferUsage::ShaderResource, rhi::D3D12BufferHeap::UploadCpu,
                                            true, false);
            if (!r) {
                return make_unexpected(r.error());
            }
            sl.studio_lights_buf = r.value();
        }
        if (sl.pose_buf.resource == nullptr || sl.pose_vertex_cap < need) {
            device_->destroy_buffer(sl.pose_buf);
            auto r = device_->create_buffer(pose_bytes, rhi::D3D12BufferUsage::ShaderResource,
                                            rhi::D3D12BufferHeap::UploadCpu, true, false);
            if (!r) {
                return make_unexpected(r.error());
            }
            sl.pose_buf = r.value();
            sl.pose_vertex_cap = need;
        }
    }
    max_vertices_ = need;
    return {};
}

Expected<void> Dx12FrameGraph::create_pipelines() {
#if !defined(_WIN32)
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    using SId = shaders::Phase4ShaderId;
    auto build_cs = [&](SId id, bool rt, ID3D12PipelineState*& out) -> Expected<void> {
        const auto dxil = shaders::load_phase4_shader_dxil(id);
        auto res = device_->create_compute_pso(dxil, rt);
        if (!res) {
            return make_unexpected(res.error());
        }
        out = res.value();
        return {};
    };

    if (auto r = build_cs(SId::SkinMorph, false, pso_skin_morph_); !r)
        return r;
    if (auto r = build_cs(SId::CsmFallbackCs, false, pso_csm_fallback_); !r)
        return r;
    if (auto r = build_cs(SId::DenoiseSvgf, false, pso_denoise_svgf_); !r)
        return r;
    if (auto r = build_cs(SId::DenoiseSigma, false, pso_denoise_sigma_); !r)
        return r;
    if (auto r = build_cs(SId::DenoiseFfx, false, pso_denoise_ffx_); !r)
        return r;
    if (auto r = build_cs(SId::CreaseLines, false, pso_crease_lines_); !r)
        return r;
    if (auto r = build_cs(SId::BloomDown, false, pso_bloom_down_); !r)
        return r;
    if (auto r = build_cs(SId::BloomUp, false, pso_bloom_up_); !r)
        return r;
    if (auto r = build_cs(SId::ToneComposite, false, pso_tone_composite_); !r)
        return r;
    if (auto r = build_cs(SId::EncodeBgra, false, pso_encode_bgra_); !r)
        return r;

    if (device_->dxr_supported()) {
        if (auto r = build_cs(SId::RayEffects, true, pso_ray_effects_); !r) {
            AETHER_LOG_WARN("renderer", "DX12 RayEffects PSO unavailable: %s", r.error().message.data());
            pso_ray_effects_ = nullptr;
        }
    }

    // Prepass PSO (4 MRTs + D32S8 write + stencil replace, CCW front-face)
    {
        rhi::D3D12GraphicsPipelineDesc d{};
        d.vs_dxil = shaders::load_phase4_shader_dxil(SId::PrepassVs);
        d.ps_dxil = shaders::load_phase4_shader_dxil(SId::PrepassFs);
        d.num_render_targets = 4;
        d.rtv_formats[0] = static_cast<int>(kNormalFmt);
        d.rtv_formats[1] = static_cast<int>(kMvFmt);
        d.rtv_formats[2] = static_cast<int>(kMatFmt);
        d.rtv_formats[3] = static_cast<int>(kMiscFmt);
        d.dsv_format = static_cast<int>(kDepthFmt);
        d.depth_enable = true;
        d.depth_write = true;
        d.depth_func = D3D12_COMPARISON_FUNC_LESS;
        d.stencil_enable = true;
        d.stencil_pass_op = D3D12_STENCIL_OP_REPLACE;
        d.front_counter_clockwise = true;
        d.cull_mode = D3D12_CULL_MODE_BACK;
        auto r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_prepass_cull_ = r.value();

        d.cull_mode = D3D12_CULL_MODE_NONE;
        r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_prepass_nocull_ = r.value();
    }

    // CSM Shadow Depth PSO (depth-only 2048^2)
    {
        rhi::D3D12GraphicsPipelineDesc d{};
        d.vs_dxil = shaders::load_phase4_shader_dxil(SId::CsmShadowVs);
        d.ps_dxil = shaders::load_phase4_shader_dxil(SId::CsmShadowFs);
        d.num_render_targets = 0;
        d.dsv_format = static_cast<int>(kCsmFmt);
        d.depth_enable = true;
        d.depth_write = true;
        d.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.front_counter_clockwise = true;
        d.cull_mode = D3D12_CULL_MODE_NONE;
        auto r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_csm_shadow_ = r.value();
    }

    // Live2D NPR PSOs (RGBA16F HDR target + depth LESS_EQUAL read-only)
    {
        rhi::D3D12GraphicsPipelineDesc d{};
        d.vs_dxil = shaders::load_phase4_shader_dxil(SId::Live2dNprVs);
        d.ps_dxil = shaders::load_phase4_shader_dxil(SId::Live2dNprFs);
        d.num_render_targets = 1;
        d.rtv_formats[0] = static_cast<int>(kHdrFmt);
        d.dsv_format = static_cast<int>(kDepthFmt);
        d.depth_enable = true;
        d.depth_write = false;
        d.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.front_counter_clockwise = true;

        d.cull_mode = D3D12_CULL_MODE_BACK;
        auto r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_npr_opaque_cull_ = r.value();

        d.cull_mode = D3D12_CULL_MODE_NONE;
        r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_npr_opaque_nocull_ = r.value();

        d.wireframe = true;
        r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_npr_wire_ = r.value();
        d.wireframe = false;

        d.alpha_blend = true;
        d.premultiplied_alpha = true;
        d.cull_mode = D3D12_CULL_MODE_BACK;
        r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_npr_blend_cull_ = r.value();

        d.cull_mode = D3D12_CULL_MODE_NONE;
        r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_npr_blend_nocull_ = r.value();
    }

    // Inverted Hull Outline PSO (FrontCounterClockwise + CullMode FRONT)
    {
        rhi::D3D12GraphicsPipelineDesc d{};
        d.vs_dxil = shaders::load_phase4_shader_dxil(SId::OutlineHullVs);
        d.ps_dxil = shaders::load_phase4_shader_dxil(SId::OutlineHullFs);
        d.num_render_targets = 1;
        d.rtv_formats[0] = static_cast<int>(kHdrFmt);
        d.dsv_format = static_cast<int>(kDepthFmt);
        d.depth_enable = true;
        d.depth_write = false;
        d.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.front_counter_clockwise = true;
        d.cull_mode = D3D12_CULL_MODE_FRONT;
        auto r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_outline_hull_ = r.value();
    }

    // Lighting Stage Schematic-Only Overlay PSO (Stage View exclusively, alpha-blended onto RGBA8 RTV)
    {
        rhi::D3D12GraphicsPipelineDesc d{};
        d.vs_dxil = shaders::load_phase4_shader_dxil(SId::StageSchematicVs);
        d.ps_dxil = shaders::load_phase4_shader_dxil(SId::StageSchematicFs);
        d.num_render_targets = 1;
        d.rtv_formats[0] = static_cast<int>(kRgba8Fmt);
        d.dsv_format = static_cast<int>(DXGI_FORMAT_UNKNOWN);
        d.depth_enable = false;
        d.depth_write = false;
        d.cull_mode = D3D12_CULL_MODE_NONE;
        d.alpha_blend = true;
        d.premultiplied_alpha = true;
        auto r = device_->create_graphics_pso(d);
        if (!r)
            return make_unexpected(r.error());
        pso_stage_schematic_ = r.value();
    }

    pipelines_ready_ = true;
    return {};
#endif
}

void Dx12FrameGraph::destroy_pipelines() noexcept {
    using rhi::D3D12Device;
    D3D12Device::destroy_pso(pso_skin_morph_);
    D3D12Device::destroy_pso(pso_prepass_cull_);
    D3D12Device::destroy_pso(pso_prepass_nocull_);
    D3D12Device::destroy_pso(pso_csm_shadow_);
    D3D12Device::destroy_pso(pso_csm_fallback_);
    D3D12Device::destroy_pso(pso_ray_effects_);
    D3D12Device::destroy_pso(pso_denoise_svgf_);
    D3D12Device::destroy_pso(pso_denoise_sigma_);
    D3D12Device::destroy_pso(pso_denoise_ffx_);
    D3D12Device::destroy_pso(pso_npr_opaque_cull_);
    D3D12Device::destroy_pso(pso_npr_opaque_nocull_);
    D3D12Device::destroy_pso(pso_npr_blend_cull_);
    D3D12Device::destroy_pso(pso_npr_blend_nocull_);
    D3D12Device::destroy_pso(pso_npr_wire_);
    D3D12Device::destroy_pso(pso_outline_hull_);
    D3D12Device::destroy_pso(pso_crease_lines_);
    D3D12Device::destroy_pso(pso_bloom_down_);
    D3D12Device::destroy_pso(pso_bloom_up_);
    D3D12Device::destroy_pso(pso_tone_composite_);
    D3D12Device::destroy_pso(pso_encode_bgra_);
    D3D12Device::destroy_pso(pso_stage_schematic_);
    pipelines_ready_ = false;
}

Expected<void> Dx12FrameGraph::create_size_dependent_targets(uint32_t w, uint32_t h) {
#if !defined(_WIN32)
    (void)w;
    (void)h;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    destroy_size_dependent_targets();
    width_ = w;
    height_ = h;
    prev_stage_w_ = 0;
    prev_stage_h_ = 0;
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }

    const float clear_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float clear_norm[4] = {0.5f, 0.5f, 0.0f, 1.0f};

    auto ds_res = device_->create_depth_stencil_s8(w, h);
    if (!ds_res)
        return make_unexpected(ds_res.error());
    depth_stencil_ = ds_res.value();

    auto mk = [&](rhi::D3D12Texture& dst, uint32_t tw, uint32_t th, DXGI_FORMAT fmt, bool rtv, bool uav, bool shared,
                  bool d3d11, const float* cv) -> Expected<void> {
        auto r = device_->create_texture_2d(tw, th, static_cast<int>(fmt), rtv, false, uav, shared, d3d11, cv);
        if (!r)
            return make_unexpected(r.error());
        dst = r.value();
        return {};
    };

    if (auto r = mk(gb_n_, w, h, kNormalFmt, true, false, false, false, clear_norm); !r)
        return r;
    if (auto r = mk(gb_mv_, w, h, kMvFmt, true, false, false, false, clear_zero); !r)
        return r;
    if (auto r = mk(gb_mat_, w, h, kMatFmt, true, false, false, false, clear_zero); !r)
        return r;
    if (auto r = mk(gb_misc_, w, h, kMiscFmt, true, false, false, false, clear_zero); !r)
        return r;

    if (auto r = mk(rt_shadow_raw_, w, h, kRtFmt, false, true, false, false, nullptr); !r)
        return r;
    if (auto r = mk(rt_shadow_denoised_, w, h, kRtFmt, false, true, false, false, nullptr); !r)
        return r;

    if (auto r = mk(rim_occ_raw_, w, h, kRtFmt, false, true, false, false, nullptr); !r)
        return r;
    if (auto r = mk(rim_occ_denoised_, w, h, kRtFmt, false, true, false, false, nullptr); !r)
        return r;

    const uint32_t hw = std::max(1u, (w + 1u) / 2u);
    const uint32_t hh = std::max(1u, (h + 1u) / 2u);
    if (auto r = mk(bounce_half_, hw, hh, kHdrFmt, false, true, false, false, nullptr); !r)
        return r;
    if (auto r = mk(bounce_denoised_, hw, hh, kHdrFmt, false, true, false, false, nullptr); !r)
        return r;
    if (auto r = mk(aov_backscatter_, w, h, kHdrFmt, false, true, false, false, nullptr); !r)
        return r;
    if (auto r = mk(aov_cornea_spec_, w, h, kHdrFmt, false, true, false, false, nullptr); !r)
        return r;

    for (auto& hb : history_) {
        if (auto r = mk(hb.prev_normal, w, h, kNormalFmt, false, false, false, false, nullptr); !r)
            return r;
        if (auto r = mk(hb.prev_rt_shadow, w, h, kRtFmt, false, false, false, false, nullptr); !r)
            return r;
        if (auto r = mk(hb.prev_rim_occ, w, h, kRtFmt, false, false, false, false, nullptr); !r)
            return r;
        if (auto r = mk(hb.prev_bounce, hw, hh, kHdrFmt, false, false, false, false, nullptr); !r)
            return r;
        const uint64_t res_bytes = static_cast<uint64_t>(hw) * static_cast<uint64_t>(hh) * sizeof(GpuReSTIRReservoir);
        for (auto& rb : hb.reservoir_bufs) {
            auto br = device_->create_buffer(
                res_bytes, rhi::D3D12BufferUsage::UnorderedAccess | rhi::D3D12BufferUsage::ShaderResource,
                rhi::D3D12BufferHeap::DefaultGpu, true, true);
            if (!br)
                return make_unexpected(br.error());
            rb = br.value();
        }
    }

    if (auto r = mk(hdr_scene_, w, h, kHdrFmt, true, false, false, false, clear_zero); !r)
        return r;
    if (auto r = mk(crease_mask_, w, h, kCreaseFmt, false, true, false, false, nullptr); !r)
        return r;

    uint32_t mw = std::max(1u, w >> 1u);
    uint32_t mh = std::max(1u, h >> 1u);
    for (uint32_t m = 0; m < kBloomMipCount; ++m) {
        if (auto r = mk(bloom_mips_[m], mw, mh, kHdrFmt, false, true, false, false, nullptr); !r)
            return r;
        mw = std::max(1u, mw >> 1u);
        mh = std::max(1u, mh >> 1u);
    }

    if (auto r = mk(out_rgba16f_, w, h, kHdrFmt, false, true, enable_shared_output_, false, nullptr); !r)
        return r;
    if (auto r = mk(out_rgba8_, w, h, kRgba8Fmt, true, true, enable_shared_output_, false, nullptr); !r)
        return r;

    const bool bgra_uav = device_->gpu_info().supports_bgra8_uav;
    if (auto r = mk(out_bgra8_, w, h, kBgra8Fmt, !bgra_uav, bgra_uav, enable_shared_output_,
                    device_->d3d11on12_active(), nullptr);
        !r) {
        return r;
    }

    readback_row_pitch_ =
        (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const uint64_t rb_bytes = static_cast<uint64_t>(readback_row_pitch_) * h;
    auto rb1 = device_->create_buffer(rb_bytes, rhi::D3D12BufferUsage::CopyDst, rhi::D3D12BufferHeap::ReadbackCpu,
                                      false, false);
    if (!rb1)
        return make_unexpected(rb1.error());
    readback_rgba8_buf_ = rb1.value();

    auto rb2 = device_->create_buffer(rb_bytes, rhi::D3D12BufferUsage::CopyDst, rhi::D3D12BufferHeap::ReadbackCpu,
                                      false, false);
    if (!rb2)
        return make_unexpected(rb2.error());
    readback_bgra8_buf_ = rb2.value();
    return {};
#endif
}

void Dx12FrameGraph::destroy_size_dependent_targets() noexcept {
    if (device_ == nullptr) {
        return;
    }
    device_->destroy_buffer(readback_bgra8_buf_);
    device_->destroy_buffer(readback_rgba8_buf_);
    device_->destroy_texture(out_bgra8_);
    device_->destroy_texture(out_rgba8_);
    device_->destroy_texture(out_rgba16f_);
    for (auto& m : bloom_mips_) {
        device_->destroy_texture(m);
    }
    device_->destroy_texture(crease_mask_);
    device_->destroy_texture(hdr_scene_);
    for (auto& hb : history_) {
        for (auto& rb : hb.reservoir_bufs) {
            device_->destroy_buffer(rb);
        }
        device_->destroy_texture(hb.prev_bounce);
        device_->destroy_texture(hb.prev_rim_occ);
        device_->destroy_texture(hb.prev_rt_shadow);
        device_->destroy_texture(hb.prev_normal);
    }
    device_->destroy_texture(aov_cornea_spec_);
    device_->destroy_texture(aov_backscatter_);
    device_->destroy_texture(bounce_denoised_);
    device_->destroy_texture(bounce_half_);
    device_->destroy_texture(rim_occ_denoised_);
    device_->destroy_texture(rim_occ_raw_);
    device_->destroy_texture(rt_shadow_denoised_);
    device_->destroy_texture(rt_shadow_raw_);
    device_->destroy_texture(gb_misc_);
    device_->destroy_texture(gb_mat_);
    device_->destroy_texture(gb_mv_);
    device_->destroy_texture(gb_n_);
    device_->destroy_texture(depth_stencil_);
}

void Dx12FrameGraph::destroy_dxr_structures() noexcept {
    if (device_ != nullptr) {
        device_->destroy_buffer(dxr_scratch_buf_);
        device_->destroy_buffer(tlas_buf_);
        for (auto& b : blas_buckets_) {
            device_->destroy_buffer(b.buf);
        }
        device_->destroy_buffer(rt_index_buf_);
        device_->destroy_buffer(rt_tri_meta_buf_);
    }
    for (auto& b : blas_buckets_) {
        b.geom_bytes.clear();
        b.geom_count = 0;
        b.tri_count = 0;
        b.tri_offset = 0;
    }
    dxr_ready_ = false;
    dxr_has_built_once_ = false;
}

void Dx12FrameGraph::destroy_avatar_resources() noexcept {
    if (device_ == nullptr) {
        return;
    }
    destroy_dxr_structures();
    device_->destroy_texture(baked_face_sdf_tex_);
    for (auto& t : avatar_textures_) {
        device_->destroy_texture(t);
    }
    avatar_textures_.clear();
    avatar_tex_slots_.clear();
    device_->destroy_buffer(material_buf_);
    device_->destroy_buffer(material_grade_buf_);
    device_->destroy_buffer(index_buf_);
    device_->destroy_buffer(surface_buf_);
    device_->destroy_buffer(skinned_vertices_buf_);
    cached_materials_.clear();
    active_materials_scratch_.clear();
    active_grades_scratch_.clear();
    cached_submeshes_.clear();
    opaque_draws_.clear();
    cutout_draws_.clear();
    transparent_draws_.clear();
    avatar_face_sdf_slot_ = rhi::kD3D12InvalidDescIndex;
    avatar_vertex_count_ = 0;
    bound_avatar_ = nullptr;
    for (auto& hb : history_) {
        hb.have_prev_view_proj = false;
    }
}

Expected<void> Dx12FrameGraph::bind_avatar(const GpuAvatar* avatar) {
#if !defined(_WIN32)
    (void)avatar;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    if (device_ == nullptr) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph uninitialized"));
    }
    device_->wait_idle();
    destroy_avatar_resources();
    if (avatar == nullptr) {
        return {};
    }
    const uint32_t vtx_count = avatar->requirements.vertex_count;
    if (vtx_count == 0 || avatar->rest_indices.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::bind_avatar empty geometry"));
    }
    if (auto r = ensure_upload_capacity(vtx_count); !r) {
        return r;
    }

    avatar_vertex_count_ = vtx_count;
    auto sv_res = device_->create_buffer(static_cast<uint64_t>(vtx_count) * sizeof(GpuSkinnedVertex),
                                         rhi::D3D12BufferUsage::UnorderedAccess | rhi::D3D12BufferUsage::ShaderResource,
                                         rhi::D3D12BufferHeap::DefaultGpu, true, true);
    if (!sv_res)
        return make_unexpected(sv_res.error());
    skinned_vertices_buf_ = sv_res.value();

    // Surface buffer (optional)
    if (!avatar->rest_surfaces.empty()) {
        const uint64_t sbytes = static_cast<uint64_t>(avatar->rest_surfaces.size()) * sizeof(AetherVertexSurface);
        auto sr = device_->create_buffer(sbytes, rhi::D3D12BufferUsage::ShaderResource, rhi::D3D12BufferHeap::UploadCpu,
                                         true, false);
        if (!sr)
            return make_unexpected(sr.error());
        surface_buf_ = sr.value();
        std::memcpy(surface_buf_.mapped_ptr, avatar->rest_surfaces.data(), static_cast<size_t>(sbytes));
    }

    // Index buffer in DefaultGpu + synchronous immediate copy for optimal IA / DXR traversal
    const uint64_t idx_bytes = static_cast<uint64_t>(avatar->rest_indices.size()) * sizeof(uint32_t);
    auto ib_res = device_->create_buffer(idx_bytes, rhi::D3D12BufferUsage::IndexBuffer | rhi::D3D12BufferUsage::CopyDst,
                                         rhi::D3D12BufferHeap::DefaultGpu, false, false);
    if (!ib_res)
        return make_unexpected(ib_res.error());
    index_buf_ = ib_res.value();

    auto ib_stg = device_->create_buffer(idx_bytes, rhi::D3D12BufferUsage::CopySrc, rhi::D3D12BufferHeap::UploadCpu,
                                         false, false);
    if (!ib_stg)
        return make_unexpected(ib_stg.error());
    std::memcpy(ib_stg.value().mapped_ptr, avatar->rest_indices.data(), static_cast<size_t>(idx_bytes));

    auto copy_rc = device_->execute_immediate([&](ID3D12GraphicsCommandList4* cmd) {
        device_->cmd_transition_buffer(cmd, index_buf_, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(index_buf_.resource, 0, ib_stg.value().resource, 0, idx_bytes);
        device_->cmd_transition_buffer(
            cmd, index_buf_, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    });
    device_->destroy_buffer(ib_stg.value());
    if (!copy_rc)
        return copy_rc;

    // Upload all avatar textures from retained CPU pixel buffer
    avatar_tex_slots_.assign(avatar->texture_descs.size(), fallback_white_tex_.srv_index);
    avatar_textures_.resize(avatar->texture_descs.size());
    if (!avatar->cpu_texture_pixels.empty()) {
        for (size_t i = 0; i < avatar->texture_descs.size(); ++i) {
            const AetherTextureDesc& td = avatar->texture_descs[i];
            if (td.pixel_size == 0 || td.pixel_offset + td.pixel_size > avatar->cpu_texture_pixels.size()) {
                continue;
            }
            const DXGI_FORMAT fmt = map_dxgi_avatar_tex(td.format);
            std::span<const uint8_t> pix(avatar->cpu_texture_pixels.data() + td.pixel_offset, td.pixel_size);
            const uint32_t tw = std::max(1u, td.width);
            const uint32_t th = std::max(1u, td.height);
            auto t_res = device_->create_texture_2d_from_data(tw, th, static_cast<int>(fmt), pix, 0u);
            if (!t_res) {
                return make_unexpected(t_res.error());
            }
            avatar_textures_[i] = t_res.value();
            avatar_tex_slots_[i] = avatar_textures_[i].srv_index;
            if ((td.usage_flags & AETHER_TEX_USAGE_SDF_FACE) != 0u &&
                avatar_face_sdf_slot_ == rhi::kD3D12InvalidDescIndex) {
                avatar_face_sdf_slot_ = avatar_textures_[i].srv_index;
            }
        }
    }

    const uint32_t sdf_fallback =
        (avatar_face_sdf_slot_ != rhi::kD3D12InvalidDescIndex) ? avatar_face_sdf_slot_ : UINT32_MAX;
    detail::populate_materials_and_submeshes(*avatar, avatar_tex_slots_, fallback_white_tex_.srv_index, sdf_fallback,
                                             cached_materials_, cached_submeshes_, opaque_draws_, cutout_draws_,
                                             transparent_draws_);
    if (!avatar->submeshes.empty()) {
        auto by_queue = [&](uint32_t a, uint32_t b) {
            return avatar->submeshes[a].render_queue_offset < avatar->submeshes[b].render_queue_offset;
        };
        std::stable_sort(opaque_draws_.begin(), opaque_draws_.end(), by_queue);
        std::stable_sort(cutout_draws_.begin(), cutout_draws_.end(), by_queue);
        std::stable_sort(transparent_draws_.begin(), transparent_draws_.end(), by_queue);
    }

    const uint64_t mat_bytes = static_cast<uint64_t>(cached_materials_.size()) * sizeof(GpuMaterial);
    auto mat_res = device_->create_buffer(mat_bytes, rhi::D3D12BufferUsage::ShaderResource,
                                          rhi::D3D12BufferHeap::UploadCpu, true, false);
    if (!mat_res)
        return make_unexpected(mat_res.error());
    material_buf_ = mat_res.value();
    std::memcpy(material_buf_.mapped_ptr, cached_materials_.data(), static_cast<size_t>(mat_bytes));

    active_materials_scratch_ = cached_materials_;
    active_grades_scratch_.assign(cached_materials_.size(), GpuMaterialGrade{});
    const uint64_t grade_bytes = static_cast<uint64_t>(active_grades_scratch_.size()) * sizeof(GpuMaterialGrade);
    auto grade_res = device_->create_buffer(grade_bytes, rhi::D3D12BufferUsage::ShaderResource,
                                            rhi::D3D12BufferHeap::UploadCpu, true, false);
    if (!grade_res)
        return make_unexpected(grade_res.error());
    material_grade_buf_ = grade_res.value();
    std::memcpy(material_grade_buf_.mapped_ptr, active_grades_scratch_.data(), static_cast<size_t>(grade_bytes));

    if (device_->dxr_supported() && pso_ray_effects_ != nullptr) {
        if (auto dr = prepare_dxr_structures(*avatar); !dr) {
            AETHER_LOG_WARN("renderer", "DX12 DXR BLAS/TLAS setup skipped: %s", dr.error().message.data());
            destroy_dxr_structures();
        }
    }

    bound_avatar_ = avatar;
    return {};
#endif
}

Expected<void> Dx12FrameGraph::apply_face_sdf(const SdfBakeOutput& baked) {
#if !defined(_WIN32)
    (void)baked;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    if (device_ == nullptr || baked.width == 0 || baked.height == 0 || baked.r8_pixels.empty()) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::apply_face_sdf invalid args"));
    }
    device_->wait_idle();
    auto tex_res = device_->create_texture_2d_from_data(
        baked.width, baked.height, static_cast<int>(DXGI_FORMAT_R8_UNORM), baked.r8_pixels, baked.width);
    if (!tex_res)
        return make_unexpected(tex_res.error());
    device_->destroy_texture(baked_face_sdf_tex_);
    baked_face_sdf_tex_ = tex_res.value();
    avatar_face_sdf_slot_ = baked_face_sdf_tex_.srv_index;

    std::vector<bool> target_mat(cached_materials_.size(), false);
    bool any_face_mat = false;
    for (const auto& sm : cached_submeshes_) {
        if ((sm.flags & AETHER_SUBMESH_FLAG_FACE) != 0u && sm.material_index < target_mat.size()) {
            target_mat[sm.material_index] = true;
            any_face_mat = true;
        }
    }
    for (size_t i = 0; i < cached_materials_.size(); ++i) {
        const uint32_t mflags = cached_materials_[i].extra_slots[2];
        if ((mflags & (AETHER_MAT_FLAG_SDF_FACE | AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW)) != 0u &&
            (mflags & AETHER_MAT_FLAG_HAIR_FRONT_STENCIL) == 0u) {
            target_mat[i] = true;
            any_face_mat = true;
        }
    }
    for (size_t i = 0; i < cached_materials_.size(); ++i) {
        if (!any_face_mat || target_mat[i]) {
            cached_materials_[i].tex_slots[2] = avatar_face_sdf_slot_;
            cached_materials_[i].extra_slots[2] |= (AETHER_MAT_FLAG_SDF_FACE | kMatExtraAuthoredSdfBit);
        }
    }
    if (material_buf_.mapped_ptr != nullptr && !cached_materials_.empty()) {
        std::memcpy(material_buf_.mapped_ptr, cached_materials_.data(), cached_materials_.size() * sizeof(GpuMaterial));
    }
    return {};
#endif
}

Expected<void> Dx12FrameGraph::prepare_dxr_structures(const GpuAvatar& avatar) {
    (void)avatar;
#if !defined(_WIN32)
    return {};
#else
    destroy_dxr_structures();
    std::array<std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>, 4> bucket_descs;

    auto push_geom = [&](size_t bi, uint64_t ib_gpu_va, uint32_t idx_offset, uint32_t idx_count) {
        if (idx_count < 3u || bi >= 4u) {
            return;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC gd{};
        gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        gd.Flags = (bi == 1u || bi == 3u) ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        gd.Triangles.Transform3x4 = 0;
        gd.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        gd.Triangles.IndexCount = idx_count;
        gd.Triangles.VertexCount = avatar_vertex_count_;
        gd.Triangles.IndexBuffer = ib_gpu_va + static_cast<uint64_t>(idx_offset) * sizeof(uint32_t);
        gd.Triangles.VertexBuffer.StartAddress = skinned_vertices_buf_.gpu_va;
        gd.Triangles.VertexBuffer.StrideInBytes = sizeof(GpuSkinnedVertex);
        bucket_descs[bi].push_back(gd);
        blas_buckets_[bi].tri_count += idx_count / 3u;
    };

    const RtPartitionedIndices part = build_rt_partitioned_indices(avatar);
    if (!part.sorted_indices.empty()) {
        const uint64_t rt_idx_bytes = static_cast<uint64_t>(part.sorted_indices.size()) * sizeof(uint32_t);
        auto ib_res =
            device_->create_buffer(rt_idx_bytes, rhi::D3D12BufferUsage::IndexBuffer | rhi::D3D12BufferUsage::CopyDst,
                                   rhi::D3D12BufferHeap::DefaultGpu, false, false);
        if (!ib_res)
            return make_unexpected(ib_res.error());
        rt_index_buf_ = ib_res.value();

        auto stg = device_->create_buffer(rt_idx_bytes, rhi::D3D12BufferUsage::CopySrc, rhi::D3D12BufferHeap::UploadCpu,
                                          false, false);
        if (!stg)
            return make_unexpected(stg.error());
        std::memcpy(stg.value().mapped_ptr, part.sorted_indices.data(), static_cast<size_t>(rt_idx_bytes));
        auto copy_rc = device_->execute_immediate([&](ID3D12GraphicsCommandList4* cmd) {
            device_->cmd_transition_buffer(cmd, rt_index_buf_, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(rt_index_buf_.resource, 0, stg.value().resource, 0, rt_idx_bytes);
            device_->cmd_transition_buffer(cmd, rt_index_buf_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        });
        device_->destroy_buffer(stg.value());
        if (!copy_rc)
            return copy_rc;

        if (!part.tri_meta.empty()) {
            const uint64_t tm_bytes = static_cast<uint64_t>(part.tri_meta.size()) * sizeof(RtTriMeta);
            auto tm_res = device_->create_buffer(tm_bytes, rhi::D3D12BufferUsage::ShaderResource,
                                                 rhi::D3D12BufferHeap::UploadCpu, true, false);
            if (!tm_res)
                return make_unexpected(tm_res.error());
            rt_tri_meta_buf_ = tm_res.value();
            std::memcpy(rt_tri_meta_buf_.mapped_ptr, part.tri_meta.data(), static_cast<size_t>(tm_bytes));
        }

        for (size_t b = 0; b < 4u; ++b) {
            if (part.bucket_index_count[b] >= 3u) {
                blas_buckets_[b].tri_offset = part.bucket_index_offset[b] / 3u;
                push_geom(b, rt_index_buf_.gpu_va, part.bucket_index_offset[b], part.bucket_index_count[b]);
            }
        }
    } else {
        auto push_list = [&](const std::vector<uint32_t>& draws) {
            for (const uint32_t idx : draws) {
                const auto& sm = cached_submeshes_[idx];
                const auto& gm = cached_materials_[sm.material_index];
                const uint32_t rtb = (gm.extra_slots[2] & kMatExtraRtClassMask) >> kMatExtraRtClassShift;
                push_geom(std::min<size_t>(rtb, 3u), index_buf_.gpu_va, sm.index_offset, sm.index_count);
            }
        };
        push_list(opaque_draws_);
        push_list(cutout_draws_);
    }

    UINT active_inst_count = 0u;
    for (size_t b = 0; b < 4u; ++b) {
        if (!bucket_descs[b].empty()) {
            ++active_inst_count;
        }
    }
    if (active_inst_count == 0u) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "No RT triangles for DX12 BLAS"));
    }

    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS build_flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    uint64_t scratch_max = 4096u;
    for (size_t b = 0; b < 4u; ++b) {
        auto& bk = blas_buckets_[b];
        const auto& descs = bucket_descs[b];
        if (descs.empty()) {
            continue;
        }
        bk.geom_count = static_cast<uint32_t>(descs.size());
        bk.geom_bytes.resize(descs.size() * sizeof(D3D12_RAYTRACING_GEOMETRY_DESC));
        std::memcpy(bk.geom_bytes.data(), descs.data(), bk.geom_bytes.size());

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = build_flags;
        in.NumDescs = bk.geom_count;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = descs.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device_->device()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        const uint64_t bytes = align_up_u64(info.ResultDataMaxSizeInBytes, 256u);
        auto res = device_->create_buffer(bytes, rhi::D3D12BufferUsage::AccelStructStorage,
                                          rhi::D3D12BufferHeap::DefaultGpu, false, false);
        if (!res)
            return make_unexpected(res.error());
        bk.buf = res.value();
        scratch_max = std::max({scratch_max, info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes});
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_in{};
    tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_in.NumDescs = active_inst_count;
    tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
    device_->device()->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_in, &tlas_info);
    auto tb = device_->create_buffer(align_up_u64(tlas_info.ResultDataMaxSizeInBytes, 256u),
                                     rhi::D3D12BufferUsage::AccelStructStorage, rhi::D3D12BufferHeap::DefaultGpu, false,
                                     false);
    if (!tb)
        return make_unexpected(tb.error());
    tlas_buf_ = tb.value();
    scratch_max = std::max({scratch_max, tlas_info.ScratchDataSizeInBytes, tlas_info.UpdateScratchDataSizeInBytes});

    auto sb = device_->create_buffer(align_up_u64(scratch_max, 256u), rhi::D3D12BufferUsage::UnorderedAccess,
                                     rhi::D3D12BufferHeap::DefaultGpu, false, false);
    if (!sb)
        return make_unexpected(sb.error());
    dxr_scratch_buf_ = sb.value();
    dxr_ready_ = true;
    dxr_has_built_once_ = false;
    return {};
#endif
}

void Dx12FrameGraph::record_dxr_build_or_refit(ID3D12GraphicsCommandList4* cmd, bool refit_only) noexcept {
#if defined(_WIN32)
    if (!dxr_ready_ || cmd == nullptr) {
        return;
    }
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS base_flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags =
        refit_only ? (base_flags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) : base_flags;

    for (size_t b = 0; b < 4u; ++b) {
        const auto& bk = blas_buckets_[b];
        if (bk.geom_count == 0u || bk.buf.resource == nullptr) {
            continue;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.DestAccelerationStructureData = bk.buf.gpu_va;
        bd.SourceAccelerationStructureData = refit_only ? bk.buf.gpu_va : 0ULL;
        bd.ScratchAccelerationStructureData = dxr_scratch_buf_.gpu_va;
        bd.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bd.Inputs.Flags = flags;
        bd.Inputs.NumDescs = bk.geom_count;
        bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bd.Inputs.pGeometryDescs = reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC*>(bk.geom_bytes.data());
        cmd->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        device_->cmd_uav_barrier(cmd, bk.buf.resource);
    }

    // Write TLAS instances into current upload slice (`0x01` FaceSkin, `0x02` Hair, `0x04` HandsProps, `0x08`
    // BodyCloth)
    const uint32_t fslot = static_cast<uint32_t>(rendered_frames_ % rhi::kD3D12FrameCount);
    FrameUploadSlice& sl = upload_slices_[fslot];
    auto* insts = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(sl.tlas_inst.mapped_ptr);
    UINT inst_count = 0;
    for (size_t b = 0; b < 4u; ++b) {
        const auto& bk = blas_buckets_[b];
        if (bk.geom_count == 0u || bk.buf.resource == nullptr) {
            continue;
        }
        D3D12_RAYTRACING_INSTANCE_DESC& d = insts[inst_count++];
        std::memset(&d, 0, sizeof(d));
        d.Transform[0][0] = 1.0f;
        d.Transform[1][1] = 1.0f;
        d.Transform[2][2] = 1.0f;
        d.InstanceID = bk.tri_offset;
        d.InstanceMask = bucket_to_instance_mask(static_cast<RtMaskBucket>(b));
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        d.AccelerationStructure = bk.buf.gpu_va;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td{};
    td.DestAccelerationStructureData = tlas_buf_.gpu_va;
    td.SourceAccelerationStructureData = refit_only ? tlas_buf_.gpu_va : 0ULL;
    td.ScratchAccelerationStructureData = dxr_scratch_buf_.gpu_va;
    td.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    td.Inputs.Flags = flags;
    td.Inputs.NumDescs = inst_count;
    td.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    td.Inputs.InstanceDescs = sl.tlas_inst.gpu_va;
    cmd->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    device_->cmd_uav_barrier(cmd, tlas_buf_.resource);
    dxr_has_built_once_ = true;
#else
    (void)cmd;
    (void)refit_only;
#endif
}

Expected<uint64_t> Dx12FrameGraph::render_frame(const FrameRenderRequest& req, bool wait_for_gpu) noexcept {
#if !defined(_WIN32)
    (void)req;
    (void)wait_for_gpu;
    return make_unexpected(Error::make(ErrorCode::Unsupported, "DX12 is Windows-only"));
#else
    if (device_ == nullptr || !pipelines_ready_ || req.rig == nullptr || width_ == 0 || height_ == 0) {
        return make_unexpected(Error::make(ErrorCode::InvalidArg, "Dx12FrameGraph::render_frame invalid state"));
    }

    const uint64_t t0_us = now_us();
    uint32_t frame_slot = 0u;
    auto cmd_res = device_->begin_direct_frame(frame_slot);
    if (!cmd_res) {
        return make_unexpected(cmd_res.error());
    }
    ID3D12GraphicsCommandList4* cmd = cmd_res.value();
    FrameUploadSlice& sl = upload_slices_[frame_slot % rhi::kD3D12FrameCount];

    const uint32_t buf_w = width_;
    const uint32_t buf_h = height_;
    const LightingRig& rig = *req.rig;
    const bool has_avatar = (req.avatar != nullptr && bound_avatar_ == req.avatar && avatar_vertex_count_ > 0);
    const bool rt_hardware_ready = device_->dxr_supported() && dxr_ready_ && pso_ray_effects_ != nullptr;

    const bool is_stage_view = (req.view_kind == RenderViewKind::LightingStage);
    const detail::StageRenderRegion sreg =
        is_stage_view
            ? detail::compute_stage_render_region(req, buf_w, buf_h, buf_w, buf_h)
            : detail::StageRenderRegion{buf_w, buf_h, buf_w, buf_h,
                                        static_cast<float>(buf_w) / static_cast<float>(buf_h),
                                        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const uint32_t w = sreg.scene_w;
    const uint32_t h = sreg.scene_h;
    const uint32_t gx = (w + 7u) / 8u;
    const uint32_t gy = (h + 7u) / 8u;

    const bool allow_view_hist =
        !is_stage_view || (req.stage_output_slot == FrameRenderRequest::StageOutputSlot::MainStage);
    const uint32_t view_bank = is_stage_view ? 1u : 0u;
    TemporalHistoryBank& hb = history_[view_bank];
    const bool stage_size_changed = is_stage_view && allow_view_hist && (w != prev_stage_w_ || h != prev_stage_h_);
    if (is_stage_view && allow_view_hist) {
        prev_stage_w_ = w;
        prev_stage_h_ = h;
    }
    const bool fast_stage_pan = is_stage_view && req.stage_fast_panning;
    const bool hist_valid = allow_view_hist && hb.have_prev_view_proj && !fast_stage_pan && !stage_size_changed;

    const uint32_t phys_hw = std::max(1u, (buf_w + 1u) / 2u);
    const uint32_t phys_hh = std::max(1u, (buf_h + 1u) / 2u);
    const uint32_t act_hw = std::min(phys_hw, std::max(1u, (w + 1u) / 2u));
    const uint32_t act_hh = std::min(phys_hh, std::max(1u, (h + 1u) / 2u));
    const std::array<float, 4> uv_scale{sreg.uv_scale_x, sreg.uv_scale_y, sreg.uv_max_u, sreg.uv_max_v};
    const std::array<float, 4> half_uv_scale{
        is_stage_view ? (static_cast<float>(act_hw) / static_cast<float>(phys_hw)) : 1.0f,
        is_stage_view ? (static_cast<float>(act_hh) / static_cast<float>(phys_hh)) : 1.0f,
        is_stage_view ? std::max(0.0f, (static_cast<float>(act_hw) - 0.5f) / static_cast<float>(phys_hw)) : 1.0f,
        is_stage_view ? std::max(0.0f, (static_cast<float>(act_hh) - 0.5f) / static_cast<float>(phys_hh)) : 1.0f};

    GpuFrameGlobals fg{};
    GpuMat4 next_prev_vp{};
    const uint8_t eff_mode = detail::populate_shared_frame_globals(
        req, w, h, has_avatar, rt_hardware_ready, hist_valid, hb.prev_view_proj, fg, next_prev_vp,
        is_stage_view ? sreg.proj_aspect : 0.0f, &uv_scale, &half_uv_scale);
    last_eff_lighting_mode_ = eff_mode;
    const bool use_rt = (eff_mode != 0u);
    if (has_avatar && allow_view_hist) {
        hb.prev_view_proj = next_prev_vp;
    }

    fg.gbuffer_ids[0] = gb_n_.srv_index;
    fg.gbuffer_ids[1] = gb_mv_.srv_index;
    fg.gbuffer_ids[2] = gb_mat_.srv_index;
    fg.gbuffer_ids[3] = gb_misc_.srv_index;
    fg.gbuffer_ids[4] = rt_shadow_raw_.srv_index;
    fg.gbuffer_ids[5] = crease_mask_.srv_index;
    fg.gbuffer_ids[6] = bloom_mips_[0].srv_index;
    fg.gbuffer_ids[7] = hdr_scene_.srv_index;

    fg.gbuffer_extra_ids[0] = depth_stencil_.srv_index;
    fg.gbuffer_extra_ids[1] = depth_stencil_.stencil_srv_index;
    fg.gbuffer_extra_ids[2] = rt_shadow_denoised_.srv_index;
    fg.gbuffer_extra_ids[3] = rim_occ_denoised_.srv_index;
    fg.gbuffer_extra_ids[4] = bounce_denoised_.srv_index;
    fg.gbuffer_extra_ids[5] = csm_depth_.srv_index;
    fg.gbuffer_extra_ids[6] = depth_stencil_.srv_index;
    fg.gbuffer_extra_ids[7] = hb.prev_normal.srv_index;

    fg.history_ids[0] = hb.prev_rt_shadow.srv_index;
    fg.history_ids[1] = hb.prev_rim_occ.srv_index;
    fg.history_ids[2] = rim_occ_raw_.srv_index;
    fg.history_ids[3] = depth_stencil_.srv_index;

    fg.output_storage_ids[0] = out_rgba16f_.uav_index;
    fg.output_storage_ids[1] =
        (out_bgra8_.uav_index != rhi::kD3D12InvalidDescIndex) ? out_bgra8_.uav_index : out_rgba8_.uav_index;
    fg.output_storage_ids[2] = UINT32_MAX;
    fg.output_storage_ids[3] = UINT32_MAX;

    const std::array<float, 3> head_target_ws = {fg.neck_seam_head_str[0], fg.neck_seam_head_str[1],
                                                 fg.neck_seam_head_str[2]};
    const std::array<float, 3> eye_pos_ws = {fg.camera_pos_world[0], fg.camera_pos_world[1],
                                             fg.camera_pos_world[2]};
    {
        const LightingRig& active_rig = *req.rig;
        const uint32_t sl_count =
            pack_studio_lights_gpu(active_rig, head_target_ws, eye_pos_ws, studio_lights_scratch_);
        if (sl.studio_lights_buf.mapped_ptr != nullptr) {
            std::memcpy(sl.studio_lights_buf.mapped_ptr, studio_lights_scratch_.data(),
                        sizeof(StudioLightGPU) * kMaxStudioLights);
        }
        fg.studio_lights_bda = sl.studio_lights_buf.srv_handle();
        fg.studio_slots0[0] = ltc_inv_m_.srv_index;
        fg.studio_slots0[1] = ltc_amp_.srv_index;
        fg.studio_slots0[2] = ies_lut_.srv_index;
        fg.studio_slots0[3] = sl_count;
    }
    fg.studio_slots1[0] = rt_shadow_denoised_.srv_index;
    fg.studio_slots1[1] = bounce_denoised_.srv_index;
    fg.studio_slots1[2] = aov_backscatter_.srv_index;
    fg.studio_slots1[3] = aov_cornea_spec_.srv_index;
    fg.studio_uav_slots[0] = rt_shadow_raw_.uav_index;
    fg.studio_uav_slots[1] = bounce_half_.uav_index;
    fg.studio_uav_slots[2] = aov_backscatter_.uav_index;
    fg.studio_uav_slots[3] = aov_cornea_spec_.uav_index;
    const uint32_t cur_res = static_cast<uint32_t>(req.frame_index & 1ULL);
    fg.reservoir_curr_bda = hb.reservoir_bufs[cur_res].uav_handle();
    fg.reservoir_prev_bda = hb.reservoir_bufs[1u - cur_res].srv_handle();

    if (has_avatar && !cached_materials_.empty()) {
        detail::update_gpu_materials_with_overrides(*req.avatar, cached_materials_, req.material_overrides,
                                                    active_materials_scratch_, active_grades_scratch_);
        if (material_buf_.mapped_ptr != nullptr) {
            std::memcpy(material_buf_.mapped_ptr, active_materials_scratch_.data(),
                        active_materials_scratch_.size() * sizeof(GpuMaterial));
        }
        if (material_grade_buf_.mapped_ptr != nullptr) {
            std::memcpy(material_grade_buf_.mapped_ptr, active_grades_scratch_.data(),
                        active_grades_scratch_.size() * sizeof(GpuMaterialGrade));
        }
        fg.material_grade_bda = material_grade_buf_.srv_handle();
    }

    std::memcpy(sl.fg_buf.mapped_ptr, &fg, sizeof(fg));
    const uint64_t fg_handle = sl.fg_buf.srv_handle();

    if (has_avatar) {
        // Upload live posed vertices to UploadCpu buffer (true zero-copy directly readable on Intel iGPU / UMA!)
        const uint32_t copy_verts = std::min<uint32_t>(avatar_vertex_count_, sl.pose_vertex_cap);
        const AetherVertex25D* src_verts = req.avatar->skinned_vertices_scratch.empty()
                                               ? req.avatar->rest_vertices.data()
                                               : req.avatar->skinned_vertices_scratch.data();
        if (src_verts != nullptr && copy_verts > 0) {
            std::memcpy(sl.pose_buf.mapped_ptr, src_verts, static_cast<size_t>(copy_verts) * sizeof(AetherVertex25D));
        }

        const bool reuse_skin_and_tlas = req.skip_skin_and_blas && (!use_rt || dxr_has_built_once_);
        if (!reuse_skin_and_tlas) {
            // Pass 1: Compute Skin/Pack -> GpuSkinnedVertex
            device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
            device_->cmd_transition_buffer(cmd, skinned_vertices_buf_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(pso_skin_morph_);
            GpuPushConstants pc{};
            pc.frame_globals_bda = fg_handle;
            pc.draw_data_bda = sl.pose_buf.srv_handle();
            pc.vertex_buf_bda = skinned_vertices_buf_.uav_handle();
            pc.aux_buf_bda = (surface_buf_.resource != nullptr) ? surface_buf_.srv_handle() : 0ULL;
            pc.param0 = avatar_vertex_count_;
            pc.param1 = hist_valid ? 1u : 0u;
            cmd_set_root_pc(cmd, true, pc);
            cmd->Dispatch((avatar_vertex_count_ + 63u) / 64u, 1u, 1u);
            device_->cmd_uav_barrier(cmd, skinned_vertices_buf_.resource);
            device_->cmd_transition_buffer(cmd, skinned_vertices_buf_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

            // Pass 2: DXR Tier 1.1 BLAS/TLAS build or refit
            if (use_rt) {
                const bool refit_only = dxr_has_built_once_ && ((rendered_frames_ % 300ull) != 0ull);
                record_dxr_build_or_refit(cmd, refit_only);
            }
        }

        // Pass 3: Depth + Stencil + G-Buffer Prepass (Strict positive D3D12 viewport + CCW front-face)
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        device_->cmd_transition_texture(cmd, gb_n_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        device_->cmd_transition_texture(cmd, gb_mv_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        device_->cmd_transition_texture(cmd, gb_mat_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        device_->cmd_transition_texture(cmd, gb_misc_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        device_->cmd_transition_texture(cmd, depth_stencil_, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        const float cz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float cn[4] = {0.5f, 0.5f, 0.0f, 1.0f};
        cmd_clear_rtv(*device_, cmd, gb_n_, cn);
        cmd_clear_rtv(*device_, cmd, gb_mv_, cz);
        cmd_clear_rtv(*device_, cmd, gb_mat_, cz);
        cmd_clear_rtv(*device_, cmd, gb_misc_, cz);

        D3D12_CPU_DESCRIPTOR_HANDLE pre_rtvs[4]{};
        device_->cpu_rtv_handle(gb_n_.rtv_index, &pre_rtvs[0]);
        device_->cpu_rtv_handle(gb_mv_.rtv_index, &pre_rtvs[1]);
        device_->cpu_rtv_handle(gb_mat_.rtv_index, &pre_rtvs[2]);
        device_->cpu_rtv_handle(gb_misc_.rtv_index, &pre_rtvs[3]);
        D3D12_CPU_DESCRIPTOR_HANDLE pre_dsv{};
        device_->cpu_dsv_handle(depth_stencil_.dsv_index, &pre_dsv);
        cmd->ClearDepthStencilView(pre_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0u, 0, nullptr);
        cmd->OMSetRenderTargets(4, pre_rtvs, FALSE, &pre_dsv);

        device_->cmd_bind_bindless_graphics(cmd);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_INDEX_BUFFER_VIEW ibv{index_buf_.gpu_va, static_cast<UINT>(index_buf_.size), DXGI_FORMAT_R32_UINT};
        cmd->IASetIndexBuffer(&ibv);

        auto draw_prepass_list = [&](const std::vector<uint32_t>& list, bool is_cutout) {
            ID3D12PipelineState* cur = nullptr;
            for (const uint32_t idx : list) {
                const auto& d = cached_submeshes_[idx];
                ID3D12PipelineState* want = (d.cull_mode != 0u) ? pso_prepass_nocull_ : pso_prepass_cull_;
                if (want != cur) {
                    cmd->SetPipelineState(want);
                    cur = want;
                }
                cmd->OMSetStencilRef(d.stencil_ref);
                GpuPushConstants p{};
                p.frame_globals_bda = fg_handle;
                p.vertex_buf_bda = skinned_vertices_buf_.srv_handle();
                p.aux_buf_bda = material_buf_.srv_handle();
                p.param0 = d.material_index;
                p.param1 = d.flags | (is_cutout ? AETHER_SUBMESH_FLAG_ALPHA_CUTOUT : 0u);
                cmd_set_root_pc(cmd, false, p);
                cmd->DrawIndexedInstanced(d.index_count, 1u, d.index_offset, 0, 0u);
            }
        };
        draw_prepass_list(opaque_draws_, false);
        draw_prepass_list(cutout_draws_, true);

        device_->cmd_transition_texture(cmd, gb_n_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, gb_mv_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, gb_mat_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, gb_misc_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, depth_stencil_,
                                        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Pass 4: RayEffects (DXR Tier 1.1) OR CSM Fallback (Mode 0 Traditional)
        device_->cmd_transition_texture(cmd, rt_shadow_raw_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, rim_occ_raw_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, bounce_half_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, aov_backscatter_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, aov_cornea_spec_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const uint32_t cur_res_pass = static_cast<uint32_t>(req.frame_index & 1ULL);
        device_->cmd_transition_buffer(cmd, hb.reservoir_bufs[cur_res_pass], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_buffer(cmd, hb.reservoir_bufs[1u - cur_res_pass],
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (use_rt) {
            device_->cmd_bind_bindless_compute(cmd, true, tlas_buf_.gpu_va);
            cmd->SetPipelineState(pso_ray_effects_);
            GpuPushConstants p{};
            p.frame_globals_bda = fg_handle;
            p.draw_data_bda = rt_tri_meta_buf_.srv_handle();
            p.vertex_buf_bda = skinned_vertices_buf_.srv_handle();
            p.aux_buf_bda = material_buf_.srv_handle();
            p.param0 = rt_shadow_raw_.uav_index;
            p.param1 = rim_occ_raw_.uav_index;
            p.param2 = bounce_half_.uav_index;
            p.param3 = ((w / 2u) << 16u) | (hist_valid ? 1u : 0u);
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(gx, gy, 1u);
        } else {
            if (req.render_csm_shadow) {
                device_->cmd_transition_texture(cmd, csm_depth_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                D3D12_CPU_DESCRIPTOR_HANDLE sdsv{};
                device_->cpu_dsv_handle(csm_depth_.dsv_index, &sdsv);
                cmd->ClearDepthStencilView(sdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0u, 0, nullptr);
                cmd->OMSetRenderTargets(0, nullptr, FALSE, &sdsv);

                const D3D12_VIEWPORT svp{0.0f, 0.0f, static_cast<float>(kCsmSize), static_cast<float>(kCsmSize),
                                         0.0f, 1.0f};
                const D3D12_RECT ssc{0, 0, static_cast<LONG>(kCsmSize), static_cast<LONG>(kCsmSize)};
                device_->cmd_bind_bindless_graphics(cmd);
                cmd->RSSetViewports(1, &svp);
                cmd->RSSetScissorRects(1, &ssc);
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->IASetIndexBuffer(&ibv);
                cmd->SetPipelineState(pso_csm_shadow_);
                for (const uint32_t idx : opaque_draws_) {
                    const auto& d = cached_submeshes_[idx];
                    GpuPushConstants p{};
                    p.frame_globals_bda = fg_handle;
                    p.vertex_buf_bda = skinned_vertices_buf_.srv_handle();
                    cmd_set_root_pc(cmd, false, p);
                    cmd->DrawIndexedInstanced(d.index_count, 1u, d.index_offset, 0, 0u);
                }
                device_->cmd_transition_texture(cmd, csm_depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
            cmd->SetPipelineState(pso_csm_fallback_);
            GpuPushConstants p{};
            p.frame_globals_bda = fg_handle;
            p.param0 = rt_shadow_raw_.uav_index;
            p.param1 = rim_occ_raw_.uav_index;
            p.param2 = bounce_half_.uav_index;
            p.param3 = req.render_csm_shadow ? csm_depth_.srv_index : 0xFFFFu;
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(gx, gy, 1u);
        }

        device_->cmd_transition_texture(cmd, rt_shadow_raw_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, rim_occ_raw_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, bounce_half_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, aov_backscatter_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, aov_cornea_spec_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Pass 5: SVGF Spatio-Temporal Denoise
        device_->cmd_transition_texture(cmd, rt_shadow_denoised_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, rim_occ_denoised_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_transition_texture(cmd, bounce_denoised_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
        cmd->SetPipelineState(pso_denoise_svgf_);
        {
            GpuPushConstants p{};
            p.frame_globals_bda = fg_handle;
            p.param0 =
                (rt_shadow_raw_.srv_index & 0xFFFFu) |
                (((hist_valid ? hb.prev_rt_shadow.srv_index : rt_shadow_raw_.srv_index) & 0xFFFFu) << 16u);
            p.param1 = (rt_shadow_denoised_.uav_index & 0xFFFFu) | ((hist_valid ? 1u : 0u) << 16u);
            p.param2 = w;
            p.param3 = h;
            p.fparam0 = (eff_mode >= 2u) ? 0.10f : 0.20f;
            p.fparam1 = 1.0f;
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(gx, gy, 1u);

            p.param0 = (rim_occ_raw_.srv_index & 0xFFFFu) |
                       (((hist_valid ? hb.prev_rim_occ.srv_index : rim_occ_raw_.srv_index) & 0xFFFFu) << 16u);
            p.param1 = (rim_occ_denoised_.uav_index & 0xFFFFu) | ((hist_valid ? 1u : 0u) << 16u);
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(gx, gy, 1u);

            const uint32_t hw = std::max(1u, (w + 1u) / 2u);
            const uint32_t hh = std::max(1u, (h + 1u) / 2u);
            const uint32_t bgx = (hw + 7u) / 8u;
            const uint32_t bgy = (hh + 7u) / 8u;
            p.param0 = (bounce_half_.srv_index & 0xFFFFu) |
                       (((hist_valid ? hb.prev_bounce.srv_index : bounce_half_.srv_index) & 0xFFFFu) << 16u);
            p.param1 = (bounce_denoised_.uav_index & 0xFFFFu) | ((hist_valid ? 1u : 0u) << 16u);
            p.param2 = hw;
            p.param3 = hh;
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(bgx, bgy, 1u);
        }
        device_->cmd_transition_texture(cmd, rt_shadow_denoised_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, rim_occ_denoised_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        device_->cmd_transition_texture(cmd, bounce_denoised_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Snapshot current view history immediately after Pass 5 so interleaved Broadcast <-> Stage never cross-contaminate
        if (allow_view_hist) {
            auto copy_tex = [&](rhi::D3D12Texture& src, rhi::D3D12Texture& dst) {
                const int prev_s = src.current_state;
                device_->cmd_transition_texture(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
                device_->cmd_transition_texture(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(dst.resource, src.resource);
                device_->cmd_transition_texture(cmd, src, prev_s);
                device_->cmd_transition_texture(cmd, dst, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            };
            copy_tex(gb_n_, hb.prev_normal);
            copy_tex(rt_shadow_denoised_, hb.prev_rt_shadow);
            copy_tex(rim_occ_denoised_, hb.prev_rim_occ);
            copy_tex(bounce_denoised_, hb.prev_bounce);
        }

        // Pass 6 & 7: Live2D NPR Shading + Inverted-Hull Outlines (depth read-only so SRVs work in PS)
        device_->cmd_transition_texture(cmd, hdr_scene_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd_clear_rtv(*device_, cmd, hdr_scene_, cz);
        D3D12_CPU_DESCRIPTOR_HANDLE hdr_rtv{};
        device_->cpu_rtv_handle(hdr_scene_.rtv_index, &hdr_rtv);
        D3D12_CPU_DESCRIPTOR_HANDLE ro_dsv{};
        device_->cpu_dsv_handle(depth_stencil_.dsv_readonly_index, &ro_dsv);
        cmd->OMSetRenderTargets(1, &hdr_rtv, FALSE, &ro_dsv);

        device_->cmd_bind_bindless_graphics(cmd);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetIndexBuffer(&ibv);

        const bool use_wire = req.wireframe && pso_npr_wire_ != nullptr;
        auto draw_npr_list = [&](const std::vector<uint32_t>& list, bool is_cutout, bool is_blend) {
            ID3D12PipelineState* cur = nullptr;
            for (const uint32_t idx : list) {
                const auto& d = cached_submeshes_[idx];
                ID3D12PipelineState* want =
                    use_wire ? pso_npr_wire_
                             : (is_blend ? ((d.cull_mode != 0u) ? pso_npr_blend_nocull_ : pso_npr_blend_cull_)
                                         : ((d.cull_mode != 0u) ? pso_npr_opaque_nocull_ : pso_npr_opaque_cull_));
                if (want != cur) {
                    cmd->SetPipelineState(want);
                    cur = want;
                }
                GpuPushConstants p{};
                p.frame_globals_bda = fg_handle;
                p.vertex_buf_bda = skinned_vertices_buf_.srv_handle();
                p.aux_buf_bda = material_buf_.srv_handle();
                p.param0 = d.material_index;
                p.param1 = d.flags | (is_cutout ? AETHER_SUBMESH_FLAG_ALPHA_CUTOUT : 0u) |
                           (is_blend ? AETHER_SUBMESH_FLAG_TRANSPARENT : 0u);
                cmd_set_root_pc(cmd, false, p);
                cmd->DrawIndexedInstanced(d.index_count, 1u, d.index_offset, 0, 0u);
            }
        };
        draw_npr_list(opaque_draws_, false, false);
        draw_npr_list(cutout_draws_, true, false);

        const float outline_mul = std::max(0.0f, req.outline_width_scale) * std::max(0.0f, rig.outline_width_scale);
        if (!use_wire && outline_mul > 0.0f) {
            cmd->SetPipelineState(pso_outline_hull_);
            for (const uint32_t idx : opaque_draws_) {
                const auto& d = cached_submeshes_[idx];
                if (cached_materials_[d.material_index].outline_color_width[3] <= 0.0001f) {
                    continue;
                }
                GpuPushConstants p{};
                p.frame_globals_bda = fg_handle;
                p.vertex_buf_bda = skinned_vertices_buf_.srv_handle();
                p.aux_buf_bda = material_buf_.srv_handle();
                p.param0 = d.material_index;
                p.param1 = d.flags;
                p.fparam0 = outline_mul;
                cmd_set_root_pc(cmd, false, p);
                cmd->DrawIndexedInstanced(d.index_count, 1u, d.index_offset, 0, 0u);
            }
        }
        draw_npr_list(transparent_draws_, false, true);

        device_->cmd_transition_texture(cmd, hdr_scene_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Pass 8: Crease Ink Lines
        device_->cmd_transition_texture(cmd, crease_mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
        cmd->SetPipelineState(pso_crease_lines_);
        {
            GpuPushConstants p{};
            p.frame_globals_bda = fg_handle;
            p.param0 = crease_mask_.uav_index;
            p.param2 = w;
            p.param3 = h;
            p.fparam0 = rig.crease_threshold_deg;
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch(gx, gy, 1u);
        }
        device_->cmd_transition_texture(cmd, crease_mask_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

        // Pass 9: 6-Mip Dual-Kawase Bloom
        const bool subrect_bloom = (w < buf_w) || (h < buf_h);
        auto pack_subrect = [](uint32_t psw, uint32_t psh, uint32_t asw, uint32_t ash) noexcept -> uint64_t {
            return static_cast<uint64_t>(std::min(psw, 0xFFFFu)) |
                   (static_cast<uint64_t>(std::min(psh, 0xFFFFu)) << 16u) |
                   (static_cast<uint64_t>(std::min(asw, 0xFFFFu)) << 32u) |
                   (static_cast<uint64_t>(std::min(ash, 0xFFFFu)) << 48u);
        };
        std::array<uint32_t, kBloomMipCount> act_mip_w{};
        std::array<uint32_t, kBloomMipCount> act_mip_h{};
        uint32_t src_slot = hdr_scene_.srv_index;
        uint32_t phys_sw = buf_w;
        uint32_t phys_sh = buf_h;
        uint32_t act_sw = w;
        uint32_t act_sh = h;
        cmd->SetPipelineState(pso_bloom_down_);
        for (uint32_t m = 0; m < kBloomMipCount; ++m) {
            device_->cmd_transition_texture(cmd, bloom_mips_[m], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t phys_mw = bloom_mips_[m].width;
            const uint32_t phys_mh = bloom_mips_[m].height;
            const uint32_t mw = subrect_bloom ? std::min(phys_mw, std::max(1u, act_sw >> 1u)) : phys_mw;
            const uint32_t mh = subrect_bloom ? std::min(phys_mh, std::max(1u, act_sh >> 1u)) : phys_mh;
            act_mip_w[m] = mw;
            act_mip_h[m] = mh;
            GpuPushConstants p{};
            p.vertex_buf_bda = subrect_bloom ? pack_subrect(phys_sw, phys_sh, act_sw, act_sh) : 0ULL;
            p.param0 = src_slot;
            p.param1 = bloom_mips_[m].uav_index;
            p.param2 = mw;
            p.param3 = mh;
            p.fparam0 = (m == 0u) ? rig.bloom_threshold : 0.0f;
            p.fparam1 = 0.5f / static_cast<float>(std::max(1u, phys_sw));
            p.fparam2 = 0.5f / static_cast<float>(std::max(1u, phys_sh));
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch((mw + 7u) / 8u, (mh + 7u) / 8u, 1u);
            device_->cmd_transition_texture(cmd, bloom_mips_[m], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            src_slot = bloom_mips_[m].srv_index;
            phys_sw = phys_mw;
            phys_sh = phys_mh;
            act_sw = mw;
            act_sh = mh;
        }
        cmd->SetPipelineState(pso_bloom_up_);
        for (int m = static_cast<int>(kBloomMipCount) - 2; m >= 0; --m) {
            const uint32_t dst_idx = static_cast<uint32_t>(m);
            const uint32_t up_src = dst_idx + 1u;
            device_->cmd_transition_texture(cmd, bloom_mips_[dst_idx], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t mw = act_mip_w[dst_idx];
            const uint32_t mh = act_mip_h[dst_idx];
            GpuPushConstants p{};
            p.vertex_buf_bda =
                subrect_bloom ? pack_subrect(bloom_mips_[up_src].width, bloom_mips_[up_src].height,
                                             act_mip_w[up_src], act_mip_h[up_src])
                              : 0ULL;
            p.param0 = bloom_mips_[up_src].srv_index;
            p.param1 = bloom_mips_[dst_idx].uav_index;
            p.param2 = mw;
            p.param3 = mh;
            p.fparam1 = 0.5f / static_cast<float>(std::max(1u, bloom_mips_[up_src].width));
            p.fparam2 = 0.5f / static_cast<float>(std::max(1u, bloom_mips_[up_src].height));
            cmd_set_root_pc(cmd, true, p);
            cmd->Dispatch((mw + 7u) / 8u, (mh + 7u) / 8u, 1u);
            device_->cmd_transition_texture(cmd, bloom_mips_[dst_idx], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        if (allow_view_hist) {
            hb.have_prev_view_proj = true;
        }
    } else {
        device_->cmd_transition_texture(cmd, hdr_scene_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float cz[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd_clear_rtv(*device_, cmd, hdr_scene_, cz);
        device_->cmd_transition_texture(cmd, hdr_scene_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        hb.have_prev_view_proj = false;
    }

    // Pass 10: Tone Composite + 2D Filter Pro -> out_rgba16f_
    device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
    device_->cmd_transition_texture(cmd, out_rgba16f_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetPipelineState(pso_tone_composite_);
    {
        GpuPushConstants p{};
        p.frame_globals_bda = fg_handle;
        p.param0 = hdr_scene_.srv_index;
        p.param1 = out_rgba16f_.uav_index;
        p.param2 = w;
        p.param3 = h;
        cmd_set_root_pc(cmd, true, p);
        cmd->Dispatch(gx, gy, 1u);
    }
    device_->cmd_transition_texture(cmd, out_rgba16f_, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    // Pass 11: Encode out_rgba16f_ -> out_rgba8_ (universal RGBA8) & Broadcast-only out_bgra8_ (Spout2 D3D11On12)
    if (w < buf_w || h < buf_h) {
        device_->cmd_transition_texture(cmd, out_rgba8_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd_clear_rtv(*device_, cmd, out_rgba8_, clr);
    }
    device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
    cmd->SetPipelineState(pso_encode_bgra_);
    device_->cmd_transition_texture(cmd, out_rgba8_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    {
        GpuPushConstants p{};
        p.frame_globals_bda = fg_handle;
        p.param0 = out_rgba16f_.srv_index;
        p.param1 = out_rgba8_.uav_index;
        p.param2 = w;
        p.param3 = h;
        p.fparam0 = 0.0f;
        p.fparam1 = 0.0f;
        cmd_set_root_pc(cmd, true, p);
        cmd->Dispatch(gx, gy, 1u);
    }

    // Strict Schematic-Only Overlay onto out_rgba8_ ONLY for LightingStageView (never Broadcast/out_bgra8_)
    const bool allow_schematic = (req.view_kind == RenderViewKind::LightingStage) && req.show_stage_schematic &&
                                 (pso_stage_schematic_ != nullptr) && (stage_overlay_buf_.mapped_ptr != nullptr);
    if (allow_schematic) {
        StageSchematicBuildParams sp{};
        sp.selected_light_idx = req.selected_studio_light_idx;
        sp.show_grid = req.show_stage_grid;
        sp.show_frustum = req.has_studio_frustum;
        sp.has_studio_frustum = req.has_studio_frustum;
        sp.studio_cam = req.studio_camera;
        sp.studio_aspect = (req.studio_frustum_aspect > 0.05f)
                               ? req.studio_frustum_aspect
                               : ((h > 0u) ? (static_cast<float>(w) / static_cast<float>(h)) : (16.0f / 9.0f));
        sp.pivot_ws = head_target_ws;
        sp.eye_pos_ws = eye_pos_ws;
        const uint32_t elem_cnt = build_stage_schematic_overlay(*req.rig, sp, stage_overlay_scratch_);
        if (elem_cnt > 0u) {
            std::memcpy(stage_overlay_buf_.mapped_ptr, stage_overlay_scratch_.data(),
                        static_cast<size_t>(elem_cnt) * sizeof(GpuStageOverlayElem));
            device_->cmd_transition_texture(cmd, out_rgba8_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            device_->cpu_rtv_handle(out_rgba8_.rtv_index, &rtv);
            cmd->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);
            const D3D12_VIEWPORT svp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            const D3D12_RECT ssc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            device_->cmd_bind_bindless_graphics(cmd);
            cmd->RSSetViewports(1, &svp);
            cmd->RSSetScissorRects(1, &ssc);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->SetPipelineState(pso_stage_schematic_);
            GpuPushConstants p{};
            p.frame_globals_bda = fg_handle;
            p.draw_data_bda = stage_overlay_buf_.srv_handle();
            cmd_set_root_pc(cmd, false, p);
            cmd->DrawInstanced(elem_cnt * 6u, 1u, 0u, 0u);
        }
    }
    device_->cmd_transition_texture(cmd, out_rgba8_, D3D12_RESOURCE_STATE_COMMON);

    if (req.view_kind == RenderViewKind::Broadcast && out_bgra8_.uav_index != rhi::kD3D12InvalidDescIndex) {
        device_->cmd_bind_bindless_compute(cmd, false, 0ULL);
        cmd->SetPipelineState(pso_encode_bgra_);
        device_->cmd_transition_texture(cmd, out_bgra8_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GpuPushConstants p{};
        p.frame_globals_bda = fg_handle;
        p.param0 = out_rgba16f_.srv_index;
        p.param1 = out_bgra8_.uav_index;
        p.param2 = w;
        p.param3 = h;
        p.fparam0 = 0.0f;
        p.fparam1 = 0.0f;
        cmd_set_root_pc(cmd, true, p);
        cmd->Dispatch(gx, gy, 1u);
        device_->cmd_transition_texture(cmd, out_bgra8_, D3D12_RESOURCE_STATE_COMMON);
    }
    device_->cmd_transition_texture(cmd, out_rgba16f_, D3D12_RESOURCE_STATE_COMMON);

    auto sub_res = device_->submit_direct_frame(frame_slot);
    if (!sub_res) {
        return make_unexpected(sub_res.error());
    }
    if (wait_for_gpu) {
        (void)device_->wait_direct_fence(sub_res.value(), 5000u);
    }
    last_render_ms_ = static_cast<float>(now_us() - t0_us) / 1000.0f;
    ++rendered_frames_;
    return sub_res.value();
#endif
}

bool Dx12FrameGraph::readback_rgba8(std::span<uint8_t> out_rgba) noexcept {
#if !defined(_WIN32)
    (void)out_rgba;
    return false;
#else
    if (device_ == nullptr || width_ == 0 || height_ == 0 || out_rgba8_.resource == nullptr ||
        readback_rgba8_buf_.mapped_ptr == nullptr) {
        return false;
    }
    const size_t need = static_cast<size_t>(width_) * height_ * 4u;
    if (out_rgba.size() < need) {
        return false;
    }
    auto rc = device_->execute_immediate([&](ID3D12GraphicsCommandList4* cmd) {
        const int prev_s = out_rgba8_.current_state;
        device_->cmd_transition_texture(cmd, out_rgba8_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = out_rgba8_.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback_rgba8_buf_.resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = kRgba8Fmt;
        dst.PlacedFootprint.Footprint.Width = width_;
        dst.PlacedFootprint.Footprint.Height = height_;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = readback_row_pitch_;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        device_->cmd_transition_texture(cmd, out_rgba8_, prev_s);
    });
    if (!rc) {
        return false;
    }
    const auto* src_bytes = static_cast<const uint8_t*>(readback_rgba8_buf_.mapped_ptr);
    const size_t row_bytes = static_cast<size_t>(width_) * 4u;
    for (uint32_t y = 0; y < height_; ++y) {
        std::memcpy(out_rgba.data() + static_cast<size_t>(y) * row_bytes,
                    src_bytes + static_cast<size_t>(y) * readback_row_pitch_, row_bytes);
    }
    return true;
#endif
}

bool Dx12FrameGraph::readback_bgra8(std::span<uint8_t> out_bgra, bool bottom_up) noexcept {
#if !defined(_WIN32)
    (void)out_bgra;
    (void)bottom_up;
    return false;
#else
    if (device_ == nullptr || width_ == 0 || height_ == 0 || readback_bgra8_buf_.mapped_ptr == nullptr) {
        return false;
    }
    const size_t need = static_cast<size_t>(width_) * height_ * 4u;
    if (out_bgra.size() < need) {
        return false;
    }
    rhi::D3D12Texture& src_tex = (out_bgra8_.uav_index != rhi::kD3D12InvalidDescIndex) ? out_bgra8_ : out_rgba8_;
    const bool needs_swizzle = (&src_tex == &out_rgba8_);
    auto rc = device_->execute_immediate([&](ID3D12GraphicsCommandList4* cmd) {
        const int prev_s = src_tex.current_state;
        device_->cmd_transition_texture(cmd, src_tex, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = src_tex.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback_bgra8_buf_.resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = static_cast<DXGI_FORMAT>(src_tex.format);
        dst.PlacedFootprint.Footprint.Width = width_;
        dst.PlacedFootprint.Footprint.Height = height_;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = readback_row_pitch_;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        device_->cmd_transition_texture(cmd, src_tex, prev_s);
    });
    if (!rc) {
        return false;
    }
    const auto* src_bytes = static_cast<const uint8_t*>(readback_bgra8_buf_.mapped_ptr);
    const size_t row_bytes = static_cast<size_t>(width_) * 4u;
    for (uint32_t y = 0; y < height_; ++y) {
        const uint32_t dst_y = bottom_up ? (height_ - 1u - y) : y;
        const uint8_t* s = src_bytes + static_cast<size_t>(y) * readback_row_pitch_;
        uint8_t* d = out_bgra.data() + static_cast<size_t>(dst_y) * row_bytes;
        if (!needs_swizzle) {
            std::memcpy(d, s, row_bytes);
        } else {
            for (uint32_t x = 0; x < width_; ++x) {
                d[x * 4u + 0u] = s[x * 4u + 2u];
                d[x * 4u + 1u] = s[x * 4u + 1u];
                d[x * 4u + 2u] = s[x * 4u + 0u];
                d[x * 4u + 3u] = s[x * 4u + 3u];
            }
        }
    }
    return true;
#endif
}

} // namespace aether::renderer
