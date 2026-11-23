#include "core/hot_path_guard.hpp"
#include "flip_metric.hpp"
#include "renderer/npr_renderer.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

struct SceneCapture {
    std::string scene_name{};
    bool rt_enabled{true};
    uint32_t width{640};
    uint32_t height{360};
    std::vector<uint8_t> rgba{};
};

[[nodiscard]] aether::Expected<std::vector<SceneCapture>>
render_all_canonical_scenes(uint32_t preferred_vendor_id, bool prefer_integrated, std::string& out_gpu_name) {
    aether::rhi::RhiDeviceConfig dcfg{};
    dcfg.enable_validation = true;
    dcfg.require_validation = true;
    dcfg.headless = true;
    dcfg.enable_ray_tracing = true;
    dcfg.preferred_vendor_id = preferred_vendor_id;
    dcfg.prefer_integrated = prefer_integrated;

    auto dev_res = aether::rhi::RhiDevice::create(dcfg);
    if (!dev_res) {
        return aether::make_unexpected(dev_res.error());
    }
    std::unique_ptr<aether::rhi::RhiDevice> rhi(dev_res.value());
    out_gpu_name = rhi->gpu_info().device_name;

    auto avatar_res = aether::test::load_reference_vrm_avatar(*rhi);
    if (!avatar_res) {
        rhi->destroy();
        return aether::make_unexpected(avatar_res.error());
    }
    auto avatar = std::move(avatar_res.value());

    constexpr uint32_t kW = 640;
    constexpr uint32_t kH = 360;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    if (auto init_res = renderer.init(*rhi, rcfg); !init_res) {
        avatar->destroy();
        rhi->destroy();
        return aether::make_unexpected(init_res.error());
    }
    if (auto bind_res = renderer.bind_avatar(avatar.get()); !bind_res) {
        renderer.destroy();
        avatar->destroy();
        rhi->destroy();
        return aether::make_unexpected(bind_res.error());
    }

    const auto scenes = aether::test::make_canonical_scenes();
    std::vector<SceneCapture> captures;
    captures.reserve(scenes.size() * 2U);

    uint64_t frame_idx = 0;
    for (const bool rt_on : {true, false}) {
        if (rt_on && !rhi->rt_supported()) {
            continue;
        }
        for (const auto& sc : scenes) {
            aether::renderer::FrameRenderRequest req{};
            req.avatar = avatar.get();
            req.rig = &sc.rig;
            req.camera = sc.camera;
            req.rt_enabled = rt_on;
            req.show_checkerboard = sc.checkerboard;

            // Render 3 warmup frames so SVGF-lite / SIGMA temporal history converges cleanly
            for (int w = 0; w < 3; ++w) {
                req.frame_index = frame_idx;
                req.completed_frame_index = (frame_idx >= 2) ? (frame_idx - 2) : 0;
                req.frame_slot = static_cast<uint32_t>(frame_idx % aether::rhi::RhiSwapchain::kFramesInFlight);
                auto rr = renderer.render_headless_frame(req);
                if (!rr) {
                    renderer.destroy();
                    avatar->destroy();
                    rhi->destroy();
                    return aether::make_unexpected(rr.error());
                }
                ++frame_idx;
            }

            const auto outs = renderer.graph().outputs();
            const VkImage src_img = sc.checkerboard ? outs.viewport_image : outs.bgra8_image;
            const VkImageLayout src_layout =
                sc.checkerboard ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            auto rb = renderer.graph().readback_image_rgba8(src_img, src_layout, kW, kH);
            if (!rb) {
                renderer.destroy();
                avatar->destroy();
                rhi->destroy();
                return aether::make_unexpected(rb.error());
            }
            captures.push_back(SceneCapture{sc.name, rt_on, kW, kH, std::move(rb.value())});
        }
    }

    const uint64_t val_errors = rhi->validation_error_count();
    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    if (val_errors != 0ULL) {
        return aether::make_unexpected(
            aether::Error::make(aether::ErrorCode::VulkanError, "Vulkan validation errors during golden capture"));
    }
    return captures;
}

} // namespace

