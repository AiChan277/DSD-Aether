#include "aether_ffi_contract.h"
#include "animation/sim_engine.hpp"
#include "animation/spring_bone.hpp"
#include "core/clock.hpp"
#include "core/hot_path_guard.hpp"
#include "renderer/asset_upload/asset_job.hpp"
#include "renderer/npr_renderer.hpp"
#include "rhi/rhi_device.hpp"
#include "test_png_io.hpp"
#include "tracking/camera_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
        const bool is_bg =
            (std::abs(r - bg_r) <= 4.0) && (std::abs(g - bg_g) <= 4.0) && (std::abs(b - bg_b) <= 4.0);
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
    namespace anim = aether::animation;
    anim::RestSkeletonMetrics rm = anim::build_default_humanoid_rest_metrics();

    std::vector<anim::Quat> joint_world_rot(avatar.joints.size(), anim::Quat::identity());
    std::vector<uint8_t> rot_resolved(avatar.joints.size(), 0U);
    auto resolve_joint_world_rot = [&](auto& self, size_t idx, int depth) -> anim::Quat {
        if (idx >= avatar.joints.size() || depth > 128) {
            return anim::Quat::identity();
        }
        if (rot_resolved[idx] != 0U) {
            return joint_world_rot[idx];
        }
        const auto& j = avatar.joints[idx];
        const float* br = j.bind_rotation_quat;
        const float q2 = br[0] * br[0] + br[1] * br[1] + br[2] * br[2] + br[3] * br[3];
        const anim::Quat q_local =
            (q2 > 1e-6f) ? anim::normalize(anim::Quat{br[0], br[1], br[2], br[3]}) : anim::Quat::identity();
        anim::Quat q_parent = anim::Quat::identity();
        if (j.parent_index >= 0 && static_cast<size_t>(j.parent_index) < avatar.joints.size()) {
            q_parent = self(self, static_cast<size_t>(j.parent_index), depth + 1);
        }
        joint_world_rot[idx] = anim::normalize(q_parent * q_local);
        rot_resolved[idx] = 1U;
        return joint_world_rot[idx];
    };
    for (size_t i = 0; i < avatar.joints.size(); ++i) {
        (void)resolve_joint_world_rot(resolve_joint_world_rot, i, 0);
    }

    std::array<anim::Vec3, AETHER_HUMANOID_BONE_COUNT> world{};
    const uint16_t hips_j = avatar.desc.humanoid_to_joint[AETHER_BONE_HIPS];
    if (hips_j < avatar.joint_rest_world_pos.size()) {
        const auto& hp = avatar.joint_rest_world_pos[hips_j];
        world[AETHER_BONE_HIPS] = {hp[0], hp[1], hp[2]};
        rm.rest_world_rotations[AETHER_BONE_HIPS] =
            (hips_j < joint_world_rot.size()) ? joint_world_rot[hips_j] : anim::Quat::identity();
    } else {
        world[AETHER_BONE_HIPS] = rm.rest_local_positions[AETHER_BONE_HIPS];
        rm.rest_world_rotations[AETHER_BONE_HIPS] = anim::Quat::identity();
    }
    rm.rest_local_rotations[AETHER_BONE_HIPS] = rm.rest_world_rotations[AETHER_BONE_HIPS];

    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        const uint16_t j = avatar.desc.humanoid_to_joint[b];
        if (j < avatar.joint_rest_world_pos.size()) {
            const auto& jp = avatar.joint_rest_world_pos[j];
            world[b] = {jp[0], jp[1], jp[2]};
            rm.rest_world_rotations[b] =
                (j < joint_world_rot.size()) ? joint_world_rot[j] : rm.rest_world_rotations[pi];
        } else {
            world[b] = world[pi] + rm.rest_local_positions[b];
            rm.rest_world_rotations[b] = rm.rest_world_rotations[pi];
        }
        rm.rest_local_rotations[b] =
            anim::normalize(anim::conjugate(rm.rest_world_rotations[pi]) * rm.rest_world_rotations[b]);
    }

    rm.rest_local_positions[AETHER_BONE_HIPS] = world[AETHER_BONE_HIPS];
    for (size_t b = 1; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        const int32_t p = rm.parent_indices[b];
        const size_t pi = (p >= 0 && p < static_cast<int32_t>(b)) ? static_cast<size_t>(p) : 0U;
        rm.rest_local_positions[b] = world[b] - world[pi];
        rm.bone_lengths[b] = std::max(anim::length(rm.rest_local_positions[b]), 0.02f);
    }

    uint32_t custom_cnt = 0;
    for (const auto& col : avatar.colliders) {
        if (custom_cnt >= rm.custom_colliders.size()) {
            break;
        }
        uint16_t hb = AETHER_BONE_HIPS;
        std::array<float, 3> jr{0.0f, 0.0f, 0.0f};
        anim::Quat col_j_rot = anim::Quat::identity();
        if (col.joint_index < avatar.joints.size()) {
            jr = avatar.joint_rest_world_pos[col.joint_index];
            col_j_rot = joint_world_rot[col.joint_index];
            uint32_t j = col.joint_index;
            for (size_t steps = 0; steps < avatar.joints.size() && j < avatar.joints.size(); ++steps) {
                const uint16_t candidate = avatar.joints[j].humanoid_bone;
                if (candidate < AETHER_HUMANOID_BONE_COUNT) {
                    hb = candidate;
                    break;
                }
                const int32_t p_idx = avatar.joints[j].parent_index;
                if (p_idx < 0 || static_cast<size_t>(p_idx) >= avatar.joints.size()) {
                    break;
                }
                j = static_cast<uint32_t>(p_idx);
            }
        }
        auto& dst = rm.custom_colliders[custom_cnt++];
        dst.attached_bone = static_cast<int32_t>(hb);
        dst.shape = (col.shape_kind == AETHER_COLLIDER_SHAPE_CAPSULE) ? 1U : 0U;
        dst.radius = std::clamp(col.radius, 0.01f, 0.50f);
        const anim::Vec3 j_pos{jr[0], jr[1], jr[2]};
        const anim::Vec3 a_ws = j_pos + anim::rotate_vec(col_j_rot, {col.offset[0], col.offset[1], col.offset[2]});
        const anim::Vec3 b_ws = j_pos + anim::rotate_vec(col_j_rot, {col.tail[0], col.tail[1], col.tail[2]});
        const anim::Quat inv_hb_rot = anim::conjugate(rm.rest_world_rotations[hb]);
        dst.local_a = anim::rotate_vec(inv_hb_rot, a_ws - world[hb]);
        dst.local_b = anim::rotate_vec(inv_hb_rot, b_ws - world[hb]);
    }
    rm.custom_collider_count = custom_cnt;
    rm.valid = true;
    return rm;
}

