#include "ui/panels/lighting/lighting_stage_panels.hpp"

#include "renderer/frame_graph.hpp"
#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

std::array<float, 3> compute_pivot_ws(const DevShellAvatarView& avatar) noexcept {
    if (avatar.has_avatar && avatar.bone_head_y > 0.25f) {
        return {0.0f, avatar.bone_head_y, 0.0f};
    }
    return {0.0f, 1.32f, 0.0f};
}

ImVec2 project_world_to_canvas(const std::array<float, 16>& vp, const std::array<float, 3>& p, ImVec2 c0,
                               ImVec2 csz, bool& out_ok) noexcept {
    const float cx = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
    const float cy = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
    const float cw = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    if (cw <= 0.01f) {
        out_ok = false;
        return c0;
    }
    const float inv_w = 1.0f / cw;
    const float nx = cx * inv_w;
    const float ny = cy * inv_w;
    out_ok = true;
    return ImVec2(c0.x + (nx * 0.5f + 0.5f) * csz.x, c0.y + (0.5f - ny * 0.5f) * csz.y);
}

void draw_camera_pan_orbit_zoom(renderer::PreviewCamera& cam, bool hovered, bool active, bool allow_orbit,
                                bool& out_dragging) noexcept {
    out_dragging = false;
    if (!hovered && !active) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool pan_drag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                          (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f));
    const bool orb_drag =
        allow_orbit && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                        (io.KeyAlt && !io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)));

    if (orb_drag && active) {
        cam.yaw_rad -= io.MouseDelta.x * 0.0075f;
        cam.pitch_rad = std::clamp(cam.pitch_rad + io.MouseDelta.y * 0.0065f, -1.48f, 1.48f);
        out_dragging = true;
    } else if (pan_drag && active) {
        const float cp = std::cos(cam.pitch_rad);
        const float sp = std::sin(cam.pitch_rad);
        const float cy = std::cos(cam.yaw_rad);
        const float sy = std::sin(cam.yaw_rad);
        const float fx = -cp * sy;
        const float fy = -sp;
        const float fz = -cp * cy;
        float ux = 0.0f, uy = 1.0f, uz = 0.0f;
        if (std::fabs(fy) > 0.985f) {
            const float sign_y = (fy < 0.0f) ? -1.0f : 1.0f;
            ux = sign_y * sy;
            uy = 0.0f;
            uz = sign_y * cy;
        }
        float rx = fy * uz - fz * uy;
        float ry = fz * ux - fx * uz;
        float rz = fx * uy - fy * ux;
        const float rl = std::sqrt(std::max(1e-10f, rx * rx + ry * ry + rz * rz));
        rx /= rl;
        ry /= rl;
        rz /= rl;
        const float uox = ry * fz - rz * fy;
        const float uoy = rz * fx - rx * fz;
        const float uoz = rx * fy - ry * fx;

        const float scale_m = cam.is_orthographic ? (cam.ortho_height * 0.0018f) : (cam.distance * 0.0018f);
        cam.target_x += (-io.MouseDelta.x * rx + io.MouseDelta.y * uox) * scale_m;
        cam.target_y += (-io.MouseDelta.x * ry + io.MouseDelta.y * uoy) * scale_m;
        cam.target_z += (-io.MouseDelta.x * rz + io.MouseDelta.y * uoz) * scale_m;
        out_dragging = true;
    }

    if (hovered && std::fabs(io.MouseWheel) > 1e-4f) {
        const float factor = std::exp(-io.MouseWheel * 0.12f);
        if (cam.is_orthographic) {
            cam.ortho_height = std::clamp(cam.ortho_height * factor, 0.35f, 12.0f);
        } else {
            cam.distance = std::clamp(cam.distance * factor, 0.35f, 16.0f);
        }
        out_dragging = true;
    }
}

bool contains_icase(const char* hay, const char* needle) noexcept {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    if (hay == nullptr) {
        return false;
    }
    const size_t nlen = std::strlen(needle);
    for (const char* p = hay; *p != '\0'; ++p) {
        size_t k = 0;
        while (k < nlen && p[k] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[k])) == std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == nlen) {
            return true;
        }
    }
    return false;
}

const char* channel_short_label(uint32_t mask) noexcept {
    if (mask == AETHER_LIGHT_CHANNEL_ALL) {
        return "ALL";
    }
    if ((mask & AETHER_LIGHT_CHANNEL_FACE) != 0u && (mask & AETHER_LIGHT_CHANNEL_HAIR) == 0u) {
        return "FACE";
    }
    if ((mask & AETHER_LIGHT_CHANNEL_HAIR) != 0u && (mask & AETHER_LIGHT_CHANNEL_FACE) == 0u) {
        return "HAIR";
    }
    if ((mask & AETHER_LIGHT_CHANNEL_BODY) != 0u && (mask & AETHER_LIGHT_CHANNEL_FACE) == 0u) {
        return "BODY";
    }
    return "CUST";
}

} // namespace

std::filesystem::path LightingStagePanels::rigs_json_path() const noexcept {
    if (!config_dir_.empty()) {
        // Resolve <repo>/config/lighting_rigs.json or user config dir cleanly
        const auto parent_cfg = config_dir_ / "lighting_rigs.json";
        if (std::filesystem::exists(parent_cfg)) {
            return parent_cfg;
        }
    }
    if (std::filesystem::exists("config/lighting_rigs.json")) {
        return std::filesystem::path("config/lighting_rigs.json");
    }
    if (!config_dir_.empty()) {
        return config_dir_ / "lighting_rigs.json";
    }
    return std::filesystem::path("config/lighting_rigs.json");
}

void LightingStagePanels::ensure_rigs_loaded() noexcept {
    if (rigs_loaded_) {
        return;
    }
    rigs_loaded_ = true;
    const auto p = rigs_json_path();
    if (!renderer::load_lighting_rigs_json(p, saved_rigs_)) {
        std::array<renderer::StudioRigPresetRecord, 5> builtins{};
        renderer::builtin_studio_rig_presets(builtins);
        saved_rigs_.assign(builtins.begin(), builtins.end());
    }
}

renderer::LightingRig& LightingStagePanels::active_rig(DevShellViewportActions& vp_actions) noexcept {
    renderer::LightingRig& rig =
        (vp_actions.mutable_lighting_rig != nullptr) ? *vp_actions.mutable_lighting_rig : fallback_rig_;
    renderer::ensure_studio_lights_initialized(rig);
    if (rig.studio_light_count > 0u) {
        selected_light_idx_ = std::clamp(selected_light_idx_, 0, static_cast<int>(rig.studio_light_count) - 1);
    } else {
        selected_light_idx_ = -1;
    }
    return rig;
}

