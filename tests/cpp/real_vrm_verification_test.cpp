#include "aether_ffi_contract.h"
#include "animation/hand_collision.hpp"
#include "animation/sim_engine.hpp"
#include "animation/spring_bone.hpp"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/avatar_rt_classifier.hpp"
#include "renderer/circadian_lighting.hpp"
#include "renderer/frame_globals_builder.hpp"
#include "renderer/npr_renderer.hpp"
#include "renderer/preview_pass.hpp"
#include "rhi/rhi_device.hpp"
#include "test_png_io.hpp"
#include "tracking/blink_gaze_filter.hpp"
#include "tracking/camera_source.hpp"
#include "tracking/canonical_face_model.hpp"
#include "tracking/landmark_solver.hpp"
#include "tracking/wasapi_audio_lipsync.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

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

struct ImageLumaStats {
    double mean_luma{0.0};
    double p95_luma{0.0};
    uint32_t white_clipped_pixels{0};
    uint32_t fg_pixels{0};
};

ImageLumaStats analyze_rgba8(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    ImageLumaStats st{};
    if (rgba.size() < 4U) {
        return st;
    }
    const double bg_r = rgba[0];
    const double bg_g = rgba[1];
    const double bg_b = rgba[2];
    std::vector<double> fg_lumas;
    fg_lumas.reserve(static_cast<size_t>(w) * h / 2U);
    double sum = 0.0;
    for (size_t i = 0; i + 3U < rgba.size(); i += 4U) {
        const double r = rgba[i + 0];
        const double g = rgba[i + 1];
        const double b = rgba[i + 2];
        const bool is_bg = (std::abs(r - bg_r) <= 4.0) && (std::abs(g - bg_g) <= 4.0) && (std::abs(b - bg_b) <= 4.0);
        const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (!is_bg) {
            fg_lumas.push_back(l);
            sum += l;
            if (r >= 248.0 && g >= 248.0 && b >= 248.0) {
                ++st.white_clipped_pixels;
            }
        }
    }
    st.fg_pixels = static_cast<uint32_t>(fg_lumas.size());
    if (!fg_lumas.empty()) {
        st.mean_luma = sum / static_cast<double>(fg_lumas.size());
        std::sort(fg_lumas.begin(), fg_lumas.end());
        st.p95_luma = fg_lumas[(fg_lumas.size() * 95U) / 100U];
    }
    return st;
}

aether::animation::RestSkeletonMetrics build_vrm_rest_metrics(const aether::renderer::GpuAvatar& avatar) {
    return aether::animation::build_rest_metrics_from_avatar_streams(
        avatar.desc, avatar.joints.data(), avatar.joints.size(), avatar.joint_rest_world_pos.data(),
        avatar.joint_rest_world_pos.size(), avatar.colliders.data(), avatar.colliders.size(), avatar.springs.data(),
        avatar.springs.size(), avatar.has_split_brow_morphs);
}

double mat3x4_delta_angle_deg(const float m[12]) noexcept {
    const double sx = static_cast<double>(m[9]) - static_cast<double>(m[6]);
    const double sy = static_cast<double>(m[2]) - static_cast<double>(m[8]);
    const double sz = static_cast<double>(m[4]) - static_cast<double>(m[1]);
    const double sin_theta = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    const double cos_theta =
        0.5 * ((static_cast<double>(m[0]) + static_cast<double>(m[5]) + static_cast<double>(m[10])) - 1.0);
    return std::atan2(sin_theta, cos_theta) * static_cast<double>(aether::animation::kRadToDeg);
}

aether::animation::Vec3 transform_point_3x4(const float m[12], const aether::animation::Vec3& p) noexcept {
    return {
        m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
        m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
        m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11],
    };
}

} // namespace

