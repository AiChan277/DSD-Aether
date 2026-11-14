#include "core/hot_path_guard.hpp"
#include "tracking/canonical_face_model.hpp"
#include "tracking/landmark_solver.hpp"
#include "tracking/manifest.hpp"
#include "tracking/roi_preprocess.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace aether::tracking;

namespace {

void fill_synthetic_nv12_pattern(CapturedFrameBuffer& fb, uint32_t w, uint32_t h) {
    fb.width = w;
    fb.height = h;
    fb.stride_y = w;
    fb.stride_uv = w;
    fb.format = CameraPixelFormat::NV12;
    fb.capture_timestamp_us = 100000;
    fb.sequence = 1;
    uint8_t* y_plane = fb.bytes.data();
    uint8_t* uv_plane = fb.bytes.data() + static_cast<size_t>(w) * h;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            y_plane[y * w + x] = static_cast<uint8_t>(16U + ((x * 219U) / w + (y * 100U) / h) % 219U);
        }
    }
    for (uint32_t y = 0; y < h / 2; ++y) {
        for (uint32_t x = 0; x < w / 2; ++x) {
            uv_plane[y * w + x * 2 + 0] = static_cast<uint8_t>(64U + (x * 128U) / (w / 2));
            uv_plane[y * w + x * 2 + 1] = static_cast<uint8_t>(64U + (y * 128U) / (h / 2));
        }
    }
}

