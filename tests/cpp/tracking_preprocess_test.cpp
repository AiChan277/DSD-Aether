#include "core/hot_path_guard.hpp"
#include "tracking/blink_gaze_filter.hpp"
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

    // Stage 1 3-frame median rejects a 1-frame landmark spike cleanly, then sustained EAR close & MAR open drive blink
    // + jawOpen
    lm[160 * 3 + 1] = lm[144 * 3 + 1];
    lm[158 * 3 + 1] = lm[153 * 3 + 1];
    lm[13 * 3 + 1] -= 4.0f;
    lm[14 * 3 + 1] += 18.0f;
    solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                   nullptr, nullptr, 566661ULL, 568661ULL, EpTier::NpuOpenVino, 17U, false, tf, kCamW,
                                   kCamH);
    CHECK(tf.arkit_blendshapes[17] < 0.05f); // 1-frame spike suppressed by 3-frame median!

    for (uint32_t f = 18U; f <= 29U; ++f) {
        const uint64_t ts = static_cast<uint64_t>(f) * 33333ULL;
        solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, ts, ts + 2000ULL, EpTier::NpuOpenVino, f, false, tf, kCamW,
                                       kCamH);
    }
    // Landmarks 160/144/158/153 belong to MediaPipe's 33/133 eye = the subject's RIGHT eye -> ARKit eyeBlinkRight (7).
    // The other eye stays open: a decisive one-eye close survives Kalidokit stabilizeBlink as a wink.
    CHECK(tf.arkit_blendshapes[7] > 0.65f);  // eyeBlinkRight
    CHECK(tf.arkit_blendshapes[0] < 0.20f);  // eyeBlinkLeft stays open
    CHECK(tf.arkit_blendshapes[17] > 0.40f); // jawOpen
    CHECK(tf.webcam_visemes[0] > 0.50f);     // Live2D vowel "A" from the opened inner lips
    CHECK(tf.webcam_visemes[0] > tf.webcam_visemes[1]);
}

