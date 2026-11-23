#pragma once

#include "aether_ffi_contract.h"
#include "animation/live2d_params.hpp"
#include "tracking/blink_gaze_filter.hpp"
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
    std::array<float, 68U * 2U> keypoints_norm{};
    uint32_t keypoint_count{0};
    // Normalized [0..1] camera coords: [0]=subject right iris (468, image-left), [1]=subject left iris (473,
    // image-right)
    std::array<float, 2U * 2U> iris_centers_norm{};
    bool iris_valid{false};
    std::array<float, 8U * 2U> left_eye_poly_norm{};
    std::array<float, 8U * 2U> right_eye_poly_norm{};
    bool eye_poly_valid{false};
    std::array<float, 5U * 2U> left_brow_pts_norm{};
    std::array<float, 5U * 2U> right_brow_pts_norm{};
    bool brow_pts_valid{false};
    std::array<float, 2U> gaze_vec{0.0f, 0.0f};
    // 4 points in normalized [0..1] camera coords: [0]=nose origin, [1]=+X (right), [2]=+Y (up), [3]=+Z (fwd)
    std::array<float, 4U * 2U> head_axes_norm{};
    bool head_axes_valid{false};
    bool face_locked{false};
    bool neutral_calibrated{false};
    uint32_t stable_lock_frames{0};
    HeadEulerDeg head_euler_deg{};
    bool hand_tracking_enabled{false};
    bool left_hand_detected{false};
    bool right_hand_detected{false};
    std::array<float, 4U> left_hand_bbox_obb{};
    float left_hand_angle_rad{0.0f};
    std::array<float, 2U> left_palm_center_norm{};
    std::array<float, 2U> left_palm_normal_2d{};
    std::array<float, 21U * 2U> left_hand_keypoints_norm{};
    std::array<float, 4U> right_hand_bbox_obb{};
    float right_hand_angle_rad{0.0f};
    std::array<float, 2U> right_palm_center_norm{};
    std::array<float, 2U> right_palm_normal_2d{};
    std::array<float, 21U * 2U> right_hand_keypoints_norm{};
    animation::Live2DParameters live2d{};
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
[[nodiscard]] HeadPose6D solve_pnp_head_pose(const float* landmarks_478x3, const RoiRectNorm& face_roi, uint32_t cam_w,
                                             uint32_t cam_h, const HeadPose6D* warm_start = nullptr) noexcept;

/** 2D image point in camera pixels (`y` down). */
struct FacePlanePoint2D {
    float x{0.0f};
    float y{0.0f};
};

/**
 * Kalidokit `calcHead`-style facial-plane head angles (degrees, camera convention: +yaw = nose toward image right,
 * +pitch = nose down, +roll = counter-clockwise in a y-up view) from five camera-pixel landmarks:
 * eye outer corners (MediaPipe 33 = image-left, 263 = image-right), nose bridge (168), nose tip (4), chin (152).
 * `neutral_pitch_ratio` is the calibrated `(nose_tip.y - eye_mid.y) / (chin.y - eye_mid.y)` at rest.
 * Used as the fallback whenever the rigid solvePnP fit has no solution.
 */
[[nodiscard]] HeadEulerDeg kalido_calc_head_plane(FacePlanePoint2D eye_img_left, FacePlanePoint2D eye_img_right,
                                                  FacePlanePoint2D nose_bridge, FacePlanePoint2D nose_tip,
                                                  FacePlanePoint2D chin, float neutral_pitch_ratio,
                                                  float& out_pitch_ratio) noexcept;

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

    void populate_tracking_frame(
        const RoiRectNorm& face_roi, const float* m2_landmarks_478x3, float face_flag, const float* m3_blendshapes_52,
        const float* m1_body_world_39x3, const float* m1_body_lm_39x5, float pose_flag, bool run_left_hand,
        bool run_right_hand, const float* m1_hands_lm_2x63, const float* m1_hands_flag_2,
        const float* m1_hands_handed_2, uint64_t capture_timestamp_us, uint64_t inference_complete_us, EpTier active_ep,
        uint32_t frame_sequence, bool roi_reset_this_frame, AetherTrackingFrame& out_frame, uint32_t cam_w = 1280U,
        uint32_t cam_h = 720U, bool enable_hand_tracking = false, const CapturedFrameBuffer* raw_frame = nullptr,
        const RoiRectNorm* left_hand_roi = nullptr, const RoiRectNorm* right_hand_roi = nullptr,
        const float* m1_hands_world_2x63 = nullptr) noexcept;

    [[nodiscard]] const TrackingOverlaySnapshot& overlay_snapshot() const noexcept { return overlay_; }
    [[nodiscard]] bool neutral_calibrated() const noexcept { return neutral_calibrated_; }
    [[nodiscard]] uint32_t stable_lock_frames() const noexcept { return stable_lock_frames_; }
    [[nodiscard]] HeadEulerDeg last_head_euler_deg() const noexcept { return overlay_.head_euler_deg; }

  private:
    struct LandmarkFilter2D {
        float win_u[3]{};
        float win_v[3]{};
        float x_u{0.0f};
        float dx_u{0.0f};
        float x_v{0.0f};
        float dx_v{0.0f};
    };

    bool neutral_calibrated_{false};
    float neutral_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float neutral_head_pos_[3]{0.0f, 0.0f, 0.0f};
    bool calibrate_next_{false};

    uint32_t stable_lock_frames_{0};
    float neutral_ear_l_{0.28f};
    float neutral_ear_r_{0.28f};
    float neutral_mar_open_{0.04f};
    float neutral_mouth_height_ratio_{0.0616f};
    float neutral_mouth_width_ratio_{0.5525f};
    float neutral_plane_pitch_ratio_{0.42f};
    std::array<float, 5> neutral_brow_l_{0.268f, 0.275f, 0.279f, 0.235f, 0.183f};
    std::array<float, 5> neutral_brow_r_{0.268f, 0.275f, 0.279f, 0.235f, 0.183f};

    std::array<LandmarkFilter2D, 478U> lm_filters_{};
    std::array<float, 478U * 3U> filtered_lm_buf_{};
    uint64_t last_lm_filter_ts_us_{0};
    bool lm_filter_primed_{false};

    HeadPose6D last_pnp_pose_{};
    float prev_raw_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool has_prev_raw_quat_{false};
    float last_emitted_head_quat_[4]{0.0f, 0.0f, 0.0f, 1.0f};
    bool has_emitted_head_quat_{false};
    BlinkGazeStabilizer blink_gaze_{};
    TrackingOverlaySnapshot overlay_{};
};

} // namespace aether::tracking
