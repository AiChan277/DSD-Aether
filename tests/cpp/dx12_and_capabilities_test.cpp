// Tasks 1-8 Comprehensive Verification Suite:
// - Task 1: DirectX 12 SM 6.6 + DXR 1.1 parity vs Vulkan, strict anti-upside-down & anti-mirror checksums/IoU on
// 8590256991748008892.vrm, DX12 Spout2 & VCam SHM.
// - Task 2: Intel iGPU / UMA detection, zero-copy heaps, capability matrix & safe Mode 0 Traditional auto-selection.
// - Tasks 3-6: 3-tier lighting modes, SVGF temporal lock in Path Tracing, pre-quantized color bleeding, dynamic Angel
// Ring hair specular, and 2D Filter Pro.
// - Task 7: Circadian / Solar-Time 24h lighting curve, timezones, and non-dark Night Studio mode.
// - Task 8: NPU Safe-Zone bounding box C^1 tanh compression, pitch glitch clamping, and predictive capsule
// anti-clipping.

#include "aether_ffi_contract.h"
#include "animation/math_quat.hpp"
#include "core/clock.hpp"
#include "core/garbage_collector.hpp"
#include "output/spout_output.hpp"
#include "output/vcam_transport.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/asset_upload/vrm_parity_prep.hpp"
#include "renderer/circadian_lighting.hpp"
#include "renderer/dx12_bridge.hpp"
#include "renderer/dx12_frame_graph.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/npr_renderer.hpp"
#include "renderer/sdf_generator.hpp"
#include "rhi/d3d12_device.hpp"
#include "rhi/rhi_device.hpp"
#include "rhi/rhi_swapchain.hpp"
#include "synthetic_avatar_helper.hpp"
#include "test_png_io.hpp"
#include "tracking/npu_postprocess.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kW = 640u;
constexpr uint32_t kH = 360u;

std::string read_env_var(const char* name) {
#if defined(_WIN32)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string out(buf);
        std::free(buf);
        return out;
    }
    return {};
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
#endif
}

std::filesystem::path find_user_vrm_path() {
    const std::string up = read_env_var("USERPROFILE");
    if (!up.empty()) {
        std::filesystem::path cand = std::filesystem::path(up) / "Downloads" / "8590256991748008892.vrm";
        if (std::filesystem::is_regular_file(cand)) {
            return cand;
        }
    }
    std::filesystem::path fallback(R"(C:\Users\LEGION\Downloads\8590256991748008892.vrm)");
    if (std::filesystem::is_regular_file(fallback)) {
        return fallback;
    }
    return {};
}

std::filesystem::path snapshot_out_dir() {
#ifdef AETHER_VRM_SNAPSHOT_DIR
    return std::filesystem::path(AETHER_VRM_SNAPSHOT_DIR);
#else
    return std::filesystem::current_path() / "ui_snapshots";
#endif
}

struct ImageMoments {
    uint64_t opaque_count{0};
    double cx_norm{0.5};
    double cy_norm{0.5};
    uint64_t top_fnv1a{0};
    uint64_t bottom_fnv1a{0};
    double mean_luma_lit{0.0};
};

ImageMoments analyze_rgba8(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    ImageMoments m{};
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_luma = 0.0;
    uint64_t h_top = 14695981039346656037ULL;
    uint64_t h_bot = 14695981039346656037ULL;
    const uint32_t mid_y = h / 2u;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
            const uint8_t r = rgba[idx + 0u];
            const uint8_t g = rgba[idx + 1u];
            const uint8_t b = rgba[idx + 2u];
            const uint8_t a = rgba[idx + 3u];
            uint64_t& hh = (y < mid_y) ? h_top : h_bot;
            hh ^= static_cast<uint64_t>(r) ^ (static_cast<uint64_t>(g) << 8) ^ (static_cast<uint64_t>(b) << 16) ^
                  (static_cast<uint64_t>(a) << 24);
            hh *= 1099511628211ULL;
            if (a > 32u) {
                ++m.opaque_count;
                sum_x += (static_cast<double>(x) + 0.5) / static_cast<double>(w);
                sum_y += (static_cast<double>(y) + 0.5) / static_cast<double>(h);
                sum_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            }
        }
    }
    m.top_fnv1a = h_top;
    m.bottom_fnv1a = h_bot;
    if (m.opaque_count > 0u) {
        m.cx_norm = sum_x / static_cast<double>(m.opaque_count);
        m.cy_norm = sum_y / static_cast<double>(m.opaque_count);
        m.mean_luma_lit = sum_luma / static_cast<double>(m.opaque_count);
    }
    return m;
}

double compute_alpha_iou(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t w, uint32_t h,
                         bool flip_b_y, bool flip_b_x) {
    uint64_t inter = 0;
    uint64_t uni = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t by = flip_b_y ? (h - 1u - y) : y;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t bx = flip_b_x ? (w - 1u - x) : x;
            const bool ca = a[(static_cast<size_t>(y) * w + x) * 4u + 3u] > 32u;
            const bool cb = b[(static_cast<size_t>(by) * w + bx) * 4u + 3u] > 32u;
            inter += (ca && cb) ? 1ULL : 0ULL;
            uni += (ca || cb) ? 1ULL : 0ULL;
        }
    }
    return uni == 0ULL ? 0.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

double compute_mean_abs_diff_rgb(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const size_t px_count = std::min(a.size(), b.size()) / 4u;
    if (px_count == 0u) {
        return 255.0;
    }
    double sum = 0.0;
    for (size_t i = 0; i < px_count; ++i) {
        sum += std::abs(static_cast<int>(a[i * 4u + 0u]) - static_cast<int>(b[i * 4u + 0u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 1u]) - static_cast<int>(b[i * 4u + 1u]));
        sum += std::abs(static_cast<int>(a[i * 4u + 2u]) - static_cast<int>(b[i * 4u + 2u]));
    }
    return sum / (static_cast<double>(px_count) * 3.0);
}

double compute_roi_mean_abs_diff_rgb(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t w,
                                     uint32_t h, uint32_t y0, uint32_t y1) {
    double sum = 0.0;
    uint64_t cnt = 0;
    const uint32_t end_y = std::min(h, y1);
    for (uint32_t y = y0; y < end_y; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            if (a[i + 3u] > 32u || b[i + 3u] > 32u) {
                sum += std::abs(static_cast<int>(a[i + 0u]) - static_cast<int>(b[i + 0u]));
                sum += std::abs(static_cast<int>(a[i + 1u]) - static_cast<int>(b[i + 1u]));
                sum += std::abs(static_cast<int>(a[i + 2u]) - static_cast<int>(b[i + 2u]));
                ++cnt;
            }
        }
    }
    return cnt == 0u ? 0.0 : sum / (static_cast<double>(cnt) * 3.0);
}

std::unique_ptr<aether::renderer::GpuAvatar> load_avatar_file(aether::rhi::RhiDevice& rhi,
                                                              const std::filesystem::path& path) {
    aether::renderer::AssetJob job;
    job.start_load(rhi, path.string());
    aether::HighResTimer timer;
    const uint64_t deadline = aether::now_us() + 30'000'000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline) {
        timer.sleep_us(500);
    }
    if (job.state() != aether::renderer::AssetJobState::ReadyForGpu) {
        return nullptr;
    }
    auto res = job.finalize_gpu_upload(rhi);
    return res ? std::move(res.value()) : nullptr;
}

aether::renderer::SdfBakeOutput bake_avatar_face_sdf(const aether::renderer::GpuAvatar& av) {
    aether::renderer::SdfBakeInput in{};
    in.resolution = 256;
    in.azimuth_steps = 16;
    in.vertices = av.rest_vertices.data();
    in.vertex_count = static_cast<uint32_t>(av.rest_vertices.size());
    in.indices = av.rest_indices.data();
    in.index_count = static_cast<uint32_t>(av.rest_indices.size());
    auto res = aether::renderer::OfflineSdfGenerator::bake_face_sdf(in);
    REQUIRE(res.has_value());
    return std::move(res.value());
}

} // namespace

TEST_CASE("Task 1: DirectX 12 parity vs Vulkan & strict anti-flip orientation on 8590256991748008892.vrm",
          "[dx12][orientation][vrm][validation]") {
    const auto vrm_path = find_user_vrm_path();
    REQUIRE_FALSE(vrm_path.empty());

    // 1) Render reference frames on Vulkan
    aether::rhi::RhiDeviceConfig vk_cfg{};
    vk_cfg.headless = true;
    vk_cfg.enable_validation = true;
    vk_cfg.require_validation = true;
    auto vk_dev_res = aether::rhi::RhiDevice::create(vk_cfg);
    REQUIRE(vk_dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(vk_dev_res.value());

    auto avatar = load_avatar_file(*rhi, vrm_path);
    REQUIRE(avatar != nullptr);
    const auto face_sdf = bake_avatar_face_sdf(*avatar);

    aether::renderer::NprRenderer vk_npr;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();
    REQUIRE(vk_npr.init(*rhi, rcfg).has_value());
    REQUIRE(vk_npr.bind_avatar(avatar.get()).has_value());

    aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
    rig.studio_lights[0].orbit_azimuth_deg = 52.0f;
    rig.studio_lights[0].orbit_elevation_deg = 32.0f;
    rig.lighting_mode = 1u;

    auto cam = aether::renderer::fit_camera_to_avatar(*avatar);
    cam.yaw_rad += 0.35f; // Deliberate asymmetric yaw to detect any horizontal mirror as well as vertical flip!

    auto run_vk = [&](uint64_t fidx) {
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.avatar_rot_y_deg = 22.0f;
        req.lighting_mode = 1u;
        req.rt_enabled = rhi->rt_supported();
        req.show_checkerboard = false;
        req.frame_index = fidx;
        req.completed_frame_index = (fidx >= 2u) ? fidx - 2u : 0u;
        req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::RhiSwapchain::kFramesInFlight);
        REQUIRE(vk_npr.render_headless_frame(req));
        auto rb =
            vk_npr.graph().readback_image_rgba8(vk_npr.graph().outputs().bgra8_image, VK_IMAGE_LAYOUT_GENERAL, kW, kH);
        REQUIRE(rb.has_value());
        return rb.value();
    };
    (void)run_vk(0);
    const auto vk_rgba = run_vk(1);
    const ImageMoments vk_mom = analyze_rgba8(vk_rgba, kW, kH);
    REQUIRE(vk_mom.opaque_count > (kW * kH) / 25u);

    // 2) Render identically configured frame on standalone DX12 Device + Dx12FrameGraph
    aether::rhi::D3D12DeviceConfig dx_cfg{};
    dx_cfg.enable_debug_layer = true;
    dx_cfg.use_preferred_luid = rhi->gpu_info().luid_valid;
    dx_cfg.preferred_luid = rhi->gpu_info().luid;
    auto dx_res = aether::rhi::D3D12Device::create(dx_cfg);
    REQUIRE(dx_res.has_value());
    std::unique_ptr<aether::rhi::D3D12Device> d3d12(dx_res.value());

    aether::renderer::Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kW, kH, avatar->requirements.vertex_count, true).has_value());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());
    REQUIRE(dx_fg.apply_face_sdf(face_sdf).has_value());

    std::vector<uint8_t> dx_rgba(static_cast<size_t>(kW) * kH * 4u, 0u);
    {
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.avatar_rot_y_deg = 22.0f;
        req.lighting_mode = 1u;
        req.rt_enabled = d3d12->dxr_supported();
        req.frame_index = 1;
        req.frame_slot = 1;
        REQUIRE(dx_fg.render_frame(req, true).has_value());
        REQUIRE(dx_fg.readback_rgba8(dx_rgba));
    }

    const ImageMoments dx_mom = analyze_rgba8(dx_rgba, kW, kH);
    REQUIRE(dx_mom.opaque_count > (kW * kH) / 25u);
    REQUIRE(dx_mom.top_fnv1a != dx_mom.bottom_fnv1a);

    const double iou_upright = compute_alpha_iou(vk_rgba, dx_rgba, kW, kH, false, false);
    const double iou_vflip = compute_alpha_iou(vk_rgba, dx_rgba, kW, kH, true, false);
    const double iou_hmirror = compute_alpha_iou(vk_rgba, dx_rgba, kW, kH, false, true);
    const double vk_dx_color_roi_diff = compute_roi_mean_abs_diff_rgb(vk_rgba, dx_rgba, kW, kH, 0u, kH);

    std::printf("[DX12_PARITY] vk_cov=%llu dx_cov=%llu iou_upright=%.4f iou_vflip=%.4f iou_hmirror=%.4f "
                "vk_centroid=(%.3f,%.3f) dx_centroid=(%.3f,%.3f) roi_rgb_diff=%.2f\n",
                static_cast<unsigned long long>(vk_mom.opaque_count),
                static_cast<unsigned long long>(dx_mom.opaque_count), iou_upright, iou_vflip, iou_hmirror,
                vk_mom.cx_norm, vk_mom.cy_norm, dx_mom.cx_norm, dx_mom.cy_norm, vk_dx_color_roi_diff);

    CHECK(iou_upright >= 0.85);
    CHECK(iou_upright > iou_vflip + 0.15);
    CHECK(iou_upright > iou_hmirror + 0.05);
    CHECK(std::abs(vk_mom.cx_norm - dx_mom.cx_norm) < 0.035);
    CHECK(std::abs(vk_mom.cy_norm - dx_mom.cy_norm) < 0.035);
    CHECK(vk_dx_color_roi_diff < 25.0);
    CHECK(d3d12->poll_debug_errors() == 0u);

    const auto snap_dir = snapshot_out_dir();
    CHECK(aether::test::save_rgba8_png(snap_dir / "vk_parity_upright.png", kW, kH, vk_rgba.data()));
    CHECK(aether::test::save_rgba8_png(snap_dir / "dx12_parity_upright.png", kW, kH, dx_rgba.data()));

    // 3) Verify DX12 Spout2 + VCam SHM output end-to-end
    std::vector<uint8_t> dx_bgra(static_cast<size_t>(kW) * kH * 4u, 0u);
    REQUIRE(dx_fg.readback_bgra8(dx_bgra, false));

    aether::output::SpoutOutput spout;
    REQUIRE(spout
                .init_d3d12(kW, kH, "AetherDX12Test", d3d12->gpu_info().luid, d3d12->gpu_info().luid_valid,
                            aether::output::SpoutSharingMode::GpuDefault)
                .has_value());
    REQUIRE(spout.publish_d3d12_texture(*d3d12, dx_fg.out_bgra8(), dx_bgra).has_value());
    CHECK(spout.frames_published() == 1u);
    spout.shutdown();

    aether::output::VcamTransport vcam;
    REQUIRE(vcam.init_host_only().has_value());
    REQUIRE(vcam.publish_host_bgra8(dx_bgra, kW, kH, 60u, false, 1u, aether::now_us()).has_value());
    CHECK(vcam.frames_published() == 1u);
    CHECK(vcam.header()->width == kW);
    CHECK(vcam.header()->height == kH);
    vcam.shutdown();

    dx_fg.destroy();
    d3d12->destroy();

    // 4) Verify live Dx12Bridge -> Vulkan compositor integration + GC reclamation
    aether::GarbageCollector gc;
    aether::renderer::Dx12Bridge bridge;
    bridge.attach(*rhi, gc);
    REQUIRE(bridge.configure(kW, kH, avatar->requirements.vertex_count, &rhi->gpu_info()).has_value());
    REQUIRE(bridge.bind_avatar(avatar.get()).has_value());
    REQUIRE(bridge.apply_face_sdf(face_sdf).has_value());

    std::vector<uint8_t> bridged_rgba;
    for (uint64_t f = 2; f <= 4; ++f) {
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.avatar_rot_y_deg = 22.0f;
        req.lighting_mode = 1u;
        req.rt_enabled = true;
        req.show_checkerboard = false;
        req.frame_index = f;
        req.completed_frame_index = f - 2u;
        req.frame_slot = static_cast<uint32_t>(f % aether::rhi::RhiSwapchain::kFramesInFlight);
        REQUIRE(bridge.render(req));
        bridge.fill_request(req);
        REQUIRE(vk_npr.render_headless_frame(req));
        if (req.external_color_image != VK_NULL_HANDLE) {
            bridge.on_submitted(vk_npr.frame_timeline_value());
        }
        if (f == 4) {
            auto rb = vk_npr.graph().readback_image_rgba8(vk_npr.graph().outputs().bgra8_image, VK_IMAGE_LAYOUT_GENERAL,
                                                          kW, kH);
            REQUIRE(rb.has_value());
            bridged_rgba = std::move(rb.value());
        }
    }
    CHECK(compute_alpha_iou(vk_rgba, bridged_rgba, kW, kH, false, false) >= 0.85);
    CHECK(compute_roi_mean_abs_diff_rgb(dx_rgba, bridged_rgba, kW, kH, 0u, kH) < 20.0);
    bridge.release();
    (void)gc.flush_all_epochs();

    rhi->wait_idle();
    vk_npr.destroy();
    avatar->destroy();
    rhi->destroy();
    CHECK(rhi->validation_error_count() == 0u);
}