TEST_CASE(
    "Task 2-4: Live2D 68-pt overlay, rigid head unprojection (<0.01 bleed), 0.25 deg lock, and 3-pt brow tracking",
    "[tracking][live2d][brow]") {
    constexpr uint32_t kCamW = 1280U;
    constexpr uint32_t kCamH = 720U;
    constexpr float kFocalPx = 900.0f;
    const RoiRectNorm roi{0.50f, 0.50f, 0.45f, 0.45f, 0.0f, true};

    auto make_face_roi_landmarks = [&](float yaw_rad, float pitch_rad, float roll_rad, float mouth_open_m,
                                       float mouth_corner_dx_m, float brow_dy_m) {
        std::array<float, 478 * 3> lm{};
        auto is_brow_pt = [](uint32_t idx) -> bool {
            constexpr uint32_t kBrows[] = {46,  52,  53,  55,  63,  65,  66,  70,  105, 107,
                                           276, 282, 283, 285, 293, 295, 296, 300, 334, 336};
            for (const uint32_t b : kBrows) {
                if (idx == b) {
                    return true;
                }
            }
            return false;
        };
        for (uint32_t i = 0; i < 478U; ++i) {
            const float* base = (i < kCanonicalFaceVertexCount)
                                    ? kCanonicalFaceVerticesM[i]
                                    : ((i < 473U) ? kCanonicalFaceVerticesM[159] : kCanonicalFaceVerticesM[386]);
            float p3[3]{base[0], base[1], base[2]};
            if (i == 13U || i == 312U || i == 82U) {
                p3[1] += mouth_open_m * 0.45f;
            } else if (i == 14U || i == 317U || i == 87U) {
                p3[1] -= mouth_open_m * 0.55f;
            } else if (i == 61U || i == 78U) {
                p3[0] -= mouth_corner_dx_m;
            } else if (i == 291U || i == 308U) {
                p3[0] += mouth_corner_dx_m;
            } else if (is_brow_pt(i)) {
                p3[1] += brow_dy_m;
            }
            float rot[3]{};
            rotate_point_euler(yaw_rad, pitch_rad, roll_rad, p3, rot);
            const float z = -0.55f + rot[2];
            const float u_cam = 0.5f + (rot[0] * kFocalPx) / (std::max(-z, 0.2f) * static_cast<float>(kCamW));
            const float v_cam = 0.5f - (rot[1] * kFocalPx) / (std::max(-z, 0.2f) * static_cast<float>(kCamH));
            lm[i * 3 + 0] = ((u_cam - roi.cx) / roi.width + 0.5f) * 256.0f;
            lm[i * 3 + 1] = ((v_cam - roi.cy) / roi.height + 0.5f) * 256.0f;
            lm[i * 3 + 2] = -rot[2] * 100.0f;
        }
        return lm;
    };

    LandmarkPoseSolver solver;
    solver.reset();
    AetherTrackingFrame tf{};
    // Calibrate neutral for 25 frames at zero expression
    for (int f = 0; f < 25; ++f) {
        const auto lm = make_face_roi_landmarks(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        const uint64_t ts = static_cast<uint64_t>(f + 1) * 33333ULL;
        solver.populate_tracking_frame(roi, lm.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, ts, ts, EpTier::Cpu, static_cast<uint32_t>(f + 1), false, tf,
                                       kCamW, kCamH);
    }
    REQUIRE(solver.neutral_calibrated());
    const auto& ov = solver.overlay_snapshot();
    CHECK(ov.keypoint_count == 68U);
    CHECK(ov.iris_valid);
    CHECK(ov.live2d.param_mouth_open_y == 0.0f);

    // Stage 3: tiny 0.10 deg jitter inside 0.25 deg deadband stays locked at neutral.
    {
        const auto lm_tiny = make_face_roi_landmarks(0.10f * 0.01745329f, -0.08f * 0.01745329f, 0.0f, 0.0f, 0.0f, 0.0f);
        for (int f = 0; f < 6; ++f) {
            const uint64_t ts = static_cast<uint64_t>(30 + f) * 33333ULL;
            solver.populate_tracking_frame(roi, lm_tiny.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false,
                                           nullptr, nullptr, nullptr, ts, ts, EpTier::Cpu,
                                           static_cast<uint32_t>(30 + f), false, tf, kCamW, kCamH);
        }
        CHECK(std::abs(solver.last_head_euler_deg().yaw) < 0.25f);
        CHECK(std::abs(solver.last_head_euler_deg().pitch) < 0.25f);
    }

    // Stage 2: large rigid head rotation (Yaw 18 deg, Pitch 15 deg, Roll 10 deg) with neutral face must not bleed
    // into mouth opening or brow expression (< 0.03).
    {
        for (int f = 0; f < 30; ++f) {
            const float s = std::min(1.0f, static_cast<float>(f + 1) / 15.0f);
            const auto lm_rot = make_face_roi_landmarks(s * 18.0f * 0.01745329f, s * 15.0f * 0.01745329f,
                                                        s * 10.0f * 0.01745329f, 0.0f, 0.0f, 0.0f);
            const uint64_t ts = static_cast<uint64_t>(40 + f) * 33333ULL;
            solver.populate_tracking_frame(roi, lm_rot.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false,
                                           nullptr, nullptr, nullptr, ts, ts, EpTier::Cpu,
                                           static_cast<uint32_t>(40 + f), false, tf, kCamW, kCamH);
        }
        REQUIRE(aether::animation::has_packed_live2d(tf));
        aether::animation::Live2DParameters l2d{};
        aether::animation::unpack_live2d_from_tracking_frame(tf, l2d);
        CHECK(std::abs(l2d.param_mouth_open_y) < 0.03f);
        CHECK(std::abs(l2d.param_brow_l_y) < 0.05f);
        CHECK(std::abs(l2d.param_brow_r_y) < 0.05f);
    }

    // Task 4: Raise & furrow brows at neutral pose -> verify positive and negative ParamBrowLY/RY & ARKit brows.
    for (int f = 0; f < 25; ++f) {
        const auto lm_up = make_face_roi_landmarks(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.010f);
        const uint64_t ts = static_cast<uint64_t>(80 + f) * 33333ULL;
        solver.populate_tracking_frame(roi, lm_up.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, ts, ts, EpTier::Cpu, static_cast<uint32_t>(80 + f), false, tf,
                                       kCamW, kCamH);
    }
    aether::animation::Live2DParameters brow_up{};
    REQUIRE(aether::animation::has_packed_live2d(tf));
    aether::animation::unpack_live2d_from_tracking_frame(tf, brow_up);
    CHECK(brow_up.param_brow_l_y > 0.30f);
    CHECK(brow_up.param_brow_r_y > 0.30f);
    CHECK(tf.arkit_blendshapes[43] > 0.25f); // browInnerUp

    for (int f = 0; f < 25; ++f) {
        const auto lm_dn = make_face_roi_landmarks(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.010f);
        const uint64_t ts = static_cast<uint64_t>(110 + f) * 33333ULL;
        solver.populate_tracking_frame(roi, lm_dn.data(), 0.96f, nullptr, nullptr, nullptr, 0.0f, false, false, nullptr,
                                       nullptr, nullptr, ts, ts, EpTier::Cpu, static_cast<uint32_t>(110 + f), false, tf,
                                       kCamW, kCamH);
    }
    aether::animation::Live2DParameters brow_dn{};
    REQUIRE(aether::animation::has_packed_live2d(tf));
    aether::animation::unpack_live2d_from_tracking_frame(tf, brow_dn);
    CHECK(brow_dn.param_brow_l_y < -0.30f);
    CHECK(brow_dn.param_brow_r_y < -0.30f);
    CHECK(tf.arkit_blendshapes[41] > 0.25f); // browDownLeft
    CHECK(tf.arkit_blendshapes[42] > 0.25f); // browDownRight

    // Verify Task 4 overlay fields (8-pt eye polygons, 5-pt eyebrow curves, gaze_vec)
    CHECK(solver.overlay_snapshot().eye_poly_valid);
    CHECK(solver.overlay_snapshot().brow_pts_valid);
    for (size_t k = 0; k < 5U; ++k) {
        CHECK(solver.overlay_snapshot().left_brow_pts_norm[k * 2U + 0U] > 0.0f);
        CHECK(solver.overlay_snapshot().left_brow_pts_norm[k * 2U + 0U] < 1.0f);
        CHECK(solver.overlay_snapshot().right_brow_pts_norm[k * 2U + 0U] > 0.0f);
        CHECK(solver.overlay_snapshot().right_brow_pts_norm[k * 2U + 0U] < 1.0f);
    }
}

TEST_CASE("Full-res eye ROI dark-iris refinement rejects specular glare across NV12, YUY2, and BGRA",
          "[tracking][iris][task4]") {
    constexpr uint32_t kW = 1280U;
    constexpr uint32_t kH = 720U;
    const float iris_true_x = 562.0f;
    const float iris_true_y = 324.0f;
    const std::array<float, 16> eye_poly = {{
        544.0f,
        323.0f,
        551.0f,
        317.0f,
        560.0f,
        316.0f,
        569.0f,
        317.0f,
        576.0f,
        323.0f,
        569.0f,
        329.0f,
        560.0f,
        330.0f,
        551.0f,
        329.0f,
    }};

    auto luma_at = [&](uint32_t x, uint32_t y) -> uint8_t {
        const float dx = static_cast<float>(x) - iris_true_x;
        const float dy = static_cast<float>(y) - iris_true_y;
        const float d2 = dx * dx + dy * dy;
        // Bright specular catchlight at offset (iris_true_x + 2, iris_true_y - 2) must be rejected
        const float gx = static_cast<float>(x) - (iris_true_x + 2.0f);
        const float gy = static_cast<float>(y) - (iris_true_y - 2.0f);
        if (gx * gx + gy * gy <= 2.25f) {
            return 250U;
        }
        if (d2 <= 30.25f) {
            return 24U; // Dark iris/pupil
        }
        return 215U; // Bright sclera
    };

    auto fb = std::make_unique<CapturedFrameBuffer>();

    // 1) NV12
    fb->width = kW;
    fb->height = kH;
    fb->stride_y = kW;
    fb->stride_uv = kW;
    fb->format = CameraPixelFormat::NV12;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            fb->bytes[y * kW + x] = luma_at(x, y);
        }
    }
    {
        float cx = 559.5f; // Biased initial M2 guess
        float cy = 322.0f;
        CHECK(refine_eye_iris_fullres(*fb, eye_poly.data(), cx, cy));
        CHECK(std::abs(cx - iris_true_x) < 0.85f);
        CHECK(std::abs(cy - iris_true_y) < 0.85f);
    }

    // 2) YUY2
    fb->format = CameraPixelFormat::YUY2;
    fb->stride_y = kW * 2U;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            fb->bytes[y * fb->stride_y + x * 2U + 0U] = luma_at(x, y);
            fb->bytes[y * fb->stride_y + x * 2U + 1U] = 128U;
        }
    }
    {
        float cx = 564.2f;
        float cy = 325.1f;
        CHECK(refine_eye_iris_fullres(*fb, eye_poly.data(), cx, cy));
        CHECK(std::abs(cx - iris_true_x) < 0.85f);
        CHECK(std::abs(cy - iris_true_y) < 0.85f);
    }

    // 3) BGRA32
    fb->format = CameraPixelFormat::BGRA32;
    fb->stride_y = kW * 4U;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t v = luma_at(x, y);
            const size_t o = static_cast<size_t>(y) * fb->stride_y + static_cast<size_t>(x) * 4U;
            fb->bytes[o + 0] = v;
            fb->bytes[o + 1] = v;
            fb->bytes[o + 2] = v;
            fb->bytes[o + 3] = 255U;
        }
    }
    {
        float cx = 559.0f;
        float cy = 326.0f;
        CHECK(refine_eye_iris_fullres(*fb, eye_poly.data(), cx, cy));
        CHECK(std::abs(cx - iris_true_x) < 0.85f);
        CHECK(std::abs(cy - iris_true_y) < 0.85f);
    }
}

