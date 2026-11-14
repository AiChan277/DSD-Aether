#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/workspace/studio_types.hpp"

#include <span>

namespace aether::ui {

/**
 * Z3 Properties Inspector (`§12.4`, `§12.5`, `§12.12`, broadcast overhaul §1 BUG-05/06).
 * Default dock width 325 px * dpi. Every collapsible section is a DaVinci Resolve-style 2-column
 * property table (label / full-width control) rendered via single-source `draw_property_row(ParamId)`
 * with scrubbable sliders, amber reset dots, tooltips, context menus, and inline validation errors.
 * Status readouts render as wrapping bordered badges so long values never overflow the panel.
 */
class InspectorPanel {
  public:
    /** Opens, focuses and scrolls to the `Output & Audio-DSP` section on the next draw. */
    void request_focus_output_section() noexcept { focus_output_pending_ = true; }
    [[nodiscard]] bool focus_output_pending() const noexcept { return focus_output_pending_; }

    void draw(StudioMode mode, bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi, bool ffi_layout_ok,
              const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
              const DevShellRenderView& render_view, const DevShellOutputView& output_view,
              const StudioHostStatus& host_status, std::span<const LoadedPluginVm> plugins,
              const UiSelection& selection, ParamStore& params, UndoStack& undo, AppSettings& settings,
              bool& settings_dirty, const UiFontSet& fonts, PropertyRowActions& row_actions, StudioActions& actions);

  private:
    bool focus_output_pending_{false};
};

} // namespace aether::ui
