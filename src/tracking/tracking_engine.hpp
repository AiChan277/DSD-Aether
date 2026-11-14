#pragma once

#include "aether_ffi_contract.h"
#include "core/hw_enum.hpp"
#include "core/triple_buffer.hpp"
#include "tracking/camera_source.hpp"
#include "tracking/landmark_solver.hpp"
#include "tracking/ort_router.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace aether::tracking {

struct TrackingStatsSnapshot {
    uint64_t frames_processed{0};
    uint64_t frames_dropped{0};
    double last_preprocess_ms{0.0};
    double last_inference_ms{0.0};
    double last_total_t1_ms{0.0};
    double rolling_infer_ms{0.0};
    EpTier active_ep{EpTier::Cpu};
    std::string active_ep_label{"Uninitialized"};
    std::string camera_label{"None"};
    std::string camera_symlink{};
    uint32_t camera_width{0};
    uint32_t camera_height{0};
    uint32_t camera_fps{0};
    bool camera_active{false};
    uint32_t demotion_count{0};
    float face_confidence{0.0f};
    float upper_body_confidence{0.0f};
    float left_hand_confidence{0.0f};
    float right_hand_confidence{0.0f};
    bool roi_locked{false};
};

/**
 * Real-time `T1` Capture & ONNX Tracking Engine (`§2.2`, `§8`).
 * Reads newest NV12/BGRA frame from `ICameraSource`, builds ROIs (`<= 0.6 ms` AVX2),
 * executes `M0(conditional) -> M1_body + M2 -> M1_hands(conditional) -> M3`,
 * writes `AetherTrackingFrame` into caller-owned `LockFreeTripleBuffer`, and sets `PoseReady`.
 */
class TrackingEngine {
  public:
    TrackingEngine() noexcept = default;
    ~TrackingEngine();

    TrackingEngine(const TrackingEngine&) = delete;
    TrackingEngine& operator=(const TrackingEngine&) = delete;

    [[nodiscard]] AetherStatus initialize(const TrackingRouterConfig& router_cfg, const HardwareInventory& hw,
                                          std::unique_ptr<ICameraSource> source, std::string* out_err = nullptr);
    void shutdown() noexcept;

    /** Runs one synchronous capture + inference step into `out_buffer` (called inside T1 loop). */
    [[nodiscard]] bool step_once(LockFreeTripleBuffer<AetherTrackingFrame>& out_buffer, uint32_t wait_timeout_ms,
                                 bool synthetic_fallback_when_no_frame = false) noexcept;

    void request_calibration() noexcept;
    [[nodiscard]] AetherStatus switch_camera_source(std::unique_ptr<ICameraSource> next_source) noexcept;

    [[nodiscard]] TrackingStatsSnapshot snapshot_stats() const;
    [[nodiscard]] AetherTrackingRouter& router() noexcept { return router_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    void step_synthetic_idle_frame(LockFreeTripleBuffer<AetherTrackingFrame>& out_buffer, uint64_t now_us) noexcept;

    AetherTrackingRouter router_{};
    LandmarkPoseSolver solver_{};
    std::unique_ptr<ICameraSource> camera_{};
    std::unique_ptr<CapturedFrameBuffer> standby_frame_{};

    RoiRectNorm face_roi_{};
    uint32_t frame_seq_{0};
    uint64_t total_frames_{0};
    std::atomic<bool> calibrate_requested_{false};
    std::atomic<uint64_t> last_infer_us_scaled_{0};
    std::atomic<uint64_t> last_prep_us_scaled_{0};
    std::atomic<uint64_t> last_total_us_scaled_{0};
    std::atomic<float> last_face_conf_{0.0f};
    std::atomic<float> last_body_conf_{0.0f};
    std::atomic<float> last_lhand_conf_{0.0f};
    std::atomic<float> last_rhand_conf_{0.0f};
    std::atomic<bool> roi_locked_{false};
    bool initialized_{false};
};

} // namespace aether::tracking
