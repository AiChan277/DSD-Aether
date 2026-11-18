#include "app/pipeline_skeleton.hpp"

#include "core/clock.hpp"
#include "core/expected.hpp"
#include "core/hot_path_guard.hpp"
#include "core/log.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <intrin.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::app {

namespace {

std::filesystem::path resolve_manifest_path(const std::filesystem::path& configured) {
    std::error_code ec;
    if (!configured.empty() && std::filesystem::is_regular_file(configured, ec)) {
        return configured;
    }
    const char* candidates[4] = {
        "models/manifest.toml",
        "../models/manifest.toml",
        "../../models/manifest.toml",
#if defined(AETHER_SOURCE_DIR)
        AETHER_SOURCE_DIR "/models/manifest.toml",
#else
        "models/manifest.toml",
#endif
    };
    for (const char* p : candidates) {
        if (std::filesystem::is_regular_file(p, ec)) {
            return std::filesystem::path(p);
        }
    }
    return {};
}

std::filesystem::path resolve_ov_cache_dir(const std::filesystem::path& configured) {
    if (!configured.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(configured, ec);
        return configured;
    }
    auto p = std::filesystem::temp_directory_path() / "aether_studio_ov_cache";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

std::unique_ptr<tracking::ICameraSource> open_camera_source_with_fallback(bool camera_active, bool want_synthetic,
                                                                     const std::string& preferred_link, uint32_t fps,
                                                                     bool& out_is_synthetic) {
    if (!camera_active) {
        out_is_synthetic = want_synthetic;
        return nullptr;
    }
    const uint32_t target_fps = (fps >= 45U) ? 60U : 30U;
    if (!want_synthetic) {
        std::string chosen_link = preferred_link;
        if (chosen_link.empty()) {
            const auto devices = tracking::enumerate_camera_devices();
            if (!devices.empty()) {
                chosen_link = devices.front().symbolic_link;
            }
        }
        auto cam = tracking::create_mf_camera_source(chosen_link, 1280, 720, target_fps);
        if (cam && cam->open() == AETHER_OK) {
            out_is_synthetic = false;
            return cam;
        }
        AETHER_LOG_WARN("tracking", "No physical webcam opened cleanly; staying in neutral camera-stopped idle state");
        out_is_synthetic = false;
        return nullptr;
    }
    auto syn = tracking::create_file_camera_source({}, true, 1280, 720, target_fps, 0);
    if (syn) {
        (void)syn->open();
    }
    out_is_synthetic = true;
    return syn;
}

} // namespace

PipelineSkeleton::~PipelineSkeleton() {
    stop();
}

bool PipelineSkeleton::start(const CpuTopology& topology) {
    HardwareInventory hw{};
    hw.cpu = topology;
    return start(hw, PipelineConfig{});
}

bool PipelineSkeleton::start(const HardwareInventory& hw, const PipelineConfig& cfg) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

#if defined(_WIN32)
    pose_ready_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    output_ready_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pose_ready_event_ == nullptr || output_ready_event_ == nullptr || stop_event_ == nullptr) {
        stop();
        return false;
    }
#endif

    hw_ = hw;
    topology_ = hw.cpu;
    cfg_ = cfg;
    sim_engine_ = std::make_unique<animation::SimEngine>();
    sim_engine_->reset();
    if (cfg_.enable_live_tracking) {
        audio_lipsync_ = std::make_unique<tracking::WasapiAudioLipsync>();
        (void)audio_lipsync_->start(cfg_.preferred_mic_id, cfg_.mic_gain, cfg_.mic_noise_gate_db,
                                    cfg_.enable_audio_lipsync);
    }
    (void)dsp_reader_bridge_.init();
    t3_dsp_stale_count_ = 0;
    pending_output_slots_.store(0u, std::memory_order_release);
    (void)output_cfg_mailbox_.store(cfg_.output_cfg);
    vmc_enabled_.store(cfg_.output_cfg.osc_enabled, std::memory_order_release);
    output_status_buffer_.publish(output::OutputRuntimeStatus{});

    TrackingHudSnapshot init_hud{};
    init_hud.enabled = cfg_.enable_live_tracking;
    init_hud.ready = false;
    init_hud.using_synthetic_camera = cfg_.use_synthetic_camera;
    init_hud.camera_active = cfg_.camera_active;
    init_hud.selected_ep_override = cfg_.preferred_ep;
    std::snprintf(init_hud.active_ep_label, sizeof(init_hud.active_ep_label), "%s",
                  cfg_.enable_live_tracking ? "Initializing..." : "Synthetic Only");
    std::snprintf(init_hud.camera_label, sizeof(init_hud.camera_label), "%s",
                  !cfg_.camera_active ? "Camera Stopped"
                                      : (cfg_.use_synthetic_camera ? "Synthetic 60Hz" : "Camera Opening..."));
    tracking_hud_buffer_.publish(init_hud);

    running_.store(true, std::memory_order_release);
    t1_thread_ = std::thread(&PipelineSkeleton::t1_entry, this);
    t2_thread_ = std::thread(&PipelineSkeleton::t2_entry, this);
    t4_thread_ = std::thread(&PipelineSkeleton::t4_entry, this);
    return true;
}