TEST_CASE("Real VRM 8590256991748008892.vrm headless snapshots and shading diagnostics", "[vrm][snapshot]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = true;
    cfg.require_validation = false;
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();

    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    INFO("AssetJob last_error: " << job.last_error());
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    std::printf("[REAL-VRM] loaded '%s': verts=%u tris=%u mats=%zu tex=%zu springs=%zu colliders=%zu\n",
                vrm_path.string().c_str(), avatar->requirements.vertex_count, avatar->requirements.index_count / 3U,
                avatar->materials.size(), avatar->textures.size(), avatar->springs.size(), avatar->colliders.size());
    for (size_t i = 0; i < avatar->materials.size(); ++i) {
        const auto& m = avatar->materials[i];
        std::printf("  mat[%zu]: base=(%.2f,%.2f,%.2f,%.2f) shade=(%.2f,%.2f,%.2f) emissive=(%.2f,%.2f,%.2f) "
                    "shift=%.2f toony=%.2f tex=(base=%u,sh=%u,em=%u)\n",
                    i, static_cast<double>(m.base_color[0]), static_cast<double>(m.base_color[1]),
                    static_cast<double>(m.base_color[2]), static_cast<double>(m.base_color[3]),
                    static_cast<double>(m.shade_color_gi[0]), static_cast<double>(m.shade_color_gi[1]),
                    static_cast<double>(m.shade_color_gi[2]), static_cast<double>(m.emissive_and_pad[0]),
                    static_cast<double>(m.emissive_and_pad[1]), static_cast<double>(m.emissive_and_pad[2]),
                    static_cast<double>(m.toon_params[0]), static_cast<double>(m.toon_params[1]), m.tex_base,
                    m.tex_shade, m.tex_emissive);
    }

    constexpr uint32_t kW = 1280;
    constexpr uint32_t kH = 720;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();

    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    const auto fit_cam = aether::renderer::fit_camera_to_avatar(*avatar);
    const std::string env_prefix = read_env_var("AETHER_VRM_SNAPSHOT_PREFIX");
    const std::string prefix = env_prefix.empty() ? std::string("after_8590256991748008892") : env_prefix;

    const std::filesystem::path snap_dir(AETHER_VRM_SNAPSHOT_DIR);
    std::filesystem::create_directories(snap_dir);

    // Evaluate neutral tracking (P=0, Y=0, R=0, T=(0,1.48,0), untracked hands) across 60 frames at 60 Hz
    // and upload the resulting upright A-pose to both GPU pose slots before capturing headless snapshots.
    const aether::animation::RestSkeletonMetrics rm = build_vrm_rest_metrics(*avatar);
    std::array<aether::animation::Vec3, AETHER_HUMANOID_BONE_COUNT> rest_ws{};
    rest_ws[AETHER_BONE_HIPS] = rm.rest_local_positions[AETHER_BONE_HIPS];
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rest_ws[b] = rest_ws[pi] + rm.rest_local_positions[b];
    }

    aether::animation::SimEngine engine;
    engine.reset(rm);
    aether::animation::SimEngineConfig sim_cfg{};
    sim_cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    aether::animation::SkinningFrame skin{};
    uint64_t sim_t_us = 1000000ULL;

    // Drive the pose through the live default node graph (NPU Pose -> Visibility-First IK -> Output Pose), exactly
    // like the Studio T2 thread, with the screenshot's near-neutral HUD reading `LOCKED P+0.6 Y-0.8 R-0.4`.
    AetherHostConfig hcfg{};
    hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
    hcfg.worker_threads = 2;
    AetherHostHandle graph_host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &graph_host) == AETHER_OK);
    const aether::animation::Quat q_hud =
        aether::animation::from_euler_xyz(0.6f * aether::animation::kDegToRad, -0.8f * aether::animation::kDegToRad,
                                          -0.4f * aether::animation::kDegToRad);
    for (int f = 0; f < 60; ++f) {
        AetherTrackingFrame tf{};
        tf.frame_sequence = static_cast<uint32_t>(f + 1);
        tf.capture_timestamp_us = sim_t_us;
        tf.inference_complete_us = sim_t_us;
        tf.valid_flags = 0x01U | 0x08U;
        tf.upper_body_confidence = 0.95f;
        tf.left_hand_confidence = 0.0f;
        tf.right_hand_confidence = 0.0f;
        tf.lower_body_confidence = 0.0f;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            tf.joint_rotations_quat[b][3] = 1.0f;
            tf.joint_positions_ws[b][3] = 0.0f;
        }
        tf.joint_positions_ws[AETHER_BONE_HIPS][0] = rm.rest_local_positions[AETHER_BONE_HIPS].x;
        tf.joint_positions_ws[AETHER_BONE_HIPS][1] = rm.rest_local_positions[AETHER_BONE_HIPS].y;
        tf.joint_positions_ws[AETHER_BONE_HIPS][2] = rm.rest_local_positions[AETHER_BONE_HIPS].z;
        tf.joint_positions_ws[AETHER_BONE_HIPS][3] = 0.95f;
        tf.joint_positions_ws[AETHER_BONE_SPINE][3] = 0.95f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][0] = 0.0f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.48f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][2] = 0.0f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.98f;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][0] = q_hud.x;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][1] = q_hud.y;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][2] = q_hud.z;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = q_hud.w;
        AetherGraphInputs gin{};
        gin.tracking = &tf;
        gin.now_us = sim_t_us;
        gin.dt_s = 1.0f / 60.0f;
        gin.tick_index = static_cast<uint32_t>(f);
        AetherGraphOutputs graph_out{};
        aether_set_thread_role(AETHER_THREAD_ROLE_T2);
        REQUIRE(aether_host_evaluate_active(graph_host, &gin, &graph_out) == AETHER_OK);
        aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
        engine.tick(tf, dsp, graph_out, sim_t_us, 0.0f, sim_cfg, skin);
        sim_t_us += 16667ULL;
    }
    aether_host_destroy(graph_host);

    const double head_deg = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_HEAD]);
    const double neck_deg = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_NECK]);
    const double chest_deg = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_CHEST]);
    const double spine_deg = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_SPINE]);
    const auto l_sh_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_LEFT_SHOULDER], rest_ws[AETHER_BONE_LEFT_SHOULDER]);
    const auto r_sh_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_RIGHT_SHOULDER], rest_ws[AETHER_BONE_RIGHT_SHOULDER]);
    const auto l_ua_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_LEFT_UPPER_ARM], rest_ws[AETHER_BONE_LEFT_UPPER_ARM]);
    const auto l_la_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_LEFT_LOWER_ARM], rest_ws[AETHER_BONE_LEFT_LOWER_ARM]);
    const auto r_ua_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_RIGHT_UPPER_ARM], rest_ws[AETHER_BONE_RIGHT_UPPER_ARM]);
    const auto r_la_ws =
        transform_point_3x4(skin.bone_matrices[AETHER_BONE_RIGHT_LOWER_ARM], rest_ws[AETHER_BONE_RIGHT_LOWER_ARM]);
    const auto l_dir = aether::animation::normalize_safe(l_la_ws - l_ua_ws);
    const auto r_dir = aether::animation::normalize_safe(r_la_ws - r_ua_ws);
    const double l_drop_deg = std::asin(std::clamp(static_cast<double>(-l_dir.y), -1.0, 1.0)) *
                              static_cast<double>(aether::animation::kRadToDeg);
    const double r_drop_deg = std::asin(std::clamp(static_cast<double>(-r_dir.y), -1.0, 1.0)) *
                              static_cast<double>(aether::animation::kRadToDeg);

    std::printf("[NEUTRAL-POSE] delta_deg(head=%.3f neck=%.3f chest=%.3f spine=%.3f) sh_dy=%.4f arm_drop=(L=%.2f, "
                "R=%.2f deg)\n",
                head_deg, neck_deg, chest_deg, spine_deg, static_cast<double>(std::abs(l_sh_ws.y - r_sh_ws.y)),
                l_drop_deg, r_drop_deg);

    // Head/neck carry only the ~1.1 deg the user turned (HUD P+0.6 Y-0.8 R-0.4); the torso stays at rest.
    CHECK(head_deg < 1.8);
    CHECK(neck_deg < 1.5);
    CHECK(chest_deg < 1.0);
    CHECK(spine_deg < 1.0);
    CHECK(std::abs(l_sh_ws.y - r_sh_ws.y) < 0.015f);
    CHECK(l_drop_deg >= 60.0);
    CHECK(l_drop_deg <= 76.0);
    CHECK(r_drop_deg >= 60.0);
    CHECK(r_drop_deg <= 76.0);

    // Facing proof from actual skinned joint positions (not assumed axes): the avatar's left is +X for a +Z-facing
    // VRM, so forward = cross(left, up). The front Studio camera sits on +Z looking down -Z.
    {
        using aether::animation::Vec3;
        const auto skinned = [&](uint32_t b) { return transform_point_3x4(skin.bone_matrices[b], rest_ws[b]); };
        const Vec3 world_up{0.0f, 1.0f, 0.0f};
        const Vec3 cam_fwd{0.0f, 0.0f, -1.0f};
        const float* hm = skin.bone_matrices[AETHER_BONE_HEAD];
        const Vec3 head_up = aether::animation::normalize_safe(Vec3{hm[1], hm[5], hm[9]});
        const Vec3 eye_left =
            aether::animation::normalize_safe(skinned(AETHER_BONE_LEFT_EYE) - skinned(AETHER_BONE_RIGHT_EYE));
        const Vec3 head_fwd = aether::animation::normalize_safe(aether::animation::cross(eye_left, head_up));
        const Vec3 chest_left = aether::animation::normalize_safe(skinned(AETHER_BONE_LEFT_UPPER_ARM) -
                                                                  skinned(AETHER_BONE_RIGHT_UPPER_ARM));
        const float* cm = skin.bone_matrices[AETHER_BONE_CHEST];
        const Vec3 chest_up = aether::animation::normalize_safe(Vec3{cm[1], cm[5], cm[9]});
        const Vec3 chest_fwd = aether::animation::normalize_safe(aether::animation::cross(chest_left, chest_up));
        const float head_dot_cam = aether::animation::dot(head_fwd, cam_fwd);
        const float chest_dot_cam = aether::animation::dot(chest_fwd, cam_fwd);
        const float head_up_dot = aether::animation::dot(head_up, world_up);
        std::printf("[FACING] dot(head_fwd,cam_fwd)=%.4f dot(chest_fwd,cam_fwd)=%.4f dot(head_up,world_up)=%.4f\n",
                    static_cast<double>(head_dot_cam), static_cast<double>(chest_dot_cam),
                    static_cast<double>(head_up_dot));
        CHECK(head_dot_cam < -0.98f);
        CHECK(chest_dot_cam < -0.98f);
        CHECK(head_up_dot > 0.98f);
    }

    avatar->update_pose(0, 1, skin.bone_matrices, skin.bone_count, skin.morph_weights, skin.morph_count, true);
    avatar->update_pose(1, 1, skin.bone_matrices, skin.bone_count, skin.morph_weights, skin.morph_count, true);

    auto rig = aether::renderer::LightingRig::default_three_point();
    rig.bg_rgba[0] = 0.067f;
    rig.bg_rgba[1] = 0.075f;
    rig.bg_rgba[2] = 0.094f;
    rig.bg_rgba[3] = 1.0f;

    uint64_t fidx = 0;
    auto capture_shot = [&](const char* suffix, const aether::renderer::PreviewCamera& cam, bool rt_enabled) {
        for (int warm = 0; warm < 4; ++warm) {
            aether::renderer::FrameRenderRequest req{};
            req.avatar = avatar.get();
            req.rig = &rig;
            req.camera = cam;
            req.frame_index = fidx;
            req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
            req.rt_enabled = rt_enabled && rhi->rt_supported();
            req.show_checkerboard = false;
            req.render_csm_shadow = true;
            REQUIRE(renderer.render_headless_frame(req).has_value());
            ++fidx;
        }
        const auto outs = renderer.graph().outputs();
        auto px = renderer.graph().readback_image_rgba8(outs.viewport_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                        kW, kH);
        REQUIRE(px.has_value());
        const auto st = analyze_rgba8(px.value(), kW, kH);
        const auto& snap = renderer.graph().timing().latest_snapshot();
        const auto& dm = renderer.graph().denoiser();
        std::printf("[SHOT %s_%s] fg=%u mean_luma=%.1f p95_luma=%.1f white_clip=%u denoise=%.3fms (p95=%.3fms, "
                    "active=%s, fallback=%d/%d)\n",
                    prefix.c_str(), suffix, st.fg_pixels, st.mean_luma, st.p95_luma, st.white_clipped_pixels,
                    static_cast<double>(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Denoise)]),
                    static_cast<double>(dm.rolling_p95_ms()), dm.active_name(), dm.fallback_triggered() ? 1 : 0,
                    static_cast<int>(dm.fallback_reason()));
        const std::filesystem::path out_path = snap_dir / (prefix + "_" + suffix + ".png");
        REQUIRE(aether::test::save_rgba8_png(out_path, kW, kH, px.value().data()));
    };

    aether::renderer::PreviewCamera front_cam = fit_cam;
    front_cam.yaw_rad = 0.0f;
    front_cam.pitch_rad = 0.05f;
    capture_shot("front", front_cam, true);

    aether::renderer::PreviewCamera tq = fit_cam;
    tq.yaw_rad = 0.40f;
    tq.pitch_rad = 0.08f;
    tq.distance = std::max(0.85f, fit_cam.distance * 0.78f);
    capture_shot("three_quarter_rt_on", tq, true);

    aether::renderer::PreviewCamera neck = fit_cam;
    neck.target_y = fit_cam.target_y + 0.32f;
    neck.yaw_rad = 0.25f;
    neck.pitch_rad = 0.05f;
    neck.distance = 0.65f;
    neck.fovy_rad = 0.55f;
    capture_shot("neck_closeup_rt_on", neck, true);
    capture_shot("neck_closeup_rt_off", neck, false);

    // Verify 4-mask RT geometry partitioning on real VRM (FaceSkin 0x01, Hair 0x02, HandsProps 0x04, BodyCloth 0x08)
    {
        const auto part = aether::renderer::build_rt_partitioned_indices(*avatar);
        std::printf("[RT_4MASK] face=%u hair=%u hands=%u body=%u tris\n", part.bucket_index_count[0] / 3u,
                    part.bucket_index_count[1] / 3u, part.bucket_index_count[2] / 3u, part.bucket_index_count[3] / 3u);
        CHECK(part.bucket_index_count[0] > 0u);
        CHECK(part.bucket_index_count[1] > 0u);
        CHECK(part.bucket_index_count[2] > 0u);
        CHECK(part.bucket_index_count[3] > 0u);
    }

    // Visual Benchmark Modes on 8590256991748008892.vrm: Mode 0 Traditional, Mode 1 WuWa Hybrid RT,
    // Mode 2 2.5D Path Tracing, and Makoto Shinkai Cinema Golden Hour (Circadian 17:35 sun).
    {
        const auto base_rig = rig;
        auto capture_mode = [&](const char* suffix, uint8_t lm, bool shinkai_golden, bool wuwa_nextgen) {
            rig = base_rig;
            rig.lighting_mode = lm;
            if (wuwa_nextgen) {
                rig.cel_band_sharpness = 0.018f;
                rig.rim_step_strength = 0.65f;
                rig.studio_lights[0].orbit_azimuth_deg = 36.0f;
                rig.studio_lights[0].orbit_elevation_deg = 26.0f;
                rig.studio_lights[0].intensity_ev100 = 1.08f;
            } else if (shinkai_golden) {
                rig.indirect_bleed_strength = 1.05f;
                rig.bloom_intensity = 0.14f;
                rig.enable_paper_warmth = true;
                rig.paper_warmth_strength = 0.35f;
                const auto cres = aether::renderer::evaluate_circadian_lighting(17.58f);
                aether::renderer::apply_circadian_to_rig(cres, rig);
            }
            const int warmup = (lm >= 2u) ? 8 : 4;
            for (int w = 0; w < warmup; ++w) {
                aether::renderer::FrameRenderRequest req{};
                req.avatar = avatar.get();
                req.rig = &rig;
                req.camera = tq;
                req.frame_index = fidx;
                req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
                req.lighting_mode = lm;
                req.rt_enabled = (lm > 0u) && rhi->rt_supported();
                req.show_checkerboard = false;
                req.render_csm_shadow = true;
                REQUIRE(renderer.render_headless_frame(req).has_value());
                ++fidx;
            }
            const auto outs = renderer.graph().outputs();
            auto px = renderer.graph().readback_image_rgba8(outs.viewport_image,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW, kH);
            REQUIRE(px.has_value());
            const auto& snap = renderer.graph().timing().latest_snapshot();
            std::printf("[BENCH %s] mode=%u total=%.2fms rt=%.2fms denoise=%.2fms npr=%.2fms post=%.2fms\n", suffix,
                        static_cast<unsigned>(lm), static_cast<double>(snap.gpu_total_ms),
                        static_cast<double>(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::RayTrace)]),
                        static_cast<double>(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Denoise)]),
                        static_cast<double>(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::Live2dNpr)]),
                        static_cast<double>(snap.pass_ms[static_cast<size_t>(aether::rhi::GpuPassId::ToneMap)]));
            REQUIRE(
                aether::test::save_rgba8_png(snap_dir / (prefix + "_" + suffix + ".png"), kW, kH, px.value().data()));
        };
        capture_mode("mode0_traditional", 0u, false, false);
        capture_mode("preset_wuthering_waves", 1u, false, true);
        capture_mode("mode2_path_tracing", 2u, false, false);
        capture_mode("preset_shinkai_golden_hour", 2u, true, false);
        rig = base_rig;
    }

    // Face close-ups: neutral (both RT ON and OFF), then both eyes fully blinked + mouth "A".
    aether::renderer::PreviewCamera face = fit_cam;
    face.target_y = fit_cam.target_y + 0.53f;
    face.yaw_rad = 0.0f;
    face.pitch_rad = 0.0f;
    face.distance = 0.36f;
    face.fovy_rad = 0.50f;
    capture_shot("face_neutral", face, false);
    capture_shot("face_neutral_rt_on", face, true);

    // Side key light (65 deg azimuth, 30 deg elevation): exercises the face SDF terminator, hair-bang cast
    // shadows on the face, the SSS terminator band and shadow-side ambient, which a frontal key hides.
    {
        const auto base_rig = rig;
        auto capture_side = [&](const char* suffix, const aether::renderer::PreviewCamera& cam, uint8_t lm,
                                float az_deg = 65.0f) {
            rig = base_rig;
            rig.lighting_mode = lm;
            rig.studio_lights[0].orbit_azimuth_deg = az_deg;
            rig.studio_lights[0].orbit_elevation_deg = 30.0f;
            const int warmup = (lm >= 2u) ? 8 : 4;
            for (int w = 0; w < warmup; ++w) {
                aether::renderer::FrameRenderRequest req{};
                req.avatar = avatar.get();
                req.rig = &rig;
                req.camera = cam;
                req.frame_index = fidx;
                req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
                req.lighting_mode = lm;
                req.rt_enabled = (lm > 0u) && rhi->rt_supported();
                req.show_checkerboard = false;
                req.render_csm_shadow = true;
                REQUIRE(renderer.render_headless_frame(req).has_value());
                ++fidx;
            }
            const auto outs = renderer.graph().outputs();
            auto px = renderer.graph().readback_image_rgba8(outs.viewport_image,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW, kH);
            REQUIRE(px.has_value());
            const auto st = analyze_rgba8(px.value(), kW, kH);
            std::printf("[SIDE %s] mode=%u fg=%u mean_luma=%.1f p95_luma=%.1f\n", suffix, static_cast<unsigned>(lm),
                        st.fg_pixels, st.mean_luma, st.p95_luma);
            REQUIRE(
                aether::test::save_rgba8_png(snap_dir / (prefix + "_" + suffix + ".png"), kW, kH, px.value().data()));
        };
        capture_side("side_face_mode0_traditional", face, 0u);
        capture_side("side_face_mode1_hybrid_rt", face, 1u);
        capture_side("side_face_mode2_path_tracing", face, 2u);
        capture_side("side_three_quarter_mode1_hybrid_rt", tq, 1u);
        capture_side("side_three_quarter_mode2_path_tracing", tq, 2u);
        capture_side("side_left_face_mode1_hybrid_rt", face, 1u, -65.0f);
        capture_side("side_back_face_mode1_hybrid_rt", face, 1u, 110.0f);
        capture_side("side_front_face_mode1_hybrid_rt", face, 1u, 20.0f);
        rig = base_rig;
    }

    std::array<float, aether::animation::kMaxMorphs> expr{};
    std::copy_n(skin.morph_weights, std::min<size_t>(expr.size(), skin.morph_count), expr.begin());
    expr[0] = 1.0f;                                        // eyeBlinkLeft  -> VRM Blink_L
    expr[7] = 1.0f;                                        // eyeBlinkRight -> VRM Blink_R
    expr[aether::animation::kVisemeSlotOffset + 0] = 1.0f; // VRM "A"
    avatar->update_pose(0, 2, skin.bone_matrices, skin.bone_count, expr.data(), skin.morph_count, true);
    avatar->update_pose(1, 2, skin.bone_matrices, skin.bone_count, expr.data(), skin.morph_count, true);
    capture_shot("face_blink_mouth_a", face, false);

    CHECK(rhi->validation_error_count() == 0);
    renderer.destroy();
    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("120-frame idle steady state on 8590256991748008892.vrm (330 Hz SimEngine + SpringBones)",
          "[vrm][steady][spring]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = false;
    cfg.require_validation = false;
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();

    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());
    REQUIRE(avatar->springs.size() > 0U);
    REQUIRE(avatar->colliders.size() > 0U);

    // 1. Verify full SimEngine idle convergence across 120 frames after tracking loss (at 333 Hz high FPS).
    const aether::animation::RestSkeletonMetrics rm = build_vrm_rest_metrics(*avatar);

    aether::animation::SimEngine engine;
    engine.reset(rm);
    aether::animation::SimEngineConfig sim_cfg{};
    sim_cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs graph_out{};
    aether::animation::SkinningFrame skin{};

    // Stimulate brief head movement so tracking & spring filters are non-stationary, then drop confidence to 0.
    uint64_t t_us = 1000000ULL;
    for (int f = 0; f < 10; ++f) {
        AetherTrackingFrame tf{};
        tf.frame_sequence = static_cast<uint32_t>(f + 1);
        tf.capture_timestamp_us = t_us;
        tf.inference_complete_us = t_us;
        tf.valid_flags = 0x01U;
        tf.upper_body_confidence = 0.9f;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            tf.joint_rotations_quat[b][3] = 1.0f;
            tf.joint_positions_ws[b][3] = 0.0f;
        }
        tf.joint_positions_ws[AETHER_BONE_HIPS][0] = rm.rest_local_positions[AETHER_BONE_HIPS].x;
        tf.joint_positions_ws[AETHER_BONE_HIPS][1] = rm.rest_local_positions[AETHER_BONE_HIPS].y;
        tf.joint_positions_ws[AETHER_BONE_HIPS][2] = rm.rest_local_positions[AETHER_BONE_HIPS].z;
        tf.joint_positions_ws[AETHER_BONE_HIPS][3] = 0.9f;
        tf.joint_positions_ws[AETHER_BONE_SPINE][3] = 0.9f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][0] = 0.015f * static_cast<float>(f % 3);
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.48f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][2] = 0.0f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.9f;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][1] = 0.08f;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = 0.9968f;
        engine.tick(tf, dsp, graph_out, t_us, 0.010f, sim_cfg, skin);
        t_us += 16667ULL;
    }

    // 120 consecutive idle frames at 60 Hz (2.0 s settling window after perturbation):
    // monitor final 30 frames' bone rotation/position variance & SpringBone tip velocity.
    std::vector<std::array<float, AETHER_HUMANOID_BONE_COUNT * 12U>> tail_mats;
    tail_mats.reserve(30);
    std::vector<aether::animation::Vec3> prev_tips(engine.spring_simulator().joint_count());
    double max_sim_tip_vel = 0.0;
    uint32_t worst_sim_j = 0;
    double worst_sim_v = 0.0;
    for (int f = 0; f < 120; ++f) {
        AetherTrackingFrame idle_tf{};
        idle_tf.frame_sequence = static_cast<uint32_t>(11 + f);
        idle_tf.capture_timestamp_us = t_us;
        idle_tf.inference_complete_us = t_us;
        idle_tf.valid_flags = 0;
        engine.tick(idle_tf, dsp, graph_out, t_us, 0.010f, sim_cfg, skin);

        if (f >= 90) {
            std::array<float, AETHER_HUMANOID_BONE_COUNT * 12U> row{};
            for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                std::copy_n(skin.bone_matrices[b], 12U, &row[b * 12U]);
            }
            tail_mats.push_back(row);
            double step_max_vel = 0.0;
            for (uint32_t j = 0; j < engine.spring_simulator().joint_count(); ++j) {
                const auto p = engine.spring_simulator().joint_tail_pos(j);
                if (f > 90) {
                    const double v = static_cast<double>(aether::animation::length(p - prev_tips[j])) * 60.0;
                    step_max_vel = std::max(step_max_vel, v);
                    if (f == 119 && v > worst_sim_v) {
                        worst_sim_v = v;
                        worst_sim_j = j;
                    }
                }
                prev_tips[j] = p;
            }
            if (f >= 118) {
                max_sim_tip_vel = std::max(max_sim_tip_vel, step_max_vel);
            }
        }
        t_us += 16667ULL;
    }

    // Verify 120 additional consecutive idle frames at 333 Hz (dt = 3 ms, >326 FPS) produce zero jitter.
    double max_333hz_tip_vel = 0.0;
    for (int f = 0; f < 120; ++f) {
        AetherTrackingFrame idle_tf{};
        idle_tf.frame_sequence = static_cast<uint32_t>(131 + f);
        idle_tf.capture_timestamp_us = t_us;
        idle_tf.inference_complete_us = t_us;
        idle_tf.valid_flags = 0;
        engine.tick(idle_tf, dsp, graph_out, t_us, 0.010f, sim_cfg, skin);
        for (uint32_t j = 0; j < engine.spring_simulator().joint_count(); ++j) {
            const auto p = engine.spring_simulator().joint_tail_pos(j);
            const double v = static_cast<double>(aether::animation::length(p - prev_tips[j])) / 0.003;
            max_333hz_tip_vel = std::max(max_333hz_tip_vel, v);
            prev_tips[j] = p;
        }
        t_us += 3000ULL;
    }

    double max_bone_mat_var = 0.0;
    for (size_t k = 0; k < tail_mats[0].size(); ++k) {
        double mean = 0.0;
        for (const auto& r : tail_mats) {
            mean += r[k];
        }
        mean /= static_cast<double>(tail_mats.size());
        double var = 0.0;
        for (const auto& r : tail_mats) {
            const double d = r[k] - mean;
            var += d * d;
        }
        var /= static_cast<double>(tail_mats.size());
        max_bone_mat_var = std::max(max_bone_mat_var, var);
    }

    // 2. Also populate SpringBoneSimulator directly with 8590256991748008892.vrm's loaded springs + colliders.
    aether::animation::SpringBoneSimulator vrm_spring;
    std::array<aether::animation::Vec3, aether::animation::kMaxBones> bpos{};
    std::array<aether::animation::Quat, aether::animation::kMaxBones> brot{};
    brot.fill(aether::animation::Quat::identity());
    const uint32_t num_bones =
        std::min<uint32_t>(aether::animation::kMaxBones, static_cast<uint32_t>(avatar->joint_rest_world_pos.size()));
    for (uint32_t b = 0; b < num_bones; ++b) {
        const auto& jp = avatar->joint_rest_world_pos[b];
        bpos[b] = {jp[0], jp[1], jp[2]};
    }
    for (const auto& c : avatar->colliders) {
        if (c.joint_index >= num_bones) {
            continue;
        }
        const int32_t j = static_cast<int32_t>(c.joint_index);
        if (c.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE) {
            (void)vrm_spring.add_capsule_collider(j, {c.offset[0], c.offset[1], c.offset[2]},
                                                  {c.tail[0], c.tail[1], c.tail[2]}, c.radius, c.group_bit);
        } else {
            (void)vrm_spring.add_sphere_collider(j, {c.offset[0], c.offset[1], c.offset[2]}, c.radius, c.group_bit);
        }
    }
    for (const auto& sp : avatar->springs) {
        if (sp.joint_index >= num_bones) {
            continue;
        }
        aether::animation::SpringJointDesc d{};
        d.bone_index = static_cast<int32_t>(sp.joint_index);
        d.parent_joint_index = sp.parent_spring_index;
        d.parent_bone_index = avatar->joints[sp.joint_index].parent_index;
        d.center_bone_index = sp.center_joint_index;
        if (d.parent_bone_index >= 0 && static_cast<uint32_t>(d.parent_bone_index) < num_bones) {
            d.local_bone_offset = bpos[sp.joint_index] - bpos[d.parent_bone_index];
        } else {
            d.local_bone_offset = bpos[sp.joint_index];
        }
        d.rest_dir = {sp.rest_dir[0], sp.rest_dir[1], sp.rest_dir[2]};
        d.bone_length = sp.bone_length;
        d.hit_radius = sp.hit_radius;
        d.stiffness = sp.stiffness;
        d.drag_force = sp.drag_force;
        d.gravity_power = sp.gravity_power;
        d.gravity_dir = {sp.gravity_dir[0], sp.gravity_dir[1], sp.gravity_dir[2]};
        d.collider_mask = static_cast<uint32_t>(sp.collider_group_mask & 0xFFFFFFFFULL);
        d.filter_colliders_by_mask = true;
        (void)vrm_spring.add_joint(d);
    }
    vrm_spring.reset_state(bpos, brot, num_bones);

    std::vector<aether::animation::Vec3> prev_vrm_tips(vrm_spring.joint_count());
    double max_vrm_tip_vel = 0.0;
    uint32_t worst_vrm_j = 0;
    double worst_vrm_v = 0.0;
    for (int f = 0; f < 120; ++f) {
        vrm_spring.step(1.0f / 60.0f, {0.0f, 0.0f, 0.0f}, 1.0f, bpos, brot, num_bones);
        if (f >= 100) {
            for (uint32_t j = 0; j < vrm_spring.joint_count(); ++j) {
                const auto p = vrm_spring.joint_tail_pos(j);
                if (f > 100) {
                    const double v = static_cast<double>(aether::animation::length(p - prev_vrm_tips[j])) * 60.0;
                    max_vrm_tip_vel = std::max(max_vrm_tip_vel, v);
                    if (f == 119 && v > worst_vrm_v) {
                        worst_vrm_v = v;
                        worst_vrm_j = j;
                    }
                }
                prev_vrm_tips[j] = p;
            }
        }
    }

    std::printf("[STEADY-STATE 120f] max_bone_mat_var=%.2e sim_tip_vel=%.2e m/s (333Hz=%.2e, worst j=%u %.2e) "
                "vrm_tip_vel=%.2e m/s (worst j=%u %.2e) (vrm_springs=%u cols=%u)\n",
                max_bone_mat_var, max_sim_tip_vel, max_333hz_tip_vel, worst_sim_j, worst_sim_v, max_vrm_tip_vel,
                worst_vrm_j, worst_vrm_v, vrm_spring.joint_count(), vrm_spring.collider_count());
    CHECK(max_bone_mat_var < 1e-5);
    CHECK(max_sim_tip_vel < 1e-5);
    CHECK(max_333hz_tip_vel < 1e-5);
    CHECK(max_vrm_tip_vel < 1e-5);

    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("8590256991748008892.vrm: zero stationary jitter and smooth Spine->Chest->Neck->Head cascade",
          "[vrm][motion][cascade]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();
    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());
    const aether::animation::RestSkeletonMetrics rm = build_vrm_rest_metrics(*avatar);

    using namespace aether::animation;
    SimEngine engine;
    engine.reset(rm);
    SimEngineConfig sim_cfg{};
    sim_cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs graph_out{};
    SkinningFrame skin{};

    const auto make_frame = [&](int f, float yaw_rad) {
        AetherTrackingFrame tf{};
        tf.frame_sequence = static_cast<uint32_t>((f / 2) + 1); // 30 Hz camera -> 60 Hz sim
        tf.capture_timestamp_us = static_cast<uint64_t>((f / 2) * 2 + 1) * 16667ULL;
        tf.inference_complete_us = tf.capture_timestamp_us;
        tf.valid_flags = 0x01U;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            tf.joint_rotations_quat[b][3] = 1.0f;
        }
        const Quat q = from_axis_angle({0.0f, 1.0f, 0.0f}, yaw_rad);
        tf.joint_rotations_quat[AETHER_BONE_HEAD][1] = q.y;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = q.w;
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        return tf;
    };

    // 1. 120 stationary frames at neutral: the last 60 skinning matrices must be bit-identical (0.0000 jitter).
    std::array<float, AETHER_HUMANOID_BONE_COUNT * 12U> ref{};
    double max_jitter = 0.0;
    for (int f = 0; f < 120; ++f) {
        const AetherTrackingFrame tf = make_frame(f, 0.0f);
        engine.tick(tf, dsp, graph_out, static_cast<uint64_t>(f + 1) * 16667ULL, 0.0f, sim_cfg, skin);
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            for (size_t k = 0; k < 12U; ++k) {
                if (f > 60) {
                    max_jitter = std::max(max_jitter, std::abs(static_cast<double>(skin.bone_matrices[b][k]) -
                                                               static_cast<double>(ref[b * 12U + k])));
                }
                ref[b * 12U + k] = skin.bone_matrices[b][k];
            }
        }
    }
    std::printf("[CASCADE] stationary max bone-matrix delta over 60 frames = %.7f\n", max_jitter);
    CHECK(max_jitter == 0.0);

    // 2. Sine head turn (+-20 deg @ 0.5 Hz): world yaw must cascade Spine < Chest < Neck < Head with no spikes.
    const auto world_yaw_deg = [&](uint32_t b) {
        const Quat d = normalize(engine.bone_rot_ws()[b] * conjugate(rm.rest_world_rotations[b]));
        const Vec3 fwd = rotate_vec(d, {0.0f, 0.0f, 1.0f});
        return std::atan2(fwd.x, fwd.z) * kRadToDeg;
    };
    constexpr std::array<uint32_t, 4> kChain = {AETHER_BONE_SPINE, AETHER_BONE_CHEST, AETHER_BONE_NECK,
                                                AETHER_BONE_HEAD};
    std::array<float, 4> peak{};
    std::array<float, 3> hist{};
    float max_jerk = 0.0f;
    for (int f = 0; f < 180; ++f) {
        const float t = static_cast<float>(f) / 60.0f;
        const AetherTrackingFrame tf = make_frame(120 + f, 20.0f * kDegToRad * std::sin(kTwoPi * 0.5f * t));
        engine.tick(tf, dsp, graph_out, static_cast<uint64_t>(121 + f) * 16667ULL, 0.0f, sim_cfg, skin);
        for (size_t i = 0; i < kChain.size(); ++i) {
            peak[i] = std::max(peak[i], std::abs(world_yaw_deg(kChain[i])));
        }
        const float hy = world_yaw_deg(AETHER_BONE_HEAD);
        if (f >= 2) {
            max_jerk = std::max(max_jerk, std::abs(hy - 2.0f * hist[1] + hist[0]));
        }
        hist[0] = hist[1];
        hist[1] = hy;
    }
    std::printf("[CASCADE] peak world yaw: spine=%.2f chest=%.2f neck=%.2f head=%.2f deg, max head 2nd-diff=%.3f deg\n",
                static_cast<double>(peak[0]), static_cast<double>(peak[1]), static_cast<double>(peak[2]),
                static_cast<double>(peak[3]), static_cast<double>(max_jerk));
    CHECK(peak[0] > 1.0f);
    CHECK(peak[1] > peak[0]);
    CHECK(peak[2] > peak[1]);
    CHECK(peak[3] > peak[2]);
    CHECK(peak[3] > 15.0f);
    CHECK(peak[3] < 20.5f);  // critically damped: no overshoot beyond the tracked 20 deg
    CHECK(max_jerk < 0.35f); // C2-smooth: no per-frame acceleration spikes from the 30 Hz camera steps

    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Real Windows Media Foundation webcam enumeration and live capture", "[tracking][wmf][webcam]") {
    const auto devs = aether::tracking::enumerate_camera_devices();
    std::printf("[WMF-CAM] enumerated %zu device(s):\n", devs.size());
    for (size_t i = 0; i < devs.size(); ++i) {
        std::printf("  [%zu] name='%s' pref=%ux%u@%ufps virtual=%d\n", i, devs[i].friendly_name.c_str(),
                    devs[i].preferred_width, devs[i].preferred_height, devs[i].preferred_fps,
                    devs[i].likely_virtual ? 1 : 0);
    }
    REQUIRE_FALSE(devs.empty());

    // Physical camera should be ordered ahead of virtual camera drivers.
    const bool has_physical = std::any_of(devs.begin(), devs.end(), [](const auto& d) { return !d.likely_virtual; });
    if (has_physical) {
        CHECK_FALSE(devs.front().likely_virtual);
    }

    auto src = aether::tracking::create_mf_camera_source(devs.front().symbolic_link, 1280, 720, 60);
    REQUIRE(src != nullptr);
    REQUIRE(src->open() == AETHER_OK);
    CHECK_FALSE(src->is_synthetic());
    CHECK(src->is_open());
    CHECK(src->width() >= 640U);
    CHECK(src->height() >= 480U);
    CHECK(src->fps() >= 15U);

    const aether::tracking::CapturedFrameBuffer* frame = nullptr;
    bool got_frame = false;
    for (int attempt = 0; attempt < 5 && !got_frame; ++attempt) {
        got_frame = src->read_latest_blocking(600, frame) && frame != nullptr;
    }
    REQUIRE(got_frame);
    CHECK(frame->width == src->width());
    CHECK(frame->height == src->height());
    CHECK(frame->sequence >= 1ULL);
    size_t non_zero_bytes = 0;
    const size_t probe_len = std::min<size_t>(frame->bytes.size(), 4096U);
    for (size_t i = 0; i < probe_len; ++i) {
        if (frame->bytes[i] != 0U) {
            ++non_zero_bytes;
        }
    }
    CHECK(non_zero_bytes > 0U);
    std::printf("[WMF-CAM] live captured seq=%llu %ux%u@%ufps fmt=%u non_zero=%zu/%zu label='%s'\n",
                static_cast<unsigned long long>(frame->sequence), frame->width, frame->height, src->fps(),
                static_cast<unsigned>(frame->format), non_zero_bytes, probe_len, src->source_label().c_str());

    src->close();
    CHECK_FALSE(src->is_open());
}