double mat3x4_delta_angle_deg(const float m[12]) noexcept {
    const double trace = static_cast<double>(m[0]) + static_cast<double>(m[5]) + static_cast<double>(m[10]);
    const double cos_theta = std::clamp(0.5 * (trace - 1.0), -1.0, 1.0);
    return std::acos(cos_theta) * static_cast<double>(aether::animation::kRadToDeg);
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
        std::printf(
            "  mat[%zu]: base=(%.2f,%.2f,%.2f,%.2f) shade=(%.2f,%.2f,%.2f) emissive=(%.2f,%.2f,%.2f) shift=%.2f toony=%.2f tex=(base=%u,sh=%u,em=%u)\n",
            i, static_cast<double>(m.base_color[0]), static_cast<double>(m.base_color[1]),
            static_cast<double>(m.base_color[2]), static_cast<double>(m.base_color[3]),
            static_cast<double>(m.shade_color_gi[0]), static_cast<double>(m.shade_color_gi[1]),
            static_cast<double>(m.shade_color_gi[2]), static_cast<double>(m.emissive_and_pad[0]),
            static_cast<double>(m.emissive_and_pad[1]), static_cast<double>(m.emissive_and_pad[2]),
            static_cast<double>(m.toon_params[0]), static_cast<double>(m.toon_params[1]), m.tex_base, m.tex_shade,
            m.tex_emissive);
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
    const double l_drop_deg =
        std::asin(std::clamp(static_cast<double>(-l_dir.y), -1.0, 1.0)) * static_cast<double>(aether::animation::kRadToDeg);
    const double r_drop_deg =
        std::asin(std::clamp(static_cast<double>(-r_dir.y), -1.0, 1.0)) * static_cast<double>(aether::animation::kRadToDeg);

    std::printf(
        "[NEUTRAL-POSE] delta_deg(head=%.3f neck=%.3f chest=%.3f spine=%.3f) sh_dy=%.4f arm_drop=(L=%.2f, R=%.2f deg)\n",
        head_deg, neck_deg, chest_deg, spine_deg, static_cast<double>(std::abs(l_sh_ws.y - r_sh_ws.y)), l_drop_deg,
        r_drop_deg);

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
        const Vec3 eye_left = aether::animation::normalize_safe(skinned(AETHER_BONE_LEFT_EYE) -
                                                                skinned(AETHER_BONE_RIGHT_EYE));
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
            req.show_orbit_orb = false;
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
        std::printf(
            "[SHOT %s_%s] fg=%u mean_luma=%.1f p95_luma=%.1f white_clip=%u denoise=%.3fms (p95=%.3fms, active=%s, fallback=%d/%d)\n",
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

    // Face close-ups: neutral, then both eyes fully blinked + mouth "A". With per-primitive morph duplication the
    // blink only moved the eyelid skin (lashes / eye whites / irises stayed open), so this shot is the visual check.
    aether::renderer::PreviewCamera face = fit_cam;
    face.target_y = fit_cam.target_y + 0.53f;
    face.yaw_rad = 0.0f;
    face.pitch_rad = 0.0f;
    face.distance = 0.36f;
    face.fovy_rad = 0.50f;
    capture_shot("face_neutral", face, false);
    std::array<float, aether::animation::kMaxMorphs> expr{};
    std::copy_n(skin.morph_weights, std::min<size_t>(expr.size(), skin.morph_count), expr.begin());
    expr[0] = 1.0f;  // eyeBlinkLeft  -> VRM Blink_L
    expr[7] = 1.0f;  // eyeBlinkRight -> VRM Blink_R
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
            if (f >= 110) {
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

    std::printf(
        "[STEADY-STATE 120f] max_bone_mat_var=%.2e sim_tip_vel=%.2e m/s (333Hz=%.2e, worst j=%u %.2e) vrm_tip_vel=%.2e m/s (worst j=%u %.2e) (vrm_springs=%u cols=%u)\n",
        max_bone_mat_var, max_sim_tip_vel, max_333hz_tip_vel, worst_sim_j, worst_sim_v, max_vrm_tip_vel, worst_vrm_j,
        worst_vrm_v, vrm_spring.joint_count(), vrm_spring.collider_count());
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
    CHECK(peak[3] < 20.5f);    // critically damped: no overshoot beyond the tracked 20 deg
    CHECK(max_jerk < 0.35f);   // C2-smooth: no per-frame acceleration spikes from the 30 Hz camera steps

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
    const bool has_physical =
        std::any_of(devs.begin(), devs.end(), [](const auto& d) { return !d.likely_virtual; });
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