TEST_CASE("Task 2: Intel iGPU UMA detection, zero-copy heaps & safe Traditional auto-tier",
          "[igpu][uma][dx12][vulkan]") {
    auto dx_adapters_res = aether::rhi::D3D12Device::enumerate_adapters();
    REQUIRE(dx_adapters_res.has_value());
    REQUIRE_FALSE(dx_adapters_res.value().empty());

    auto vk_adapters_res = aether::rhi::RhiDevice::enumerate_vulkan_devices();
    REQUIRE(vk_adapters_res.has_value());
    REQUIRE_FALSE(vk_adapters_res.value().empty());

    const auto& dx_ads = dx_adapters_res.value();
    const auto* intel_dx = static_cast<const aether::rhi::D3D12AdapterDescriptor*>(nullptr);
    for (const auto& ad : dx_ads) {
        std::printf("[HW_MATRIX_DX12] '%s' discrete=%d uma=%d intel_igpu=%d dxr11=%d fp16=%d bw=%uGB/s rec_mode=%u\n",
                    ad.device_name.c_str(), ad.is_discrete ? 1 : 0, ad.is_uma ? 1 : 0, ad.is_intel_igpu ? 1 : 0,
                    ad.supports_dxr_1_1 ? 1 : 0, ad.supports_fp16 ? 1 : 0, ad.estimated_bandwidth_gbps,
                    static_cast<unsigned>(ad.recommended_lighting_mode));
        if (ad.is_intel_igpu || (!ad.is_discrete && !ad.is_software && ad.is_uma)) {
            intel_dx = &ad;
        }
    }

    if (intel_dx != nullptr) {
        CHECK(intel_dx->is_uma);
        if (!intel_dx->supports_dxr_1_1 || intel_dx->is_intel_igpu) {
            CHECK(intel_dx->recommended_lighting_mode == 0u);
        }

        // Create DX12 device directly on the Intel iGPU / UMA adapter
        aether::rhi::D3D12DeviceConfig dcfg{};
        dcfg.enable_debug_layer = true;
        dcfg.use_preferred_luid = intel_dx->luid_valid;
        dcfg.preferred_luid = intel_dx->luid;
        dcfg.prefer_integrated = true;
        auto dev_res = aether::rhi::D3D12Device::create(dcfg);
        REQUIRE(dev_res.has_value());
        std::unique_ptr<aether::rhi::D3D12Device> d3d12(dev_res.value());
        CHECK(d3d12->is_uma());

        auto zero_buf = d3d12->create_buffer(4096u, aether::rhi::D3D12BufferUsage::ShaderResource,
                                             aether::rhi::D3D12BufferHeap::UploadCpu, true, false);
        REQUIRE(zero_buf.has_value());
        CHECK(zero_buf.value().is_uma_zero_copy);
        CHECK(zero_buf.value().mapped_ptr != nullptr);
        d3d12->destroy_buffer(zero_buf.value());

        // Render VRM in safe Mode 0 (Traditional) on the Intel iGPU without TDR
        const auto vrm_path = find_user_vrm_path();
        if (!vrm_path.empty()) {
            aether::rhi::RhiDeviceConfig vk_cfg{};
            vk_cfg.headless = true;
            vk_cfg.enable_validation = false;
            vk_cfg.prefer_integrated = true;
            vk_cfg.preferred_luid = intel_dx->luid;
            vk_cfg.use_preferred_luid = intel_dx->luid_valid;
            auto vk_dev = aether::rhi::RhiDevice::create(vk_cfg);
            if (vk_dev) {
                std::unique_ptr<aether::rhi::RhiDevice> rhi(vk_dev.value());
                auto uma_vk_buf = rhi->create_uma_zero_copy_buffer(4096u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                REQUIRE(uma_vk_buf.has_value());
                CHECK(uma_vk_buf.value().info.pMappedData != nullptr);
                rhi->destroy_buffer(uma_vk_buf.value());

                auto avatar = load_avatar_file(*rhi, vrm_path);
                if (avatar) {
                    aether::renderer::Dx12FrameGraph fg;
                    REQUIRE(fg.init(*d3d12, kW, kH, avatar->requirements.vertex_count, false).has_value());
                    REQUIRE(fg.bind_avatar(avatar.get()).has_value());
                    aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
                    rig.lighting_mode = 0u;
                    const auto cam = aether::renderer::fit_camera_to_avatar(*avatar);
                    for (uint32_t f = 0; f < 3u; ++f) {
                        aether::renderer::FrameRenderRequest req{};
                        req.avatar = avatar.get();
                        req.rig = &rig;
                        req.camera = cam;
                        req.lighting_mode = 0u;
                        req.rt_enabled = false;
                        req.frame_index = f;
                        req.frame_slot = f % aether::rhi::kD3D12FrameCount;
                        REQUIRE(fg.render_frame(req, true).has_value());
                        CHECK(fg.last_effective_lighting_mode() == 0u);
                    }
                    std::vector<uint8_t> igpu_rgba(static_cast<size_t>(kW) * kH * 4u, 0u);
                    REQUIRE(fg.readback_rgba8(igpu_rgba));
                    CHECK(analyze_rgba8(igpu_rgba, kW, kH).opaque_count > (kW * kH) / 25u);
                    fg.destroy();
                    avatar->destroy();
                }
                rhi->destroy();
            }
        }
        CHECK(d3d12->poll_debug_errors() == 0u);
        d3d12->destroy();
    }
}

TEST_CASE("Tasks 3-6: 3-tier lighting, Path Tracing SVGF lock, pre-cel GI bleed, dynamic Angel Ring, 2D Filter Pro",
          "[lighting][pathtrace][npr][vrm]") {
    const auto vrm_path = find_user_vrm_path();
    REQUIRE_FALSE(vrm_path.empty());

    aether::rhi::RhiDeviceConfig vk_cfg{};
    vk_cfg.headless = true;
    vk_cfg.enable_validation = false;
    auto vk_dev = aether::rhi::RhiDevice::create(vk_cfg);
    REQUIRE(vk_dev.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(vk_dev.value());

    auto avatar = load_avatar_file(*rhi, vrm_path);
    REQUIRE(avatar != nullptr);
    const auto face_sdf = bake_avatar_face_sdf(*avatar);

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = true;
    dcfg.use_preferred_luid = rhi->gpu_info().luid_valid;
    dcfg.preferred_luid = rhi->gpu_info().luid;
    auto d_res = aether::rhi::D3D12Device::create(dcfg);
    REQUIRE(d_res.has_value());
    std::unique_ptr<aether::rhi::D3D12Device> d3d12(d_res.value());

    aether::renderer::Dx12FrameGraph fg;
    REQUIRE(fg.init(*d3d12, kW, kH, avatar->requirements.vertex_count, false).has_value());
    REQUIRE(fg.bind_avatar(avatar.get()).has_value());
    REQUIRE(fg.apply_face_sdf(face_sdf).has_value());

    aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
    const auto cam = aether::renderer::fit_camera_to_avatar(*avatar);

    auto render_one = [&](uint32_t fidx, uint8_t mode, const aether::renderer::PreviewCamera& use_cam,
                          const std::array<float, 3>& hair_vel) {
        rig.lighting_mode = mode;
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = use_cam;
        req.lighting_mode = mode;
        req.rt_enabled = (mode > 0u) && d3d12->dxr_supported();
        req.hair_velocity_ws = hair_vel;
        req.frame_index = fidx;
        req.frame_slot = fidx % aether::rhi::kD3D12FrameCount;
        REQUIRE(fg.render_frame(req, true).has_value());
        std::vector<uint8_t> out(static_cast<size_t>(kW) * kH * 4u, 0u);
        REQUIRE(fg.readback_rgba8(out));
        return out;
    };

    const auto snap_dir = snapshot_out_dir();

    // Mode 0: Traditional
    const auto m0 = render_one(1, 0u, cam, {0.0f, 0.0f, 0.0f});
    CHECK(analyze_rgba8(m0, kW, kH).opaque_count > (kW * kH) / 25u);
    CHECK(aether::test::save_rgba8_png(snap_dir / "dx12_mode0_traditional.png", kW, kH, m0.data()));

    // Mode 1: Hybrid Ray Tracing
    const auto m1 = render_one(2, 1u, cam, {0.0f, 0.0f, 0.0f});
    CHECK(analyze_rgba8(m1, kW, kH).opaque_count > (kW * kH) / 25u);
    CHECK(aether::test::save_rgba8_png(snap_dir / "dx12_mode1_hybrid_rt.png", kW, kH, m1.data()));

    // Mode 2: Real-Time 2.5D Path Tracing + stationary SVGF temporal stability check
    std::vector<uint8_t> pt_prev;
    std::vector<uint8_t> pt_last;
    for (uint32_t f = 3; f <= 10u; ++f) {
        pt_prev = pt_last;
        pt_last = render_one(f, 2u, cam, {0.0f, 0.0f, 0.0f});
    }
    const double pt_temporal_mad = compute_mean_abs_diff_rgb(pt_prev, pt_last);
    std::printf("[PT_SVGF_LOCK] stationary consecutive frame MAD_RGB=%.4f\n", pt_temporal_mad);
    CHECK(pt_temporal_mad < 2.0);
    CHECK(aether::test::save_rgba8_png(snap_dir / "dx12_mode2_path_tracing.png", kW, kH, pt_last.data()));

    // Pre-Quantization Indirect Color Bleeding check (0.0 vs 1.25 over avatar pixels)
    rig.indirect_bleed_strength = 0.0f;
    const auto bleed_off = render_one(11, 2u, cam, {0.0f, 0.0f, 0.0f});
    rig.indirect_bleed_strength = 1.25f;
    const auto bleed_on = render_one(12, 2u, cam, {0.0f, 0.0f, 0.0f});
    rig.indirect_bleed_strength = 0.75f;
    const double bleed_diff = compute_roi_mean_abs_diff_rgb(bleed_off, bleed_on, kW, kH, 0u, kH);
    std::printf("[GI_BLEED] roi_diff=%.4f\n", bleed_diff);
    if (d3d12->dxr_supported()) {
        CHECK(bleed_diff > 0.10);
    }

    // Dynamic Angel Ring hair specular response under SpringBone velocity + head tilt
    auto tilt_cam = cam;
    tilt_cam.pitch_rad += 0.18f;
    const auto hair_still = render_one(13, 0u, cam, {0.0f, 0.0f, 0.0f});
    const auto hair_sway = render_one(14, 0u, cam, {1.5f, 0.4f, 0.0f});
    const double hair_vel_diff = compute_roi_mean_abs_diff_rgb(hair_still, hair_sway, kW, kH, 0u, kH / 2u);
    CHECK(hair_vel_diff > 0.05);

    const auto hair_tilt = render_one(15, 0u, tilt_cam, {0.0f, 0.0f, 0.0f});
    CHECK(compute_roi_mean_abs_diff_rgb(hair_still, hair_tilt, kW, kH, 0u, kH) > 2.0);

    // 2D Filter Pro toggle check
    rig.enable_2d_filter_pro = false;
    const auto f2d_off = render_one(16, 0u, cam, {0.0f, 0.0f, 0.0f});
    rig.enable_2d_filter_pro = true;
    rig.enable_paper_warmth = true;
    rig.paper_warmth_strength = 0.55f;
    const auto f2d_on = render_one(17, 0u, cam, {0.0f, 0.0f, 0.0f});
    const double f2d_diff = compute_roi_mean_abs_diff_rgb(f2d_off, f2d_on, kW, kH, 0u, kH);
    std::printf("[FILTER_PRO] roi_diff=%.4f\n", f2d_diff);
    CHECK(f2d_diff > 1.0);

    // Circadian Golden Hour DX12 parity check (2.5D Path Tracing at golden-hour sun)
    const auto cres = aether::renderer::evaluate_circadian_lighting(17.58f);
    aether::renderer::apply_circadian_to_rig(cres, rig);
    const auto shinkai_dx = render_one(18, 2u, cam, {0.0f, 0.0f, 0.0f});
    CHECK(analyze_rgba8(shinkai_dx, kW, kH).opaque_count > (kW * kH) / 25u);
    CHECK(aether::test::save_rgba8_png(snap_dir / "dx12_shinkai_golden_hour.png", kW, kH, shinkai_dx.data()));

    CHECK(d3d12->poll_debug_errors() == 0u);
    fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    rhi->destroy();
}

TEST_CASE("Task 7: Circadian Solar-Time Lighting timezones, 24h continuity & Night Studio legibility", "[circadian]") {
    using namespace aether::renderer;

    REQUIRE(circadian_timezone_count() == 29);
    const std::string enum_str = circadian_timezone_enum_items();
    CHECK(enum_str.find("System Local Time") != std::string::npos);
    CHECK(enum_str.find("UTC+07:00 WIB (Jakarta / Surakarta)") != std::string::npos);
    CHECK(enum_str.find("UTC+08:00 WITA / SGT") != std::string::npos);
    CHECK(enum_str.find("UTC+09:00 WIT / JST (Tokyo)") != std::string::npos);

    // Manual override clamps [0, 1439]
    CHECK(std::abs(resolve_circadian_hour(0, 15 * 60 + 30) - 15.5f) < 1e-4f);

    // Live timezone offset delta between UTC+07:00 WIB (idx 21) and UTC+09:00 JST (idx 23) is 2.0 hours mod 24
    int wib_idx = -1;
    int jst_idx = -1;
    for (int i = 0; i < circadian_timezone_count(); ++i) {
        const auto& sp = circadian_timezone_spec(i);
        if (!sp.is_system_local && sp.utc_offset_minutes == 7 * 60) {
            wib_idx = i;
        }
        if (!sp.is_system_local && sp.utc_offset_minutes == 9 * 60) {
            jst_idx = i;
        }
    }
    REQUIRE(wib_idx >= 0);
    REQUIRE(jst_idx >= 0);
    const float h_wib = resolve_circadian_hour(wib_idx, -1);
    const float h_jst = resolve_circadian_hour(jst_idx, -1);
    float delta_h = h_jst - h_wib;
    if (delta_h < 0.0f) {
        delta_h += 24.0f;
    }
    CHECK(std::abs(delta_h - 2.0f) < 0.05f);

    const auto dawn = evaluate_circadian_lighting(6.0f);
    const auto day = evaluate_circadian_lighting(12.0f);
    const auto golden = evaluate_circadian_lighting(17.0f);
    const auto night = evaluate_circadian_lighting(23.0f);
    const auto wrap_a = evaluate_circadian_lighting(23.99f);
    const auto wrap_b = evaluate_circadian_lighting(0.01f);

    CHECK(dawn.phase == CircadianPhase::Dawn);
    CHECK(day.phase == CircadianPhase::Daytime);
    CHECK(golden.phase == CircadianPhase::GoldenHour);
    CHECK(night.phase == CircadianPhase::NightStudio);

    // Sunset is warm amber (< 3800K), Day is neutral (~5800K), Night Studio stays bright & readable!
    CHECK(golden.key_kelvin < 3800.0f);
    CHECK(day.key_kelvin > 5200.0f);
    CHECK(night.key_intensity >= 1.0f);
    CHECK(night.rim_intensity >= 0.85f);
    CHECK(night.fill_intensity >= 0.45f);
    CHECK(std::abs(wrap_a.key_intensity - wrap_b.key_intensity) < 0.02f);
    CHECK(std::abs(wrap_a.key_azimuth_deg - wrap_b.key_azimuth_deg) < 0.5f);

    LightingRig rig = LightingRig::default_three_point();
    const float orig_rim_ev = rig.studio_lights[2].intensity_ev100;
    apply_circadian_to_rig(golden, rig);
    CHECK(rig.circadian_sun_slot_id == 0);
    CHECK(rig.studio_lights[0].fixture == StudioFixtureProfile::NaturalSunCieSky);
    CHECK(std::abs(rig.studio_lights[0].intensity_ev100 - golden.key_intensity) < 1e-4f);
    CHECK(std::abs(rig.studio_lights[0].orbit_azimuth_deg - golden.key_azimuth_deg) < 1e-4f);
    CHECK(std::abs(rig.studio_lights[2].intensity_ev100 - orig_rim_ev) < 1e-4f);
}

TEST_CASE("Task 8: NPU Safe-Zone Box C^1 tanh compression, pitch glitch clamping & predictive capsule anti-clip",
          "[tracking][npu][bounds]") {
    using namespace aether::tracking;

    NpuMovementBoundsConfig cfg{};
    cfg.bounds_min = {-0.35f, -0.22f, -0.30f};
    cfg.bounds_max = {0.35f, 0.22f, 0.30f};
    cfg.soft_wall_elasticity = 0.35f;
    cfg.max_head_pitch_deg = 42.0f;
    cfg.anti_clip_margin_m = 0.035f;
    cfg.enable_bounds = true;
    cfg.enable_anti_clip = true;

    // 1) Interior exact linearity + extreme saturation containment
    const auto interior =
        NpuPostProcessor::compress_translation_tanh({0.08f, -0.05f, 0.04f}, cfg.bounds_min, cfg.bounds_max, 0.35f);
    CHECK(std::abs(interior[0] - 0.08f) < 1e-5f);
    CHECK(std::abs(interior[1] - (-0.05f)) < 1e-5f);
    CHECK(std::abs(interior[2] - 0.04f) < 1e-5f);

    const auto extreme =
        NpuPostProcessor::compress_translation_tanh({250.0f, -100.0f, 50.0f}, cfg.bounds_min, cfg.bounds_max, 0.35f);
    CHECK(extreme[0] >= cfg.bounds_min[0]);
    CHECK(extreme[0] <= cfg.bounds_max[0]);
    CHECK(extreme[1] >= cfg.bounds_min[1]);
    CHECK(extreme[1] <= cfg.bounds_max[1]);
    CHECK(extreme[2] >= cfg.bounds_min[2]);
    CHECK(extreme[2] <= cfg.bounds_max[2]);

    // 2) Full process(): head extreme translation + extreme pitch glitch + penetrating hand
    NpuPostProcessor proc;
    proc.reset();

    AetherTrackingFrame tf{};
    tf.valid_flags = 0x0Fu;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        tf.joint_rotations_quat[b][3] = 1.0f;
    }
    // Head pushed +4.0m off anchor with ~82 deg downward pitch (sin(41 deg))
    tf.joint_positions_ws[AETHER_BONE_HEAD][0] = 4.0f;
    tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
    tf.joint_positions_ws[AETHER_BONE_HEAD][2] = 0.0f;
    tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
    const float half_rad = 41.0f * (3.14159265f / 180.0f);
    tf.joint_rotations_quat[AETHER_BONE_HEAD][0] = std::sin(half_rad);
    tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = std::cos(half_rad);

    // Place Right Hand deep inside head sphere & torso capsule
    tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] = 0.01f;
    tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][1] = 1.22f;
    tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][2] = 0.00f;
    tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][3] = 0.95f;

    const auto diag = proc.process(tf, cfg);
    CHECK(diag.translation_compressed);
    CHECK(diag.orientation_clamped);
    CHECK(diag.collision_projected);
    CHECK(diag.max_penetration_m > 0.05f);
    CHECK(tf.joint_positions_ws[AETHER_BONE_HEAD][0] <= cfg.bounds_max[0] + 1e-4f);
    CHECK(tf.joint_positions_ws[AETHER_BONE_HEAD][0] >= cfg.bounds_min[0] - 1e-4f);
    // Zero residual capsule penetration after projection, and re-running anti-clip on the output is a no-op.
    // (Bounds are disabled for the re-run: tanh compression maps RAW input and is intentionally not idempotent.)
    CHECK(diag.residual_penetration_m == 0.0f);
    {
        AetherTrackingFrame again = tf;
        NpuMovementBoundsConfig clip_only = cfg;
        clip_only.enable_bounds = false;
        const auto diag2 = proc.process(again, clip_only);
        CHECK_FALSE(diag2.collision_projected);
        CHECK(diag2.residual_penetration_m == 0.0f);
        CHECK(std::abs(again.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] -
                       tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0]) < 1e-5f);
    }

    // 3) Adversarial sweep: hands/forearms placed on a dense grid through head, torso and hair volumes
    //    must always land outside every capsule (overlapping capsules need the iterative projection).
    float worst_residual = 0.0f;
    uint32_t projected_count = 0u;
    for (int ix = -6; ix <= 6; ++ix) {
        for (int iy = 0; iy <= 14; ++iy) {
            for (int iz = -5; iz <= 5; ++iz) {
                AetherTrackingFrame g{};
                g.valid_flags = 0x0Fu;
                for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                    g.joint_rotations_quat[b][3] = 1.0f;
                }
                g.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
                g.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
                const float px = 0.03f * static_cast<float>(ix);
                const float py = 0.95f + 0.045f * static_cast<float>(iy);
                const float pz = 0.03f * static_cast<float>(iz);
                for (uint32_t j : {static_cast<uint32_t>(AETHER_BONE_LEFT_HAND),
                                   static_cast<uint32_t>(AETHER_BONE_RIGHT_LOWER_ARM)}) {
                    g.joint_positions_ws[j][0] = px;
                    g.joint_positions_ws[j][1] = py;
                    g.joint_positions_ws[j][2] = pz;
                    g.joint_positions_ws[j][3] = 0.9f;
                }
                NpuPostProcessor p2;
                const auto d = p2.process(g, cfg);
                projected_count += d.collision_projected ? 1u : 0u;
                worst_residual = std::max(worst_residual, d.residual_penetration_m);
            }
        }
    }
    std::printf("[NPU_ANTICLIP] sweep projected=%u worst_residual=%.6f m\n", projected_count, worst_residual);
    CHECK(projected_count > 100u);
    CHECK(worst_residual == 0.0f);

    // 4) Soft wall: leaning the head steadily past +X never leaves the box, decelerates monotonically,
    //    and never jumps (C^1 tanh knee instead of a robotic hard clamp).
    float prev_x = -1.0f;
    float prev_step = 1.0f;
    float max_step = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        const float in_x = 0.005f * static_cast<float>(i); // 0 .. 1.0 m (box half-width is 0.35 m)
        const auto o = NpuPostProcessor::compress_translation_tanh({in_x, 0.0f, 0.0f}, cfg.bounds_min, cfg.bounds_max,
                                                                   cfg.soft_wall_elasticity);
        CHECK(o[0] <= cfg.bounds_max[0]);
        if (i > 0) {
            const float step = o[0] - prev_x;
            CHECK(step >= 0.0f);              // monotonic
            CHECK(step <= prev_step + 1e-6f); // velocity never increases => smooth deceleration
            max_step = std::max(max_step, step);
            prev_step = step;
        }
        prev_x = o[0];
    }
    CHECK(max_step <= 0.005f + 1e-6f); // never moves faster than the input (no jumps)
    CHECK(prev_x > 0.95f * cfg.bounds_max[0]);

    // 5) Chin-into-Chest / Collar anti-clipping verification under downward pitch + dip
    {
        AetherTrackingFrame chin_tf{};
        chin_tf.valid_flags = 0x0Fu;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            chin_tf.joint_rotations_quat[b][3] = 1.0f;
        }
        chin_tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.38f;
        chin_tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.98f;
        const float dip_rad = 0.5f * 38.0f * (3.14159265f / 180.0f);
        chin_tf.joint_rotations_quat[AETHER_BONE_HEAD][0] = std::sin(dip_rad);
        chin_tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = std::cos(dip_rad);

        NpuPostProcessor p_chin;
        NpuMovementBoundsConfig c_chin = cfg;
        c_chin.enable_bounds = false;
        const auto d1 = p_chin.process(chin_tf, c_chin);
        CHECK(d1.collision_projected);
        CHECK(d1.residual_penetration_m == 0.0f);

        AetherTrackingFrame chin_again = chin_tf;
        const auto d2 = p_chin.process(chin_again, c_chin);
        CHECK_FALSE(d2.collision_projected);
        CHECK(d2.residual_penetration_m == 0.0f);
    }
}

