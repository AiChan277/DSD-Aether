#pragma once

#include "aether_ffi_contract.h"
#include "core/hw_enum.hpp"
#include "tracking/manifest.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aether::tracking {

struct TrackingRouterConfig {
    std::filesystem::path manifest_path{};
    std::filesystem::path ov_cache_dir{};
    EpTier preferred_ep{EpTier::NpuOpenVino};
    bool enable_npu_fast_compile{false};
    uint32_t cpu_intra_op_threads{4};
    uint32_t health_window_frames{120};
    double health_budget_ms{9.0};
    // CI testing bitmask: bit `k` set forces tier `EpTier(k)` initialization to simulate failure.
    uint32_t forced_fail_ep_mask{0};
};

struct PreallocatedModelBuffers {
    std::vector<float> m0_in;         // 1 * 3 * 128 * 128
    std::vector<float> m0_regressors; // 1 * 896 * 16
    std::vector<float> m0_classif;    // 1 * 896 * 1

    std::vector<float> m1_body_in;    // 1 * 3 * 256 * 256
    std::vector<float> m1_body_lm;    // 1 * 195
    std::vector<float> m1_body_flag;  // 1
    std::vector<float> m1_body_seg;   // 256 * 256 (or unused scratch)
    std::vector<float> m1_body_heat;  // 64 * 64 * 39
    std::vector<float> m1_body_world; // 1 * 117

    std::vector<float> m1_hands_in;     // 2 * 3 * 224 * 224
    std::vector<float> m1_hands_lm;     // 2 * 63
    std::vector<float> m1_hands_flag;   // 2 * 1
    std::vector<float> m1_hands_handed; // 2 * 1
    std::vector<float> m1_hands_world;  // 2 * 63

    std::vector<float> m2_face_in;   // 1 * 3 * 256 * 256
    std::vector<float> m2_face_lm;   // 1 * 1434 (478 * 3)
    std::vector<float> m2_face_flag; // 1

    std::vector<float> m3_in;          // 1 * 146 * 2
    std::vector<float> m3_blendshapes; // 1 * 52
};

/**
 * Multi-tier ORT Execution Provider Router (`AETHER-TS-001` §8.4, `ADR-0007`).
 * Pre-allocates all input/output tensors and `Ort::IoBinding` objects once at initialize(),
 * ensuring zero heap allocations during per-frame `run_*` calls (`HR-01`).
 */
class AetherTrackingRouter {
  public:
    AetherTrackingRouter();
    ~AetherTrackingRouter();

    AetherTrackingRouter(const AetherTrackingRouter&) = delete;
    AetherTrackingRouter& operator=(const AetherTrackingRouter&) = delete;

    [[nodiscard]] AetherStatus initialize(const TrackingRouterConfig& cfg, const HardwareInventory& hw,
                                          std::string* out_err = nullptr);
    void shutdown() noexcept;

    [[nodiscard]] PreallocatedModelBuffers& buffers() noexcept { return buffers_; }
    [[nodiscard]] const PreallocatedModelBuffers& buffers() const noexcept { return buffers_; }
    [[nodiscard]] const TrackingModelManifest& manifest() const noexcept { return manifest_; }

    /** Zero-alloc hot-path inference steps using pre-bound `Ort::IoBinding`. */
    [[nodiscard]] AetherStatus run_m0_face_detector() noexcept;
    [[nodiscard]] AetherStatus run_m1_body() noexcept;
    [[nodiscard]] AetherStatus run_m1_hands() noexcept;
    [[nodiscard]] AetherStatus run_m2_face_landmarks() noexcept;
    [[nodiscard]] AetherStatus run_m3_blendshapes() noexcept;

    /** Health monitor (`§8.4`): demotes tier automatically if 120-frame avg > `9.0 ms`. */
    void record_frame_latency(double total_infer_ms, std::string* out_demotion_msg = nullptr) noexcept;
    [[nodiscard]] AetherStatus force_demote_one_tier(const char* reason) noexcept;

    [[nodiscard]] EpTier active_ep() const noexcept { return active_ep_; }
    [[nodiscard]] const std::string& active_ep_label() const noexcept { return active_ep_label_; }
    [[nodiscard]] uint32_t demotion_count() const noexcept { return demotion_count_; }
    [[nodiscard]] double rolling_avg_ms() const noexcept { return rolling_avg_ms_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TrackingRouterConfig cfg_{};
    HardwareInventory hw_{};
    TrackingModelManifest manifest_{};
    PreallocatedModelBuffers buffers_{};
    EpTier active_ep_{EpTier::Cpu};
    std::string active_ep_label_{"Uninitialized"};
    uint32_t demotion_count_{0};
    double rolling_avg_ms_{0.0};
    bool initialized_{false};
};

} // namespace aether::tracking
