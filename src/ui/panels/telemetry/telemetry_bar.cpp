#include "ui/panels/telemetry/telemetry_bar.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace aether::ui {

TopMenuAction TelemetryBar::draw_top_bar(StudioMode& mode, bool& mode_changed, const rhi::RhiDevice& rhi,
                                         bool ffi_layout_ok, const StudioHostStatus& host_status, const UndoStack& undo,
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
            char undo_lbl[80]{};
            std::snprintf(undo_lbl, sizeof(undo_lbl), "Undo %s", undo.undo_label());
            if (ImGui::MenuItem(undo_lbl, "Ctrl+Z", false, undo.can_undo())) {
                // Triggered cleanly via Actions / Keymap
            }
            if (ImGui::MenuItem(tr("Preferences..."), "Ctrl+,")) {
                menu_act = TopMenuAction::OpenPreferences;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("View"))) {
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
        const auto mode_tab = [&](StudioMode m, const char* label) {
            const bool active = (mode == m);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? t.bg_selected : t.bg_app);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? t.text_primary : t.text_secondary);
            if (ImGui::Button(label, ImVec2(96.0f * scale, t.control_h_compact_px * scale))) {
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
            ImGui::PopStyleColor(2);
        };

        mode_tab(StudioMode::LiveStudio, tr("Live Studio"));
        ImGui::SameLine(0.0f, 4.0f * scale);
        mode_tab(StudioMode::Rigging, tr("Rigging"));
        ImGui::SameLine(0.0f, 4.0f * scale);
        mode_tab(StudioMode::Nodes, tr("Nodes"));

        // Right-aligned Palette trigger + Project dirty indicator + ON AIR pill (§12.4)
        const float right_pad = 250.0f * scale;
        const float bar_w = ImGui::GetWindowWidth();
        if (ImGui::GetCursorPosX() < bar_w - right_pad) {
            ImGui::SetCursorPosX(bar_w - right_pad);
        }

        ImGui::PushStyleColor(ImGuiCol_Button, t.bg_inset);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_tertiary);
        if (ImGui::Button(tr("Search Ctrl+K"), ImVec2(118.0f * scale, t.control_h_compact_px * scale))) {
            menu_act = TopMenuAction::OpenPalette;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 6.0f * scale);

        ImGui::PushStyleColor(ImGuiCol_Button, host_status.on_air ? t.status_live : t.bg_raised);
        ImGui::PushStyleColor(ImGuiCol_Text, host_status.on_air ? t.text_on_accent : t.text_secondary);
        if (ImGui::Button(host_status.on_air ? "ON AIR##oa" : "Go Live##oa",
                          ImVec2(92.0f * scale, t.control_h_compact_px * scale))) {
            actions.request_set_on_air = true;
            actions.target_on_air = !host_status.on_air;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Lock destructive avatar/wizard actions while broadcasting (Ctrl+Shift+L)"));
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
        std::snprintf(m2p_buf_.data(), m2p_buf_.size(), "M2P p50 %.1f / p95 %.1fms",
                      static_cast<double>(telemetry.m2p.p50_ms), static_cast<double>(telemetry.m2p.p95_ms));
        std::snprintf(dsp_buf_.data(), dsp_buf_.size(), "DSP %.1fms",
                      static_cast<double>(output_view.dsp_recommended_delay_ms));
        std::snprintf(ui_buf_.data(), ui_buf_.size(), "UI %.3fms | Arena+%zuB",
                      static_cast<double>(telemetry.ui_cpu.p95_ms), arena.steady_bump_growth_bytes());

        const bool raw_over =
            (telemetry.m2p.p95_ms > 20.0f) || (render_view.total_gpu_ms > 4.2f) || (telemetry.ui_cpu.p95_ms > 0.4f);
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
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
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
