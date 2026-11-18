#include "ui/panels/telemetry/telemetry_bar.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::ui {

TopMenuAction TelemetryBar::draw_top_bar(double now_s, StudioMode& mode, bool& mode_changed, const rhi::RhiDevice& rhi,
                                         bool ffi_layout_ok, const StudioHostStatus& host_status, const UndoStack& undo,
                                         const HardwareInventory& hw, const DevShellTrackingView& tracking,
                                         ParamStore& params, AppSettings& settings, bool& settings_dirty,
                                         const UiFontSet& fonts, float dpi_scale, StudioActions& actions) {
    TopMenuAction menu_act = TopMenuAction::None;
    mode_changed = false;
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(t.panel_pad_px * scale, 7.0f * scale));
    if (ImGui::BeginMainMenuBar()) {
        if (fonts.body_strong != nullptr) {
            ImGui::PushFont(fonts.body_strong);
        }
        ImGui::TextColored(t.text_primary, "%s", tr("AETHER"));
        if (fonts.body_strong != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Separator();

        if (ImGui::BeginMenu(tr("File"))) {
            if (ImGui::MenuItem(tr("New Project"), "Ctrl+N", false, !host_status.on_air)) {
                menu_act = TopMenuAction::NewProject;
            }
            if (ImGui::MenuItem(tr("Open Project..."), "Ctrl+O", false, !host_status.on_air)) {
                menu_act = TopMenuAction::OpenProject;
            }
            if (ImGui::MenuItem(tr("Save Project"), "Ctrl+S")) {
                menu_act = TopMenuAction::SaveProject;
            }
            if (ImGui::MenuItem(tr("Save Project As..."), "Ctrl+Shift+S")) {
                menu_act = TopMenuAction::SaveProjectAs;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Import Avatar..."), "Ctrl+I", false, !host_status.on_air)) {
                menu_act = TopMenuAction::ImportAvatar;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Edit"))) {
            char undo_lbl[96]{};
            std::snprintf(undo_lbl, sizeof(undo_lbl), "Undo %s", undo.undo_label());
            if (ImGui::MenuItem(undo_lbl, "Ctrl+Z", false, undo.can_undo())) {
                menu_act = TopMenuAction::Undo;
            }
            char redo_lbl[96]{};
            std::snprintf(redo_lbl, sizeof(redo_lbl), "Redo %s", undo.redo_label());
            if (ImGui::MenuItem(redo_lbl, "Ctrl+Y", false, undo.can_redo())) {
                menu_act = TopMenuAction::Redo;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Preferences..."), "Ctrl+,")) {
                menu_act = TopMenuAction::OpenPreferences;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("View"))) {
            const bool hud_on = params.get_bool(ParamId::AppTrackingHudVisible);
            if (ImGui::MenuItem(tr("Tracking HUD"), nullptr, hud_on)) {
                (void)params.set(ParamId::AppTrackingHudVisible, hud_on ? 0.0 : 1.0);
                settings_dirty = true;
            }
            const bool orb_on = params.get_bool(ParamId::AppOrbitOrbVisible);
            if (ImGui::MenuItem(tr("Light Orb"), "Alt+L", orb_on)) {
                (void)params.set(ParamId::AppOrbitOrbVisible, orb_on ? 0.0 : 1.0);
                settings_dirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Command Palette..."), "Ctrl+K")) {
                menu_act = TopMenuAction::OpenPalette;
            }
            if (ImGui::MenuItem(tr("Keyboard Shortcuts..."), "Ctrl+/")) {
                menu_act = TopMenuAction::OpenShortcuts;
            }
            if (ImGui::MenuItem(tr("Reset Workspace Layout"))) {
                menu_act = TopMenuAction::ResetLayout;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Tracking"))) {
            char cam_status[192]{};
            format_camera_status_badge(tracking, cam_status, sizeof(cam_status));
            ImGui::TextColored(tracking.camera_active ? t.status_ok : t.text_tertiary, "%s", cam_status);
            ImGui::TextColored(t.text_tertiary, "%s · %.1f ms", tracking.active_ep_label,
                               static_cast<double>(tracking.inference_ms));
            ImGui::Separator();
            const bool cam_on = tracking.camera_active;
            if (ImGui::MenuItem(cam_on ? tr("Stop Camera") : tr("Start Camera"), nullptr, cam_on)) {
                actions.viewport.request_set_camera_active = true;
                actions.viewport.target_camera_active = !cam_on;
            }
            if (ImGui::BeginMenu(tr("Camera Device"))) {
                if (hw.cameras.empty()) {
                    ImGui::MenuItem(tr("No Webcam Detected"), nullptr, false, false);
                } else {
                    const int active_idx = active_camera_device_index(hw, tracking, settings);
                    for (int i = 0; i < static_cast<int>(hw.cameras.size()); ++i) {
                        const auto& cam = hw.cameras[static_cast<size_t>(i)];
                        char lbl[192]{};
                        std::snprintf(lbl, sizeof(lbl), "%s%s##cam_%d", cam.friendly_name.c_str(),
                                      cam.likely_virtual ? " [Virtual]" : "", i);
                        const bool selected = (i == active_idx) && !tracking.using_synthetic_camera;
                        if (ImGui::MenuItem(lbl, nullptr, selected)) {
                            select_camera_device_action(cam, params, settings, settings_dirty, actions);
                        }
                    }
                }
                ImGui::Separator();
                const bool syn_on = params.get_bool(ParamId::TrackingSyntheticCamera);
                if (ImGui::MenuItem(tr("Use Synthetic Feed"), nullptr, syn_on)) {
                    (void)params.set(ParamId::TrackingSyntheticCamera, syn_on ? 0.0 : 1.0);
                    actions.viewport.request_set_synthetic_camera = true;
                    actions.viewport.target_synthetic_camera = !syn_on;
                    settings_dirty = true;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            const bool hand_on = params.get_bool(ParamId::TrackingHandEnabled);
            if (ImGui::MenuItem(tr("Enable Hand Tracking"), nullptr, hand_on)) {
                const bool next_hand = !hand_on;
                (void)params.set(ParamId::TrackingHandEnabled, next_hand ? 1.0 : 0.0);
                actions.viewport.request_set_hand_tracking = true;
                actions.viewport.target_hand_tracking_enabled = next_hand;
                settings_dirty = true;
            }
            const bool hud_on = params.get_bool(ParamId::AppTrackingHudVisible);
            if (ImGui::MenuItem(tr("Tracking HUD"), nullptr, hud_on)) {
                (void)params.set(ParamId::AppTrackingHudVisible, hud_on ? 0.0 : 1.0);
                settings_dirty = true;
            }
            const bool orb_on = params.get_bool(ParamId::AppOrbitOrbVisible);
            if (ImGui::MenuItem(tr("Light Orb"), "Alt+L", orb_on)) {
                (void)params.set(ParamId::AppOrbitOrbVisible, orb_on ? 0.0 : 1.0);
                settings_dirty = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Hardware Optimizer..."), nullptr, false, !host_status.on_air)) {
                menu_act = TopMenuAction::OpenWizard;
            }
            if (ImGui::MenuItem(tr("Calibrate Neutral"), "Ctrl+Shift+K")) {
                actions.viewport.request_calibrate_neutral = true;
            }
            if (ImGui::MenuItem(tr("Pause Tracking"), "Ctrl+P", host_status.tracking_paused)) {
                actions.request_pause_tracking = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Graphics"))) {
            const auto& gpu = rhi.gpu_info();
            ImGui::TextColored(t.text_tertiary, "GPU: %s",
                               gpu.device_name.empty() ? tr("Unknown") : gpu.device_name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Graphics Settings..."))) {
                menu_act = TopMenuAction::OpenGraphicsSettings;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Target FPS"))) {
                const int cur_fps_mode = params.get_int(ParamId::RenderTargetFps);
                static constexpr const char* kFpsLabels[4] = {"30 FPS", "60 FPS (Default)", "120 FPS", "Uncapped"};
                for (int fi = 0; fi < 4; ++fi) {
                    if (ImGui::MenuItem(kFpsLabels[fi], nullptr, cur_fps_mode == fi)) {
                        (void)params.set(ParamId::RenderTargetFps, static_cast<double>(fi));
                        settings_dirty = true;
                    }
                }
                ImGui::EndMenu();
            }
            const bool vsync = params.get_bool(ParamId::RenderVsync);
            if (ImGui::MenuItem(tr("VSync"), nullptr, vsync)) {
                (void)params.set(ParamId::RenderVsync, vsync ? 0.0 : 1.0);
                settings_dirty = true;
            }
            const bool eco = params.get_bool(ParamId::RenderBackgroundEco);
            if (ImGui::MenuItem(tr("Background Eco (30 FPS)"), nullptr, eco)) {
                (void)params.set(ParamId::RenderBackgroundEco, eco ? 0.0 : 1.0);
                settings_dirty = true;
            }
            const bool rt_on = params.get_bool(ParamId::RenderRtEnabled);
            if (ImGui::MenuItem(tr("Ray Tracing"), nullptr, rt_on, rhi.rt_supported())) {
                (void)params.set(ParamId::RenderRtEnabled, rt_on ? 0.0 : 1.0);
                actions.viewport.lighting_dirty = true;
                settings_dirty = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Diagnostics"))) {
            ImGui::Text("FFI ABI: %s", ffi_layout_ok ? "VERIFIED" : "FAILED");
            ImGui::Text("VK Validation: %s (errors: %llu)", rhi.validation_active() ? "ON" : "OFF",
                        static_cast<unsigned long long>(rhi.validation_error_count()));
            if (ImGui::MenuItem(tr("Reload Plugins"))) {
                actions.request_reload_plugins = true;
            }
            ImGui::EndMenu();
        }

        // Centered 3-Mode Selector (§12.4)
        const float avail = ImGui::GetContentRegionAvail().x;
        const float cluster_w = 300.0f * scale;
        if (avail > cluster_w + 260.0f * scale) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - cluster_w - 240.0f * scale) * 0.45f);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float menu_h = ImGui::GetWindowHeight();
        const float btn_h = std::min(t.control_h_compact_px * scale, std::max(14.0f, menu_h - 4.0f * scale));
        const float btn_y = std::max(0.0f, (menu_h - btn_h) * 0.5f);

        const auto mode_tab = [&](StudioMode m, const char* label) {
            const bool active = (mode == m);
            ImGui::SetCursorPosY(btn_y);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? t.bg_selected : t.bg_app);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.bg_hover);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? t.text_primary : t.text_secondary);
            const float tab_w = std::max(96.0f * scale, ImGui::CalcTextSize(label).x + 24.0f * scale);
            if (ImGui::Button(label, ImVec2(tab_w, btn_h))) {
                if (mode != m) {
                    mode = m;
                    mode_changed = true;
                }
            }
            if (active) {
                const ImVec2 bmin = ImGui::GetItemRectMin();
                const ImVec2 bmax = ImGui::GetItemRectMax();
                dl->AddRectFilled(ImVec2(bmin.x, bmax.y - 2.0f * scale), bmax, t.col32_accent);
            }
            ImGui::PopStyleColor(3);
        };

        mode_tab(StudioMode::LiveStudio, tr("Live Studio"));
        ImGui::SameLine(0.0f, 4.0f * scale);
        mode_tab(StudioMode::Rigging, tr("Rigging"));
        ImGui::SameLine(0.0f, 4.0f * scale);
        mode_tab(StudioMode::Nodes, tr("Nodes"));

        // Right-aligned Palette trigger + Go Live tally (§12.4)
        if (host_status.on_air && on_air_since_s_ < 0.0) {
            on_air_since_s_ = now_s;
        } else if (!host_status.on_air) {
            on_air_since_s_ = -1.0;
        }
        char live_lbl[48]{};
        if (host_status.on_air) {
            const auto secs = static_cast<long long>(std::max(0.0, now_s - on_air_since_s_));
            std::snprintf(live_lbl, sizeof(live_lbl), "%s LIVE  %02lld:%02lld:%02lld###oa", icons::kRadioOn,
                          secs / 3600, (secs / 60) % 60, secs % 60);
        } else {
            std::snprintf(live_lbl, sizeof(live_lbl), "%s###oa", tr("Go Live"));
        }
        const float search_w = 132.0f * scale;
        const float live_w = std::max(112.0f * scale, ImGui::CalcTextSize("_ LIVE  00:00:00").x + 20.0f * scale);
        const float right_block = search_w + live_w + 6.0f * scale + t.panel_pad_px * scale;
        const float bar_w = ImGui::GetWindowWidth();
        if (ImGui::GetCursorPosX() < bar_w - right_block) {
            ImGui::SetCursorPosX(bar_w - right_block);
        }

        ImGui::SetCursorPosY(btn_y);
        ImGui::PushStyleColor(ImGuiCol_Button, t.bg_inset);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.bg_hover);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_tertiary);
        char search_lbl[48]{};
        std::snprintf(search_lbl, sizeof(search_lbl), "%s  %s", icons::kSearch, tr("Search  Ctrl+K"));
        if (ImGui::Button(search_lbl, ImVec2(search_w, btn_h))) {
            menu_act = TopMenuAction::OpenPalette;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, 6.0f * scale);

        ImGui::SetCursorPosY(btn_y);
        ImGui::PushStyleColor(ImGuiCol_Button, host_status.on_air ? t.status_live : t.bg_raised);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, host_status.on_air ? t.status_live : t.bg_hover);
        ImGui::PushStyleColor(ImGuiCol_Text, host_status.on_air ? t.text_on_accent : t.text_primary);
        if (ImGui::Button(live_lbl, ImVec2(live_w, btn_h))) {
            actions.request_set_on_air = true;
            actions.target_on_air = !host_status.on_air;
        }
        ImGui::PopStyleColor(3);
        if (host_status.on_air) {
            // Glowing tally halo, pulsing at 1 Hz
            const ImVec2 bmin = ImGui::GetItemRectMin();
            const ImVec2 bmax = ImGui::GetItemRectMax();
            const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now_s) * 6.2831853f);
            const auto alpha = static_cast<uint32_t>(70.0f + 90.0f * pulse);
            for (int i = 1; i <= 3; ++i) {
                const float g = static_cast<float>(i) * 1.5f * scale;
                ImGui::GetWindowDrawList()->AddRect(
                    ImVec2(bmin.x - g, bmin.y - g), ImVec2(bmax.x + g, bmax.y + g),
                    col32_with_alpha(t.col32_status_live, alpha / static_cast<uint32_t>(i)),
                    t.radius_control_px * scale + g, 0, 1.5f * scale);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", host_status.on_air
                                        ? tr("Stop broadcasting (Ctrl+Shift+L). Outputs keep their settings.")
                                        : tr("Start broadcasting: enables Spout2 + Virtual Cam and locks destructive "
                                             "edits (Ctrl+Shift+L)"));
        }

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    return menu_act;
}

