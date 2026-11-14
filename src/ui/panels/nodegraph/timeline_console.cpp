#include "ui/panels/nodegraph/timeline_console.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdio>

namespace aether::ui {

TimelineConsole::TimelineConsole() noexcept {
    for (size_t i = 0; i < kPlotHistory; ++i) {
        x_vals_[i] = static_cast<float>(i);
    }
}

void TimelineConsole::push_sample(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                                  const DevShellTrackingView& tracking, bool paused) noexcept {
    if (paused) {
        return;
    }
    const size_t idx = write_idx_ % kPlotHistory;
    hist_t1_[idx] = tracking.inference_ms;
    hist_t2_[idx] = telemetry.sim.latest_ms;
    hist_t3_[idx] = render_view.total_gpu_ms;
    hist_t4_[idx] =
        std::max(0.05f, telemetry.last_frame.motion_to_photon_ms() - (hist_t1_[idx] + hist_t2_[idx] + hist_t3_[idx]));
    hist_m2p_[idx] = telemetry.last_frame.motion_to_photon_ms();
    write_idx_ = (write_idx_ + 1U) % kPlotHistory;
}

void TimelineConsole::draw_console_tab(std::span<const LogRecord> logs, size_t log_head, size_t log_count,
                                       const UiFontSet& fonts, const char*& out_copied_notice) {
    const PrecisionDarkTokens& t = tokens();

    ImGui::Checkbox(tr("Info"), &filter_info_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Warn"), &filter_warn_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Error"), &filter_err_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Auto-scroll"), &auto_scroll_);

    ImGui::BeginChild("##ConsoleRows", ImVec2(0.0f, 0.0f), true);
    if (log_count == 0) {
        (void)draw_empty_state(fonts, tr("No messages"));
        ImGui::EndChild();
        return;
    }

    static std::array<size_t, 256> s_indices{};
    size_t filtered_n = 0;
    const size_t ring_cap = logs.size();
    const size_t start_idx = (log_head + ring_cap - log_count) % ring_cap;
    for (size_t i = 0; i < log_count && filtered_n < s_indices.size(); ++i) {
        const size_t real_idx = (start_idx + i) % ring_cap;
        const LogLevel lvl = logs[real_idx].level;
        const bool pass = ((lvl == LogLevel::Info || lvl == LogLevel::Debug) && filter_info_) ||
                          (lvl == LogLevel::Warn && filter_warn_) || (lvl == LogLevel::Error && filter_err_);
        if (pass) {
            s_indices[filtered_n++] = real_idx;
        }
    }

    if (filtered_n == 0) {
        (void)draw_empty_state(fonts, tr("No messages"));
        ImGui::EndChild();
        return;
    }

    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered_n));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogRecord& rec = logs[s_indices[static_cast<size_t>(row)]];
            const ImVec4 col = (rec.level == LogLevel::Error)
                                   ? t.status_error
                                   : (rec.level == LogLevel::Warn ? t.status_warn : t.text_secondary);
            char line[360]{};
            std::snprintf(line, sizeof(line), "[%s] %s", rec.category, rec.message);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(line, false)) {
                ImGui::SetClipboardText(line);
                out_copied_notice = tr("Copied log line");
            }
            ImGui::PopStyleColor();
        }
    }
    if (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
    ImGui::EndChild();
}

void TimelineConsole::draw_expressions_tab(std::span<ExpressionSlotInfo> expressions, const UiFontSet& fonts,
                                           StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    if (ImGui::BeginTable("##ExprMatrix", 4,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Hotkey"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(tr("Expression"), ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn(tr("Live Weight"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Trigger"), ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < expressions.size() && i < 8; ++i) {
            ExpressionSlotInfo& ex = expressions[i];
            ImGui::PushID(static_cast<int>(ex.slot));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (fonts.mono != nullptr) {
                ImGui::PushFont(fonts.mono);
            }
            ImGui::TextColored(t.text_secondary, "%s", ex.hotkey.data());
            if (fonts.mono != nullptr) {
                ImGui::PopFont();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(t.text_primary, "%s", ex.name.data());

            ImGui::TableSetColumnIndex(2);
            ImGui::ProgressBar(std::clamp(ex.current_weight, 0.0f, 1.0f), ImVec2(-1.0f, 14.0f));

            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Button, ex.triggered_override ? t.accent_button : t.bg_raised);
            if (ImGui::SmallButton(ex.triggered_override ? tr("Release") : tr("Fire"))) {
                actions.request_trigger_expression = true;
                actions.expression_slot = static_cast<int32_t>(ex.slot);
                actions.expression_weight = ex.triggered_override ? 0.0f : 1.0f;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void TimelineConsole::draw_timeline_tab(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                                        const DevShellTrackingView& tracking, ParamStore& params,
                                        const UiFontSet& fonts) {
    const PrecisionDarkTokens& t = tokens();

    bool paused = params.get_bool(ParamId::AppTelemetryPaused);
    if (ImGui::SmallButton(paused ? icons::kPlay : icons::kPause)) {
        (void)params.set(ParamId::AppTelemetryPaused, paused ? 0.0 : 1.0);
    }
    ImGui::SameLine();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::TextColored(t.text_secondary,
                       "T1 NPU %.2f ms  |  T2 Sim %.2f ms  |  T3 GPU %.2f ms  |  M2P p50 %.2f / p95 %.2f ms",
                       static_cast<double>(tracking.inference_ms), static_cast<double>(telemetry.sim.latest_ms),
                       static_cast<double>(render_view.total_gpu_ms), static_cast<double>(telemetry.m2p.p50_ms),
                       static_cast<double>(telemetry.m2p.p95_ms));
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }

    if (ImPlot::GetCurrentContext() != nullptr) {
        ImPlot::PushStyleColor(ImPlotCol_PlotBg, t.bg_inset);
        ImPlot::PushStyleColor(ImPlotCol_FrameBg, t.bg_panel);
        if (ImPlot::BeginPlot("##M2PTimelinePlot", ImVec2(-1.0f, -1.0f), ImPlotFlags_NoTitle | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kPlotHistory - 1), ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 24.0, ImPlotCond_Once);

            const int off = static_cast<int>(write_idx_ % kPlotHistory);
            ImPlot::PushStyleColor(ImPlotCol_Line, t.viz_t1);
            ImPlot::PlotLine("T1 NPU", hist_t1_.data(), static_cast<int>(kPlotHistory), 1.0, 0.0, 0, off);
            ImPlot::PopStyleColor();

            ImPlot::PushStyleColor(ImPlotCol_Line, t.viz_t2);
            ImPlot::PlotLine("T2 Sim", hist_t2_.data(), static_cast<int>(kPlotHistory), 1.0, 0.0, 0, off);
            ImPlot::PopStyleColor();

            ImPlot::PushStyleColor(ImPlotCol_Line, t.viz_t3);
            ImPlot::PlotLine("T3 Render", hist_t3_.data(), static_cast<int>(kPlotHistory), 1.0, 0.0, 0, off);
            ImPlot::PopStyleColor();

            ImPlot::PushStyleColor(ImPlotCol_Line, t.viz_t4);
            ImPlot::PlotLine("M2P Total", hist_m2p_.data(), static_cast<int>(kPlotHistory), 1.0, 0.0, 0, off);
            ImPlot::PopStyleColor();

            double budget_y = 20.0;
            ImPlot::PushStyleColor(ImPlotCol_Line, t.status_error);
            ImPlot::PlotInfLines("20 ms Budget", &budget_y, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::PopStyleColor();

            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor(2);
    }
}

} // namespace aether::ui