void PipelineSkeleton::stop() noexcept {
    running_.store(false, std::memory_order_release);
#if defined(_WIN32)
    if (stop_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(stop_event_));
    }
    if (pose_ready_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(pose_ready_event_));
    }
    if (output_ready_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(output_ready_event_));
    }
#endif
    if (t1_thread_.joinable()) {
        t1_thread_.join();
    }
    if (t2_thread_.joinable()) {
        t2_thread_.join();
    }
    if (t4_thread_.joinable()) {
        t4_thread_.join();
    }
    if (audio_lipsync_) {
        audio_lipsync_->stop();
        audio_lipsync_.reset();
    }
    dsp_reader_bridge_.shutdown();
    sim_engine_.reset();
#if defined(_WIN32)
    if (pose_ready_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(pose_ready_event_));
        pose_ready_event_ = nullptr;
    }
    if (output_ready_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(output_ready_event_));
        output_ready_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
#endif
}

bool PipelineSkeleton::acquire_skinning(SkinningSnapshot& out) noexcept {
    const bool updated = skinning_buffer_.acquire();
    out = skinning_buffer_.latest();
    return updated;
}

const SkinningSnapshot& PipelineSkeleton::acquire_skinning_ref(bool* out_updated) noexcept {
    const bool updated = skinning_buffer_.acquire();
    if (out_updated != nullptr) {
        *out_updated = updated;
    }
    return skinning_buffer_.latest();
}

void PipelineSkeleton::submit_rendered_frame(const OutputWorkItem& item) noexcept {
    if (output_ring_.try_push(item)) {
#if defined(_WIN32)
        if (output_ready_event_ != nullptr) {
            ::SetEvent(static_cast<HANDLE>(output_ready_event_));
        }
#endif
    }
}

void PipelineSkeleton::submit_output_slot(const aether::output::OutputRingSlot& slot) noexcept {
    pending_output_slots_.fetch_add(1u, std::memory_order_acq_rel);
    if (output_slot_ring_.try_push(slot)) {
#if defined(_WIN32)
        if (output_ready_event_ != nullptr) {
            ::SetEvent(static_cast<HANDLE>(output_ready_event_));
        }
#endif
    } else {
        pending_output_slots_.fetch_sub(1u, std::memory_order_acq_rel);
    }
}

void PipelineSkeleton::update_output_config(const aether::output::OutputConfig& cfg) noexcept {
    (void)output_cfg_mailbox_.store(cfg);
    vmc_enabled_.store(cfg.osc_enabled, std::memory_order_release);
#if defined(_WIN32)
    if (output_ready_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(output_ready_event_));
    }
#endif
}

bool PipelineSkeleton::wait_output_slots_drained(uint32_t max_wait_ms) noexcept {
    const uint64_t deadline_us = now_us() + static_cast<uint64_t>(max_wait_ms) * 1000ULL;
    while (pending_output_slots_.load(std::memory_order_acquire) > 0u) {
        if (!running_.load(std::memory_order_acquire) || now_us() >= deadline_us) {
            return pending_output_slots_.load(std::memory_order_acquire) == 0u;
        }
        std::this_thread::yield();
    }
    return true;
}

bool PipelineSkeleton::try_read_latest_dsp(AetherDspSyncPacket& out) noexcept {
    return output::DspSyncBridge::try_read_dsp_line1(dsp_reader_bridge_.mapped_packet(), now_us(), out,
                                                     t3_dsp_stale_count_);
}

output::OutputRuntimeStatus PipelineSkeleton::latest_output_status() noexcept {
    (void)output_status_buffer_.acquire();
    return output_status_buffer_.latest();
}

void PipelineSkeleton::push_rest_skeleton(const animation::RestSkeletonMetrics& rest) noexcept {
    (void)rest_metrics_mailbox_.store(rest);
}

void PipelineSkeleton::send_tracking_command(const TrackingControlCommand& cmd) noexcept {
    (void)tracking_cmd_ring_.try_push(cmd);
}