TEST_CASE("DX12 Bridge mid-way avatar swap to larger vertex count keeps rendering cleanly",
          "[dx12][avatar-swap][regression]") {
    const auto small_path = std::filesystem::path(AETHER_REPO_ROOT) / "tests" / "data" / "reference_avatar.vrm";
    const auto large_path = find_user_vrm_path();
    REQUIRE(std::filesystem::is_regular_file(small_path));
    if (large_path.empty()) {
        SUCCEED("Optional larger VRM absent; skipping swap test");
        return;
    }

    auto run_swap_on_adapter = [&](bool prefer_igpu, const char* tag) {
        aether::rhi::RhiDeviceConfig vk_cfg{};
        vk_cfg.headless = true;
        vk_cfg.enable_validation = true;
        vk_cfg.require_validation = true;
        vk_cfg.prefer_integrated = prefer_igpu;
        auto vk_dev_res = aether::rhi::RhiDevice::create(vk_cfg);
        REQUIRE(vk_dev_res.has_value());
        std::unique_ptr<aether::rhi::RhiDevice> rhi(vk_dev_res.value());

        auto small_av = load_avatar_file(*rhi, small_path);
        auto large_av = load_avatar_file(*rhi, large_path);
        REQUIRE(small_av != nullptr);
        REQUIRE(large_av != nullptr);
        std::printf("[DX12_SWAP:%s] adapter='%s' small_verts=%u large_verts=%u\n", tag,
                    rhi->gpu_info().device_name.c_str(), small_av->requirements.vertex_count,
                    large_av->requirements.vertex_count);
        REQUIRE(large_av->requirements.vertex_count > small_av->requirements.vertex_count);

        aether::renderer::NprRenderer vk_npr;
        aether::renderer::RendererConfig rcfg{};
        rcfg.width = kW;
        rcfg.height = kH;
        rcfg.enable_rt = rhi->rt_supported() && !rhi->is_intel_igpu();
        REQUIRE(vk_npr.init(*rhi, rcfg).has_value());

        aether::GarbageCollector gc;
        aether::renderer::Dx12Bridge bridge;
        bridge.attach(*rhi, gc);

        aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
        rig.lighting_mode = rhi->is_intel_igpu() ? 0u : 1u;

        auto bind_all = [&](const aether::renderer::GpuAvatar* av) {
            REQUIRE(vk_npr.bind_avatar(av).has_value());
            REQUIRE(bridge.bind_avatar(av).has_value());
            REQUIRE(bridge.configure(kW, kH, av->requirements.vertex_count, &rhi->gpu_info()).has_value());
        };

        auto render_frames = [&](const aether::renderer::GpuAvatar* av, uint64_t& fidx, uint32_t count) {
            const auto cam = aether::renderer::fit_camera_to_avatar(*av);
            std::vector<uint8_t> last_rgba;
            for (uint32_t i = 0; i < count; ++i, ++fidx) {
                // Mirror application.cpp:2257 per-frame configure check.
                REQUIRE(bridge.configure(kW, kH, av->requirements.vertex_count, &rhi->gpu_info()).has_value());
                aether::renderer::FrameRenderRequest req{};
                req.avatar = av;
                req.rig = &rig;
                req.camera = cam;
                req.lighting_mode = rig.lighting_mode;
                req.rt_enabled = (rig.lighting_mode > 0u);
                req.show_checkerboard = false;
                req.frame_index = fidx;
                req.completed_frame_index = (fidx >= 2u) ? fidx - 2u : 0u;
                req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::RhiSwapchain::kFramesInFlight);
                REQUIRE(bridge.render(req));
                bridge.fill_request(req);
                REQUIRE(vk_npr.render_headless_frame(req));
                if (req.external_color_image != VK_NULL_HANDLE) {
                    bridge.on_submitted(vk_npr.frame_timeline_value());
                }
                if (i + 1u == count) {
                    auto rb = vk_npr.graph().readback_image_rgba8(vk_npr.graph().outputs().bgra8_image,
                                                                  VK_IMAGE_LAYOUT_GENERAL, kW, kH);
                    REQUIRE(rb.has_value());
                    last_rgba = std::move(rb.value());
                }
            }
            return last_rgba;
        };

        uint64_t fidx = 1;
        // 1) Initial smaller avatar at startup
        bind_all(small_av.get());
        const auto rgba_small_1 = render_frames(small_av.get(), fidx, 4u);
        const auto mom_small_1 = analyze_rgba8(rgba_small_1, kW, kH);
        std::printf("[DX12_SWAP:%s] initial small opaque=%llu shared=%d\n", tag,
                    static_cast<unsigned long long>(mom_small_1.opaque_count), bridge.stats().shared_texture ? 1 : 0);
        CHECK(mom_small_1.opaque_count > (kW * kH) / 25u);

        // 2) Mid-way load of larger character (bind_avatar then configure, mirroring application.cpp)
        bind_all(large_av.get());
        CHECK(bridge.is_avatar_bound(large_av.get()));
        const uint64_t retired_before_steady = gc.stats().retired_pending + gc.stats().retired_reclaimed_total;
        const auto rgba_large = render_frames(large_av.get(), fidx, 5u);
        const uint64_t retired_after_steady = gc.stats().retired_pending + gc.stats().retired_reclaimed_total;
        const auto mom_large = analyze_rgba8(rgba_large, kW, kH);
        std::printf("[DX12_SWAP:%s] swapped large opaque=%llu gc_churn=%llu\n", tag,
                    static_cast<unsigned long long>(mom_large.opaque_count),
                    static_cast<unsigned long long>(retired_after_steady - retired_before_steady));
        CHECK(mom_large.opaque_count > (kW * kH) / 25u);
        CHECK(retired_after_steady == retired_before_steady);

        // 3) Mid-way swap back to smaller character
        bind_all(small_av.get());
        CHECK(bridge.is_avatar_bound(small_av.get()));
        const auto rgba_small_2 = render_frames(small_av.get(), fidx, 4u);
        const auto mom_small_2 = analyze_rgba8(rgba_small_2, kW, kH);
        CHECK(mom_small_2.opaque_count > (kW * kH) / 25u);

        // 4) Failed bind_avatar must not leave stale bound_avatar_ or emit blank frames
        aether::renderer::GpuAvatar empty_av{};
        CHECK_FALSE(bridge.bind_avatar(&empty_av).has_value());
        CHECK_FALSE(bridge.is_avatar_bound(&empty_av));
        {
            aether::renderer::FrameRenderRequest bad_req{};
            bad_req.avatar = &empty_av;
            bad_req.rig = &rig;
            CHECK_FALSE(bridge.render(bad_req));
        }

        bridge.release();
        (void)gc.flush_all_epochs();
        rhi->wait_idle();
        vk_npr.destroy();
        small_av->destroy();
        large_av->destroy();
        rhi->destroy();
        CHECK(rhi->validation_error_count() == 0u);
    };

    run_swap_on_adapter(false, "default");
    run_swap_on_adapter(true, "igpu");
}

