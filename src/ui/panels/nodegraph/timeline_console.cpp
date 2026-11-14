#include "ui/panels/nodegraph/timeline_console.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

bool icase_contains(const char* haystack, const char* needle) noexcept {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    const size_t nlen = std::strlen(needle);
    for (const char* p = haystack; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

const char* level_tag(LogLevel lvl) noexcept {
    switch (lvl) {
    case LogLevel::Error:
        return "ERR ";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Debug:
        return "DBG ";
    default:
        return "INFO";
    }
}

} // namespace

TimelineConsole::TimelineConsole() noexcept = default;

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
    sample_count_ = std::min(sample_count_ + 1U, kPlotHistory);
}

ConsoleTabResult TimelineConsole::draw_console_tab(std::span<const LogRecord> logs, size_t log_head, size_t log_count,
                                                   const UiFontSet& fonts) {
    const PrecisionDarkTokens& t = tokens();
    ConsoleTabResult result{};

    // Toolbar: level filters | text filter | auto-scroll | Copy | Clear
    ImGui::Checkbox(tr("Info"), &filter_info_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Warn"), &filter_warn_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Error"), &filter_err_);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Auto-scroll"), &auto_scroll_);
    ImGui::SameLine();
    const float btns_w = ImGui::CalcTextSize("CopyClear").x + ImGui::GetStyle().FramePadding.x * 4.0f +
                         ImGui::GetStyle().ItemSpacing.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - btns_w));
    ImGui::InputTextWithHint("##ConsoleFilter", tr("Filter messages"), text_filter_.data(), text_filter_.size());

    // Collect visible rows (bounded, allocation-free)
    size_t filtered_n = 0;
    const size_t ring_cap = logs.size();
    if (ring_cap > 0 && log_count > 0) {
        const size_t start_idx = (log_head + ring_cap - log_count) % ring_cap;
        for (size_t i = 0; i < log_count && filtered_n < filtered_rows_.size(); ++i) {
            const size_t real_idx = (start_idx + i) % ring_cap;
            const LogRecord& rec = logs[real_idx];
            const bool level_pass = ((rec.level == LogLevel::Info || rec.level == LogLevel::Debug) && filter_info_) ||
                                    (rec.level == LogLevel::Warn && filter_warn_) ||
                                    (rec.level == LogLevel::Error && filter_err_);
            if (level_pass && (icase_contains(rec.message, text_filter_.data()) ||
                               icase_contains(rec.category, text_filter_.data()))) {
                filtered_rows_[filtered_n++] = real_idx;
            }
        }
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(filtered_n == 0);
    if (ImGui::Button(tr("Copy"))) {
        ImGui::LogToClipboard();
        for (size_t r = 0; r < filtered_n; ++r) {
            const LogRecord& rec = logs[filtered_rows_[r]];
            ImGui::LogText("[%s] [%s] %s\n", level_tag(rec.level), rec.category, rec.message);
        }
        ImGui::LogFinish();
        result.copied_notice = tr("Copied visible log lines");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("Copy all visible (filtered) lines"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(log_count == 0);
    if (ImGui::Button(tr("Clear"))) {
        result.clear_requested = true;
    }
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bg_inset);
    ImGui::BeginChild("##ConsoleRows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (filtered_n == 0) {
        (void)draw_empty_state(fonts, log_count == 0 ? tr("No messages") : tr("No messages match the filters"));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return result;
    }

    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered_n));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogRecord& rec = logs[filtered_rows_[static_cast<size_t>(row)]];
            const ImVec4 col = (rec.level == LogLevel::Error)
                                   ? t.status_error
                                   : (rec.level == LogLevel::Warn ? t.status_warn : t.text_secondary);
            char line[384]{};
            std::snprintf(line, sizeof(line), "%s  [%s] %s##log%d", level_tag(rec.level), rec.category, rec.message,
                          row);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(line, false)) {
                char clip[360]{};
                std::snprintf(clip, sizeof(clip), "[%s] %s", rec.category, rec.message);
                ImGui::SetClipboardText(clip);
                result.copied_notice = tr("Copied log line");
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
    ImGui::PopStyleColor();
    return result;
}