TEST_CASE(
    "Task 4: 8590256991748008892.vrm binds Live2D brow/vowel parameters and supports !has_split_brow_morphs fallback",
    "[vrm][live2d][brow]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();
    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    std::printf("[VRM-BROW] has_split_brow_morphs=%d brow_bones=%zu\n", avatar->has_split_brow_morphs ? 1 : 0,
                avatar->brow_bones.size());

    using namespace aether::animation;
    const RestSkeletonMetrics rm = build_vrm_rest_metrics(*avatar);

    auto run_l2d = [&](const RestSkeletonMetrics& use_rm, const Live2DParameters& l2d_in) {
        SimEngine engine;
        engine.reset(use_rm);
        SimEngineConfig sim_cfg{};
        sim_cfg.visibility.enable_idle_breathing = false;
        AetherDspSyncPacket dsp{};
        AetherGraphOutputs gout{};
        SkinningFrame skin{};

        AetherTrackingFrame tf{};
        tf.valid_flags = 0x09U;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            tf.joint_rotations_quat[b][3] = 1.0f;
        }
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        pack_live2d_into_tracking_frame(l2d_in, tf);

        for (int f = 0; f < 30; ++f) {
            tf.frame_sequence = static_cast<uint32_t>(f + 1);
            tf.capture_timestamp_us = static_cast<uint64_t>(f + 1) * 16667ULL;
            tf.inference_complete_us = tf.capture_timestamp_us;
            engine.tick(tf, dsp, gout, tf.capture_timestamp_us, 0.0f, sim_cfg, skin);
        }
        return skin;
    };

    // Split-brow path with raised brows & wide open 'A' mouth
    Live2DParameters up_a{};
    up_a.param_brow_l_y = 0.85f;
    up_a.param_brow_r_y = 0.85f;
    up_a.param_brow_l_form = 0.50f;
    up_a.param_brow_r_form = 0.50f;
    up_a.param_mouth_open_y = 0.85f;
    up_a.param_mouth_form = 0.0f;
    const SkinningFrame s_split = run_l2d(rm, up_a);
    CHECK(s_split.live2d.vowel_a > 0.45f);
    CHECK(s_split.arkit_preview[17] > 0.50f); // jawOpen
    CHECK(s_split.arkit_preview[43] > 0.40f); // browInnerUp

    // Smile with closed mouth: must keep vowels 0 and enrich smile without overriding blinks
    Live2DParameters smile_closed{};
    smile_closed.param_brow_l_y = -0.80f;
    smile_closed.param_brow_r_y = -0.80f;
    smile_closed.param_mouth_open_y = 0.02f; // < 0.06 gate
    smile_closed.param_mouth_form = 0.90f;
    RestSkeletonMetrics rm_fallback = rm;
    rm_fallback.has_split_brow_morphs = false;
    const SkinningFrame s_fb = run_l2d(rm_fallback, smile_closed);
    CHECK(s_fb.live2d.vowel_a == 0.0f);
    CHECK(s_fb.live2d.vowel_i == 0.0f);
    CHECK(s_fb.arkit_preview[23] > 0.15f); // mouthSmileLeft
    CHECK(s_fb.arkit_preview[24] > 0.15f); // mouthSmileRight
    CHECK(s_fb.arkit_preview[58] > 0.25f); // SLOT_ANGRY fallback when !has_split_brow_morphs

    // Exercise GpuAvatar::update_pose with opt_live2d and verify clean morphs/brow bones
    avatar->update_pose(0U, 1U, s_split.bone_matrices, s_split.bone_count, s_split.arkit_preview,
                        AETHER_EXPRESSION_SLOT_COUNT, true, &s_split.live2d);
    avatar->update_pose(1U, 2U, s_fb.bone_matrices, s_fb.bone_count, s_fb.arkit_preview, AETHER_EXPRESSION_SLOT_COUNT,
                        true, &s_fb.live2d);

    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}