void PipelineSkeleton::set_audio_lipsync_params(bool assist_enabled, float mic_gain, float noise_gate_db) noexcept {
    if (audio_lipsync_) {
        audio_lipsync_->set_assist_enabled(assist_enabled);
        audio_lipsync_->set_gain(mic_gain);
        audio_lipsync_->set_noise_gate_db(noise_gate_db);
    }
}

TrackingHudSnapshot PipelineSkeleton::latest_tracking_hud() noexcept {
    (void)tracking_hud_buffer_.acquire();
    return tracking_hud_buffer_.latest();
}

bool PipelineSkeleton::acquire_live_preview(tracking::LiveCameraPreviewFrame& out) noexcept {
    (void)live_preview_buffer_.acquire();
    const auto& latest = live_preview_buffer_.latest();
    if (!latest.valid || latest.sequence == out.sequence) {
        return false;
    }
    out = latest;
    return true;
}

TelemetrySnapshot PipelineSkeleton::latest_telemetry() noexcept {
    (void)telemetry_hub_.acquire();
    return telemetry_hub_.latest();
}

void PipelineSkeleton::t1_entry() {
    (void)apply_thread_placement(ThreadRole::TrackingT1, topology_);
    HighResTimer timer;
    constexpr uint64_t kPeriodUs = 16667ULL; // 60 Hz pacing

    if (!cfg_.enable_live_tracking) {
        uint64_t next_tick = now_us();
        uint32_t seq = 0;

        while (running_.load(std::memory_order_acquire)) {
            next_tick += kPeriodUs;
            {
                AETHER_HOT_PATH_SCOPE;
                AETHER_ZONE("T1_SyntheticCapture");
                const uint64_t cap_us = now_us();
                AetherTrackingFrame& frame = pose_buffer_.staging();
                std::memset(&frame, 0, sizeof(frame));
                frame.capture_timestamp_us = cap_us;
                frame.inference_complete_us = cap_us;
                frame.active_execution_provider = AETHER_EP_CPU;
                frame.frame_sequence = ++seq;
                frame.valid_flags = 0U;
                for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                    frame.joint_rotations_quat[b][3] = 1.0f;
                }
                pose_buffer_.publish();
                t1_frames_.store(seq, std::memory_order_release);

#if defined(_WIN32)
                if (pose_ready_event_ != nullptr) {
                    ::SetEvent(static_cast<HANDLE>(pose_ready_event_));
                }
#endif
            }
            timer.wait_until_us(next_tick);
        }
        return;
    }

    const std::filesystem::path manifest = resolve_manifest_path(cfg_.manifest_path);
    const std::filesystem::path ov_cache = resolve_ov_cache_dir(cfg_.ov_cache_dir);
    bool use_synthetic = cfg_.use_synthetic_camera;
    bool camera_active = cfg_.camera_active;
    uint32_t camera_fps = (cfg_.camera_fps >= 45U) ? 60U : 30U;
    std::string preferred_camera_link = cfg_.preferred_camera_link;
    int32_t ep_override = cfg_.preferred_ep;

    auto init_engine = [&](tracking::TrackingEngine& eng, std::string& out_err) -> bool {
        if (manifest.empty()) {
            out_err = "models/manifest.toml not found";
            return false;
        }
        tracking::TrackingRouterConfig rcfg{};
        rcfg.manifest_path = manifest;
        rcfg.ov_cache_dir = ov_cache;
        rcfg.preferred_ep =
            (ep_override >= 0) ? static_cast<tracking::EpTier>(ep_override) : tracking::EpTier::TensorRt;
        rcfg.health_budget_ms = 22.0;

        bool actual_syn = use_synthetic;
        auto cam = open_camera_source_with_fallback(camera_active, use_synthetic, preferred_camera_link, camera_fps,
                                                    actual_syn);
        use_synthetic = actual_syn;
        camera_active = (cam != nullptr && cam->is_open());
        if (cam && !cam->device_symlink().empty()) {
            preferred_camera_link = cam->device_symlink();
        }
        return eng.initialize(rcfg, hw_, std::move(cam), &out_err) == AETHER_OK;
    };

    auto publish_hud = [&](const tracking::TrackingEngine* eng, bool ready, const char* fallback_msg) {
        TrackingHudSnapshot& hud = tracking_hud_buffer_.staging();
        hud.enabled = true;
        hud.ready = ready;
        hud.using_synthetic_camera = use_synthetic;
        hud.selected_ep_override = ep_override;
        if (eng != nullptr && ready) {
            const auto st = eng->snapshot_stats();
            hud.camera_active = st.camera_active;
            hud.camera_width = st.camera_width;
            hud.camera_height = st.camera_height;
            hud.camera_fps = st.camera_fps;
            hud.roi_locked = st.roi_locked;
            hud.neutral_calibrated = st.neutral_calibrated;
            hud.stable_lock_frames = st.stable_lock_frames;
            hud.hand_tracking_enabled = eng->hand_tracking_enabled();
            hud.active_ep = tracking::to_ffi_ep(st.active_ep);
            hud.frames_processed = st.frames_processed;
            hud.demotion_count = st.demotion_count;
            hud.preprocess_ms = static_cast<float>(st.last_preprocess_ms);
            hud.inference_ms = static_cast<float>(st.last_inference_ms);
            hud.rolling_infer_ms = static_cast<float>(st.rolling_infer_ms);
            hud.face_confidence = st.face_confidence;
            hud.upper_body_confidence = st.upper_body_confidence;
            hud.left_hand_confidence = st.left_hand_confidence;
            hud.right_hand_confidence = st.right_hand_confidence;
            std::memcpy(hud.head_euler_deg, st.head_euler_deg, sizeof(hud.head_euler_deg));
            std::snprintf(hud.active_ep_label, sizeof(hud.active_ep_label), "%s", st.active_ep_label.c_str());
            std::snprintf(hud.camera_label, sizeof(hud.camera_label), "%s", st.camera_label.c_str());
            std::snprintf(hud.camera_symlink, sizeof(hud.camera_symlink), "%s", st.camera_symlink.c_str());
            auto& preview_staging = live_preview_buffer_.staging();
            if (eng->copy_live_preview(preview_staging) && preview_staging.valid) {
                live_preview_buffer_.publish();
            }
        } else {
            hud.camera_active = false;
            hud.camera_width = 0;
            hud.camera_height = 0;
            hud.camera_fps = 0;
            hud.camera_symlink[0] = '\0';
            hud.roi_locked = false;
            hud.neutral_calibrated = false;
            hud.stable_lock_frames = 0;
            hud.hand_tracking_enabled = hand_tracking_enabled_.load(std::memory_order_acquire);
            std::memset(hud.head_euler_deg, 0, sizeof(hud.head_euler_deg));
            hud.active_ep = AETHER_EP_CPU;
            std::snprintf(hud.active_ep_label, sizeof(hud.active_ep_label), "%s",
                          fallback_msg != nullptr ? fallback_msg : "Tracking Unavailable");
            std::snprintf(hud.camera_label, sizeof(hud.camera_label), "%s",
                          !camera_active ? "Camera Stopped" : (use_synthetic ? "Synthetic 60Hz" : "Camera"));
        }
        if (audio_lipsync_) {
            const auto af = audio_lipsync_->latest();
            hud.audio_mic_available = af.device_available;
            hud.audio_assist_enabled = audio_lipsync_->assist_enabled();
            hud.audio_vad = (af.vad > 0.0f);
            hud.audio_rms_db = af.rms_db;
            hud.audio_open_y = af.audio_open_y;
            std::snprintf(hud.audio_device_label, sizeof(hud.audio_device_label), "%s",
                          audio_lipsync_->active_device_label().c_str());
            std::snprintf(hud.audio_device_id, sizeof(hud.audio_device_id), "%s",
                          audio_lipsync_->active_device_id().c_str());
        } else {
            hud.audio_mic_available = false;
            hud.audio_assist_enabled = false;
            hud.audio_vad = false;
            hud.audio_rms_db = -96.0f;
            hud.audio_open_y = 0.0f;
            std::snprintf(hud.audio_device_label, sizeof(hud.audio_device_label), "Disabled");
            hud.audio_device_id[0] = '\0';
        }
        tracking_hud_buffer_.publish();
    };

    auto engine = std::make_unique<tracking::TrackingEngine>();
    std::string init_err;
    bool engine_ready = init_engine(*engine, init_err);
    if (!engine_ready) {
        AETHER_LOG_WARN("tracking", "TrackingEngine init fallback: %s", init_err.c_str());
    }
    publish_hud(engine.get(), engine_ready, engine_ready ? nullptr : init_err.c_str());

    uint64_t bounds_epoch = 0ULL;
    tracking::NpuMovementBoundsConfig bounds_cfg{};
    uint64_t next_tick = now_us();
    while (running_.load(std::memory_order_acquire)) {
        if (movement_bounds_mailbox_.poll(bounds_epoch, bounds_cfg)) {
            engine->set_movement_bounds_config(bounds_cfg);
        }
        TrackingControlCommand cmd{};
        while (tracking_cmd_ring_.try_pop(cmd)) {
            if (cmd.kind == TrackingCmdKind::CalibrateNeutral) {
                engine->request_calibration();
            } else if (cmd.kind == TrackingCmdKind::ToggleCameraSource && engine_ready) {
                const bool target_syn = !use_synthetic;
                bool actual_syn = target_syn;
                auto new_cam = open_camera_source_with_fallback(true, target_syn, preferred_camera_link, camera_fps,
                                                                 actual_syn);
                use_synthetic = actual_syn;
                camera_active = (new_cam != nullptr && new_cam->is_open());
                (void)engine->switch_camera_source(std::move(new_cam));
                publish_hud(engine.get(), engine_ready, nullptr);
            } else if (cmd.kind == TrackingCmdKind::SetCameraActive && engine_ready) {
                const bool want_active = (cmd.int_arg != 0);
                bool actual_syn = use_synthetic;
                auto new_cam = open_camera_source_with_fallback(want_active, use_synthetic, preferred_camera_link,
                                                                 camera_fps, actual_syn);
                use_synthetic = actual_syn;
                camera_active = (new_cam != nullptr && new_cam->is_open());
                if (new_cam && !new_cam->device_symlink().empty()) {
                    preferred_camera_link = new_cam->device_symlink();
                }
                (void)engine->switch_camera_source(std::move(new_cam));
                publish_hud(engine.get(), engine_ready, nullptr);
            } else if (cmd.kind == TrackingCmdKind::SetSyntheticCamera && engine_ready) {
                const bool want_syn = (cmd.int_arg != 0);
                if (want_syn != use_synthetic || (camera_active && want_syn != engine->snapshot_stats().camera_active)) {
                    bool actual_syn = want_syn;
                    auto new_cam = open_camera_source_with_fallback(camera_active, want_syn, preferred_camera_link,
                                                                     camera_fps, actual_syn);
                    use_synthetic = actual_syn;
                    camera_active = (new_cam != nullptr && new_cam->is_open());
                    if (new_cam && !new_cam->device_symlink().empty()) {
                        preferred_camera_link = new_cam->device_symlink();
                    }
                    (void)engine->switch_camera_source(std::move(new_cam));
                    publish_hud(engine.get(), engine_ready, nullptr);
                }
            } else if (cmd.kind == TrackingCmdKind::SelectCameraDevice && engine_ready) {
                preferred_camera_link = cmd.str_arg;
                if (cmd.int_arg >= 15) {
                    camera_fps = static_cast<uint32_t>(cmd.int_arg);
                }
                use_synthetic = false;
                camera_active = true;
                bool actual_syn = false;
                auto new_cam =
                    open_camera_source_with_fallback(true, false, preferred_camera_link, camera_fps, actual_syn);
                use_synthetic = actual_syn;
                camera_active = (new_cam != nullptr && new_cam->is_open());
                if (new_cam && !new_cam->device_symlink().empty()) {
                    preferred_camera_link = new_cam->device_symlink();
                }
                (void)engine->switch_camera_source(std::move(new_cam));
                publish_hud(engine.get(), engine_ready, nullptr);
            } else if (cmd.kind == TrackingCmdKind::SetEpOverride) {
                ep_override = cmd.int_arg;
                engine->shutdown();
                init_err.clear();
                engine_ready = init_engine(*engine, init_err);
                publish_hud(engine.get(), engine_ready, engine_ready ? nullptr : init_err.c_str());
            } else if (cmd.kind == TrackingCmdKind::SelectAudioDevice) {
                if (audio_lipsync_) {
                    audio_lipsync_->select_device(cmd.str_arg);
                }
                publish_hud(engine.get(), engine_ready, engine_ready ? nullptr : init_err.c_str());
            } else if (cmd.kind == TrackingCmdKind::SetHandTracking) {
                const bool enabled = (cmd.int_arg != 0);
                hand_tracking_enabled_.store(enabled, std::memory_order_release);
                if (engine) {
                    engine->set_hand_tracking_enabled(enabled);
                }
                publish_hud(engine.get(), engine_ready, engine_ready ? nullptr : init_err.c_str());
            }
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        if (engine_ready) {
            AETHER_ZONE("T1_TrackingInference");
            const bool produced = engine->step_once(pose_buffer_, 20U, true);
            if (produced) {
                t1_frames_.fetch_add(1ULL, std::memory_order_acq_rel);
#if defined(_WIN32)
                if (pose_ready_event_ != nullptr) {
                    ::SetEvent(static_cast<HANDLE>(pose_ready_event_));
                }
#endif
                publish_hud(engine.get(), true, nullptr);
            }
        } else {
            // Fallback heartbeat if model manifest unavailable
            const uint64_t cap_us = now_us();
            AetherTrackingFrame& frame = pose_buffer_.staging();
            std::memset(&frame, 0, sizeof(frame));
            frame.capture_timestamp_us = cap_us;
            frame.inference_complete_us = cap_us;
            frame.active_execution_provider = AETHER_EP_CPU;
            frame.frame_sequence = static_cast<uint32_t>(t1_frames_.fetch_add(1ULL, std::memory_order_acq_rel) + 1ULL);
            frame.valid_flags = 0U;
            for (size_t b = 0; b < AETHER_HUMANOID_BONE_COUNT; ++b) {
                frame.joint_rotations_quat[b][3] = 1.0f;
            }
            pose_buffer_.publish();
#if defined(_WIN32)
            if (pose_ready_event_ != nullptr) {
                ::SetEvent(static_cast<HANDLE>(pose_ready_event_));
            }
#endif
        }

        const uint64_t now = now_us();
        if (next_tick + kPeriodUs <= now) {
            next_tick = now;
        } else {
            next_tick += kPeriodUs;
            timer.wait_until_us(next_tick);
        }
    }

    engine->shutdown();
}

