#pragma once

#include "aether_ffi_contract.h"
#include "animation/sim_engine.hpp"
#include "core/cpu_topology.hpp"
#include "core/hw_enum.hpp"
#include "core/spsc_ring.hpp"
#include "core/telemetry.hpp"
#include "core/triple_buffer.hpp"
#include "output/dsp_sync_bridge.hpp"
#include "output/output_config.hpp"
#include "output/output_coordinator.hpp"
#include "output/output_ring.hpp"
#include "output/vmc_sender.hpp"
#include "rhi/rhi_device.hpp"
#include "tracking/tracking_engine.hpp"
#include "tracking/wasapi_audio_lipsync.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

namespace aether::app {

struct SkinningSnapshot {
    aether::animation::SkinningFrame frame{};
    AetherGraphOutputs outputs{};
    uint64_t frame_sequence{0};
    uint64_t sim_sequence{0};
    uint64_t t1_capture_us{0};
    uint64_t t2_start_us{0};
    uint64_t t2_done_us{0};
    float spring_sim_ms{0.0f};
    bool dsp_attenuated_on_t2{false};
    uint8_t reserved_pad_[3]{};
    uint32_t spring_collider_count{0};
    uint32_t spring_segment_count{0};
    std::array<aether::animation::SpringColliderOverlay, aether::animation::kMaxOverlayColliders> spring_colliders{};
    std::array<aether::animation::SpringSegmentOverlay, aether::animation::kMaxOverlaySpringSegments>
        spring_segments{};
    uint64_t reserved_[1]{};
};

struct TrackingHudSnapshot {
    bool enabled{false};
    bool ready{false};
    bool using_synthetic_camera{true};
    bool camera_active{false};
    bool roi_locked{false};
    bool neutral_calibrated{false};
    uint32_t active_ep{AETHER_EP_CPU};
    int32_t selected_ep_override{-1};
    uint32_t camera_width{0};
    uint32_t camera_height{0};
    uint32_t camera_fps{0};
    uint32_t stable_lock_frames{0};
    uint64_t frames_processed{0};
    uint64_t demotion_count{0};
    float preprocess_ms{0.0f};
    float inference_ms{0.0f};
    float rolling_infer_ms{0.0f};
    float face_confidence{0.0f};
    float upper_body_confidence{0.0f};
    float left_hand_confidence{0.0f};
    float right_hand_confidence{0.0f};
    float head_euler_deg[3]{0.0f, 0.0f, 0.0f};
    char active_ep_label[64]{"Initializing..."};
    char camera_label[128]{"Synthetic 60Hz"};
    char camera_symlink[256]{};
    bool audio_mic_available{false};
    bool audio_assist_enabled{true};
    bool audio_vad{false};
    float audio_rms_db{-96.0f};
    float audio_open_y{0.0f};
    char audio_device_label[96]{"System Default"};
    char audio_device_id[192]{};
};

enum class TrackingCmdKind : uint8_t {
    CalibrateNeutral = 0,
    ToggleCameraSource = 1,
    SetEpOverride = 2,
    SetCameraActive = 3,
    SelectCameraDevice = 4,
    SetSyntheticCamera = 5,
    SelectAudioDevice = 6,
};

struct TrackingControlCommand {
    TrackingCmdKind kind{TrackingCmdKind::CalibrateNeutral};
    int32_t int_arg{0};
    char str_arg[256]{};
};

struct PipelineConfig {
    bool enable_live_tracking{false};
    bool use_synthetic_camera{true};
    bool camera_active{true};
    uint32_t camera_fps{60};
    int32_t preferred_ep{-1};
    std::string preferred_camera_link{};
    bool enable_audio_lipsync{true};
    float mic_gain{1.25f};
    float mic_noise_gate_db{-48.0f};
    std::string preferred_mic_id{};
    std::filesystem::path manifest_path{};
    std::filesystem::path ov_cache_dir{};
    rhi::RhiDevice* output_device{nullptr};
    aether::output::OutputConfig output_cfg{};
};

struct OutputWorkItem {
    uint64_t frame_sequence{0};
    uint64_t sim_sequence{0};
    uint64_t t1_capture_us{0};
    uint64_t t2_start_us{0};
    uint64_t t2_done_us{0};
    uint64_t t3_start_us{0};
    uint64_t t3_submit_us{0};
    uint64_t ui_cpu_us{0};
    bool tracking_active{true};
};

/**
 * Lock-free T1 -> T2 -> T3 -> T4 pipeline (`§2.2`, `§4.1`, `§8`, `§9`, `HR-01`, `HR-02`).
 * Coordinates T1 (TrackingEngine NPU/GPU/CPU capture & landmark inference), T2 (240 Hz
 * SimEngine: 1-Euro, Kalman, Visibility-First, IK, SpringBone, ExpressionMixer),
 * T3 (caller render loop publishing submit stamps), and T4 (output sink + telemetry summary).
 */
class PipelineSkeleton {
  public:
    PipelineSkeleton() = default;
    ~PipelineSkeleton();

    PipelineSkeleton(const PipelineSkeleton&) = delete;
    PipelineSkeleton& operator=(const PipelineSkeleton&) = delete;

    bool start(const CpuTopology& topology);
    bool start(const HardwareInventory& hw, const PipelineConfig& cfg = {});
    void stop() noexcept;

    /** Called by T3 at start of render frame (late latch reads latest T2 + T1 state). */
    [[nodiscard]] bool acquire_skinning(SkinningSnapshot& out) noexcept;
    [[nodiscard]] const SkinningSnapshot& acquire_skinning_ref(bool* out_updated = nullptr) noexcept;

