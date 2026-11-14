#include "ui/panels/viewport/viewport_panel.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace aether::ui {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kOrbitRadPerPx = 0.0075f;
constexpr float kPanPerPx = 0.0022f;
constexpr float kZoomPerWheel = 0.12f;
constexpr uint8_t kDebugViewSdfFace = 5;

struct GizmoVec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

inline GizmoVec3 operator+(GizmoVec3 a, GizmoVec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline GizmoVec3 operator-(GizmoVec3 a, GizmoVec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline GizmoVec3 operator*(GizmoVec3 a, float s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
inline float dot3(GizmoVec3 a, GizmoVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline GizmoVec3 cross3(GizmoVec3 a, GizmoVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float len3(GizmoVec3 v) noexcept {
    return std::sqrt(dot3(v, v));
}
inline GizmoVec3 norm3(GizmoVec3 v, GizmoVec3 fallback = {0.0f, 1.0f, 0.0f}) noexcept {
    const float l = len3(v);
    return (l > 1e-6f) ? GizmoVec3{v.x / l, v.y / l, v.z / l} : fallback;
}

struct GizmoCameraBasis {
    GizmoVec3 eye{0.0f, 1.11f, 2.0f};
    GizmoVec3 fwd{0.0f, 0.0f, -1.0f};
    GizmoVec3 right{1.0f, 0.0f, 0.0f};
    GizmoVec3 up{0.0f, 1.0f, 0.0f};
    ImVec2 p0{0.0f, 0.0f};
    float w{64.0f};
    float h{64.0f};
    float fx{1.0f};
    float fy{1.0f};

    static GizmoCameraBasis build(const DevShellAvatarView& avatar, ImVec2 canvas_p0, ImVec2 canvas_p1) noexcept {
        GizmoCameraBasis cam{};
        cam.p0 = canvas_p0;
        cam.w = std::max(64.0f, canvas_p1.x - canvas_p0.x);
        cam.h = std::max(64.0f, canvas_p1.y - canvas_p0.y);
        const float aspect = cam.w / cam.h;
        cam.eye = {avatar.camera_eye[0], avatar.camera_eye[1], avatar.camera_eye[2]};
        const GizmoVec3 tgt{avatar.camera_target[0], avatar.camera_target[1], avatar.camera_target[2]};
        cam.fwd = norm3(tgt - cam.eye, {0.0f, -0.1f, -1.0f});
        const GizmoVec3 world_up =
            (std::abs(cam.fwd.y) > 0.99f) ? GizmoVec3{0.0f, 0.0f, 1.0f} : GizmoVec3{0.0f, 1.0f, 0.0f};
        cam.right = norm3(cross3(cam.fwd, world_up), {1.0f, 0.0f, 0.0f});
        cam.up = cross3(cam.right, cam.fwd);
        const float fovy = std::clamp(avatar.camera_fovy_rad, 0.1f, 2.5f);
        cam.fy = 1.0f / std::tan(fovy * 0.5f);
        cam.fx = cam.fy / std::max(0.1f, aspect);
        return cam;
    }

    [[nodiscard]] bool project(GizmoVec3 p, ImVec2& out_screen) const noexcept {
        const GizmoVec3 d = p - eye;
        const float pw = dot3(fwd, d);
        if (pw <= 0.05f) {
            return false;
        }
        const float ndc_x = (fx * dot3(right, d)) / pw;
        const float ndc_y = -(fy * dot3(up, d)) / pw;
        out_screen.x = p0.x + (ndc_x * 0.5f + 0.5f) * w;
        out_screen.y = p0.y + (ndc_y * 0.5f + 0.5f) * h;
        return true;
    }

    void screen_ray(ImVec2 mp, GizmoVec3& out_origin, GizmoVec3& out_dir) const noexcept {
        const float ndc_x = ((mp.x - p0.x) / w) * 2.0f - 1.0f;
        const float ndc_y_up = -(((mp.y - p0.y) / h) * 2.0f - 1.0f);
        out_origin = eye;
        out_dir = norm3(fwd + right * (ndc_x / fx) + up * (ndc_y_up / fy), fwd);
    }
};

float dist_point_segment(ImVec2 p, ImVec2 a, ImVec2 b) noexcept {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float denom = abx * abx + aby * aby;
    if (denom <= 1e-5f) {
        return std::hypot(p.x - a.x, p.y - a.y);
    }
    const float t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / denom, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + t * abx), p.y - (a.y + t * aby));
}

bool closest_t_on_axis(GizmoVec3 axis_origin, GizmoVec3 axis_dir, GizmoVec3 ray_origin, GizmoVec3 ray_dir,
                       float& out_t) noexcept {
    const GizmoVec3 w0 = axis_origin - ray_origin;
    const float b = dot3(axis_dir, ray_dir);
    const float d = dot3(axis_dir, w0);
    const float e = dot3(ray_dir, w0);
    const float denom = 1.0f - b * b;
    if (denom <= 1e-3f) {
        return false;
    }
    out_t = (b * e - d) / denom;
    return std::isfinite(out_t);
}

bool intersect_xz_plane(float plane_y, GizmoVec3 ray_origin, GizmoVec3 ray_dir, GizmoVec3& out_hit) noexcept {
    if (std::abs(ray_dir.y) <= 1e-3f) {
        return false;
    }
    const float t = (plane_y - ray_origin.y) / ray_dir.y;
    if (!std::isfinite(t) || t <= 0.05f) {
        return false;
    }
    out_hit = ray_origin + ray_dir * t;
    return true;
}

void format_xform_payload(char* buf, size_t cap, float px, float py, float pz, float rot_y, float scale) noexcept {
    std::snprintf(buf, cap, "xform:%.5f;%.5f;%.5f;%.4f;%.5f", static_cast<double>(px), static_cast<double>(py),
                  static_cast<double>(pz), static_cast<double>(rot_y), static_cast<double>(scale));
}

void draw_styled_legend_segment(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, int style_kind,
                                float thickness) noexcept {
    if (style_kind == 0) {
        dl->AddLine(a, b, col, thickness);
    } else if (style_kind == 1) {
        const float dx = b.x - a.x;
        for (float x = 0.0f; x < dx; x += 8.0f) {
            const float x2 = std::min(dx, x + 5.0f);
            dl->AddLine(ImVec2(a.x + x, a.y), ImVec2(a.x + x2, b.y), col, thickness);
        }
    } else {
        const float dx = b.x - a.x;
        for (float x = 2.0f; x < dx; x += 5.0f) {
            dl->AddCircleFilled(ImVec2(a.x + x, a.y), thickness * 0.65f, col);
        }
    }
}

/** Compact toggle button for the floating toolbar; returns true when clicked. */
bool toolbar_toggle(const char* label, bool active, float h, const char* tooltip) {
    const PrecisionDarkTokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_Button, active ? t.accent_button : t.transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? t.accent_hover : t.bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.accent_pressed);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? t.text_on_accent : t.text_primary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool clicked = ImGui::Button(label, ImVec2(0.0f, h));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

void toggle_bool_param(ParamStore& params, UndoStack& undo, ParamId pid) {
    const double old_v = params.get(pid);
    const double new_v = params.get_bool(pid) ? 0.0 : 1.0;
    if (params.set(pid, new_v)) {
        undo.push_param(pid, old_v, new_v);
    }
}

/** Projects an orbit light direction onto the gizmo disc; `z > 0` = front hemisphere (facing the viewer). */
struct OrbProjection {
    ImVec2 pt{};
    float z{0.0f};
};

OrbProjection project_orbit(ImVec2 center, float r, float az_deg, float el_deg) noexcept {
    const float az = az_deg * kDegToRad;
    const float el = el_deg * kDegToRad;
    return OrbProjection{ImVec2(center.x + r * std::cos(el) * std::sin(az), center.y - r * std::sin(el)),
                         std::cos(el) * std::cos(az)};
}

} // namespace

float wrap_degrees(float deg) noexcept {
    float d = std::fmod(deg + 180.0f, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d - 180.0f;
}

const std::array<FocalPreset, ViewportPanel::kFocalPresetCount>& ViewportPanel::focal_presets() noexcept {
    static const std::array<FocalPreset, kFocalPresetCount> kPresets = {{
        {"Fit (Auto)", 0.0f},
        {"24mm Wide", 24.0f},
        {"35mm", 35.0f},
        {"50mm Normal", 50.0f},
        {"85mm Portrait", 85.0f},
        {"105mm Tele", 105.0f},
    }};
    return kPresets;
}

void ViewportPanel::recall_camera_bookmark(size_t idx, StudioActions& actions) {
    if (idx < bookmarks_.size() && bookmarks_[idx].valid) {
        const auto& bm = bookmarks_[idx];
        actions.viewport.orbit_yaw_delta += (bm.yaw_rad - cam_yaw_rad_);
        actions.viewport.orbit_pitch_delta += (bm.pitch_rad - cam_pitch_rad_);
        actions.viewport.zoom_delta += (bm.log_zoom - cam_log_zoom_);
        cam_yaw_rad_ = bm.yaw_rad;
        cam_pitch_rad_ = bm.pitch_rad;
        cam_log_zoom_ = bm.log_zoom;
    }
}

void ViewportPanel::store_camera_bookmark(size_t idx) {
    if (idx < bookmarks_.size()) {
        bookmarks_[idx] = CameraBookmark{true, cam_yaw_rad_, cam_pitch_rad_, cam_log_zoom_};
    }
}

void ViewportPanel::reset_camera(StudioActions& actions) noexcept {
    actions.viewport.reset_camera = true;
    // `fit_camera_to_avatar` restores the default framing; re-apply the chosen focal length on top.
    actions.viewport.request_focal_mm = focal_presets()[static_cast<size_t>(focal_idx_)].focal_mm;
    cam_yaw_rad_ = 0.35f;
    cam_pitch_rad_ = 0.10f;
    cam_log_zoom_ = 0.0f;
}

ImVec2 ViewportPanel::draw_toolbar(ImVec2 anchor, StudioMode mode, const DevShellAvatarView& avatar, ParamStore& params,
                                   UndoStack& undo, float scale, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bar_h = std::round(t.viewport_toolbar_h_px * scale);
    const float pad = 3.0f * scale;
    const float btn_h = bar_h - pad * 2.0f;

    // 1) Top Viewport pill (Camera, Render overlays, Guides, Reset)
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * scale, (btn_h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * scale, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(anchor.x + pad, anchor.y + pad));
    ImGui::BeginGroup();

    // Focal-length picker
    const auto& presets = focal_presets();
    char preview[48]{};
    std::snprintf(preview, sizeof(preview), "%s %s", icons::kCamera, presets[static_cast<size_t>(focal_idx_)].label);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, t.transparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, t.bg_hover);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("_ 85mm Portrait").x + ImGui::GetFrameHeight() + 12.0f * scale);
    if (ImGui::BeginCombo("##focal", preview)) {
        for (size_t i = 0; i < presets.size(); ++i) {
            const bool sel = (static_cast<int>(i) == focal_idx_);
            if (ImGui::Selectable(presets[i].label, sel)) {
                focal_idx_ = static_cast<int>(i);
                if (presets[i].focal_mm > 0.0f) {
                    actions.viewport.request_focal_mm = presets[i].focal_mm;
                } else {
                    reset_camera(actions);
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tr("Camera focal length (keeps the subject framed)"));
    }
    ImGui::SameLine();

    if (!avatar.wireframe_supported) {
        ImGui::BeginDisabled();
    }
    const bool wire_on = avatar.wireframe_supported && wireframe_enabled_;
    if (toolbar_toggle(tr("Wireframe"), wire_on, btn_h,
                       avatar.wireframe_supported ? tr("Toggle polygon wireframe mode")
                                                  : tr("Wireframe mode requires Vulkan fillModeNonSolid support"))) {
        wireframe_enabled_ = !wireframe_enabled_;
    }
    if (!avatar.wireframe_supported) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Wireframe mode requires Vulkan fillModeNonSolid support"));
        }
    }
    ImGui::SameLine();

    const bool colliders_forced = (mode == StudioMode::Rigging);
    const bool colliders_on = rig_colliders_enabled_ || colliders_forced;
    if (toolbar_toggle(tr("Colliders"), colliders_on, btn_h,
                       colliders_forced ? tr("Spring-bone collider & physics chain overlay (always shown in Rigging mode)")
                                        : tr("Toggle Spring-bone collider & physics chain wireframe overlay"))) {
        if (!colliders_forced) {
            rig_colliders_enabled_ = !rig_colliders_enabled_;
        }
    }
    ImGui::SameLine();

    if (toolbar_toggle(tr("Grid"), show_grid_, btn_h, tr("Rule-of-thirds + title/action safe guides (G)"))) {
        show_grid_ = !show_grid_;
    }
    ImGui::SameLine();
    const bool skeleton_forced = (mode == StudioMode::Rigging);
    const bool skeleton_on = params.get_bool(ParamId::AppSkeletonHeatmap) || skeleton_forced;
    if (toolbar_toggle(tr("Confidence Skeleton"), skeleton_on, btn_h,
                       skeleton_forced ? tr("Always shown in Rigging mode")
                                       : tr("Per-joint solver weight overlay (Alt+C)"))) {
        if (!skeleton_forced) {
            toggle_bool_param(params, undo, ParamId::AppSkeletonHeatmap);
        }
    }
    ImGui::SameLine();
    const int dbg = params.get_int(ParamId::RenderDebugView);
    const bool sdf_on = (dbg == kDebugViewSdfFace);
    if (toolbar_toggle(tr("SDF Shadow View"), sdf_on, btn_h, tr("Show the Face SDF shadow debug pass"))) {
        const double old_v = params.get(ParamId::RenderDebugView);
        const double new_v =
            sdf_on ? static_cast<double>(sdf_restore_debug_view_) : static_cast<double>(kDebugViewSdfFace);
        if (!sdf_on) {
            sdf_restore_debug_view_ = static_cast<uint8_t>(dbg);
        }
        if (params.set(ParamId::RenderDebugView, new_v)) {
            undo.push_param(ParamId::RenderDebugView, old_v, new_v);
        }
    }
    ImGui::SameLine();
    const bool rt_on = params.get_bool(ParamId::RenderRtEnabled);
    if (toolbar_toggle(tr("RT"), rt_on, btn_h, tr("Ray-traced shadows, hair occlusion and bounce (Ctrl+Shift+R)"))) {
        toggle_bool_param(params, undo, ParamId::RenderRtEnabled);
        actions.viewport.lighting_dirty = true;
    }
    ImGui::SameLine();
    const bool orb_on = params.get_bool(ParamId::AppOrbitOrbVisible);
    if (toolbar_toggle(icons::kSun, orb_on, btn_h, tr("Light Orbit Gizmo (Alt+L)"))) {
        toggle_bool_param(params, undo, ParamId::AppOrbitOrbVisible);
    }
    ImGui::SameLine();
    if (toolbar_toggle(tr("Reset Camera"), false, btn_h, tr("Re-frame the avatar (F)"))) {
        reset_camera(actions);
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar(2);

    const ImVec2 top_group_max = ImGui::GetItemRectMax();
    dl->ChannelsSetCurrent(0);
    const ImVec2 top_bg_max(top_group_max.x + pad, anchor.y + bar_h);
    dl->AddRectFilled(anchor, top_bg_max, t.col32_hud_bg, bar_h * 0.5f);
    dl->AddRect(anchor, top_bg_max, t.col32_hud_border, bar_h * 0.5f, 0, 1.0f);
    dl->ChannelsMerge();

    // 2) Secondary Transform Gizmo pill directly below the top toolbar (Q / W / E / R + Local/World)
    const ImVec2 giz_anchor(anchor.x, top_bg_max.y + 6.0f * scale);
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * scale, (btn_h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f * scale, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(giz_anchor.x + pad, giz_anchor.y + pad));
    ImGui::BeginGroup();

    if (toolbar_toggle("Q", gizmo_tool_ == GizmoTool::Select, btn_h, tr("Select Tool (Q)"))) {
        gizmo_tool_ = GizmoTool::Select;
    }
    ImGui::SameLine();
    if (toolbar_toggle("W", gizmo_tool_ == GizmoTool::Move, btn_h, tr("Translate Gizmo (W)"))) {
        gizmo_tool_ = GizmoTool::Move;
    }
    ImGui::SameLine();
    if (toolbar_toggle("E", gizmo_tool_ == GizmoTool::Rotate, btn_h, tr("Rotate Gizmo (E)"))) {
        gizmo_tool_ = GizmoTool::Rotate;
    }
    ImGui::SameLine();
    if (toolbar_toggle("R", gizmo_tool_ == GizmoTool::Scale, btn_h, tr("Scale Gizmo (R)"))) {
        gizmo_tool_ = GizmoTool::Scale;
    }
    ImGui::SameLine();
    if (toolbar_toggle(gizmo_local_ ? tr("Local") : tr("World"), gizmo_tool_ != GizmoTool::Select && !gizmo_local_,
                       btn_h, tr("Toggle Local / World gizmo axes (X)"))) {
        gizmo_local_ = !gizmo_local_;
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar(2);

    const ImVec2 giz_group_max = ImGui::GetItemRectMax();
    dl->ChannelsSetCurrent(0);
    const ImVec2 giz_bg_max(giz_group_max.x + pad, giz_anchor.y + bar_h);
    dl->AddRectFilled(giz_anchor, giz_bg_max, t.col32_hud_bg, bar_h * 0.5f);
    dl->AddRect(giz_anchor, giz_bg_max, t.col32_hud_border, bar_h * 0.5f, 0, 1.0f);
    dl->ChannelsMerge();

    return ImVec2(std::max(top_bg_max.x, giz_bg_max.x), giz_bg_max.y);
}

void ViewportPanel::draw_tracking_hud(ImVec2 top_right, float min_y, const DevShellTrackingView& tracking,
                                      const DevShellRenderView& render_view, bool tracking_paused,
                                      const UiFontSet& fonts, float scale, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    char line1[96]{};
    std::snprintf(line1, sizeof(line1), "%s · %.1f ms", tracking.active_ep_label,
                  static_cast<double>(tracking.inference_ms));
    char line2[96]{};
    std::snprintf(line2, sizeof(line2), "Face %.0f%% · GPU %.1f ms",
                  static_cast<double>(tracking.face_confidence * 100.0f),
                  static_cast<double>(render_view.total_gpu_ms));
    const float pad = 8.0f * scale;
    const float dot_w = 12.0f * scale;
    const float l1_w = dot_w + ImGui::CalcTextSize(line1).x;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float btn_row_w = ImGui::CalcTextSize(icons::kPause).x + ImGui::CalcTextSize("Calibrate").x +
                            st.FramePadding.x * 4.0f + st.ItemSpacing.x * 2.0f + ImGui::CalcTextSize(line2).x;
    const float w = std::max(l1_w, btn_row_w) + pad * 2.0f;
    const float line_h = ImGui::GetTextLineHeight();
    const float h = pad * 2.0f + line_h * 2.0f + st.ItemSpacing.y; // text line + SmallButton row
    const ImVec2 p0(top_right.x - w, std::max(top_right.y, min_y));
    const ImVec2 p1(top_right.x, p0.y + h);
    dl->AddRectFilled(p0, p1, t.col32_hud_bg, 8.0f * scale);
    dl->AddRect(p0, p1, t.col32_hud_border, 8.0f * scale, 0, 1.0f);

    const ImU32 ep_dot =
        tracking.ready ? (tracking.demotion_count > 0 ? t.col32_status_warn : t.col32_status_ok) : t.col32_status_error;
    dl->AddCircleFilled(ImVec2(p0.x + pad + 4.0f * scale, p0.y + pad + line_h * 0.5f), 3.5f * scale, ep_dot, 12);
    dl->AddText(ImVec2(p0.x + pad + dot_w, p0.y + pad), t.col32_text_primary, line1);

    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad + line_h + st.ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Button, t.bg_raised);
    if (ImGui::SmallButton(tracking_paused ? icons::kPlay : icons::kPause)) {
        actions.request_pause_tracking = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tracking_paused ? tr("Resume tracking (Ctrl+P)") : tr("Pause tracking (Ctrl+P)"));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Calibrate"))) {
        actions.viewport.request_calibrate_neutral = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s",
                          tr("Capture neutral rest pose + face zero-offset from the current frame (Ctrl+Shift+K)"));
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(t.text_secondary, "%s", line2);
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
}

void ViewportPanel::draw_composition_grid(ImVec2 p0, ImVec2 p1, float scale) const {
    const PrecisionDarkTokens& t = tokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const ImU32 guide = col32_with_alpha(t.col32_text_secondary, 0x40);
    const ImU32 safe = col32_with_alpha(t.col32_text_secondary, 0x2A);
    for (int i = 1; i <= 2; ++i) {
        const float fx = p0.x + w * static_cast<float>(i) / 3.0f;
        const float fy = p0.y + h * static_cast<float>(i) / 3.0f;
        dl->AddLine(ImVec2(fx, p0.y), ImVec2(fx, p1.y), guide, 1.0f);
        dl->AddLine(ImVec2(p0.x, fy), ImVec2(p1.x, fy), guide, 1.0f);
    }
    // Action-safe 93% and title-safe 90% (EBU R95 / SMPTE ST 2046-1)
    for (const float frac : {0.93f, 0.90f}) {
        const float mx = w * (1.0f - frac) * 0.5f;
        const float my = h * (1.0f - frac) * 0.5f;
        dl->AddRect(ImVec2(p0.x + mx, p0.y + my), ImVec2(p1.x - mx, p1.y - my), safe, 0.0f, 0, 1.0f);
    }
    const ImVec2 c(p0.x + w * 0.5f, p0.y + h * 0.5f);
    const float k = 6.0f * scale;
    dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), guide, 1.0f);
    dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), guide, 1.0f);
}