TEST_CASE("Task 1 Forensic Audit: 8590256991748008892.vrm morph overlaps, SpringBone collider penetrations, 30->240Hz "
          "pacing, and Rust graph",
          "[forensic][vrm]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();

    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    auto str_slice = [&](uint32_t off, uint32_t len) -> std::string {
        if (off >= avatar->strings.size()) {
            return {};
        }
        const size_t n = std::min<size_t>(len, avatar->strings.size() - off);
        return avatar->strings.substr(off, n);
    };

    std::printf("\n=== [FORENSIC] MODEL INVENTORY (%s) ===\n", vrm_path.string().c_str());
    std::printf("source_format=%u verts=%zu tris=%zu joints=%zu springs=%zu colliders=%zu morphs=%zu expr_bindings=%zu "
                "split_brow=%d brow_bones=%zu\n",
                avatar->desc.source_format, avatar->rest_vertices.size(), avatar->rest_indices.size() / 3,
                avatar->joints.size(), avatar->springs.size(), avatar->colliders.size(), avatar->morph_ranges.size(),
                avatar->expressions.size(), avatar->has_split_brow_morphs ? 1 : 0, avatar->brow_bones.size());

    // Resolve joint world rotations for collider transforms
    std::vector<aether::animation::Quat> j_world_rot(avatar->joints.size(), aether::animation::Quat::identity());
    std::vector<uint8_t> j_done(avatar->joints.size(), 0U);
    auto resolve_jrot = [&](auto& self, size_t idx, int depth) -> aether::animation::Quat {
        if (idx >= avatar->joints.size() || depth > 128)
            return aether::animation::Quat::identity();
        if (j_done[idx] != 0U)
            return j_world_rot[idx];
        const auto& j = avatar->joints[idx];
        const float* br = j.bind_rotation_quat;
        const float q2 = br[0] * br[0] + br[1] * br[1] + br[2] * br[2] + br[3] * br[3];
        const aether::animation::Quat ql = (q2 > 1e-6f) ? aether::animation::normalize({br[0], br[1], br[2], br[3]})
                                                        : aether::animation::Quat::identity();
        const aether::animation::Quat qp =
            (j.parent_index >= 0 && static_cast<size_t>(j.parent_index) < avatar->joints.size())
                ? self(self, static_cast<size_t>(j.parent_index), depth + 1)
                : aether::animation::Quat::identity();
        j_world_rot[idx] = aether::animation::normalize(qp * ql);
        j_done[idx] = 1U;
        return j_world_rot[idx];
    };
    for (size_t i = 0; i < avatar->joints.size(); ++i)
        (void)resolve_jrot(resolve_jrot, i, 0);

    // 1. Expression Bindings & Morph Vertex Overlap Audit
    std::printf("\n=== [FORENSIC] EXPRESSION BINDINGS & MORPH OVERLAPS ===\n");
    auto collect_slot_verts = [&](uint32_t slot) -> std::vector<uint32_t> {
        std::vector<uint32_t> verts;
        for (const auto& eb : avatar->expressions) {
            if (eb.input_slot != slot || eb.morph_index >= avatar->morph_ranges.size())
                continue;
            const auto& mr = avatar->morph_ranges[eb.morph_index];
            const size_t start = mr.delta_offset;
            const size_t end = std::min(start + mr.delta_count, avatar->morph_deltas.size());
            for (size_t di = start; di < end; ++di) {
                const auto& d = avatar->morph_deltas[di];
                const float mag2 = d.d_position[0] * d.d_position[0] + d.d_position[1] * d.d_position[1] +
                                   d.d_position[2] * d.d_position[2];
                if (mag2 > 1e-10f)
                    verts.push_back(d.vertex_index);
            }
        }
        std::sort(verts.begin(), verts.end());
        verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
        return verts;
    };

    for (size_t ei = 0; ei < avatar->expressions.size(); ++ei) {
        const auto& eb = avatar->expressions[ei];
        const std::string p_name = str_slice(eb.name_offset, eb.name_len);
        std::string m_name = "<oob>";
        uint32_t d_cnt = 0;
        float max_disp = 0.0f;
        uint32_t is_face = 0;
        if (eb.morph_index < avatar->morph_ranges.size()) {
            const auto& mr = avatar->morph_ranges[eb.morph_index];
            m_name = str_slice(mr.name_offset, mr.name_len);
            d_cnt = mr.delta_count;
            is_face = mr.is_face_morph;
            const size_t end = std::min<size_t>(mr.delta_offset + mr.delta_count, avatar->morph_deltas.size());
            for (size_t di = mr.delta_offset; di < end; ++di) {
                const auto& d = avatar->morph_deltas[di];
                max_disp =
                    std::max(max_disp, std::sqrt(d.d_position[0] * d.d_position[0] + d.d_position[1] * d.d_position[1] +
                                                 d.d_position[2] * d.d_position[2]));
            }
        }
        std::printf(
            "  expr[%2zu]: slot=%2u preset='%s' morph[%u]='%s' weight=%.3f deltas=%u max_disp=%.4fm is_face=%u\n", ei,
            eb.input_slot, p_name.c_str(), eb.morph_index, m_name.c_str(), eb.weight, d_cnt, max_disp, is_face);
    }

    struct SlotTag {
        uint32_t slot;
        const char* name;
    };
    const SlotTag check_slots[] = {
        {0, "Blink_L(0)"},   {7, "Blink_R(7)"},   {17, "jawOpen(17)"},  {23, "SmileL(23)"},     {24, "SmileR(24)"},
        {41, "BrowDnL(41)"}, {42, "BrowDnR(42)"}, {43, "BrowInUp(43)"}, {44, "BrowOutUpL(44)"}, {45, "BrowOutUpR(45)"},
        {52, "Vis_A(52)"},   {53, "Vis_I(53)"},   {54, "Vis_U(54)"},    {55, "Vis_E(55)"},      {56, "Vis_O(56)"},
        {57, "Joy(57)"},     {58, "Angry(58)"},   {59, "Sorrow(59)"},   {60, "Surprised(60)"},  {61, "Blush(61)"},
        {62, "Sparkle(62)"}, {63, "Custom(63)"},
    };
    const auto v_blink_l = collect_slot_verts(0);
    const auto v_blink_r = collect_slot_verts(7);
    for (const auto& st : check_slots) {
        const auto sv = collect_slot_verts(st.slot);
        if (sv.empty())
            continue;
        std::vector<uint32_t> inter_l, inter_r;
        std::set_intersection(sv.begin(), sv.end(), v_blink_l.begin(), v_blink_l.end(), std::back_inserter(inter_l));
        std::set_intersection(sv.begin(), sv.end(), v_blink_r.begin(), v_blink_r.end(), std::back_inserter(inter_r));
        std::printf("  slot_verts %-16s: non_zero_verts=%zu overlap_BlinkL=%zu overlap_BlinkR=%zu\n", st.name,
                    sv.size(), inter_l.size(), inter_r.size());
    }

    // 2. VRMC_springBone / Colliders Rest-Pose Penetration Audit
    std::printf("\n=== [FORENSIC] VRM COLLIDERS (%zu) & SPRING CHAINS (%zu) ===\n", avatar->colliders.size(),
                avatar->springs.size());
    for (size_t ci = 0; ci < avatar->colliders.size(); ++ci) {
        const auto& c = avatar->colliders[ci];
        std::string jname = "<root>";
        aether::animation::Vec3 jpos{0, 0, 0};
        aether::animation::Quat jrot = aether::animation::Quat::identity();
        if (c.joint_index < avatar->joints.size()) {
            const auto& j = avatar->joints[c.joint_index];
            jname = str_slice(j.name_offset, j.name_len);
            jpos = {avatar->joint_rest_world_pos[c.joint_index][0], avatar->joint_rest_world_pos[c.joint_index][1],
                    avatar->joint_rest_world_pos[c.joint_index][2]};
            jrot = j_world_rot[c.joint_index];
        }
        const auto wa = jpos + aether::animation::rotate_vec(jrot, {c.offset[0], c.offset[1], c.offset[2]});
        const auto wb = jpos + aether::animation::rotate_vec(jrot, {c.tail[0], c.tail[1], c.tail[2]});
        std::printf("  col[%2zu]: group=%2u shape=%s j=%u('%s') r=%.4fm a=(%.3f,%.3f,%.3f) b=(%.3f,%.3f,%.3f)\n", ci,
                    c.group_bit, (c.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE ? "CAPS" : "SPH "), c.joint_index,
                    jname.c_str(), c.radius, wa.x, wa.y, wa.z, wb.x, wb.y, wb.z);
    }

    auto closest_pt_seg = [](aether::animation::Vec3 a, aether::animation::Vec3 b, aether::animation::Vec3 p) {
        const auto ab = b - a;
        const float len2 = aether::animation::dot(ab, ab);
        if (len2 < 1e-12f)
            return a;
        const float t = std::clamp(aether::animation::dot(p - a, ab) / len2, 0.0f, 1.0f);
        return a + ab * t;
    };

    uint32_t penetrating_pairs = 0;
    uint32_t near_touch_pairs = 0;
    for (size_t si = 0; si < avatar->springs.size(); ++si) {
        const auto& sp = avatar->springs[si];
        std::string jname = "<oob>";
        aether::animation::Vec3 jp{0, 0, 0};
        if (sp.joint_index < avatar->joints.size()) {
            const auto& j = avatar->joints[sp.joint_index];
            jname = str_slice(j.name_offset, j.name_len);
            jp = {avatar->joint_rest_world_pos[sp.joint_index][0], avatar->joint_rest_world_pos[sp.joint_index][1],
                  avatar->joint_rest_world_pos[sp.joint_index][2]};
        }
        float min_clear = 999.0f;
        int min_ci = -1;
        for (size_t ci = 0; ci < avatar->colliders.size(); ++ci) {
            const auto& c = avatar->colliders[ci];
            const uint64_t bit = (c.group_bit < 64U) ? (1ULL << c.group_bit) : 0ULL;
            if ((sp.collider_group_mask & bit) == 0ULL)
                continue;
            aether::animation::Vec3 jpos{0, 0, 0};
            aether::animation::Quat jrot = aether::animation::Quat::identity();
            if (c.joint_index < avatar->joints.size()) {
                jpos = {avatar->joint_rest_world_pos[c.joint_index][0], avatar->joint_rest_world_pos[c.joint_index][1],
                        avatar->joint_rest_world_pos[c.joint_index][2]};
                jrot = j_world_rot[c.joint_index];
            }
            const auto wa = jpos + aether::animation::rotate_vec(jrot, {c.offset[0], c.offset[1], c.offset[2]});
            const auto wb = (c.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE)
                                ? (jpos + aether::animation::rotate_vec(jrot, {c.tail[0], c.tail[1], c.tail[2]}))
                                : wa;
            const auto cp = (c.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE) ? closest_pt_seg(wa, wb, jp) : wa;
            const float dist = aether::animation::length(jp - cp);
            const float clearance = dist - (sp.hit_radius + c.radius);
            if (clearance < 0.002f) {
                if (clearance < 0.0f)
                    ++penetrating_pairs;
                else
                    ++near_touch_pairs;
                std::printf("  [COLLISION-REST] spring[%3zu] chain=%2u depth=%u j=%u('%s') vs col[%2zu] (grp=%u): "
                            "dist=%.4fm sum_r=%.4fm CLEARANCE=%.4fm\n",
                            si, sp.chain_id, sp.depth_level, sp.joint_index, jname.c_str(), ci, c.group_bit, dist,
                            sp.hit_radius + c.radius, clearance);
            }
            if (clearance < min_clear) {
                min_clear = clearance;
                min_ci = static_cast<int>(ci);
            }
        }
        if (sp.depth_level <= 1U) {
            std::printf("  spring[%3zu] chain=%2u d=%u j=%3u('%s') hitR=%.3f stiff=%.2f drag=%.2f grav=%.2f "
                        "mask=0x%llx min_clear=%.4fm(c=%d)\n",
                        si, sp.chain_id, sp.depth_level, sp.joint_index, jname.c_str(), sp.hit_radius, sp.stiffness,
                        sp.drag_force, sp.gravity_power, static_cast<unsigned long long>(sp.collider_group_mask),
                        min_clear, min_ci);
        }
    }
    std::printf("  Summary: penetrating_pairs=%u near_touch(<2mm)_pairs=%u\n", penetrating_pairs, near_touch_pairs);
    std::printf("  All avatar joints matching 'J_Sec_' or non-humanoid secondary:\n");
    for (size_t ji = 0; ji < avatar->joints.size(); ++ji) {
        const auto& j = avatar->joints[ji];
        const std::string jn = str_slice(j.name_offset, j.name_len);
        if (jn.find("J_Sec") != std::string::npos || j.humanoid_bone >= AETHER_HUMANOID_BONE_COUNT) {
            std::printf("    joint[%2zu] parent=%2d hum=%3u pos=(%.3f,%.3f,%.3f) tail=%.3f name='%s'\n", ji,
                        j.parent_index, j.humanoid_bone, avatar->joint_rest_world_pos[ji][0],
                        avatar->joint_rest_world_pos[ji][1], avatar->joint_rest_world_pos[ji][2], j.tail_length,
                        jn.c_str());
        }
    }

    // 3. Check Rust Plugin Host default graph dual-writer & 30->240->60 Hz pacing
    std::printf("\n=== [FORENSIC] RUST GRAPH DUAL-WRITER & 30->240->60 HZ PACING ===\n");
    AetherHostConfig hcfg{};
    hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
    hcfg.worker_threads = 1;
    AetherHostHandle host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &host) == AETHER_OK);
    REQUIRE(host != nullptr);

    AetherTrackingFrame tf_test{};
    tf_test.frame_sequence = 1;
    tf_test.valid_flags = 0x09U;
    tf_test.upper_body_confidence = 0.0f; // intentionally 0 while face valid!
    tf_test.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
    tf_test.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b)
        tf_test.joint_rotations_quat[b][3] = 1.0f;
    const auto qyaw = aether::animation::from_euler_xyz(0.0f, 15.0f * aether::animation::kDegToRad, 0.0f);
    tf_test.joint_rotations_quat[AETHER_BONE_HEAD][0] = qyaw.x;
    tf_test.joint_rotations_quat[AETHER_BONE_HEAD][1] = qyaw.y;
    tf_test.joint_rotations_quat[AETHER_BONE_HEAD][2] = qyaw.z;
    tf_test.joint_rotations_quat[AETHER_BONE_HEAD][3] = qyaw.w;
    tf_test.arkit_blendshapes[0] = 0.75f; // eyeBlinkLeft

    AetherGraphInputs gin{};
    gin.tracking = &tf_test;
    gin.now_us = 1000000ULL;
    gin.dt_s = 1.0f / 240.0f;
    gin.tick_index = 1;
    gin.dsp_visemes[0] = 0.80f;
    gin.dsp_rms_db = -18.0f;
    AetherGraphOutputs gout{};
    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    int32_t eval_rc = AETHER_E_INTERNAL;
    for (int wait_i = 0; wait_i < 200; ++wait_i) {
        eval_rc = aether_host_evaluate_active(host, &gin, &gout);
        if (eval_rc == AETHER_OK)
            break;
        timer.sleep_us(2000);
    }
    REQUIRE(eval_rc == AETHER_OK);
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    std::printf("  Rust default graph output: bone_rot_off[HEAD]=(%.4f,%.4f,%.4f,%.4f) ik[HEAD].w=%.3f expr[0]=%.3f "
                "expr[24]=%.3f expr[52]=%.3f override[0]=%.3f override[24]=%.3f\n",
                gout.bone_rotation_offsets[AETHER_BONE_HEAD][0], gout.bone_rotation_offsets[AETHER_BONE_HEAD][1],
                gout.bone_rotation_offsets[AETHER_BONE_HEAD][2], gout.bone_rotation_offsets[AETHER_BONE_HEAD][3],
                gout.ik_targets[0][7], gout.expression_weights[0], gout.expression_weights[24],
                gout.expression_weights[52], gout.expression_override[0], gout.expression_override[24]);
    aether_host_destroy(host);

    // 4. Current SimEngine 30 Hz camera -> 240 Hz sim -> 60 Hz sample logging
    const auto rm = build_vrm_rest_metrics(*avatar);
    aether::animation::SimEngine sim;
    sim.reset(rm);
    aether::animation::SimEngineConfig scfg{};
    scfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout_zero{};
    aether::animation::SkinningFrame skin{};

    std::printf("  SimEngine 30Hz cam -> 240Hz tick -> 60Hz render sample (constant 20 deg/s yaw ramp with 0.08 deg "
                "cam jitter):\n");
    for (int t2 = 0; t2 < 48;
         ++t2) { // 12 camera intervals * 4 ticks? At 240Hz, 30Hz = every 8 ticks; 48 ticks = 0.2s (12 render frames)
        const uint32_t cam_seq = static_cast<uint32_t>(t2 / 8 + 1);
        const double cam_t_s = static_cast<double>(cam_seq - 1U) * (1.0 / 30.0);
        const double yaw_deg = 20.0 * cam_t_s + ((cam_seq & 1U) ? 0.08 : -0.08);
        const auto q =
            aether::animation::from_euler_xyz(0.0f, static_cast<float>(yaw_deg * aether::animation::kDegToRad), 0.0f);
        AetherTrackingFrame tf{};
        tf.frame_sequence = cam_seq;
        tf.capture_timestamp_us = static_cast<uint64_t>(t2 + 1) * 4167ULL;
        tf.inference_complete_us = tf.capture_timestamp_us;
        tf.valid_flags = 0x01U;
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b)
            tf.joint_rotations_quat[b][3] = 1.0f;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][0] = q.x;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][1] = q.y;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][2] = q.z;
        tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = q.w;
        sim.tick(tf, dsp, gout_zero, tf.capture_timestamp_us, 0.0f, scfg, skin);
        if ((t2 % 4) == 3) {
            const double h_ang = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_HEAD]);
            std::printf(
                "    rFrame=%2d t2=%2d cam_seq=%u cam_yaw=%.3f deg -> rendered_head_angle=%.4f deg spring_ms=%.4f\n",
                t2 / 4, t2, cam_seq, yaw_deg, h_ang, skin.spring_sim_ms);
        }
    }

    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}

namespace {

struct LoadedVrmFixture {
    aether::rhi::RhiDevice* rhi{nullptr};
    std::unique_ptr<aether::renderer::GpuAvatar> avatar;
    aether::animation::RestSkeletonMetrics rm{};
    bool ok{false};

    ~LoadedVrmFixture() {
        avatar.reset();
        if (rhi != nullptr) {
            rhi->destroy();
            rhi = nullptr;
        }
        aether_runtime_shutdown();
    }
};

bool load_real_vrm_fixture(LoadedVrmFixture& fx) {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        return false;
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    if (aether_runtime_init(&rt) != AETHER_OK) {
        return false;
    }
    aether::rhi::RhiDeviceConfig cfg{};
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    if (!dev_res.has_value()) {
        return false;
    }
    fx.rhi = dev_res.value();
    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    job.start_load(*fx.rhi, vrm_path.string());
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    if (job.state() != aether::renderer::AssetJobState::ReadyForGpu) {
        return false;
    }
    auto avatar_res = job.finalize_gpu_upload(*fx.rhi);
    if (!avatar_res.has_value()) {
        return false;
    }
    fx.avatar = std::move(avatar_res.value());
    fx.rm = build_vrm_rest_metrics(*fx.avatar);
    fx.ok = true;
    return true;
}

void rotate_point_euler(float yaw, float pitch, float roll, const float p[3], float out[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float x1 = p[0] * cr - p[1] * sr;
    const float y1 = p[0] * sr + p[1] * cr;
    const float z1 = p[2];

    const float x2 = x1;
    const float y2 = y1 * cp - z1 * sp;
    const float z2 = y1 * sp + z1 * cp;

    out[0] = x2 * cy + z2 * sy;
    out[1] = y2;
    out[2] = -x2 * sy + z2 * cy;
}

std::array<float, 478 * 3> project_synthetic_face_landmarks(float yaw_rad, float pitch_rad, float roll_rad,
                                                            float ear_scale_l, float ear_scale_r, float mouth_open_m,
                                                            float mouth_corner_dx_m,
                                                            const aether::tracking::RoiRectNorm& roi, uint32_t cam_w,
                                                            uint32_t cam_h, std::mt19937* opt_rng = nullptr,
                                                            float gaussian_px_std = 0.0f) {
    using namespace aether::tracking;
    constexpr float kFocalPx = 900.0f;
    std::array<float, 478 * 3> lm{};
    std::normal_distribution<float> px_noise(0.0f, std::max(gaussian_px_std, 1e-6f));
    const float cy_l = 0.5f * (kCanonicalFaceVerticesM[159][1] + kCanonicalFaceVerticesM[145][1]);
    const float cy_r = 0.5f * (kCanonicalFaceVerticesM[386][1] + kCanonicalFaceVerticesM[374][1]);

    for (uint32_t i = 0; i < 478U; ++i) {
        const float* base = (i < kCanonicalFaceVertexCount)
                                ? kCanonicalFaceVerticesM[i]
                                : ((i < 473U) ? kCanonicalFaceVerticesM[159] : kCanonicalFaceVerticesM[386]);
        float p3[3]{base[0], base[1], base[2]};
        if (i == 159U || i == 145U || i == 160U || i == 144U || i == 158U || i == 153U) {
            p3[1] = cy_l + (p3[1] - cy_l) * ear_scale_l;
        } else if (i == 386U || i == 374U || i == 387U || i == 373U || i == 385U || i == 380U) {
            p3[1] = cy_r + (p3[1] - cy_r) * ear_scale_r;
        } else if (i == 13U || i == 312U || i == 82U) {
            p3[1] += mouth_open_m * 0.45f;
        } else if (i == 14U || i == 317U || i == 87U) {
            p3[1] -= mouth_open_m * 0.55f;
        } else if (i == 61U || i == 78U) {
            p3[0] -= mouth_corner_dx_m;
        } else if (i == 291U || i == 308U) {
            p3[0] += mouth_corner_dx_m;
        }
        float rot[3]{};
        rotate_point_euler(yaw_rad, pitch_rad, roll_rad, p3, rot);
        const float z = -0.55f + rot[2];
        float u_px =
            (0.5f + (rot[0] * kFocalPx) / (std::max(-z, 0.2f) * static_cast<float>(cam_w))) * static_cast<float>(cam_w);
        float v_px =
            (0.5f - (rot[1] * kFocalPx) / (std::max(-z, 0.2f) * static_cast<float>(cam_h))) * static_cast<float>(cam_h);
        if (opt_rng != nullptr && gaussian_px_std > 0.0f) {
            u_px += px_noise(*opt_rng);
            v_px += px_noise(*opt_rng);
        }
        const float u_cam = u_px / static_cast<float>(cam_w);
        const float v_cam = v_px / static_cast<float>(cam_h);
        lm[i * 3 + 0] = ((u_cam - roi.cx) / roi.width + 0.5f) * 256.0f;
        lm[i * 3 + 1] = ((v_cam - roi.cy) / roi.height + 0.5f) * 256.0f;
        lm[i * 3 + 2] = -rot[2] * 100.0f;
    }
    return lm;
}

} // namespace

TEST_CASE("Task 6.1: 8590256991748008892.vrm idle stability under +-1.5 px Gaussian landmark noise",
          "[vrm][task6][idle]") {
    LoadedVrmFixture fx;
    if (!load_real_vrm_fixture(fx)) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    constexpr uint32_t kCamW = 1280U;
    constexpr uint32_t kCamH = 720U;
    const aether::tracking::RoiRectNorm roi{0.50f, 0.50f, 0.45f, 0.45f, 0.0f, true};

    aether::tracking::LandmarkPoseSolver solver;
    solver.reset();
    AetherTrackingFrame tf{};
    for (int f = 0; f < 25; ++f) {
        const auto lm = project_synthetic_face_landmarks(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, roi, kCamW, kCamH);
        const uint64_t ts = static_cast<uint64_t>(f + 1) * 33333ULL;
        solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, ts, ts + 2000ULL, aether::tracking::EpTier::Cpu,
                                       static_cast<uint32_t>(f + 1), false, tf, kCamW, kCamH);
    }
    REQUIRE(solver.neutral_calibrated());

    using namespace aether::animation;
    SimEngine engine;
    engine.reset(fx.rm);
    SimEngineConfig scfg{};
    scfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    SkinningFrame skin{};