namespace {

double srgb_u8_to_linear(uint8_t u) noexcept {
    const double c = static_cast<double>(u) / 255.0;
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

struct OklabColor {
    double L{0.0};
    double a{0.0};
    double b{0.0};
};

OklabColor rgb_u8_to_oklab(uint8_t r8, uint8_t g8, uint8_t b8) noexcept {
    const double r = srgb_u8_to_linear(r8);
    const double g = srgb_u8_to_linear(g8);
    const double b = srgb_u8_to_linear(b8);
    const double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    const double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    const double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    const double l_ = std::cbrt(std::max(0.0, l));
    const double m_ = std::cbrt(std::max(0.0, m));
    const double s_ = std::cbrt(std::max(0.0, s));
    return {
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    };
}

double oklab_delta_e(const OklabColor& x, const OklabColor& y) noexcept {
    const double dL = x.L - y.L;
    const double da = x.a - y.a;
    const double db = x.b - y.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

std::filesystem::path find_downloads_vrm_by_prefix(const std::string& prefix) {
    const std::string up = read_env_var("USERPROFILE");
    std::filesystem::path dl =
        up.empty() ? std::filesystem::path(R"(C:\Users\LEGION\Downloads)") : (std::filesystem::path(up) / "Downloads");
    std::error_code ec;
    if (!std::filesystem::is_directory(dl, ec)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(dl, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string fn = entry.path().filename().string();
        if (fn.rfind(prefix, 0) == 0 && entry.path().extension() == ".vrm") {
            return entry.path();
        }
    }
    return {};
}

aether::renderer::PreviewCamera make_neck_closeup_camera(const aether::renderer::GpuAvatar& av) noexcept {
    float head_y = av.desc.aabb_min[1] + (av.desc.aabb_max[1] - av.desc.aabb_min[1]) * 0.88f;
    float neck_y = head_y - 0.07f;
    if (av.desc.humanoid_to_joint[AETHER_BONE_HEAD] < av.joint_rest_world_pos.size()) {
        head_y = av.joint_rest_world_pos[av.desc.humanoid_to_joint[AETHER_BONE_HEAD]][1];
    }
    if (av.desc.humanoid_to_joint[AETHER_BONE_NECK] < av.joint_rest_world_pos.size()) {
        neck_y = av.joint_rest_world_pos[av.desc.humanoid_to_joint[AETHER_BONE_NECK]][1];
    }
    aether::renderer::PreviewCamera cam{};
    cam.target_x = 0.0f;
    cam.target_y = 0.5f * (head_y + neck_y) + 0.015f;
    cam.target_z = 0.0f;
    cam.yaw_rad = 0.0f;
    cam.pitch_rad = 0.0f;
    cam.fovy_rad = 0.2805f;
    cam.distance = 1.15f;
    return cam;
}

} // namespace

TEST_CASE("Forensic Probe: Jaw-to-Neck Seam Quantification (DX12 & Vulkan x Traditional / Hybrid RT / 2.5D PT)",
          "[neck-seam-probe]") {
    constexpr uint32_t kPW = 512u;
    constexpr uint32_t kPH = 512u;

    const std::array<std::pair<const char*, std::filesystem::path>, 2> models = {{
        {"6493", find_downloads_vrm_by_prefix("649314313514")},
        {"8590", find_user_vrm_path()},
    }};

    aether::rhi::RhiDeviceConfig vk_cfg{};
    vk_cfg.headless = true;
    vk_cfg.enable_validation = false;
    auto vk_dev_res = aether::rhi::RhiDevice::create(vk_cfg);
    REQUIRE(vk_dev_res.has_value());
    std::unique_ptr<aether::rhi::RhiDevice> rhi(vk_dev_res.value());

    aether::rhi::D3D12DeviceConfig dx_cfg{};
    dx_cfg.enable_debug_layer = false;
    dx_cfg.use_preferred_luid = rhi->gpu_info().luid_valid;
    dx_cfg.preferred_luid = rhi->gpu_info().luid;
    auto dx_res = aether::rhi::D3D12Device::create(dx_cfg);
    REQUIRE(dx_res.has_value());
    std::unique_ptr<aether::rhi::D3D12Device> d3d12(dx_res.value());

    const auto snap_dir = snapshot_out_dir();
    std::filesystem::create_directories(snap_dir);

    for (const auto& [tag, vrm_path] : models) {
        if (vrm_path.empty()) {
            std::printf("[NECK_PROBE:%s] SKIPPED (VRM not found)\n", tag);
            continue;
        }
        auto avatar = load_avatar_file(*rhi, vrm_path);
        REQUIRE(avatar != nullptr);

        std::printf("\n===========================================================================\n");
        std::printf(
            "[NECK_PROBE:%s] File: %s (verts=%u, mats=%zu, submeshes=%zu) face_ref=(%.3f,%.3f,%.3f,valid=%.0f)\n", tag,
            vrm_path.filename().string().c_str(), avatar->requirements.vertex_count, avatar->materials.size(),
            avatar->submeshes.size(), avatar->face_skin_ref_linear[0], avatar->face_skin_ref_linear[1],
            avatar->face_skin_ref_linear[2], avatar->face_skin_ref_linear[3]);

        // Print exact C++ material classification table
        std::vector<uint32_t> dummy_tex(avatar->textures.size(), 1u);
        std::vector<aether::renderer::GpuMaterial> gmats;
        std::vector<aether::renderer::GpuSubmeshDraw> gsubs;
        std::vector<uint32_t> opq, cut, trn;
        aether::renderer::detail::populate_materials_and_submeshes(*avatar, dummy_tex, 0u, UINT32_MAX, gmats, gsubs,
                                                                   opq, cut, trn);
        for (size_t mi = 0; mi < avatar->materials.size(); ++mi) {
            const auto& m = avatar->materials[mi];
            const auto& gm = gmats[mi];
            const std::string mname = aether::renderer::read_material_cstr(*avatar, m.name_offset);
            const uint32_t pf = gm.extra_slots[2];
            const uint32_t rt_class = (pf >> 10u) & 3u;
            uint32_t sm_f = 0u;
            for (const auto& sm : avatar->submeshes) {
                if (sm.material_index == mi) {
                    sm_f |= sm.flags;
                }
            }
            const bool is_face = (rt_class == 0u) || ((sm_f & AETHER_SUBMESH_FLAG_FACE) != 0u) || ((pf & 2u) != 0u);
            const bool is_body_atlas = (pf & (1u << 13u)) != 0u;
            if (is_face || is_body_atlas || mname.find("SKIN") != std::string::npos ||
                mname.find("Face") != std::string::npos || mname.find("Body") != std::string::npos) {
                std::printf(
                    "  Mat[%zu] '%s': rt_class=%u is_face=%d body_skin_atlas=%d mat_flags=0x%04X sm_flags=0x%02X | "
                    "lit=(%.2f,%.2f,%.2f) shade=(%.2f,%.2f,%.2f) shift=%.2f toony=%.2f\n",
                    mi, mname.c_str(), rt_class, is_face ? 1 : 0, is_body_atlas ? 1 : 0, pf, sm_f, gm.lit_color[0],
                    gm.lit_color[1], gm.lit_color[2], gm.shade_color[0], gm.shade_color[1], gm.shade_color[2],
                    gm.emission_and_shift[3], gm.toon_params[0]);
            }
        }

        aether::renderer::NprRenderer vk_npr;
        aether::renderer::RendererConfig rcfg{};
        rcfg.width = kPW;
        rcfg.height = kPH;
        rcfg.output_width = kPW;
        rcfg.output_height = kPH;
        rcfg.enable_rt = rhi->rt_supported();
        REQUIRE(vk_npr.init(*rhi, rcfg).has_value());
        REQUIRE(vk_npr.bind_avatar(avatar.get()).has_value());

        aether::renderer::Dx12FrameGraph dx_fg;
        REQUIRE(dx_fg.init(*d3d12, kPW, kPH, avatar->requirements.vertex_count, false).has_value());
        REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

        aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
        aether::renderer::configure_fixture_defaults(rig.studio_lights[0],
                                                     aether::renderer::StudioFixtureProfile::ArriSkyPanel);
        rig.studio_lights[0].orbit_azimuth_deg = 32.0f;
        rig.studio_lights[0].orbit_elevation_deg = 26.0f;
        const auto cam = make_neck_closeup_camera(*avatar);

        // Compute world key light direction L_key (az=32 deg, el=26 deg)
        const float az_rad = 32.0f * (3.14159265f / 180.0f);
        const float el_rad = 26.0f * (3.14159265f / 180.0f);
        const std::array<float, 3> l_key = {std::cos(el_rad) * std::sin(az_rad), std::sin(el_rad),
                                            std::cos(el_rad) * std::cos(az_rad)};

        auto render_vk_view = [&](uint8_t mode, aether::renderer::ShadingDebugView dv, uint32_t warmup = 4u) {
            rig.lighting_mode = mode;
            std::vector<uint8_t> out;
            for (uint32_t s = 0; s < warmup; ++s) {
                aether::renderer::FrameRenderRequest req{};
                req.avatar = avatar.get();
                req.rig = &rig;
                req.camera = cam;
                req.lighting_mode = mode;
                req.rt_enabled = (mode > 0u) && rhi->rt_supported();
                req.debug_view = dv;
                req.show_checkerboard = false;
                req.frame_index = s + 1u;
                req.completed_frame_index = (s >= 2u) ? (s - 1u) : 0u;
                req.frame_slot = s % aether::rhi::RhiSwapchain::kFramesInFlight;
                REQUIRE(vk_npr.render_headless_frame(req));
            }
            auto rb = vk_npr.graph().readback_image_rgba8(vk_npr.graph().outputs().bgra8_image, VK_IMAGE_LAYOUT_GENERAL,
                                                          kPW, kPH);
            REQUIRE(rb.has_value());
            return rb.value();
        };

        auto render_dx_view = [&](uint8_t mode, aether::renderer::ShadingDebugView dv, uint32_t warmup = 4u) {
            rig.lighting_mode = mode;
            for (uint32_t s = 0; s < warmup; ++s) {
                aether::renderer::FrameRenderRequest req{};
                req.avatar = avatar.get();
                req.rig = &rig;
                req.camera = cam;
                req.lighting_mode = mode;
                req.rt_enabled = (mode > 0u) && d3d12->dxr_supported();
                req.debug_view = dv;
                req.show_checkerboard = false;
                req.frame_index = s + 1u;
                req.frame_slot = s % aether::rhi::kD3D12FrameCount;
                REQUIRE(dx_fg.render_frame(req, true).has_value());
            }
            std::vector<uint8_t> out(static_cast<size_t>(kPW) * kPH * 4u, 0u);
            REQUIRE(dx_fg.readback_rgba8(out));
            return out;
        };

        // Locate exact jaw-to-neck seam row along center x = kPW / 2 using TrueNormals + Albedo discontinuity
        const auto albedo_dx = render_dx_view(2u, aether::renderer::ShadingDebugView::Albedo, 1u);
        const auto norm_dx = render_dx_view(2u, aether::renderer::ShadingDebugView::TrueNormals, 1u);
        const auto final_dx_pt = render_dx_view(2u, aether::renderer::ShadingDebugView::Final, 6u);
        const auto final_vk_pt = render_vk_view(2u, aether::renderer::ShadingDebugView::Final, 6u);

        CHECK(aether::test::save_rgba8_png(snap_dir / (std::string("probe_") + tag + "_dx12_pt.png"), kPW, kPH,
                                           final_dx_pt.data()));
        CHECK(aether::test::save_rgba8_png(snap_dir / (std::string("probe_") + tag + "_vk_pt.png"), kPW, kPH,
                                           final_vk_pt.data()));
        CHECK(aether::test::save_rgba8_png(snap_dir / (std::string("probe_") + tag + "_albedo.png"), kPW, kPH,
                                           albedo_dx.data()));

        const uint32_t cx = kPW / 2u;
        // Scan vertical band around chin/neck (y in [235..315] on 512px portrait centered between head and neck)
        // First find the first sharp downward-facing normal drop or albedo drop below the mouth (~y=220).
        uint32_t y_seam = 0u;
        double best_jump = 0.0;
        for (uint32_t y = 240u; y <= 310u; ++y) {
            const size_t i0 = (static_cast<size_t>(y) * kPW + cx) * 4u;
            const size_t i1 = (static_cast<size_t>(y + 2u) * kPW + cx) * 4u;
            // Check that y is bright face skin (R > 160) and y+2 is darker or normal jumps steeply downward
            const double r0 = albedo_dx[i0];
            const double g0 = albedo_dx[i0 + 1];
            const double b0 = albedo_dx[i0 + 2];
            const double r1 = albedo_dx[i1];
            const double g1 = albedo_dx[i1 + 1];
            const double b1 = albedo_dx[i1 + 2];
            const double ny0 = srgb_u8_to_linear(norm_dx[i0 + 1]) * 2.0 - 1.0;
            const double ny1 = srgb_u8_to_linear(norm_dx[i1 + 1]) * 2.0 - 1.0;
            const double alb_drop = (r0 - r1) + (g0 - g1) + (b0 - b1);
            const double n_jump = std::abs(ny0 - ny1) * 120.0;
            const double score = (r0 > 150.0 && r1 > 110.0) ? (alb_drop + n_jump) : 0.0;
            if (score > best_jump) {
                best_jump = score;
                y_seam = y;
            }
        }
        std::printf("  Centerline scan (x=%u, y=250..305, step=3):\n", cx);
        for (uint32_t y = 250u; y <= 305u; y += 3u) {
            const size_t i = (static_cast<size_t>(y) * kPW + cx) * 4u;
            const double ny = srgb_u8_to_linear(norm_dx[i + 1]) * 2.0 - 1.0;
            const double nz = srgb_u8_to_linear(norm_dx[i + 2]) * 2.0 - 1.0;
            std::printf("    y=%3u: alb=(%3u,%3u,%3u) n_yz=(%+.2f,%+.2f) final_pt=(%3u,%3u,%3u)\n", y, albedo_dx[i],
                        albedo_dx[i + 1], albedo_dx[i + 2], ny, nz, final_dx_pt[i], final_dx_pt[i + 1],
                        final_dx_pt[i + 2]);
        }
        REQUIRE(y_seam > 20u);
        const uint32_t y_chin = y_seam - 10u; // Lower cheek / chin skin cleanly above outline/seam
        const uint32_t y_neck = y_seam + 8u;  // Upper neck skin cleanly below outline/seam (above choker on 6493)
        std::printf("  Seam located at x=%u: y_seam=%u (jump=%.1f) -> Probe Chin=(%u,%u), UpperNeck=(%u,%u)\n", cx,
                    y_seam, best_jump, cx, y_chin, cx, y_neck);

        auto probe_backend = [&](const char* bname, auto&& render_fn) {
            for (uint8_t mode = 0u; mode <= 2u; ++mode) {
                const char* mname = (mode == 0u) ? "Traditional" : ((mode == 1u) ? "HybridRT" : "2.5D_PT");
                const auto img_final = render_fn(mode, aether::renderer::ShadingDebugView::Final, 6u);
                const auto img_alb = render_fn(mode, aether::renderer::ShadingDebugView::Albedo, 1u);
                const auto img_nrm = render_fn(mode, aether::renderer::ShadingDebugView::TrueNormals, 1u);
                const auto img_msk = render_fn(mode, aether::renderer::ShadingDebugView::SdfFace, 1u);
                const auto img_shd = render_fn(mode, aether::renderer::ShadingDebugView::RtShadowDenoised, 1u);
                const auto img_gi = render_fn(mode, aether::renderer::ShadingDebugView::Bounce, 1u);

                auto read_px = [&](const std::vector<uint8_t>& buf, uint32_t x, uint32_t y) {
                    const size_t i = (static_cast<size_t>(y) * kPW + x) * 4u;
                    return std::array<uint8_t, 4>{buf[i + 0u], buf[i + 1u], buf[i + 2u], buf[i + 3u]};
                };
                auto decode_n = [&](std::array<uint8_t, 4> p) {
                    const float nx = static_cast<float>(srgb_u8_to_linear(p[0])) * 2.0f - 1.0f;
                    const float ny = static_cast<float>(srgb_u8_to_linear(p[1])) * 2.0f - 1.0f;
                    const float nz = static_cast<float>(srgb_u8_to_linear(p[2])) * 2.0f - 1.0f;
                    const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                    return std::array<float, 3>{l > 1e-4f ? nx / l : 0.0f, l > 1e-4f ? ny / l : 0.0f,
                                                l > 1e-4f ? nz / l : 1.0f};
                };

                const auto cf = read_px(img_final, cx, y_chin);
                const auto nf = read_px(img_final, cx, y_neck);
                const auto ca = read_px(img_alb, cx, y_chin);
                const auto na = read_px(img_alb, cx, y_neck);
                const auto cn = decode_n(read_px(img_nrm, cx, y_chin));
                const auto nn = decode_n(read_px(img_nrm, cx, y_neck));
                const float c_ndl = cn[0] * l_key[0] + cn[1] * l_key[1] + cn[2] * l_key[2];
                const float n_ndl = nn[0] * l_key[0] + nn[1] * l_key[1] + nn[2] * l_key[2];
                const double c_sh = srgb_u8_to_linear(read_px(img_shd, cx, y_chin)[0]);
                const double n_sh = srgb_u8_to_linear(read_px(img_shd, cx, y_neck)[0]);
                const auto cgi = read_px(img_gi, cx, y_chin);
                const auto ngi = read_px(img_gi, cx, y_neck);
                const double cm = srgb_u8_to_linear(read_px(img_msk, cx, y_chin)[0]);
                const double nm = srgb_u8_to_linear(read_px(img_msk, cx, y_neck)[0]);

                const OklabColor ok_c = rgb_u8_to_oklab(cf[0], cf[1], cf[2]);
                const OklabColor ok_n = rgb_u8_to_oklab(nf[0], nf[1], nf[2]);
                const double dL_pct = std::abs(ok_c.L - ok_n.L) * 100.0;
                const double dE_pct = oklab_delta_e(ok_c, ok_n) * 100.0;

                std::printf("  [%s | %-11s] dL*=%.2f%% dE=%.2f%%\n", bname, mname, dL_pct, dE_pct);
                std::printf("    CHIN: mask=%.2f alb=(%3u,%3u,%3u) N=(%+.2f,%+.2f,%+.2f) N.L=%+.3f RTsh=%.3f "
                            "GI=(%.3f,%.3f,%.3f) Final=(%3u,%3u,%3u) L*=%.4f\n",
                            cm, ca[0], ca[1], ca[2], cn[0], cn[1], cn[2], c_ndl, c_sh, srgb_u8_to_linear(cgi[0]),
                            srgb_u8_to_linear(cgi[1]), srgb_u8_to_linear(cgi[2]), cf[0], cf[1], cf[2], ok_c.L);
                std::printf("    NECK: mask=%.2f alb=(%3u,%3u,%3u) N=(%+.2f,%+.2f,%+.2f) N.L=%+.3f RTsh=%.3f "
                            "GI=(%.3f,%.3f,%.3f) Final=(%3u,%3u,%3u) L*=%.4f\n",
                            nm, na[0], na[1], na[2], nn[0], nn[1], nn[2], n_ndl, n_sh, srgb_u8_to_linear(ngi[0]),
                            srgb_u8_to_linear(ngi[1]), srgb_u8_to_linear(ngi[2]), nf[0], nf[1], nf[2], ok_n.L);

                // Step 3 automated assertions: visually seamless chin-to-neck transition (< 3.0% L* and Delta-E)
                CHECK(dL_pct < 3.0);
                CHECK(dE_pct < 3.0);
                CHECK(n_sh >= 0.90);
            }
        };

        probe_backend("DX12  ", render_dx_view);
        probe_backend("Vulkan", render_vk_view);

        // Verify choker / dark accessory preservation on shared Body_00_SKIN atlas (6493 black leather choker)
        if (std::string(tag) == "6493") {
            const size_t ci = (static_cast<size_t>(y_seam + 24u) * kPW + cx) * 4u;
            CHECK(final_dx_pt[ci + 0u] < 75u);
            CHECK(final_vk_pt[ci + 0u] < 75u);
        }

        // Verify Inspector Neck Seam Blend slider = 0.0 restores raw VRM neck shadow cleanly
        rig.neck_seam_blend = 0.0f;
        const auto off_dx_pt = render_dx_view(2u, aether::renderer::ShadingDebugView::Final, 4u);
        const size_t ic = (static_cast<size_t>(y_chin) * kPW + cx) * 4u;
        const size_t in = (static_cast<size_t>(y_neck) * kPW + cx) * 4u;
        const double off_dL_pct = std::abs(rgb_u8_to_oklab(off_dx_pt[ic], off_dx_pt[ic + 1u], off_dx_pt[ic + 2u]).L -
                                           rgb_u8_to_oklab(off_dx_pt[in], off_dx_pt[in + 1u], off_dx_pt[in + 2u]).L) *
                                  100.0;
        std::printf("  [Slider=0.0 check] DX12 2.5D_PT raw VRM dL*=%.2f%%\n", off_dL_pct);
        CHECK(off_dL_pct > 10.0);
        rig.neck_seam_blend = 1.0f;

        dx_fg.destroy();
        vk_npr.destroy();
        avatar->destroy();
    }

    d3d12->destroy();
    rhi->destroy();
}

TEST_CASE("Phase 2 DX12 Dual-Viewport Zero Schematic Leakage, Ortho, Clay 18% Gray & False Color EL-Zone",
          "[dx12][phase2][lighting_stage]") {
    using namespace aether::renderer;

    aether::rhi::RhiDeviceConfig rcfg{};
    rcfg.enable_validation = false;
    rcfg.require_validation = false;
    rcfg.headless = true;
    auto vk_res = aether::rhi::RhiDevice::create(rcfg);
    if (!vk_res) {
        SUCCEED("Skipped DX12 Phase 2 test: Vulkan helper unavailable");
        return;
    }
    auto rhi = std::unique_ptr<aether::rhi::RhiDevice>(vk_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = rhi->gpu_info().luid_valid;
    dcfg.preferred_luid = rhi->gpu_info().luid;
    auto d3d12_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d12_res) {
        rhi->destroy();
        SUCCEED("Skipped DX12 Phase 2 test: D3D12 unavailable");
        return;
    }
    auto d3d12 = std::unique_ptr<aether::rhi::D3D12Device>(d3d12_res.value());

    aether::test::SyntheticMeshSpec spec{};
    auto mat = aether::test::make_basic_npr_material(0.92f, 0.35f, 0.25f); // Saturated coral-red material
    mat.toon_params[0] = 0.0f;
    spec.materials.push_back(mat);
    const float p0[3] = {-0.55f, 0.70f, 0.0f};
    const float p1[3] = {0.55f, 0.70f, 0.0f};
    const float p2[3] = {0.55f, 1.85f, 0.0f};
    const float p3[3] = {-0.55f, 1.85f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.92f, 0.35f, 0.25f, 0, AETHER_SUBMESH_FLAG_OPAQUE);

    auto av_res = aether::test::upload_synthetic_avatar(*rhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    constexpr uint32_t kTestW = 256u;
    constexpr uint32_t kTestH = 256u;
    Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kTestW, kTestH, 65536u, false).has_value());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

    // Place 6 huge front softboxes + barn doors + honeycomb directly in front of the avatar
    LightingRig rig = LightingRig::default_three_point();
    rig.studio_light_count = 6;
    for (uint32_t i = 0; i < 6u; ++i) {
        configure_fixture_defaults(rig.studio_lights[i], StudioFixtureProfile::RectSoftbox);
        rig.studio_lights[i].use_orbit_angles = false;
        rig.studio_lights[i].position_ws = {-0.75f + 0.30f * static_cast<float>(i), 1.30f, 0.90f};
        rig.studio_lights[i].target_ws = {0.0f, 1.30f, 0.0f};
        rig.studio_lights[i].aim_lock = false;
        rig.studio_lights[i].width_or_outer_m = 0.85f;
        rig.studio_lights[i].height_or_inner_m = 0.85f;
        rig.studio_lights[i].grid_enabled = true;
        rig.studio_lights[i].barn_doors_enabled = true;
        rig.studio_lights[i].intensity_ev100 = (i == 0u) ? 1.0f : 0.12f;
    }

    const PreviewCamera studio_cam = make_stage_camera_preset(StageCameraView::FreePerspective, {0.0f, 1.30f, 0.0f});
    PreviewCamera bcast_cam = studio_cam;
    bcast_cam.yaw_rad = 0.0f;
    bcast_cam.pitch_rad = 0.0f;
    bcast_cam.distance = 2.4f;

    auto run_dx = [&](RenderViewKind kind, const PreviewCamera& cam, StageDiagnosticMode dm, bool req_schem,
                      uint8_t mode, uint64_t fidx, std::vector<uint8_t>& out_rgba, std::vector<uint8_t>& out_bgra) {
        FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.studio_camera = bcast_cam;
        req.has_studio_frustum = true;
        req.studio_frustum_aspect = 1.0f;
        req.view_kind = kind;
        req.stage_diag_mode = dm;
        req.show_stage_schematic = req_schem;
        req.show_stage_grid = true;
        req.preview_25d_flatten = true;
        req.lighting_mode = mode;
        req.rt_enabled = (mode > 0u) && d3d12->dxr_supported();
        req.show_checkerboard = false;
        req.frame_index = fidx;
        req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::kD3D12FrameCount);
        REQUIRE(dx_fg.render_frame(req, true).has_value());
        out_rgba.assign(static_cast<size_t>(kTestW) * kTestH * 4u, 0u);
        out_bgra.assign(static_cast<size_t>(kTestW) * kTestH * 4u, 0u);
        REQUIRE(dx_fg.readback_rgba8(out_rgba));
        REQUIRE(dx_fg.readback_bgra8(out_bgra));
    };

    std::vector<uint8_t> bcast_rgba_0, bcast_bgra_0;
    std::vector<uint8_t> bcast_adv_rgba, bcast_adv_bgra;
    std::vector<uint8_t> stage_lit_rgba, stage_lit_bgra;
    std::vector<uint8_t> stage_clay_rgba, stage_clay_bgra;
    std::vector<uint8_t> stage_fc_rgba, stage_fc_bgra;
    std::vector<uint8_t> stage_ortho_top_rgba, stage_ortho_top_bgra;
    std::vector<uint8_t> bcast_after_rgba, bcast_after_bgra;

    // 1. Clean BroadcastView baseline (mode 0 for bit-exact repeatability across cold/warm frames)
    run_dx(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, false, 0u, 1u, bcast_rgba_0, bcast_bgra_0);

    // 2. Adversarial BroadcastView with show_stage_schematic = true MUST NOT draw schematics (0 bytes leaked!)
    run_dx(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, true, 0u, 1u, bcast_adv_rgba,
           bcast_adv_bgra);
    CHECK(std::memcmp(bcast_rgba_0.data(), bcast_adv_rgba.data(), bcast_rgba_0.size()) == 0);
    CHECK(std::memcmp(bcast_bgra_0.data(), bcast_adv_bgra.data(), bcast_bgra_0.size()) == 0);

    // 3. LightingStageView draws rich schematics onto out_rgba8_, while out_bgra8_ (Spout2) stays 100% clean Broadcast!
    run_dx(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::Lit, true, 1u, 1u, stage_lit_rgba,
           stage_lit_bgra);
    CHECK(std::memcmp(bcast_bgra_0.data(), stage_lit_bgra.data(), bcast_bgra_0.size()) == 0);

    uint32_t stage_diff_px = 0u;
    for (size_t i = 0; i < stage_lit_rgba.size(); i += 4u) {
        if (std::abs(int(stage_lit_rgba[i + 0]) - int(bcast_rgba_0[i + 0])) > 12 ||
            std::abs(int(stage_lit_rgba[i + 1]) - int(bcast_rgba_0[i + 1])) > 12 ||
            std::abs(int(stage_lit_rgba[i + 2]) - int(bcast_rgba_0[i + 2])) > 12) {
            ++stage_diff_px;
        }
    }
    INFO("DX12 Stage schematic diff pixels vs clean Broadcast: " << stage_diff_px);
    CHECK(stage_diff_px > 600u);

    // 4. Clay (18% Gray) drops chromatic spread on saturated coral surface, False Color renders EL-Zone stops, OrthoTop
    // works
    run_dx(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::Clay, false, 1u, 2u, stage_clay_rgba,
           stage_clay_bgra);
    run_dx(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::FalseColor, false, 1u, 3u, stage_fc_rgba,
           stage_fc_bgra);
    const PreviewCamera top_cam = make_stage_camera_preset(StageCameraView::OrthoTop, {0.0f, 1.30f, 0.0f}, 3.2f);
    run_dx(RenderViewKind::LightingStage, top_cam, StageDiagnosticMode::Lit, true, 1u, 4u, stage_ortho_top_rgba,
           stage_ortho_top_bgra);

    // Center pixel at (kTestW/2, kTestH/2): Lit is saturated coral (|R-B| > 60); Clay albedo is 18% gray under warm key
    const size_t center_i = (static_cast<size_t>(kTestH / 2u) * kTestW + (kTestW / 2u)) * 4u;
    const int lit_chroma =
        std::abs(static_cast<int>(bcast_rgba_0[center_i]) - static_cast<int>(bcast_rgba_0[center_i + 2]));
    const int clay_chroma =
        std::abs(static_cast<int>(stage_clay_rgba[center_i]) - static_cast<int>(stage_clay_rgba[center_i + 2]));
    INFO("DX12 Center Lit RGB=(" << int(bcast_rgba_0[center_i]) << "," << int(bcast_rgba_0[center_i + 1]) << ","
                                 << int(bcast_rgba_0[center_i + 2]) << ") Clay RGB=(" << int(stage_clay_rgba[center_i])
                                 << "," << int(stage_clay_rgba[center_i + 1]) << ","
                                 << int(stage_clay_rgba[center_i + 2]) << ") FC RGB=(" << int(stage_fc_rgba[center_i])
                                 << "," << int(stage_fc_rgba[center_i + 1]) << "," << int(stage_fc_rgba[center_i + 2])
                                 << ")");
    CHECK(lit_chroma > 60);
    CHECK(clay_chroma <= 35);
    CHECK(clay_chroma * 3 < lit_chroma);
    CHECK(std::memcmp(stage_fc_rgba.data(), stage_clay_rgba.data(), stage_fc_rgba.size()) != 0);

    // 5. After Stage Clay / Ortho / False Color frames, returning to BroadcastView keeps Broadcast temporal bank
    // untouched!
    run_dx(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, false, 0u, 1u, bcast_after_rgba,
           bcast_after_bgra);
    CHECK(std::memcmp(bcast_rgba_0.data(), bcast_after_rgba.data(), bcast_rgba_0.size()) == 0);

    dx_fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    rhi->destroy();
}

TEST_CASE("Phase 2 Visual Capture on 8590.vrm: Clean Broadcast vs Stage Persp / Top Ortho / Clay / EL-Zone",
          "[dx12][phase2][visual]") {
    using namespace aether::renderer;
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SUCCEED("Skipped 8590.vrm visual capture: VRM not present");
        return;
    }
    auto rhi_res = aether::rhi::RhiDevice::create(aether::rhi::RhiDeviceConfig{false, false, true, true});
    if (!rhi_res) {
        SUCCEED("Skipped visual capture: Vulkan unavailable");
        return;
    }
    auto rhi = std::unique_ptr<aether::rhi::RhiDevice>(rhi_res.value());
    auto avatar = load_avatar_file(*rhi, vrm_path);
    REQUIRE(avatar != nullptr);