void ViewportPanel::handle_camera_input(bool hovered, bool active, float scale, StudioActions& actions) {
    const ImGuiIO& io = ImGui::GetIO();
    const float px = 1.0f / std::max(0.5f, scale);
    if (active) {
        const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (pan) {
            actions.viewport.pan_dx -= io.MouseDelta.x * kPanPerPx * px;
            actions.viewport.pan_dy += io.MouseDelta.y * kPanPerPx * px;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float dyaw = io.MouseDelta.x * kOrbitRadPerPx * px;
            const float dpitch = io.MouseDelta.y * kOrbitRadPerPx * px;
            actions.viewport.orbit_yaw_delta += dyaw;
            actions.viewport.orbit_pitch_delta += dpitch;
            cam_yaw_rad_ += dyaw;
            cam_pitch_rad_ = std::clamp(cam_pitch_rad_ + dpitch, -1.45f, 1.45f);
        }
    }
    if (hovered && std::fabs(io.MouseWheel) > 1e-4f) {
        const float dz = io.MouseWheel * kZoomPerWheel;
        actions.viewport.zoom_delta += dz;
        cam_log_zoom_ += dz;
    }
}

bool ViewportPanel::draw_transform_gizmo(const DevShellAvatarView& avatar, ParamStore& params, UndoStack& undo,
                                         const UiFontSet& fonts, ImVec2 canvas_p0, ImVec2 canvas_p1, float dpi_scale) {
    if (gizmo_tool_ == GizmoTool::Select) {
        if (!gizmo_dragging_) {
            gizmo_hover_handle_ = -1;
            gizmo_active_handle_ = -1;
            return false;
        }
        gizmo_dragging_ = false;
        gizmo_active_handle_ = -1;
        gizmo_hover_handle_ = -1;
        return false;
    }

    const PrecisionDarkTokens& t = tokens();
    const GizmoCameraBasis cam = GizmoCameraBasis::build(avatar, canvas_p0, canvas_p1);

    const float px = params.get_float(ParamId::AvatarPosX);
    const float py = params.get_float(ParamId::AvatarPosY);
    const float pz = params.get_float(ParamId::AvatarPosZ);
    const float rot_y_deg = params.get_float(ParamId::AvatarRotY);
    const float sc = std::clamp(params.get_float(ParamId::AvatarScale), 0.25f, 4.0f);

    const GizmoVec3 origin{px, py + avatar.gizmo_pivot_y * sc, pz};
    ImVec2 o_scr{};
    if (!cam.project(origin, o_scr)) {
        gizmo_hover_handle_ = -1;
        return gizmo_dragging_;
    }

    const float pw_origin = std::max(0.25f, dot3(cam.fwd, origin - cam.eye));
    const float desired_px = 68.0f * dpi_scale;
    const float world_len = std::clamp((desired_px * 2.0f / cam.h) * (pw_origin / cam.fy), 0.08f, 3.0f);

    const float ry = gizmo_local_ ? (rot_y_deg * kDegToRad) : 0.0f;
    const float cr = std::cos(ry);
    const float sr = std::sin(ry);
    const std::array<GizmoVec3, 3> axes{{
        GizmoVec3{cr, 0.0f, -sr}, // Local/World +X (matches shader yaw matrix)
        GizmoVec3{0.0f, 1.0f, 0.0f},
        GizmoVec3{sr, 0.0f, cr},  // Local/World +Z
    }};
    const std::array<ImU32, 3> axis_cols{{
        t.col32_gizmo_x,
        t.col32_gizmo_y,
        t.col32_gizmo_z,
    }};
    const std::array<const char*, 3> axis_labels{{"X", "Y", "Z"}};

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;
    const float hit_thresh = 11.0f * dpi_scale;
    const bool cursor_in_canvas =
        (mp.x >= canvas_p0.x && mp.x <= canvas_p1.x && mp.y >= canvas_p0.y && mp.y <= canvas_p1.y);

    // Hit-test when not currently dragging and foreground UI widgets (toolbar/orb) didn't claim hover.
    const ImGuiID cur_hov = (GImGui != nullptr) ? GImGui->HoveredId : 0;
    const bool ui_item_hovered = (cur_hov != 0 && cur_hov != static_cast<ImGuiID>(viewport_input_id_));
    if (!gizmo_dragging_) {
        gizmo_hover_handle_ = -1;
        if (cursor_in_canvas && !orb_dragging_ && !ui_item_hovered) {
            float best_d = hit_thresh;
            if (gizmo_tool_ == GizmoTool::Scale && std::hypot(mp.x - o_scr.x, mp.y - o_scr.y) <= 13.0f * dpi_scale) {
                gizmo_hover_handle_ = 3;
            } else if (gizmo_tool_ == GizmoTool::Rotate) {
                const float r_ring = world_len * 0.90f;
                constexpr int kRingSegs = 36;
                ImVec2 prev_s{};
                bool prev_ok = false;
                for (int i = 0; i <= kRingSegs; ++i) {
                    const float a = (static_cast<float>(i) / static_cast<float>(kRingSegs)) * (2.0f * kPi);
                    const GizmoVec3 p = origin + GizmoVec3{std::sin(a) * r_ring, 0.0f, std::cos(a) * r_ring};
                    ImVec2 cur_s{};
                    const bool cur_ok = cam.project(p, cur_s);
                    if (i > 0 && prev_ok && cur_ok) {
                        const float d = dist_point_segment(mp, prev_s, cur_s);
                        if (d < best_d) {
                            best_d = d;
                            gizmo_hover_handle_ = 1; // Y-rotation ring
                        }
                    }
                    prev_s = cur_s;
                    prev_ok = cur_ok;
                }
            } else {
                const float stem_min = world_len * 0.14f;
                const float stem_max = (gizmo_tool_ == GizmoTool::Scale) ? (world_len * 0.92f) : world_len;
                for (int i = 0; i < 3; ++i) {
                    ImVec2 p_a{};
                    ImVec2 p_b{};
                    if (cam.project(origin + axes[i] * stem_min, p_a) &&
                        cam.project(origin + axes[i] * stem_max, p_b)) {
                        const float d = dist_point_segment(mp, p_a, p_b);
                        if (d < best_d) {
                            best_d = d;
                            gizmo_hover_handle_ = i;
                        }
                    }
                }
            }
        }
    }

    // Reserve a tight interactive item over the hovered handle (or active drag) so ViewportInput
    // does not start a camera orbit when clicking a gizmo handle.
    if (gizmo_hover_handle_ >= 0 || gizmo_dragging_) {
        const float box_r = 16.0f * dpi_scale;
        const ImVec2 b_min(std::clamp(mp.x - box_r, canvas_p0.x, canvas_p1.x - 4.0f),
                           std::clamp(mp.y - box_r, canvas_p0.y, canvas_p1.y - 4.0f));
        const ImVec2 b_max(std::clamp(mp.x + box_r, b_min.x + 4.0f, canvas_p1.x),
                           std::clamp(mp.y + box_r, b_min.y + 4.0f, canvas_p1.y));
        ImGui::SetCursorScreenPos(b_min);
        ImGui::InvisibleButton("##TransformGizmo", ImVec2(b_max.x - b_min.x, b_max.y - b_min.y),
                               ImGuiButtonFlags_MouseButtonLeft);

        if (!gizmo_dragging_ && (ImGui::IsItemActivated() ||
                                 (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
            gizmo_dragging_ = true;
            gizmo_active_handle_ = gizmo_hover_handle_;
            gizmo_drag_tool_ = gizmo_tool_;
            gizmo_drag_origin_[0] = origin.x;
            gizmo_drag_origin_[1] = origin.y;
            gizmo_drag_origin_[2] = origin.z;
            gizmo_drag_start_vals_[0] = px;
            gizmo_drag_start_vals_[1] = py;
            gizmo_drag_start_vals_[2] = pz;
            gizmo_drag_start_vals_[3] = rot_y_deg;
            gizmo_drag_start_vals_[4] = sc;
            format_xform_payload(gizmo_before_payload_.data(), gizmo_before_payload_.size(), px, py, pz, rot_y_deg, sc);

            GizmoVec3 ray_o{};
            GizmoVec3 ray_d{};
            cam.screen_ray(mp, ray_o, ray_d);
            if (gizmo_drag_tool_ == GizmoTool::Rotate) {
                GizmoVec3 hit{};
                if (intersect_xz_plane(origin.y, ray_o, ray_d, hit)) {
                    gizmo_drag_start_angle_ = std::atan2(hit.x - origin.x, hit.z - origin.z);
                } else {
                    gizmo_drag_start_angle_ = mp.x;
                }
            } else {
                const GizmoVec3 drag_axis =
                    (gizmo_active_handle_ >= 0 && gizmo_active_handle_ < 3) ? axes[gizmo_active_handle_] : cam.right;
                gizmo_drag_axis_[0] = drag_axis.x;
                gizmo_drag_axis_[1] = drag_axis.y;
                gizmo_drag_axis_[2] = drag_axis.z;
                float t_hit = 0.0f;
                if (!closest_t_on_axis(origin, drag_axis, ray_o, ray_d, t_hit)) {
                    t_hit = 0.0f;
                }
                gizmo_drag_start_t_ = t_hit;
            }
        }
    }

    if (gizmo_dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const GizmoVec3 start_origin{gizmo_drag_origin_[0], gizmo_drag_origin_[1], gizmo_drag_origin_[2]};
            GizmoVec3 ray_o{};
            GizmoVec3 ray_d{};
            cam.screen_ray(mp, ray_o, ray_d);

            if (gizmo_drag_tool_ == GizmoTool::Move) {
                const GizmoVec3 axis_dir{gizmo_drag_axis_[0], gizmo_drag_axis_[1], gizmo_drag_axis_[2]};
                float t_cur = gizmo_drag_start_t_;
                float dt = 0.0f;
                if (closest_t_on_axis(start_origin, axis_dir, ray_o, ray_d, t_cur)) {
                    dt = t_cur - gizmo_drag_start_t_;
                }
                (void)params.set(ParamId::AvatarPosX, static_cast<double>(gizmo_drag_start_vals_[0] + axis_dir.x * dt));
                (void)params.set(ParamId::AvatarPosY, static_cast<double>(gizmo_drag_start_vals_[1] + axis_dir.y * dt));
                (void)params.set(ParamId::AvatarPosZ, static_cast<double>(gizmo_drag_start_vals_[2] + axis_dir.z * dt));
            } else if (gizmo_drag_tool_ == GizmoTool::Rotate) {
                float delta_deg = 0.0f;
                GizmoVec3 hit{};
                if (intersect_xz_plane(start_origin.y, ray_o, ray_d, hit)) {
                    const float cur_a = std::atan2(hit.x - start_origin.x, hit.z - start_origin.z);
                    float d_rad = cur_a - gizmo_drag_start_angle_;
                    while (d_rad > kPi) {
                        d_rad -= 2.0f * kPi;
                    }
                    while (d_rad < -kPi) {
                        d_rad += 2.0f * kPi;
                    }
                    delta_deg = d_rad * kRadToDeg;
                } else {
                    delta_deg = (mp.x - o_scr.x) * 0.35f;
                }
                float next_deg = gizmo_drag_start_vals_[3] + delta_deg;
                while (next_deg > 180.0f) {
                    next_deg -= 360.0f;
                }
                while (next_deg < -180.0f) {
                    next_deg += 360.0f;
                }
                (void)params.set(ParamId::AvatarRotY, static_cast<double>(next_deg));
            } else if (gizmo_drag_tool_ == GizmoTool::Scale) {
                float ratio = 1.0f;
                if (gizmo_active_handle_ == 3) {
                    ratio = 1.0f + ((mp.x - o_scr.x) - (mp.y - o_scr.y)) / std::max(80.0f, 140.0f * dpi_scale);
                } else {
                    const GizmoVec3 axis_dir{gizmo_drag_axis_[0], gizmo_drag_axis_[1], gizmo_drag_axis_[2]};
                    float t_cur = gizmo_drag_start_t_;
                    if (closest_t_on_axis(start_origin, axis_dir, ray_o, ray_d, t_cur)) {
                        const float dt = t_cur - gizmo_drag_start_t_;
                        ratio = 1.0f + dt / std::max(0.15f, world_len);
                    }
                }
                const float next_sc = std::clamp(gizmo_drag_start_vals_[4] * std::clamp(ratio, 0.1f, 4.0f), 0.25f, 4.0f);
                (void)params.set(ParamId::AvatarScale, static_cast<double>(next_sc));
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            // Release: push one atomic undoable command onto UndoStack if any transform changed.
            const float end_px = params.get_float(ParamId::AvatarPosX);
            const float end_py = params.get_float(ParamId::AvatarPosY);
            const float end_pz = params.get_float(ParamId::AvatarPosZ);
            const float end_ry = params.get_float(ParamId::AvatarRotY);
            const float end_sc = params.get_float(ParamId::AvatarScale);
            const bool changed = std::abs(end_px - gizmo_drag_start_vals_[0]) > 1e-4f ||
                                 std::abs(end_py - gizmo_drag_start_vals_[1]) > 1e-4f ||
                                 std::abs(end_pz - gizmo_drag_start_vals_[2]) > 1e-4f ||
                                 std::abs(end_ry - gizmo_drag_start_vals_[3]) > 1e-3f ||
                                 std::abs(end_sc - gizmo_drag_start_vals_[4]) > 1e-4f;
            if (changed) {
                std::array<char, 128> after_buf{};
                format_xform_payload(after_buf.data(), after_buf.size(), end_px, end_py, end_pz, end_ry, end_sc);
                undo.push_custom_edit("Transform Avatar", gizmo_before_payload_.data(), after_buf.data());
            }
            gizmo_dragging_ = false;
            gizmo_active_handle_ = -1;
        }
    }

    // Visual rendering on top of viewport image
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }

    const int highlight_idx = gizmo_dragging_ ? gizmo_active_handle_ : gizmo_hover_handle_;
    if (gizmo_tool_ == GizmoTool::Rotate) {
        const float r_ring = world_len * 0.90f;
        const ImU32 ring_col = (highlight_idx == 1) ? t.col32_gizmo_hover : t.col32_gizmo_y;
        const float ring_w = ((highlight_idx == 1) ? 3.5f : 2.3f) * dpi_scale;
        constexpr int kRingSegs = 40;
        ImVec2 prev_s{};
        bool prev_ok = false;
        for (int i = 0; i <= kRingSegs; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(kRingSegs)) * (2.0f * kPi);
            const GizmoVec3 p = origin + GizmoVec3{std::sin(a) * r_ring, 0.0f, std::cos(a) * r_ring};
            ImVec2 cur_s{};
            const bool cur_ok = cam.project(p, cur_s);
            if (i > 0 && prev_ok && cur_ok) {
                dl->AddLine(prev_s, cur_s, ring_col, ring_w);
            }
            prev_s = cur_s;
            prev_ok = cur_ok;
        }
        // Subtle orientation spokes (+Z and +X) inside the ring so yaw reads at a glance
        for (int i = 0; i < 3; i += 2) {
            ImVec2 tip{};
            if (cam.project(origin + axes[i] * (r_ring * 0.92f), tip)) {
                dl->AddLine(o_scr, tip, col32_with_alpha(axis_cols[i], 0xB8), 1.6f * dpi_scale);
                dl->AddCircleFilled(tip, 3.5f * dpi_scale, axis_cols[i], 10);
            }
        }
        dl->AddCircleFilled(o_scr, 4.5f * dpi_scale, t.col32_gizmo_center, 12);
    } else {
        const float stem_min = world_len * 0.14f;
        const float stem_max = (gizmo_tool_ == GizmoTool::Scale) ? (world_len * 0.90f) : world_len;
        for (int i = 0; i < 3; ++i) {
            ImVec2 p_a{};
            ImVec2 p_b{};
            if (!cam.project(origin + axes[i] * stem_min, p_a) || !cam.project(origin + axes[i] * stem_max, p_b)) {
                continue;
            }
            const bool hot = (highlight_idx == i);
            const ImU32 col = hot ? t.col32_gizmo_hover : axis_cols[i];
            const float thick = (hot ? 3.8f : 2.5f) * dpi_scale;
            dl->AddLine(o_scr, p_b, col, thick);

            const ImVec2 dir2(p_b.x - o_scr.x, p_b.y - o_scr.y);
            const float l2 = std::hypot(dir2.x, dir2.y);
            const ImVec2 u2 = (l2 > 1e-3f) ? ImVec2(dir2.x / l2, dir2.y / l2) : ImVec2(1.0f, 0.0f);
            const ImVec2 n2(-u2.y, u2.x);

            if (gizmo_tool_ == GizmoTool::Move) {
                const float ah = 10.0f * dpi_scale;
                const float aw = 4.5f * dpi_scale;
                const ImVec2 tip(p_b.x + u2.x * ah, p_b.y + u2.y * ah);
                const ImVec2 b1(p_b.x + n2.x * aw, p_b.y + n2.y * aw);
                const ImVec2 b2(p_b.x - n2.x * aw, p_b.y - n2.y * aw);
                dl->AddTriangleFilled(tip, b1, b2, col);
                dl->AddText(ImVec2(tip.x + u2.x * 4.0f * dpi_scale - 3.0f, tip.y + u2.y * 4.0f * dpi_scale - 6.0f), col,
                            axis_labels[i]);
            } else {
                const float hs = 4.2f * dpi_scale;
                dl->AddRectFilled(ImVec2(p_b.x - hs, p_b.y - hs), ImVec2(p_b.x + hs, p_b.y + hs), col, 1.0f);
                dl->AddText(ImVec2(p_b.x + u2.x * 9.0f * dpi_scale - 3.0f, p_b.y + u2.y * 9.0f * dpi_scale - 6.0f), col,
                            axis_labels[i]);
            }
        }
        const ImU32 ctr_col =
            (gizmo_tool_ == GizmoTool::Scale && highlight_idx == 3) ? t.col32_gizmo_hover : t.col32_gizmo_center;
        if (gizmo_tool_ == GizmoTool::Scale) {
            const float ds = 5.5f * dpi_scale;
            dl->AddQuadFilled(ImVec2(o_scr.x, o_scr.y - ds), ImVec2(o_scr.x + ds, o_scr.y),
                              ImVec2(o_scr.x, o_scr.y + ds), ImVec2(o_scr.x - ds, o_scr.y), ctr_col);
        } else {
            dl->AddCircleFilled(o_scr, 4.5f * dpi_scale, ctr_col, 12);
        }
    }

    if (gizmo_dragging_) {
        char readout[64]{};
        if (gizmo_drag_tool_ == GizmoTool::Move) {
            std::snprintf(readout, sizeof(readout), "Pos %.2f, %.2f, %.2f", static_cast<double>(px),
                          static_cast<double>(py), static_cast<double>(pz));
        } else if (gizmo_drag_tool_ == GizmoTool::Rotate) {
            std::snprintf(readout, sizeof(readout), "Rot Y %.1f deg", static_cast<double>(rot_y_deg));
        } else {
            std::snprintf(readout, sizeof(readout), "Scale %.2fx", static_cast<double>(sc));
        }
        const ImVec2 txt_sz = ImGui::CalcTextSize(readout);
        const ImVec2 pill_p0(o_scr.x + 14.0f * dpi_scale, o_scr.y + 14.0f * dpi_scale);
        const ImVec2 pill_p1(pill_p0.x + txt_sz.x + 12.0f * dpi_scale, pill_p0.y + txt_sz.y + 6.0f * dpi_scale);
        dl->AddRectFilled(pill_p0, pill_p1, t.col32_hud_bg, 4.0f * dpi_scale);
        dl->AddRect(pill_p0, pill_p1, t.col32_gizmo_hover, 4.0f * dpi_scale, 0, 1.0f);
        dl->AddText(ImVec2(pill_p0.x + 6.0f * dpi_scale, pill_p0.y + 3.0f * dpi_scale), t.col32_text_primary, readout);
    }

    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
    return gizmo_dragging_ || (gizmo_hover_handle_ >= 0);
}

