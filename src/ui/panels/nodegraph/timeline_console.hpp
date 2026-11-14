#pragma once

#include "core/log.hpp"
#include "core/telemetry.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <span>

namespace aether::ui {

/** Result flags emitted by the Console tab toolbar. */
struct ConsoleTabResult {
    const char* copied_notice{nullptr};
    bool clear_requested{false};
};

/**
 * Z4 Auxiliary Bottom Tabs alongside Node Graph (`§12.7`, `§12.13`, `§12.15`):
 * - Console: clipped `ImGuiListClipper` log view with Info/Warn/Error + text filters, Clear, Copy (all
 *   visible lines) and click-to-copy single lines.
 * - Expressions & Hotkeys: `F1-F8` expression matrix with latch toggles and live Hermite-ramped weights.
 * - Performance Timeline: rolling 240-frame ImPlot of T1 NPU / T2 Sim-IK / T3 GPU / T4 Output with the
 *   20.0 ms motion-to-photon budget line.
 */
class TimelineConsole {
  public:
    static constexpr size_t kPlotHistory = 240;

    TimelineConsole() noexcept;

    void push_sample(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                     const DevShellTrackingView& tracking, bool paused) noexcept;

    ConsoleTabResult draw_console_tab(std::span<const LogRecord> logs, size_t log_head, size_t log_count,
                                      const UiFontSet& fonts);

    void draw_expressions_tab(std::span<ExpressionSlotInfo> expressions, const UiFontSet& fonts);

    void draw_timeline_tab(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                           const DevShellTrackingView& tracking, ParamStore& params, const UiFontSet& fonts);

    [[nodiscard]] size_t sample_count() const noexcept { return sample_count_; }

  private:
    bool filter_info_{true};
    bool filter_warn_{true};
    bool filter_err_{true};
    bool auto_scroll_{true};
    std::array<char, 64> text_filter_{};
    std::array<size_t, 256> filtered_rows_{};

    size_t write_idx_{0};
    size_t sample_count_{0};
    std::array<float, kPlotHistory> hist_t1_{};
    std::array<float, kPlotHistory> hist_t2_{};
    std::array<float, kPlotHistory> hist_t3_{};
    std::array<float, kPlotHistory> hist_t4_{};
    std::array<float, kPlotHistory> hist_m2p_{};
};

} // namespace aether::ui