TEST_CASE("FLIP evaluator unit axioms hold", "[phase4][golden][flip]") {
    constexpr uint32_t kW = 32;
    constexpr uint32_t kH = 32;
    std::vector<uint8_t> black(kW * kH * 4U, 0);
    std::vector<uint8_t> white(kW * kH * 4U, 255);
    std::vector<uint8_t> subtle(kW * kH * 4U, 0);
    std::vector<uint8_t> strong(kW * kH * 4U, 0);
    for (size_t i = 0; i < kW * kH; ++i) {
        black[i * 4 + 3] = 255;
        subtle[i * 4 + 0] = 18;
        subtle[i * 4 + 1] = 18;
        subtle[i * 4 + 2] = 18;
        subtle[i * 4 + 3] = 255;
        strong[i * 4 + 0] = 140;
        strong[i * 4 + 1] = 140;
        strong[i * 4 + 2] = 140;
        strong[i * 4 + 3] = 255;
    }

    const auto same = aether::test::evaluate_flip_rgba8(white.data(), white.data(), kW, kH);
    CHECK(same.mean == 0.0);
    CHECK(same.max == 0.0);

    const auto bw = aether::test::evaluate_flip_rgba8(black.data(), white.data(), kW, kH);
    CHECK(bw.mean > 0.90);

    const auto s1 = aether::test::evaluate_flip_rgba8(black.data(), subtle.data(), kW, kH);
    const auto s2 = aether::test::evaluate_flip_rgba8(black.data(), strong.data(), kW, kH);
    CHECK(s1.mean > 0.0);
    CHECK(s2.mean > s1.mean);
}

TEST_CASE("Canonical 6-scene golden regression and dual-GPU FLIP parity <= 0.05", "[phase4][golden][validation]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    const auto adapters_res = aether::rhi::RhiDevice::enumerate_vulkan_devices();
    REQUIRE(adapters_res.has_value());

    std::string primary_name;
    auto primary_res = render_all_canonical_scenes(0, false, primary_name);
    INFO("Primary capture error: " << (!primary_res ? primary_res.error().c_str() : ""));
    REQUIRE(primary_res.has_value());
    const auto& primary = primary_res.value();
    REQUIRE(primary.size() >= 6U);

    const auto golden_dir = std::filesystem::path(AETHER_REPO_ROOT) / "tests" / "golden" / "references";
    bool update_goldens = false;
    {
        char* env_buf = nullptr;
        size_t env_len = 0;
        if (_dupenv_s(&env_buf, &env_len, "AETHER_UPDATE_GOLDENS") == 0 && env_buf != nullptr) {
            update_goldens = (std::string(env_buf) == "1");
            std::free(env_buf);
        }
    }

    for (const auto& cap : primary) {
        const std::string fname = cap.scene_name + (cap.rt_enabled ? "_rt_on.png" : "_rt_off.png");
        const auto ref_path = golden_dir / fname;
        if (update_goldens) {
            REQUIRE(aether::test::save_rgba8_png(ref_path, cap.width, cap.height, cap.rgba.data()));
        } else {
            INFO("Missing locked golden image: " << ref_path.string());
            auto ref_png = aether::test::load_rgba8_png(ref_path);
            REQUIRE(ref_png.has_value());
            REQUIRE(ref_png.value().width == cap.width);
            REQUIRE(ref_png.value().height == cap.height);
            const auto flip =
                aether::test::evaluate_flip_rgba8(ref_png.value().rgba.data(), cap.rgba.data(), cap.width, cap.height);
            std::printf("[GOLDEN-REG] %-32s (%s) FLIP mean=%.5f p95=%.5f max=%.5f\n", fname.c_str(),
                        primary_name.c_str(), flip.mean, flip.p95, flip.max);
            CHECK(flip.mean <= 0.05);
        }
    }

    // Physical cross-vendor parity on secondary Intel/AMD GPU if present on host (ADR-0010)
    uint32_t secondary_vendor = 0;
    for (const auto& ad : adapters_res.value()) {
        if (ad.vendor_id == 0x8086u || ad.vendor_id == 0x1002u) {
            secondary_vendor = ad.vendor_id;
            break;
        }
    }
    if (secondary_vendor != 0U) {
        std::string sec_name;
        auto sec_res = render_all_canonical_scenes(secondary_vendor, secondary_vendor == 0x8086u, sec_name);
        INFO("Secondary GPU capture error: " << (!sec_res ? sec_res.error().c_str() : ""));
        REQUIRE(sec_res.has_value());
        const auto& secondary = sec_res.value();
        for (const auto& scap : secondary) {
            auto it = std::find_if(primary.begin(), primary.end(), [&](const SceneCapture& p) {
                return p.scene_name == scap.scene_name && p.rt_enabled == scap.rt_enabled;
            });
            REQUIRE(it != primary.end());
            const auto flip =
                aether::test::evaluate_flip_rgba8(it->rgba.data(), scap.rgba.data(), scap.width, scap.height);
            std::printf("[GOLDEN-XVENDOR] %-32s (%s vs %s) FLIP mean=%.5f p95=%.5f\n", scap.scene_name.c_str(),
                        primary_name.c_str(), sec_name.c_str(), flip.mean, flip.p95);
            CHECK(flip.mean <= 0.05);
        }
    }

    aether_runtime_shutdown();
}
