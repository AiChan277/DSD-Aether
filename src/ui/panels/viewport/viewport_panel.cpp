#include "ui/panels/viewport/viewport_panel.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::ui {
namespace {

constexpr float kPi = 3.14159265f;

void draw_styled_legend_segment(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, int style_kind,
                                float thickness) noexcept {
    if (style_kind == 0) {
        // Solid (Tracked w = 1)
        dl->AddLine(a, b, col, thickness);
    } else if (style_kind == 1) {
        // Dashed (Interpolating 0 < w < 1)
        const float dx = b.x - a.x;
        const float step = 8.0f;
        for (float x = 0.0f; x < dx; x += step) {
            const float x2 = std::min(dx, x + 5.0f);
            dl->AddLine(ImVec2(a.x + x, a.y), ImVec2(a.x + x2, b.y), col, thickness);
        }
    } else {
        // Dotted (Procedural w = 0)
        const float dx = b.x - a.x;
        for (float x = 2.0f; x < dx; x += 5.0f) {
            dl->AddCircleFilled(ImVec2(a.x + x, a.y), thickness * 0.65f, col);
        }
    }
}

} // namespace

void ViewportPanel::recall_camera_bookmark(size_t idx, StudioActions& actions) {
    if (idx < bookmarks_.size() && bookmarks_[idx].valid) {
        const auto& bm = bookmarks_[idx];
        actions.viewport.orbit_yaw_delta += (bm.yaw_deg - cam_yaw_deg_);
        actions.viewport.orbit_pitch_delta += (bm.pitch_deg - cam_pitch_deg_);
        cam_yaw_deg_ = bm.yaw_deg;
        cam_pitch_deg_ = bm.pitch_deg;
        cam_dist_ = bm.distance;
        cam_target_y_ = bm.target_y;
    }
}

void ViewportPanel::store_camera_bookmark(size_t idx) {
    if (idx < bookmarks_.size()) {
        bookmarks_[idx] = CameraBookmark{true, cam_yaw_deg_, cam_pitch_deg_, cam_dist_, cam_target_y_};
    }
}

