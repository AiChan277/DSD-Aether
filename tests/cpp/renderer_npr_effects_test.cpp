#include "animation/spring_bone.hpp"
#include "core/hot_path_guard.hpp"
#include "flip_metric.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/npr_renderer.hpp"
#include "renderer/sdf_generator.hpp"
#include "renderer/studio_lighting.hpp"
#include "rhi/d3d12_device.hpp"
#include "synthetic_avatar_helper.hpp"
#include "test_png_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

[[nodiscard]] std::unique_ptr<aether::rhi::RhiDevice> create_test_rhi(bool enable_rt = true) {
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = true;
    cfg.require_validation = true;
    cfg.headless = true;
    cfg.enable_ray_tracing = enable_rt;
    auto res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(res.has_value());
    REQUIRE(res.value()->validation_active());
    REQUIRE(res.value()->sync_validation_active());
    return std::unique_ptr<aether::rhi::RhiDevice>(res.value());
}

[[nodiscard]] std::vector<uint8_t> render_synthetic_once(aether::rhi::RhiDevice& rhi,
                                                         const aether::test::SyntheticMeshSpec& spec, uint32_t width,
                                                         uint32_t height, const aether::renderer::PreviewCamera& cam,
                                                         const aether::renderer::LightingRig& rig,
                                                         bool checkerboard = false, bool rt_enabled = false,
                                                         bool use_bgra8_premul = false) {
    auto avatar_res = aether::test::upload_synthetic_avatar(rhi, spec);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = width;
    rcfg.height = height;
    rcfg.enable_rt = rt_enabled && rhi.rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &rig;
    req.camera = cam;
    req.rt_enabled = rcfg.enable_rt;
    req.show_checkerboard = checkerboard;
    req.show_orbit_orb = false;
    req.render_csm_shadow = false;
    REQUIRE(renderer.render_headless_frame(req).has_value());

    const auto outs = renderer.graph().outputs();
    const VkImage img = use_bgra8_premul ? outs.bgra8_image : outs.viewport_image;
    const VkImageLayout lay = use_bgra8_premul ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auto pixels = renderer.graph().readback_image_rgba8(img, lay, width, height);
    REQUIRE(pixels.has_value());
    REQUIRE(rhi.validation_error_count() == 0);
    REQUIRE(rhi.validation_warning_count() == 0);

    renderer.destroy();
    avatar->destroy();
    return std::move(pixels.value());
}

} // namespace

