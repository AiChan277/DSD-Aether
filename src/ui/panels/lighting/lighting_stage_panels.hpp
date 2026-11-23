#pragma once

#include "renderer/lighting_rig.hpp"
#include "renderer/preview_pass.hpp"
#include "renderer/studio_lighting.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace aether::ui {

/**
 * Self-contained controller and renderer for the Phase 2 Dedicated `Lighting` Tab:
 *   - Lighting Outliner (+ 12 Fixtures, + Neg Fill / Bounce Flags, Eye, Solo, Anchors)
 *   - Lighting Stage Viewport (Free + 6 Ortho Views, ViewCube, Quad View Ctrl+Alt+Q,
 *     Lit / Clay 18% Gray / False Color EL-Zone, Parallel Ray-Picking, 3D Gizmo,
 *     Pilot Light, Aim At Target, Dockable PiP Broadcast Monitor)
 *   - Light & Modifier Inspector (Full Photometry, CCT/Gels, Softbox/Grid/Barn Doors/Gobo/IES)
 *   - Light Mixer Console (16-Slot Table + 5 Built-in Presets + Save/Load `.alfx` in `presets/alfx`)
 */
class LightingStagePanels {
  public:
    LightingStagePanels() noexcept = default;

    void set_config_dir(std::filesystem::path dir) noexcept { config_dir_ = std::move(dir); }
    void set_shipped_config_dir(std::filesystem::path dir) noexcept { shipped_config_dir_ = std::move(dir); }
    void set_active_preset_index(int idx) noexcept { active_preset_idx_ = std::clamp(idx, 0, 4); }
    void ensure_initialized(DevShellViewportActions& vp_actions) noexcept;

    void set_stage_view(renderer::StageCameraView view, const std::array<float, 3>& pivot_ws) noexcept;
    void toggle_quad_view() noexcept;
    void toggle_pilot_light(DevShellViewportActions& vp_actions, const std::array<float, 3>& pivot_ws,
                            const std::array<float, 3>& eye_ws) noexcept;

    [[nodiscard]] renderer::StageCameraView stage_view() const noexcept { return stage_view_; }
    [[nodiscard]] renderer::StageDiagnosticMode diagnostic_mode() const noexcept { return diag_mode_; }
    [[nodiscard]] bool quad_view_enabled() const noexcept { return quad_view_; }
    [[nodiscard]] bool pilot_light_active() const noexcept { return pilot_light_; }
    [[nodiscard]] bool preview_25d_flatten() const noexcept { return preview_25d_flatten_; }
    [[nodiscard]] int selected_light_index() const noexcept { return selected_light_idx_; }
    [[nodiscard]] const std::vector<renderer::StudioRigPresetRecord>& rig_presets() const noexcept {
        return saved_rigs_;
    }
    [[nodiscard]] float last_stage_draw_w() const noexcept { return last_stage_draw_w_; }
    [[nodiscard]] float last_stage_draw_h() const noexcept { return last_stage_draw_h_; }
    [[nodiscard]] const renderer::PreviewCamera& stage_camera() const noexcept { return stage_camera_; }
    [[nodiscard]] const renderer::PreviewCamera& quad_top_camera() const noexcept { return quad_top_camera_; }
    [[nodiscard]] const renderer::PreviewCamera& quad_right_camera() const noexcept { return quad_right_camera_; }
    [[nodiscard]] std::array<float, 4> last_pip_inner_rect() const noexcept { return last_pip_inner_rect_; }
    [[nodiscard]] float last_toolbar_height() const noexcept { return last_toolbar_height_; }
    /** Right edge of the last toolbar widget vs the toolbar's available right edge (screen px). */
    [[nodiscard]] float last_toolbar_right() const noexcept { return last_toolbar_right_; }
    [[nodiscard]] float last_toolbar_limit() const noexcept { return last_toolbar_limit_; }
    [[nodiscard]] std::array<float, 4> last_canvas_rect() const noexcept { return last_canvas_rect_; }
    [[nodiscard]] std::array<float, 4> last_viewcube_rect() const noexcept { return last_viewcube_rect_; }
    [[nodiscard]] std::array<float, 4> last_pip_outer_rect() const noexcept { return last_pip_outer_rect_; }
    [[nodiscard]] std::array<float, 4> last_pip_uv() const noexcept { return last_pip_uv_; }
    [[nodiscard]] bool pip_monitor_open() const noexcept { return pip_monitor_open_; }
    [[nodiscard]] std::array<float, 4> last_quad_rect(int idx) const noexcept {
        return (idx >= 0 && idx < 4) ? last_quad_rects_[static_cast<size_t>(idx)] : std::array<float, 4>{};
    }