    constexpr uint32_t kSnapW = 512u;
    constexpr uint32_t kSnapH = 512u;
    NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, RendererConfig{kSnapW, kSnapH, 0u, 0u, rhi->rt_supported(), false}).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    apply_studio_rig_preset(rig, StudioRigPresetId::ShinkaiGoldenHour, {0.0f, 1.32f, 0.0f});

    PreviewCamera studio_cam{};
    studio_cam.target_x = 0.0f;
    studio_cam.target_y = 1.28f;
    studio_cam.target_z = 0.0f;
    studio_cam.distance = 2.0f;
    studio_cam.fovy_rad = 0.42f;
    studio_cam.yaw_rad = 0.25f;
    studio_cam.pitch_rad = 0.08f;

    auto snap_vk = [&](const std::string& filename, RenderViewKind kind, const PreviewCamera& cam,
                       StageDiagnosticMode dm, bool schem, bool flat25d, uint64_t fidx) {
        FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.studio_camera = studio_cam;
        req.has_studio_frustum = true;
        req.studio_frustum_aspect = 1.0f;
        req.view_kind = kind;
        req.stage_diag_mode = dm;
        req.show_stage_schematic = schem;
        req.show_stage_grid = true;
        req.preview_25d_flatten = flat25d;
        req.selected_studio_light_idx = 0;
        req.lighting_mode = 1;
        req.rt_enabled = rhi->rt_supported();
        req.show_checkerboard = false;
        req.frame_index = fidx;
        req.completed_frame_index = (fidx >= 2u) ? (fidx - 1u) : 0u;
        req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::RhiSwapchain::kFramesInFlight);
        REQUIRE(renderer.render_headless_frame(req).has_value());
        const auto outs = renderer.graph().outputs();
        const VkImage target_vp =
            (kind == RenderViewKind::LightingStage) ? outs.stage_viewport_image : outs.viewport_image;
        auto px =
            renderer.graph().readback_image_rgba8(target_vp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kSnapW, kSnapH);
        REQUIRE(px.has_value());
        const auto dir = snapshot_out_dir();
        std::filesystem::create_directories(dir);
        CHECK(aether::test::save_rgba8_png(dir / filename, kSnapW, kSnapH, px.value().data()));
    };

    const PreviewCamera stage_persp = make_stage_camera_preset(StageCameraView::FreePerspective, {0.0f, 1.15f, 0.0f});
    const PreviewCamera stage_top = make_stage_camera_preset(StageCameraView::OrthoTop, {0.0f, 1.0f, 0.0f}, 4.5f);

    snap_vk("phase2_8590_broadcast_clean.png", RenderViewKind::Broadcast, studio_cam, StageDiagnosticMode::Lit, false,
            true, 1u);
    snap_vk("phase2_8590_stage_persp.png", RenderViewKind::LightingStage, stage_persp, StageDiagnosticMode::Lit, true,
            false, 2u);
    snap_vk("phase2_8590_stage_top_ortho.png", RenderViewKind::LightingStage, stage_top, StageDiagnosticMode::Lit, true,
            false, 3u);
    snap_vk("phase2_8590_stage_clay.png", RenderViewKind::LightingStage, studio_cam, StageDiagnosticMode::Clay, false,
            false, 4u);
    snap_vk("phase2_8590_stage_falsecolor.png", RenderViewKind::LightingStage, studio_cam,
            StageDiagnosticMode::FalseColor, false, false, 5u);

    // Wide (4.44:1 -> 1280x288) & Tall (0.5:1 -> 360x720) Stage snapshots inside LightingStageView
    {
        auto snap_aspect = [&](const std::string& fname, uint32_t w, uint32_t h, const PreviewCamera& cam,
                               uint64_t fidx) {
            REQUIRE(renderer.resize(w, h, 1280u, 720u).has_value());
            FrameRenderRequest req{};
            req.avatar = avatar.get();
            req.rig = &rig;
            req.camera = cam;
            req.studio_camera = studio_cam;
            req.has_studio_frustum = true;
            req.studio_frustum_aspect = 16.0f / 9.0f;
            req.view_kind = RenderViewKind::LightingStage;
            req.stage_viewport_w = w;
            req.stage_viewport_h = h;
            req.stage_diag_mode = StageDiagnosticMode::Lit;
            req.show_stage_schematic = true;
            req.show_stage_grid = true;
            req.preview_25d_flatten = false;
            req.selected_studio_light_idx = 0;
            req.lighting_mode = 1;
            req.rt_enabled = rhi->rt_supported();
            req.show_checkerboard = false;
            req.frame_index = fidx;
            req.completed_frame_index = (fidx >= 2u) ? (fidx - 1u) : 0u;
            req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::RhiSwapchain::kFramesInFlight);
            REQUIRE(renderer.render_headless_frame(req).has_value());
            const auto outs = renderer.graph().outputs();
            auto px = renderer.graph().readback_image_rgba8(outs.stage_viewport_image,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, w, h);
            REQUIRE(px.has_value());
            CHECK(aether::test::save_rgba8_png(snapshot_out_dir() / fname, w, h, px.value().data()));
        };
        snap_aspect("phase2_8590_stage_wide_1280x288.png", 1280u, 288u, stage_persp, 6u);
        snap_aspect("phase2_8590_stage_tall_360x720.png", 360u, 720u, stage_persp, 7u);
    }

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
}

