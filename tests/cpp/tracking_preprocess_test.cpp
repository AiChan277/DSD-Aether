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