void PipelineSkeleton::t2_entry() {
    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    (void)apply_thread_placement(ThreadRole::SimT2, topology_);
    HighResTimer timer;
    constexpr uint64_t kSimPeriodUs = 4167ULL; // 240 Hz simulation cadence (§4.1)
    uint64_t next_tick = now_us() + kSimPeriodUs;
    uint64_t sim_tick = 0;
    uint64_t param_epoch = 0;
    uint64_t rest_epoch = 0;
    float gain = 1.0f;
    std::array<float, 3> prev_hair_centroid{0.0f, 0.0f, 0.0f};
    std::array<float, 3> hair_vel_ema{0.0f, 0.0f, 0.0f};
    bool have_prev_hair = false;
    AetherTrackingFrame latest_pose{};
    animation::RestSkeletonMetrics pending_rest{};
    AetherDspSyncPacket dsp_pkt{};
    uint64_t t2_dsp_stale = 0;
    AetherGraphOutputs graph_zero{};
    animation::SimEngineConfig sim_cfg{};

    while (running_.load(std::memory_order_acquire)) {
        (void)timer.wait_event_or_deadline_us(pose_ready_event_, next_tick);
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        const uint64_t now = now_us();
        if (now >= next_tick) {
            next_tick = now + kSimPeriodUs;
        }

        {
            AETHER_HOT_PATH_SCOPE;
            AETHER_ZONE("T2_SimTick");
            const uint64_t t2_start = now;
            if (rest_metrics_mailbox_.poll(rest_epoch, pending_rest) && sim_engine_) {
                sim_engine_->set_rest_metrics(pending_rest);
            }
            (void)sim_gain_mailbox_.poll(param_epoch, gain);
            if (!tracking_paused_.load(std::memory_order_acquire) && pose_buffer_.acquire()) {
                latest_pose = pose_buffer_.latest();
            }

            (void)output::DspSyncBridge::try_read_dsp_line1(dsp_reader_bridge_.mapped_packet(), t2_start, dsp_pkt,
                                                            t2_dsp_stale);

            AetherGraphInputs graph_in{};
            graph_in.tracking = &latest_pose;
            graph_in.now_us = t2_start;
            graph_in.dt_s = 1.0f / 240.0f;
            graph_in.tick_index = static_cast<uint32_t>(sim_tick);
            for (size_t vi = 0; vi < 5; ++vi) {
                graph_in.dsp_visemes[vi] = dsp_pkt.dsp_viseme_weights[vi];
            }
            graph_in.dsp_rms_db = dsp_pkt.dsp_input_rms_db;

            AetherGraphOutputs graph_out{};
            if (AetherHostHandle host = plugin_host_.load(std::memory_order_acquire); host != nullptr) {
                (void)aether_host_evaluate_active(host, &graph_in, &graph_out);
            }

            SkinningSnapshot& snap = skinning_buffer_.staging();
            vmc_frame_.valid = false;
            const animation::AudioLipSyncFrame audio_frame =
                audio_lipsync_ ? audio_lipsync_->latest() : animation::AudioLipSyncFrame{};
            const animation::AudioLipSyncFrame* const opt_audio = audio_lipsync_ ? &audio_frame : nullptr;
            sim_cfg.visibility.enable_hand_ik = hand_tracking_enabled_.load(std::memory_order_acquire);
            if (sim_engine_) {
                sim_engine_->tick(latest_pose, dsp_pkt, graph_out, t2_start, -1.0f, sim_cfg, snap.frame, opt_audio,
                                  0.65f);
                if ((sim_tick % 4U) == 0U && vmc_enabled_.load(std::memory_order_acquire)) {
                    sim_engine_->export_vmc_pose(snap.frame, static_cast<float>(static_cast<double>(t2_start) * 1e-6),
                                                 vmc_frame_);
                }
                sim_engine_->export_spring_overlay(
                    snap.spring_colliders.data(), static_cast<uint32_t>(snap.spring_colliders.size()),
                    snap.spring_collider_count, snap.spring_segments.data(),
                    static_cast<uint32_t>(snap.spring_segments.size()), snap.spring_segment_count);
            }
            if (snap.spring_segment_count > 0U) {
                std::array<float, 3> cur_c{0.0f, 0.0f, 0.0f};
                for (uint32_t si = 0; si < snap.spring_segment_count; ++si) {
                    cur_c[0] += snap.spring_segments[si].tail[0];
                    cur_c[1] += snap.spring_segments[si].tail[1];
                    cur_c[2] += snap.spring_segments[si].tail[2];
                }
                const float inv_n = 1.0f / static_cast<float>(snap.spring_segment_count);
                cur_c[0] *= inv_n;
                cur_c[1] *= inv_n;
                cur_c[2] *= inv_n;
                if (have_prev_hair) {
                    const std::array<float, 3> inst = {
                        std::clamp((cur_c[0] - prev_hair_centroid[0]) * 240.0f, -4.0f, 4.0f),
                        std::clamp((cur_c[1] - prev_hair_centroid[1]) * 240.0f, -4.0f, 4.0f),
                        std::clamp((cur_c[2] - prev_hair_centroid[2]) * 240.0f, -4.0f, 4.0f),
                    };
                    for (size_t k = 0; k < 3; ++k) {
                        hair_vel_ema[k] = hair_vel_ema[k] * 0.85f + inst[k] * 0.15f;
                    }
                }
                prev_hair_centroid = cur_c;
                have_prev_hair = true;
            }
            snap.hair_velocity_ws = hair_vel_ema;
            const uint64_t t2_done = now_us();
            snap.frame.sim_tick_ms = static_cast<float>(t2_done - t2_start) * 0.001f;
            snap.frame_sequence = latest_pose.frame_sequence;
            snap.sim_sequence = sim_tick + 1ULL;
            snap.t1_capture_us = latest_pose.capture_timestamp_us;
            snap.t2_start_us = t2_start;
            snap.t2_done_us = t2_done;
            snap.spring_sim_ms = snap.frame.spring_sim_ms;
            snap.dsp_attenuated_on_t2 = ((dsp_pkt.dsp_flags & 0x05U) == 0x05U);
            snap.outputs = graph_out;
            if (snap.outputs.expression_weights[0] == 0.0f) {
                snap.outputs.expression_weights[0] = snap.frame.morph_weights[17] * gain; // Appendix B slot 17: jawOpen
            }
            if (snap.outputs.look_at_ws[3] <= 0.0f) {
                snap.outputs.look_at_ws[2] = 1.0f;
            }
            (void)graph_zero;
            skinning_buffer_.publish();
            t2_ticks_.store(++sim_tick, std::memory_order_release);
        }

        // VMC send happens outside the hot-path scope (socket setup allocates; sendto is a syscall).
        if (vmc_frame_.valid) {
            if (!vmc_sender_.is_open() && !vmc_init_failed_) {
                vmc_init_failed_ = !vmc_sender_.init(cfg_.output_cfg.osc_host, output::kVmcDefaultPort).has_value();
            }
            (void)vmc_sender_.send_frame(vmc_frame_);
        } else if (!vmc_enabled_.load(std::memory_order_relaxed) && vmc_sender_.is_open()) {
            vmc_sender_.shutdown();
        }
    }
    vmc_sender_.shutdown();
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}