    void draw_outliner(const DevShellAvatarView& avatar, const UiFontSet& fonts, UiSelection& selection,
                       StudioActions& actions) noexcept;

    void draw_stage_viewport(const DevShellAvatarView& avatar, const UiFontSet& fonts, float dpi_scale,
                             UiSelection& selection, StudioActions& actions) noexcept;

    void draw_inspector(const DevShellAvatarView& avatar, const UiFontSet& fonts, ParamStore& params,
                        UiSelection& selection, StudioActions& actions) noexcept;

    void draw_mixer_console(const DevShellAvatarView& avatar, const UiFontSet& fonts, UiSelection& selection,
                            StudioActions& actions);

    void export_stage_actions(const DevShellAvatarView& avatar, DevShellViewportActions& vp_actions) noexcept;

  private:
    [[nodiscard]] renderer::LightingRig& active_rig(DevShellViewportActions& vp_actions) noexcept;
    [[nodiscard]] std::filesystem::path rigs_json_path() const;
    void ensure_rigs_loaded() noexcept;
    void sync_store_dirty(const renderer::LightingRig& rig, DevShellViewportActions& vp_actions) noexcept;

    std::filesystem::path config_dir_{};
    std::filesystem::path shipped_config_dir_{};
    renderer::LightingRig fallback_rig_{renderer::LightingRig::default_three_point()};
    std::vector<renderer::StudioRigPresetRecord> saved_rigs_{};
    bool rigs_loaded_{false};
    int active_preset_idx_{0};
    int selected_light_idx_{0};
    bool outliner_selected_cam_{false};

    std::array<char, 64> search_buf_{};
    std::array<char, 64> save_name_buf_{"Custom Studio Rig"};

    renderer::StageCameraView stage_view_{renderer::StageCameraView::FreePerspective};
    renderer::PreviewCamera stage_camera_{};
    renderer::PreviewCamera quad_top_camera_{};
    renderer::PreviewCamera quad_right_camera_{};
    bool stage_cam_ready_{false};

    renderer::StageDiagnosticMode diag_mode_{renderer::StageDiagnosticMode::Lit};
    bool show_schematic_{true};
    bool show_grid_{true};
    bool preview_25d_flatten_{false};
    bool quad_view_{false};
    bool pip_monitor_open_{true};
    bool pilot_light_{false};
    renderer::PreviewCamera saved_before_pilot_{};
    bool has_saved_before_pilot_{false};

    GizmoTool gizmo_tool_{GizmoTool::Move};
    bool gizmo_dragging_{false};
    int gizmo_active_axis_{-1};
    bool fast_panning_{false};
    uint32_t desired_stage_w_{960};
    uint32_t desired_stage_h_{640};
    uint32_t desired_quad_top_w_{480};
    uint32_t desired_quad_top_h_{320};
    uint32_t desired_quad_right_w_{480};
    uint32_t desired_quad_right_h_{320};
    float last_stage_draw_w_{960.0f};
    float last_stage_draw_h_{640.0f};
    std::array<float, 4> last_pip_inner_rect_{};
    std::array<float, 4> last_pip_outer_rect_{};
    std::array<float, 4> last_pip_uv_{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<float, 4> last_canvas_rect_{};
    std::array<float, 4> last_viewcube_rect_{};
    float last_toolbar_height_{0.0f};
    float last_toolbar_right_{0.0f};
    float last_toolbar_limit_{0.0f};
    bool mouse_over_overlay_{false};
    std::array<std::array<float, 4>, 4> last_quad_rects_{};
};

} // namespace aether::ui