TEST_CASE("Sync2 read-only D32_S8 stencil, 14 shading debug views, CVD heatmap, and zero hot-path allocs",
          "[phase4][renderer][validation]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    auto rhi = create_test_rhi(true);
    auto avatar_res = aether::test::load_reference_vrm_avatar(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 512;
    rcfg.height = 512;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    std::vector<float> bone_stress(std::max<size_t>(avatar->joints.size(), 1U), 0.35f);
    if (bone_stress.size() > 4U) {
        bone_stress[1] = 0.72f;
        bone_stress[2] = 0.96f;
    }

    aether::renderer::FrameRenderRequest req{};
    req.avatar = avatar.get();
    req.rig = &renderer.lighting();
    req.camera = aether::renderer::PreviewCamera{0.0f, 1.32f, 0.0f, 0.25f, 0.05f, 1.4f, 0.55f};
    req.joint_stress = bone_stress.data();
    req.joint_stress_count = static_cast<uint32_t>(bone_stress.size());
    req.show_skeleton_heatmap = true;
    req.show_orbit_orb = true;
    req.rt_enabled = rhi->rt_supported();

    // Verify all 14 ShadingDebugView modes render cleanly with zero validation errors/warnings and 0 heap allocs
    for (uint8_t mode = 0; mode < static_cast<uint8_t>(aether::renderer::ShadingDebugView::Count); ++mode) {
        req.frame_index = mode;
        req.completed_frame_index = (mode >= 2) ? (mode - 2) : 0;
        req.frame_slot = mode % aether::rhi::RhiSwapchain::kFramesInFlight;
        req.debug_view = static_cast<aether::renderer::ShadingDebugView>(mode);

        aether::HotPathGuard::reset_thread_violations();
        bool mode_ok = false;
        {
            AETHER_HOT_PATH_SCOPE;
            mode_ok = renderer.render_headless_frame(req).has_value();
        }
        REQUIRE(mode_ok);
#if !defined(__SANITIZE_ADDRESS__)
        // Under ASan, process-wide malloc hooks count internal allocations inside Vulkan layers/drivers
        // invoked by vkCmd* / vkQueueSubmit2; MSVC Debug CRT verifies zero heap allocs in AETHER C++ code.
        CHECK(aether::HotPathGuard::thread_violations() == 0);
#endif
    }

    // Verify 128x128 Light Orbit Orb rendered non-empty pixels
    const auto outs = renderer.graph().outputs();
    auto orb_px =
        renderer.graph().readback_image_rgba8(outs.orbit_orb_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 128, 128);
    REQUIRE(orb_px.has_value());
    int non_zero_orb = 0;
    for (size_t i = 0; i < static_cast<size_t>(128) * 128U; ++i) {
        if (orb_px.value()[i * 4 + 3] > 200) {
            ++non_zero_orb;
        }
    }
    CHECK(non_zero_orb > 4000);
    CHECK(rhi->validation_error_count() == 0);
    CHECK(rhi->validation_warning_count() == 0);

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Optical Invariant 1: Z-flattening reduces 0.5 m nose foreshortening >= 70% while keeping occlusion",
          "[phase4][effects][zflatten]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    // Mat 0: white base + behind-nose occludee (z_flat=0.0); Mat 1: red protruding nose (z=+0.18, z_flat=0.85).
    spec.materials.push_back(aether::test::make_basic_npr_material(0.95f, 0.95f, 0.95f, 1.0f, AETHER_MAT_FLAG_UNLIT));
    spec.materials.push_back(aether::test::make_basic_npr_material(0.95f, 0.02f, 0.02f, 1.0f, AETHER_MAT_FLAG_UNLIT));

    const float n_z[3] = {0.0f, 0.0f, 1.0f};
    const float tan_z[3] = {0.0f, 0.0f, 1.0f};
    {
        // Reference face strip at head pivot z = 0.0 (lower half of frame)
        const float p0[3] = {-0.04f, -0.08f, 0.0f}, p1[3] = {0.04f, -0.08f, 0.0f};
        const float p2[3] = {0.04f, -0.01f, 0.0f}, p3[3] = {-0.04f, -0.01f, 0.0f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n_z, tan_z, 0.0f, 0.0f, 0.0f, 0, AETHER_SUBMESH_FLAG_OPAQUE);
    }
    {
        // Occludee white strip at z = +0.05 (z_flat=0.0): remains closer in flattened Z than nose when k=0.85,
        // verifying clip.z un-flattened depth still keeps the nose in front (`§10.6`).
        const float p0[3] = {-0.04f, 0.01f, 0.05f}, p1[3] = {0.04f, 0.01f, 0.05f};
        const float p2[3] = {0.04f, 0.08f, 0.05f}, p3[3] = {-0.04f, 0.08f, 0.05f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n_z, tan_z, 0.0f, 0.0f, 0.0f, 0, AETHER_SUBMESH_FLAG_OPAQUE);
    }
    {
        // Protruding nose quad at z = +0.18 with z_flatten_weight = 0.85
        const float p0[3] = {-0.04f, 0.01f, 0.18f}, p1[3] = {0.04f, 0.01f, 0.18f};
        const float p2[3] = {0.04f, 0.08f, 0.18f}, p3[3] = {-0.04f, 0.08f, 0.18f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n_z, tan_z, 0.85f, 0.0f, 0.0f, 1, AETHER_SUBMESH_FLAG_OPAQUE);
    }

    // Close-up 0.50 m camera straight-on so un-flattened nose suffers ~56% "big nose" magnification
    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.50f, 0.60f};
    auto measure_nose_foreshortening = [&](float flatness) -> double {
        auto rig = aether::renderer::LightingRig::default_three_point();
        rig.global_live2d_flatness = flatness;
        rig.bg_rgba[0] = 0.0f;
        rig.bg_rgba[1] = 0.0f;
        rig.bg_rgba[2] = 0.0f;
        rig.bg_rgba[3] = 1.0f;
        const auto px = render_synthetic_once(*rhi, spec, 256, 256, cam, rig, false, false, false);
        uint32_t red_min_x = 256, red_max_x = 0, red_count = 0;
        uint32_t ref_min_x = 256, ref_max_x = 0, ref_count = 0;
        for (uint32_t y = 0; y < 256; ++y) {
            for (uint32_t x = 0; x < 256; ++x) {
                const size_t i = (static_cast<size_t>(y) * 256 + x) * 4;
                if (px[i + 0] > 160 && px[i + 1] < 80) {
                    red_min_x = std::min(red_min_x, x);
                    red_max_x = std::max(red_max_x, x);
                    ++red_count;
                } else if (y > 128 && px[i + 0] > 160 && px[i + 1] > 160 && px[i + 2] > 160) {
                    ref_min_x = std::min(ref_min_x, x);
                    ref_max_x = std::max(ref_max_x, x);
                    ++ref_count;
                }
            }
        }
        REQUIRE(red_count > 100U);
        REQUIRE(ref_count > 100U);
        const double red_w = static_cast<double>(red_max_x - red_min_x + 1U);
        const double ref_w = static_cast<double>(ref_max_x - ref_min_x + 1U);
        return (red_w / ref_w) - 1.0;
    };

    const double mag_3d = measure_nose_foreshortening(0.0f);
    const double mag_default = measure_nose_foreshortening(0.75f);
    const double mag_25d = measure_nose_foreshortening(1.0f);
    std::printf("[P4-ZFLAT] mag_3d=%.3f mag_default(0.75)=%.3f mag_full(1.0)=%.3f\n", mag_3d, mag_default, mag_25d);
    CHECK(mag_3d > 0.40);
    // z_flatten_weight = 0.85 compresses close-up foreshortening enlargement >= 70% while keeping occlusion
    CHECK(mag_25d <= mag_3d * 0.30);
    CHECK(mag_default <= mag_3d * 0.50);

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Optical Invariant 2: 0-360 deg SDF face shadow sweep is smooth and symmetric", "[phase4][effects][sdf]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    // Build a symmetric 64x64 linear-ramp face SDF map (tex_desc 0)
    constexpr uint32_t kSdfRes = 64;
    spec.tex_pixels.resize(static_cast<size_t>(kSdfRes) * kSdfRes * 4U);
    for (uint32_t y = 0; y < kSdfRes; ++y) {
        for (uint32_t x = 0; x < kSdfRes; ++x) {
            const uint8_t val = static_cast<uint8_t>(std::clamp(static_cast<int>(x * 255 / (kSdfRes - 1)), 0, 255));
            const size_t i = (static_cast<size_t>(y) * kSdfRes + x) * 4U;
            spec.tex_pixels[i + 0] = val;
            spec.tex_pixels[i + 1] = val;
            spec.tex_pixels[i + 2] = val;
            spec.tex_pixels[i + 3] = 255;
        }
    }
    AetherTextureDesc td{};
    td.pixel_offset = 0;
    td.pixel_size = spec.tex_pixels.size();
    td.width = kSdfRes;
    td.height = kSdfRes;
    td.format = AETHER_TEX_FORMAT_RGBA8_UNORM;
    td.usage_flags = AETHER_TEX_USAGE_SDF_FACE;
    spec.tex_descs.push_back(td);

    auto mat = aether::test::make_basic_npr_material(0.95f, 0.85f, 0.80f, 1.0f, AETHER_MAT_FLAG_SDF_FACE);
    mat.shade_color_gi[0] = 0.25f;
    mat.shade_color_gi[1] = 0.22f;
    mat.shade_color_gi[2] = 0.30f;
    mat.tex_sdf_face = 0;
    spec.materials.push_back(mat);

    const float p0[3] = {-0.25f, -0.25f, 0.0f}, p1[3] = {0.25f, -0.25f, 0.0f};
    const float p2[3] = {0.25f, 0.25f, 0.0f}, p3[3] = {-0.25f, 0.25f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {0.0f, 0.0f, 1.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 1.0f, 0.0f, 0, AETHER_SUBMESH_FLAG_OPAQUE);

    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.65f};
    auto mean_center_luma = [&](float azimuth_deg) -> double {
        auto rig = aether::renderer::LightingRig::default_three_point();
        rig.active_count = 1;
        rig.lights[0].orbit_azimuth_deg = azimuth_deg;
        rig.lights[0].orbit_elevation_deg = 0.0f;
        rig.lights[0].intensity = 1.6f;
        rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = 0.0f;
        rig.bg_rgba[3] = 1.0f;
        const auto px = render_synthetic_once(*rhi, spec, 128, 128, cam, rig, false, false, false);
        double sum = 0.0;
        int cnt = 0;
        for (uint32_t y = 44; y < 84; ++y) {
            for (uint32_t x = 44; x < 84; ++x) {
                const size_t i = (static_cast<size_t>(y) * 128 + x) * 4;
                sum += (0.2126 * px[i + 0] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]);
                ++cnt;
            }
        }
        return sum / static_cast<double>(cnt);
    };

    const double l_0 = mean_center_luma(0.0f);
    const double l_p60 = mean_center_luma(60.0f);
    const double l_m60 = mean_center_luma(-60.0f);
    const double l_p120 = mean_center_luma(120.0f);
    const double l_180 = mean_center_luma(180.0f);
    std::printf("[P4-SDF] luma(0)=%.1f l(+60)=%.1f l(-60)=%.1f l(120)=%.1f l(180)=%.1f\n", l_0, l_p60, l_m60, l_p120,
                l_180);
    CHECK(l_0 > l_p60);
    CHECK(l_p60 > l_p120);
    CHECK(l_p120 >= l_180);
    CHECK(l_0 > l_180 + 40.0);
    CHECK(std::abs(l_p60 - l_m60) <= 3.0);

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Optical Invariant 3: Read-only D32_S8 stencil hair drop shadow shifts with light azimuth",
          "[phase4][effects][hair_stencil]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    // Mat 0: Skin shadow receiver; Mat 1: Hair front caster (stencil 0x02)
    spec.materials.push_back(
        aether::test::make_basic_npr_material(0.95f, 0.88f, 0.84f, 1.0f, AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW));
    spec.materials.push_back(
        aether::test::make_basic_npr_material(0.20f, 0.25f, 0.45f, 1.0f, AETHER_MAT_FLAG_HAIR_FRONT_STENCIL));

    const float n[3] = {0.0f, 0.0f, 1.0f}, t[3] = {0.0f, 0.0f, 1.0f};
    {
        const float p0[3] = {-0.30f, -0.30f, 0.0f}, p1[3] = {0.30f, -0.30f, 0.0f};
        const float p2[3] = {0.30f, 0.30f, 0.0f}, p3[3] = {-0.30f, 0.30f, 0.0f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 0.0f, 0.0f, 0, AETHER_SUBMESH_FLAG_OPAQUE);
    }
    {
        // Thin vertical hair lock centered horizontally in front of skin
        const float p0[3] = {-0.03f, -0.25f, 0.04f}, p1[3] = {0.03f, -0.25f, 0.04f};
        const float p2[3] = {0.03f, 0.25f, 0.04f}, p3[3] = {-0.03f, 0.25f, 0.04f};
        aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 0.0f, 0.0f, 1, AETHER_SUBMESH_FLAG_OPAQUE);
    }

    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.65f};
    auto render_azimuth = [&](float az_deg) {
        auto rig = aether::renderer::LightingRig::default_three_point();
        rig.active_count = 1;
        rig.lights[0].orbit_azimuth_deg = az_deg;
        rig.lights[0].orbit_elevation_deg = 0.0f;
        rig.hair_shadow_offset_px = 14.0f;
        rig.hair_shadow_strength = 0.70f;
        return render_synthetic_once(*rhi, spec, 256, 256, cam, rig, false, false, false);
    };

    const auto left_light = render_azimuth(-60.0f);  // Light on left -> drop shadow shifts RIGHT (+X)
    const auto right_light = render_azimuth(+60.0f); // Light on right -> drop shadow shifts LEFT (-X)

    auto sample_band_luma = [&](const std::vector<uint8_t>& px, uint32_t x0, uint32_t x1) -> double {
        double sum = 0.0;
        int cnt = 0;
        for (uint32_t y = 112; y < 144; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t i = (static_cast<size_t>(y) * 256 + x) * 4;
                sum += px[i + 0] * 0.2126 + px[i + 1] * 0.7152 + px[i + 2] * 0.0722;
                ++cnt;
            }
        }
        return sum / static_cast<double>(cnt);
    };

    // Hair lock is near x=116..140; check right adjacent skin (144..152) vs left adjacent skin (104..112)
    const double ll_left_skin = sample_band_luma(left_light, 102, 110);
    const double ll_right_skin = sample_band_luma(left_light, 146, 154);
    const double rl_left_skin = sample_band_luma(right_light, 102, 110);
    const double rl_right_skin = sample_band_luma(right_light, 146, 154);

    std::printf("[P4-HAIRSTENCIL] az=-60 L=%.1f R=%.1f | az=+60 L=%.1f R=%.1f\n", ll_left_skin, ll_right_skin,
                rl_left_skin, rl_right_skin);
    CHECK(ll_right_skin < ll_left_skin - 12.0);
    CHECK(rl_left_skin < rl_right_skin - 12.0);

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Optical Invariant 4: Distance-compensated inverted-hull outline follows w^0.65 across 0.5-3.0 m",
          "[phase4][effects][outline]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    // Closed thin prism box so front faces draw white body and back faces draw bright green outline hull
    auto mat = aether::test::make_basic_npr_material(0.95f, 0.95f, 0.95f, 1.0f,
                                                     AETHER_MAT_FLAG_UNLIT | AETHER_MAT_FLAG_OUTLINE_SCREEN, 4.0f);
    mat.outline_color_mix[0] = 0.05f;
    mat.outline_color_mix[1] = 0.95f;
    mat.outline_color_mix[2] = 0.10f;
    spec.materials.push_back(mat);

    const float hx = 0.14f, hy = 0.14f, hz = 0.02f;
    auto push_face = [&](const float p0[3], const float p1[3], const float p2[3], const float p3[3], const float n[3],
                         const float t0[3], const float t1[3], const float t2[3], const float t3[3]) {
        const uint32_t bv = static_cast<uint32_t>(spec.vertices.size());
        const uint32_t bi = static_cast<uint32_t>(spec.indices.size());
        const float* pts[4] = {p0, p1, p2, p3};
        const float* tans[4] = {t0, t1, t2, t3};
        for (int k = 0; k < 4; ++k) {
            AetherVertex25D v{};
            std::memcpy(v.position, pts[k], sizeof(float) * 3);
            std::memcpy(v.true_normal, n, sizeof(float) * 3);
            std::memcpy(v.proxy_normal, n, sizeof(float) * 3);
            std::memcpy(v.smooth_tangent, tans[k], sizeof(float) * 3);
            v.smooth_tangent[3] = 1.0f;
            v.outline_width_scale = 1.0f;
            spec.vertices.push_back(v);

            AetherVertexSurface s{};
            s.color_rgba8 = 0xFFFFFFFFu;
            spec.surfaces.push_back(s);

            AetherVertexSkin sk{};
            sk.joints[0] = 0;
            sk.weights_unorm16[0] = 65535u;
            spec.skins.push_back(sk);
        }
        const uint32_t idx[6] = {bv, bv + 1, bv + 2, bv, bv + 2, bv + 3};
        spec.indices.insert(spec.indices.end(), idx, idx + 6);
        AetherSubmeshDesc sm{};
        sm.index_offset = bi;
        sm.index_count = 6;
        sm.vertex_offset = bv;
        sm.vertex_count = 4;
        sm.material_index = 0;
        sm.flags = AETHER_SUBMESH_FLAG_OPAQUE;
        spec.submeshes.push_back(sm);
    };

    // Front (+Z) & Back (-Z) with outward diagonal smooth tangents for hull extrusion
    const float f0[3] = {-hx, -hy, hz}, f1[3] = {hx, -hy, hz}, f2[3] = {hx, hy, hz}, f3[3] = {-hx, hy, hz};
    const float b0[3] = {hx, -hy, -hz}, b1[3] = {-hx, -hy, -hz}, b2[3] = {-hx, hy, -hz}, b3[3] = {hx, hy, -hz};
    const float t0[3] = {-0.707f, -0.707f, 0.0f}, t1[3] = {0.707f, -0.707f, 0.0f};
    const float t2[3] = {0.707f, 0.707f, 0.0f}, t3[3] = {-0.707f, 0.707f, 0.0f};
    const float nf[3] = {0, 0, 1}, nb[3] = {0, 0, -1};
    const float nr[3] = {1, 0, 0}, nl[3] = {-1, 0, 0}, nt[3] = {0, 1, 0}, nbot[3] = {0, -1, 0};
    push_face(f0, f1, f2, f3, nf, t0, t1, t2, t3);
    push_face(b0, b1, b2, b3, nb, t1, t0, t3, t2);
    // Close the 4 side faces of the prism box so the front silhouette rim extrudes seamlessly into back-facing hull
    push_face(f1, b0, b3, f2, nr, t1, t1, t2, t2);
    push_face(f2, b3, b2, f3, nt, t2, t2, t3, t3);
    push_face(f3, b2, b1, f0, nl, t3, t3, t0, t0);
    push_face(f0, b1, b0, f1, nbot, t0, t0, t1, t1);

    auto measure_green_border_px = [&](float dist_m) -> double {
        aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, dist_m, 0.65f};
        auto rig = aether::renderer::LightingRig::default_three_point();
        rig.outline_width_scale = 1.5f; // effective stroke = 1.5 * 4.0 = 6.0 nominal
        rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = 0.0f;
        rig.bg_rgba[3] = 1.0f;
        const auto px = render_synthetic_once(*rhi, spec, 512, 512, cam, rig, false, false, false);
        // Count green outline pixels along row y=256 on the right half
        int green_px = 0;
        for (uint32_t x = 256; x < 512; ++x) {
            const size_t i = (static_cast<size_t>(256) * 512 + x) * 4;
            if (px[i + 1] > 140 && px[i + 0] < 90) {
                ++green_px;
            }
        }
        return static_cast<double>(green_px);
    };

    const double w05 = measure_green_border_px(0.5f);
    const double w12 = measure_green_border_px(1.2f);
    const double w30 = measure_green_border_px(3.0f);
    std::printf("[P4-OUTLINE] d=0.5m -> %.1f px | d=1.2m -> %.1f px | d=3.0m -> %.1f px\n", w05, w12, w30);
    CHECK(w05 >= 4.0);
    CHECK(w12 >= 3.0);
    CHECK(w30 >= 2.0);
    // World-space inverse depth would shrink 6.0x (w05/w30 == 6.0); w^0.65 compresses attenuation to <= 2.3x
    CHECK((w05 / w30) < 2.3);

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Optical Invariant 5: Premultiplied glow matches analytical composite within Delta-E <= 1.0",
          "[phase4][effects][bloom]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    // Semi-transparent emissive sprite over transparent canvas -> OUT_BGRA8 (premultiplied)
    auto mat = aether::test::make_basic_npr_material(0.20f, 0.55f, 0.90f, 0.50f, AETHER_MAT_FLAG_UNLIT);
    mat.alpha_mode = AETHER_ALPHA_MODE_BLEND;
    mat.emissive_and_pad[0] = 0.45f;
    mat.emissive_and_pad[1] = 0.80f;
    mat.emissive_and_pad[2] = 1.60f; // above default 1.05 bloom threshold
    spec.materials.push_back(mat);

    const float p0[3] = {-0.22f, -0.22f, 0.0f}, p1[3] = {0.22f, -0.22f, 0.0f};
    const float p2[3] = {0.22f, 0.22f, 0.0f}, p3[3] = {-0.22f, 0.22f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f}, t[3] = {0.0f, 0.0f, 1.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 0.0f, 0.0f, 0, AETHER_SUBMESH_FLAG_TRANSPARENT);

    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.65f};
    auto rig = aether::renderer::LightingRig::default_three_point();
    rig.bg_rgba[0] = rig.bg_rgba[1] = rig.bg_rgba[2] = rig.bg_rgba[3] = 0.0f;

    // 1) Read premultiplied OUT_BGRA8 rendered over transparent black
    const auto premul_px = render_synthetic_once(*rhi, spec, 128, 128, cam, rig, false, false, true);
    const size_t center = (static_cast<size_t>(64) * 128U + 64U) * 4U;
    const uint8_t pr = premul_px[center + 0];
    const uint8_t pg = premul_px[center + 1];
    const uint8_t pb = premul_px[center + 2];
    const uint8_t pa = premul_px[center + 3];
    CHECK(pa > 90);
    CHECK(pb >= 150);

    // 2) Render directly onto solid white vs analytical Porter-Duff Over(premul, #FFFFFF)
    auto rig_white = rig;
    rig_white.bg_rgba[0] = rig_white.bg_rgba[1] = rig_white.bg_rgba[2] = rig_white.bg_rgba[3] = 1.0f;
    const auto direct_white = render_synthetic_once(*rhi, spec, 128, 128, cam, rig_white, false, false, true);

    auto srgb_to_lin = [](uint8_t c) -> float {
        const float u = static_cast<float>(c) / 255.0f;
        return u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
    };
    auto lin_to_srgb8 = [](float x) -> uint8_t {
        const float c = std::clamp(x, 0.0f, 1.0f);
        const float u = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        return static_cast<uint8_t>(std::lround(u * 255.0f));
    };

    const float a = static_cast<float>(pa) / 255.0f;
    const uint8_t comp_white[4] = {
        lin_to_srgb8(srgb_to_lin(pr) + (1.0f - a) * 1.0f),
        lin_to_srgb8(srgb_to_lin(pg) + (1.0f - a) * 1.0f),
        lin_to_srgb8(srgb_to_lin(pb) + (1.0f - a) * 1.0f),
        255,
    };
    const uint8_t gpu_white[4] = {direct_white[center + 0], direct_white[center + 1], direct_white[center + 2], 255};
    const double de_white = aether::test::compute_delta_e76(comp_white, gpu_white);
    std::printf("[P4-BLOOM-PREMUL] premul=(%u,%u,%u,a=%u) DeltaE(white)=%.3f\n", pr, pg, pb, pa, de_white);
    CHECK(de_white <= 1.0);

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("OfflineSdfGenerator bakes monotonic symmetric UV R8 SDF and uploads cleanly", "[phase4][sdf_bake]") {
    std::vector<AetherVertex25D> verts(4);
    std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    // Curved face proxy normals sweeping left (-X,+Z) to right (+X,+Z)
    for (int i = 0; i < 4; ++i) {
        verts[i].position[0] = (i == 1 || i == 2) ? 0.1f : -0.1f;
        verts[i].position[1] = (i >= 2) ? 0.1f : -0.1f;
        verts[i].proxy_normal[0] = (i == 1 || i == 2) ? 0.707f : -0.707f;
        verts[i].proxy_normal[2] = 0.707f;
        verts[i].sdf_face_mask = 1.0f;
    }

    aether::renderer::SdfBakeInput in{};
    in.resolution = 64;
    in.azimuth_steps = 16;
    in.vertices = verts.data();
    in.vertex_count = 4;
    in.indices = idx.data();
    in.index_count = 6;

    auto out = aether::renderer::OfflineSdfGenerator::bake_face_sdf(in);
    REQUIRE(out.has_value());
    CHECK(out.value().width == 64);
    CHECK(out.value().height == 64);
    CHECK(out.value().r8_pixels.size() == static_cast<size_t>(64) * 64U);
    CHECK(out.value().r8_pixels[32U * 64U + 4U] < out.value().r8_pixels[32U * 64U + 32U]);
    CHECK(out.value().r8_pixels[32U * 64U + 32U] < out.value().r8_pixels[32U * 64U + 59U]);
}