void LightingStagePanels::ensure_initialized(DevShellViewportActions& vp_actions) noexcept {
    ensure_rigs_loaded();
    if (!stage_cam_ready_) {
        stage_cam_ready_ = true;
        stage_camera_ =
            renderer::make_stage_camera_preset(renderer::StageCameraView::FreePerspective, {0.0f, 1.32f, 0.0f});
        quad_top_camera_ =
            renderer::make_stage_camera_preset(renderer::StageCameraView::OrthoTop, {0.0f, 1.32f, 0.0f}, 3.60f);
        quad_right_camera_ =
            renderer::make_stage_camera_preset(renderer::StageCameraView::OrthoRight, {0.0f, 1.32f, 0.0f}, 2.40f);
    }
    (void)active_rig(vp_actions);
}

void LightingStagePanels::sync_key_fill_rim_mirror(const renderer::LightingRig& rig,
                                                   DevShellViewportActions& vp_actions) noexcept {
    if (rig.studio_light_count > 0u) {
        const auto& k = rig.studio_lights[0];
        vp_actions.key_azimuth_deg = k.orbit_azimuth_deg;
        vp_actions.key_elevation_deg = k.orbit_elevation_deg;
        vp_actions.key_intensity = k.intensity_ev100;
        vp_actions.lighting_mode = rig.lighting_mode;
    }
    vp_actions.lighting_dirty = true;
}

void LightingStagePanels::set_stage_view(renderer::StageCameraView view, const std::array<float, 3>& pivot_ws) noexcept {
    stage_view_ = view;
    stage_camera_ = renderer::make_stage_camera_preset(view, pivot_ws, stage_camera_.ortho_height);
}

void LightingStagePanels::toggle_quad_view() noexcept {
    quad_view_ = !quad_view_;
}

void LightingStagePanels::toggle_pilot_light(DevShellViewportActions& vp_actions, const std::array<float, 3>& pivot_ws,
                                             const std::array<float, 3>& eye_ws) noexcept {
    renderer::LightingRig& rig = active_rig(vp_actions);
    if (pilot_light_) {
        pilot_light_ = false;
        if (has_saved_before_pilot_) {
            stage_camera_ = saved_before_pilot_;
            has_saved_before_pilot_ = false;
        }
        return;
    }
    if (selected_light_idx_ < 0 || static_cast<uint32_t>(selected_light_idx_) >= rig.studio_light_count) {
        return;
    }
    saved_before_pilot_ = stage_camera_;
    has_saved_before_pilot_ = true;
    pilot_light_ = true;
    auto& light = rig.studio_lights[static_cast<size_t>(selected_light_idx_)];
    const auto pose = renderer::resolve_studio_light_world_pose(light, pivot_ws, eye_ws);
    stage_camera_.is_orthographic = false;
    stage_camera_.target_x = pose.target_ws[0];
    stage_camera_.target_y = pose.target_ws[1];
    stage_camera_.target_z = pose.target_ws[2];
    stage_camera_.distance = pose.distance_m;
    const float dx = pose.position_ws[0] - pose.target_ws[0];
    const float dy = pose.position_ws[1] - pose.target_ws[1];
    const float dz = pose.position_ws[2] - pose.target_ws[2];
    const float horiz = std::sqrt(std::max(1e-8f, dx * dx + dz * dz));
    stage_camera_.yaw_rad = std::atan2(dx, dz);
    stage_camera_.pitch_rad = std::atan2(dy, horiz);
    stage_view_ = renderer::StageCameraView::FreePerspective;
}

void LightingStagePanels::export_stage_actions(const DevShellAvatarView& avatar,
                                               DevShellViewportActions& vp_actions) noexcept {
    ensure_initialized(vp_actions);
    const std::array<float, 3> pivot = compute_pivot_ws(avatar);
    if (pilot_light_ && selected_light_idx_ >= 0) {
        renderer::LightingRig& rig = active_rig(vp_actions);
        if (static_cast<uint32_t>(selected_light_idx_) < rig.studio_light_count) {
            auto& l = rig.studio_lights[static_cast<size_t>(selected_light_idx_)];
            l.use_orbit_angles = true;
            l.orbit_azimuth_deg = stage_camera_.yaw_rad / kDegToRad;
            l.orbit_elevation_deg = stage_camera_.pitch_rad / kDegToRad;
            l.orbit_distance_m = std::clamp(stage_camera_.distance, 0.25f, 16.0f);
            l.target_ws = {stage_camera_.target_x, stage_camera_.target_y, stage_camera_.target_z};
            l.aim_lock = (std::fabs(l.target_ws[0] - pivot[0]) < 0.02f &&
                          std::fabs(l.target_ws[1] - pivot[1]) < 0.02f &&
                          std::fabs(l.target_ws[2] - pivot[2]) < 0.02f);
            sync_key_fill_rim_mirror(rig, vp_actions);
        }
    }

    vp_actions.request_stage_render = true;
    vp_actions.stage_camera = stage_camera_;
    vp_actions.stage_diag_mode = diag_mode_;
    vp_actions.stage_show_schematic = show_schematic_;
    vp_actions.stage_show_grid = show_grid_;
    vp_actions.stage_preview_25d_flatten = preview_25d_flatten_;
    vp_actions.stage_fast_panning = fast_panning_;
    vp_actions.stage_selected_light_idx = outliner_selected_cam_ ? -1 : selected_light_idx_;
    vp_actions.stage_quad_view = quad_view_;
    vp_actions.quad_top_camera = quad_top_camera_;
    vp_actions.quad_right_camera = quad_right_camera_;
}

