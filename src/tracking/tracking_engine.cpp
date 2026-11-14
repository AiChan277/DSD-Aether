#include "tracking/tracking_engine.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace aether::tracking {

TrackingEngine::~TrackingEngine() {
    shutdown();
}

AetherStatus TrackingEngine::initialize(const TrackingRouterConfig& router_cfg, const HardwareInventory& hw,
                                        std::unique_ptr<ICameraSource> source, std::string* out_err) {
    shutdown();
    const AetherStatus rst = router_.initialize(router_cfg, hw, out_err);
    if (rst != AETHER_OK) {
        return rst;
    }
    camera_ = std::move(source);
    if (camera_ && !camera_->is_open()) {
        const AetherStatus cst = camera_->open();
        if (cst != AETHER_OK) {
            AETHER_LOG_WARN("tracking", "Camera source failed to open (status=%d); falling back to idle frame mode",
                            static_cast<int>(cst));
            camera_.reset();
        }
    }
    standby_frame_ = std::make_unique<CapturedFrameBuffer>();
    face_roi_ = RoiRectNorm{};
    solver_.reset();
    frame_seq_ = 0;
    total_frames_ = 0;
    roi_locked_.store(false, std::memory_order_release);
    initialized_ = true;
    return AETHER_OK;
}

void TrackingEngine::shutdown() noexcept {
    if (camera_) {
        camera_->close();
        camera_.reset();
    }
    router_.shutdown();
    standby_frame_.reset();
    initialized_ = false;
}

void TrackingEngine::request_calibration() noexcept {
    calibrate_requested_.store(true, std::memory_order_release);
}

AetherStatus TrackingEngine::switch_camera_source(std::unique_ptr<ICameraSource> next_source) noexcept {
    if (camera_) {
        camera_->close();
        camera_.reset();
    }
    camera_ = std::move(next_source);
    face_roi_ = RoiRectNorm{};
    roi_locked_.store(false, std::memory_order_release);
    if (camera_ && !camera_->is_open()) {
        return camera_->open();
    }
    return AETHER_OK;
}

void TrackingEngine::step_synthetic_idle_frame(LockFreeTripleBuffer<AetherTrackingFrame>& out_buffer,
                                               uint64_t now_us) noexcept {
    AetherTrackingFrame frame{};
    frame.capture_timestamp_us = now_us;
    frame.inference_complete_us = aether::now_us();
    frame.active_execution_provider = static_cast<uint32_t>(router_.active_ep());
    frame.frame_sequence = ++frame_seq_;
    frame.valid_flags = 0U;
    for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
        frame.joint_rotations_quat[b][3] = 1.0f;
    }
    out_buffer.publish(frame);
    last_face_conf_.store(0.0f, std::memory_order_relaxed);
    last_body_conf_.store(0.0f, std::memory_order_relaxed);
    last_lhand_conf_.store(0.0f, std::memory_order_relaxed);
    last_rhand_conf_.store(0.0f, std::memory_order_relaxed);
    roi_locked_.store(false, std::memory_order_release);
}

