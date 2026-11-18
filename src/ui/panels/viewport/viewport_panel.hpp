#pragma once

#include "core/viewport_aspect.hpp"
#include "ui/panels/viewport/camera_presets.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <string>

namespace aether::ui {

/**
 * Fill / Rim lights orbit rigidly with the Key light: their azimuth/elevation are fixed offsets from
 * the key (defaults reproduce the renderer rig: key 32/26, fill -52/12, rim 148/22). The Light Orbit
 * Gizmo and `StudioWorkspace` both derive fill/rim directions from these offsets.
 */
struct LightRigOffsets {
    static constexpr float kFillAzDeg = -84.0f;
    static constexpr float kFillElDeg = -14.0f;
    static constexpr float kRimAzDeg = 116.0f;
    static constexpr float kRimElDeg = -4.0f;
};

/** Wraps an angle in degrees into `[-180, 180)`. */
[[nodiscard]] float wrap_degrees(float deg) noexcept;

/** Camera focal-length presets offered by the viewport toolbar (full-frame 24 mm sensor height). */
struct FocalPreset {
    const char* label;
    float focal_mm; // 0 = framing default (fit camera)
};

/**
 * Z2 Center 3D Viewport (`§12.6`, broadcast overhaul §1 BUG-03/07, §4 Zone 2):
 * - Translucent floating toolbar pill anchored at the client-area origin (`GetCursorScreenPos()` after
 *   `Begin`) + 12/10 px * dpi: focal-length picker, Grid (thirds + safe areas), Confidence Skeleton
 *   (`Alt+C`), SDF Shadow View, Ray Tracing, Light Orb and Reset Camera.
 * - Camera: left-drag orbits, middle-drag / right-drag / Alt+right-drag pans, wheel zooms
 *   multiplicatively (distance-proportional). Bookmarks `1-5` / `Ctrl+Alt+1-5`.
 * - Top-right tracking HUD pill with pause + `Calibrate` (neutral pose / face zero-offset capture).
 * - Dark vector Light Orbit Gizmo: Key (orange) / Fill (cyan) / Rim (green) handles rotate the light
 *   rig and stay in sync with the Inspector `Key Azimuth` / `Key Elevation` sliders.
 */
class ViewportPanel {
  public:
    static constexpr size_t kFocalPresetCount = 6;
    static const std::array<FocalPreset, kFocalPresetCount>& focal_presets() noexcept;

    void recall_camera_bookmark(size_t idx, StudioActions& actions);
    void store_camera_bookmark(size_t idx);
    void reset_camera(StudioActions& actions) noexcept;

    [[nodiscard]] CameraPresetBank& preset_bank() noexcept { return preset_bank_; }
    [[nodiscard]] const CameraPresetBank& preset_bank() const noexcept { return preset_bank_; }
    void set_persist_presets(bool persist) noexcept { persist_presets_ = persist; }
    [[nodiscard]] bool persist_presets() const noexcept { return persist_presets_; }
    void apply_builtin_preset(int32_t preset_idx, const DevShellAvatarView& avatar, const ParamStore& params,
                              StudioActions& actions) noexcept;
    void apply_custom_preset(size_t custom_idx, ParamStore& params, UndoStack& undo, StudioActions& actions) noexcept;
    void save_custom_preset_from_current(std::string name, const DevShellAvatarView& avatar, const ParamStore& params);
    void open_save_preset_modal(const std::string& initial_name = {});

    [[nodiscard]] core::ViewportAspectLayout last_aspect_layout() const noexcept { return last_aspect_layout_; }
    [[nodiscard]] bool last_hud_drawn() const noexcept { return last_hud_drawn_; }
    [[nodiscard]] ImVec2 last_hud_p0() const noexcept { return last_hud_p0_; }
    [[nodiscard]] ImVec2 last_hud_p1() const noexcept { return last_hud_p1_; }
    [[nodiscard]] ImVec2 last_orb_pos() const noexcept { return last_orb_pos_; }
    [[nodiscard]] float last_orb_size() const noexcept { return last_orb_size_; }
    [[nodiscard]] int32_t active_builtin_preset() const noexcept { return active_builtin_preset_; }
    [[nodiscard]] const std::string& active_preset_name() const noexcept { return active_preset_name_; }

    [[nodiscard]] GizmoTool gizmo_tool() const noexcept { return gizmo_tool_; }
    void set_gizmo_tool(GizmoTool tool) noexcept { gizmo_tool_ = tool; }
    void toggle_local_world() noexcept { gizmo_local_ = !gizmo_local_; }
    [[nodiscard]] bool gizmo_local() const noexcept { return gizmo_local_; }
    void toggle_grid() noexcept { show_grid_ = !show_grid_; }
    [[nodiscard]] bool grid_visible() const noexcept { return show_grid_; }
    void toggle_wireframe() noexcept { wireframe_enabled_ = !wireframe_enabled_; }
    [[nodiscard]] bool wireframe_enabled() const noexcept { return wireframe_enabled_; }
    void set_wireframe_enabled(bool enabled) noexcept { wireframe_enabled_ = enabled; }
    void toggle_rig_colliders() noexcept { rig_colliders_enabled_ = !rig_colliders_enabled_; }
    [[nodiscard]] bool rig_colliders_enabled() const noexcept { return rig_colliders_enabled_; }
    void set_rig_colliders_enabled(bool enabled) noexcept { rig_colliders_enabled_ = enabled; }
    [[nodiscard]] bool is_focused() const noexcept { return viewport_focused_; }
    [[nodiscard]] int focal_preset_index() const noexcept { return focal_idx_; }

