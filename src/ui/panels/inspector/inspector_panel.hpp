#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/workspace/studio_types.hpp"

#include <span>

namespace aether::ui {

/**
 * Z3 Properties Inspector (`§12.4`, `§12.5`, `§12.12`).
 * Width: 320 px Live Studio / 340 px Rigging.
 * Every control renders via single-source `draw_property_row(ParamId)` with amber reset dots,
 * 450 ms tooltips, context menus, and inline validation errors.
 */
class InspectorPanel {
  public:
    void draw(StudioMode mode, bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi, bool ffi_layout_ok,
              const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
              const DevShellRenderView& render_view, const DevShellOutputView& output_view,
              const StudioHostStatus& host_status, std::span<const LoadedPluginVm> plugins,
              const UiSelection& selection, ParamStore& params, UndoStack& undo, const UiFontSet& fonts,
              PropertyRowActions& row_actions, StudioActions& actions);
};

} // namespace aether::ui
