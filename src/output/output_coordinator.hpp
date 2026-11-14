#pragma once

#include "core/expected.hpp"
#include "output/dsp_sync_bridge.hpp"
#include "output/ndi_output.hpp"
#include "output/osc_sender.hpp"
#include "output/output_config.hpp"
#include "output/output_ring.hpp"
#include "output/spout_output.hpp"
#include "output/vcam_transport.hpp"
#include "rhi/rhi_device.hpp"

#include <cstdint>

namespace aether::output {

/**
 * Coordinates all T4 consumers (`SpoutOutput`, `VcamTransport`, `NdiOutput`, `DspSyncBridge`, `OscSender`).
 * Runs exclusively on T4 (or synchronous verification harness) with zero per-frame heap allocations (`HR-01`, `HR-02`).
 */
class OutputCoordinator {
  public:
    OutputCoordinator() = default;
    ~OutputCoordinator() { shutdown(); }

    OutputCoordinator(const OutputCoordinator&) = delete;
    OutputCoordinator& operator=(const OutputCoordinator&) = delete;

    [[nodiscard]] Expected<void> init(rhi::RhiDevice& device, const OutputConfig& cfg);
    void shutdown() noexcept;

    /** Updates non-structural flags or resizes/toggles outputs cleanly when config changes on T4. */
    void apply_config(const OutputConfig& cfg) noexcept;

    /** Consumes one ready slot from T3: waits `frame_timeline_ >= slot.frame_timeline_value`, publishes enabled
     * outputs, computes `dt_video`, updates `DspSyncBridge`, emits OSC telemetry, and writes stats. */
    [[nodiscard]] uint64_t process_slot(const OutputRingSlot& slot, float observed_fps) noexcept;

    [[nodiscard]] bool is_rgb_vcam_pin_connected() const noexcept { return vcam_.is_rgb_pin_connected(); }
    [[nodiscard]] OutputRuntimeStatus snapshot_status() const noexcept { return status_; }
    [[nodiscard]] DspSyncBridge& dsp_bridge() noexcept { return dsp_; }
    [[nodiscard]] const DspSyncBridge& dsp_bridge() const noexcept { return dsp_; }
    [[nodiscard]] SpoutOutput& spout() noexcept { return spout_; }
    [[nodiscard]] VcamTransport& vcam() noexcept { return vcam_; }
    [[nodiscard]] NdiOutput& ndi() noexcept { return ndi_; }
    [[nodiscard]] OscSender& osc() noexcept { return osc_; }

  private:
    rhi::RhiDevice* device_{nullptr};
    OutputConfig cfg_{};
    SpoutOutput spout_{};
    VcamTransport vcam_{};
    NdiOutput ndi_{};
    DspSyncBridge dsp_{};
    OscSender osc_{};
    OutputRuntimeStatus status_{};
    uint64_t last_process_qpc_us_{0};
    bool initialized_{false};
};

} // namespace aether::output