void ViewportPanel::draw(StudioMode mode, const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                         const DevShellRenderView& render_view, const DevShellInputEcho& input_echo,
                         bool tracking_paused, ParamStore& params, UndoStack& undo, const UiFontSet& fonts,
                         float dpi_scale, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    (void)input_echo;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin(tr("Viewport"), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        viewport_focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // BUG-03: every overlay is anchored to the client area (below the tab bar), never the window origin.
        const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float vp_w = std::max(64.0f, avail.x);
        const float vp_h = std::max(64.0f, avail.y);
        const ImVec2 canvas_p1(canvas_p0.x + vp_w, canvas_p0.y + vp_h);
        actions.viewport.desired_vp_w = static_cast<uint32_t>(vp_w);
        actions.viewport.desired_vp_h = static_cast<uint32_t>(vp_h);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(canvas_p0, canvas_p1, true);

        // 1) Backdrop + rendered avatar (draw-list only, so overlays submitted later stay on top)
        if (params.get_bool(ParamId::AppCheckerboardBg)) {
            const float cell = std::round(16.0f * scale);
            dl->AddRectFilled(canvas_p0, canvas_p1, t.col32_checker_a);
            for (float y = canvas_p0.y; y < canvas_p1.y; y += cell) {
                const int row = static_cast<int>((y - canvas_p0.y) / cell);
                for (float x = canvas_p0.x + ((row & 1) != 0 ? cell : 0.0f); x < canvas_p1.x; x += cell * 2.0f) {
                    dl->AddRectFilled(ImVec2(x, y),
                                      ImVec2(std::min(canvas_p1.x, x + cell), std::min(canvas_p1.y, y + cell)),
                                      t.col32_checker_b);
                }
            }
        } else {
            dl->AddRectFilled(canvas_p0, canvas_p1, t.col32_bg_canvas);
        }
        if (avatar.viewport_texture != VK_NULL_HANDLE) {
            dl->AddImage(reinterpret_cast<ImTextureID>(avatar.viewport_texture), canvas_p0, canvas_p1);
        }
        if (show_grid_) {
            draw_composition_grid(canvas_p0, canvas_p1, scale);
        }

        // 2) Canvas-wide camera input surface FIRST with AllowOverlap; toolbar, Light Orbit Orb,
        // and 3D Transform Gizmo submitted after it cleanly steal hover & clicks on top.
        ImGui::SetCursorScreenPos(canvas_p0);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##ViewportInput", ImVec2(vp_w, vp_h),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const ImGuiID vp_input_id = ImGui::GetItemID();
        viewport_input_id_ = static_cast<uint32_t>(vp_input_id);

        // 3) Floating overlays & interactive 3D gizmos
        const float margin_x = 12.0f * scale;
        const float margin_y = 10.0f * scale;
        const ImVec2 toolbar_max =
            draw_toolbar(ImVec2(canvas_p0.x + margin_x, canvas_p0.y + margin_y), mode, avatar, params, undo, scale, actions);
        const float toolbar_bottom = toolbar_max.y + 3.0f * scale;
        const float toolbar_right = toolbar_max.x + 3.0f * scale;

        // Tracking HUD top-right; drops below the toolbar when the viewport is too narrow for both.
        const ImVec2 hud_anchor(canvas_p1.x - margin_x, canvas_p0.y + margin_y);
        const float hud_min_y = (canvas_p1.x - margin_x - 300.0f * scale < toolbar_right) ? toolbar_bottom + margin_y
                                                                                          : canvas_p0.y + margin_y;
        draw_tracking_hud(hud_anchor, hud_min_y, tracking, render_view, tracking_paused, fonts, scale, actions);

        if (params.get_bool(ParamId::AppSkeletonHeatmap) || mode == StudioMode::Rigging) {
            draw_confidence_heatmap_legend(tracking, fonts, scale,
                                           ImVec2(canvas_p0.x + margin_x, canvas_p1.y - 78.0f * scale));
        }

        const float orb_px = std::round(t.orbit_orb_px * scale);
        const ImVec2 orb_pos(canvas_p1.x - orb_px - 18.0f * scale, canvas_p1.y - orb_px - 40.0f * scale);
        if (params.get_bool(ParamId::AppOrbitOrbVisible)) {
            draw_light_orbit_gizmo(params, undo, fonts, scale, orb_pos, actions);
        }

        const bool gizmo_claimed = draw_transform_gizmo(avatar, params, undo, fonts, canvas_p0, canvas_p1, scale);

        const bool cam_hovered =
            (GImGui != nullptr) && (GImGui->HoveredId == vp_input_id) && !gizmo_claimed && !gizmo_dragging_;
        const bool cam_active = (GImGui != nullptr) && (GImGui->ActiveId == vp_input_id) && !orb_dragging_ &&
                                !gizmo_claimed && !gizmo_dragging_;
        handle_camera_input(cam_hovered, cam_active, scale, actions);
        if (cam_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }

        dl->PopClipRect();
    } else {
        viewport_focused_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void ViewportPanel::draw_light_orbit_gizmo(ParamStore& params, UndoStack& undo, const UiFontSet& fonts, float dpi_scale,
                                           ImVec2 orb_pos, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float orb_px = std::round(t.orbit_orb_px * dpi_scale);
    const float radius = orb_px * 0.5f;
    const ImVec2 center(orb_pos.x + radius, orb_pos.y + radius);
    const float r_handle = radius * 0.86f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float key_az = params.get_float(ParamId::RenderKeyAzimuthDeg);
    const float key_el = params.get_float(ParamId::RenderKeyElevationDeg);
    struct Handle {
        float az;
        float el;
        ImU32 col;
        const char* name;
    };
    const std::array<Handle, 3> handles = {{
        {key_az, key_el, t.col32_orb_key, "Key"},
        {wrap_degrees(key_az + LightRigOffsets::kFillAzDeg),
         std::clamp(key_el + LightRigOffsets::kFillElDeg, -85.0f, 85.0f), t.col32_orb_fill_light, "Fill"},
        {wrap_degrees(key_az + LightRigOffsets::kRimAzDeg),
         std::clamp(key_el + LightRigOffsets::kRimElDeg, -85.0f, 85.0f), t.col32_orb_rim_light, "Rim"},
    }};

    // --- Dark vector sphere: shaded body, latitude/longitude rings, crosshair, rim + specular ---
    const int vtx0 = dl->VtxBuffer.Size;
    dl->AddCircleFilled(center, radius, t.col32_orb_fill, 48);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(
        dl, vtx0, dl->VtxBuffer.Size, ImVec2(center.x - radius * 0.6f, center.y - radius * 0.7f),
        ImVec2(center.x + radius * 0.7f, center.y + radius), t.col32_orb_fill, t.col32_orb_fill_edge);
    for (const float lat : {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f}) {
        const float la = lat * kDegToRad;
        const float rx = radius * std::cos(la);
        dl->AddEllipse(ImVec2(center.x, center.y - radius * std::sin(la)), ImVec2(rx, rx * 0.24f), t.col32_orb_ring,
                       0.0f, 40, lat == 0.0f ? 1.2f : 1.0f);
    }
    for (const float lon : {30.0f, 60.0f}) {
        dl->AddEllipse(center, ImVec2(radius * std::sin(lon * kDegToRad), radius), t.col32_orb_ring, 0.0f, 40, 1.0f);
    }
    dl->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), t.col32_orb_ring, 1.0f);
    dl->AddCircle(center, radius, t.col32_orb_rim, 48, 1.5f * dpi_scale);
    dl->AddEllipseFilled(ImVec2(center.x - radius * 0.34f, center.y - radius * 0.42f),
                         ImVec2(radius * 0.26f, radius * 0.13f), t.col32_orb_specular, -0.6f, 24);

    // --- Interaction: pick the nearest handle on press, rotate the whole rig while dragging ---
    ImGui::SetCursorScreenPos(orb_pos);
    ImGui::InvisibleButton("##LightOrbitGizmo", ImVec2(orb_px, orb_px));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 mp = ImGui::GetIO().MousePos;

    if (ImGui::IsItemActivated()) {
        orb_dragging_ = true;
        orb_drag_handle_ = 0;
        float best = 1e9f;
        for (int i = 0; i < 3; ++i) {
            const OrbProjection pr =
                project_orbit(center, r_handle, handles[static_cast<size_t>(i)].az, handles[static_cast<size_t>(i)].el);
            const float d = (pr.pt.x - mp.x) * (pr.pt.x - mp.x) + (pr.pt.y - mp.y) * (pr.pt.y - mp.y);
            if (d < best && d <= (14.0f * dpi_scale) * (14.0f * dpi_scale)) {
                best = d;
                orb_drag_handle_ = i;
            }
        }
        orb_drag_back_hemisphere_ = project_orbit(center, r_handle, handles[static_cast<size_t>(orb_drag_handle_)].az,
                                                  handles[static_cast<size_t>(orb_drag_handle_)].el)
                                        .z < 0.0f;
        orb_drag_start_az_ = params.get(ParamId::RenderKeyAzimuthDeg);
        orb_drag_start_el_ = params.get(ParamId::RenderKeyElevationDeg);
    }

    if (active && orb_dragging_) {
        // Inverse-project the cursor onto the sphere, keeping the hemisphere the handle started in.
        const float nx = std::clamp((mp.x - center.x) / r_handle, -1.0f, 1.0f);
        const float ny = std::clamp((center.y - mp.y) / r_handle, -0.996f, 0.996f);
        const float el = std::asin(ny);
        const float sin_az = std::clamp(nx / std::max(1e-3f, std::cos(el)), -1.0f, 1.0f);
        float az = std::asin(sin_az) * kRadToDeg;
        if (orb_drag_back_hemisphere_) {
            az = (az >= 0.0f ? 180.0f : -180.0f) - az;
        }
        float el_deg = el * kRadToDeg;
        if (orb_drag_handle_ == 1) {
            az -= LightRigOffsets::kFillAzDeg;
            el_deg -= LightRigOffsets::kFillElDeg;
        } else if (orb_drag_handle_ == 2) {
            az -= LightRigOffsets::kRimAzDeg;
            el_deg -= LightRigOffsets::kRimElDeg;
        }
        const bool ch_a = params.set(ParamId::RenderKeyAzimuthDeg, static_cast<double>(wrap_degrees(az)));
        const bool ch_e = params.set(ParamId::RenderKeyElevationDeg, static_cast<double>(el_deg));
        if (ch_a || ch_e) {
            actions.viewport.lighting_dirty = true;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (orb_dragging_ && !active) {
        orb_dragging_ = false;
        const double end_az = params.get(ParamId::RenderKeyAzimuthDeg);
        const double end_el = params.get(ParamId::RenderKeyElevationDeg);
        if (end_az != orb_drag_start_az_) {
            undo.push_param(ParamId::RenderKeyAzimuthDeg, orb_drag_start_az_, end_az);
        }
        if (end_el != orb_drag_start_el_) {
            undo.push_param(ParamId::RenderKeyElevationDeg, orb_drag_start_el_, end_el);
        }
    }

    // Keyboard nudge while hovered or focused (1 deg, 10 deg with Shift §12.6)
    if (hovered || ImGui::IsItemFocused()) {
        const float nudge = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
        const float dax = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? nudge : 0.0f) -
                          (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? nudge : 0.0f);
        const float del = (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? nudge : 0.0f) -
                          (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? nudge : 0.0f);
        if (dax != 0.0f) {
            const double old_v = params.get(ParamId::RenderKeyAzimuthDeg);
            if (params.set(ParamId::RenderKeyAzimuthDeg,
                           static_cast<double>(wrap_degrees(static_cast<float>(old_v) + dax)))) {
                undo.push_param(ParamId::RenderKeyAzimuthDeg, old_v, params.get(ParamId::RenderKeyAzimuthDeg));
                actions.viewport.lighting_dirty = true;
            }
        }
        if (del != 0.0f) {
            const double old_v = params.get(ParamId::RenderKeyElevationDeg);
            if (params.set(ParamId::RenderKeyElevationDeg, old_v + del)) {
                undo.push_param(ParamId::RenderKeyElevationDeg, old_v, params.get(ParamId::RenderKeyElevationDeg));
                actions.viewport.lighting_dirty = true;
            }
        }
        if (hovered && !active) {
            ImGui::SetTooltip("%s", tr("Drag Key / Fill / Rim to rotate the light rig · arrows nudge (Shift = 10°)"));
        }
    }

    // --- Handles: glowing front-facing dots, dimmed hollow dots behind the sphere ---
    const float hr = 5.0f * dpi_scale;
    for (size_t i = 0; i < handles.size(); ++i) {
        const Handle& h = handles[i];
        const OrbProjection pr = project_orbit(center, r_handle, h.az, h.el);
        const bool front = pr.z >= 0.0f;
        const bool hot = orb_dragging_ && orb_drag_handle_ == static_cast<int>(i);
        dl->AddLine(center, pr.pt, col32_with_alpha(h.col, front ? 0x55 : 0x26), 1.0f);
        if (front) {
            dl->AddCircleFilled(pr.pt, hr * 2.2f, col32_with_alpha(h.col, hot ? 0x55 : 0x30), 20);
            dl->AddCircleFilled(pr.pt, hr, h.col, 16);
            dl->AddCircle(pr.pt, hr + 1.0f, col32_with_alpha(t.col32_text_primary, 0xB0), 16, 1.0f);
        } else {
            dl->AddCircleFilled(pr.pt, hr, t.col32_orb_fill_edge, 16);
            dl->AddCircle(pr.pt, hr, col32_with_alpha(h.col, 0x99), 16, 1.5f);
        }
    }
    dl->AddCircleFilled(center, 1.5f * dpi_scale, t.col32_orb_ring, 8);

    // --- HUD readout pill under the sphere ---
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    char readout[64]{};
    std::snprintf(readout, sizeof(readout), "Key az %.0f\xC2\xB0 el %.0f\xC2\xB0 \xC2\xB7 %.2f EV",
                  static_cast<double>(key_az), static_cast<double>(key_el),
                  static_cast<double>(params.get_float(ParamId::RenderKeyIntensity)));
    const ImVec2 tsz = ImGui::CalcTextSize(readout);
    const float pill_pad = 8.0f * dpi_scale;
    const ImVec2 pill_min(center.x - tsz.x * 0.5f - pill_pad, orb_pos.y + orb_px + 6.0f * dpi_scale);
    const ImVec2 pill_max(center.x + tsz.x * 0.5f + pill_pad, pill_min.y + tsz.y + 6.0f * dpi_scale);
    dl->AddRectFilled(pill_min, pill_max, t.col32_hud_bg, (pill_max.y - pill_min.y) * 0.5f);
    dl->AddRect(pill_min, pill_max, t.col32_hud_border, (pill_max.y - pill_min.y) * 0.5f, 0, 1.0f);
    dl->AddText(ImVec2(pill_min.x + pill_pad, pill_min.y + 3.0f * dpi_scale), t.col32_text_primary, readout);
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
}

void ViewportPanel::draw_confidence_heatmap_legend(const DevShellTrackingView& tracking, const UiFontSet& fonts,
                                                   float dpi_scale, ImVec2 pos) {
    const PrecisionDarkTokens& t = tokens();
    if (fonts.caption != nullptr) {
        ImGui::PushFont(fonts.caption);
    }
    char proc_txt[64]{};
    std::snprintf(proc_txt, sizeof(proc_txt), "Procedural w=0  (L:%.0f%% R:%.0f%%)",
                  static_cast<double>(tracking.left_hand_confidence * 100.0f),
                  static_cast<double>(tracking.right_hand_confidence * 100.0f));
    const float line_h = ImGui::GetTextLineHeight();
    const float w = 52.0f * dpi_scale + ImGui::CalcTextSize(proc_txt).x;
    const float h = line_h * 3.0f + 22.0f * dpi_scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), t.col32_hud_bg, 6.0f * dpi_scale);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), t.col32_hud_border, 6.0f * dpi_scale, 0, 1.0f);

    const float x0 = pos.x + 8.0f * dpi_scale;
    const float x1 = pos.x + 40.0f * dpi_scale;
    const float step = line_h + 4.0f * dpi_scale;
    const float y0 = pos.y + 7.0f * dpi_scale + line_h * 0.5f;
    draw_styled_legend_segment(dl, ImVec2(x0, y0), ImVec2(x1, y0), t.col32_status_ok, 0, 2.0f);
    dl->AddText(ImVec2(x1 + 6.0f * dpi_scale, y0 - line_h * 0.5f), t.col32_text_primary, "Tracked · w = 1.0");
    const float y1 = y0 + step;
    draw_styled_legend_segment(dl, ImVec2(x0, y1), ImVec2(x1, y1), t.col32_status_warn, 1, 2.0f);
    dl->AddText(ImVec2(x1 + 6.0f * dpi_scale, y1 - line_h * 0.5f), t.col32_text_secondary, "Interpolating · 0 < w < 1");
    const float y2 = y1 + step;
    draw_styled_legend_segment(dl, ImVec2(x0, y2), ImVec2(x1, y2), t.col32_accent, 2, 2.0f);
    dl->AddText(ImVec2(x1 + 6.0f * dpi_scale, y2 - line_h * 0.5f), t.col32_text_secondary, proc_txt);
    if (fonts.caption != nullptr) {
        ImGui::PopFont();
    }
}

} // namespace aether::ui