    AudioLipSyncFrame silent_audio{};
    silent_audio.device_available = true;
    silent_audio.assist_active = true;
    silent_audio.vad = 0.0f;
    silent_audio.audio_open_y = 0.0f;

    std::mt19937 rng(20261115U);
    float max_head_jitter_deg = 0.0f;
    float max_head_pos_vel = 0.0f;
    float max_blink = 0.0f;
    float max_vowel = 0.0f;
    float max_spring_tip_vel = 0.0f;
    float max_spring_sim_vel = 0.0f;
    float max_spring_local_vel = 0.0f;
    Quat prev_head_rot = engine.bone_rot_ws()[AETHER_BONE_HEAD];
    Vec3 prev_head_pos = engine.bone_pos_ws()[AETHER_BONE_HEAD];
    std::array<Vec3, aether::animation::kMaxSpringJoints> prev_spring_pos{};
    std::array<Quat, aether::animation::kMaxSpringJoints> prev_spring_local_rot{};
    const uint32_t spring_joints = engine.spring_simulator().joint_count();
    REQUIRE(spring_joints >= 34U);

    // 180 simulation frames @ 60 Hz with 30 Hz noisy webcam packets (+-1.5 px 3-sigma -> std 0.50 px)
    for (int f = 0; f < 180; ++f) {
        if ((f % 2) == 0) {
            const auto lm_noisy = project_synthetic_face_landmarks(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, roi, kCamW,
                                                                   kCamH, &rng, 0.50f);
            const uint32_t seq = static_cast<uint32_t>(26 + f / 2);
            const uint64_t ts = static_cast<uint64_t>(26 + f) * 16667ULL;
            solver.populate_tracking_frame(roi, lm_noisy.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false,
                                           nullptr, nullptr, nullptr, ts, ts + 2000ULL, aether::tracking::EpTier::Cpu,
                                           seq, false, tf, kCamW, kCamH);
        }
        const uint64_t tick_us = static_cast<uint64_t>(26 + f) * 16667ULL + 4000ULL;
        engine.tick(tf, dsp, gout, tick_us, -1.0f, scfg, skin, &silent_audio, 0.65f);

        const Quat hrot = engine.bone_rot_ws()[AETHER_BONE_HEAD];
        const Vec3 hpos = engine.bone_pos_ws()[AETHER_BONE_HEAD];
        if (f >= 60) {
            max_head_jitter_deg = std::max(max_head_jitter_deg, angle_between(hrot, prev_head_rot) * kRadToDeg);
            max_head_pos_vel = std::max(max_head_pos_vel, length(hpos - prev_head_pos) * 60.0f);
            max_blink = std::max({max_blink, skin.morph_weights[0], skin.morph_weights[7]});
            for (size_t v = 52U; v <= 56U; ++v) {
                max_vowel = std::max(max_vowel, skin.morph_weights[v]);
            }
            for (uint32_t j = 0; j < spring_joints; ++j) {
                const uint32_t b = 64U + j;
                const float vel = length(engine.bone_pos_ws()[b] - prev_spring_pos[j]) * 60.0f;
                max_spring_tip_vel = std::max(max_spring_tip_vel, vel);
                max_spring_sim_vel = std::max(max_spring_sim_vel, engine.spring_simulator().joint_velocity_mps(j));
                const uint32_t hb = (j < fx.rm.custom_spring_count)
                                        ? static_cast<uint32_t>(fx.rm.custom_springs[j].skin_base_humanoid)
                                        : AETHER_BONE_HEAD;
                const Quat p_rot = (hb < AETHER_HUMANOID_BONE_COUNT) ? engine.bone_rot_ws()[hb] : Quat::identity();
                const Quat rel_rot = normalize(conjugate(p_rot) * engine.bone_rot_ws()[b]);
                const float d_rad = angle_between(rel_rot, prev_spring_local_rot[j]);
                const float blen = (j < fx.rm.custom_spring_count) ? fx.rm.custom_springs[j].bone_length : 0.10f;
                max_spring_local_vel = std::max(max_spring_local_vel, d_rad * blen * 60.0f);
            }
        }
        prev_head_rot = hrot;
        prev_head_pos = hpos;
        for (uint32_t j = 0; j < spring_joints; ++j) {
            const uint32_t b = 64U + j;
            prev_spring_pos[j] = engine.bone_pos_ws()[b];
            const uint32_t hb = (j < fx.rm.custom_spring_count)
                                    ? static_cast<uint32_t>(fx.rm.custom_springs[j].skin_base_humanoid)
                                    : AETHER_BONE_HEAD;
            const Quat p_rot = (hb < AETHER_HUMANOID_BONE_COUNT) ? engine.bone_rot_ws()[hb] : Quat::identity();
            prev_spring_local_rot[j] = normalize(conjugate(p_rot) * engine.bone_rot_ws()[b]);
        }
    }

    std::printf("[TASK6-IDLE] max_head_jitter=%.6f deg head_pos_vel=%.2e m/s max_blink=%.5f max_vowel=%.5f "
                "bone_pos_vel=%.2e sim_vel=%.2e local_swing_vel=%.2e m/s\n",
                max_head_jitter_deg, max_head_pos_vel, max_blink, max_vowel, max_spring_tip_vel, max_spring_sim_vel,
                max_spring_local_vel);
    CHECK(max_head_jitter_deg <= 0.002f);
    CHECK(max_blink == 0.0f);
    CHECK(max_vowel == 0.0f);
    CHECK(max_spring_sim_vel < 1e-5f);
    CHECK(max_spring_local_vel < 1e-4f);
}

TEST_CASE("Task 6.2: Warudo Ballistic Blink State Machine, Blink Link, Pitch Comp, and Saccadic Gaze",
          "[vrm][task6][blink]") {
    using namespace aether::tracking;
    BlinkGazeStabilizer bg;
    bg.reset();

    // 1. Calibrate open baseline over 30 frames (EAR = 0.28, slight normal variance)
    BlinkGazeInput in{};
    in.dt_s = 1.0f / 30.0f;
    in.neutral_ear_left_hint = 0.28f;
    in.neutral_ear_right_hint = 0.28f;
    for (int i = 0; i < 30; ++i) {
        in.ear_left = 0.28f + ((i % 2 == 0) ? 0.004f : -0.004f);
        in.ear_right = 0.28f + ((i % 2 == 0) ? -0.004f : 0.004f);
        in.gaze_gx_left = in.gaze_gx_right = 0.01f;
        in.gaze_gy_left = in.gaze_gy_right = -0.01f;
        const auto out = bg.update(in);
        CHECK(out.blink_left == 0.0f);
        CHECK(out.blink_right == 0.0f);
        CHECK(out.gaze_gx == 0.0f); // within 0.035 saccadic deadband
        CHECK(out.gaze_gy == 0.0f);
    }

    // 2. Fast 3-camera-frame blink @ 30 Hz (100 ms closed) stepped in 15 ms resolution via dt_s or 30 Hz:
    // verify full 1.0 closure (never hovers halfway) and clean return to 0.0, plus gaze recentering during blink.
    bg.reset();
    in.dt_s = 0.015f;
    for (int i = 0; i < 20; ++i) {
        in.ear_left = in.ear_right = 0.28f;
        in.gaze_gx_left = in.gaze_gx_right = 0.50f;
        in.gaze_gy_left = in.gaze_gy_right = 0.0f;
        (void)bg.update(in);
    }
    const float gaze_before_blink = bg.update(in).gaze_gx;
    CHECK(gaze_before_blink > 0.30f);

    float peak_blink = 0.0f;
    float mid_gaze_during_blink = gaze_before_blink;
    for (int i = 0; i < 6; ++i) { // 90 ms eye closure
        in.ear_left = in.ear_right = 0.09f;
        const auto out = bg.update(in);
        peak_blink = std::max(peak_blink, out.blink_left);
        mid_gaze_during_blink = std::min(mid_gaze_during_blink, std::abs(out.gaze_gx));
        CHECK(out.blink_left == out.blink_right); // synchronized by Blink Link
    }
    CHECK(peak_blink >= 0.999f);
    CHECK(mid_gaze_during_blink < gaze_before_blink * 0.7f);

    float end_blink = 1.0f;
    for (int i = 0; i < 16; ++i) { // reopen
        in.ear_left = in.ear_right = 0.28f;
        end_blink = bg.update(in).blink_left;
    }
    CHECK(end_blink == 0.0f);

    // 3. Single-frame asymmetry (1-frame right-eye drop noise) stays linked without one-eye flutter
    in.dt_s = 1.0f / 30.0f;
    in.ear_left = 0.28f;
    in.ear_right = 0.09f;
    const auto noise_sync = bg.update(in);
    CHECK(std::abs(noise_sync.blink_left - noise_sync.blink_right) < 1e-5f);
    CHECK_FALSE(noise_sync.wink_active);

    // Clear noise & verify +25 deg downward pitch compensation prevents false half-blinks
    in.ear_left = in.ear_right = 0.28f;
    for (int i = 0; i < 8; ++i) {
        (void)bg.update(in);
    }
    in.pitch_down_rad = 25.0f * aether::animation::kDegToRad;
    in.ear_left = in.ear_right = 0.225f; // geometrically narrowed by looking down at keyboard
    for (int i = 0; i < 5; ++i) {
        const auto pitched = bg.update(in);
        CHECK(pitched.blink_left == 0.0f);
        CHECK(pitched.blink_right == 0.0f);
    }

    // 4. Deliberate sustained one-eye wink (> 120 ms with abs(Yaw) < 15 deg) separates L/R
    in.pitch_down_rad = 0.0f;
    in.head_yaw_rad = 0.0f;
    in.ear_left = in.ear_right = 0.28f;
    for (int i = 0; i < 6; ++i) {
        (void)bg.update(in);
    }
    BlinkGazeOutput wink_out{};
    for (int i = 0; i < 7; ++i) { // 7 * 33.3 ms = 233 ms > 120 ms
        in.ear_left = 0.08f;
        in.ear_right = 0.28f;
        wink_out = bg.update(in);
    }
    CHECK(wink_out.wink_active);
    CHECK(wink_out.blink_left >= 0.99f);
    CHECK(wink_out.blink_right == 0.0f);
}

TEST_CASE("Task 6.3: Hybrid WASAPI Audio-Visual Lip-Sync Formants, Silence Lock, and Smile/Yawn Exceptions",
          "[vrm][task6][audio]") {
    using namespace aether::animation;
    using namespace aether::tracking;

    auto gen_tone = [](float freq_hz, float amp, size_t n, uint32_t sr) {
        std::vector<float> buf(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            buf[i] = amp * std::sin(kTwoPi * freq_hz * static_cast<float>(i) / static_cast<float>(sr));
        }
        return buf;
    };

    AudioLipSyncProcessor dsp_proc;
    dsp_proc.reset();
    constexpr uint32_t kSr = 48000U;
    constexpr size_t kBlock = 1024U;

    const auto run_tone = [&](float freq_hz) {
        dsp_proc.reset();
        const auto pcm = gen_tone(freq_hz, 0.35f, kBlock, kSr);
        AudioLipSyncFrame f{};
        for (int b = 0; b < 8; ++b) {
            f = dsp_proc.process_pcm_block(pcm.data(), kBlock, kSr, 1.0f, -52.0f,
                                           static_cast<uint64_t>(b + 1) * 21333ULL);
        }
        return f;
    };

    const AudioLipSyncFrame low_u = run_tone(350.0f); // B1 250-650 Hz (F1 / U,O)
    const AudioLipSyncFrame mid_a = run_tone(950.0f); // B2 700-1600 Hz (A)
    const AudioLipSyncFrame hi_i = run_tone(2400.0f); // B3 1700-3200 Hz (F2 / I,E)

    CHECK(low_u.vad > 0.70f);
    CHECK(low_u.audio_open_y > 0.25f);
    CHECK((low_u.vowel_probs[2] + low_u.vowel_probs[4]) > (low_u.vowel_probs[1] + low_u.vowel_probs[3]));
    CHECK(mid_a.vad > 0.70f);
    CHECK(mid_a.vowel_probs[0] > mid_a.vowel_probs[1]);
    CHECK(hi_i.vad > 0.70f);
    CHECK((hi_i.vowel_probs[1] + hi_i.vowel_probs[3]) > (hi_i.vowel_probs[2] + hi_i.vowel_probs[4]));
    const float sum_hi =
        hi_i.vowel_probs[0] + hi_i.vowel_probs[1] + hi_i.vowel_probs[2] + hi_i.vowel_probs[3] + hi_i.vowel_probs[4];
    CHECK(sum_hi <= 1.001f);

    // Hybrid ExpressionMixer fusion checks:
    ExpressionMixer mixer;
    mixer.reset();
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    std::array<float, kMaxMorphs> out{};
    float owner_blend = 0.0f;

    // 1. Silence Lock: audio VAD=0 + webcam mouth tremor 0.15 (< 0.22) must crush to 0.0
    AudioLipSyncFrame silence{};
    silence.device_available = true;
    silence.assist_active = true;
    silence.vad = 0.0f;
    std::array<float, 52> cam_tremor{};
    cam_tremor[17] = 0.15f; // jawOpen tremor
    const float cam_v_tremor[5] = {0.15f, 0.0f, 0.0f, 0.0f, 0.0f};
    Live2DParameters l2d_tremor{};
    l2d_tremor.param_mouth_open_y = 0.15f;
    for (int f = 0; f < 30; ++f) {
        mixer.update(cam_tremor.data(), dsp, gout, 1.0f / 60.0f, out.data(), owner_blend, cam_v_tremor, true,
                     &l2d_tremor, nullptr, &silence, 0.65f);
    }
    CHECK(out[17] < 1e-4f);
    CHECK(out[52] < 1e-4f);

    // 2. Silent Wide Open Mouth (>= 0.25) opens despite VAD=0
    mixer.reset();
    std::array<float, 52> cam_yawn{};
    cam_yawn[17] = 0.65f;
    const float cam_v_yawn[5] = {0.65f, 0.0f, 0.0f, 0.0f, 0.0f};
    Live2DParameters l2d_yawn{};
    l2d_yawn.param_mouth_open_y = 0.65f;
    for (int f = 0; f < 20; ++f) {
        mixer.update(cam_yawn.data(), dsp, gout, 1.0f / 60.0f, out.data(), owner_blend, cam_v_yawn, true, &l2d_yawn,
                     nullptr, &silence, 0.65f);
    }
    CHECK(out[17] > 0.35f);
    CHECK(out[52] > 0.30f);

    // 3. Silent Closed-Lip Smile (MouthForm > +0.20) preserves smile while keeping vowels 0
    mixer.reset();
    std::array<float, 52> cam_smile{};
    cam_smile[23] = 0.80f;
    cam_smile[24] = 0.80f;
    Live2DParameters l2d_smile{};
    l2d_smile.param_mouth_open_y = 0.05f;
    l2d_smile.param_mouth_form = 0.80f;
    for (int f = 0; f < 20; ++f) {
        mixer.update(cam_smile.data(), dsp, gout, 1.0f / 60.0f, out.data(), owner_blend, nullptr, true, &l2d_smile,
                     nullptr, &silence, 0.65f);
    }
    CHECK(out[52] + out[53] + out[54] + out[55] + out[56] == 0.0f);
    CHECK(out[23] > 0.30f);
    CHECK(out[24] > 0.30f);
}

TEST_CASE("Task 6.4: 8590256991748008892.vrm morph mutual exclusivity & per-region influence clamping",
          "[vrm][task6][morphs]") {
    LoadedVrmFixture fx;
    if (!load_real_vrm_fixture(fx)) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    using namespace aether::animation;
    SimEngine engine;
    engine.reset(fx.rm);
    SimEngineConfig scfg{};
    scfg.visibility.enable_idle_breathing = false;
    AetherTrackingFrame tf{};
    tf.frame_sequence = 1U;
    tf.valid_flags = 0x01U;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        tf.joint_rotations_quat[b][3] = 1.0f;
    }
    tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
    tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    SkinningFrame skin{};
    engine.tick(tf, dsp, gout, 16667ULL, 0.0f, scfg, skin);

    std::array<float, AETHER_EXPRESSION_SLOT_COUNT> expr{};
    // Full blink + max Joy + Surprised + Brows + stacked vowels to stress region caps and eyelid guard
    expr[0] = 1.0f;  // Blink_L
    expr[7] = 1.0f;  // Blink_R
    expr[41] = 0.8f; // BrowDownL
    expr[43] = 0.8f; // BrowInnerUp
    expr[44] = 0.8f; // BrowOuterUpL
    expr[52] = 0.7f; // A
    expr[53] = 0.7f; // I
    expr[54] = 0.7f; // U
    expr[55] = 0.7f; // E
    expr[56] = 0.7f; // O
    expr[57] = 1.0f; // Joy
    expr[60] = 1.0f; // Surprised
    fx.avatar->update_pose(0U, 1U, skin.bone_matrices, skin.bone_count, expr.data(), static_cast<uint32_t>(expr.size()),
                           true);

    using Region = aether::renderer::GpuAvatar::MorphRegion;
    double sum_eyel = 0.0;
    double sum_eyer = 0.0;
    double sum_brow = 0.0;
    double sum_mouth = 0.0;
    float max_eyelid_affecting_emotion = 0.0f;
    for (size_t mi = 0; mi < fx.avatar->morph_weights_scratch.size(); ++mi) {
        const float w = fx.avatar->morph_weights_scratch[mi];
        const auto& meta = fx.avatar->morph_meta[mi];
        if (meta.affects_eyelids && !meta.is_blink) {
            max_eyelid_affecting_emotion = std::max(max_eyelid_affecting_emotion, w);
        }
        switch (meta.region) {
        case Region::EyeL:
            sum_eyel += w;
            break;
        case Region::EyeR:
            sum_eyer += w;
            break;
        case Region::Brow:
            sum_brow += w;
            break;
        case Region::Mouth:
            sum_mouth += w;
            break;
        default:
            break;
        }
    }
    CHECK(max_eyelid_affecting_emotion == 0.0f);
    CHECK(sum_eyel <= 1.001);
    CHECK(sum_eyer <= 1.001);
    CHECK(sum_brow <= 0.751);
    CHECK(sum_mouth <= 1.001);

    // Partial blink (0.5) attenuates overlapping emotion morphs proportionally (<= 0.5)
    expr[0] = 0.5f;
    expr[7] = 0.5f;
    fx.avatar->update_pose(1U, 2U, skin.bone_matrices, skin.bone_count, expr.data(), static_cast<uint32_t>(expr.size()),
                           true);
    float partial_eyelid_emotion = 0.0f;
    for (size_t mi = 0; mi < fx.avatar->morph_weights_scratch.size(); ++mi) {
        const auto& meta = fx.avatar->morph_meta[mi];
        if (meta.affects_eyelids && !meta.is_blink) {
            partial_eyelid_emotion = std::max(partial_eyelid_emotion, fx.avatar->morph_weights_scratch[mi]);
        }
    }
    CHECK(partial_eyelid_emotion <= 0.501f);
}