void LightingStagePanels::draw_outliner(const DevShellAvatarView& avatar, const UiFontSet& fonts,
                                        UiSelection& selection, StudioActions& actions) noexcept {
    (void)fonts;
    ensure_initialized(actions.viewport);
    const PrecisionDarkTokens& t = tokens();
    renderer::LightingRig& rig = active_rig(actions.viewport);
    const std::array<float, 3> pivot = compute_pivot_ws(avatar);

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##LightSearch", tr("Search lights / fixtures..."), search_buf_.data(), search_buf_.size());
    ImGui::Spacing();

    if (ImGui::Button(tr("+ Add Light"), ImVec2(ImGui::GetContentRegionAvail().x * 0.52f, 0.0f))) {
        ImGui::OpenPopup("##AddFixtureMenu");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("+ Flag / Card"), ImVec2(-FLT_MIN, 0.0f))) {
        ImGui::OpenPopup("##AddFlagMenu");
    }

    if (ImGui::BeginPopup("##AddFixtureMenu")) {
        for (int p = 0; p < 12; ++p) {
            const auto prof = static_cast<renderer::StudioFixtureProfile>(p);
            if (ImGui::MenuItem(renderer::studio_fixture_name(prof))) {
                const int idx = renderer::add_studio_light(rig, prof, pivot);
                if (idx >= 0) {
                    selected_light_idx_ = idx;
                    outliner_selected_cam_ = false;
                    selection.kind = SelectionKind::StudioLight;
                    selection.item_index = idx;
                    std::snprintf(selection.name.data(), selection.name.size(), "%s",
                                  rig.studio_lights[static_cast<size_t>(idx)].name.data());
                    sync_key_fill_rim_mirror(rig, actions.viewport);
                }
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddFlagMenu")) {
        for (int m = 1; m <= 4; ++m) {
            const auto mode = static_cast<renderer::PassiveFlagMode>(m);
            if (ImGui::MenuItem(renderer::passive_flag_name(mode))) {
                const int idx = renderer::add_passive_flag(rig, mode, pivot);
                if (idx >= 0) {
                    selected_light_idx_ = idx;
                    outliner_selected_cam_ = false;
                    selection.kind = SelectionKind::StudioLight;
                    selection.item_index = idx;
                    std::snprintf(selection.name.data(), selection.name.size(), "%s",
                                  rig.studio_lights[static_cast<size_t>(idx)].name.data());
                    sync_key_fill_rim_mirror(rig, actions.viewport);
                }
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::TextColored(t.text_tertiary, "%s", tr("SCENE ANCHORS"));
    {
        if (ImGui::Selectable(tr("Studio Broadcast Camera (Locked)"), outliner_selected_cam_)) {
            outliner_selected_cam_ = true;
            selection.kind = SelectionKind::Camera;
            selection.item_index = 0;
            std::snprintf(selection.name.data(), selection.name.size(), "%s", tr("Studio Camera"));
        }
        ImGui::TextDisabled("  2.5D Z-Flatten: ON · Live Broadcast Feed");
    }

    ImGui::Spacing();
    ImGui::TextColored(t.text_tertiary, "LIGHTS (%u / %u)", rig.studio_light_count, renderer::kMaxStudioLights);

    const float row_w = ImGui::GetContentRegionAvail().x;
    for (uint32_t i = 0; i < rig.studio_light_count; ++i) {
        auto& light = rig.studio_lights[i];
        const char* fix_lbl = (light.passive_flag != renderer::PassiveFlagMode::None)
                                  ? renderer::passive_flag_name(light.passive_flag)
                                  : renderer::studio_fixture_name(light.fixture);
        if (!contains_icase(light.name.data(), search_buf_.data()) &&
            !contains_icase(fix_lbl, search_buf_.data())) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        const bool is_sel = !outliner_selected_cam_ && (selected_light_idx_ == static_cast<int>(i));

        // Eye toggle button
        ImGui::PushStyleColor(ImGuiCol_Text, light.enabled ? t.stage_cyan : t.stage_muted);
        if (ImGui::SmallButton(light.enabled ? "ON" : "--")) {
            light.enabled = !light.enabled;
            sync_key_fill_rim_mirror(rig, actions.viewport);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        // Solo button
        ImGui::PushStyleColor(ImGuiCol_Text, light.solo ? t.stage_amber : t.text_tertiary);
        if (ImGui::SmallButton("S")) {
            light.solo = !light.solo;
            sync_key_fill_rim_mirror(rig, actions.viewport);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        char row_lbl[96]{};
        std::snprintf(row_lbl, sizeof(row_lbl), "%s##lrow", light.name.data());
        if (ImGui::Selectable(row_lbl, is_sel, ImGuiSelectableFlags_None, ImVec2(std::max(80.0f, row_w - 86.0f), 0.0f))) {
            selected_light_idx_ = static_cast<int>(i);
            outliner_selected_cam_ = false;
            selection.kind = SelectionKind::StudioLight;
            selection.item_index = static_cast<int32_t>(i);
            std::snprintf(selection.name.data(), selection.name.size(), "%s", light.name.data());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s · %.2f EV · %.0fK", fix_lbl, light.intensity_ev100, light.kelvin_cct);
        }
        ImGui::PopID();
    }
}

void LightingStagePanels::draw_stage_viewport(const DevShellAvatarView& avatar, const UiFontSet& fonts, float dpi_scale,
                                              UiSelection& selection, StudioActions& actions) noexcept {
    (void)fonts;
    ensure_initialized(actions.viewport);
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::clamp(dpi_scale, 0.75f, 3.0f);
    renderer::LightingRig& rig = active_rig(actions.viewport);
    const std::array<float, 3> pivot = compute_pivot_ws(avatar);
    fast_panning_ = false;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 64.0f || avail.y < 64.0f) {
        return;
    }

    // 1. Floating Top Stage Control Bar
    {
        auto view_btn = [&](renderer::StageCameraView v, const char* lbl) {
            const bool active = !quad_view_ && (stage_view_ == v);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
            }
            if (ImGui::SmallButton(lbl)) {
                quad_view_ = false;
                if (pilot_light_) {
                    pilot_light_ = false;
                }
                set_stage_view(v, pivot);
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.0f, 3.0f * scale);
        };
        view_btn(renderer::StageCameraView::FreePerspective, "Persp");
        view_btn(renderer::StageCameraView::OrthoFront, "Front");
        view_btn(renderer::StageCameraView::OrthoBack, "Back");
        view_btn(renderer::StageCameraView::OrthoRight, "Right");
        view_btn(renderer::StageCameraView::OrthoLeft, "Left");
        view_btn(renderer::StageCameraView::OrthoTop, "Top");
        view_btn(renderer::StageCameraView::OrthoBottom, "Bot");

        if (quad_view_) {
            ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
        }
        if (ImGui::SmallButton("Quad")) {
            toggle_quad_view();
        }
        if (quad_view_) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Quad View (Ctrl+Alt+Q): Top / Right / Stage / Clean Broadcast Monitor");
        }
        ImGui::SameLine(0.0f, 8.0f * scale);

        auto diag_btn = [&](renderer::StageDiagnosticMode dm, const char* lbl) {
            const bool active = (diag_mode_ == dm);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
            }
            if (ImGui::SmallButton(lbl)) {
                diag_mode_ = dm;
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.0f, 3.0f * scale);
        };
        diag_btn(renderer::StageDiagnosticMode::Lit, "Lit");
        diag_btn(renderer::StageDiagnosticMode::Clay, "Clay 18%");
        diag_btn(renderer::StageDiagnosticMode::FalseColor, "EL-Zone");

        ImGui::SameLine(0.0f, 8.0f * scale);
        auto tgl = [&](bool& b, const char* lbl) {
            if (b) {
                ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
            }
            if (ImGui::SmallButton(lbl)) {
                b = !b;
            }
            if (b) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.0f, 3.0f * scale);
        };
        tgl(show_schematic_, "Schem");
        tgl(show_grid_, "Grid");
        tgl(preview_25d_flatten_, "2.5D Flat");
        if (pilot_light_) {
            ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
        }
        if (ImGui::SmallButton("Pilot")) {
            toggle_pilot_light(actions.viewport, pivot, avatar.camera_eye);
        }
        if (pilot_light_) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0.0f, 3.0f * scale);
        if (ImGui::SmallButton("Aim Head") && selected_light_idx_ >= 0 &&
            static_cast<uint32_t>(selected_light_idx_) < rig.studio_light_count) {
            auto& sl = rig.studio_lights[static_cast<size_t>(selected_light_idx_)];
            sl.target_ws = pivot;
            sl.aim_lock = true;
            sync_key_fill_rim_mirror(rig, actions.viewport);
        }
        ImGui::SameLine(0.0f, 3.0f * scale);
        tgl(pip_monitor_open_, "PiP Cam");

        // Gizmo Q/W/E/R pills
        ImGui::SameLine(0.0f, 8.0f * scale);
        auto giz_btn = [&](GizmoTool gt, const char* lbl) {
            const bool active = (gizmo_tool_ == gt);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
            }
            if (ImGui::SmallButton(lbl)) {
                gizmo_tool_ = gt;
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0.0f, 2.0f * scale);
        };
        giz_btn(GizmoTool::Select, "Q");
        giz_btn(GizmoTool::Move, "W");
        giz_btn(GizmoTool::Rotate, "E");
        giz_btn(GizmoTool::Scale, "R");
        ImGui::NewLine();
    }

    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 32.0f || canvas_sz.y < 32.0f) {
        return;
    }
    const ImVec2 canvas_p1(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvas_p0, canvas_p1, true);
    dl->AddRectFilled(canvas_p0, canvas_p1, t.col32_bg_canvas);

    auto draw_stage_interaction = [&](ImVec2 p0, ImVec2 sz, const char* btn_id, renderer::PreviewCamera& cam,
                                      bool allow_gizmo_pick) {
        const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
        ImGui::SetCursorScreenPos(p0);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(btn_id, sz,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();

        bool cam_moving = false;
        draw_camera_pan_orbit_zoom(cam, hov, act, !cam.is_orthographic, cam_moving);
        if (cam_moving) {
            fast_panning_ = true;
        }

        const float aspect = std::max(sz.x / std::max(sz.y, 1.0f), 0.1f);
        const auto vp = renderer::compute_view_proj(cam, aspect);

        // Draw constant screen-pixel 3D Transform Gizmo on selected light + handle click/drag
        if (allow_gizmo_pick && !outliner_selected_cam_ && selected_light_idx_ >= 0 &&
            static_cast<uint32_t>(selected_light_idx_) < rig.studio_light_count &&
            gizmo_tool_ != GizmoTool::Select) {
            auto& sl = rig.studio_lights[static_cast<size_t>(selected_light_idx_)];
            const auto pose = renderer::resolve_studio_light_world_pose(sl, pivot, avatar.camera_eye);
            const float axis_len = renderer::stage_gizmo_world_scale(cam, pose.position_ws, 68.0f * scale, sz.y);
            bool ok_c = false;
            const ImVec2 c_scr = project_world_to_canvas(vp, pose.position_ws, p0, sz, ok_c);
            if (ok_c) {
                const std::array<std::array<float, 3>, 3> axes{{
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                }};
                const std::array<ImU32, 3> axis_cols{
                    t.col32_stage_gizmo_x,
                    t.col32_stage_gizmo_y,
                    t.col32_stage_gizmo_z,
                };
                std::array<ImVec2, 3> tips{};
                std::array<bool, 3> tip_ok{};
                for (int a = 0; a < 3; ++a) {
                    const std::array<float, 3> wp{
                        pose.position_ws[0] + axes[a][0] * axis_len,
                        pose.position_ws[1] + axes[a][1] * axis_len,
                        pose.position_ws[2] + axes[a][2] * axis_len,
                    };
                    tips[a] = project_world_to_canvas(vp, wp, p0, sz, tip_ok[a]);
                    if (tip_ok[a]) {
                        const float thick = (gizmo_active_axis_ == a) ? 3.5f * scale : 2.2f * scale;
                        dl->AddLine(c_scr, tips[a], axis_cols[a], thick);
                        dl->AddCircleFilled(tips[a], 5.0f * scale, axis_cols[a]);
                    }
                }
                dl->AddCircleFilled(c_scr, 4.5f * scale, t.col32_stage_amber);

                const ImGuiIO& io = ImGui::GetIO();
                if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !io.KeyShift) {
                    const ImVec2 mp = io.MousePos;
                    gizmo_active_axis_ = -1;
                    float best_d = 18.0f * scale;
                    for (int a = 0; a < 3; ++a) {
                        if (!tip_ok[a]) {
                            continue;
                        }
                        const float d = std::hypot(mp.x - tips[a].x, mp.y - tips[a].y);
                        if (d < best_d) {
                            best_d = d;
                            gizmo_active_axis_ = a;
                        }
                    }
                    if (gizmo_active_axis_ >= 0) {
                        gizmo_dragging_ = true;
                        renderer::bake_studio_light_world_pose(sl, pivot, avatar.camera_eye);
                    }
                }
                if (gizmo_dragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) && gizmo_active_axis_ >= 0) {
                    const float drag_scale =
                        cam.is_orthographic ? (cam.ortho_height / std::max(sz.y, 1.0f)) : (pose.distance_m * 0.0028f);
                    if (gizmo_tool_ == GizmoTool::Move && tip_ok[gizmo_active_axis_]) {
                        const float adx = tips[gizmo_active_axis_].x - c_scr.x;
                        const float ady = tips[gizmo_active_axis_].y - c_scr.y;
                        const float alen = std::hypot(adx, ady);
                        const float proj_px =
                            (alen > 4.0f) ? ((io.MouseDelta.x * adx + io.MouseDelta.y * ady) / alen) : io.MouseDelta.x;
                        sl.position_ws[static_cast<size_t>(gizmo_active_axis_)] += proj_px * drag_scale;
                        sync_key_fill_rim_mirror(rig, actions.viewport);
                    } else if (gizmo_tool_ == GizmoTool::Rotate) {
                        sl.orbit_azimuth_deg += io.MouseDelta.x * 0.45f;
                        sl.orbit_elevation_deg = std::clamp(sl.orbit_elevation_deg - io.MouseDelta.y * 0.35f, -85.0f, 85.0f);
                        sl.use_orbit_angles = true;
                        sync_key_fill_rim_mirror(rig, actions.viewport);
                    } else if (gizmo_tool_ == GizmoTool::Scale) {
                        const float delta = (io.MouseDelta.x - io.MouseDelta.y) * 0.004f;
                        sl.width_or_outer_m = std::clamp(sl.width_or_outer_m + delta, 0.05f, 3.0f);
                        sl.height_or_inner_m = std::clamp(sl.height_or_inner_m + delta, 0.05f, 3.0f);
                        sync_key_fill_rim_mirror(rig, actions.viewport);
                    }
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    gizmo_dragging_ = false;
                    gizmo_active_axis_ = -1;
                }
            }
        }

        // Parallel & Perspective Ray-Picking on Left Click when not hitting a Gizmo handle
        if (allow_gizmo_pick && !gizmo_dragging_ && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const float u_01 = std::clamp((mp.x - p0.x) / std::max(sz.x, 1.0f), 0.0f, 1.0f);
            const float v_01 = std::clamp((mp.y - p0.y) / std::max(sz.y, 1.0f), 0.0f, 1.0f);
            const int hit_idx = renderer::pick_stage_studio_light(rig, pivot, avatar.camera_eye, cam, u_01, v_01,
                                                                  aspect, nullptr);
            if (hit_idx >= 0) {
                selected_light_idx_ = hit_idx;
                outliner_selected_cam_ = false;
                selection.kind = SelectionKind::StudioLight;
                selection.item_index = hit_idx;
                std::snprintf(selection.name.data(), selection.name.size(), "%s",
                              rig.studio_lights[static_cast<size_t>(hit_idx)].name.data());
            }
        }
        (void)p1;
    };

    if (quad_view_) {
        const float half_w = std::floor(canvas_sz.x * 0.5f);
        const float half_h = std::floor(canvas_sz.y * 0.5f);
        const ImVec2 cell_sz(half_w - 2.0f, half_h - 2.0f);
        struct QuadCell {
            ImVec2 p0;
            VkDescriptorSet tex;
            const char* tag;
            const char* btn_id;
            renderer::PreviewCamera* cam;
            bool allow_pick;
        };
        const QuadCell cells[4]{
            {canvas_p0, avatar.quad_top_texture, "TOP ORTHO (LIGHTING PLOT)", "##QTop", &quad_top_camera_, false},
            {ImVec2(canvas_p0.x + half_w + 2.0f, canvas_p0.y), avatar.quad_right_texture,
             "RIGHT ORTHO (ELEVATION)", "##QRight", &quad_right_camera_, false},
            {ImVec2(canvas_p0.x, canvas_p0.y + half_h + 2.0f), avatar.stage_viewport_texture,
             "STAGE 3D (SCHEMATIC)", "##QStage", &stage_camera_, true},
            {ImVec2(canvas_p0.x + half_w + 2.0f, canvas_p0.y + half_h + 2.0f), avatar.viewport_texture,
             "LIVE BROADCAST MONITOR (NO SCHEMATICS)", "##QBroadcast", &stage_camera_, false},
        };
        for (int idx = 0; idx < 4; ++idx) {
            const auto& c = cells[idx];
            const ImVec2 cp1(c.p0.x + cell_sz.x, c.p0.y + cell_sz.y);
            dl->AddRectFilled(c.p0, cp1, t.col32_bg_canvas);
            if (c.tex != VK_NULL_HANDLE) {
                dl->AddImage(reinterpret_cast<ImTextureID>(c.tex), c.p0, cp1);
            }
            dl->AddRect(c.p0, cp1, (idx == 3) ? t.col32_stage_pip_border : t.col32_node_border);
            dl->AddRectFilled(ImVec2(c.p0.x + 6.0f, c.p0.y + 6.0f),
                              ImVec2(c.p0.x + 250.0f * scale, c.p0.y + 24.0f * scale), t.col32_stage_badge_bg, 4.0f);
            dl->AddText(ImVec2(c.p0.x + 10.0f, c.p0.y + 8.0f), (idx == 3) ? t.col32_stage_cyan : t.col32_text_primary,
                        c.tag);
            if (idx < 3) {
                draw_stage_interaction(c.p0, cell_sz, c.btn_id, *c.cam, c.allow_pick);
            }
        }
    } else {
        if (avatar.stage_viewport_texture != VK_NULL_HANDLE) {
            dl->AddImage(reinterpret_cast<ImTextureID>(avatar.stage_viewport_texture), canvas_p0, canvas_p1);
        }
        draw_stage_interaction(canvas_p0, canvas_sz, "##StageCanvas", stage_camera_, true);

        // Docked / Floating PiP Live Broadcast Monitor in bottom-right corner
        if (pip_monitor_open_) {
            const float pip_w = std::clamp(260.0f * scale, 160.0f, canvas_sz.x * 0.36f);
            const float pip_h = pip_w * (9.0f / 16.0f);
            const ImVec2 pip_p0(canvas_p1.x - pip_w - 14.0f * scale, canvas_p1.y - pip_h - 14.0f * scale);
            const ImVec2 pip_p1(pip_p0.x + pip_w, pip_p0.y + pip_h);
            dl->AddRectFilled(ImVec2(pip_p0.x - 2.0f, pip_p0.y - 20.0f * scale), ImVec2(pip_p1.x + 2.0f, pip_p1.y + 2.0f),
                              t.col32_stage_badge_bg, 4.0f);
            if (avatar.viewport_texture != VK_NULL_HANDLE) {
                dl->AddImage(reinterpret_cast<ImTextureID>(avatar.viewport_texture), pip_p0, pip_p1);
            }
            dl->AddRect(pip_p0, pip_p1, t.col32_stage_pip_border, 2.0f);
            dl->AddText(ImVec2(pip_p0.x + 6.0f, pip_p0.y - 17.0f * scale), t.col32_stage_cyan,
                        "LIVE CAM · BroadcastView (Clean)");
        }
    }

    // Interactive ViewCube badge in Top-Right corner
    {
        const float vc_w = 104.0f * scale;
        const ImVec2 vc_pos(canvas_p1.x - vc_w - 12.0f * scale, canvas_p0.y + 10.0f * scale);
        dl->AddRectFilled(vc_pos, ImVec2(vc_pos.x + vc_w, vc_pos.y + 72.0f * scale), t.col32_stage_viewcube_bg, 6.0f);
        dl->AddRect(vc_pos, ImVec2(vc_pos.x + vc_w, vc_pos.y + 72.0f * scale), t.col32_node_border, 6.0f);
        ImGui::SetCursorScreenPos(ImVec2(vc_pos.x + 6.0f * scale, vc_pos.y + 4.0f * scale));
        ImGui::BeginGroup();
        auto vc_btn = [&](const char* lbl, renderer::StageCameraView sv) {
            if (ImGui::SmallButton(lbl)) {
                quad_view_ = false;
                set_stage_view(sv, pivot);
            }
        };
        vc_btn("  T  ", renderer::StageCameraView::OrthoTop);
        ImGui::SameLine(0.0f, 3.0f);
        vc_btn("Iso", renderer::StageCameraView::FreePerspective);
        vc_btn("L", renderer::StageCameraView::OrthoLeft);
        ImGui::SameLine(0.0f, 3.0f);
        vc_btn("F", renderer::StageCameraView::OrthoFront);
        ImGui::SameLine(0.0f, 3.0f);
        vc_btn("R", renderer::StageCameraView::OrthoRight);
        vc_btn("B", renderer::StageCameraView::OrthoBack);
        ImGui::SameLine(0.0f, 3.0f);
        vc_btn("Btm", renderer::StageCameraView::OrthoBottom);
        ImGui::EndGroup();
    }

    // False Color EL-Zone Stop Scale HUD in Bottom-Left when active
    if (diag_mode_ == renderer::StageDiagnosticMode::FalseColor) {
        static constexpr const char* kElLabels[7]{"<-4", "-3", "-1", "0(18%)", "+1Skin", "+2", ">+3EV"};
        const ImVec2 el_pos(canvas_p0.x + 14.0f * scale, canvas_p1.y - 34.0f * scale);
        dl->AddRectFilled(ImVec2(el_pos.x - 4.0f, el_pos.y - 4.0f),
                          ImVec2(el_pos.x + 330.0f * scale, el_pos.y + 24.0f * scale), t.col32_stage_badge_bg, 4.0f);
        for (int k = 0; k < 7; ++k) {
            const float x0 = el_pos.x + static_cast<float>(k) * 46.0f * scale;
            dl->AddRectFilled(ImVec2(x0, el_pos.y), ImVec2(x0 + 42.0f * scale, el_pos.y + 10.0f * scale),
                              t.col32_stage_el_zones[static_cast<size_t>(k)]);
            dl->AddText(ImVec2(x0, el_pos.y + 11.0f * scale), t.col32_text_secondary, kElLabels[k]);
        }
    }

    dl->PopClipRect();
}