TEST_CASE("LightingStageView DirectX 12 1:1 Pixel Aspect Ratio Across Wide/Tall Resizes & Sub-Rects",
          "[dx12][phase2][lighting_stage][aspect]") {
    using namespace aether::renderer;

    aether::rhi::RhiDeviceConfig rcfg{};
    rcfg.enable_validation = false;
    rcfg.require_validation = false;
    rcfg.headless = true;
    auto vk_res = aether::rhi::RhiDevice::create(rcfg);
    if (!vk_res) {
        SUCCEED("Skipped DX12 LightingStage aspect test: Vulkan helper unavailable");
        return;
    }
    auto rhi = std::unique_ptr<aether::rhi::RhiDevice>(vk_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = rhi->gpu_info().luid_valid;
    dcfg.preferred_luid = rhi->gpu_info().luid;
    auto d3d12_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d12_res) {
        rhi->destroy();
        SUCCEED("Skipped DX12 LightingStage aspect test: D3D12 unavailable");
        return;
    }
    auto d3d12 = std::unique_ptr<aether::rhi::D3D12Device>(d3d12_res.value());

    aether::test::SyntheticMeshSpec spec{};
    auto mat = aether::test::make_basic_npr_material(0.96f, 0.96f, 0.96f);
    mat.toon_params[0] = 0.0f;
    mat.emissive_and_pad[0] = 0.95f;
    mat.emissive_and_pad[1] = 0.95f;
    mat.emissive_and_pad[2] = 0.95f;
    mat.outline_width = 0.0f;
    spec.materials.push_back(mat);
    const float x0 = -0.5f, x1 = 0.5f, y0 = 0.5f, y1 = 1.5f, z0 = -0.5f, z1 = 0.5f;
    const std::array<float, 3> p000{x0, y0, z0}, p100{x1, y0, z0}, p110{x1, y1, z0}, p010{x0, y1, z0};
    const std::array<float, 3> p001{x0, y0, z1}, p101{x1, y0, z1}, p111{x1, y1, z1}, p011{x0, y1, z1};
    aether::test::append_quad(spec, p001.data(), p101.data(), p111.data(), p011.data(),
                              std::array<float, 3>{0, 0, 1}.data(), std::array<float, 3>{1, 0, 0}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    aether::test::append_quad(spec, p100.data(), p000.data(), p010.data(), p110.data(),
                              std::array<float, 3>{0, 0, -1}.data(), std::array<float, 3>{-1, 0, 0}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    aether::test::append_quad(spec, p101.data(), p100.data(), p110.data(), p111.data(),
                              std::array<float, 3>{1, 0, 0}.data(), std::array<float, 3>{0, 0, -1}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    aether::test::append_quad(spec, p000.data(), p001.data(), p011.data(), p010.data(),
                              std::array<float, 3>{-1, 0, 0}.data(), std::array<float, 3>{0, 0, 1}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    aether::test::append_quad(spec, p011.data(), p111.data(), p110.data(), p010.data(),
                              std::array<float, 3>{0, 1, 0}.data(), std::array<float, 3>{1, 0, 0}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    aether::test::append_quad(spec, p000.data(), p100.data(), p101.data(), p001.data(),
                              std::array<float, 3>{0, -1, 0}.data(), std::array<float, 3>{1, 0, 0}.data(), 1, 1, 1, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE);
    auto av_res = aether::test::upload_synthetic_avatar(*rhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    Dx12FrameGraph dx_fg;
    constexpr uint32_t kBufW = 1600u;
    constexpr uint32_t kBufH = 900u;
    REQUIRE(dx_fg.init(*d3d12, kBufW, kBufH, 65536u, false).has_value());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    rig.studio_lights[0].tint_rgb = {1.0f, 1.0f, 1.0f};
    rig.studio_lights[0].intensity_ev100 = 1.8f;
    rig.bloom_intensity = 0.0f;
    rig.satsuei.halation_strength = 0.0f;
    rig.satsuei.light_wrap_strength = 0.0f;

    auto measure_dx = [&](uint32_t vw, uint32_t vh, uint32_t sub_w, uint32_t sub_h, StageCameraView sv,
                          uint64_t fidx) -> std::pair<uint32_t, uint32_t> {
        PreviewCamera cam = make_stage_camera_preset(sv, {0.0f, 1.0f, 0.0f}, 2.20f);
        if (sv == StageCameraView::FreePerspective) {
            cam.yaw_rad = 0.0f;
            cam.pitch_rad = 0.0f;
            cam.distance = 4.5f;
            cam.fovy_rad = 0.52f;
        }
        FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.view_kind = RenderViewKind::LightingStage;
        req.stage_viewport_w = sub_w;
        req.stage_viewport_h = sub_h;
        req.stage_diag_mode = StageDiagnosticMode::Lit;
        req.show_stage_schematic = false;
        req.show_stage_grid = false;
        req.preview_25d_flatten = false;
        req.lighting_mode = 0u;
        req.rt_enabled = false;
        req.aa_mode = 0u;
        req.outline_width_scale = 0.0f;
        req.show_checkerboard = false;
        req.render_csm_shadow = false;
        req.frame_index = fidx;
        req.frame_slot = static_cast<uint32_t>(fidx % aether::rhi::kD3D12FrameCount);
        REQUIRE(dx_fg.render_frame(req, true).has_value());

        std::vector<uint8_t> rgba(static_cast<size_t>(vw) * vh * 4u, 0u);
        REQUIRE(dx_fg.readback_rgba8(rgba));
        const uint32_t mw = (sub_w > 0u) ? sub_w : vw;
        const uint32_t mh = (sub_h > 0u) ? sub_h : vh;
        uint32_t min_x = mw, max_x = 0u, min_y = mh, max_y = 0u;
        for (uint32_t y = 0; y < mh; ++y) {
            for (uint32_t x = 0; x < mw; ++x) {
                const size_t o = (static_cast<size_t>(y) * vw + x) * 4u;
                if (rgba[o] > 30u || rgba[o + 1] > 30u || rgba[o + 2] > 30u) {
                    min_x = std::min(min_x, x);
                    max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y);
                    max_y = std::max(max_y, y);
                }
            }
        }
        REQUIRE(max_x > min_x);
        REQUIRE(max_y > min_y);
        return {max_x - min_x + 1u, max_y - min_y + 1u};
    };

    // 1. Sub-rect zero-hitch mode across all 7 stage camera modes inside 1600x900 buffer
    uint64_t fidx = 1u;
    for (int vi = 0; vi <= 6; ++vi) {
        const auto sv = static_cast<StageCameraView>(vi);
        const auto m16_9 = measure_dx(kBufW, kBufH, 1600u, 900u, sv, fidx++);
        const auto mwide = measure_dx(kBufW, kBufH, 1600u, 360u, sv, fidx++);
        const auto mtall = measure_dx(kBufW, kBufH, 450u, 900u, sv, fidx++);
        const float r_16_9 = static_cast<float>(m16_9.first) / static_cast<float>(m16_9.second);
        const float r_wide = static_cast<float>(mwide.first) / static_cast<float>(mwide.second);
        const float r_tall = static_cast<float>(mtall.first) / static_cast<float>(mtall.second);
        INFO("DX12 subrect vi=" << vi << " 1600x900=" << r_16_9 << " 1600x360=" << r_wide << " 450x900=" << r_tall);
        CHECK(std::fabs(r_16_9 - 1.0f) < 0.005f);
        CHECK(std::fabs(r_wide - 1.0f) < 0.005f);
        CHECK(std::fabs(r_tall - 1.0f) < 0.005f);
        CHECK(std::abs(static_cast<int>(m16_9.second) - static_cast<int>(mtall.second)) <= 1);
    }

    // 2. Explicit Dx12FrameGraph::resize to 1600x360 and 450x900
    REQUIRE(dx_fg.resize(1600u, 360u).has_value());
    const auto r_dx_w = measure_dx(1600u, 360u, 0u, 0u, StageCameraView::OrthoTop, fidx++);
    CHECK(std::fabs((static_cast<float>(r_dx_w.first) / static_cast<float>(r_dx_w.second)) - 1.0f) < 0.005f);

    REQUIRE(dx_fg.resize(450u, 900u).has_value());
    const auto r_dx_t = measure_dx(450u, 900u, 0u, 0u, StageCameraView::FreePerspective, fidx++);
    CHECK(std::fabs((static_cast<float>(r_dx_t.first) / static_cast<float>(r_dx_t.second)) - 1.0f) < 0.005f);

    dx_fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    rhi->destroy();
}

TEST_CASE("Rev 3.1 DX12 Zero-Light Isolation Proof (Single Lighting Authority)", "[dx12][lighting][zero_light]") {
    using namespace aether::renderer;
    aether::rhi::RhiDeviceConfig vcfg{};
    vcfg.enable_validation = false;
    vcfg.require_validation = false;
    vcfg.headless = true;
    auto vk_res = aether::rhi::RhiDevice::create(vcfg);
    if (!vk_res) {
        SUCCEED("Skipped DX12 Zero-Light proof: RhiDevice unavailable");
        return;
    }
    auto vrhi = std::unique_ptr<aether::rhi::RhiDevice>(vk_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = vrhi->gpu_info().luid_valid;
    dcfg.preferred_luid = vrhi->gpu_info().luid;
    auto d3d_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d_res) {
        vrhi->destroy();
        SUCCEED("Skipped DX12 Zero-Light proof: D3D12 unavailable");
        return;
    }
    auto d3d12 = std::unique_ptr<aether::rhi::D3D12Device>(d3d_res.value());

    aether::test::SyntheticMeshSpec spec{};
    auto mat = aether::test::make_basic_npr_material(0.85f, 0.72f, 0.65f);
    mat.toon_params[0] = 0.0f;
    spec.materials.push_back(mat);
    const float p0[3] = {-0.55f, 0.70f, 0.0f}, p1[3] = {0.55f, 0.70f, 0.0f}, p2[3] = {0.55f, 1.85f, 0.0f},
                p3[3] = {-0.55f, 1.85f, 0.0f}, n[3] = {0.0f, 0.0f, 1.0f}, t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.85f, 0.72f, 0.65f, 0u, AETHER_SUBMESH_FLAG_OPAQUE);
    auto av_res = aether::test::upload_synthetic_avatar(*vrhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    constexpr uint32_t kZeroW = 128u;
    constexpr uint32_t kZeroH = 128u;
    Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kZeroW, kZeroH, 4096u, true).has_value());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    for (uint32_t i = 0; i < rig.studio_light_count; ++i) {
        rig.studio_lights[i].enabled = false;
    }
    rig.ambient_floor = 0.0f;
    rig.environment_sky_strength = 0.0f;
    rig.ground_bounce_strength = 0.0f;
    rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = 0.0f;
    rig.bg_rgba[3] = 1.0f;
    rig.bloom_intensity = 0.0f;

    for (uint8_t mode = 0u; mode <= 1u; ++mode) {
        FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = PreviewCamera{0.0f, 1.30f, 0.0f, 0.0f, 0.0f, 2.2f, 0.65f};
        req.frame_index = mode + 1u;
        req.frame_slot = mode;
        req.lighting_mode = mode;
        req.rt_enabled = (mode > 0u);
        req.show_checkerboard = false;
        req.debug_view = ShadingDebugView::Final;
        REQUIRE(dx_fg.render_frame(req, true).has_value());
        std::vector<uint8_t> rgba(static_cast<size_t>(kZeroW) * kZeroH * 4u, 0u);
        REQUIRE(dx_fg.readback_rgba8(rgba));
        uint8_t max_rgb = 0u;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            max_rgb = std::max({max_rgb, rgba[i + 0], rgba[i + 1], rgba[i + 2]});
        }
        INFO("DX12 Zero-Light mode=" << static_cast<int>(mode) << " max_rgb=" << static_cast<int>(max_rgb));
        CHECK(max_rgb == 0u);
    }

    dx_fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    vrhi->destroy();
}

TEST_CASE("Rev 3.1 DX12 Additive Overlay & VRM Parity Proof", "[dx12][pmx][rev31]") {
    using namespace aether::renderer;
    aether::rhi::RhiDeviceConfig vcfg{};
    vcfg.enable_validation = false;
    vcfg.require_validation = false;
    vcfg.headless = true;
    auto vk_res = aether::rhi::RhiDevice::create(vcfg);
    if (!vk_res) {
        SUCCEED("Skipped DX12 Additive Overlay proof: RhiDevice unavailable");
        return;
    }
    auto vrhi = std::unique_ptr<aether::rhi::RhiDevice>(vk_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = vrhi->gpu_info().luid_valid;
    dcfg.preferred_luid = vrhi->gpu_info().luid;
    auto d3d_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d_res) {
        vrhi->destroy();
        SUCCEED("Skipped DX12 Additive Overlay proof: D3D12 unavailable");
        return;
    }
    auto d3d12 = std::unique_ptr<aether::rhi::D3D12Device>(d3d_res.value());

    aether::test::SyntheticMeshSpec spec{};
    spec.materials.push_back(aether::test::make_basic_npr_material(0.25f, 0.25f, 0.28f));
    auto mat_add = aether::test::make_basic_npr_material(1.0f, 1.0f, 1.0f, 1.0f, AETHER_MAT_FLAG_ADDITIVE);
    mat_add.alpha_mode = AETHER_ALPHA_MODE_ADDITIVE;
    mat_add.tex_base = 0u;
    spec.materials.push_back(mat_add);

    AetherTextureDesc td{};
    td.width = 8u;
    td.height = 8u;
    td.format = AETHER_TEX_FORMAT_RGBA8_UNORM;
    td.usage_flags = AETHER_TEX_USAGE_COLOR;
    td.pixel_size = 8u * 8u * 4u;
    spec.tex_descs.push_back(td);
    spec.tex_pixels.resize(td.pixel_size, 0u);
    for (uint32_t y = 0; y < 8u; ++y) {
        for (uint32_t x = 0; x < 8u; ++x) {
            const size_t o = static_cast<size_t>(y * 8u + x) * 4u;
            const bool center = (x >= 3u && x <= 4u && y >= 3u && y <= 4u);
            spec.tex_pixels[o + 0] = spec.tex_pixels[o + 1] = spec.tex_pixels[o + 2] = center ? 255u : 0u;
            spec.tex_pixels[o + 3] = 255u;
        }
    }

    const float p0[3] = {-0.55f, 0.70f, 0.0f}, p1[3] = {0.55f, 0.70f, 0.0f}, p2[3] = {0.55f, 1.85f, 0.0f},
                p3[3] = {-0.55f, 1.85f, 0.0f}, n[3] = {0.0f, 0.0f, 1.0f}, t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.25f, 0.25f, 0.28f, 0u, AETHER_SUBMESH_FLAG_OPAQUE);
    const float q0[3] = {-0.40f, 0.85f, 0.02f}, q1[3] = {0.40f, 0.85f, 0.02f}, q2[3] = {0.40f, 1.65f, 0.02f},
                q3[3] = {-0.40f, 1.65f, 0.02f};
    aether::test::append_quad(spec, q0, q1, q2, q3, n, t, 1.0f, 1.0f, 1.0f, 1u,
                              AETHER_SUBMESH_FLAG_TRANSPARENT | AETHER_SUBMESH_FLAG_ADDITIVE);

    auto av_res = aether::test::upload_synthetic_avatar(*vrhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());
    avatar->desc.source_format = AETHER_SOURCE_FORMAT_VRM1;
    (void)prepare_vrm_and_pmx_parity(*avatar, true);

    constexpr uint32_t kAddW = 128u;
    constexpr uint32_t kAddH = 128u;
    Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kAddW, kAddH, 4096u, true).has_value());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    rig.studio_lights[0].orbit_azimuth_deg = 48.0f;
    rig.studio_lights[0].orbit_elevation_deg = 22.0f;
    rig.studio_lights[0].intensity_ev100 = 1.15f;
    rig.studio_lights[1].enabled = false;
    rig.studio_lights[2].enabled = false;
    rig.ambient_floor = 0.14f;
    rig.environment_sky_strength = 0.14f;
    rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = 0.0f;
    rig.bg_rgba[3] = 1.0f;
    rig.bloom_intensity = 0.0f;

    FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &rig;
    req.camera = PreviewCamera{0.0f, 1.28f, 0.0f, 0.0f, 0.0f, 2.2f, 0.65f};
    req.frame_index = 1u;
    req.frame_slot = 0u;
    req.lighting_mode = 0u;
    req.rt_enabled = false;
    req.debug_view = ShadingDebugView::Final;
    REQUIRE(dx_fg.render_frame(req, true).has_value());

    std::vector<uint8_t> rgba(static_cast<size_t>(kAddW) * kAddH * 4u, 0u);
    REQUIRE(dx_fg.readback_rgba8(rgba));
    auto lum_at = [&](uint32_t x, uint32_t y) {
        const size_t o = (static_cast<size_t>(y) * kAddW + x) * 4u;
        return static_cast<int>(rgba[o]) + static_cast<int>(rgba[o + 1]) + static_cast<int>(rgba[o + 2]);
    };
    CHECK(lum_at(22u, 64u) > 20);
    CHECK(lum_at(34u, 36u) >= (lum_at(22u, 64u) * 85) / 100);
    CHECK(lum_at(64u, 64u) > lum_at(34u, 36u) + 60);

    dx_fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    vrhi->destroy();
}

