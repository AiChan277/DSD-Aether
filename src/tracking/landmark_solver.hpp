#pragma once

#include "aether_ffi_contract.h"
#include "tracking/manifest.hpp"
#include "tracking/roi_preprocess.hpp"

#include <array>
#include <cstdint>

namespace aether::tracking {

struct HeadPose6D {
    float rotation_xyzw[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float position_m[3]{0.0f, 0.0f, 0.0f};
    bool valid{false};
};

/**
 * Computes rigid head rotation & translation (in metres) by weighted Kabsch/Procrustes
 * alignment from observed 468 3D face landmarks onto canonical Apache-2.0 MediaPipe 3D face geometry.
 */
[[nodiscard]] HeadPose6D solve_procrustes_head_pose(const float* landmarks_478x3,
                                                    float aspect_w_over_h = 1.0f) noexcept;

/**
 * Decodes M0 BlazeFace SSD outputs (`regressors [896x16]`, `classificators [896x1]`) and returns
 * best normalized face ROI rectangle (`valid == false` if max confidence < `min_score`).
 */
[[nodiscard]] RoiRectNorm decode_m0_best_face_roi(const float* regressors_896x16, const float* classificators_896x1,
                                                  float min_score = 0.50f) noexcept;

/**
 * Builds enlarged + padded normalized hand ROI boxes from `M1_body` wrist/index/pinky landmarks,
 * applying the 5% frame-border safety margin & `>= 0.30` confidence gate (`P3-02`).
 */
struct HandRoiDecision {
    RoiRectNorm left_roi{};
    RoiRectNorm right_roi{};
    bool run_left{false};
    bool run_right{false};
};

[[nodiscard]] HandRoiDecision compute_hand_rois_from_pose(const float* pose_landmarks_39x5, float pose_flag,
                                                          float min_conf = 0.30f, float border_margin = 0.05f) noexcept;

/**
 * Stateful landmark-to-`AetherTrackingFrame` solver: handles neutral pose calibration,
 * Procrustes 6-DoF head pose, iris gaze (`468..477`), ARKit-52 remapping, upper-body bone
 * orientation solving, and hand finger curl/spread onto the 55-bone frozen humanoid rig.
 */
class LandmarkPoseSolver {
  public:
    void reset() noexcept;
    void calibrate_neutral() noexcept;

    [[nodiscard]] RoiRectNorm compute_next_face_roi(const float* m2_landmarks_478x3, const RoiRectNorm& prev_face_roi,
                                                    float face_flag) const noexcept;

    void populate_tracking_frame(const RoiRectNorm& face_roi, const float* m2_landmarks_478x3, float face_flag,
                                 const float* m3_blendshapes_52, const float* m1_body_world_39x3,
                                 const float* m1_body_lm_39x5, float pose_flag, bool run_left_hand, bool run_right_hand,
                                 const float* m1_hands_lm_2x63, const float* m1_hands_flag_2,
                                 const float* m1_hands_handed_2, uint64_t capture_timestamp_us,
                                 uint64_t inference_complete_us, EpTier active_ep, uint32_t frame_sequence,
                                 bool roi_reset_this_frame, AetherTrackingFrame& out_frame) noexcept;

  private:
    bool neutral_calibrated_{false};
    float neutral_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float neutral_head_pos_[3]{0.0f, 0.0f, 0.0f};
    bool calibrate_next_{false};
};

} // namespace aether::tracking