TEST_CASE("NprRenderer::bake_and_apply_face_sdf activates live R8 SDF on unflagged avatar",
          "[phase4][effects][sdf_apply]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    auto mat = aether::test::make_basic_npr_material(0.95f, 0.85f, 0.80f, 1.0f, 0U);
    mat.shade_color_gi[0] = 0.25f;
    mat.shade_color_gi[1] = 0.22f;
    mat.shade_color_gi[2] = 0.30f;
    spec.materials.push_back(mat);

    const float p0[3] = {-0.25f, -0.25f, 0.0f}, p1[3] = {0.25f, -0.25f, 0.0f};
    const float p2[3] = {0.25f, 0.25f, 0.0f}, p3[3] = {-0.25f, 0.25f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {0.0f, 0.0f, 1.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 1.0f, 1.0f, 0,
                              AETHER_SUBMESH_FLAG_OPAQUE | AETHER_SUBMESH_FLAG_FACE);
    spec.vertices[0].proxy_normal[0] = -0.707f;
    spec.vertices[0].proxy_normal[2] = 0.707f;
    spec.vertices[3].proxy_normal[0] = -0.707f;
    spec.vertices[3].proxy_normal[2] = 0.707f;
    spec.vertices[1].proxy_normal[0] = 0.707f;
    spec.vertices[1].proxy_normal[2] = 0.707f;
    spec.vertices[2].proxy_normal[0] = 0.707f;
    spec.vertices[2].proxy_normal[2] = 0.707f;

    auto avatar_res = aether::test::upload_synthetic_avatar(*rhi, spec);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());
    REQUIRE(avatar->rest_indices.size() == spec.indices.size());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 128;
    rcfg.height = 128;
    rcfg.enable_rt = false;

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    aether::renderer::SdfBakeInput sdf_in{};
    sdf_in.resolution = 64;
    sdf_in.azimuth_steps = 16;
    sdf_in.vertices = avatar->rest_vertices.data();
    sdf_in.vertex_count = static_cast<uint32_t>(avatar->rest_vertices.size());
    sdf_in.indices = avatar->rest_indices.data();
    sdf_in.index_count = static_cast<uint32_t>(avatar->rest_indices.size());
    REQUIRE(renderer.bake_and_apply_face_sdf(sdf_in).has_value());

    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.65f};
    auto render_luma = [&](float az_deg, uint64_t fidx) -> double {
        auto rig = aether::renderer::LightingRig::default_three_point();
        rig.active_count = 1;
        rig.lights[0].orbit_azimuth_deg = az_deg;
        rig.lights[0].orbit_elevation_deg = 0.0f;
        rig.lights[0].intensity = 1.6f;
        rig.bg_rgba[3] = 1.0f;

        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.frame_index = fidx;
        req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
        req.rt_enabled = false;
        req.show_checkerboard = false;
        req.show_orbit_orb = false;
        req.render_csm_shadow = false;
        REQUIRE(renderer.render_headless_frame(req).has_value());

        const auto outs = renderer.graph().outputs();
        auto px = renderer.graph().readback_image_rgba8(outs.viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                        128, 128);
        REQUIRE(px.has_value());
        double sum = 0.0;
        int cnt = 0;
        for (uint32_t y = 44; y < 84; ++y) {
            for (uint32_t x = 44; x < 84; ++x) {
                const size_t i = (static_cast<size_t>(y) * 128 + x) * 4U;
                sum += (0.2126 * px.value()[i + 0] + 0.7152 * px.value()[i + 1] + 0.0722 * px.value()[i + 2]);
                ++cnt;
            }
        }
        return sum / static_cast<double>(cnt);
    };

    const double l_front = render_luma(0.0f, 1);
    const double l_back = render_luma(180.0f, 2);
    CHECK(l_front > l_back + 30.0);
    CHECK(rhi->validation_error_count() == 0);
    CHECK(rhi->validation_warning_count() == 0);

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Vulkan wireframe mode, AvatarRoot 3D transform, and Rigging spring-bone collider overlay",
          "[phase4][effects][wireframe][rigging]") {
    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);

    aether::test::SyntheticMeshSpec spec;
    auto mat = aether::test::make_basic_npr_material(0.95f, 0.90f, 0.85f, 1.0f, 0U);
    spec.materials.push_back(mat);
    const float p0[3] = {-0.35f, -0.35f, 0.0f}, p1[3] = {0.35f, -0.35f, 0.0f};
    const float p2[3] = {0.35f, 0.35f, 0.0f}, p3[3] = {-0.35f, 0.35f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.0f, 1.0f, 1.0f, 0, AETHER_SUBMESH_FLAG_OPAQUE);
    auto avatar_res = aether::test::upload_synthetic_avatar(*rhi, spec);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    aether::renderer::RendererConfig rcfg{};
    rcfg.width = 128;
    rcfg.height = 128;
    rcfg.enable_rt = false;
    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    CHECK(renderer.wireframe_supported() == rhi->fill_mode_non_solid_supported());

    aether::renderer::PreviewCamera cam{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.4f, 0.65f};
    auto rig = aether::renderer::LightingRig::default_three_point();
    rig.bg_rgba[0] = 0.0f;
    rig.bg_rgba[1] = 0.0f;
    rig.bg_rgba[2] = 0.0f;
    rig.bg_rgba[3] = 1.0f;

    auto render_opts = [&](bool wireframe, float pos_x, float rot_y, float scale, bool colliders,
                           const aether::animation::SpringColliderOverlay* cols, uint32_t col_cnt,
                           const aether::animation::SpringSegmentOverlay* segs, uint32_t seg_cnt,
                           uint64_t fidx) -> std::vector<uint8_t> {
        aether::renderer::FrameRenderRequest req{};
        req.avatar = avatar.get();
        req.rig = &rig;
        req.camera = cam;
        req.frame_index = fidx;
        req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
        req.rt_enabled = false;
        req.premul_alpha = false;
        req.non_alpha_bg_rgb = {0.0f, 0.0f, 0.0f};
        req.show_checkerboard = false;
        req.show_orbit_orb = false;
        req.render_csm_shadow = false;
        req.outline_width_scale = 0.0f;
        req.wireframe = wireframe;
        req.show_rig_colliders = colliders;
        req.avatar_pos = {pos_x, 0.0f, 0.0f};
        req.avatar_rot_y_deg = rot_y;
        req.avatar_scale = scale;
        req.spring_colliders = cols;
        req.spring_collider_count = col_cnt;
        req.spring_segments = segs;
        req.spring_segment_count = seg_cnt;
        REQUIRE(renderer.render_headless_frame(req).has_value());
        const auto outs = renderer.graph().outputs();
        auto px = renderer.graph().readback_image_rgba8(outs.viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                        128, 128);
        REQUIRE(px.has_value());
        return std::move(px.value());
    };

    // 1. Solid vs Wireframe mode
    const auto px_solid = render_opts(false, 0.0f, 0.0f, 1.0f, false, nullptr, 0, nullptr, 0, 1);
    const auto px_wire = render_opts(true, 0.0f, 0.0f, 1.0f, false, nullptr, 0, nullptr, 0, 2);
    auto count_lit = [](const std::vector<uint8_t>& px) {
        int cnt = 0;
        for (size_t i = 0; i < static_cast<size_t>(128) * 128U; ++i) {
            const uint8_t r = px[i * 4U + 0U];
            const uint8_t g = px[i * 4U + 1U];
            const uint8_t b = px[i * 4U + 2U];
            if (r > 24U || g > 24U || b > 24U) {
                ++cnt;
            }
        }
        return cnt;
    };
    const int lit_solid = count_lit(px_solid);
    const int lit_wire = count_lit(px_wire);
    INFO("Solid lit px=" << lit_solid << " Wireframe lit px=" << lit_wire);
    CHECK(lit_solid > 1200);
    if (renderer.wireframe_supported()) {
        CHECK(lit_wire > 40);
        CHECK(lit_wire < static_cast<int>(static_cast<double>(lit_solid) * 0.75));
    }

    // 2. AvatarRoot 3D transform shifts rendered quad horizontally
    const auto px_xform = render_opts(false, 0.28f, 30.0f, 1.1f, false, nullptr, 0, nullptr, 0, 3);
    auto x_centroid = [](const std::vector<uint8_t>& px) {
        double wx = 0.0;
        double wsum = 0.0;
        for (uint32_t y = 0; y < 128U; ++y) {
            for (uint32_t x = 0; x < 128U; ++x) {
                const size_t i = (static_cast<size_t>(y) * 128U + x) * 4U;
                const double lum = static_cast<double>(px[i] + px[i + 1] + px[i + 2]);
                if (lum > 40.0) {
                    wx += static_cast<double>(x) * lum;
                    wsum += lum;
                }
            }
        }
        return (wsum > 1.0) ? (wx / wsum) : 64.0;
    };
    const double cx_base = x_centroid(px_solid);
    const double cx_xform = x_centroid(px_xform);
    INFO("Centroid base=" << cx_base << " transformed=" << cx_xform);
    CHECK(cx_xform > cx_base + 12.0);

    // 3. Rigging Spring-Bone collider overlay renders Cyan (rest) and Orange/Red (contact)
    std::array<aether::animation::SpringColliderOverlay, 2> cols{{
        {{-0.20f, 0.0f, 0.0f}, 0.16f, {0.0f, 0.0f, 0.0f}, 0U, 0U, {}},
        {{0.20f, -0.10f, 0.0f}, 0.11f, {0.20f, 0.10f, 0.0f}, 1U, 1U, {}},
    }};

    std::array<aether::animation::SpringSegmentOverlay, 2> segs{{
        {{-0.20f, 0.18f, 0.0f}, 0.015f, {-0.20f, 0.28f, 0.0f}, 0U, {}},
        {{0.20f, 0.12f, 0.0f}, 0.015f, {0.20f, 0.24f, 0.0f}, 1U, {}},
    }};

    const auto px_col = render_opts(false, 0.0f, 0.0f, 1.0f, true, cols.data(), 2, segs.data(), 2, 4);
    int cyan_left = 0;
    int orange_right = 0;
    for (uint32_t y = 0; y < 128U; ++y) {
        for (uint32_t x = 0; x < 128U; ++x) {
            const size_t i = (static_cast<size_t>(y) * 128U + x) * 4U;
            const int r = px_col[i + 0U];
            const int g = px_col[i + 1U];
            const int b = px_col[i + 2U];
            if (x < 64U && b > r + 40 && g > r + 30) {
                ++cyan_left;
            }
            if (x >= 64U && r > b + 40 && r > g + 15) {
                ++orange_right;
            }
        }
    }
    INFO("Cyan rest collider px=" << cyan_left << " Orange contact collider px=" << orange_right);
    CHECK(cyan_left >= 15);
    CHECK(orange_right >= 15);
    CHECK(rhi->validation_error_count() == 0);
    CHECK(rhi->validation_warning_count() == 0);

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Phase 1 studio photometry: GPU priority, Heitz LTC/IES LUTs, 12 fixtures, Modifiers & 4+ light OKLab",
          "[renderer][phase1][studio_lighting]") {
    using namespace aether::renderer;

    // 1. Discrete GPU strictly ranks at Index 0 ahead of integrated GPUs in both Vulkan & DX12
    {
        auto vk_adapters = aether::rhi::RhiDevice::enumerate_vulkan_devices();
        REQUIRE(vk_adapters.has_value());
        bool has_vk_disc = false;
        for (const auto& a : vk_adapters.value()) {
            if (a.device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                has_vk_disc = true;
            }
        }
        if (has_vk_disc) {
            CHECK(vk_adapters.value()[0].device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        }

        auto dx_adapters = aether::rhi::D3D12Device::enumerate_adapters();
        REQUIRE(dx_adapters.has_value());
        bool has_dx_disc = false;
        for (const auto& d : dx_adapters.value()) {
            if (d.is_discrete) {
                has_dx_disc = true;
            }
        }
        if (has_dx_disc) {
            CHECK(dx_adapters.value()[0].is_discrete);
        }
    }

    // 2. CPU precomputed 64x64 Heitz LTC LUTs, 256x64 IES LM-63-19 LUT, & all 12 fixture profiles
    {
        std::array<float, kLtcLutSize * kLtcLutSize * 4> inv_m{};
        std::array<float, kLtcLutSize * kLtcLutSize * 4> amp{};
        generate_heitz_ltc_luts(inv_m, amp);
        CHECK(std::isfinite(inv_m[0]));
        CHECK(inv_m[0] > 0.0f);
        CHECK(amp[0] > 0.3f);
        CHECK(amp[0] <= 1.0f);

        std::array<float, kIesLutWidth * kIesLutRows> ies{};
        generate_ies_lm63_lut(ies);
        // Spot/Dedolight beam profile row 5 must peak on axis and attenuate strongly off axis
        CHECK(ies[5u * kIesLutWidth + 0u] > 0.9f);
        CHECK(ies[5u * kIesLutWidth + 128u] < 0.05f);

        LightingRig rig = LightingRig::default_three_point();
        rig.use_studio_lights = true;
        rig.studio_light_count = 12;
        for (uint32_t f = 0; f < 12u; ++f) {
            configure_fixture_defaults(rig.studio_lights[f], static_cast<StudioFixtureProfile>(f));
            rig.studio_lights[f].enabled = true;
        }
        std::array<StudioLightGPU, kMaxStudioLights> packed{};
        const uint32_t cnt = pack_studio_lights_gpu(rig, {0.0f, 1.35f, 0.0f}, {0.0f, 1.35f, 2.0f}, packed);
        CHECK(cnt == 12u);
        for (uint32_t f = 0; f < 12u; ++f) {
            CHECK((packed[f].flags_and_type & 0x0Fu) == f);
        }
    }

    // 3. Quantitative Photometric Modifier & 4+ Light OKLab Equilibrium Rendering Verification
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);
    if (!rhi) {
        aether_runtime_shutdown();
        SUCCEED("Skipped GPU render portion: Vulkan unavailable in environment");
        return;
    }

    // Build wide quad torso/face proxy panel spanning [-0.9..+0.9] X and [-0.7..+0.7] Y
    aether::test::SyntheticMeshSpec spec{};
    spec.materials.push_back(aether::test::make_basic_npr_material(0.92f, 0.88f, 0.86f));
    spec.materials[0].toon_params[0] = 0.0f; // smooth shading transition across modifiers
    const float p0[3] = {-0.90f, -0.70f, 0.0f};
    const float p1[3] = {0.90f, -0.70f, 0.0f};
    const float p2[3] = {0.90f, 0.70f, 0.0f};
    const float p3[3] = {-0.90f, 0.70f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.92f, 0.88f, 0.86f, 0, AETHER_SUBMESH_FLAG_OPAQUE);

    PreviewCamera cam{};
    cam.target_x = 0.0f;
    cam.target_y = 0.0f;
    cam.target_z = 0.0f;
    cam.distance = 2.0f;
    cam.fovy_rad = 0.75f;

    auto region_luma = [](const std::vector<uint8_t>& px, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        double sum = 0.0;
        uint64_t cnt = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t i = (static_cast<size_t>(y) * 128u + x) * 4u;
                sum += 0.2126 * px[i + 0] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
                ++cnt;
            }
        }
        return (cnt > 0u) ? (sum / static_cast<double>(cnt)) : 0.0;
    };

    // (a) Softbox + Honeycomb Grid (30 deg) measurably attenuates peripheral edges relative to ungridded softbox
    LightingRig rig_open = LightingRig::default_three_point();
    rig_open.use_studio_lights = true;
    rig_open.studio_light_count = 1;
    configure_fixture_defaults(rig_open.studio_lights[0], StudioFixtureProfile::RectSoftbox);
    rig_open.studio_lights[0].use_orbit_angles = false;
    rig_open.studio_lights[0].position_ws = {0.0f, 0.0f, 1.3f};
    rig_open.studio_lights[0].target_ws = {0.0f, 0.0f, 0.0f};
    rig_open.studio_lights[0].aim_lock = false;
    rig_open.studio_lights[0].intensity_ev100 = 1.25f;
    rig_open.studio_lights[0].grid_enabled = false;
    const auto px_open = render_synthetic_once(*rhi, spec, 128u, 128u, cam, rig_open);

    LightingRig rig_grid = rig_open;
    rig_grid.studio_lights[0].grid_enabled = true;
    rig_grid.studio_lights[0].grid_angle_deg = 30.0f;
    const auto px_grid = render_synthetic_once(*rhi, spec, 128u, 128u, cam, rig_grid);

    const double edge_open = region_luma(px_open, 12u, 48u, 28u, 80u);
    const double edge_grid = region_luma(px_grid, 12u, 48u, 28u, 80u);
    INFO("Honeycomb Grid 30 deg lateral edge luma: open=" << edge_open << " grid=" << edge_grid);
    CHECK(edge_grid < edge_open - 4.0);

    // (b) 4-Leaf Barn Doors bottom cut measurably darkens lower torso band while leaving upper center lit
    LightingRig rig_barn = rig_open;
    rig_barn.studio_lights[0].barn_doors_enabled = true;
    rig_barn.studio_lights[0].barn_door_cuts_deg = {45.0f, 6.0f, 45.0f, 45.0f};
    const auto px_barn = render_synthetic_once(*rhi, spec, 128u, 128u, cam, rig_barn);
    const double bot_open = region_luma(px_open, 48u, 96u, 80u, 114u);
    const double bot_barn = region_luma(px_barn, 48u, 96u, 80u, 114u);
    INFO("Barn Doors bottom cut luma: open=" << bot_open << " cut=" << bot_barn);
    CHECK(bot_barn < bot_open - 6.0);

    // (c) 4+ simultaneous studio lights with soft-knee compression + OKLab accent color preservation
    LightingRig rig_4l = rig_open;
    rig_4l.studio_light_count = 4;
    rig_4l.studio_lights[0].intensity_ev100 = 1.6f; // bright white key
    rig_4l.studio_lights[1] = rig_open.studio_lights[0];
    configure_fixture_defaults(rig_4l.studio_lights[1], StudioFixtureProfile::LanternChinaBall);
    rig_4l.studio_lights[1].use_orbit_angles = false;
    rig_4l.studio_lights[1].position_ws = {-0.6f, 0.2f, 1.5f};
    rig_4l.studio_lights[1].intensity_ev100 = 0.7f;
    rig_4l.studio_lights[2] = rig_open.studio_lights[0];
    configure_fixture_defaults(rig_4l.studio_lights[2], StudioFixtureProfile::LedTubeAstera);
    rig_4l.studio_lights[2].use_orbit_angles = false;
    rig_4l.studio_lights[2].position_ws = {-1.1f, 0.0f, 0.6f};
    rig_4l.studio_lights[2].target_ws = {-0.5f, 0.0f, 0.0f};
    rig_4l.studio_lights[2].aim_lock = false;
    rig_4l.studio_lights[2].gel_enabled = true;
    rig_4l.studio_lights[2].gel = StudioGelPreset::CyberCyan;
    rig_4l.studio_lights[2].intensity_ev100 = 1.1f;
    rig_4l.studio_lights[3] = rig_open.studio_lights[2];
    rig_4l.studio_lights[3].position_ws = {1.1f, 0.0f, 0.6f};
    rig_4l.studio_lights[3].target_ws = {0.5f, 0.0f, 0.0f};
    rig_4l.studio_lights[3].gel = StudioGelPreset::StageMagenta;
    rig_4l.studio_lights[3].intensity_ev100 = 1.1f;
    const auto px_4l = render_synthetic_once(*rhi, spec, 128u, 128u, cam, rig_4l);

    int max_clipped = 0;
    double left_b_minus_r = 0.0;
    double right_r_minus_g = 0.0;
    for (uint32_t y = 40u; y < 88u; ++y) {
        for (uint32_t x = 24u; x < 104u; ++x) {
            const size_t i = (static_cast<size_t>(y) * 128u + x) * 4u;
            if (px_4l[i] >= 254 && px_4l[i + 1] >= 254 && px_4l[i + 2] >= 254) {
                ++max_clipped;
            }
            if (x < 48u) {
                left_b_minus_r += static_cast<double>(px_4l[i + 2]) - static_cast<double>(px_4l[i + 0]);
            }
            if (x >= 80u) {
                right_r_minus_g += static_cast<double>(px_4l[i + 0]) - static_cast<double>(px_4l[i + 1]);
            }
        }
    }
    INFO("4-Light Equilibrium clipped=" << max_clipped << " left(B-R)=" << left_b_minus_r
                                        << " right(R-G)=" << right_r_minus_g);
    CHECK(max_clipped < 200);       // Soft-knee prevents blown-out white center
    CHECK(left_b_minus_r > 150.0);  // CyberCyan accent hue preserved on left
    CHECK(right_r_minus_g > 150.0); // StageMagenta accent hue preserved on right

    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Phase 2 Vulkan Dual-Viewport Zero Leakage, 6 Ortho Cameras, Parallel Picking & Gizmo Scale",
          "[renderer][phase2][lighting_stage]") {
    using namespace aether::renderer;

    // 1. Validate all 7 Stage Camera presets, Persp/Ortho inv_proj*proj identity, ray-picking & constant-px gizmos
    {
        const std::array<float, 3> pivot{0.0f, 1.32f, 0.0f};
        for (int vi = 0; vi <= 6; ++vi) {
            const auto v = static_cast<StageCameraView>(vi);
            const PreviewCamera c = make_stage_camera_preset(v, pivot, 2.40f);
            GpuMat4 view{}, inv_view{}, proj{}, inv_proj{}, vp{};
            detail::Mat4Vec3 eye{};
            detail::build_camera_matrices(c, 16.0f / 9.0f, view, inv_view, proj, inv_proj, vp, eye);
            const GpuMat4 prod = detail::mul_mat4(proj, inv_proj);
            INFO("vi=" << vi << " ortho=" << c.is_orthographic << " col2=(" << prod.col2[0] << "," << prod.col2[1]
                       << "," << prod.col2[2] << "," << prod.col2[3] << ") col3=(" << prod.col3[0] << ","
                       << prod.col3[1] << "," << prod.col3[2] << "," << prod.col3[3] << ")");
            CHECK(std::fabs(prod.col0[0] - 1.0f) < 1e-4f);
            CHECK(std::fabs(prod.col1[1] - 1.0f) < 1e-4f);
            CHECK(std::fabs(prod.col2[2] - 1.0f) < 1e-4f);
            CHECK(std::fabs(prod.col3[3] - 1.0f) < 1e-4f);
            CHECK(std::fabs(prod.col3[2]) < 1e-4f);
            CHECK(std::fabs(prod.col2[3]) < 1e-4f);
            CHECK(c.is_orthographic == (vi > 0));
        }

        // Parallel Ortho ray picking vs Perspective ray picking on placed Softboxes
        LightingRig pick_rig = LightingRig::default_three_point();
        pick_rig.use_studio_lights = true;
        pick_rig.studio_light_count = 2;
        configure_fixture_defaults(pick_rig.studio_lights[0], StudioFixtureProfile::RectSoftbox);
        pick_rig.studio_lights[0].use_orbit_angles = false;
        pick_rig.studio_lights[0].position_ws = {-0.8f, 1.5f, 0.6f};
        pick_rig.studio_lights[0].width_or_outer_m = 0.8f;
        configure_fixture_defaults(pick_rig.studio_lights[1], StudioFixtureProfile::Octabox);
        pick_rig.studio_lights[1].use_orbit_angles = false;
        pick_rig.studio_lights[1].position_ws = {0.9f, 1.4f, -0.5f};
        pick_rig.studio_lights[1].width_or_outer_m = 0.9f;

        const PreviewCamera top_cam = make_stage_camera_preset(StageCameraView::OrthoTop, pivot, 3.0f);
        const StagePickRay r0 = compute_stage_pick_ray(top_cam, 0.5f, 0.5f, 1.0f);
        const StagePickRay r1 = compute_stage_pick_ray(top_cam, 0.25f, 0.75f, 1.0f);
        CHECK(r0.is_orthographic);
        // Parallel rays in Orthographic share identical world direction!
        CHECK(std::fabs(r0.dir_ws[0] - r1.dir_ws[0]) < 1e-5f);
        CHECK(std::fabs(r0.dir_ws[1] - r1.dir_ws[1]) < 1e-5f);
        CHECK(std::fabs(r0.dir_ws[2] - r1.dir_ws[2]) < 1e-5f);

        // Compute normalized screen coord of studio_lights[1] under OrthoTop & verify pick hits index 1!
        const auto m_top = compute_view_proj(top_cam, 1.0f);
        const auto& p1 = pick_rig.studio_lights[1].position_ws;
        const float ndc_x = m_top[0] * p1[0] + m_top[4] * p1[1] + m_top[8] * p1[2] + m_top[12];
        const float ndc_y = m_top[1] * p1[0] + m_top[5] * p1[1] + m_top[9] * p1[2] + m_top[13];
        const float u01 = ndc_x * 0.5f + 0.5f;
        const float v01 = 0.5f - ndc_y * 0.5f;
        CHECK(pick_stage_studio_light(pick_rig, pivot, {0.0f, 1.4f, 2.0f}, top_cam, u01, v01, 1.0f) == 1);

        const PreviewCamera persp_cam = make_stage_camera_preset(StageCameraView::FreePerspective, pivot);
        const float g_near = stage_gizmo_world_scale(persp_cam, {0.0f, 1.32f, 1.5f}, 72.0f, 720.0f);
        const float g_far = stage_gizmo_world_scale(persp_cam, {0.0f, 1.32f, -1.5f}, 72.0f, 720.0f);
        CHECK(g_far > g_near); // Constant px size in perspective grows with distance!
        const float g_o1 = stage_gizmo_world_scale(top_cam, {-1.0f, 1.3f, 0.0f}, 72.0f, 720.0f);
        const float g_o2 = stage_gizmo_world_scale(top_cam, {1.0f, 0.1f, 0.5f}, 72.0f, 720.0f);
        CHECK(std::fabs(g_o1 - g_o2) < 1e-5f); // Ortho constant-px scale is position invariant!
    }

    // 2. Vulkan Dual-Viewport Zero Schematic Leakage + Quad Targets + Clay/FalseColor/TemporalBank Isolation
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    auto rhi = create_test_rhi(false);
    if (!rhi) {
        aether_runtime_shutdown();
        SUCCEED("Skipped Vulkan Phase 2 GPU test: Vulkan unavailable");
        return;
    }

    aether::test::SyntheticMeshSpec spec{};
    auto mat = aether::test::make_basic_npr_material(0.20f, 0.55f, 0.92f); // Saturated cobalt blue cloth
    mat.toon_params[0] = 0.0f;
    spec.materials.push_back(mat);
    const float p0[3] = {-0.55f, 0.70f, 0.0f};
    const float p1[3] = {0.55f, 0.70f, 0.0f};
    const float p2[3] = {0.55f, 1.85f, 0.0f};
    const float p3[3] = {-0.55f, 1.85f, 0.0f};
    const float n[3] = {0.0f, 0.0f, 1.0f};
    const float t[3] = {1.0f, 0.0f, 0.0f};
    aether::test::append_quad(spec, p0, p1, p2, p3, n, t, 0.20f, 0.55f, 0.92f, 0, AETHER_SUBMESH_FLAG_OPAQUE);
    auto av_res = aether::test::upload_synthetic_avatar(*rhi, spec);
    REQUIRE(av_res.has_value());
    auto avatar = std::move(av_res.value());

    constexpr uint32_t kW = 256u;
    constexpr uint32_t kH = 256u;
    RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();
    NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    LightingRig rig = LightingRig::default_three_point();
    rig.use_studio_lights = true;
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

    PreviewCamera bcast_cam = make_stage_camera_preset(StageCameraView::FreePerspective, {0.0f, 1.30f, 0.0f});
    bcast_cam.yaw_rad = 0.0f;
    bcast_cam.pitch_rad = 0.0f;
    bcast_cam.distance = 2.4f;

    auto render_vk = [&](RenderViewKind kind, const PreviewCamera& cam, StageDiagnosticMode dm, bool req_schem,
                         bool skip_skin, uint8_t mode, uint64_t fidx, std::vector<uint8_t>& out_vp,
                         std::vector<uint8_t>& out_bgra) {
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
        req.skip_skin_and_blas = skip_skin;
        req.lighting_mode = mode;
        req.rt_enabled = (mode > 0u) && rcfg.enable_rt;
        req.show_checkerboard = false;
        req.show_orbit_orb = false;
        req.render_csm_shadow = false;
        req.frame_index = fidx;
        req.completed_frame_index = (fidx >= 2u) ? (fidx - 1u) : 0u;
        req.frame_slot = static_cast<uint32_t>(fidx % ::aether::rhi::RhiSwapchain::kFramesInFlight);
        REQUIRE(renderer.render_headless_frame(req).has_value());
        const auto outs = renderer.graph().outputs();
        const VkImage target_vp = (kind == RenderViewKind::LightingStage) ? outs.stage_viewport_image : outs.viewport_image;
        auto vp_res = renderer.graph().readback_image_rgba8(target_vp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW, kH);
        auto bg_res = renderer.graph().readback_image_rgba8(outs.bgra8_image, VK_IMAGE_LAYOUT_GENERAL, kW, kH);
        REQUIRE(vp_res.has_value());
        REQUIRE(bg_res.has_value());
        out_vp = std::move(vp_res.value());
        out_bgra = std::move(bg_res.value());
        REQUIRE(rhi->validation_error_count() == 0);
        REQUIRE(rhi->validation_warning_count() == 0);
    };

    std::vector<uint8_t> bcast_vp_0, bcast_bg_0;
    std::vector<uint8_t> bcast_adv_vp, bcast_adv_bg;
    std::vector<uint8_t> stage_vp, stage_bg;
    std::vector<uint8_t> clay_vp, clay_bg;
    std::vector<uint8_t> fc_vp, fc_bg;
    std::vector<uint8_t> bcast_after_vp, bcast_after_bg;

    // Warmup 1 Broadcast frame so history_[0] is valid & identical across baseline / adversarial / post-stage checks
    render_vk(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, false, false, 0u, 1u, bcast_vp_0,
              bcast_bg_0);

    // 1. Broadcast clean baseline (mode 0 for bit-exact determinism across frames)
    render_vk(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, false, false, 0u, 1u, bcast_vp_0,
              bcast_bg_0);

    // 2. Adversarial Broadcast with show_stage_schematic=true -> MUST NOT leak into viewport OR bgra8!
    render_vk(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, true, false, 0u, 1u, bcast_adv_vp,
              bcast_adv_bg);
    CHECK(std::memcmp(bcast_vp_0.data(), bcast_adv_vp.data(), bcast_vp_0.size()) == 0);
    CHECK(std::memcmp(bcast_bg_0.data(), bcast_adv_bg.data(), bcast_bg_0.size()) == 0);

    // 3. Stage View (with RT enabled) renders schematics cleanly into stage_viewport_image while bgra8_image remains untouched Broadcast!
    render_vk(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::Lit, true, true, 1u, 1u, stage_vp,
              stage_bg);
    CHECK(std::memcmp(bcast_bg_0.data(), stage_bg.data(), bcast_bg_0.size()) == 0);

    uint32_t vk_diff_px = 0u;
    for (size_t i = 0; i < stage_vp.size(); i += 4u) {
        if (std::abs(int(stage_vp[i + 0]) - int(bcast_vp_0[i + 0])) > 12 ||
            std::abs(int(stage_vp[i + 1]) - int(bcast_vp_0[i + 1])) > 12 ||
            std::abs(int(stage_vp[i + 2]) - int(bcast_vp_0[i + 2])) > 12) {
            ++vk_diff_px;
        }
    }
    INFO("Vulkan Stage schematic diff pixels vs Broadcast: " << vk_diff_px);
    CHECK(vk_diff_px > 600u);

    // 4. Clay 18% Gray & False Color EL-Zone
    render_vk(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::Clay, false, true, 1u, 2u, clay_vp,
              clay_bg);
    render_vk(RenderViewKind::LightingStage, bcast_cam, StageDiagnosticMode::FalseColor, false, true, 1u, 3u, fc_vp,
              fc_bg);
    const size_t ci = (static_cast<size_t>(kH / 2u) * kW + (kW / 2u)) * 4u;
    const int vk_lit_chroma = std::abs(static_cast<int>(bcast_vp_0[ci + 2]) - static_cast<int>(bcast_vp_0[ci + 0]));
    const int vk_clay_chroma = std::abs(static_cast<int>(clay_vp[ci + 2]) - static_cast<int>(clay_vp[ci + 0]));
    CHECK(vk_lit_chroma > 60);
    CHECK(vk_clay_chroma <= 35);
    CHECK(vk_clay_chroma * 3 < vk_lit_chroma);
    CHECK(std::memcmp(fc_vp.data(), clay_vp.data(), fc_vp.size()) != 0);

    // 5. After Stage renders, Broadcast remains 100% invariant (zero bleed)
    render_vk(RenderViewKind::Broadcast, bcast_cam, StageDiagnosticMode::Lit, false, false, 0u, 1u, bcast_after_vp,
              bcast_after_bg);
    CHECK(std::memcmp(bcast_vp_0.data(), bcast_after_vp.data(), bcast_vp_0.size()) == 0);

    renderer.destroy();
    avatar->destroy();
    rhi->destroy();
    aether_runtime_shutdown();
}