void ViewportPanel::draw(const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                         const DevShellRenderView& render_view, const DevShellInputEcho& input_echo,
                         bool tracking_paused, ParamStore& params, UndoStack& undo, const UiFontSet& fonts,
                         float dpi_scale, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin(tr("Viewport"))) {
        // Compact 28 px top overlay bar (§12.6)
        ImGui::SetCursorPos(ImVec2(t.panel_pad_px * scale, t.unit_px * scale));
        ImGui::BeginGroup();

        auto tool_btn = [&](GizmoTool tool, const char* lbl, const char* tip) {
            const bool active = (gizmo_tool_ == tool);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? t.accent_button : t.bg_raised);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? t.text_on_accent : t.text_secondary);
            if (ImGui::Button(lbl, ImVec2(24.0f * scale, t.control_h_compact_px * scale))) {
                gizmo_tool_ = tool;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr(tip));
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0.0f, 2.0f * scale);
        };

        tool_btn(GizmoTool::Select, "Q", "Select (Q)");
        tool_btn(GizmoTool::Move, "W", "Move (W)");
        tool_btn(GizmoTool::Rotate, "E", "Rotate (E)");
        tool_btn(GizmoTool::Scale, "R", "Scale (R)");

        if (ImGui::Button(gizmo_local_ ? "Local##x" : "World##x",
                          ImVec2(48.0f * scale, t.control_h_compact_px * scale))) {
            gizmo_local_ = !gizmo_local_;
        }
        ImGui::SameLine(0.0f, t.unit_px * scale);
        if (ImGui::Button(show_grid_ ? "Grid On##g" : "Grid Off##g",
                          ImVec2(56.0f * scale, t.control_h_compact_px * scale))) {
            show_grid_ = !show_grid_;
        }
        ImGui::SameLine(0.0f, t.unit_px * scale);

        if (ImGui::Button(tr("Reset Cam"), ImVec2(72.0f * scale, t.control_h_compact_px * scale))) {
            actions.viewport.reset_camera = true;
            cam_yaw_deg_ = 0.0f;
            cam_pitch_deg_ = 8.0f;
            cam_dist_ = 2.1f;
        }
        ImGui::SameLine(0.0f, t.unit_px * scale);

        bool rt_on = params.get_bool(ParamId::RenderRtEnabled);
        ImGui::PushStyleColor(ImGuiCol_Button, rt_on ? t.accent_button : t.bg_raised);
        ImGui::PushStyleColor(ImGuiCol_Text, rt_on ? t.text_on_accent : t.text_secondary);
        if (ImGui::Button(rt_on ? "RT On##rt" : "RT Off##rt", ImVec2(52.0f * scale, t.control_h_compact_px * scale))) {
            const double old_v = params.get(ParamId::RenderRtEnabled);
            const double next_v = rt_on ? 0.0 : 1.0;
            if (params.set(ParamId::RenderRtEnabled, next_v)) {
                undo.push_param(ParamId::RenderRtEnabled, old_v, next_v);
                actions.viewport.lighting_dirty = true;
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, t.unit_px * scale);

        bool hm_on = params.get_bool(ParamId::AppSkeletonHeatmap);
        ImGui::PushStyleColor(ImGuiCol_Button, hm_on ? t.bg_selected : t.bg_raised);
        if (ImGui::Button("Heatmap##hm", ImVec2(64.0f * scale, t.control_h_compact_px * scale))) {
            const double old_v = params.get(ParamId::AppSkeletonHeatmap);
            if (params.set(ParamId::AppSkeletonHeatmap, hm_on ? 0.0 : 1.0)) {
                undo.push_param(ParamId::AppSkeletonHeatmap, old_v, hm_on ? 0.0 : 1.0);
            }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, t.unit_px * scale);

        bool orb_on = params.get_bool(ParamId::AppOrbitOrbVisible);
        ImGui::PushStyleColor(ImGuiCol_Button, orb_on ? t.bg_selected : t.bg_raised);
        if (ImGui::Button("Orb##orb", ImVec2(38.0f * scale, t.control_h_compact_px * scale))) {
            const double old_v = params.get(ParamId::AppOrbitOrbVisible);
            if (params.set(ParamId::AppOrbitOrbVisible, orb_on ? 0.0 : 1.0)) {
                undo.push_param(ParamId::AppOrbitOrbVisible, old_v, orb_on ? 0.0 : 1.0);
            }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, t.unit_px * scale);

        ImGui::SetNextItemWidth(116.0f * scale);
        int dbg = params.get_int(ParamId::RenderDebugView);
        static constexpr const char* kDbgViews =
            "Final\0Albedo\0True Normals\0Proxy Normals\0Z-Flatten\0SDF Face\0Hair Stencil\0"
            "RT Raw\0RT Denoised\0Rim Occ\0Bounce\0Outline\0Motion Vec\0Overdraw\0";
        if (ImGui::Combo("##dbg", &dbg, kDbgViews)) {
            const double old_v = params.get(ParamId::RenderDebugView);
            if (params.set(ParamId::RenderDebugView, static_cast<double>(dbg))) {
                undo.push_param(ParamId::RenderDebugView, old_v, static_cast<double>(dbg));
            }
        }

        ImGui::EndGroup();

        // Viewport canvas surface
        const ImVec2 vp_pos = ImGui::GetCursorScreenPos();
        const ImVec2 vp_avail = ImGui::GetContentRegionAvail();
        const float vp_w = std::max(64.0f, vp_avail.x);
        const float vp_h = std::max(64.0f, vp_avail.y);
        actions.viewport.desired_vp_w = static_cast<uint32_t>(vp_w);
        actions.viewport.desired_vp_h = static_cast<uint32_t>(vp_h);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 vp_max(vp_pos.x + vp_w, vp_pos.y + vp_h);

        if (params.get_bool(ParamId::AppCheckerboardBg)) {
            dl->PushClipRect(vp_pos, vp_max, true);
            const float cell = std::round(16.0f * scale);
            dl->AddRectFilled(vp_pos, vp_max, t.col32_checker_a);
            for (float y = vp_pos.y; y < vp_max.y; y += cell) {
                const int row = static_cast<int>((y - vp_pos.y) / cell);
                for (float x = vp_pos.x + ((row & 1) != 0 ? cell : 0.0f); x < vp_max.x; x += cell * 2.0f) {
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(vp_max.x, x + cell), std::min(vp_max.y, y + cell)),
                                      t.col32_checker_b);
                }
            }
            dl->PopClipRect();
        } else {
            dl->AddRectFilled(vp_pos, vp_max, t.col32_bg_canvas);
        }

        if (avatar.viewport_texture != VK_NULL_HANDLE) {
            ImGui::SetCursorScreenPos(vp_pos);
            ImGui::Image(reinterpret_cast<ImTextureID>(avatar.viewport_texture), ImVec2(vp_w, vp_h));
        } else {
            ImGui::SetCursorScreenPos(vp_pos);
            ImGui::Dummy(ImVec2(vp_w, vp_h));
        }

        if (show_grid_) {
            const float cy = vp_pos.y + vp_h * 0.78f;
            dl->AddLine(ImVec2(vp_pos.x + vp_w * 0.25f, cy), ImVec2(vp_pos.x + vp_w * 0.75f, cy),
                        t.col32_border_default, hairline_px(scale));
        }

        // Camera controls inside viewport region (respecting Light Orbit Orb priority)
        const float orb_px = std::round(t.orbit_orb_px * scale);
        const ImVec2 orb_pos(vp_max.x - orb_px - 14.0f * scale, vp_max.y - orb_px - 28.0f * scale);
        const ImVec2 orb_center(orb_pos.x + orb_px * 0.5f, orb_pos.y + orb_px * 0.5f);

        ImGui::SetCursorScreenPos(vp_pos);
        ImGui::InvisibleButton("##ViewportInput", ImVec2(vp_w, vp_h));
        const bool vp_hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();

        if (vp_hovered && !orb_dragging_) {
            const float dx_to_orb = io.MousePos.x - orb_center.x;
            const float dy_to_orb = io.MousePos.y - orb_center.y;
            const bool over_orb = params.get_bool(ParamId::AppOrbitOrbVisible) &&
                                  (dx_to_orb * dx_to_orb + dy_to_orb * dy_to_orb) <= (orb_px * orb_px * 0.28f);
            if (!over_orb) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyAlt) {
                    actions.viewport.orbit_yaw_delta += io.MouseDelta.x * 0.35f;
                    actions.viewport.orbit_pitch_delta += io.MouseDelta.y * 0.35f;
                    cam_yaw_deg_ += io.MouseDelta.x * 0.35f;
                    cam_pitch_deg_ = std::clamp(cam_pitch_deg_ + io.MouseDelta.y * 0.35f, -75.0f, 75.0f);
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                           ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    actions.viewport.pan_dx += io.MouseDelta.x * 0.004f;
                    actions.viewport.pan_dy += io.MouseDelta.y * 0.004f;
                }
                if (std::fabs(io.MouseWheel) > 1e-4f) {
                    actions.viewport.zoom_delta += io.MouseWheel * 0.18f;
                    cam_dist_ = std::clamp(cam_dist_ - io.MouseWheel * 0.18f, 0.5f, 6.0f);
                }
            }
        }

        // Top-right floating Tracking HUD pill (§12.6, Fig 12-1a)
        const float hud_w = std::round(248.0f * scale);
        const float hud_h = std::round(64.0f * scale);
        const ImVec2 hud_pos(vp_max.x - hud_w - 10.0f * scale, vp_pos.y + 8.0f * scale);
        dl->AddRectFilled(hud_pos, ImVec2(hud_pos.x + hud_w, hud_pos.y + hud_h), t.col32_bg_overlay,
                          t.radius_popover_px * scale);
        dl->AddRect(hud_pos, ImVec2(hud_pos.x + hud_w, hud_pos.y + hud_h), t.col32_border_default,
                    t.radius_popover_px * scale, 0, hairline_px(scale));

        ImGui::SetCursorScreenPos(ImVec2(hud_pos.x + 8.0f * scale, hud_pos.y + 6.0f * scale));
        ImGui::BeginGroup();
        if (fonts.mono != nullptr) {
            ImGui::PushFont(fonts.mono);
        }
        const ImU32 ep_dot = tracking.ready ? (tracking.demotion_count > 0 ? t.col32_status_warn : t.col32_status_ok)
                                            : t.col32_status_error;
        dl->AddCircleFilled(ImVec2(hud_pos.x + 12.0f * scale, hud_pos.y + 13.0f * scale), 3.5f * scale, ep_dot);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f * scale);
        ImGui::TextColored(t.text_primary, "%s · %.1f ms", tracking.active_ep_label,
                           static_cast<double>(tracking.inference_ms));
        if (ImGui::SmallButton(tracking_paused ? icons::kPlay : icons::kPause)) {
            actions.request_pause_tracking = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Calibrate"))) {
            actions.viewport.request_calibrate_neutral = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(t.text_secondary, "Face %.0f%% · GPU %.1f ms",
                           static_cast<double>(tracking.face_confidence * 100.0f),
                           static_cast<double>(render_view.total_gpu_ms));
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        ImGui::EndGroup();

        // Optional Confidence Heatmap legend (bottom-left §12.6 / §12.14 color + line style)
        if (params.get_bool(ParamId::AppSkeletonHeatmap)) {
            draw_confidence_heatmap_legend(tracking, fonts, scale,
                                           ImVec2(vp_pos.x + 10.0f * scale, vp_max.y - 76.0f * scale));
        }

        // Light Orbit Orb (112 px, bottom-right, Alt+L §12.6)
        if (params.get_bool(ParamId::AppOrbitOrbVisible)) {
            draw_light_orbit_orb(avatar, params, undo, fonts, scale, orb_pos, actions);
        }

        // Suppress unused param warning while keeping live input telemetry accessible
        (void)input_echo;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void ViewportPanel::draw_light_orbit_orb(const DevShellAvatarView& avatar, ParamStore& params, UndoStack& undo,
                                         const UiFontSet& fonts, float dpi_scale, ImVec2 orb_pos,
                                         StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float orb_px = std::round(t.orbit_orb_px * dpi_scale);
    const float radius = orb_px * 0.5f;
    const ImVec2 center(orb_pos.x + radius, orb_pos.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddCircleFilled(center, radius, t.col32_bg_inset, 48);
    if (avatar.orbit_orb_texture != VK_NULL_HANDLE) {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(avatar.orbit_orb_texture), orb_pos,
                            ImVec2(orb_pos.x + orb_px, orb_pos.y + orb_px), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                            0xFFFFFFFFu, radius);
    }
    dl->AddCircle(center, radius, t.col32_border_strong, 48, hairline_px(dpi_scale));
    dl->AddLine(ImVec2(center.x - radius + 4.0f, center.y), ImVec2(center.x + radius - 4.0f, center.y),
                t.col32_border_subtle, 1.0f);
    dl->AddLine(ImVec2(center.x, center.y - radius + 4.0f), ImVec2(center.x, center.y + radius - 4.0f),
                t.col32_border_subtle, 1.0f);

    ImGui::SetCursorScreenPos(orb_pos);
    ImGui::InvisibleButton("##LightOrbitOrb", ImVec2(orb_px, orb_px));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    const float cur_az = params.get_float(ParamId::RenderKeyAzimuthDeg);
    const float cur_el = params.get_float(ParamId::RenderKeyElevationDeg);
    (void)cur_el;

    if (ImGui::IsItemActivated()) {
        orb_dragging_ = true;
        undo.begin_drag(ParamId::RenderKeyAzimuthDeg, static_cast<double>(cur_az));
    }

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float nx = std::clamp((mp.x - center.x) / (radius * 0.85f), -1.0f, 1.0f);
        const float ny = std::clamp((center.y - mp.y) / (radius * 0.85f), -1.0f, 1.0f);
        const float new_az = nx * 180.0f;
        const float new_el = ny * 85.0f;
        const bool ch_a = params.set(ParamId::RenderKeyAzimuthDeg, static_cast<double>(new_az));
        const bool ch_e = params.set(ParamId::RenderKeyElevationDeg, static_cast<double>(new_el));
        if (ch_a || ch_e) {
            actions.viewport.lighting_dirty = true;
        }
    }

    if (orb_dragging_ && ImGui::IsItemDeactivatedAfterEdit()) {
        orb_dragging_ = false;
        undo.commit_drag(ParamId::RenderKeyAzimuthDeg, params.get(ParamId::RenderKeyAzimuthDeg));
    } else if (orb_dragging_ && !active) {
        orb_dragging_ = false;
        undo.cancel_drag();
    }

    // Keyboard nudge while hovered or focused (1 deg, 10 deg with Shift §12.6)
    if (hovered || ImGui::IsItemFocused()) {
        const float nudge = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
        float dax = 0.0f;
        float del = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            dax -= nudge;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            dax += nudge;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            del += nudge;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            del -= nudge;
        }
        if (dax != 0.0f) {
            const double old_v = params.get(ParamId::RenderKeyAzimuthDeg);
            if (params.set(ParamId::RenderKeyAzimuthDeg, old_v + dax)) {
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
    }

    // Draw Key (filled ●), Rim (hollow ○), Hair (diamond ◆) markers
    const float az_rad = params.get_float(ParamId::RenderKeyAzimuthDeg) * (kPi / 180.0f);
    const float el_rad = params.get_float(ParamId::RenderKeyElevationDeg) * (kPi / 180.0f);
    const float r_proj = radius * 0.78f * std::cos(el_rad);
    const ImVec2 key_pt(center.x + r_proj * std::sin(az_rad), center.y - radius * 0.78f * std::sin(el_rad));
    const ImVec2 rim_pt(center.x - r_proj * std::sin(az_rad) * 0.85f,
                        center.y + radius * 0.78f * std::sin(el_rad) * 0.65f);
    const ImVec2 hair_pt(center.x + 8.0f * dpi_scale, center.y - radius * 0.62f);

    dl->AddCircleFilled(key_pt, 5.0f * dpi_scale, t.col32_viz_t4);
    dl->AddCircle(key_pt, 5.5f * dpi_scale, t.col32_text_primary, 16, 1.2f);
    dl->AddCircle(rim_pt, 4.5f * dpi_scale, t.col32_accent, 16, 1.5f);
    dl->AddQuadFilled(ImVec2(hair_pt.x, hair_pt.y - 4.0f), ImVec2(hair_pt.x + 4.0f, hair_pt.y),
                      ImVec2(hair_pt.x, hair_pt.y + 4.0f), ImVec2(hair_pt.x - 4.0f, hair_pt.y), t.col32_status_ok);

    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    char readout[64]{};
    std::snprintf(readout, sizeof(readout), "Key az %.0f° el %.0f° · %.2f EV",
                  static_cast<double>(params.get_float(ParamId::RenderKeyAzimuthDeg)),
                  static_cast<double>(params.get_float(ParamId::RenderKeyElevationDeg)),
                  static_cast<double>(params.get_float(ParamId::RenderKeyIntensity)));
    dl->AddText(ImVec2(orb_pos.x - 12.0f * dpi_scale, orb_pos.y + orb_px + 4.0f * dpi_scale), t.col32_text_secondary,
                readout);
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
}

void ViewportPanel::draw_confidence_heatmap_legend(const DevShellTrackingView& tracking, const UiFontSet& fonts,
                                                   float dpi_scale, ImVec2 pos) {
    const PrecisionDarkTokens& t = tokens();
    const float w = std::round(236.0f * dpi_scale);
    const float h = std::round(66.0f * dpi_scale);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), t.col32_bg_overlay, t.radius_popover_px * dpi_scale);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), t.col32_border_default, t.radius_popover_px * dpi_scale, 0,
                hairline_px(dpi_scale));

    if (fonts.caption != nullptr) {
        ImGui::PushFont(fonts.caption);
    }
    const float y0 = pos.y + 11.0f * dpi_scale;
    const float x0 = pos.x + 8.0f * dpi_scale;
    const float x1 = pos.x + 42.0f * dpi_scale;
    draw_styled_legend_segment(dl, ImVec2(x0, y0), ImVec2(x1, y0), t.col32_status_ok, 0, 2.0f);
    dl->AddText(ImVec2(x1 + 6.0f, y0 - 6.0f), t.col32_text_primary, "Tracked · w = 1.0");

    const float y1 = y0 + 18.0f * dpi_scale;
    draw_styled_legend_segment(dl, ImVec2(x0, y1), ImVec2(x1, y1), t.col32_status_warn, 1, 2.0f);
    dl->AddText(ImVec2(x1 + 6.0f, y1 - 6.0f), t.col32_text_secondary, "Interpolating · 0 < w < 1");

    const float y2 = y1 + 18.0f * dpi_scale;
    draw_styled_legend_segment(dl, ImVec2(x0, y2), ImVec2(x1, y2), t.col32_accent, 2, 2.0f);
    char proc_txt[64]{};
    std::snprintf(proc_txt, sizeof(proc_txt), "Procedural w=0  (L:%.0f%% R:%.0f%%)",
                  static_cast<double>(tracking.left_hand_confidence * 100.0f),
                  static_cast<double>(tracking.right_hand_confidence * 100.0f));
    dl->AddText(ImVec2(x1 + 6.0f, y2 - 6.0f), t.col32_text_secondary, proc_txt);
    if (fonts.caption != nullptr) {
        ImGui::PopFont();
    }
}

} // namespace aether::ui