TEST_CASE("Rev 3.1 Empirical Test #6: Hardware DX12 ID3D12QueryHeap Timestamps, TransparentPass & prev_depth History",
          "[dx12][rev31][timing]") {
    using namespace aether::renderer;
    aether::rhi::RhiDeviceConfig vcfg{};
    vcfg.enable_validation = false;
    vcfg.require_validation = false;
    vcfg.headless = true;
    auto vk_res = aether::rhi::RhiDevice::create(vcfg);
    if (!vk_res) {
        SUCCEED("Skipped Empirical Test #6: RhiDevice unavailable");
        return;
    }
    auto vrhi = std::unique_ptr<aether::rhi::RhiDevice>(vk_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = vrhi->gpu_info().luid_valid;
    dcfg.preferred_luid = vrhi->gpu_info().luid;
    auto d3d_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d_res) {
        vrhi->destroy();
        SUCCEED("Skipped Empirical Test #6: D3D12 unavailable");
        return;
    }
    auto d3d12 = std::unique_ptr<aether::rhi::D3D12Device>(d3d_res.value());

    constexpr uint32_t kTimeW = 128u;
    constexpr uint32_t kTimeH = 128u;
    aether::test::SyntheticMeshSpec spec{};
    spec.materials.push_back(aether::test::make_basic_npr_material(0.7f, 0.6f, 0.8f));
    auto tr_mat = aether::test::make_basic_npr_material(0.9f, 0.9f, 0.9f, 0.45f);
    tr_mat.alpha_mode = AETHER_ALPHA_MODE_BLEND;
    spec.materials.push_back(tr_mat);
    const float p0[3] = {-0.4f, 0.8f, 0.0f}, p1[3] = {0.4f, 0.8f, 0.0f}, p2[3] = {0.4f, 1.7f, 0.0f},
                p3[3] = {-0.4f, 1.7f, 0.0f}, n[3] = {0.0f, 0.0f, 1.0f}, t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.7f, 0.6f, 0.8f, 0u, AETHER_SUBMESH_FLAG_OPAQUE);
    const float q0[3] = {-0.3f, 0.9f, 0.05f}, q1[3] = {0.3f, 0.9f, 0.05f}, q2[3] = {0.3f, 1.6f, 0.05f},
                q3[3] = {-0.3f, 1.6f, 0.05f};
    aether::test::append_quad(spec, q0, q1, q2, q3, n, t, 0.9f, 0.9f, 0.9f, 1u, AETHER_SUBMESH_FLAG_TRANSPARENT);

    auto av_res = aether::test::upload_synthetic_avatar(*vrhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kTimeW, kTimeH, 4096u, true).has_value());
    REQUIRE(dx_fg.hardware_timestamps_supported());
    REQUIRE(dx_fg.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &rig;
    req.camera = PreviewCamera{0.0f, 1.25f, 0.0f, 0.1f, 0.05f, 2.0f, 0.65f};
    req.lighting_mode = 0u;
    req.rt_enabled = false;
    req.debug_view = ShadingDebugView::Final;

    for (uint64_t f = 0; f < 4u; ++f) {
        req.frame_index = f;
        req.frame_slot = static_cast<uint32_t>(f % aether::rhi::kD3D12FrameCount);
        if (f == 2u) {
            dx_fg.invalidate_temporal_history();
        }
        REQUIRE(dx_fg.render_frame(req, true).has_value());
    }

    const auto& snap = dx_fg.timing_snapshot();
    REQUIRE(snap.valid);
    CHECK(snap.gpu_total_ms > 0.0f);
    CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Prepass)] >= 0.0f);
    CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Live2dNpr)] >= 0.0f);
    CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::TransparentPass)] >= 0.0f);
    CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::OutputEncode)] >= 0.0f);
    CHECK(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::FrameTotal)] >= 0.0f);

    REQUIRE(dx_fg.bind_avatar(nullptr).has_value());
    CHECK(dx_fg.bound_avatar() == nullptr);

    dx_fg.destroy();
    avatar->destroy();
    d3d12->destroy();
    vrhi->destroy();
}