void TimelineConsole::draw_expressions_tab(std::span<ExpressionSlotInfo> expressions,
                                           const DevShellTrackingView& tracking, const UiFontSet& fonts) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = ImGui::GetFontSize() / std::max(1.0f, t.type_body_px);

    if (ImGui::BeginTable("##ExprSplit", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##HotkeysCol", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("##Live2DCol", ImGuiTableColumnFlags_WidthStretch, 0.68f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginTable("##ExprMatrix", 4,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr("Hotkey"), ImGuiTableColumnFlags_WidthFixed, 52.0f * scale);
            ImGui::TableSetupColumn(tr("Expression"), ImGuiTableColumnFlags_WidthFixed, 120.0f * scale);
            ImGui::TableSetupColumn(tr("Live Weight"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr("Trigger"), ImGuiTableColumnFlags_WidthFixed, 76.0f * scale);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < expressions.size() && i < StudioActions::kQuickExpressionSlots; ++i) {
                ExpressionSlotInfo& ex = expressions[i];
                ImGui::PushID(static_cast<int>(ex.slot));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (fonts.mono != nullptr) {
                    ImGui::PushFont(fonts.mono);
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(t.text_secondary, "%s", ex.hotkey.data());
                if (fonts.mono != nullptr) {
                    ImGui::PopFont();
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(t.text_primary, "%s", ex.name.data());

                ImGui::TableSetColumnIndex(2);
                char pct[16]{};
                std::snprintf(pct, sizeof(pct), "%.0f%%", static_cast<double>(ex.current_weight * 100.0f));
                ImGui::ProgressBar(std::clamp(ex.current_weight, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), pct);

                ImGui::TableSetColumnIndex(3);
                const bool latched = ex.triggered_override;
                ImGui::PushStyleColor(ImGuiCol_Button, latched ? t.accent_button : t.bg_raised);
                ImGui::PushStyleColor(ImGuiCol_Text, latched ? t.text_on_accent : t.text_primary);
                if (ImGui::Button(latched ? tr("Release") : tr("Latch"), ImVec2(-FLT_MIN, 0.0f))) {
                    ex.triggered_override = !ex.triggered_override;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::TableSetColumnIndex(1);
        const auto& l2d = tracking.live2d;
        const bool l2d_active = tracking.ready && tracking.face_confidence > 0.0f;
        ImGui::TextDisabled("Live2D Parameter Pipeline (%s)", l2d_active ? "ACTIVE" : "IDLE");

        struct ParamCell {
            const char* name;
            float value;
            float min_val;
            float max_val;
            ImU32 color;
        };
        const ImU32 col_angle = IM_COL32(82, 168, 255, 210);
        const ImU32 col_eye = IM_COL32(72, 224, 232, 210);
        const ImU32 col_brow = IM_COL32(250, 206, 72, 210);
        const ImU32 col_mouth = IM_COL32(255, 110, 196, 210);
        const ImU32 col_vowel = IM_COL32(120, 232, 152, 220);

        constexpr size_t kRows = 8U;
        const ParamCell col0[kRows] = {
            {"ParamAngleX", l2d.param_angle_x, -30.0f, 30.0f, col_angle},
            {"ParamAngleY", l2d.param_angle_y, -30.0f, 30.0f, col_angle},
            {"ParamAngleZ", l2d.param_angle_z, -30.0f, 30.0f, col_angle},
            {"ParamBodyAngleX", l2d.param_body_angle_x, -10.0f, 10.0f, col_angle},
            {"ParamBodyAngleY", l2d.param_body_angle_y, -10.0f, 10.0f, col_angle},
            {"ParamBodyAngleZ", l2d.param_body_angle_z, -10.0f, 10.0f, col_angle},
            {"ParamEyeBallX", l2d.param_eye_ball_x, -1.0f, 1.0f, col_eye},
            {"ParamEyeBallY", l2d.param_eye_ball_y, -1.0f, 1.0f, col_eye},
        };
        const ParamCell col1[kRows] = {
            {"ParamEyeLOpen", l2d.param_eye_l_open, 0.0f, 1.0f, col_eye},
            {"ParamEyeROpen", l2d.param_eye_r_open, 0.0f, 1.0f, col_eye},
            {"ParamBrowLY", l2d.param_brow_l_y, -1.0f, 1.0f, col_brow},
            {"ParamBrowRY", l2d.param_brow_r_y, -1.0f, 1.0f, col_brow},
            {"ParamBrowLAngle", l2d.param_brow_l_angle, -1.0f, 1.0f, col_brow},
            {"ParamBrowRAngle", l2d.param_brow_r_angle, -1.0f, 1.0f, col_brow},
            {"ParamBrowLForm", l2d.param_brow_l_form, -1.0f, 1.0f, col_brow},
            {"ParamBrowRForm", l2d.param_brow_r_form, -1.0f, 1.0f, col_brow},
        };
        const ParamCell col2[kRows] = {
            {"ParamMouthOpenY", l2d.param_mouth_open_y, 0.0f, 1.0f, col_mouth},
            {"ParamMouthForm", l2d.param_mouth_form, -1.0f, 1.0f, col_mouth},
            {"Vowel A", l2d.vowel_a, 0.0f, 1.0f, col_vowel},
            {"Vowel I", l2d.vowel_i, 0.0f, 1.0f, col_vowel},
            {"Vowel U", l2d.vowel_u, 0.0f, 1.0f, col_vowel},
            {"Vowel E", l2d.vowel_e, 0.0f, 1.0f, col_vowel},
            {"Vowel O", l2d.vowel_o, 0.0f, 1.0f, col_vowel},
            {nullptr, 0.0f, 0.0f, 1.0f, col_vowel},
        };

        auto draw_cell = [](int base_col, const ParamCell& c) {
            if (c.name == nullptr) {
                return;
            }
            const float span = (c.max_val > c.min_val) ? (c.max_val - c.min_val) : 1.0f;
            const float frac = std::clamp((c.value - c.min_val) / span, 0.0f, 1.0f);
            char overlay[16]{};
            std::snprintf(overlay, sizeof(overlay), "%.2f", static_cast<double>(c.value));
            ImGui::TableSetColumnIndex(base_col);
            ImGui::TextUnformatted(c.name);
            ImGui::TableSetColumnIndex(base_col + 1);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c.color);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 13.0f * ImGui::GetFontSize() / 13.0f), overlay);
            ImGui::PopStyleColor();
        };

        if (ImGui::BeginTable("##Live2DParamsTable", 6,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Head / Gaze", ImGuiTableColumnFlags_WidthFixed, 118.0f * scale);
            ImGui::TableSetupColumn("Val##0", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Eyes / Brows", ImGuiTableColumnFlags_WidthFixed, 118.0f * scale);
            ImGui::TableSetupColumn("Val##1", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Mouth / Vowel", ImGuiTableColumnFlags_WidthFixed, 118.0f * scale);
            ImGui::TableSetupColumn("Val##2", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t r = 0; r < kRows; ++r) {
                ImGui::TableNextRow();
                draw_cell(0, col0[r]);
                draw_cell(2, col1[r]);
                draw_cell(4, col2[r]);
            }
            ImGui::EndTable();
        }

        ImGui::EndTable();
    }
}

void TimelineConsole::draw_timeline_tab(const TelemetrySnapshot& telemetry, const DevShellRenderView& render_view,
                                        const DevShellTrackingView& tracking, ParamStore& params,
                                        const UiFontSet& fonts) {
    const PrecisionDarkTokens& t = tokens();

    const bool paused = params.get_bool(ParamId::AppTelemetryPaused);
    if (ImGui::SmallButton(paused ? icons::kPlay : icons::kPause)) {
        (void)params.set(ParamId::AppTelemetryPaused, paused ? 0.0 : 1.0);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", paused ? tr("Resume live capture") : tr("Freeze the timeline for inspection"));
    }
    ImGui::SameLine();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    const size_t last = (write_idx_ + kPlotHistory - 1U) % kPlotHistory;
    ImGui::TextColored(t.viz_t1, "T1 NPU %.2f", static_cast<double>(tracking.inference_ms));
    ImGui::SameLine();
    ImGui::TextColored(t.viz_t2, "T2 Sim %.2f", static_cast<double>(telemetry.sim.latest_ms));
    ImGui::SameLine();
    ImGui::TextColored(t.viz_t3, "T3 GPU %.2f", static_cast<double>(render_view.total_gpu_ms));
    ImGui::SameLine();
    ImGui::TextColored(t.viz_t4, "T4 Out %.2f", static_cast<double>(sample_count_ > 0 ? hist_t4_[last] : 0.0f));
    ImGui::SameLine();
    ImGui::TextColored(t.text_secondary, "| M2P p50 %.2f / p95 %.2f ms", static_cast<double>(telemetry.m2p.p50_ms),
                       static_cast<double>(telemetry.m2p.p95_ms));
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }

    if (ImPlot::GetCurrentContext() == nullptr) {
        return;
    }
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, t.bg_inset);
    ImPlot::PushStyleColor(ImPlotCol_FrameBg, t.bg_panel);
    ImPlot::PushStyleColor(ImPlotCol_LegendBg, t.bg_overlay);
    if (ImPlot::BeginPlot("##M2PTimelinePlot", ImVec2(-1.0f, -1.0f), ImPlotFlags_NoTitle | ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(kPlotHistory - 1), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 24.0, ImPlotCond_Once);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

        const int count = static_cast<int>(kPlotHistory);
        const int off = static_cast<int>(write_idx_ % kPlotHistory);
        const auto line = [&](const char* name, const ImVec4& col, const std::array<float, kPlotHistory>& data) {
            ImPlot::PushStyleColor(ImPlotCol_Line, col);
            ImPlot::PlotLine(name, data.data(), count, 1.0, 0.0, 0, off);
            ImPlot::PopStyleColor();
        };
        line("T1 NPU", t.viz_t1, hist_t1_);
        line("T2 Sim/IK", t.viz_t2, hist_t2_);
        line("T3 GPU", t.viz_t3, hist_t3_);
        line("T4 Output", t.viz_t4, hist_t4_);
        line("M2P Total", t.text_secondary, hist_m2p_);

        const double budget_y = 20.0;
        ImPlot::PushStyleColor(ImPlotCol_Line, t.status_error);
        ImPlot::PlotInfLines("20 ms Budget", &budget_y, 1, ImPlotInfLinesFlags_Horizontal);
        ImPlot::PopStyleColor();
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleColor(3);
}

} // namespace aether::ui
