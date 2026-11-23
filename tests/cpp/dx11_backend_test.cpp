// DirectX 11 render backend + garbage-collector integration (ADR-0016).
// Renders the reference avatar through the Vulkan frame graph and through Dx11Bridge (shared texture and
// CPU copy), checks the DX11 image reaches the Vulkan output encode with a matching silhouette, that Vulkan
// validation stays clean, and that replaced bridge resources are reclaimed by the GC after the GPU passes them.

#include "core/garbage_collector.hpp"
#include "renderer/dx11_bridge.hpp"
#include "renderer/npr_renderer.hpp"
#include "rhi/rhi_swapchain.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr uint32_t kW = 640;
constexpr uint32_t kH = 360;

struct Mask {
    std::vector<uint8_t> covered;
    uint64_t count{0};
};

Mask alpha_mask(const std::vector<uint8_t>& rgba) {
    Mask m{};
    m.covered.resize(static_cast<size_t>(kW) * kH, 0U);
    for (size_t i = 0; i < m.covered.size(); ++i) {
        if (rgba[i * 4U + 3U] > 32U) {
            m.covered[i] = 1U;
            ++m.count;
        }
    }
    return m;
}

double iou(const Mask& a, const Mask& b) {
    uint64_t inter = 0;
    uint64_t uni = 0;
    for (size_t i = 0; i < a.covered.size(); ++i) {
        inter += (a.covered[i] & b.covered[i]);
        uni += (a.covered[i] | b.covered[i]);
    }
    return uni == 0U ? 0.0 : static_cast<double>(inter) / static_cast<double>(uni);
}

struct Harness {
    std::unique_ptr<aether::rhi::RhiDevice> rhi;
    std::unique_ptr<aether::renderer::GpuAvatar> avatar;
    aether::renderer::NprRenderer renderer;
    aether::renderer::LightingRig rig = aether::renderer::LightingRig::default_three_point();
    aether::renderer::PreviewCamera camera{};
    uint64_t frame_idx{0};

    bool init() {
        aether::rhi::RhiDeviceConfig dcfg{};
        dcfg.enable_validation = true;
        dcfg.require_validation = true;
        dcfg.headless = true;
        dcfg.enable_ray_tracing = false;
        auto dev_res = aether::rhi::RhiDevice::create(dcfg);
        if (!dev_res) {
            std::printf("device: %s\n", dev_res.error().c_str());
            return false;
        }
        rhi.reset(dev_res.value());
        auto av = aether::test::load_reference_vrm_avatar(*rhi);
        if (!av) {
            std::printf("avatar: %s\n", av.error().c_str());
            return false;
        }
        avatar = std::move(av.value());
        aether::renderer::RendererConfig rcfg{};
        rcfg.width = kW;
        rcfg.height = kH;
        rcfg.enable_rt = false;
        if (auto rc = renderer.init(*rhi, rcfg); !rc) {
            std::printf("renderer: %s\n", rc.error().c_str());
            return false;
        }
        if (auto rc = renderer.bind_avatar(avatar.get()); !rc) {
            std::printf("bind: %s\n", rc.error().c_str());
            return false;
        }
        camera = aether::renderer::fit_camera_to_avatar(*avatar);
        return true;
    }

    // Renders one frame; `bridge` == nullptr renders the native Vulkan path.
    std::vector<uint8_t> frame(aether::renderer::Dx11Bridge* bridge) {
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = camera;
        req.rt_enabled = false;
        req.show_checkerboard = false;
        if (bridge != nullptr && bridge->render(*avatar, rig, camera)) {
            bridge->fill_request(req);
        }
        req.frame_index = frame_idx;
        req.completed_frame_index = frame_idx >= 2U ? frame_idx - 2U : 0U;
        req.frame_slot = static_cast<uint32_t>(frame_idx % aether::rhi::RhiSwapchain::kFramesInFlight);
        ++frame_idx;
        if (!renderer.render_headless_frame(req)) {
            return {};
        }
        if (bridge != nullptr && req.external_color_image != VK_NULL_HANDLE) {
            bridge->on_submitted(renderer.frame_timeline_value());
        }
        const auto outs = renderer.graph().outputs();
        auto rb = renderer.graph().readback_image_rgba8(outs.bgra8_image, VK_IMAGE_LAYOUT_GENERAL, kW, kH);
        return rb ? std::move(rb.value()) : std::vector<uint8_t>{};
    }

    void shutdown() {
        rhi->wait_idle();
        renderer.destroy();
        if (avatar) {
            avatar->destroy();
        }
        rhi->destroy();
    }
};

