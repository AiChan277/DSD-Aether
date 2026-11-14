#pragma once

#include "aether_ffi_contract.h"
#include "core/expected.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace aether::output {

inline constexpr const wchar_t* kDspSyncShmName = L"Local\\AetherDspSyncSharedMem";
inline constexpr size_t kDspSyncShmBytes = 4096u;
inline constexpr uint64_t kDspStaleThresholdUs = 250'000ULL; // 250 ms (§11.5)
inline constexpr float kDspDelayEmaAlpha = 0.1f;
inline constexpr float kDspMaxDelayMs = 250.0f;
inline constexpr float kDspMaxSlewMsPerSec = 2.0f;

/**
 * Bidirectional zero-lock shared-memory bridge (`Local\AetherDspSyncSharedMem`, 128 B `AetherDspSyncPacket`)
 * connecting T4 (Cache Line 0 writer) and T2/T3 late-latch (Cache Line 1 reader) with external audio DSPs (§11.5).
 * Strictly zero heap allocations after `init()` (`HR-01`, `HR-02`).
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class DspSyncBridge {
  public:
    DspSyncBridge() = default;
    ~DspSyncBridge() { shutdown(); }

    DspSyncBridge(const DspSyncBridge&) = delete;
    DspSyncBridge& operator=(const DspSyncBridge&) = delete;

    [[nodiscard]] Expected<void> init(const wchar_t* section_name = kDspSyncShmName);
    void shutdown() noexcept;

    /**
     * Publishes Cache Line 0 on T4 using the odd/even seqlock protocol (`§11.5`).
     * Smooths `recommended_dsp_delay_ms` with `EMA_0.1`, subtracts DSP-reported latency,
     * adds `user_offset_ms`, clamps to `[0, 250] ms`, and slew-limits to `+-2 ms/s`.
     */
    void publish_video_telemetry(float video_latency_ms, uint64_t present_qpc_us, uint32_t fps_milli,
                                 uint32_t video_flags, float user_offset_ms, float dt_sec) noexcept;

    /**
     * Wait-free 4-attempt seqlock read of Cache Line 1 (`dsp_*` fields).
     * Rejects payloads older than `250 ms` relative to `now_qpc_us` by clearing `visemes_valid`/`voice_active`.
     */
    [[nodiscard]] bool read_dsp_packet(AetherDspSyncPacket& out_packet, uint64_t now_qpc_us) noexcept;

    /** Helper to read current mapped Cache Line 1 directly in standalone IPC callers/late-latch. */
    [[nodiscard]] static bool try_read_dsp_line1(const AetherDspSyncPacket* shm_ptr, uint64_t now_qpc_us,
                                                 AetherDspSyncPacket& out_packet, uint64_t& stale_counter) noexcept;

    /**
     * Standalone late-latch viseme application (`REQ-ANM-020`, `PERF-008`):
     * blends DSP visemes `A/I/U/E/O` + ARKit mouth weights into `morph_weights[kMaxMorphs]` in <= 10 us.
     */
    static void apply_late_latch_visemes(const AetherDspSyncPacket& dsp, float mouth_owner_blend, float* morph_weights,
                                         uint32_t morph_count) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return packet_ != nullptr; }
    [[nodiscard]] float current_ema_latency_ms() const noexcept { return ema_latency_ms_; }
    [[nodiscard]] float current_recommended_delay_ms() const noexcept { return smoothed_delay_ms_; }
    [[nodiscard]] float current_jitter_ms() const noexcept { return latest_jitter_ms_; }
    [[nodiscard]] uint64_t stale_reject_count() const noexcept { return stale_rejects_; }
    [[nodiscard]] AetherDspSyncPacket* mapped_packet() noexcept { return packet_; }

  private:
    void* file_mapping_{nullptr};
    AetherDspSyncPacket* packet_{nullptr};
    AetherDspSyncPacket last_good_{};

    std::array<float, 120> lat_window_{};
    std::array<float, 120> lat_sort_scratch_{};
    uint32_t lat_count_{0};
    uint32_t lat_cursor_{0};

    float ema_latency_ms_{0.0f};
    float smoothed_delay_ms_{0.0f};
    float latest_jitter_ms_{0.0f};
    bool has_ema_{false};
    bool has_delay_{false};
    uint64_t stale_rejects_{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace aether::output