    /** Called by T3 right after frame submission to dispatch synthetic T4 work + stamps. */
    void submit_rendered_frame(const OutputWorkItem& item) noexcept;

    /** Called by T3 right after critical Passes 1-12 submission to dispatch GPU outputs + stamps on T4. */
    void submit_output_slot(const aether::output::OutputRingSlot& slot) noexcept;

    /** Updates live output configuration (resolution, Spout2, VCam, NDI, OSC, DSP offset) lock-free (`HR-02`). */
    void update_output_config(const aether::output::OutputConfig& cfg) noexcept;

    /** Drains pending T4 output slots so T3 can safely resize/recreate Pass 12 output ring images. */
    [[nodiscard]] bool wait_output_slots_drained(uint32_t max_wait_ms = 250u) noexcept;

    /** Wait-free read of latest Audio-DSP Cache Line 1 packet for T3 late-latch (`PERF-008`). */
    [[nodiscard]] bool try_read_latest_dsp(AetherDspSyncPacket& out) noexcept;

    [[nodiscard]] aether::output::OutputRuntimeStatus latest_output_status() noexcept;

    /** Pushes newly loaded avatar rest metrics from T3 -> T2 without locks (`HR-02`). */
    void push_rest_skeleton(const aether::animation::RestSkeletonMetrics& rest) noexcept;

    /** Sends non-blocking T3 UI commands (calibrate/camera/EP/mic switch) to T1 (`HR-02`). */
    void send_tracking_command(const TrackingControlCommand& cmd) noexcept;

    /** Wait-free audio lip-sync parameter update (`HR-02`) called by T3. */
    void set_audio_lipsync_params(bool assist_enabled, float mic_gain, float noise_gate_db) noexcept;

    [[nodiscard]] TrackingHudSnapshot latest_tracking_hud() noexcept;
    [[nodiscard]] bool acquire_live_preview(tracking::LiveCameraPreviewFrame& out) noexcept;
    [[nodiscard]] TelemetrySnapshot latest_telemetry() noexcept;
    [[nodiscard]] uint64_t total_t1_frames() const noexcept { return t1_frames_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t total_t2_ticks() const noexcept { return t2_ticks_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t total_t4_frames() const noexcept { return t4_frames_.load(std::memory_order_acquire); }

    ParamMailbox<float>& sim_gain_mailbox() noexcept { return sim_gain_mailbox_; }

    /** Wait-free plugin host pointer handoff for T2 graph evaluation (`HR-02`). */
    void set_plugin_host(AetherHostHandle host) noexcept { plugin_host_.store(host, std::memory_order_release); }

    /** Pauses or resumes live tracking pose ingestion cleanly on T2 (`HR-02`). */
    void set_tracking_paused(bool paused) noexcept { tracking_paused_.store(paused, std::memory_order_release); }
    [[nodiscard]] bool tracking_paused() const noexcept { return tracking_paused_.load(std::memory_order_acquire); }

  private:
    void t1_entry();
    void t2_entry();
    void t4_entry();

    LockFreeTripleBuffer<AetherTrackingFrame> pose_buffer_{};
    LockFreeTripleBuffer<SkinningSnapshot> skinning_buffer_{};
    LockFreeTripleBuffer<TrackingHudSnapshot> tracking_hud_buffer_{};
    LockFreeTripleBuffer<tracking::LiveCameraPreviewFrame> live_preview_buffer_{};
    LockFreeTripleBuffer<aether::output::OutputRuntimeStatus> output_status_buffer_{};
    SpscRing<OutputWorkItem, 16> output_ring_{};
    SpscRing<aether::output::OutputRingSlot, 16> output_slot_ring_{};
    SpscRing<TrackingControlCommand, 16> tracking_cmd_ring_{};
    ParamMailbox<float> sim_gain_mailbox_{};
    ParamMailbox<aether::animation::RestSkeletonMetrics> rest_metrics_mailbox_{};
    ParamMailbox<aether::output::OutputConfig> output_cfg_mailbox_{};
    TelemetryHub telemetry_hub_{};
    aether::output::DspSyncBridge dsp_reader_bridge_{};
    uint64_t t3_dsp_stale_count_{0};
    std::atomic<uint32_t> pending_output_slots_{0};

    void* pose_ready_event_{nullptr};
    void* output_ready_event_{nullptr};
    void* stop_event_{nullptr};

    std::atomic<bool> running_{false};
    std::atomic<bool> tracking_paused_{false};
    std::atomic<AetherHostHandle> plugin_host_{nullptr};
    std::atomic<uint64_t> t1_frames_{0};
    std::atomic<uint64_t> t2_ticks_{0};
    std::atomic<uint64_t> t4_frames_{0};

    std::thread t1_thread_;
    std::thread t2_thread_;
    std::thread t4_thread_;
    HardwareInventory hw_{};
    CpuTopology topology_{};
    PipelineConfig cfg_{};
    std::unique_ptr<aether::animation::SimEngine> sim_engine_;
    std::unique_ptr<aether::tracking::WasapiAudioLipsync> audio_lipsync_;

    // VMC Protocol performer output (T2, 60 Hz) mirroring the Studio OSC toggle.
    std::atomic<bool> vmc_enabled_{false};
    aether::output::VmcSender vmc_sender_{};
    aether::animation::VmcPoseFrame vmc_frame_{};
    bool vmc_init_failed_{false};
};

} // namespace aether::app
