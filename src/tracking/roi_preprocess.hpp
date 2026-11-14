#pragma once

#include "tracking/camera_source.hpp"

#include <array>
#include <cstdint>

namespace aether::tracking {

struct RoiRectNorm {
    float cx{0.5f};
    float cy{0.5f};
    float width{1.0f};
    float height{1.0f};
    float rotation_rad{0.0f};
    bool valid{false};
};

enum class RoiNormRange : uint8_t {
    ZeroToOne = 0,     // [0, 1] for M1_body, M1_hands, M2_face
    NegOneToPosOne = 1 // [-1, +1] for M0 BlazeFace
};

/**
 * SIMD AVX2 + scalar NV12/BGRA -> NCHW FP32 ROI affine warp & colour conversion (`P3-02`, `HR-01`).
 * Zero heap allocations: writes directly into pre-allocated ORT tensor buffers cleanly.
 * Target budget: `<= 0.6 ms` per 1280x720 source frame across all active crops.
 */
void warp_frame_to_nchw(const CapturedFrameBuffer& src, const RoiRectNorm& roi, uint32_t dst_w, uint32_t dst_h,
                        RoiNormRange range, float* dst_nchw, bool force_scalar = false) noexcept;

/**
 * Extracts the exact 146 MediaPipe landmark subset `(x_roi_px, y_roi_px)` from M2's `[478 x 3]` output
 * into M3's `[1 x 146 x 2]` tensor buffer cleanly (coordinates in M2's `[0..256]` pixel space).
 */
void extract_m3_subset_146x2(const float* m2_landmarks_478x3, float m2_norm_scale, float* dst_146x2) noexcept;

} // namespace aether::tracking