TEST_CASE("Empirical Test #3: DX12 VRM vs PMX Toon Parity Audit & Live Parity Shader Parameters",
          "[dx12][rev31][empirical][parity]") {
    using namespace aether::renderer;

    aether::rhi::RhiDeviceConfig vcfg{};
    vcfg.enable_validation = false;
    vcfg.require_validation = false;
    vcfg.headless = true;
    auto vrhi_res = aether::rhi::RhiDevice::create(vcfg);
    if (!vrhi_res) {
        SUCCEED("Skipped DX12 Parity test: Vulkan upload helper unavailable");
        return;
    }
    std::unique_ptr<aether::rhi::RhiDevice> vrhi(vrhi_res.value());

    aether::rhi::D3D12DeviceConfig dcfg{};
    dcfg.enable_debug_layer = false;
    dcfg.use_preferred_luid = vrhi->gpu_info().luid_valid;
    dcfg.preferred_luid = vrhi->gpu_info().luid;
    auto d3d_res = aether::rhi::D3D12Device::create(dcfg);
    if (!d3d_res) {
        vrhi->destroy();
        SUCCEED("Skipped DX12 Parity test: D3D12 device unavailable");
        return;
    }
    std::unique_ptr<aether::rhi::D3D12Device> d3d12(d3d_res.value());

    auto make_dx_avatar = [&](bool is_pmx) -> std::unique_ptr<GpuAvatar> {
        aether::test::SyntheticMeshSpec spec{};
        auto face_m =
            aether::test::make_basic_npr_material(0.92f, 0.78f, 0.72f, 1.0f, AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW);
        if (is_pmx) {
            face_m.flags |= AETHER_MAT_FLAG_IS_PMX;
            face_m.shade_color_gi[0] = 0.74f;
            face_m.shade_color_gi[1] = 0.56f;
            face_m.shade_color_gi[2] = 0.52f;
        } else {
            face_m.shade_color_gi[0] = face_m.base_color[0];
            face_m.shade_color_gi[1] = face_m.base_color[1];
            face_m.shade_color_gi[2] = face_m.base_color[2];
        }
        face_m.outline_width = 0.0f;

        auto cloth_m =
            aether::test::make_basic_npr_material(0.38f, 0.44f, 0.76f, 1.0f, is_pmx ? AETHER_MAT_FLAG_IS_PMX : 0u);
        if (is_pmx) {
            cloth_m.shade_color_gi[0] = 0.22f;
            cloth_m.shade_color_gi[1] = 0.27f;
            cloth_m.shade_color_gi[2] = 0.58f;
        } else {
            cloth_m.shade_color_gi[0] = cloth_m.base_color[0];
            cloth_m.shade_color_gi[1] = cloth_m.base_color[1];
            cloth_m.shade_color_gi[2] = cloth_m.base_color[2];
        }
        cloth_m.outline_width = 0.0f;
        spec.materials = {face_m, cloth_m};

        const float tf[3] = {1.0f, 0.0f, 0.0f};
        const float f0[3] = {-0.18f, 1.24f, 0.08f}, f1[3] = {0.18f, 1.24f, 0.08f}, f2[3] = {0.18f, 1.44f, 0.08f},
                    f3[3] = {-0.18f, 1.44f, 0.08f}, nf[3] = {0.20f, 0.08f, 0.97f};
        aether::test::append_quad(spec, f0, f1, f2, f3, nf, tf, 0.92f, 0.78f, 0.72f, 0u,
                                  AETHER_SUBMESH_FLAG_OPAQUE | AETHER_SUBMESH_FLAG_FACE);
        const float cl0[3] = {-0.48f, 0.86f, 0.04f}, cl1[3] = {-0.04f, 0.86f, 0.04f}, cl2[3] = {-0.04f, 1.20f, 0.04f},
                    cl3[3] = {-0.48f, 1.20f, 0.04f}, nl[3] = {0.48f, 0.25f, 0.84f};
        aether::test::append_quad(spec, cl0, cl1, cl2, cl3, nl, tf, 0.38f, 0.44f, 0.76f, 1u,
                                  AETHER_SUBMESH_FLAG_OPAQUE);
        const float cs0[3] = {0.04f, 0.86f, 0.04f}, cs1[3] = {0.48f, 0.86f, 0.04f}, cs2[3] = {0.48f, 1.20f, 0.04f},
                    cs3[3] = {0.04f, 1.20f, 0.04f}, ns[3] = {-0.85f, -0.15f, 0.50f};
        aether::test::append_quad(spec, cs0, cs1, cs2, cs3, ns, tf, 0.38f, 0.44f, 0.76f, 1u,
                                  AETHER_SUBMESH_FLAG_OPAQUE);

        auto res = aether::test::upload_synthetic_avatar(*vrhi, spec);
        REQUIRE(res.has_value());
        auto av = std::move(res.value());
        av->desc.source_format = is_pmx ? AETHER_SOURCE_FORMAT_PMX : AETHER_SOURCE_FORMAT_VRM1;
        av->pmx_aux_header.is_pmx = is_pmx ? 1u : 0u;
        (void)prepare_vrm_and_pmx_parity(*av, true);
        return av;
    };

    auto vrm = make_dx_avatar(false);
    auto pmx = make_dx_avatar(true);

    constexpr uint32_t kParityW = 160u;
    constexpr uint32_t kParityH = 160u;
    Dx12FrameGraph dx_fg;
    REQUIRE(dx_fg.init(*d3d12, kParityW, kParityH, 4096u, true).has_value());

    LightingRig rig = LightingRig::default_three_point();
    rig.studio_lights[0].orbit_azimuth_deg = 48.0f;
    rig.studio_lights[0].orbit_elevation_deg = 22.0f;
    rig.studio_lights[0].intensity_ev100 = 1.15f;
    rig.studio_lights[1].enabled = false;
    rig.studio_lights[2].enabled = false;
    rig.ambient_floor = 0.14f;
    rig.environment_sky_strength = 0.14f;
    rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = 0.0f;
    rig.bg_rgba[3] = 1.0f;
    rig.bloom_intensity = 0.0f;

    auto render_dx = [&](GpuAvatar* av, uint64_t f) {
        REQUIRE(dx_fg.bind_avatar(av).has_value());
        FrameRenderRequest req{};
        req.avatar = av;
        req.rig = &rig;
        req.camera = PreviewCamera{0.0f, 1.26f, 0.0f, 0.0f, 0.0f, 1.85f, 0.65f};
        req.frame_index = f;
        req.frame_slot = static_cast<uint32_t>(f % aether::rhi::kD3D12FrameCount);
        req.lighting_mode = 0u;
        req.rt_enabled = false;
        req.debug_view = ShadingDebugView::Final;
        REQUIRE(dx_fg.render_frame(req, true).has_value());
        const auto& snap = dx_fg.timing_snapshot();
        CHECK(snap.valid);
        std::vector<uint8_t> rgba(static_cast<size_t>(kParityW) * kParityH * 4u, 0u);
        REQUIRE(dx_fg.readback_rgba8(rgba));
        return rgba;
    };

    const auto img_vrm = render_dx(vrm.get(), 1u);
    const auto img_pmx = render_dx(pmx.get(), 2u);
    auto patch_luma = [&](const std::vector<uint8_t>& img, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        double sum = 0.0;
        uint32_t cnt = 0u;
        for (uint32_t y = y0; y <= y1 && y < kParityH; ++y) {
            for (uint32_t x = x0; x <= x1 && x < kParityW; ++x) {
                const size_t o = (static_cast<size_t>(y) * kParityW + x) * 4u;
                sum += 0.2126 * img[o] + 0.7152 * img[o + 1] + 0.0722 * img[o + 2];
                ++cnt;
            }
        }
        return cnt > 0u ? (sum / static_cast<double>(cnt)) : 0.0;
    };
    const double vrm_lit = patch_luma(img_vrm, 36u, 96u, 62u, 114u);
    const double vrm_shd = patch_luma(img_vrm, 98u, 96u, 124u, 114u);
    const double pmx_lit = patch_luma(img_pmx, 36u, 96u, 62u, 114u);
    const double pmx_shd = patch_luma(img_pmx, 98u, 96u, 124u, 114u);
    REQUIRE(vrm_lit > vrm_shd + 8.0);
    REQUIRE(pmx_lit > pmx_shd + 8.0);
    const double vrm_ratio = vrm_lit / std::max(1.0, vrm_shd);
    const double pmx_ratio = pmx_lit / std::max(1.0, pmx_shd);
    CHECK(std::abs(vrm_ratio - pmx_ratio) / std::max(0.01, pmx_ratio) <= 0.10);

    dx_fg.destroy();
    vrm->destroy();
    pmx->destroy();
    d3d12->destroy();
    vrhi->destroy();
}
