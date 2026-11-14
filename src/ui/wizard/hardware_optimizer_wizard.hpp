#pragma once

#include "core/hw_enum.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <cstdint>

namespace aether::ui {

struct WizardRecommendation {
    int32_t tracking_ep_mode{0};  // ParamId::TrackingEpMode index (0=Auto,1=NPU,2=dGPU,3=iGPU,4=CPU)
    int32_t ep_override{-1};      // -1 Auto, else AetherEpTier
    uint8_t denoiser_kind{1};     // ParamId::RenderDenoiser
    uint8_t output_res_preset{1}; // 0=720p, 1=1080p, 2=1440p, 3=4K
    uint32_t output_fps{60};
    bool rt_enabled{true};
    bool downshifted_by_budget{false};
    std::array<char, 160> ep_reason{};
    std::array<char, 160> render_reason{};
};

/**
 * 5-Step Hardware Optimizer Wizard (`780x526` modal, `§12.13`).
 * Blocked while `ON AIR`; `Esc` or `Skip` closes without modifying parameters.
 */
class HardwareOptimizerWizard {
  public:
    void open(const HardwareInventory& hw, const rhi::SelectedGpuInfo& gpu, bool on_air) noexcept;
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    [[nodiscard]] static WizardRecommendation recommend_for_hardware(const HardwareInventory& hw,
                                                                     const rhi::SelectedGpuInfo& gpu) noexcept;

    [[nodiscard]] static uint64_t compute_inventory_hash(const HardwareInventory& hw) noexcept;

    /** Downshifts quality preset cleanly when measured M2P exceeds the 20.0 ms budget (`§12.13`). */
    [[nodiscard]] static bool adjust_for_budget(WizardRecommendation& rec, float p95_m2p_ms) noexcept;

    /** Draws modal; returns true when user finishes and applies settings. */
    [[nodiscard]] bool draw(bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi,
                            const TelemetrySnapshot& telemetry, const DevShellTrackingView& tracking,
                            const DevShellRenderView& render_view, ParamStore& params, AppSettings& settings,
                            const UiFontSet& fonts, float dpi_scale, StudioActions& actions);

  private:
    bool open_{false};
    int step_{0}; // 0..4
    WizardRecommendation rec_{};
    int selected_camera_idx_{-1}; // -1 = Synthetic / First Available
    bool synthetic_feed_{true};
    int camera_fps_{60};
    int anti_flicker_{2};

    bool benchmark_running_{false};
    uint32_t benchmark_samples_{0};
    float benchmark_max_m2p_{0.0f};
    double benchmark_sum_m2p_{0.0};
    float benchmark_avg_m2p_{0.0f};
    bool benchmark_done_{false};
};

} // namespace aether::ui
