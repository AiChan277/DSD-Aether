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
#include "renderer/circadian_lighting.hpp"
#include "renderer/dx12_bridge.hpp"
#include "renderer/dx12_frame_graph.hpp"
#include "renderer/npr_renderer.hpp"
#include "renderer/sdf_generator.hpp"
#include "rhi/d3d12_device.hpp"
#include "rhi/rhi_device.hpp"
#include "rhi/rhi_swapchain.hpp"
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
    rig.lights[0].orbit_azimuth_deg = 52.0f;
    rig.lights[0].orbit_elevation_deg = 32.0f;
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
        req.show_orbit_orb = false;
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
        req.show_orbit_orb = false;
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
    apply_circadian_to_rig(golden, rig);
    CHECK(std::abs(rig.lights[0].intensity - golden.key_intensity) < 1e-4f);
    CHECK(std::abs(rig.lights[2].intensity - golden.rim_intensity) < 1e-4f);
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
}