    void draw(StudioMode mode, const HardwareInventory& hw, const DevShellAvatarView& avatar,
              const DevShellTrackingView& tracking, const DevShellRenderView& render_view,
              const DevShellInputEcho& input_echo, bool tracking_paused, ParamStore& params, UndoStack& undo,
              AppSettings& settings, bool& settings_dirty, const UiFontSet& fonts, float dpi_scale,
              StudioActions& actions);

  private:
    ImVec2 draw_toolbar(ImVec2 anchor, float max_width, StudioMode mode, const DevShellAvatarView& avatar,
                        ParamStore& params, UndoStack& undo, bool& settings_dirty, float scale, StudioActions& actions);
    bool draw_transform_gizmo(const DevShellAvatarView& avatar, ParamStore& params, UndoStack& undo,
                              const UiFontSet& fonts, ImVec2 frame_p0, ImVec2 frame_p1, float effective_fovy_rad,
                              float dpi_scale);
    bool draw_tracking_hud(ImVec2 top_right, float min_y, float canvas_bottom_y, const HardwareInventory& hw,
                           const DevShellTrackingView& tracking, const DevShellRenderView& render_view,
                           bool tracking_paused, ParamStore& params, AppSettings& settings, bool& settings_dirty,
                           const UiFontSet& fonts, float scale, StudioActions& actions, ImVec2& out_p0, ImVec2& out_p1);
    void draw_save_preset_modal(const DevShellAvatarView& avatar, const ParamStore& params, float scale);
    void draw_composition_grid(ImVec2 p0, ImVec2 p1, float scale) const;
    void handle_camera_input(bool hovered, bool active, float scale, StudioActions& actions);
    void draw_light_orbit_gizmo(ParamStore& params, UndoStack& undo, const UiFontSet& fonts, float dpi_scale,
                                ImVec2 orb_pos, StudioActions& actions);
    void draw_confidence_heatmap_legend(const DevShellTrackingView& tracking, const UiFontSet& fonts, float dpi_scale,
                                        ImVec2 pos);

    GizmoTool gizmo_tool_{GizmoTool::Select};
    bool gizmo_local_{true};
    bool show_grid_{false};
    bool wireframe_enabled_{false};
    bool rig_colliders_enabled_{false};
    bool viewport_focused_{false};
    int focal_idx_{0};
    uint8_t sdf_restore_debug_view_{0};

    // 3D Transform Gizmo hover/drag state (zero per-frame heap allocations)
    uint32_t viewport_input_id_{0};
    bool gizmo_dragging_{false};
    int gizmo_hover_handle_{-1};  // -1=none, 0=X, 1=Y, 2=Z, 3=center/rotY
    int gizmo_active_handle_{-1};
    GizmoTool gizmo_drag_tool_{GizmoTool::Select};
    float gizmo_drag_start_t_{0.0f};
    float gizmo_drag_start_angle_{0.0f};
    float gizmo_drag_origin_[3]{0.0f, 0.0f, 0.0f};
    float gizmo_drag_axis_[3]{1.0f, 0.0f, 0.0f};
    float gizmo_drag_start_vals_[5]{0.0f, 0.0f, 0.0f, 0.0f, 1.0f}; // px, py, pz, rot_y, scale
    std::array<char, 128> gizmo_before_payload_{};

    // Light orbit gizmo drag state (0 = key, 1 = fill, 2 = rim)
    bool orb_dragging_{false};
    int orb_drag_handle_{0};
    bool orb_drag_back_hemisphere_{false};
    double orb_drag_start_az_{0.0};
    double orb_drag_start_el_{0.0};

    // UI-side camera estimate (radians, mirrors the app's PreviewCamera defaults) for bookmarks.
    float cam_yaw_rad_{0.35f};
    float cam_pitch_rad_{0.10f};
    float cam_log_zoom_{0.0f};
    std::array<CameraBookmark, 5> bookmarks_{};

    CameraPresetBank preset_bank_{};
    bool persist_presets_{true};
    int32_t active_builtin_preset_{0};
    std::string active_preset_name_{"1: Bust / Streamer (Default)"};
    bool save_preset_modal_open_{false};
    std::array<char, 64> save_preset_name_buf_{};

    core::ViewportAspectLayout last_aspect_layout_{};
    bool last_hud_drawn_{false};
    ImVec2 last_hud_p0_{};
    ImVec2 last_hud_p1_{};
    ImVec2 last_orb_pos_{};
    float last_orb_size_{0.0f};
};

} // namespace aether::ui
