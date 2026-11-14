#pragma once

#include "core/log.hpp"
#include "core/telemetry.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <span>

namespace aether::ui {

/**
 * Z4 Auxiliary Bottom Tabs alongside Node Graph (`§12.7`, `§12.13`, `§12.15`):
 * - Console: clipped `ImGuiListClipper` log view with Info/Warn/Error filters and click-to-copy.
 * - Expressions & Hotkeys: `F1-F8` expression matrix & live triggers.
 * - Performance Timeline: ImPlot T1/T2/T3/T4 breakdown with 20.0 ms M2P budget line.
 */
class TimelineConsole {
  public:
    static constexpr size_t kPlotHistory = 120;

    TimelineConsole() noexcept;

    void push_sample(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                     const DevShellTrackingView& tracking, bool paused) noexcept;

    void draw_console_tab(std::span<const LogRecord> logs, size_t log_head, size_t log_count, const UiFontSet& fonts,
                          const char*& out_copied_notice);

    void draw_expressions_tab(std::span<ExpressionSlotInfo> expressions, const UiFontSet& fonts,
                              StudioActions& actions);

    void draw_timeline_tab(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                           const DevShellTrackingView& tracking, ParamStore& params, const UiFontSet& fonts);

  private:
    bool filter_info_{true};
    bool filter_warn_{true};
    bool filter_err_{true};
    bool auto_scroll_{true};

    size_t write_idx_{0};
    std::array<float, kPlotHistory> x_vals_{};
    std::array<float, kPlotHistory> hist_t1_{};
    std::array<float, kPlotHistory> hist_t2_{};
    std::array<float, kPlotHistory> hist_t3_{};
    std::array<float, kPlotHistory> hist_t4_{};
    std::array<float, kPlotHistory> hist_m2p_{};
};

} // namespace aether::ui