void PipelineSkeleton::t4_entry() {
    (void)apply_thread_placement(ThreadRole::OutputT4, topology_);
    RollingLatencyStats m2p_stats;
    RollingLatencyStats sim_stats;
    RollingLatencyStats render_stats;
    RollingLatencyStats ui_stats;

    std::unique_ptr<output::OutputCoordinator> coord;
    uint64_t out_cfg_epoch = 0;
    output::OutputConfig next_out_cfg{};
    if (cfg_.output_device != nullptr) {
        coord = std::make_unique<output::OutputCoordinator>();
        if (auto rc = coord->init(*cfg_.output_device, cfg_.output_cfg); !rc) {
            AETHER_LOG_WARN("output", "OutputCoordinator init warning: %s", rc.error().c_str());
        }
        output_status_buffer_.publish(coord->snapshot_status());
    }

    uint64_t prev_t1_frames = 0;
    uint64_t prev_t2_ticks = 0;
    uint64_t prev_t4_frames = 0;
    uint64_t last_rate_us = now_us();
    float est_t1_hz = 60.0f;
    float est_t2_hz = 240.0f;
    float est_t3_fps = 60.0f;

    auto record_telemetry = [&](uint64_t frame_sequence, uint64_t t1_capture_us, uint64_t t2_start_us,
                                uint64_t t2_done_us, uint64_t t3_start_us, uint64_t t3_submit_us, uint64_t done_us,
                                uint64_t ui_cpu_us, bool tracking_active) noexcept {
        const uint64_t total_t4 = t4_frames_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;

        FrameStamps stamps{};
        stamps.frame_sequence = frame_sequence;
        stamps.t1_capture_us = t1_capture_us;
        stamps.t2_start_us = t2_start_us;
        stamps.t2_done_us = t2_done_us;
        stamps.t3_start_us = t3_start_us;
        stamps.t3_submit_us = t3_submit_us;
        stamps.t4_output_us = done_us;
        stamps.ui_cpu_us = ui_cpu_us;
        stamps.hot_path_violations = HotPathGuard::total_violations();

        const bool valid_m2p = tracking_active && (frame_sequence != 0ULL);
        (void)m2p_stats.add_m2p_sample(t1_capture_us, done_us, valid_m2p);
        sim_stats.add_sample_ms(stamps.sim_ms());
        render_stats.add_sample_ms(stamps.render_cpu_ms());
        ui_stats.add_sample_ms(stamps.ui_cpu_ms());

        const uint64_t elapsed_us = done_us > last_rate_us ? (done_us - last_rate_us) : 0ULL;
        if (elapsed_us >= 250'000ULL) {
            const float secs = static_cast<float>(elapsed_us) * 1e-6f;
            const uint64_t cur_t1 = t1_frames_.load(std::memory_order_relaxed);
            const uint64_t cur_t2 = t2_ticks_.load(std::memory_order_relaxed);
            est_t1_hz = static_cast<float>(cur_t1 - prev_t1_frames) / secs;
            est_t2_hz = static_cast<float>(cur_t2 - prev_t2_ticks) / secs;
            est_t3_fps = static_cast<float>(total_t4 - prev_t4_frames) / secs;
            prev_t1_frames = cur_t1;
            prev_t2_ticks = cur_t2;
            prev_t4_frames = total_t4;
            last_rate_us = done_us;
        }

        TelemetrySnapshot snap{};
        snap.last_frame = stamps;
        snap.m2p = m2p_stats.compute_summary();
        snap.sim = sim_stats.compute_summary();
        snap.render_cpu = render_stats.compute_summary();
        snap.ui_cpu = ui_stats.compute_summary();
        snap.tracking_hz = est_t1_hz;
        snap.sim_hz = est_t2_hz;
        snap.render_fps = est_t3_fps;
        snap.total_frames = total_t4;
        snap.hot_path_violations = HotPathGuard::total_violations();
        snap.log_dropped = LogSystem::dropped_count();
        telemetry_hub_.publish(snap);
    };

    while (running_.load(std::memory_order_acquire)) {
#if defined(_WIN32)
        HANDLE handles[2] = {static_cast<HANDLE>(output_ready_event_), static_cast<HANDLE>(stop_event_)};
        const DWORD wait_res = ::WaitForMultipleObjects(2, handles, FALSE, 25);
        if (wait_res == WAIT_OBJECT_0 + 1 || !running_.load(std::memory_order_acquire)) {
            break;
        }
#endif
        if (output_cfg_mailbox_.poll(out_cfg_epoch, next_out_cfg) && coord) {
            coord->apply_config(next_out_cfg);
            output_status_buffer_.publish(coord->snapshot_status());
        }

        AETHER_HOT_PATH_SCOPE;
        AETHER_ZONE("T4_OutputConsume");

        output::OutputRingSlot slot{};
        while (output_slot_ring_.try_pop(slot)) {
            uint64_t done_us = 0;
            if (coord) {
                done_us = coord->process_slot(slot, est_t3_fps);
                output_status_buffer_.publish(coord->snapshot_status());
            } else {
                done_us = now_us();
            }
            record_telemetry(slot.frame_sequence, slot.t1_capture_us, slot.t2_start_us, slot.t2_done_us,
                             slot.t3_start_us, slot.t3_submit_us, done_us, slot.ui_cpu_us, slot.tracking_active);
            pending_output_slots_.fetch_sub(1u, std::memory_order_acq_rel);
        }

        OutputWorkItem work{};
        while (output_ring_.try_pop(work)) {
            const uint64_t done_us = now_us();
            record_telemetry(work.frame_sequence, work.t1_capture_us, work.t2_start_us, work.t2_done_us,
                             work.t3_start_us, work.t3_submit_us, done_us, work.ui_cpu_us, work.tracking_active);
        }
    }

    if (coord) {
        coord->shutdown();
        coord.reset();
    }
}

} // namespace aether::app
