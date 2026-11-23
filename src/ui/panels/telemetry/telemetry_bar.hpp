#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>

namespace aether::ui {

enum class TopMenuAction : uint8_t {
    None = 0,
    OpenPalette,
    OpenWizard,
    OpenPreferences,
    OpenGraphicsSettings,
    OpenShortcuts,
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,
    ImportAvatar,
    OpenProjectManager,
    ExportAlfx,
    ImportAlfx,
    ResetLayout,
    Undo,
    Redo,
};

/**
 * Z5 Top Studio Menu/Mode Bar (`32 px`) and Bottom Telemetry Strip (`24 px`, 4 Hz sampling,
 * 500 ms color hysteresis §12.8) + Clickable Performance Breakdown Popover.
 */
class TelemetryBar {
  public:
    TopMenuAction draw_top_bar(double now_s, StudioMode& mode, bool& mode_changed, const rhi::RhiDevice& rhi,
                               bool ffi_layout_ok, const StudioHostStatus& host_status, const UndoStack& undo,
                               const HardwareInventory& hw, const DevShellTrackingView& tracking, ParamStore& params,
                               AppSettings& settings, bool& settings_dirty, const UiFontSet& fonts, float dpi_scale,
                               StudioActions& actions);

    void draw_bottom_bar(double now_s, const TelemetrySnapshot& telemetry, const ImGuiArenaAllocator& arena,
                         const rhi::RhiDevice& rhi, const DevShellTrackingView& tracking,
                         const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                         const StudioHostStatus& host_status, const UiFontSet& fonts, float dpi_scale,
                         StudioActions& actions);

  private:
    double on_air_since_s_{-1.0};
    double last_sample_s_{-1.0};
    double over_budget_since_s_{-1.0};
    double ok_since_s_{0.0};
    bool hysteresis_over_budget_{false};

    std::array<char, 64> t1_buf_{"T1: --"};
    std::array<char, 64> t2_buf_{"T2: --"};
    std::array<char, 64> t3_buf_{"T3: --"};
    std::array<char, 64> t4_buf_{"T4: --"};
    std::array<char, 64> m2p_buf_{"M2P -- / -- ms"};
    std::array<char, 64> dsp_buf_{"DSP: --"};
    std::array<char, 64> ui_buf_{"UI: --"};
};

} // namespace aether::ui