void rotate_point_euler(float yaw, float pitch, float roll, const float p[3], float out[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    // R = Ry(yaw) * Rx(pitch) * Rz(roll)
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

} // namespace

TEST_CASE("Canonical MediaPipe face metadata and 146->52 Appendix B remapping match contract", "[tracking][canon]") {
    static_assert(kCanonicalFaceVertexCount == 468);
    static_assert(kProcrustesBasisCount == 33);
    static_assert(kM3SubsetCount == 146);

    for (size_t i = 0; i < kM3SubsetCount; ++i) {
        CHECK(kM3LandmarkSubsetIndices[i] < 478);
    }
    // eyeBlinkLeft is Appendix B slot 0 -> MediaPipe index 9
    CHECK(kAppendixBFromMediaPipeM3[0] == 9);
    // jawOpen is Appendix B slot 17 -> MediaPipe index 26
    CHECK(kAppendixBFromMediaPipeM3[17] == 26);
    // tongueOut is Appendix B slot 51 -> -1
    CHECK(kAppendixBFromMediaPipeM3[51] == -1);
}

TEST_CASE("Manifest parser validates SHA-256 and rejects tampered model weights", "[tracking][manifest]") {
    const std::filesystem::path manifest_path = std::filesystem::path(AETHER_SOURCE_DIR) / "models" / "manifest.toml";
    REQUIRE(std::filesystem::exists(manifest_path));

    TrackingModelManifest manifest{};
    std::string err;
    REQUIRE(load_model_manifest(manifest_path, manifest, &err) == AETHER_OK);
    REQUIRE(manifest.all_present());

    for (size_t i = 0; i < static_cast<size_t>(ModelId::Count); ++i) {
        std::filesystem::path resolved;
        const auto mid = static_cast<ModelId>(i);
        REQUIRE(verify_and_resolve_model_path(manifest, mid, EpTier::NpuOpenVino, resolved, &err) == AETHER_OK);
        CHECK(std::filesystem::exists(resolved));
        if (manifest.models[i].tier1_variant == "int8") {
            CHECK(manifest.models[i].int8_nme_delta_pct <= 3.0);
        } else if (i != static_cast<size_t>(ModelId::M3ArkitBlendshapes)) {
            CHECK(manifest.models[i].tier1_variant == "fp16");
        }
    }

    // Tampered copy must be rejected with AETHER_E_PARSE / AETHER_E_INTERNAL (never silently loaded)
    TrackingModelManifest bad = manifest;
    bad.models[0].int8.sha256_upper = std::string(64, '0');
    std::filesystem::path bad_resolved;
    CHECK(verify_and_resolve_model_path(bad, ModelId::M0FaceDetector, EpTier::NpuOpenVino, bad_resolved, &err) !=
          AETHER_OK);
}

TEST_CASE("AVX2 NV12->NCHW ROI warp matches scalar reference within 1e-4 and runs <= 0.6ms zero-alloc",
          "[tracking][preprocess][perf]") {
    auto fb = std::make_unique<CapturedFrameBuffer>();
    fill_synthetic_nv12_pattern(*fb, 1280, 720);

    const RoiRectNorm roi{0.52f, 0.48f, 0.36f, 0.48f, 0.17f, true};
    std::vector<float> out_simd(3ULL * 256ULL * 256ULL, 0.0f);
    std::vector<float> out_scalar(3ULL * 256ULL * 256ULL, 0.0f);

    warp_frame_to_nchw(*fb, roi, 256, 256, RoiNormRange::ZeroToOne, out_scalar.data(), true);
    warp_frame_to_nchw(*fb, roi, 256, 256, RoiNormRange::ZeroToOne, out_simd.data(), false);

    float max_diff = 0.0f;
    for (size_t i = 0; i < out_simd.size(); ++i) {
        max_diff = std::max(max_diff, std::abs(out_simd[i] - out_scalar[i]));
        CHECK(out_simd[i] >= 0.0f);
        CHECK(out_simd[i] <= 1.0f);
    }
    CHECK(max_diff < 1e-4f);

    // Zero-alloc + <= 0.6 ms timing budget across rotated ROI 256x256 + 224x224 crops
    std::vector<float> out_224(3ULL * 224ULL * 224ULL, 0.0f);
    std::vector<float> scratch_146(146ULL * 2ULL, 0.0f);
    std::array<float, 478 * 3> lm478{};
    for (size_t i = 0; i < 478; ++i) {
        lm478[i * 3 + 0] = static_cast<float>(i % 256);
        lm478[i * 3 + 1] = static_cast<float>(i % 256);
    }

#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
    aether::HotPathGuard::install();
    aether::HotPathGuard::reset_thread_violations();
#endif

    // Warmup
    for (int w = 0; w < 5; ++w) {
        warp_frame_to_nchw(*fb, roi, 256, 256, RoiNormRange::ZeroToOne, out_simd.data(), false);
        warp_frame_to_nchw(*fb, roi, 224, 224, RoiNormRange::ZeroToOne, out_224.data(), false);
    }

    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kIters = 100;
    {
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        AETHER_HOT_PATH_SCOPE;
#endif
        for (int i = 0; i < kIters; ++i) {
            warp_frame_to_nchw(*fb, roi, 256, 256, RoiNormRange::ZeroToOne, out_simd.data(), false);
            warp_frame_to_nchw(*fb, roi, 224, 224, RoiNormRange::ZeroToOne, out_224.data(), false);
            extract_m3_subset_146x2(lm478.data(), 256.0f, scratch_146.data());
        }
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        CHECK(aether::HotPathGuard::thread_violations() == 0);
#endif
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double mean_ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / kIters;
    INFO("NV12->NCHW mean preprocess time (256x256 + 224x224): " << mean_ms << " ms");
    // Conservative check even under unoptimized Debug builds, tight in Release
#if defined(NDEBUG)
    CHECK(mean_ms <= 0.60);
#else
    CHECK(mean_ms <= 6.00);
#endif
}

TEST_CASE("Procrustes weighted Kabsch head pose solver recovers exact 3D rotation and translation",
          "[tracking][procrustes]") {
    std::array<float, 478 * 3> landmarks{};
    const float yaw = 0.25f;
    const float pitch = -0.15f;
    const float roll = 0.10f;
    const float true_t[3]{0.04f, -0.02f, -0.55f};

    for (uint32_t i = 0; i < kCanonicalFaceVertexCount; ++i) {
        float rotated[3]{};
        rotate_point_euler(yaw, pitch, roll, kCanonicalFaceVerticesM[i], rotated);
        landmarks[i * 3 + 0] = rotated[0] + true_t[0];
        landmarks[i * 3 + 1] = rotated[1] + true_t[1];
        landmarks[i * 3 + 2] = rotated[2] + true_t[2];
    }

    const HeadPose6D pose = solve_procrustes_head_pose(landmarks.data(), 1.0f);
    REQUIRE(pose.valid);

    // Build expected quaternion for Ry(yaw) * Rx(pitch) * Rz(roll) by converting solver axis check via dot product
    float expected[3]{};
    float got[3]{};
    const float basis_x[3]{1.0f, 0.0f, 0.0f};
    rotate_point_euler(yaw, pitch, roll, basis_x, expected);

    const float qx = pose.rotation_xyzw[0], qy = pose.rotation_xyzw[1], qz = pose.rotation_xyzw[2],
                qw = pose.rotation_xyzw[3];
    got[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    got[1] = 2.0f * (qx * qy + qz * qw);
    got[2] = 2.0f * (qx * qz - qy * qw);

    const float dot_x = std::clamp(expected[0] * got[0] + expected[1] * got[1] + expected[2] * got[2], -1.0f, 1.0f);
    const float ang_err_deg = std::acos(dot_x) * (180.0f / 3.14159265f);
    CHECK(ang_err_deg < 0.2f);
    CHECK(std::abs(pose.position_m[0] - true_t[0]) < 0.002f);
    CHECK(std::abs(pose.position_m[1] - true_t[1]) < 0.002f);
    CHECK(std::abs(pose.position_m[2] - true_t[2]) < 0.002f);
}

TEST_CASE("Classic solvePnP rigid skull, 2.5% ROI hysteresis, 15-frame auto-neutral lock, and EAR/MAR work",
          "[tracking][pnp][live2d]") {
    constexpr uint32_t kCamW = 1280U;
    constexpr uint32_t kCamH = 720U;
    const RoiRectNorm roi = enforce_square_pixel_roi({0.5f, 0.45f, 0.35f, 0.35f, 0.0f, true}, kCamW, kCamH);

    auto project_canonical_into_m2 = [&](const RoiRectNorm& crop_roi, float yaw, float pitch, float roll,
                                         const float t[3], float out_lm[478 * 3]) {
        const float fx = static_cast<float>(kCamW);
        const float fy = static_cast<float>(kCamW);
        const float cx = 0.5f * static_cast<float>(kCamW);
        const float cy = 0.5f * static_cast<float>(kCamH);
        for (uint32_t i = 0; i < kCanonicalFaceVertexCount; ++i) {
            float rot[3]{};
            rotate_point_euler(yaw, pitch, roll, kCanonicalFaceVerticesM[i], rot);
            const float px = rot[0] + t[0];
            const float py = rot[1] + t[1];
            const float pz = rot[2] + t[2];
            const float d = std::max(-pz, 0.05f);
            const float u_cam = cx + fx * (px / d);
            const float v_cam = cy - fy * (py / d);
            // Inverse of map_m2_crop_to_cam_px (square-pixel crop rotated by crop_roi.rotation_rad)
            const float dx_px = u_cam - crop_roi.cx * static_cast<float>(kCamW);
            const float dy_px = v_cam - crop_roi.cy * static_cast<float>(kCamH);
            const float side_px = crop_roi.width * static_cast<float>(kCamW);
            const float rc = std::cos(crop_roi.rotation_rad);
            const float rs = std::sin(crop_roi.rotation_rad);
            const float u_m2 = ((rc * dx_px + rs * dy_px) / side_px + 0.5f) * 256.0f;
            const float v_m2 = ((-rs * dx_px + rc * dy_px) / side_px + 0.5f) * 256.0f;
            out_lm[i * 3 + 0] = u_m2;
            out_lm[i * 3 + 1] = v_m2;
            out_lm[i * 3 + 2] = 0.0f; // intentionally zero M2 pseudo-Z to prove PnP ignores it!
        }
        // Place iris centers (468, 473) midway between eye corners
        out_lm[468 * 3 + 0] = 0.5f * (out_lm[33 * 3 + 0] + out_lm[133 * 3 + 0]);
        out_lm[468 * 3 + 1] = 0.5f * (out_lm[159 * 3 + 1] + out_lm[145 * 3 + 1]);
        out_lm[473 * 3 + 0] = 0.5f * (out_lm[263 * 3 + 0] + out_lm[362 * 3 + 0]);
        out_lm[473 * 3 + 1] = 0.5f * (out_lm[386 * 3 + 1] + out_lm[374 * 3 + 1]);
    };

    std::array<float, 478 * 3> lm{};
    const float t_tilted[3]{0.02f, -0.04f, -0.58f};
    const float base_yaw = 0.14f;
    const float base_pitch = -0.30f; // e.g., laptop webcam tilted upward from below
    const float base_roll = -0.06f;
    project_canonical_into_m2(roi, base_yaw, base_pitch, base_roll, t_tilted, lm.data());

    const HeadPose6D pnp = solve_pnp_head_pose(lm.data(), roi, kCamW, kCamH);
    REQUIRE(pnp.valid);
    CHECK(pnp.reproj_rms_px < 1.5f);

    // Check recovered 3D basis vector against true euler rotation
    const float basis_z[3]{0.0f, 0.0f, 1.0f};
    float exp_z[3]{};
    rotate_point_euler(base_yaw, base_pitch, base_roll, basis_z, exp_z);
    const float qx = pnp.rotation_xyzw[0], qy = pnp.rotation_xyzw[1], qz = pnp.rotation_xyzw[2],
                qw = pnp.rotation_xyzw[3];
    const float got_z[3]{
        2.0f * (qx * qz + qy * qw),
        2.0f * (qy * qz - qx * qw),
        1.0f - 2.0f * (qx * qx + qy * qy),
    };
    const float dot_z = std::clamp(exp_z[0] * got_z[0] + exp_z[1] * got_z[1] + exp_z[2] * got_z[2], -1.0f, 1.0f);
    CHECK(std::acos(dot_z) * (180.0f / 3.14159265f) < 0.5f);

    // 2.5% ROI hysteresis deadband freezes ROI on small jitter
    LandmarkPoseSolver solver;
    solver.reset();
    const RoiRectNorm roi0 = solver.compute_next_face_roi(lm.data(), roi, 0.95f, kCamW, kCamH);
    REQUIRE(roi0.valid);
    project_canonical_into_m2(roi0, base_yaw, base_pitch, base_roll, t_tilted, lm.data());
    for (uint32_t i = 0; i < 468U; ++i) {
        lm[i * 3 + 0] += 0.35f; // tiny pixel micro-swim < 2.5%
        lm[i * 3 + 1] -= 0.25f;
    }
    const RoiRectNorm roi1 = solver.compute_next_face_roi(lm.data(), roi0, 0.95f, kCamW, kCamH);
    CHECK(roi1.cx == roi0.cx);
    CHECK(roi1.cy == roi0.cy);
    CHECK(roi1.width == roi0.width);
    CHECK(roi1.rotation_rad == roi0.rotation_rad);

    // MediaPipe crop convention: a strongly rolled head yields a crop rotated so the eye line (33 -> 263) is level
    // inside the 256x256 M2 input, and the crop spans ~1.5x the landmark extent (not a tight 1.32x box).
    {
        // Face at 0.9 m so the 1.5x crop stays below enforce_square_pixel_roi's 56%-of-frame size clamp.
        const float t_far[3]{0.02f, -0.04f, -0.90f};
        std::array<float, 478 * 3> lm_roll{};
        project_canonical_into_m2(roi, 0.0f, 0.0f, 0.30f, t_far, lm_roll.data());
        RoiRectNorm r = roi;
        for (int it = 0; it < 3; ++it) {
            r = solver.compute_next_face_roi(lm_roll.data(), r, 0.95f, kCamW, kCamH);
            REQUIRE(r.valid);
            project_canonical_into_m2(r, 0.0f, 0.0f, 0.30f, t_far, lm_roll.data());
        }
        const float ex = lm_roll[263 * 3 + 0] - lm_roll[33 * 3 + 0];
        const float ey = lm_roll[263 * 3 + 1] - lm_roll[33 * 3 + 1];
        CHECK(std::abs(std::atan2(ey, ex)) * (180.0f / 3.14159265f) < 2.5f);
        CHECK(std::abs(r.rotation_rad) * (180.0f / 3.14159265f) > 12.0f);
        float min_x = 1e9f, max_x = -1e9f;
        for (uint32_t i = 0; i < 468U; ++i) {
            min_x = std::min(min_x, lm_roll[i * 3 + 0]);
            max_x = std::max(max_x, lm_roll[i * 3 + 0]);
        }
        CHECK((max_x - min_x) / 256.0f < 0.72f); // face occupies ~1/1.5 of the crop, leaving model context margin
    }

    // Auto-neutral lock after 15 consecutive stable frames zeroes out initial camera tilt
    project_canonical_into_m2(roi, base_yaw, base_pitch, base_roll, t_tilted, lm.data());
    AetherTrackingFrame tf{};
    for (uint32_t f = 0; f < 16U; ++f) {
        solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, (f + 1ULL) * 33333ULL, (f + 1ULL) * 33333ULL + 2000ULL,
                                       EpTier::NpuOpenVino, f + 1U, f == 0U, tf, kCamW, kCamH);
    }
    CHECK(solver.neutral_calibrated());
    CHECK(std::abs(solver.last_head_euler_deg().pitch) < 0.25f);
    CHECK(std::abs(solver.last_head_euler_deg().yaw) < 0.25f);
    CHECK(std::abs(solver.last_head_euler_deg().roll) < 0.25f);

    // Apply EAR close & MAR open and verify geometric blendshapes drive blink + jawOpen cleanly
    lm[160 * 3 + 1] = lm[144 * 3 + 1];
    lm[158 * 3 + 1] = lm[153 * 3 + 1];
    lm[13 * 3 + 1] -= 4.0f;
    lm[14 * 3 + 1] += 18.0f;
    solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                   nullptr, nullptr, 600000ULL, 602000ULL, EpTier::NpuOpenVino, 17U, false, tf,
                                   kCamW, kCamH);
    CHECK(tf.arkit_blendshapes[0] > 0.65f);  // eyeBlinkLeft
    CHECK(tf.arkit_blendshapes[17] > 0.40f); // jawOpen
}