TEST_CASE("Task 6.5: Realistic 30 Hz camera -> 240 Hz tick -> 60 Hz pacing has zero 14 ms freeze stalls",
          "[vrm][task6][pacing]") {
    LoadedVrmFixture fx;
    if (!load_real_vrm_fixture(fx)) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    using namespace aether::animation;
    SimEngine engine;
    engine.reset(fx.rm);
    SimEngineConfig scfg{};
    scfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    SkinningFrame skin{};

    std::vector<double> t2_head_deg;
    std::vector<double> render_head_deg;
    t2_head_deg.reserve(144);
    render_head_deg.reserve(36);

    // 0.6 s continuous 30 deg/s yaw turn driven by 30 Hz camera packets (new packet every 8 T2 ticks)
    // with inference_complete_us fixed per camera packet so intermediate 240 Hz ticks exceed 14 ms.
    AetherTrackingFrame tf{};
    uint64_t packet_ts_us = 0ULL;
    for (int t2 = 0; t2 < 144; ++t2) {
        const uint64_t now_us = static_cast<uint64_t>(t2 + 1) * 4167ULL;
        if ((t2 % 8) == 0) {
            const uint32_t seq = static_cast<uint32_t>(t2 / 8 + 1);
            packet_ts_us = now_us;
            const float yaw_rad = 30.0f * (static_cast<float>(seq - 1U) / 30.0f) * kDegToRad;
            const Quat q = from_euler_xyz(0.0f, yaw_rad, 0.0f);
            tf = {};
            tf.frame_sequence = seq;
            tf.capture_timestamp_us = packet_ts_us;
            tf.inference_complete_us = packet_ts_us;
            tf.valid_flags = 0x01U;
            for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                tf.joint_rotations_quat[b][3] = 1.0f;
            }
            tf.joint_rotations_quat[AETHER_BONE_HEAD][0] = q.x;
            tf.joint_rotations_quat[AETHER_BONE_HEAD][1] = q.y;
            tf.joint_rotations_quat[AETHER_BONE_HEAD][2] = q.z;
            tf.joint_rotations_quat[AETHER_BONE_HEAD][3] = q.w;
            tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.50f;
            tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.99f;
        }
        engine.tick(tf, dsp, gout, now_us, -1.0f, scfg, skin);
        const double ang = mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_HEAD]);
        t2_head_deg.push_back(ang);
        if ((t2 % 4) == 3) {
            render_head_deg.push_back(ang);
        }
    }

    // After visibility settle (t2 >= 48 = 0.2 s), every 240 Hz tick must advance monotonically with zero stalls
    double min_step_t2 = 1e9;
    double max_accel_r = 0.0;
    for (size_t i = 48; i < t2_head_deg.size(); ++i) {
        const double step = t2_head_deg[i] - t2_head_deg[i - 1];
        min_step_t2 = std::min(min_step_t2, step);
        CHECK(step > 0.001); // never stalls on > 14 ms tail between 30 Hz camera packets
    }
    for (size_t i = 10; i < render_head_deg.size(); ++i) {
        const double d1 = render_head_deg[i] - render_head_deg[i - 1];
        const double d0 = render_head_deg[i - 1] - render_head_deg[i - 2];
        max_accel_r = std::max(max_accel_r, std::abs(d1 - d0));
        CHECK(d1 > 0.0);
    }
    std::printf(
        "[TASK6-PACING] min_240hz_step=%.5f deg max_60hz_2nd_diff=%.5f deg final_head=%.3f deg "
        "(neck=%.3f spine=%.3f chest=%.3f tf_last=%.3f)\n",
        min_step_t2, max_accel_r, t2_head_deg.back(), mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_NECK]),
        mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_SPINE]),
        mat3x4_delta_angle_deg(skin.bone_matrices[AETHER_BONE_CHEST]),
        angle_between(Quat{tf.joint_rotations_quat[AETHER_BONE_HEAD][0], tf.joint_rotations_quat[AETHER_BONE_HEAD][1],
                           tf.joint_rotations_quat[AETHER_BONE_HEAD][2], tf.joint_rotations_quat[AETHER_BONE_HEAD][3]},
                      Quat::identity()) *
            kRadToDeg);
    CHECK(max_accel_r < 0.08);
    CHECK(t2_head_deg.back() > 12.5);
}

TEST_CASE("Real VRM 8590256991748008892.vrm Camera SmoothDamp transition & Self-Collision-Free Hand IK",
          "[vrm][presets][hands][collision][task6]") {
    LoadedVrmFixture fx;
    if (!load_real_vrm_fixture(fx)) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    // 1) CameraTransitionState SmoothDamp over 0.22s on 8590256991748008892.vrm bone heights + cancel guard
    {
        using namespace aether::renderer;
        const PreviewCamera base = fit_camera_to_avatar(*fx.avatar);
        const uint16_t head_j = fx.avatar->desc.humanoid_to_joint[AETHER_BONE_HEAD];
        const uint16_t chest_j = fx.avatar->desc.humanoid_to_joint[AETHER_BONE_CHEST];
        const float head_y =
            (head_j < fx.avatar->joint_rest_world_pos.size()) ? fx.avatar->joint_rest_world_pos[head_j][1] : 1.46f;
        const float chest_y =
            (chest_j < fx.avatar->joint_rest_world_pos.size()) ? fx.avatar->joint_rest_world_pos[chest_j][1] : 1.20f;
        CHECK(head_y > chest_y);

        PreviewCamera cam = base;
        PreviewCamera goal = base;
        goal.target_y = head_y - 0.04f;
        goal.distance = std::max(0.55f, (head_y - chest_y) * 2.8f);
        goal.yaw_rad = 0.05f;

        CameraTransitionState tr{};
        tr.start(cam, goal, 0.22f);
        REQUIRE(tr.active);
        for (int f = 0; f < 30; ++f) {
            (void)tr.step(1.0f / 60.0f, cam);
        }
        CHECK_FALSE(tr.active);
        CHECK(std::abs(cam.target_y - goal.target_y) < 1e-3f);
        CHECK(std::abs(cam.distance - goal.distance) < 2e-3f);

        // Starting another transition and cancelling midway freezes cleanly at current pose
        tr.start(cam, base, 0.22f);
        (void)tr.step(1.0f / 60.0f, cam);
        const PreviewCamera mid = cam;
        tr.cancel();
        CHECK_FALSE(tr.active);
        CHECK_FALSE(tr.step(1.0f / 60.0f, cam));
        CHECK(std::abs(cam.distance - mid.distance) < 1e-6f);
    }

    // 2) Sweep overlapping hand IK targets straight into 8590256991748008892.vrm torso/head/opposite wrist
    using namespace aether::animation;
    SimEngine engine;
    engine.reset(fx.rm);
    SimEngineConfig scfg{};
    scfg.visibility.enable_idle_breathing = false;
    scfg.visibility.enable_hand_ik = true;
    AetherDspSyncPacket dsp{};
    AetherGraphOutputs gout{};
    SkinningFrame skin{};

    AetherTrackingFrame tf{};
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        tf.joint_rotations_quat[b][3] = 1.0f;
    }
    tf.valid_flags = 0x0FU;
    tf.upper_body_confidence = 0.96f;
    tf.left_hand_confidence = 0.96f;
    tf.right_hand_confidence = 0.96f;

    uint64_t tick = 1;
    // Settle initial frame so FK torso/head heights match loaded VRM exactly
    for (int f = 0; f < 10; ++f) {
        tf.frame_sequence = static_cast<uint32_t>(tick);
        tf.capture_timestamp_us = tick++ * 16667ULL;
        engine.tick(tf, dsp, gout, tf.capture_timestamp_us, 0.0f, scfg, skin);
    }
    const Vec3 vrm_chest = engine.bone_pos_ws()[AETHER_BONE_CHEST];
    const Vec3 vrm_head = engine.bone_pos_ws()[AETHER_BONE_HEAD];

    float min_sd_vrm = 1e9f;
    const std::array<std::pair<Vec3, Vec3>, 3> vrm_sweeps = {{
        {vrm_chest, vrm_chest}, // Straight through VRM chest capsule (+3.5 cm margin)
        {vrm_head, vrm_head},   // Straight through VRM neck/head capsule
        {{vrm_chest.x - 0.03f, vrm_chest.y, vrm_chest.z + 0.14f},
         {vrm_chest.x + 0.03f, vrm_chest.y, vrm_chest.z + 0.14f}}, // Bilateral hand overlap
    }};
    for (const auto& sw : vrm_sweeps) {
        tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][0] = sw.first.x;
        tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][1] = sw.first.y;
        tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][2] = sw.first.z;
        tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][3] = 0.96f;
        tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][0] = sw.second.x;
        tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][1] = sw.second.y;
        tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][2] = sw.second.z;
        tf.joint_positions_ws[AETHER_BONE_RIGHT_HAND][3] = 0.96f;
        for (int s = 0; s < 25; ++s) {
            tf.frame_sequence = static_cast<uint32_t>(tick);
            tf.capture_timestamp_us = tick++ * 16667ULL;
            engine.tick(tf, dsp, gout, tf.capture_timestamp_us, 0.0f, scfg, skin);
            std::array<Vec3, AETHER_HUMANOID_BONE_COUNT> fk_ws{};
            for (size_t bi = 0; bi < AETHER_HUMANOID_BONE_COUNT; ++bi) {
                fk_ws[bi] = engine.bone_pos_ws()[bi];
            }
            BodySelfCollisionField field;
            field.build_from_fk(fk_ws, false, false);
            min_sd_vrm = std::min(min_sd_vrm, field.min_signed_distance(fk_ws, true, true));
        }
    }
    CHECK(min_sd_vrm >= -1e-4f);

    // Toggle hand tracking OFF -> smoothly transitions arms back toward relaxed A-Pose within 0.6s
    tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][0] = vrm_chest.x + 0.30f;
    tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][1] = vrm_head.y - 0.05f;
    tf.joint_positions_ws[AETHER_BONE_LEFT_HAND][2] = vrm_chest.z + 0.25f;
    for (int f = 0; f < 25; ++f) {
        tf.frame_sequence = static_cast<uint32_t>(tick);
        tf.capture_timestamp_us = tick++ * 16667ULL;
        engine.tick(tf, dsp, gout, tf.capture_timestamp_us, 0.0f, scfg, skin);
    }
    const float raised_y = engine.bone_pos_ws()[AETHER_BONE_LEFT_HAND].y;
    scfg.visibility.enable_hand_ik = false;
    for (int f = 0; f < 36; ++f) {
        tf.frame_sequence = static_cast<uint32_t>(tick);
        tf.capture_timestamp_us = tick++ * 16667ULL;
        engine.tick(tf, dsp, gout, tf.capture_timestamp_us, 0.0f, scfg, skin);
    }
    const float settled_y = engine.bone_pos_ws()[AETHER_BONE_LEFT_HAND].y;
    CHECK(settled_y < raised_y - 0.10f);
}

