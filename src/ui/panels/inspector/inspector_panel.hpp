#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/workspace/studio_types.hpp"

#include <span>

namespace aether::ui {

class NodeCanvas;
class ViewportPanel;

/**
 * Z3 Properties Inspector (`§12.4`, `§12.5`, `§12.12`, Rev 3.1 §1.3).
 * Default dock width 325 px * dpi. Strictly contextual per `StudioMode` and selected entity:
 *   - `Live Studio`: Selected entity only (Avatar Transform & Physics; Camera SmoothDamp & Custom
 *     Presets without duplicating viewport framing/aspect; Active Lighting & FX (.alfx) selector
 *     card linking to Lighting tab; Tracking & Pipeline; Output & Audio-DSP).
 *   - `Rigging`: Bone info, humanoid mapping, SpringBone physics, and safety colliders.
 *   - `Nodes`: Selected node inspector (pins, mute/bypass, node-specific settings, recompile).
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
              bool& settings_dirty, const UiFontSet& fonts, PropertyRowActions& row_actions, StudioActions& actions,
              ViewportPanel* viewport_for_presets = nullptr, std::span<OutlinerBoneInfo> bones = {},
              std::span<OutlinerMaterialInfo> materials = {}, NodeCanvas* node_canvas = nullptr);

  private:
    bool focus_output_pending_{false};
};

} // namespace aether::ui
