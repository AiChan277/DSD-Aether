#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>

namespace aether::ui {

/**
 * Z2 Center 3D Viewport, Interactive 112 px Light Orbit Orb (`Alt+L`),
 * Confidence Heatmap Legend & Overlay (`Alt+C`), Gizmo Toolbar (`Q/W/E/R/X/G`),
 * Camera Bookmarks (`1-5` / `Ctrl+Alt+1-5`), and Top-Right Tracking HUD Pill (`§12.6`).
 */
class ViewportPanel {
  public:
    void recall_camera_bookmark(size_t idx, StudioActions& actions);
    void store_camera_bookmark(size_t idx);

    [[nodiscard]] GizmoTool gizmo_tool() const noexcept { return gizmo_tool_; }
    void set_gizmo_tool(GizmoTool tool) noexcept { gizmo_tool_ = tool; }
    void toggle_local_world() noexcept { gizmo_local_ = !gizmo_local_; }
    void toggle_grid() noexcept { show_grid_ = !show_grid_; }

    void draw(const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
              const DevShellRenderView& render_view, const DevShellInputEcho& input_echo, bool tracking_paused,
              ParamStore& params, UndoStack& undo, const UiFontSet& fonts, float dpi_scale, StudioActions& actions);

  private:
    void draw_light_orbit_orb(const DevShellAvatarView& avatar, ParamStore& params, UndoStack& undo,
                              const UiFontSet& fonts, float dpi_scale, ImVec2 orb_pos, StudioActions& actions);

    void draw_confidence_heatmap_legend(const DevShellTrackingView& tracking, const UiFontSet& fonts, float dpi_scale,
                                        ImVec2 pos);

    GizmoTool gizmo_tool_{GizmoTool::Select};
    bool gizmo_local_{true};
    bool show_grid_{true};
    bool orb_dragging_{false};
    float cam_yaw_deg_{0.0f};
    float cam_pitch_deg_{8.0f};
    float cam_dist_{2.1f};
    float cam_target_y_{1.15f};
    std::array<CameraBookmark, 5> bookmarks_{};
};

} // namespace aether::ui
