#pragma once

#include "core/triple_buffer.hpp"

#include <array>
#include <cstdint>

namespace aether {

/** End-to-end per-frame timestamps in QPC microseconds (`§4.10`). */
struct FrameStamps {
    uint64_t frame_sequence{0};
    uint64_t t0_input_us{0};
    uint64_t t1_capture_us{0};
    uint64_t t1_infer_done_us{0};
    uint64_t t2_start_us{0};
    uint64_t t2_done_us{0};
    uint64_t t3_start_us{0};
    uint64_t t3_submit_us{0};
    uint64_t t4_output_us{0};
    uint64_t ui_cpu_us{0};
    uint64_t render_gpu_us{0};
    uint64_t hot_path_violations{0};

    static constexpr uint64_t kMaxValidM2pUs = 5'000'000ULL; // 5000 ms sanity bound

    [[nodiscard]] bool has_valid_m2p() const noexcept {
        if (t1_capture_us == 0ULL || t4_output_us <= t1_capture_us) {
            return false;
        }
        const uint64_t diff_us = t4_output_us - t1_capture_us;
        return diff_us <= kMaxValidM2pUs;
    }

    [[nodiscard]] float motion_to_photon_ms() const noexcept {
        if (!has_valid_m2p()) {
            return 0.0f;
        }
        return static_cast<float>(t4_output_us - t1_capture_us) * 0.001f;
    }

    [[nodiscard]] float sim_ms() const noexcept {
        if (t2_done_us <= t2_start_us) {
            return 0.0f;
        }
        return static_cast<float>(t2_done_us - t2_start_us) * 0.001f;
    }

    [[nodiscard]] float render_cpu_ms() const noexcept {
        if (t3_submit_us <= t3_start_us) {
            return 0.0f;
        }
        return static_cast<float>(t3_submit_us - t3_start_us) * 0.001f;
    }

    [[nodiscard]] float ui_cpu_ms() const noexcept { return static_cast<float>(ui_cpu_us) * 0.001f; }
};

struct LatencySummary {
    float latest_ms{0.0f};
    float mean_ms{0.0f};
    float p50_ms{0.0f};
    float p95_ms{0.0f};
    float p99_ms{0.0f};
    float max_ms{0.0f};
    uint32_t sample_count{0};
    std::array<uint32_t, 24> bins{}; // 0..24 ms+ (1 ms width per bin)
};

/** Fixed-window allocation-free percentile & histogram accumulator (`HR-01`). */
class RollingLatencyStats {
  public:
    static constexpr size_t kWindowSize = 256;

    void reset() noexcept;
    void add_sample_ms(float ms) noexcept;
    bool add_m2p_sample(uint64_t capture_timestamp_us, uint64_t present_timestamp_us,
                        bool tracking_valid = true) noexcept;
    [[nodiscard]] LatencySummary compute_summary() const noexcept;

  private:
    std::array<float, kWindowSize> window_{};
    size_t write_cursor_{0};
    size_t filled_{0};
    float latest_{0.0f};
};

struct TelemetrySnapshot {
    FrameStamps last_frame{};
    LatencySummary m2p{};
    LatencySummary sim{};
    LatencySummary render_cpu{};
    LatencySummary ui_cpu{};
    float tracking_hz{0.0f};
    float sim_hz{0.0f};
    float render_fps{0.0f};
    uint64_t total_frames{0};
    uint64_t hot_path_violations{0};
    uint64_t vk_validation_errors{0};
    uint64_t log_dropped{0};
    uint64_t input_dropped{0};
    uint32_t active_w{0};
    uint32_t active_h{0};
    char present_mode[24]{};
};

class TelemetryHub {
  public:
    void publish(const TelemetrySnapshot& snapshot) noexcept { triple_.publish(snapshot); }

    [[nodiscard]] bool acquire() noexcept { return triple_.acquire(); }

    [[nodiscard]] const TelemetrySnapshot& latest() const noexcept { return triple_.latest(); }

  private:
    LockFreeTripleBuffer<TelemetrySnapshot> triple_{};
};

} // namespace aether