TEST_CASE("Phase 0/4 Quantitative Pixel & G-Buffer Probe on 8590256991748008892.vrm", "[vrm][probe]") {
    const auto vrm_path = find_user_vrm_path();
    if (vrm_path.empty()) {
        SKIP("Real user VRM 8590256991748008892.vrm not found in Downloads");
    }

    aether::HotPathGuard::install();
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    AetherRuntimeConfig rt{};
    rt.struct_size = static_cast<uint32_t>(sizeof(rt));
    REQUIRE(aether_runtime_init(&rt) == AETHER_OK);

    aether::rhi::RhiDeviceConfig cfg{};
    cfg.enable_validation = false;
    cfg.require_validation = false;
    cfg.headless = true;
    auto dev_res = aether::rhi::RhiDevice::create(cfg);
    REQUIRE(dev_res.has_value());
    aether::rhi::RhiDevice* rhi = dev_res.value();

    aether::renderer::AssetJob job;
    aether::HighResTimer timer;
    const uint32_t load_flags =
        aether::renderer::AssetJob::kDefaultLoadFlags & ~static_cast<uint32_t>(AETHER_LOAD_FLAG_TEXTURE_CACHE);
    job.start_load(*rhi, vrm_path.string(), load_flags);
    const uint64_t deadline_us = aether::now_us() + 30000000ULL;
    while (job.state() == aether::renderer::AssetJobState::Loading && aether::now_us() < deadline_us) {
        timer.sleep_us(500);
    }
    REQUIRE(job.state() == aether::renderer::AssetJobState::ReadyForGpu);
    auto avatar_res = job.finalize_gpu_upload(*rhi);
    REQUIRE(avatar_res.has_value());
    auto avatar = std::move(avatar_res.value());

    constexpr uint32_t kW = 1280;
    constexpr uint32_t kH = 720;
    aether::renderer::RendererConfig rcfg{};
    rcfg.width = kW;
    rcfg.height = kH;
    rcfg.enable_rt = rhi->rt_supported();
    aether::renderer::NprRenderer renderer;
    REQUIRE(renderer.init(*rhi, rcfg).has_value());
    REQUIRE(renderer.bind_avatar(avatar.get()).has_value());

    // Pose avatar into neutral upright A-pose matching real_vrm_verification_test
    const aether::animation::RestSkeletonMetrics rm = build_vrm_rest_metrics(*avatar);
    aether::animation::SimEngine engine;
    engine.reset(rm);
    aether::animation::SimEngineConfig sim_cfg{};
    sim_cfg.visibility.enable_idle_breathing = false;
    AetherDspSyncPacket dsp{};
    aether::animation::SkinningFrame skin{};
    uint64_t sim_t_us = 1000000ULL;
    AetherHostConfig hcfg{};
    hcfg.struct_size = static_cast<uint32_t>(sizeof(hcfg));
    hcfg.worker_threads = 2;
    AetherHostHandle graph_host = nullptr;
    REQUIRE(aether_host_create(&hcfg, &graph_host) == AETHER_OK);
    for (int f = 0; f < 60; ++f) {
        AetherTrackingFrame tf{};
        tf.frame_sequence = static_cast<uint32_t>(f + 1);
        tf.capture_timestamp_us = sim_t_us;
        tf.inference_complete_us = sim_t_us;
        tf.valid_flags = 0x01U | 0x08U;
        tf.upper_body_confidence = 0.95f;
        for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
            tf.joint_rotations_quat[b][3] = 1.0f;
        }
        tf.joint_positions_ws[AETHER_BONE_HIPS][0] = rm.rest_local_positions[AETHER_BONE_HIPS].x;
        tf.joint_positions_ws[AETHER_BONE_HIPS][1] = rm.rest_local_positions[AETHER_BONE_HIPS].y;
        tf.joint_positions_ws[AETHER_BONE_HIPS][2] = rm.rest_local_positions[AETHER_BONE_HIPS].z;
        tf.joint_positions_ws[AETHER_BONE_HIPS][3] = 0.95f;
        tf.joint_positions_ws[AETHER_BONE_SPINE][3] = 0.95f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][1] = 1.48f;
        tf.joint_positions_ws[AETHER_BONE_HEAD][3] = 0.98f;
        AetherGraphInputs gin{};
        gin.tracking = &tf;
        gin.now_us = sim_t_us;
        gin.dt_s = 1.0f / 60.0f;
        gin.tick_index = static_cast<uint32_t>(f);
        AetherGraphOutputs graph_out{};
        aether_set_thread_role(AETHER_THREAD_ROLE_T2);
        REQUIRE(aether_host_evaluate_active(graph_host, &gin, &graph_out) == AETHER_OK);
        aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
        engine.tick(tf, dsp, graph_out, sim_t_us, 0.0f, sim_cfg, skin);
        sim_t_us += 16667ULL;
    }
    aether_host_destroy(graph_host);
    avatar->update_pose(0, 1, skin.bone_matrices, skin.bone_count, skin.morph_weights, skin.morph_count, true);
    avatar->update_pose(1, 1, skin.bone_matrices, skin.bone_count, skin.morph_weights, skin.morph_count, true);

    // Camera framing face + neck + cardigan + inner shirt + ribbon + hair simultaneously
    const auto fit_cam = aether::renderer::fit_camera_to_avatar(*avatar);
    aether::renderer::PreviewCamera cam = fit_cam;
    cam.target_y = fit_cam.target_y + 0.36f;
    cam.yaw_rad = 0.22f;
    cam.pitch_rad = 0.04f;
    cam.distance = 0.72f;
    cam.fovy_rad = 0.56f;

    auto rig = aether::renderer::LightingRig::default_three_point();
    rig.lighting_mode = 2u; // 2.5D Path Tracing
    rig.bg_rgba[0] = 0.067f;
    rig.bg_rgba[1] = 0.075f;
    rig.bg_rgba[2] = 0.094f;
    rig.bg_rgba[3] = 1.0f;

    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    aether::renderer::GpuMat4 m_view{}, m_inv_view{}, m_proj{}, m_inv_proj{}, m_vp{};
    aether::renderer::detail::Mat4Vec3 m_eye{};
    aether::renderer::detail::build_camera_matrices(cam, aspect, m_view, m_inv_view, m_proj, m_inv_proj, m_vp, m_eye);

    float pivot_ws[3] = {cam.target_x, cam.target_y, cam.target_z};
    if (avatar->desc.head_joint < avatar->joint_anim_world_pos.size()) {
        const auto& hp = avatar->joint_anim_world_pos[avatar->desc.head_joint];
        pivot_ws[0] = hp[0];
        pivot_ws[1] = hp[1];
        pivot_ws[2] = hp[2];
    }
    const float pivot_vz =
        m_view.col0[2] * pivot_ws[0] + m_view.col1[2] * pivot_ws[1] + m_view.col2[2] * pivot_ws[2] + m_view.col3[2];

    auto project_point = [&](const float p[3], float z_flat_w, int& out_px, int& out_py, float& out_ndc_z) -> bool {
        const float vx = m_view.col0[0] * p[0] + m_view.col1[0] * p[1] + m_view.col2[0] * p[2] + m_view.col3[0];
        const float vy = m_view.col0[1] * p[0] + m_view.col1[1] * p[1] + m_view.col2[1] * p[2] + m_view.col3[1];
        const float vz = m_view.col0[2] * p[0] + m_view.col1[2] * p[1] + m_view.col2[2] * p[2] + m_view.col3[2];
        const float k = std::clamp(z_flat_w, 0.0f, 0.85f) * rig.global_live2d_flatness;
        const float fz = vz + (pivot_vz - vz) * k;
        const float cx = m_proj.col0[0] * vx + m_proj.col1[0] * vy + m_proj.col2[0] * fz + m_proj.col3[0];
        const float cy = m_proj.col0[1] * vx + m_proj.col1[1] * vy + m_proj.col2[1] * fz + m_proj.col3[1];
        const float cw = m_proj.col0[3] * vx + m_proj.col1[3] * vy + m_proj.col2[3] * fz + m_proj.col3[3];
        if (cw <= 0.05f) {
            return false;
        }
        const float cz_true = m_proj.col2[2] * vz + m_proj.col3[2];
        const float cw_true = m_proj.col2[3] * vz + m_proj.col3[3];
        out_ndc_z = cz_true / std::max(std::abs(cw_true), 1e-5f);
        const float ndc_x = cx / cw;
        const float ndc_y = cy / cw;
        const float u = ndc_x * 0.5f + 0.5f;
        const float v = 0.5f - ndc_y * 0.5f;
        out_px = static_cast<int>(u * static_cast<float>(kW));
        out_py = static_cast<int>(v * static_cast<float>(kH));
        return out_px >= 2 && out_px < static_cast<int>(kW) - 2 && out_py >= 2 && out_py < static_cast<int>(kH) - 2;
    };

    auto srgb_to_lin = [](float c) -> float {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };

    auto sample_raw_tex = [&](uint32_t tex_idx, float u, float v, uint8_t out_srgb[4], float out_lin[3]) {
        std::fill_n(out_srgb, 4, 255);
        std::fill_n(out_lin, 3, 1.0f);
        if (tex_idx >= avatar->texture_descs.size() || avatar->cpu_texture_pixels.empty()) {
            return;
        }
        const auto& td = avatar->texture_descs[tex_idx];
        if (td.format != AETHER_TEX_FORMAT_RGBA8_SRGB && td.format != AETHER_TEX_FORMAT_RGBA8_UNORM) {
            return;
        }
        const uint32_t w = std::max(1u, td.width);
        const uint32_t h = std::max(1u, td.height);
        const uint32_t x = static_cast<uint32_t>(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(w - 1));
        const uint32_t y = static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(h - 1));
        const uint64_t off = td.pixel_offset + (static_cast<uint64_t>(y) * w + x) * 4ULL;
        if (off + 4ULL <= avatar->cpu_texture_pixels.size()) {
            for (int c = 0; c < 4; ++c) {
                out_srgb[c] = avatar->cpu_texture_pixels[static_cast<size_t>(off + c)];
            }
            for (int c = 0; c < 3; ++c) {
                const float f = static_cast<float>(out_srgb[c]) / 255.0f;
                out_lin[c] = (td.format == AETHER_TEX_FORMAT_RGBA8_SRGB) ? srgb_to_lin(f) : f;
            }
        }
    };

    // Build exact CPU z-buffer across all non-transparent triangles so we pick genuinely visible pixels
    std::vector<float> zbuf(static_cast<size_t>(kW) * kH, 1e9f);
    std::vector<int32_t> tri_submesh(static_cast<size_t>(kW) * kH, -1);
    std::vector<uint32_t> tri_idx_buf(static_cast<size_t>(kW) * kH, 0u);
    std::vector<float> tri_u_buf(static_cast<size_t>(kW) * kH, 0.0f);
    std::vector<float> tri_v_buf(static_cast<size_t>(kW) * kH, 0.0f);
    std::vector<float> tri_w0_buf(static_cast<size_t>(kW) * kH, 1.0f);
    std::vector<float> tri_w1_buf(static_cast<size_t>(kW) * kH, 0.0f);
    for (size_t si = 0; si < avatar->submeshes.size(); ++si) {
        const auto& sm = avatar->submeshes[si];
        if ((sm.flags & AETHER_SUBMESH_FLAG_TRANSPARENT) != 0u) {
            continue;
        }
        for (uint32_t ii = 0; ii + 2u < sm.index_count; ii += 3u) {
            const uint32_t i0 = avatar->rest_indices[sm.index_offset + ii + 0u];
            const uint32_t i1 = avatar->rest_indices[sm.index_offset + ii + 1u];
            const uint32_t i2 = avatar->rest_indices[sm.index_offset + ii + 2u];
            int x0, y0, x1, y1, x2, y2;
            float z0, z1, z2;
            if (!project_point(avatar->skinned_vertices_scratch[i0].position,
                               avatar->skinned_vertices_scratch[i0].z_flatten_weight, x0, y0, z0) ||
                !project_point(avatar->skinned_vertices_scratch[i1].position,
                               avatar->skinned_vertices_scratch[i1].z_flatten_weight, x1, y1, z1) ||
                !project_point(avatar->skinned_vertices_scratch[i2].position,
                               avatar->skinned_vertices_scratch[i2].z_flatten_weight, x2, y2, z2)) {
                continue;
            }
            const int minx = std::max(0, std::min({x0, x1, x2}));
            const int maxx = std::min(static_cast<int>(kW) - 1, std::max({x0, x1, x2}));
            const int miny = std::max(0, std::min({y0, y1, y2}));
            const int maxy = std::min(static_cast<int>(kH) - 1, std::max({y0, y1, y2}));
            const float area = static_cast<float>((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
            if (std::abs(area) < 0.5f) {
                continue;
            }
            const float inv_area = 1.0f / area;
            for (int py = miny; py <= maxy; ++py) {
                for (int px = minx; px <= maxx; ++px) {
                    const float w0 = static_cast<float>((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv_area;
                    const float w1 = static_cast<float>((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv_area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                        const float z = w0 * z0 + w1 * z1 + w2 * z2;
                        const size_t pidx = static_cast<size_t>(py) * kW + px;
                        if (z < zbuf[pidx]) {
                            zbuf[pidx] = z;
                            tri_submesh[pidx] = static_cast<int32_t>(si);
                            tri_idx_buf[pidx] = sm.index_offset + ii;
                            tri_w0_buf[pidx] = w0;
                            tri_w1_buf[pidx] = w1;
                            tri_u_buf[pidx] = w0 * avatar->rest_surfaces[i0].uv0[0] +
                                              w1 * avatar->rest_surfaces[i1].uv0[0] +
                                              w2 * avatar->rest_surfaces[i2].uv0[0];
                            tri_v_buf[pidx] = w0 * avatar->rest_surfaces[i0].uv0[1] +
                                              w1 * avatar->rest_surfaces[i1].uv0[1] +
                                              w2 * avatar->rest_surfaces[i2].uv0[1];
                        }
                    }
                }
            }
        }
    }

    struct ProbePoint {
        const char* name;
        int px{-1};
        int py{-1};
        uint32_t mat_id{0};
        std::string mat_name;
        uint8_t tex_srgb[4]{};
        float tex_lin[3]{};
        float world_n[3]{};
        float proxy_n[3]{};
    };

    // Scan visible pixels to find exact representatives for each requested anatomical/garment landmark
    const auto key_dir_s = aether::renderer::detail::studio_light_dir_to_light(
        rig.studio_lights[0], {0.0f, 1.32f, 0.0f}, {0.0f, 1.45f, 2.0f});
    std::vector<ProbePoint> probes;
    auto find_best = [&](const char* label, auto pred, auto score_fn) {
        ProbePoint best{};
        best.name = label;
        float best_score = -1e9f;
        for (int py = 4; py < static_cast<int>(kH) - 4; ++py) {
            for (int px = 4; px < static_cast<int>(kW) - 4; ++px) {
                const size_t pidx = static_cast<size_t>(py) * kW + px;
                const int32_t si = tri_submesh[pidx];
                if (si < 0)
                    continue;
                // Require 5x5 neighborhood to belong to the same submesh so we avoid silhouette edges
                bool interior = true;
                for (int dy = -2; dy <= 2 && interior; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (tri_submesh[static_cast<size_t>(py + dy) * kW + (px + dx)] != si) {
                            interior = false;
                            break;
                        }
                    }
                }
                if (!interior)
                    continue;
                const auto& sm = avatar->submeshes[static_cast<size_t>(si)];
                const uint32_t tri_base = tri_idx_buf[pidx];
                const uint32_t v0 = avatar->rest_indices[tri_base + 0u];
                const uint32_t v1 = avatar->rest_indices[tri_base + 1u];
                const uint32_t v2 = avatar->rest_indices[tri_base + 2u];
                const float w0 = tri_w0_buf[pidx];
                const float w1 = tri_w1_buf[pidx];
                const float w2 = 1.0f - w0 - w1;
                const auto& sv0 = avatar->skinned_vertices_scratch[v0];
                const auto& sv1 = avatar->skinned_vertices_scratch[v1];
                const auto& sv2 = avatar->skinned_vertices_scratch[v2];
                AetherVertex25D sv = sv0;
                for (int k = 0; k < 3; ++k) {
                    sv.position[k] = w0 * sv0.position[k] + w1 * sv1.position[k] + w2 * sv2.position[k];
                    sv.true_normal[k] = w0 * sv0.true_normal[k] + w1 * sv1.true_normal[k] + w2 * sv2.true_normal[k];
                    sv.proxy_normal[k] = w0 * sv0.proxy_normal[k] + w1 * sv1.proxy_normal[k] + w2 * sv2.proxy_normal[k];
                }
                auto norm3 = [](float v[3]) {
                    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (l > 1e-6f) {
                        v[0] /= l;
                        v[1] /= l;
                        v[2] /= l;
                    }
                };
                norm3(sv.true_normal);
                norm3(sv.proxy_normal);
                const uint32_t mat_id = sm.material_index;
                const auto& mat = avatar->materials[mat_id];
                uint8_t tsrgb[4]{};
                float tlin[3]{};
                sample_raw_tex(mat.tex_base, tri_u_buf[pidx], tri_v_buf[pidx], tsrgb, tlin);
                if (tsrgb[3] < 200)
                    continue;
                const std::string mname = aether::renderer::read_material_cstr(*avatar, mat.name_offset);
                if (!pred(mname, sv, tsrgb))
                    continue;
                const float sc = score_fn(sv, tsrgb, px, py);
                if (sc > best_score) {
                    best_score = sc;
                    best.px = px;
                    best.py = py;
                    best.mat_id = mat_id;
                    best.mat_name = mname;
                    std::copy_n(tsrgb, 4, best.tex_srgb);
                    std::copy_n(tlin, 3, best.tex_lin);
                    std::copy_n(sv.true_normal, 3, best.world_n);
                    std::copy_n(sv.proxy_normal, 3, best.proxy_n);
                }
            }
        }
        probes.push_back(best);
    };

    find_best(
        "1a_Face_Cheek",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Face_00_SKIN") != std::string::npos && v.position[1] > 1.35f && v.position[1] < 1.40f &&
                   v.position[0] > 0.025f && v.position[0] < 0.055f && c[0] > 235 && c[1] > 210;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2]; });
    find_best(
        "1b_Face_Forehead",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Face_00_SKIN") != std::string::npos && v.position[1] > 1.43f &&
                   std::abs(v.position[0]) < 0.025f && c[0] > 235 && c[1] > 195;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2] - std::abs(v.position[0]); });
    find_best(
        "2a_Neck_UnderChin",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Body_00_SKIN") != std::string::npos && v.position[1] > 1.29f && v.position[1] < 1.33f &&
                   std::abs(v.position[0]) < 0.04f && c[0] > 230 && c[1] > 180 && (c[0] - c[2]) > 25;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2] - std::abs(v.position[0]); });
    find_best(
        "2b_Collarbone_Skin",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Body_00_SKIN") != std::string::npos && v.position[1] > 1.23f && v.position[1] < 1.275f &&
                   std::abs(v.position[0]) < 0.05f && c[0] > 235 && c[1] > 200;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2] - std::abs(v.position[0]); });
    find_best(
        "2c_Lit_Neck_Side",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Body_00_SKIN") != std::string::npos && v.position[1] > 1.245f && v.position[1] < 1.28f &&
                   std::abs(v.position[0]) < 0.045f && v.true_normal[2] > 0.65f && c[0] >= 246 && c[1] >= 230 &&
                   c[2] >= 210;
        },
        [](const AetherVertex25D& v, const uint8_t c[4], int, int) -> float {
            return v.position[2] * 5.0f + static_cast<float>(c[1]) * 0.01f;
        });
    find_best(
        "3a_Cardigan_Lit_Front",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Bottoms_01_CLOTH") != std::string::npos && v.position[1] > 1.00f && v.position[1] < 1.22f &&
                   c[0] > 230 && c[1] > 220 && c[2] > 205;
        },
        [&](const AetherVertex25D& v, const uint8_t[4], int, int) -> float {
            return v.true_normal[0] * key_dir_s.x + v.true_normal[1] * key_dir_s.y + v.true_normal[2] * key_dir_s.z;
        });
    find_best(
        "3b_Cardigan_Shaded_Fold",
        [&](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            const float ndl =
                v.true_normal[0] * key_dir_s.x + v.true_normal[1] * key_dir_s.y + v.true_normal[2] * key_dir_s.z;
            return m.find("Bottoms_01_CLOTH") != std::string::npos && v.position[1] > 0.96f && v.position[1] < 1.24f &&
                   v.position[0] < -0.05f && v.true_normal[2] > 0.12f && ndl < 0.05f && c[0] > 225 && c[1] > 210 &&
                   c[2] > 195;
        },
        [&](const AetherVertex25D& v, const uint8_t[4], int, int) -> float {
            const float ndl =
                v.true_normal[0] * key_dir_s.x + v.true_normal[1] * key_dir_s.y + v.true_normal[2] * key_dir_s.z;
            return -ndl + v.true_normal[2] * 0.15f;
        });
    find_best(
        "4_Inner_Black_Shirt",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Body_00_SKIN") != std::string::npos && v.position[1] > 1.04f && v.position[1] < 1.18f &&
                   std::abs(v.position[0]) < 0.055f && c[0] < 75 && c[1] < 75 && c[2] < 75;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2] - std::abs(v.position[0]); });
    find_best(
        "5a_Blue_Ribbon",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Bottoms_01_CLOTH") != std::string::npos && v.position[1] > 0.96f && v.position[1] < 1.28f &&
                   static_cast<int>(c[2]) > static_cast<int>(c[0]) + 15;
        },
        [](const AetherVertex25D& v, const uint8_t c[4], int, int) {
            return static_cast<float>(static_cast<int>(c[2]) - static_cast<int>(c[0])) + v.position[2] * 10.0f;
        });
    find_best(
        "5b_Hair",
        [](const std::string& m, const AetherVertex25D& v, const uint8_t c[4]) {
            return m.find("Hair_00_HAIR") != std::string::npos && v.position[1] > 1.38f && v.position[1] < 1.52f &&
                   c[0] < 120 && c[0] > c[2] + 5;
        },
        [](const AetherVertex25D& v, const uint8_t[4], int, int) { return v.position[2]; });

    uint64_t fidx = 0;
    auto render_probe_pass = [&](const aether::renderer::LightingRig& test_rig, aether::renderer::ShadingDebugView dv,
                                 int warmup_frames, std::vector<float>& out_f16,
                                 std::vector<uint8_t>* out_u8 = nullptr) {
        for (int w = 0; w < warmup_frames; ++w) {
            aether::renderer::FrameRenderRequest req{};
            req.avatar = avatar.get();
            req.rig = &test_rig;
            req.camera = cam;
            req.frame_index = fidx;
            req.frame_slot = static_cast<uint32_t>(fidx & 1ULL);
            req.debug_view = dv;
            req.lighting_mode = test_rig.lighting_mode;
            req.rt_enabled = (test_rig.lighting_mode > 0u) && rhi->rt_supported();
            req.show_checkerboard = false;
            req.render_csm_shadow = true;
            REQUIRE(renderer.render_headless_frame(req).has_value());
            ++fidx;
        }
        auto f16_res = renderer.graph().readback_rgba16f(kW, kH);
        REQUIRE(f16_res.has_value());
        out_f16 = std::move(f16_res.value());
        if (out_u8 != nullptr) {
            const auto outs = renderer.graph().outputs();
            auto u8_res = renderer.graph().readback_image_rgba8(outs.viewport_image,
                                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW, kH);
            REQUIRE(u8_res.has_value());
            *out_u8 = std::move(u8_res.value());
        }
    };

    std::vector<float> buf_albedo, buf_normals, buf_rt_shadow, buf_bounce, buf_direct, buf_hdr_full, buf_final_lin;
    std::vector<uint8_t> buf_final_srgb;

    render_probe_pass(rig, aether::renderer::ShadingDebugView::Albedo, 4, buf_albedo);
    render_probe_pass(rig, aether::renderer::ShadingDebugView::TrueNormals, 2, buf_normals);
    render_probe_pass(rig, aether::renderer::ShadingDebugView::RtShadowDenoised, 10, buf_rt_shadow);
    render_probe_pass(rig, aether::renderer::ShadingDebugView::Bounce, 12, buf_bounce);

    auto direct_rig = rig;
    direct_rig.tone_mode = 0u;
    direct_rig.bloom_intensity = 0.0f;
    direct_rig.enable_2d_filter_pro = false;
    direct_rig.enable_paper_warmth = false;
    direct_rig.crease_strength = 0.0f;
    direct_rig.rim_step_strength = 0.0f;
    direct_rig.hair_highlight_intensity = 0.0f;
    render_probe_pass(direct_rig, aether::renderer::ShadingDebugView::Final, 4, buf_direct);

    auto hdr_full_rig = rig;
    hdr_full_rig.tone_mode = 0u;
    hdr_full_rig.bloom_intensity = 0.0f;
    hdr_full_rig.enable_2d_filter_pro = false;
    hdr_full_rig.enable_paper_warmth = false;
    hdr_full_rig.crease_strength = 0.0f;
    render_probe_pass(hdr_full_rig, aether::renderer::ShadingDebugView::Final, 4, buf_hdr_full);

    render_probe_pass(rig, aether::renderer::ShadingDebugView::Final, 12, buf_final_lin, &buf_final_srgb);

    const std::filesystem::path snap_dir(AETHER_VRM_SNAPSHOT_DIR);
    std::filesystem::create_directories(snap_dir);
    REQUIRE(aether::test::save_rgba8_png(snap_dir / "probe_view.png", kW, kH, buf_final_srgb.data()));

    auto lin_to_oklab = [](float r, float g, float b) -> std::array<float, 4> {
        const float l = std::cbrt(std::max(0.0f, 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b));
        const float m = std::cbrt(std::max(0.0f, 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b));
        const float s = std::cbrt(std::max(0.0f, 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b));
        const float L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
        const float A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
        const float B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
        return {L, A, B, std::sqrt(A * A + B * B)};
    };

    std::printf("\n=== QUANTITATIVE PIXEL & G-BUFFER PROBE (mode=2 PT, 1280x720) ===\n");
    std::array<float, 4> face_lab{}, collar_lab{}, neck_lit_lab{}, card_lit_lab{}, card_sh_lab{};
    float face_rel_luma = 0.0f, neck_lit_rel_luma = 0.0f;
    for (const auto& pr : probes) {
        if (pr.px < 0) {
            std::printf("PROBE %-24s : NOT FOUND ON SCREEN\n", pr.name);
            continue;
        }
        const size_t i4 = (static_cast<size_t>(pr.py) * kW + static_cast<size_t>(pr.px)) * 4ULL;
        const auto& mat = avatar->materials[pr.mat_id];
        const float gn[3] = {buf_normals[i4 + 0] * 2.0f - 1.0f, buf_normals[i4 + 1] * 2.0f - 1.0f,
                             buf_normals[i4 + 2] * 2.0f - 1.0f};
        const float spec_rim[3] = {buf_hdr_full[i4 + 0] - buf_direct[i4 + 0], buf_hdr_full[i4 + 1] - buf_direct[i4 + 1],
                                   buf_hdr_full[i4 + 2] - buf_direct[i4 + 2]};
        const auto lab = lin_to_oklab(buf_final_lin[i4 + 0], buf_final_lin[i4 + 1], buf_final_lin[i4 + 2]);
        const float fin_luma =
            0.2126f * buf_final_lin[i4 + 0] + 0.7152f * buf_final_lin[i4 + 1] + 0.0722f * buf_final_lin[i4 + 2];
        if (std::string_view(pr.name) == "1a_Face_Cheek") {
            face_lab = lab;
            face_rel_luma = fin_luma;
        } else if (std::string_view(pr.name) == "2b_Collarbone_Skin") {
            collar_lab = lab;
        } else if (std::string_view(pr.name) == "2c_Lit_Neck_Side") {
            neck_lit_lab = lab;
            neck_lit_rel_luma = fin_luma;
        } else if (std::string_view(pr.name) == "3a_Cardigan_Lit_Front") {
            card_lit_lab = lab;
        } else if (std::string_view(pr.name) == "3b_Cardigan_Shaded_Fold") {
            card_sh_lab = lab;
        }
        std::printf(
            "\n[%s] px=(%d,%d) mat=%u (%s)\n"
            "  RawTex sRGB=(%u,%u,%u) lin=(%.3f,%.3f,%.3f) | GPU_Albedo=(%.3f,%.3f,%.3f)\n"
            "  MToon _Color=(%.2f,%.2f,%.2f) _ShadeColor=(%.2f,%.2f,%.2f) shift=%.2f toony=%.2f\n"
            "  WorldN=(%.3f,%.3f,%.3f) ProxyN=(%.3f,%.3f,%.3f) | GB_ViewN=(%.3f,%.3f,%.3f)\n"
            "  RT_Shadow=%.3f | DirectLin=(%.3f,%.3f,%.3f) | PT_Bounce=(%.3f,%.3f,%.3f)\n"
            "  Spec+Rim=(%.3f,%.3f,%.3f) | Final_HDR=(%.3f,%.3f,%.3f) | Final_sRGB=(%u,%u,%u) | "
            "OKLab=(L=%.3f,C=%.4f)\n",
            pr.name, pr.px, pr.py, pr.mat_id, pr.mat_name.c_str(), pr.tex_srgb[0], pr.tex_srgb[1], pr.tex_srgb[2],
            static_cast<double>(pr.tex_lin[0]), static_cast<double>(pr.tex_lin[1]), static_cast<double>(pr.tex_lin[2]),
            static_cast<double>(buf_albedo[i4 + 0]), static_cast<double>(buf_albedo[i4 + 1]),
            static_cast<double>(buf_albedo[i4 + 2]), static_cast<double>(mat.base_color[0]),
            static_cast<double>(mat.base_color[1]), static_cast<double>(mat.base_color[2]),
            static_cast<double>(mat.shade_color_gi[0]), static_cast<double>(mat.shade_color_gi[1]),
            static_cast<double>(mat.shade_color_gi[2]), static_cast<double>(mat.toon_params[0]),
            static_cast<double>(mat.toon_params[1]), static_cast<double>(pr.world_n[0]),
            static_cast<double>(pr.world_n[1]), static_cast<double>(pr.world_n[2]), static_cast<double>(pr.proxy_n[0]),
            static_cast<double>(pr.proxy_n[1]), static_cast<double>(pr.proxy_n[2]), static_cast<double>(gn[0]),
            static_cast<double>(gn[1]), static_cast<double>(gn[2]), static_cast<double>(buf_rt_shadow[i4 + 0]),
            static_cast<double>(buf_direct[i4 + 0]), static_cast<double>(buf_direct[i4 + 1]),
            static_cast<double>(buf_direct[i4 + 2]), static_cast<double>(buf_bounce[i4 + 0]),
            static_cast<double>(buf_bounce[i4 + 1]), static_cast<double>(buf_bounce[i4 + 2]),
            static_cast<double>(spec_rim[0]), static_cast<double>(spec_rim[1]), static_cast<double>(spec_rim[2]),
            static_cast<double>(buf_final_lin[i4 + 0]), static_cast<double>(buf_final_lin[i4 + 1]),
            static_cast<double>(buf_final_lin[i4 + 2]), buf_final_srgb[i4 + 0], buf_final_srgb[i4 + 1],
            buf_final_srgb[i4 + 2], static_cast<double>(lab[0]), static_cast<double>(lab[3]));
        CHECK(pr.px >= 0);
    }

    const float de_face_collar = std::sqrt((face_lab[0] - collar_lab[0]) * (face_lab[0] - collar_lab[0]) +
                                           (face_lab[1] - collar_lab[1]) * (face_lab[1] - collar_lab[1]) +
                                           (face_lab[2] - collar_lab[2]) * (face_lab[2] - collar_lab[2]));
    const float de_face_neck = std::sqrt((face_lab[0] - neck_lit_lab[0]) * (face_lab[0] - neck_lit_lab[0]) +
                                         (face_lab[1] - neck_lit_lab[1]) * (face_lab[1] - neck_lit_lab[1]) +
                                         (face_lab[2] - neck_lit_lab[2]) * (face_lab[2] - neck_lit_lab[2]));
    const float luma_diff_pct = 100.0f * std::abs(face_rel_luma - neck_lit_rel_luma) / std::max(face_rel_luma, 1e-4f);
    std::printf("\n[SKIN_PARITY] Face vs LitNeck OKLab DeltaE=%.4f (%.2f%%), RelLumaDiff=%.2f%% | Face vs Collarbone "
                "DeltaE=%.4f (%.2f%%) L_diff=%.2f%%\n",
                static_cast<double>(de_face_neck), static_cast<double>(de_face_neck * 100.0f),
                static_cast<double>(luma_diff_pct), static_cast<double>(de_face_collar),
                static_cast<double>(de_face_collar * 100.0f),
                static_cast<double>(100.0f * std::abs(face_lab[0] - collar_lab[0]) / std::max(face_lab[0], 1e-4f)));
    std::printf("[CARDIGAN_CONTRAST] Lit OKLab L=%.3f C=%.4f vs Shaded OKLab L=%.3f C=%.4f\n",
                static_cast<double>(card_lit_lab[0]), static_cast<double>(card_lit_lab[3]),
                static_cast<double>(card_sh_lab[0]), static_cast<double>(card_sh_lab[3]));
    CHECK(card_lit_lab[0] > card_sh_lab[0] + 0.08f);

    // Phase 2 Photometric Fixtures Exposure & Catchlight Verification (all 6 fixtures @ 1.0 EV)
    {
        static constexpr const char* kFixNames[6] = {
            "0_ARRI_SkyPanel_S60C",  "1_ARRI_Fresnel_T2",      "2_Dedolight_DLED7",
            "3_Broadcast_RingLight", "4_Natural_Sunlight_CIE", "5_Natural_Window_Portal",
        };
        std::printf("\n=== PHASE 2 PHOTOMETRIC FIXTURES (6 profiles @ 1.0 EV) ===\n");
        std::vector<float> fix_lin;
        std::vector<uint8_t> fix_u8;
        static constexpr aether::renderer::StudioFixtureProfile kProfiles[6] = {
            aether::renderer::StudioFixtureProfile::ArriSkyPanel,
            aether::renderer::StudioFixtureProfile::ArriFresnel,
            aether::renderer::StudioFixtureProfile::DedolightSpot,
            aether::renderer::StudioFixtureProfile::BroadcastRing,
            aether::renderer::StudioFixtureProfile::NaturalSunCieSky,
            aether::renderer::StudioFixtureProfile::RectSoftbox,
        };
        for (int fi = 0; fi < 6; ++fi) {
            auto frig = rig;
            aether::renderer::configure_fixture_defaults(frig.studio_lights[0], kProfiles[fi]);
            if (fi == 5) {
                frig.studio_lights[0].window_portal = true;
            }
            render_probe_pass(frig, aether::renderer::ShadingDebugView::Final, 6, fix_lin, &fix_u8);
            const auto st = analyze_rgba8(fix_u8, kW, kH);
            const size_t c_idx = (static_cast<size_t>(probes[0].py) * kW + static_cast<size_t>(probes[0].px)) * 4ULL;
            const size_t k_idx = (static_cast<size_t>(probes[5].py) * kW + static_cast<size_t>(probes[5].px)) * 4ULL;
            std::printf(
                "  Fixture %-26s : fg=%u mean_luma=%.1f p95_luma=%.1f cheek_sRGB=(%u,%u,%u) card_lit_sRGB=(%u,%u,%u)\n",
                kFixNames[fi], st.fg_pixels, st.mean_luma, st.p95_luma, fix_u8[c_idx + 0], fix_u8[c_idx + 1],
                fix_u8[c_idx + 2], fix_u8[k_idx + 0], fix_u8[k_idx + 1], fix_u8[k_idx + 2]);
            CHECK(st.mean_luma > 95.0);
            CHECK(st.p95_luma < 248.0);
            REQUIRE(aether::test::save_rgba8_png(snap_dir / (std::string("probe_fixture_") + kFixNames[fi] + ".png"),
                                                 kW, kH, fix_u8.data()));
        }

        // Catchlight intensity mute vs ON verification
        auto c_off = rig;
        c_off.eye_catchlight_intensity = 0.0f;
        std::vector<float> l_off, l_on;
        std::vector<uint8_t> u_off, u_on;
        render_probe_pass(c_off, aether::renderer::ShadingDebugView::Final, 4, l_off, &u_off);
        auto c_on = rig;
        c_on.eye_catchlight_intensity = 1.25f;
        c_on.eye_catchlight_size = 1.20f;
        render_probe_pass(c_on, aether::renderer::ShadingDebugView::Final, 4, l_on, &u_on);
        double eye_diff = 0.0;
        for (size_t i = 0; i + 3 < u_off.size(); i += 4) {
            eye_diff += std::abs(static_cast<int>(u_on[i + 0]) - static_cast<int>(u_off[i + 0])) +
                        std::abs(static_cast<int>(u_on[i + 1]) - static_cast<int>(u_off[i + 1])) +
                        std::abs(static_cast<int>(u_on[i + 2]) - static_cast<int>(u_off[i + 2]));
        }
        std::printf("[CATCHLIGHT_DELTA] total_rgb_diff(ON=1.25 vs OFF=0.0) = %.0f\n", eye_diff);
        CHECK(eye_diff > 100.0);
    }

    // Phase 3 [TESTING] Disguise Low-Poly Creases: Rule A bit-exact OFF vs ON @ 1.0
    {
        auto render_det = [&](const aether::renderer::LightingRig& r, std::vector<uint8_t>& out_u8) {
            for (int w = 0; w < 64; ++w) {
                aether::renderer::FrameRenderRequest req{};
                req.avatar = avatar.get();
                req.rig = &r;
                req.camera = cam;
                req.frame_index = 714ULL + static_cast<uint64_t>(w);
                req.frame_slot = static_cast<uint32_t>(w & 1);
                req.debug_view = aether::renderer::ShadingDebugView::Final;
                req.lighting_mode = r.lighting_mode;
                req.rt_enabled = (r.lighting_mode > 0u) && rhi->rt_supported();
                req.show_checkerboard = false;
                req.render_csm_shadow = true;
                REQUIRE(renderer.render_headless_frame(req).has_value());
            }
            const auto outs = renderer.graph().outputs();
            auto u8_res = renderer.graph().readback_image_rgba8(outs.viewport_image,
                                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kW, kH);
            REQUIRE(u8_res.has_value());
            out_u8 = std::move(u8_res.value());
        };

        auto det_off = rig;
        det_off.lighting_mode = 0u;
        det_off.crease_disguise_enabled = false;
        det_off.crease_disguise_strength = 0.85f;
        auto det_zero = det_off;
        det_zero.crease_disguise_enabled = true;
        det_zero.crease_disguise_strength = 0.0f;
        std::vector<uint8_t> d_off, d_zero;
        // Converge temporal buffers (auto-exposure / post) completely to mode=0 steady state before comparing.
        render_det(det_off, d_off);
        render_det(det_off, d_off);
        render_det(det_zero, d_zero);

        auto cd_off = rig;
        cd_off.crease_disguise_enabled = false;
        cd_off.crease_disguise_strength = 0.85f;
        auto cd_on = rig;
        cd_on.crease_disguise_enabled = true;
        cd_on.crease_disguise_strength = 1.0f;
        std::vector<float> fl;
        std::vector<uint8_t> u_off, u_on;
        render_probe_pass(cd_off, aether::renderer::ShadingDebugView::Final, 6, fl, &u_off);
        render_probe_pass(cd_on, aether::renderer::ShadingDebugView::Final, 6, fl, &u_on);
        int max_off_vs_zero = 0;
        double sum_on_vs_off = 0.0;
        for (size_t i = 0; i + 3 < u_off.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                max_off_vs_zero = std::max(max_off_vs_zero,
                                           std::abs(static_cast<int>(d_off[i + c]) - static_cast<int>(d_zero[i + c])));
                sum_on_vs_off += std::abs(static_cast<int>(u_on[i + c]) - static_cast<int>(u_off[i + c]));
            }
        }
        std::printf("[CREASE_DISGUISE] RuleA max_diff(OFF vs str=0)=%d | diff(ON=1.0 vs OFF)=%.0f\n", max_off_vs_zero,
                    sum_on_vs_off);
        CHECK(max_off_vs_zero == 0);
        CHECK(sum_on_vs_off > 100.0);
        REQUIRE(aether::test::save_rgba8_png(snap_dir / "probe_crease_off.png", kW, kH, u_off.data()));
        REQUIRE(aether::test::save_rgba8_png(snap_dir / "probe_crease_on.png", kW, kH, u_on.data()));
    }

    CHECK(rhi->validation_error_count() == 0);
    renderer.destroy();
    avatar.reset();
    rhi->destroy();
    aether_runtime_shutdown();
}