uint64_t timeline_now(const aether::rhi::RhiDevice& rhi) {
    uint64_t v = 0;
    (void)vkGetSemaphoreCounterValue(rhi.device(), rhi.frame_timeline(), &v);
    return v;
}

} // namespace

TEST_CASE("DirectX 11 backend feeds the Vulkan output encode and GC reclaims replaced resources",
          "[dx11][backend][gc][validation]") {
    Harness h{};
    REQUIRE(h.init());

    // Reference silhouette from the native Vulkan frame graph (two frames so temporal state settles).
    (void)h.frame(nullptr);
    const auto vk_rgba = h.frame(nullptr);
    REQUIRE(vk_rgba.size() == static_cast<size_t>(kW) * kH * 4U);
    const Mask vk_mask = alpha_mask(vk_rgba);
    REQUIRE(vk_mask.count > (kW * kH) / 100U);

    for (const bool force_cpu : {false, true}) {
        DYNAMIC_SECTION((force_cpu ? "CPU copy path" : "shared texture path")) {
            aether::GarbageCollector gc;
            aether::renderer::Dx11Bridge bridge;
            bridge.attach(*h.rhi, gc);
            bridge.set_force_cpu_copy(force_cpu);
            bridge.bind_avatar(h.avatar.get());
            const auto outs = h.renderer.graph().outputs();
            REQUIRE(bridge
                        .configure(outs.output_width, outs.output_height,
                                   static_cast<uint32_t>(h.avatar->rest_vertices.size()),
                                   static_cast<uint32_t>(h.avatar->rest_indices.size()))
                        .has_value());
            REQUIRE(bridge.ready());
            if (force_cpu) {
                REQUIRE_FALSE(bridge.stats().shared_texture);
            }

            // First frame of the shared path only establishes the image layout; the next frames carry pixels.
            std::vector<uint8_t> dx_rgba;
            for (int i = 0; i < 3; ++i) {
                dx_rgba = h.frame(&bridge);
            }
            REQUIRE(dx_rgba.size() == static_cast<size_t>(kW) * kH * 4U);
            const Mask dx_mask = alpha_mask(dx_rgba);
            const double overlap = iou(vk_mask, dx_mask);
            std::printf("[DX11_BACKEND] path=%s vk_px=%llu dx11_px=%llu iou=%.3f frames=%llu\n",
                        bridge.stats().shared_texture ? "shared" : "cpu",
                        static_cast<unsigned long long>(vk_mask.count), static_cast<unsigned long long>(dx_mask.count),
                        overlap, static_cast<unsigned long long>(bridge.stats().frames));
            CHECK(dx_mask.count > (kW * kH) / 100U);
            // Same camera and geometry: silhouettes must line up (an upside-down or offset image fails this).
            CHECK(overlap > 0.55);

            // Reconfigure to a new size: the old Vulkan objects go to the GC, not straight to vkDestroy.
            const uint64_t reclaimed_before = gc.stats().retired_reclaimed_total;
            REQUIRE(bridge
                        .configure(outs.output_width / 2U, outs.output_height / 2U,
                                   static_cast<uint32_t>(h.avatar->rest_vertices.size()),
                                   static_cast<uint32_t>(h.avatar->rest_indices.size()))
                        .has_value());
            h.rhi->wait_idle();
            (void)gc.collect_epoch(timeline_now(*h.rhi), 16U);
            CHECK(gc.stats().retired_reclaimed_total > reclaimed_before);

            bridge.release();
            h.rhi->wait_idle();
            CHECK(gc.flush_all_epochs() >= 1U);
            CHECK(gc.stats().retired_pending == 0U);
        }
    }

    CHECK(h.rhi->validation_error_count() == 0U);
    h.shutdown();
}

TEST_CASE("Frame-graph view-projection maps the camera target to the clip-space centre", "[dx11][camera]") {
    aether::renderer::PreviewCamera cam{};
    cam.target_x = 0.1f;
    cam.target_y = 1.2f;
    cam.target_z = -0.3f;
    const auto vp = aether::renderer::compute_view_proj(cam, 16.0f / 9.0f);
    const float p[4] = {cam.target_x, cam.target_y, cam.target_z, 1.0f};
    float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            clip[r] += vp[static_cast<size_t>(c * 4 + r)] * p[c];
        }
    }
    REQUIRE(clip[3] > 0.0f);
    CHECK(std::abs(clip[0] / clip[3]) < 1e-4f);
    CHECK(std::abs(clip[1] / clip[3]) < 1e-4f);
}