TEST_CASE("BlinkGazeStabilizer dual-stiffness C2 blink, gaze-lid follow, separated squint, and gaze spring",
          "[tracking][blink][gaze][task4]") {
    BlinkGazeStabilizer stab;
    stab.reset();

    // Warm open baseline
    BlinkGazeInput in{};
    in.dt_s = 1.0f / 60.0f;
    in.neutral_ear_left_hint = 0.30f;
    in.neutral_ear_right_hint = 0.30f;
    in.ear_left = 0.30f;
    in.ear_right = 0.30f;
    for (int f = 0; f < 15; ++f) {
        (void)stab.update(in);
    }

    // Gaze deadband (< 0.020) suppresses micro-jitter cleanly
    float max_jitter_gx = 0.0f;
    for (int f = 0; f < 10; ++f) {
        in.gaze_gx_left = (f % 2 == 0) ? 0.014f : -0.012f;
        in.gaze_gx_right = in.gaze_gx_left;
        const auto out = stab.update(in);
        max_jitter_gx = std::max(max_jitter_gx, std::abs(out.gaze_gx));
    }
    CHECK(max_jitter_gx < 0.01f);

    // Live2D Eyelid-Follows-Gaze (-22% down gaze vs +12% up gaze lift) & saccade spring
    in.gaze_gx_left = 0.0f;
    in.gaze_gx_right = 0.0f;
    in.gaze_gy_left = 0.85f;
    in.gaze_gy_right = 0.85f;
    BlinkGazeOutput down_out{};
    for (int f = 0; f < 25; ++f) {
        down_out = stab.update(in);
    }
    CHECK(down_out.gaze_gy > 0.75f);
    CHECK(down_out.blink_left > 0.08f);

    in.gaze_gy_left = -0.85f;
    in.gaze_gy_right = -0.85f;
    BlinkGazeOutput up_out{};
    for (int f = 0; f < 25; ++f) {
        up_out = stab.update(in);
    }
    CHECK(up_out.gaze_gy < -0.75f);
    CHECK(up_out.brow_lift_from_gaze > 0.04f);

    // Separated squint & cheek raise remain distinct during smiling while eye stays open
    in.gaze_gy_left = 0.0f;
    in.gaze_gy_right = 0.0f;
    in.raw_squint_left = 0.72f;
    in.raw_squint_right = 0.70f;
    in.raw_cheek_squint_left = 0.65f;
    in.raw_cheek_squint_right = 0.64f;
    BlinkGazeOutput smile_out{};
    for (int f = 0; f < 15; ++f) {
        smile_out = stab.update(in);
    }
    CHECK(smile_out.eye_squint_left > 0.55f);
    CHECK(smile_out.cheek_raise_left > 0.50f);
    CHECK(smile_out.blink_left < 0.05f);

    // Dual-stiffness blink: 22 ms snappy close reaches high blink rapidly, 55 ms open decelerates softly
    in.raw_squint_left = 0.0f;
    in.raw_squint_right = 0.0f;
    in.raw_cheek_squint_left = 0.0f;
    in.raw_cheek_squint_right = 0.0f;
    in.ear_left = 0.08f;
    in.ear_right = 0.08f;
    BlinkGazeOutput close_step{};
    for (int f = 0; f < 5; ++f) {
        close_step = stab.update(in); // ~83 ms closed
    }
    CHECK(close_step.blink_left > 0.70f);
    in.ear_left = 0.30f;
    in.ear_right = 0.30f;
    const BlinkGazeOutput open_1 = stab.update(in);
    for (int f = 0; f < 20; ++f) {
        (void)stab.update(in);
    }
    const BlinkGazeOutput settled = stab.update(in);
    CHECK(open_1.blink_left <= close_step.blink_left);
    CHECK(settled.blink_left < 0.05f);
}