bool TrackingEngine::step_once(LockFreeTripleBuffer<AetherTrackingFrame>& out_buffer, uint32_t wait_timeout_ms,
                               bool synthetic_fallback_when_no_frame) noexcept {
    if (!initialized_) {
        return false;
    }

    const CapturedFrameBuffer* cam_frame = nullptr;
    if (!camera_ || !camera_->read_latest_blocking(wait_timeout_ms, cam_frame) || cam_frame == nullptr) {
        // Real/open hardware or file cameras run slower than the 60 Hz polling loop (e.g., 30 FPS = 33.3 ms);
        // never clobber the active ROI/face lock with a synthetic zero-confidence idle frame between packets.
        const bool live_camera_open = (camera_ != nullptr && camera_->is_open() && !camera_->is_synthetic());
        if (synthetic_fallback_when_no_frame && !live_camera_open) {
            step_synthetic_idle_frame(out_buffer, aether::now_us());
            return true;
        }
        return false;
    }

    if (calibrate_requested_.exchange(false, std::memory_order_acq_rel)) {
        solver_.calibrate_neutral();
    }

    const uint64_t t_prep_start = aether::now_us();
    bool roi_reset_this_frame = false;
    const RoiRectNorm lb_roi = make_letterbox_square_roi(cam_frame->width, cam_frame->height);

    // 1. Acquire / verify face ROI (`M0_face_detector` runs ONLY on initialization or ROI loss `§8.3`)
    if (!face_roi_.valid) {
        roi_reset_this_frame = true;
        warp_frame_to_nchw(*cam_frame, lb_roi, 128, 128, RoiNormRange::NegOneToPosOne,
                           router_.buffers().m0_in.data(), false);
        fill_letterbox_bands_nchw(router_.buffers().m0_in.data(), 128, 128, cam_frame->width, cam_frame->height,
                                  RoiNormRange::NegOneToPosOne);
        const uint64_t t_m0_0 = aether::now_us();
        if (router_.run_m0_face_detector() == AETHER_OK) {
            RoiRectNorm m0_roi = decode_m0_best_face_roi(router_.buffers().m0_regressors.data(),
                                                         router_.buffers().m0_classif.data(), 0.30f);
            if (m0_roi.valid) {
                m0_roi.cx = lb_roi.cx + (m0_roi.cx - 0.5f) * lb_roi.width;
                m0_roi.cy = lb_roi.cy + (m0_roi.cy - 0.5f) * lb_roi.height;
                m0_roi.width *= lb_roi.width;
                m0_roi.height *= lb_roi.height;
                m0_roi.rotation_rad = 0.0f;
                face_roi_ = enforce_square_pixel_roi(m0_roi, cam_frame->width, cam_frame->height);
            }
        }
        const uint64_t t_m0_1 = aether::now_us();
        (void)t_m0_0;
        (void)t_m0_1;
        if (!face_roi_.valid) {
            face_roi_ = detect_skin_centroid_roi(*cam_frame);
        }
        // If no real face in synthetic/empty pattern yet, fall back to centered aspect-square nominal crop
        if (!face_roi_.valid) {
            face_roi_ = enforce_square_pixel_roi(RoiRectNorm{0.5f, 0.42f, 0.40f, 0.40f, 0.0f, true}, cam_frame->width,
                                                 cam_frame->height);
        }
    }

    // 2. Run M1_body on 2-frame cadence (always on first frame or face loss) and M2 face warp every frame
    const bool run_body_this_frame = ((frame_seq_ & 1U) == 0U) || roi_reset_this_frame || !roi_locked_.load(std::memory_order_acquire);
    if (run_body_this_frame) {
        warp_frame_to_nchw(*cam_frame, lb_roi, 256, 256, RoiNormRange::ZeroToOne, router_.buffers().m1_body_in.data(),
                           false);
        fill_letterbox_bands_nchw(router_.buffers().m1_body_in.data(), 256, 256, cam_frame->width, cam_frame->height,
                                  RoiNormRange::ZeroToOne);
    }
    warp_frame_to_nchw(*cam_frame, face_roi_, 256, 256, RoiNormRange::ZeroToOne, router_.buffers().m2_face_in.data(),
                       false);
    const uint64_t t_prep_end = aether::now_us();

    // 3. Execute M1_body (when scheduled) + M2_face_landmarks
    const uint64_t t_inf_start = aether::now_us();
    if (run_body_this_frame && router_.run_m1_body() != AETHER_OK) {
        return false;
    }
    if (router_.run_m2_face_landmarks() != AETHER_OK) {
        return false;
    }

    // 4. Conditional M1_hands (`batch=2` 224x224) — hard-bypassed when hand tracking is disabled
    const float pose_flag = router_.buffers().m1_body_flag[0];
    HandRoiDecision hand_rois{};
    if (router_.config().enable_hand_tracking) {
        hand_rois = compute_hand_rois_from_pose(router_.buffers().m1_body_lm.data(), pose_flag, 0.50f, 0.05f);
        auto unletterbox_hand_roi = [&](RoiRectNorm& r, bool& active) {
            if (!active) {
                return;
            }
            r.cx = lb_roi.cx + (r.cx - 0.5f) * lb_roi.width;
            r.cy = lb_roi.cy + (r.cy - 0.5f) * lb_roi.height;
            r.width *= lb_roi.width;
            r.height *= lb_roi.height;
            if (r.cx < 0.05f || r.cx > 0.95f || r.cy < 0.05f || r.cy > 0.88f) {
                active = false;
                return;
            }
            r = enforce_square_pixel_roi(r, cam_frame->width, cam_frame->height, 0.14f, 0.48f);
        };
        unletterbox_hand_roi(hand_rois.left_roi, hand_rois.run_left);
        unletterbox_hand_roi(hand_rois.right_roi, hand_rois.run_right);
        if (hand_rois.run_left || hand_rois.run_right) {
            const size_t hand_plane_floats = 3ULL * 224ULL * 224ULL;
            if (hand_rois.run_left) {
                warp_frame_to_nchw(*cam_frame, hand_rois.left_roi, 224, 224, RoiNormRange::ZeroToOne,
                                   router_.buffers().m1_hands_in.data(), false);
            } else {
                std::memset(router_.buffers().m1_hands_in.data(), 0, hand_plane_floats * sizeof(float));
            }
            if (hand_rois.run_right) {
                warp_frame_to_nchw(*cam_frame, hand_rois.right_roi, 224, 224, RoiNormRange::ZeroToOne,
                                   router_.buffers().m1_hands_in.data() + hand_plane_floats, false);
            } else {
                std::memset(router_.buffers().m1_hands_in.data() + hand_plane_floats, 0,
                            hand_plane_floats * sizeof(float));
            }
            (void)router_.run_m1_hands();
        }
    }

    // 5. Extract 146x2 subset from M2 -> execute M3 blendshapes MLP
    const float face_flag = router_.buffers().m2_face_flag[0];
    if (face_flag >= 0.45f) {
        extract_m3_subset_146x2(router_.buffers().m2_face_lm.data(), 256.0f, router_.buffers().m3_in.data());
        (void)router_.run_m3_blendshapes();
    } else {
        std::fill(router_.buffers().m3_blendshapes.begin(), router_.buffers().m3_blendshapes.end(), 0.0f);
    }

    const uint64_t t_inf_end = aether::now_us();

    // Update face ROI tracking state with 2.5% hysteresis deadband; recover via M1 head landmarks if M2 missed
    const RoiRectNorm current_face_roi = face_roi_;
    face_roi_ = solver_.compute_next_face_roi(router_.buffers().m2_face_lm.data(), current_face_roi, face_flag,
                                              cam_frame->width, cam_frame->height);
    if (!face_roi_.valid) {
        face_roi_ = solver_.estimate_face_roi_from_m1(router_.buffers().m1_body_lm.data(), lb_roi, pose_flag,
                                                      cam_frame->width, cam_frame->height);
    }
    roi_locked_.store(face_roi_.valid, std::memory_order_release);

    // 6. Solve rigid skull solvePnP + EAR/MAR + Iris + Upper Body + Hands directly into triple buffer
    AetherTrackingFrame out_frame{};
    solver_.populate_tracking_frame(
        current_face_roi, router_.buffers().m2_face_lm.data(), face_flag, router_.buffers().m3_blendshapes.data(),
        router_.buffers().m1_body_world.data(), router_.buffers().m1_body_lm.data(), pose_flag, hand_rois.run_left,
        hand_rois.run_right, router_.buffers().m1_hands_lm.data(), router_.buffers().m1_hands_flag.data(),
        router_.buffers().m1_hands_handed.data(), cam_frame->capture_timestamp_us, t_inf_end, router_.active_ep(),
        ++frame_seq_, roi_reset_this_frame, out_frame, cam_frame->width, cam_frame->height);

    out_buffer.publish(out_frame);
    ++total_frames_;

    const TrackingOverlaySnapshot& ov = solver_.overlay_snapshot();
    neutral_calibrated_.store(ov.neutral_calibrated, std::memory_order_relaxed);
    stable_lock_frames_.store(ov.stable_lock_frames, std::memory_order_relaxed);
    last_head_pitch_deg_.store(ov.head_euler_deg.pitch, std::memory_order_relaxed);
    last_head_yaw_deg_.store(ov.head_euler_deg.yaw, std::memory_order_relaxed);
    last_head_roll_deg_.store(ov.head_euler_deg.roll, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(preview_mutex_);
        preview_slot_.sequence = total_frames_;
        preview_slot_.src_width = cam_frame->width;
        preview_slot_.src_height = cam_frame->height;
        preview_slot_.overlay = ov;
        downscale_camera_to_rgba8(*cam_frame, LiveCameraPreviewFrame::kPreviewWidth,
                                  LiveCameraPreviewFrame::kPreviewHeight, preview_slot_.rgba8.data());
        preview_slot_.valid = true;
    }

    const uint64_t prep_us = t_prep_end - t_prep_start;
    const uint64_t infer_us = t_inf_end - t_inf_start;
    const uint64_t total_us = t_inf_end - t_prep_start;
    last_prep_us_scaled_.store(prep_us, std::memory_order_relaxed);
    last_infer_us_scaled_.store(infer_us, std::memory_order_relaxed);
    last_total_us_scaled_.store(total_us, std::memory_order_relaxed);
    last_face_conf_.store(face_flag >= 0.45f ? std::clamp(face_flag, 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    last_body_conf_.store(out_frame.upper_body_confidence, std::memory_order_relaxed);
    last_lhand_conf_.store(out_frame.left_hand_confidence, std::memory_order_relaxed);
    last_rhand_conf_.store(out_frame.right_hand_confidence, std::memory_order_relaxed);

    // Feed health monitor (`§8.4`: auto demotes EP if rolling avg > 9.0 ms)
    std::string demotion_msg;
    router_.record_frame_latency(static_cast<double>(infer_us) / 1000.0, &demotion_msg);
    if (!demotion_msg.empty()) {
        AETHER_LOG_WARN("tracking", "%s", demotion_msg.c_str());
    }
    return true;
}

TrackingStatsSnapshot TrackingEngine::snapshot_stats() const {
    TrackingStatsSnapshot s{};
    s.frames_processed = total_frames_;
    s.last_preprocess_ms = static_cast<double>(last_prep_us_scaled_.load(std::memory_order_relaxed)) / 1000.0;
    s.last_inference_ms = static_cast<double>(last_infer_us_scaled_.load(std::memory_order_relaxed)) / 1000.0;
    s.last_total_t1_ms = static_cast<double>(last_total_us_scaled_.load(std::memory_order_relaxed)) / 1000.0;
    s.rolling_infer_ms = router_.rolling_avg_ms();
    s.active_ep = router_.active_ep();
    s.active_ep_label = router_.active_ep_label();
    const bool cam_open = camera_ && camera_->is_open();
    s.camera_active = cam_open;
    if (cam_open) {
        s.camera_label = camera_->source_label();
        s.camera_symlink = camera_->device_symlink();
        s.camera_width = camera_->width();
        s.camera_height = camera_->height();
        s.camera_fps = camera_->fps();
    } else {
        s.camera_label = "Camera Stopped";
    }
    s.demotion_count = router_.demotion_count();
    s.face_confidence = last_face_conf_.load(std::memory_order_relaxed);
    s.upper_body_confidence = last_body_conf_.load(std::memory_order_relaxed);
    s.left_hand_confidence = last_lhand_conf_.load(std::memory_order_relaxed);
    s.right_hand_confidence = last_rhand_conf_.load(std::memory_order_relaxed);
    s.roi_locked = roi_locked_.load(std::memory_order_relaxed);
    s.neutral_calibrated = neutral_calibrated_.load(std::memory_order_relaxed);
    s.stable_lock_frames = stable_lock_frames_.load(std::memory_order_relaxed);
    s.head_euler_deg[0] = last_head_pitch_deg_.load(std::memory_order_relaxed);
    s.head_euler_deg[1] = last_head_yaw_deg_.load(std::memory_order_relaxed);
    s.head_euler_deg[2] = last_head_roll_deg_.load(std::memory_order_relaxed);
    return s;
}

bool TrackingEngine::copy_live_preview(LiveCameraPreviewFrame& out_preview) const noexcept {
    std::lock_guard<std::mutex> lk(preview_mutex_);
    if (!preview_slot_.valid) {
        out_preview.valid = false;
        return false;
    }
    out_preview = preview_slot_;
    return true;
}

} // namespace aether::tracking