void TelemetryBar::draw_bottom_bar(double now_s, const TelemetrySnapshot& telemetry, const ImGuiArenaAllocator& arena,
                                   const rhi::RhiDevice& rhi, const DevShellTrackingView& tracking,
                                   const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                                   const UiFontSet& fonts, float dpi_scale) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);

    // 4 Hz text refresh + 500 ms color hysteresis (§12.8)
    if (last_sample_s_ < 0.0 || (now_s - last_sample_s_) >= 0.25) {
        last_sample_s_ = now_s;
        std::snprintf(t1_buf_.data(), t1_buf_.size(), "T1 %s %.1fms", tracking.active_ep_label,
                      static_cast<double>(tracking.inference_ms));
        std::snprintf(t2_buf_.data(), t2_buf_.size(), "T2 Sim %.2fms", static_cast<double>(telemetry.sim.latest_ms));
        std::snprintf(t3_buf_.data(), t3_buf_.size(), "T3 %.0ffps GPU %.1fms",
                      static_cast<double>(telemetry.render_fps), static_cast<double>(render_view.total_gpu_ms));
        std::snprintf(t4_buf_.data(), t4_buf_.size(), "T4 Spout:%llu VCam:%llu",
                      static_cast<unsigned long long>(output_view.spout_frames_published),
                      static_cast<unsigned long long>(output_view.vcam_frames_published));
        const bool m2p_valid = (telemetry.m2p.sample_count > 0U) && (telemetry.m2p.p50_ms > 0.0f) &&
                               (telemetry.m2p.p95_ms <= 5000.0f);
        if (m2p_valid) {
            std::snprintf(m2p_buf_.data(), m2p_buf_.size(), "M2P p50 %.1f / p95 %.1fms",
                          static_cast<double>(telemetry.m2p.p50_ms), static_cast<double>(telemetry.m2p.p95_ms));
        } else {
            std::snprintf(m2p_buf_.data(), m2p_buf_.size(), "M2P -- / -- ms");
        }
        std::snprintf(dsp_buf_.data(), dsp_buf_.size(), "DSP %.1fms",
                      static_cast<double>(output_view.dsp_recommended_delay_ms));
        std::snprintf(ui_buf_.data(), ui_buf_.size(), "UI %.3fms | Arena+%zuB",
                      static_cast<double>(telemetry.ui_cpu.p95_ms), arena.steady_bump_growth_bytes());

        const bool raw_over =
            (m2p_valid && telemetry.m2p.p95_ms > 20.0f) || (render_view.total_gpu_ms > 4.2f) ||
            (telemetry.ui_cpu.p95_ms > 0.4f);
        if (raw_over) {
            if (over_budget_since_s_ < 0.0) {
                over_budget_since_s_ = now_s;
            }
            ok_since_s_ = -1.0;
            if ((now_s - over_budget_since_s_) >= 0.5) {
                hysteresis_over_budget_ = true;
            }
        } else {
            if (ok_since_s_ < 0.0) {
                ok_since_s_ = now_s;
            }
            over_budget_since_s_ = -1.0;
            if ((now_s - ok_since_s_) >= 0.5) {
                hysteresis_over_budget_ = false;
            }
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bar_h = std::round(t.status_bar_h_px * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(t.panel_pad_px * scale, 3.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_app);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##TelemetryStatusBar", nullptr, flags)) {
        if (fonts.mono != nullptr) {
            ImGui::PushFont(fonts.mono);
        }

        ImGui::TextColored(t.viz_t1, "%s", t1_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        ImGui::TextColored(t.viz_t2, "%s", t2_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        ImGui::TextColored(t.viz_t3, "%s", t3_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        ImGui::TextColored(t.viz_t4, "%s", t4_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        ImGui::TextColored(hysteresis_over_budget_ ? t.status_warn : t.status_ok, "%s", m2p_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        ImGui::TextColored(output_view.dsp_connected ? t.status_ok : t.text_secondary, "%s", dsp_buf_.data());
        ImGui::SameLine();
        ImGui::TextColored(t.text_disabled, "|");
        ImGui::SameLine();
        if (ImGui::SmallButton(ui_buf_.data())) {
            ImGui::OpenPopup("##PerfPopover");
        }
        ImGui::SameLine();
        ImGui::TextColored(rhi.validation_error_count() == 0 ? t.status_ok : t.status_error, "VK Err:%llu",
                           static_cast<unsigned long long>(rhi.validation_error_count()));

        if (ImGui::BeginPopup("##PerfPopover")) {
            ImGui::TextColored(t.text_primary, "%s", tr("Detailed Frame Budget Breakdown"));
            ImGui::Separator();
            ImGui::Text("RT Shadows/Hair: %.2f ms  |  Denoiser (%s): %.2f ms (p95 %.2f)",
                        static_cast<double>(render_view.rt_gpu_ms), render_view.denoiser_active_name,
                        static_cast<double>(render_view.denoise_gpu_ms),
                        static_cast<double>(render_view.denoise_p95_120f_ms));
            ImGui::Text("NPR Passes: %.2f ms  |  Post: %.2f ms  |  Output Encode: %.2f ms",
                        static_cast<double>(render_view.npr_gpu_ms), static_cast<double>(render_view.post_gpu_ms),
                        static_cast<double>(render_view.encode_gpu_ms));
            ImGui::Text("Present Mode: %s  |  Spout Mutex Skipped: %llu  |  Demotions: %llu",
                        telemetry.present_mode[0] != '\0' ? telemetry.present_mode : "FIFO",
                        static_cast<unsigned long long>(output_view.spout_skipped_mutex),
                        static_cast<unsigned long long>(tracking.demotion_count));
            ImGui::Text("ImGui Arena: used %zu KiB / %zu MiB | steady growth %zu B | fallback %llu",
                        arena.used_bytes() / size_t{1024}, arena.capacity_bytes() / (size_t{1024} * size_t{1024}),
                        arena.steady_bump_growth_bytes(), static_cast<unsigned long long>(arena.os_fallback_allocs()));
            ImGui::EndPopup();
        }

        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace aether::ui