void LightingStagePanels::draw_inspector(const DevShellAvatarView& avatar, const UiFontSet& fonts, ParamStore& params,
                                         UiSelection& selection, StudioActions& actions) noexcept {
    (void)fonts;
    (void)selection;
    ensure_initialized(actions.viewport);
    const PrecisionDarkTokens& t = tokens();
    renderer::LightingRig& rig = active_rig(actions.viewport);
    const std::array<float, 3> pivot = compute_pivot_ws(avatar);

    if (outliner_selected_cam_) {
        ImGui::TextColored(t.stage_cyan, "%s", tr("Studio Broadcast Camera (Locked Isolation)"));
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr("Live Broadcast outputs (Spout2, VCam, NDI, Live Studio, PiP) always render "
                                    "cleanly from this camera with 2.5D Z-Flattening ON and zero Stage schematics."));
        return;
    }

    if (selected_light_idx_ < 0 || static_cast<uint32_t>(selected_light_idx_) >= rig.studio_light_count) {
        ImGui::TextDisabled("%s", tr("Select a light or modifier in the Stage or Outliner."));
        return;
    }

    auto& sl = rig.studio_lights[static_cast<size_t>(selected_light_idx_)];
    bool dirty = false;

    ImGui::TextColored(t.stage_amber, "%s", sl.name.data());
    ImGui::Separator();

    const float ctrl_w = std::max(72.0f, ImGui::GetContentRegionAvail().x * 0.52f);
    ImGui::PushItemWidth(ctrl_w);
    if (ImGui::InputText(tr("Name"), sl.name.data(), sl.name.size())) {
        dirty = true;
    }
    dirty |= ImGui::Checkbox(tr("Enabled"), &sl.enabled);
    ImGui::SameLine();
    dirty |= ImGui::Checkbox(tr("Solo"), &sl.solo);
    ImGui::SameLine();
    dirty |= ImGui::Checkbox(tr("Cast Shadow"), &sl.cast_shadow);

    // Fixture profile selector (all 12 photometric fixtures)
    if (ImGui::BeginCombo(tr("Fixture Profile"), renderer::studio_fixture_name(sl.fixture))) {
        for (int p = 0; p < 12; ++p) {
            const auto prof = static_cast<renderer::StudioFixtureProfile>(p);
            const bool is_sel = (sl.fixture == prof);
            if (ImGui::Selectable(renderer::studio_fixture_name(prof), is_sel)) {
                sl.fixture = prof;
                renderer::configure_fixture_defaults(sl, prof);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    // Passive flag mode selector (Neg Fill / Bounce Cards)
    if (ImGui::BeginCombo(tr("Passive Flag / Card"), renderer::passive_flag_name(sl.passive_flag))) {
        for (int m = 0; m <= 4; ++m) {
            const auto mode = static_cast<renderer::PassiveFlagMode>(m);
            const bool is_sel = (sl.passive_flag == mode);
            if (ImGui::Selectable(renderer::passive_flag_name(mode), is_sel)) {
                sl.passive_flag = mode;
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::CollapsingHeader(tr("Transform & Aim"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(tr("Orbit Polar Coordinates"), &sl.use_orbit_angles)) {
            if (!sl.use_orbit_angles) {
                renderer::bake_studio_light_world_pose(sl, pivot, avatar.camera_eye);
            }
            dirty = true;
        }
        if (sl.use_orbit_angles) {
            dirty |= ImGui::SliderFloat(tr("Azimuth (deg)"), &sl.orbit_azimuth_deg, -180.0f, 180.0f, "%.1f");
            dirty |= ImGui::SliderFloat(tr("Elevation (deg)"), &sl.orbit_elevation_deg, -80.0f, 85.0f, "%.1f");
            dirty |= ImGui::SliderFloat(tr("Distance (m)"), &sl.orbit_distance_m, 0.35f, 8.0f, "%.2f");
        } else {
            dirty |= ImGui::DragFloat3(tr("Position XYZ (m)"), sl.position_ws.data(), 0.02f, -10.0f, 10.0f);
        }
        dirty |= ImGui::SliderFloat(tr("Fixture Roll (deg)"), &sl.roll_deg, -180.0f, 180.0f, "%.1f");
        dirty |= ImGui::Checkbox(tr("Aim Lock -> Pivot"), &sl.aim_lock);
        if (!sl.aim_lock) {
            dirty |= ImGui::DragFloat3(tr("Target XYZ (m)"), sl.target_ws.data(), 0.02f, -10.0f, 10.0f);
        }
        if (ImGui::SmallButton(tr("Aim at Avatar Head"))) {
            sl.target_ws = pivot;
            sl.aim_lock = true;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Aim at Chest"))) {
            sl.target_ws = {0.0f, avatar.bone_chest_y, 0.0f};
            sl.aim_lock = false;
            dirty = true;
        }
    }

    if (sl.passive_flag == renderer::PassiveFlagMode::None &&
        ImGui::CollapsingHeader(tr("Photometry & Color"), ImGuiTreeNodeFlags_DefaultOpen)) {
        dirty |= ImGui::SliderFloat(tr("Intensity EV100"), &sl.intensity_ev100, 0.0f, 4.0f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Exposure Stops"), &sl.exposure_stops, -4.0f, 4.0f, "%+.2f EV");
        dirty |= ImGui::SliderFloat(tr("Color Temp (CCT)"), &sl.kelvin_cct, 1500.0f, 20000.0f, "%.0f K");
        dirty |= ImGui::SliderFloat(tr("Green / Magenta"), &sl.green_magenta, -1.0f, 1.0f, "%+.2f");
        dirty |= ImGui::ColorEdit3(tr("Linear Tint"), sl.tint_rgb.data());
        dirty |= ImGui::SliderFloat(tr("Catchlight Weight"), &sl.catchlight_weight, 0.0f, 2.0f, "%.2f");

        // Light Channels mask
        auto ch_chk = [&](const char* lbl, uint32_t bit) {
            bool on = (sl.channel_mask & bit) != 0u;
            if (ImGui::Checkbox(lbl, &on)) {
                if (on) {
                    sl.channel_mask |= bit;
                } else {
                    sl.channel_mask &= ~bit;
                }
                dirty = true;
            }
        };
        ch_chk("Face", AETHER_LIGHT_CHANNEL_FACE);
        ImGui::SameLine();
        ch_chk("Hair", AETHER_LIGHT_CHANNEL_HAIR);
        ImGui::SameLine();
        ch_chk("Body", AETHER_LIGHT_CHANNEL_BODY);
        ImGui::SameLine();
        ch_chk("EyeSpec", AETHER_LIGHT_CHANNEL_CATCHLIGHT);
    }

    if (ImGui::CollapsingHeader(tr("Aperture & Optical Modifiers"), ImGuiTreeNodeFlags_DefaultOpen)) {
        dirty |= ImGui::SliderFloat(tr("Width / Outer (m)"), &sl.width_or_outer_m, 0.05f, 2.5f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Height / Inner (m)"), &sl.height_or_inner_m, 0.05f, 2.5f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Spot Inner (deg)"), &sl.spot_inner_deg, 2.0f, 170.0f, "%.1f");
        dirty |= ImGui::SliderFloat(tr("Spot Outer (deg)"), &sl.spot_outer_deg, 4.0f, 179.0f, "%.1f");
        if (sl.spot_outer_deg < sl.spot_inner_deg + 1.0f) {
            sl.spot_outer_deg = std::min(179.0f, sl.spot_inner_deg + 1.0f);
        }
        dirty |= ImGui::Checkbox(tr("Softbox Frame"), &sl.softbox_enabled);
        dirty |= ImGui::Checkbox(tr("Honeycomb Grid"), &sl.grid_enabled);
        if (sl.grid_enabled) {
            ImGui::SameLine();
            dirty |= ImGui::SliderFloat("##GridDeg", &sl.grid_angle_deg, 15.0f, 70.0f, "%.0f deg");
        }
        dirty |= ImGui::Checkbox(tr("4-Leaf Barn Doors"), &sl.barn_doors_enabled);
        if (sl.barn_doors_enabled) {
            dirty |= ImGui::SliderFloat4(tr("Cuts (T/B/L/R)"), sl.barn_door_cuts_deg.data(), 8.0f, 82.0f, "%.0f");
        }
        dirty |= ImGui::Checkbox(tr("Diffusion Scrim"), &sl.diffusion_enabled);
        if (sl.diffusion_enabled) {
            ImGui::SameLine();
            dirty |= ImGui::SliderFloat("##DiffDen", &sl.diffusion_density, 0.0f, 1.0f, "%.2f");
        }
        dirty |= ImGui::Checkbox(tr("Color Gel"), &sl.gel_enabled);
        if (sl.gel_enabled && ImGui::BeginCombo(tr("Gel Preset"), renderer::studio_gel_name(sl.gel))) {
            for (int g = 0; g < 12; ++g) {
                const auto gp = static_cast<renderer::StudioGelPreset>(g);
                if (ImGui::Selectable(renderer::studio_gel_name(gp), sl.gel == gp)) {
                    sl.gel = gp;
                    dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo(tr("Gobo Pattern"), renderer::studio_gobo_name(sl.gobo))) {
            for (int g = 0; g < 5; ++g) {
                const auto gb = static_cast<renderer::StudioGoboPattern>(g);
                if (ImGui::Selectable(renderer::studio_gobo_name(gb), sl.gobo == gb)) {
                    sl.gobo = gb;
                    dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        dirty |= ImGui::Checkbox(tr("Window Portal"), &sl.window_portal);
        ImGui::SameLine();
        dirty |= ImGui::Checkbox(tr("Coaxial Ring Lock"), &sl.coaxial_lock);
    }

    if (ImGui::CollapsingHeader(tr("Satsuei & Split Grading"))) {
        dirty |= ImGui::SliderFloat(tr("Light Wrap"), &rig.satsuei.light_wrap_strength, 0.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Halation"), &rig.satsuei.halation_strength, 0.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Backscatter"), &rig.satsuei.backscatter_strength, 0.0f, 1.5f, "%.2f");
        dirty |= ImGui::SliderFloat(tr("Avatar Contrast"), &rig.split_grading.avatar_contrast, 0.6f, 1.5f, "%.2f");
        float flat = rig.global_live2d_flatness;
        if (ImGui::SliderFloat(tr("Broadcast 2.5D Flatten"), &flat, 0.0f, 1.0f, "%.2f")) {
            rig.global_live2d_flatness = flat;
            (void)params.set(ParamId::RenderFlatness, static_cast<double>(flat));
            dirty = true;
        }
    }

    ImGui::PopItemWidth();

    if (dirty) {
        sync_key_fill_rim_mirror(rig, actions.viewport);
    }
}

void LightingStagePanels::draw_mixer_console(const DevShellAvatarView& avatar, const UiFontSet& fonts,
                                             UiSelection& selection, StudioActions& actions) noexcept {
    (void)fonts;
    ensure_initialized(actions.viewport);
    const PrecisionDarkTokens& t = tokens();
    renderer::LightingRig& rig = active_rig(actions.viewport);
    const std::array<float, 3> pivot = compute_pivot_ws(avatar);
    const float fs = std::max(10.0f, ImGui::GetFontSize());

    // Preset Bar & `config/lighting_rigs.json` Persistence (wraps gracefully when narrow at 150%+ DPI)
    for (int p = 0; p < 5; ++p) {
        const auto pid = static_cast<renderer::StudioRigPresetId>(p);
        if (p == active_preset_idx_) {
            ImGui::PushStyleColor(ImGuiCol_Button, t.bg_selected);
        }
        if (ImGui::SmallButton(renderer::studio_rig_preset_name(pid))) {
            active_preset_idx_ = p;
            renderer::apply_studio_rig_preset(rig, pid, pivot);
            selected_light_idx_ = 0;
            sync_key_fill_rim_mirror(rig, actions.viewport);
        }
        if (p == active_preset_idx_) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    }

    if (ImGui::GetContentRegionAvail().x < fs * 15.0f) {
        ImGui::NewLine();
    }
    ImGui::SetNextItemWidth(std::min(145.0f, std::max(fs * 6.0f, ImGui::GetContentRegionAvail().x * 0.25f)));
    ImGui::InputTextWithHint("##PresetName", "Preset name", save_name_buf_.data(), save_name_buf_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save Rig"))) {
        if (renderer::save_or_update_lighting_rig_preset(rigs_json_path(), save_name_buf_.data(), rig)) {
            rigs_loaded_ = false;
            ensure_rigs_loaded();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Load Rigs..."))) {
        rigs_loaded_ = false;
        ensure_rigs_loaded();
        ImGui::OpenPopup("##SavedRigsPopup");
    }
    if (ImGui::BeginPopup("##SavedRigsPopup")) {
        for (size_t k = 0; k < saved_rigs_.size(); ++k) {
            if (ImGui::MenuItem(saved_rigs_[k].name.data())) {
                rig = saved_rigs_[k].rig;
                renderer::ensure_studio_lights_initialized(rig, pivot);
                selected_light_idx_ = 0;
                sync_key_fill_rim_mirror(rig, actions.viewport);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    constexpr ImGuiTableFlags kTblFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                          ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##LightMixerTable", 10, kTblFlags, ImVec2(-FLT_MIN, -FLT_MIN))) {
        ImGui::TableSetupColumn("Light Name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, fs * 2.0f);
        ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, fs * 1.5f);
        ImGui::TableSetupColumn("Fixture", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Mods", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
        ImGui::TableSetupColumn("CCT", ImGuiTableColumnFlags_WidthFixed, fs * 4.2f);
        ImGui::TableSetupColumn("EV100", ImGuiTableColumnFlags_WidthFixed, fs * 5.2f);
        ImGui::TableSetupColumn("Shd", ImGuiTableColumnFlags_WidthFixed, fs * 2.1f);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, fs * 2.6f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, fs * 5.6f);
        ImGui::TableHeadersRow();

        bool dirty = false;
        int remove_req = -1;
        for (uint32_t i = 0; i < rig.studio_light_count; ++i) {
            auto& sl = rig.studio_lights[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            ImGui::TableSetColumnIndex(0);
            const bool is_sel = (!outliner_selected_cam_ && selected_light_idx_ == static_cast<int>(i));
            if (ImGui::Selectable(sl.name.data(), is_sel)) {
                selected_light_idx_ = static_cast<int>(i);
                outliner_selected_cam_ = false;
                selection.kind = SelectionKind::StudioLight;
                selection.item_index = static_cast<int32_t>(i);
                std::snprintf(selection.name.data(), selection.name.size(), "%s", sl.name.data());
            }

            ImGui::TableSetColumnIndex(1);
            dirty |= ImGui::Checkbox("##En", &sl.enabled);

            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, sl.solo ? t.stage_amber : t.text_tertiary);
            if (ImGui::SmallButton("S")) {
                sl.solo = !sl.solo;
                dirty = true;
            }
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(3);
            const char* fix_short = (sl.passive_flag != renderer::PassiveFlagMode::None)
                                        ? renderer::passive_flag_name(sl.passive_flag)
                                        : renderer::studio_fixture_name(sl.fixture);
            ImGui::TextUnformatted(fix_short);

            ImGui::TableSetColumnIndex(4);
            char mods[16]{};
            std::snprintf(mods, sizeof(mods), "%s%s%s%s", sl.softbox_enabled ? "S" : "-", sl.grid_enabled ? "G" : "-",
                          sl.barn_doors_enabled ? "B" : "-", sl.gel_enabled ? "Gel" : "-");
            ImGui::TextColored(t.stage_cyan, "%s", mods);

            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(-FLT_MIN);
            dirty |= ImGui::DragFloat("##K", &sl.kelvin_cct, 50.0f, 1500.0f, 20000.0f, "%.0fK");

            ImGui::TableSetColumnIndex(6);
            ImGui::SetNextItemWidth(-FLT_MIN);
            dirty |= ImGui::SliderFloat("##EV", &sl.intensity_ev100, 0.0f, 3.0f, "%.2f");

            ImGui::TableSetColumnIndex(7);
            dirty |= ImGui::Checkbox("##Sh", &sl.cast_shadow);

            ImGui::TableSetColumnIndex(8);
            ImGui::TextDisabled("%s", channel_short_label(sl.channel_mask));

            ImGui::TableSetColumnIndex(9);
            if (ImGui::SmallButton("Dup")) {
                const int dup_i = renderer::duplicate_studio_light(rig, i);
                if (dup_i >= 0) {
                    selected_light_idx_ = dup_i;
                    dirty = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                remove_req = static_cast<int>(i);
            }
            ImGui::PopID();
        }
        if (remove_req >= 0 && renderer::remove_studio_light(rig, static_cast<uint32_t>(remove_req))) {
            selected_light_idx_ = std::clamp(selected_light_idx_, 0, std::max(0, static_cast<int>(rig.studio_light_count) - 1));
            dirty = true;
        }
        if (dirty) {
            sync_key_fill_rim_mirror(rig, actions.viewport);
        }
        ImGui::EndTable();
    }
}

} // namespace aether::ui
