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
    float reproj_rms_px{0.0f};
    bool valid{false};
};

struct HeadEulerDeg {
    float pitch{0.0f};
    float yaw{0.0f};
    float roll{0.0f};
};

struct TrackingOverlaySnapshot {
    RoiRectNorm face_roi{};
    std::array<float, 24U * 2U> keypoints_norm{};
    uint32_t keypoint_count{0};
    // 4 points in normalized [0..1] camera coords: [0]=nose origin, [1]=+X (right), [2]=+Y (up), [3]=+Z (fwd)
    std::array<float, 4U * 2U> head_axes_norm{};
    bool head_axes_valid{false};
    bool face_locked{false};
    bool neutral_calibrated{false};
    uint32_t stable_lock_frames{0};
    HeadEulerDeg head_euler_deg{};
};

/**
 * Computes rigid head rotation & translation (in metres) by weighted Kabsch/Procrustes
 * alignment from observed 468 3D face landmarks onto canonical Apache-2.0 MediaPipe 3D face geometry.
 */
[[nodiscard]] HeadPose6D solve_procrustes_head_pose(const float* landmarks_478x3,
                                                    float aspect_w_over_h = 1.0f) noexcept;

/**
 * Classic 3D-to-2D rigid skull iterative Levenberg-Marquardt `solvePnP` head pose solver.
 * Consumes ONLY M2 2D `(u, v)` crop coordinates mapped into full-camera pixels through a
 * pinhole intrinsic matrix (`fx = fy = cam_w`, `cx = cam_w * 0.5`, `cy = cam_h * 0.5`),
 * aligned against rigid non-deforming canonical 3D skull vertices.
 */
[[nodiscard]] HeadPose6D solve_pnp_head_pose(const float* landmarks_478x3, const RoiRectNorm& face_roi,
                                             uint32_t cam_w, uint32_t cam_h,
                                             const HeadPose6D* warm_start = nullptr) noexcept;

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
    static constexpr uint32_t kAutoNeutralStableFrames = 15U;

    void reset() noexcept;
    void calibrate_neutral() noexcept;

    [[nodiscard]] RoiRectNorm compute_next_face_roi(const float* m2_landmarks_478x3, const RoiRectNorm& prev_face_roi,
                                                    float face_flag, uint32_t cam_w = 1280U,
                                                    uint32_t cam_h = 720U) const noexcept;

    [[nodiscard]] RoiRectNorm estimate_face_roi_from_m1(const float* m1_body_lm_39x5, const RoiRectNorm& lb_roi,
                                                        float pose_flag, uint32_t cam_w = 1280U,
                                                        uint32_t cam_h = 720U) const noexcept;

    void populate_tracking_frame(const RoiRectNorm& face_roi, const float* m2_landmarks_478x3, float face_flag,
                                 const float* m3_blendshapes_52, const float* m1_body_world_39x3,
                                 const float* m1_body_lm_39x5, float pose_flag, bool run_left_hand, bool run_right_hand,
                                 const float* m1_hands_lm_2x63, const float* m1_hands_flag_2,
                                 const float* m1_hands_handed_2, uint64_t capture_timestamp_us,
                                 uint64_t inference_complete_us, EpTier active_ep, uint32_t frame_sequence,
                                 bool roi_reset_this_frame, AetherTrackingFrame& out_frame,
                                 uint32_t cam_w = 1280U, uint32_t cam_h = 720U) noexcept;

    [[nodiscard]] const TrackingOverlaySnapshot& overlay_snapshot() const noexcept { return overlay_; }
    [[nodiscard]] bool neutral_calibrated() const noexcept { return neutral_calibrated_; }
    [[nodiscard]] uint32_t stable_lock_frames() const noexcept { return stable_lock_frames_; }
    [[nodiscard]] HeadEulerDeg last_head_euler_deg() const noexcept { return overlay_.head_euler_deg; }

  private:
    bool neutral_calibrated_{false};
    float neutral_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float neutral_head_pos_[3]{0.0f, 0.0f, 0.0f};
    bool calibrate_next_{false};

    uint32_t stable_lock_frames_{0};
    float neutral_ear_l_{0.28f};
    float neutral_ear_r_{0.28f};
    float neutral_mar_open_{0.04f};
    float neutral_mouth_width_ratio_{0.52f};
    bool blink_closed_l_{false};
    bool blink_closed_r_{false};

    HeadPose6D last_pnp_pose_{};
    float prev_raw_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool has_prev_raw_quat_{false};
    float last_emitted_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool has_emitted_head_quat_{false};
    TrackingOverlaySnapshot overlay_{};
};

} // namespace aether::tracking
