#include "ui/widgets/feedback.hpp"

#include "ui/theme/anim.hpp"
#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

const char* severity_icon(ToastSeverity s) noexcept {
    switch (s) {
    case ToastSeverity::Success:
        return icons::kCheck;
    case ToastSeverity::Warning:
        return icons::kAlertTriangle;
    case ToastSeverity::Error:
        return icons::kErrorCircle;
    case ToastSeverity::Info:
        return icons::kInfo;
    }
    return icons::kInfo;
}

ImU32 severity_col32(const PrecisionDarkTokens& t, ToastSeverity s) noexcept {
    switch (s) {
    case ToastSeverity::Success:
        return t.col32_status_ok;
    case ToastSeverity::Warning:
        return t.col32_status_warn;
    case ToastSeverity::Error:
        return t.col32_status_error;
    case ToastSeverity::Info:
        return t.col32_accent;
    }
    return t.col32_accent;
}

} // namespace

void ToastStack::push(ToastSeverity sev, const char* title, const char* detail, double now_s, ToastActionKind action,
                      const char* action_label) noexcept {
    size_t target = 0;
    bool found = false;
    for (size_t i = 0; i < kMaxToasts; ++i) {
        if (!items_[i].active) {
            target = i;
            found = true;
            break;
        }
    }
    if (!found) {
        // Evict oldest non-error, or slot 0
        for (size_t i = 0; i < kMaxToasts; ++i) {
            if (items_[i].severity != ToastSeverity::Error) {
                target = i;
                found = true;
                break;
            }
        }
    }

    ToastItem& item = items_[target];
    item = ToastItem{};
    item.active = true;
    item.severity = sev;
    item.action_kind = action;
    item.id = next_id_++;
    if (sev == ToastSeverity::Info || sev == ToastSeverity::Success) {
        item.expires_at_s = now_s + 5.0;
    } else if (sev == ToastSeverity::Warning) {
        item.expires_at_s = now_s + 8.0;
    } else {
        item.expires_at_s = 0.0; // Errors remain sticky until dismissed (§12.13)
    }

    std::snprintf(item.title.data(), item.title.size(), "%s", title != nullptr ? title : "");
    std::snprintf(item.detail.data(), item.detail.size(), "%s", detail != nullptr ? detail : "");
    if (action_label != nullptr) {
        std::snprintf(item.action_label.data(), item.action_label.size(), "%s", action_label);
    } else if (action == ToastActionKind::Undo) {
        std::snprintf(item.action_label.data(), item.action_label.size(), "%s", tr("Undo"));
    } else if (action == ToastActionKind::Retry) {
        std::snprintf(item.action_label.data(), item.action_label.size(), "%s", tr("Retry"));
    } else if (action == ToastActionKind::Details) {
        std::snprintf(item.action_label.data(), item.action_label.size(), "%s", tr("Details"));
    }
}

void ToastStack::dismiss(uint32_t toast_id) noexcept {
    for (auto& item : items_) {
        if (item.active && item.id == toast_id) {
            item.active = false;
            break;
        }
    }
}

void ToastStack::clear() noexcept {
    for (auto& item : items_) {
        item.active = false;
    }
}

size_t ToastStack::active_count() const noexcept {
    size_t n = 0;
    for (const auto& item : items_) {
        if (item.active) {
            ++n;
        }
    }
    return n;
}

ToastActionKind ToastStack::draw(double now_s, const UiFontSet& fonts, float dpi_scale, bool /*reduce_motion*/) {
    ToastActionKind triggered = ToastActionKind::None;
    for (auto& item : items_) {
        if (item.active && item.expires_at_s > 0.0 && now_s >= item.expires_at_s) {
            item.active = false;
        }
    }

    const PrecisionDarkTokens& t = tokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp == nullptr) {
        return triggered;
    }

    const float scale = std::max(0.75f, dpi_scale);
    const float w = std::round(t.toast_w_px * scale);
    const float pad = std::round(t.panel_pad_px * scale);
    const float toast_h = std::round(64.0f * scale);
    const float gap = std::round(t.unit_px * 2.0f * scale);
    const float margin_right = std::round(12.0f * scale);
    const float bottom_edge = vp->WorkPos.y + vp->WorkSize.y - std::round((t.status_bar_h_px + 8.0f) * scale);

    size_t visible_idx = 0;
    for (size_t i = kMaxToasts; i > 0 && visible_idx < kMaxVisible; --i) {
        ToastItem& item = items_[i - 1];
        if (!item.active) {
            continue;
        }

        const float y_bottom = bottom_edge - static_cast<float>(visible_idx) * (toast_h + gap);
        const ImVec2 win_pos(vp->WorkPos.x + vp->WorkSize.x - w - margin_right, y_bottom - toast_h);

        ImGui::SetNextWindowPos(win_pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, toast_h), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_popover_px * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad + 4.0f * scale, pad));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);

        char win_name[32]{};
        std::snprintf(win_name, sizeof(win_name), "##Toast_%u", item.id);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin(win_name, nullptr, flags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p_min = ImGui::GetWindowPos();
            const ImVec2 p_max(p_min.x + w, p_min.y + toast_h);
            dl->AddRectFilled(p_min, ImVec2(p_min.x + std::max(2.0f, 2.0f * scale), p_max.y),
                              severity_col32(t, item.severity));

            if (fonts.body_strong != nullptr) {
                ImGui::PushFont(fonts.body_strong);
            }
            ImGui::TextColored(t.text_primary, "%s %s", severity_icon(item.severity), item.title.data());
            if (fonts.body_strong != nullptr) {
                ImGui::PopFont();
            }

            ImGui::SameLine(w - 28.0f * scale);
            if (ImGui::SmallButton(icons::kClose)) {
                item.active = false;
            }

            if (fonts.caption != nullptr) {
                ImGui::PushFont(fonts.caption);
            }
            ImGui::TextColored(t.text_secondary, "%s", item.detail.data());
            if (item.action_kind != ToastActionKind::None && item.action_label[0] != '\0') {
                ImGui::SameLine(w - 78.0f * scale);
                if (ImGui::SmallButton(item.action_label.data())) {
                    triggered = item.action_kind;
                    item.active = false;
                }
            }
            if (fonts.caption != nullptr) {
                ImGui::PopFont();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        ++visible_idx;
    }

    return triggered;
}

void draw_panel_progress_bar(const UiFontSet& fonts, float progress_01, const char* status_text, bool can_cancel,
                             bool* out_cancel_clicked) noexcept {
    const PrecisionDarkTokens& t = tokens();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float win_w = ImGui::GetWindowWidth();
    const float clamped = std::clamp(progress_01, 0.0f, 1.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(win_pos, ImVec2(win_pos.x + win_w * clamped, win_pos.y + 2.0f), t.col32_accent);

    if (fonts.caption != nullptr) {
        ImGui::PushFont(fonts.caption);
    }
    ImGui::TextColored(t.text_secondary, "%s", status_text != nullptr ? status_text : "");
    if (can_cancel) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Cancel")) && out_cancel_clicked != nullptr) {
            *out_cancel_clicked = true;
        }
    }
    if (fonts.caption != nullptr) {
        ImGui::PopFont();
    }
}

bool draw_inline_error_banner(const UiFontSet& fonts, const char* what_happened, const char* consequence,
                              const char* action_label) noexcept {
    const PrecisionDarkTokens& t = tokens();
    bool clicked = false;
    if (fonts.caption != nullptr) {
        ImGui::PushFont(fonts.caption);
    }
    char line_buf[256]{};
    std::snprintf(line_buf, sizeof(line_buf), "%s %s  %s", icons::kErrorCircle,
                  what_happened != nullptr ? what_happened : "", consequence != nullptr ? consequence : "");
    ImGui::TextColored(t.status_error, "%s", line_buf);
    if (action_label != nullptr && action_label[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton(action_label)) {
            clicked = true;
        }
    }
    if (fonts.caption != nullptr) {
        ImGui::PopFont();
    }
    return clicked;
}

bool draw_empty_state(const UiFontSet& fonts, const char* message, const char* action_label) noexcept {
    const PrecisionDarkTokens& t = tokens();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float y_off = std::max(16.0f, avail.y * 0.38f);
    ImGui::Dummy(ImVec2(0.0f, y_off));
    if (fonts.body != nullptr) {
        ImGui::PushFont(fonts.body);
    }
    const char* text = message != nullptr ? message : "";
    const float text_w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - text_w) * 0.5f));
    ImGui::TextColored(t.text_secondary, "%s", text);
    bool clicked = false;
    if (action_label != nullptr && action_label[0] != '\0') {
        const float scale = ImGui::GetFontSize() / std::max(1.0f, t.type_body_px);
        const float btn_w = std::max(120.0f * scale, ImGui::CalcTextSize(action_label).x + 24.0f * scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - btn_w) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
        if (ImGui::Button(action_label, ImVec2(btn_w, t.control_h_comfy_px * scale))) {
            clicked = true;
        }
        ImGui::PopStyleColor(2);
    }
    if (fonts.body != nullptr) {
        ImGui::PopFont();
    }
    return clicked;
}

ImU32 pill_tone_color(PillTone tone) noexcept {
    const PrecisionDarkTokens& t = tokens();
    switch (tone) {
    case PillTone::Ok:
        return t.col32_status_ok;
    case PillTone::Standby:
        return t.col32_status_warn;
    case PillTone::Error:
        return t.col32_status_error;
    case PillTone::Live:
        return t.col32_status_live;
    case PillTone::Off:
    default:
        return t.col32_status_off;
    }
}

ImVec2 status_pill_size(const char* text) noexcept {
    const float scale = ImGui::GetFontSize() / std::max(1.0f, tokens().type_body_px);
    const ImVec2 ts = ImGui::CalcTextSize(text != nullptr ? text : "");
    return ImVec2(ts.x + 22.0f * scale, ts.y + 4.0f * scale);
}

void draw_status_pill_at(ImDrawList* dl, ImVec2 pos_min, const char* text, PillTone tone) noexcept {
    if (dl == nullptr) {
        return;
    }
    const PrecisionDarkTokens& t = tokens();
    const float scale = ImGui::GetFontSize() / std::max(1.0f, t.type_body_px);
    const ImVec2 sz = status_pill_size(text);
    const ImVec2 pos_max(pos_min.x + sz.x, pos_min.y + sz.y);
    const ImU32 col = pill_tone_color(tone);
    dl->AddRectFilled(pos_min, pos_max, col32_with_alpha(col, 0x2E), sz.y * 0.5f);
    dl->AddRect(pos_min, pos_max, col32_with_alpha(col, 0x80), sz.y * 0.5f, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(pos_min.x + 8.0f * scale, pos_min.y + sz.y * 0.5f), 3.0f * scale, col, 12);
    dl->AddText(ImVec2(pos_min.x + 15.0f * scale, pos_min.y + 2.0f * scale),
                tone == PillTone::Off ? t.col32_text_secondary : t.col32_text_primary, text != nullptr ? text : "");
}

bool ghost_button(const char* label, const char* trailing_icon, ImVec2 size) noexcept {
    const PrecisionDarkTokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_Button, t.transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.bg_selected);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_primary);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    if (trailing_icon != nullptr && trailing_icon[0] != '\0') {
        const ImVec2 bmin = ImGui::GetItemRectMin();
        const ImVec2 bmax = ImGui::GetItemRectMax();
        const ImVec2 isz = ImGui::CalcTextSize(trailing_icon);
        const float pad = ImGui::GetStyle().FramePadding.x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(bmax.x - pad - isz.x, bmin.y + (bmax.y - bmin.y - isz.y) * 0.5f),
                                            ImGui::IsItemHovered() ? t.col32_text_primary : t.col32_text_secondary,
                                            trailing_icon);
    }
    return clicked;
}

ConsentDecision draw_plugin_consent_modal(PluginConsentPrompt& prompt, const UiFontSet& fonts) noexcept {
    ConsentDecision decision = ConsentDecision::Pending;
    if (!prompt.open) {
        return decision;
    }

    const PrecisionDarkTokens& t = tokens();
    if (!ImGui::IsPopupOpen(tr("Plugin Capability Permission"))) {
        ImGui::OpenPopup(tr("Plugin Capability Permission"));
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp != nullptr) {
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 320.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    if (ImGui::BeginPopupModal(tr("Plugin Capability Permission"), &prompt.open,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s %s", icons::kLock, prompt.plugin_name.data());
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Spacing();
        ImGui::TextColored(t.text_secondary, "%s",
                           tr("Review requested sandbox capabilities before enabling this WASM plugin:"));
        ImGui::Separator();

        if (fonts.mono != nullptr) {
            ImGui::PushFont(fonts.mono);
        }
        ImGui::TextColored(t.text_tertiary, "ID:     %s", prompt.plugin_id.data());
        ImGui::TextColored(t.text_tertiary, "SHA256: %s", prompt.sha256_hex.data());
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Spacing();

        static constexpr struct CapLabel {
            uint32_t bit;
            const char* name;
        } kCaps[] = {
            {1u << 0, "osc_out (127.0.0.1:9001)"},
            {1u << 1, "osc_in (UDP 9002)"},
            {1u << 2, "network_http (Allowlisted loopback)"},
            {1u << 3, "scene_read (Avatar & Tracking telemetry)"},
            {1u << 4, "expression_write (Override morph weights)"},
            {1u << 5, "filesystem_read (Plugin assets folder only)"},
        };
        for (const auto& cap : kCaps) {
            if ((prompt.capability_mask & cap.bit) != 0u) {
                ImGui::BulletText("%s", cap.name);
            }
        }
        if (prompt.capability_mask == 0u) {
            ImGui::BulletText("%s", tr("Pure compute (no host I/O capabilities requested)"));
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button(tr("Deny"), ImVec2(120.0f, t.control_h_comfy_px))) {
            decision = ConsentDecision::Deny;
            prompt.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
        ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
        if (ImGui::Button(tr("Trust & Enable"), ImVec2(148.0f, t.control_h_comfy_px))) {
            decision = ConsentDecision::TrustAndEnable;
            prompt.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return decision;
}

} // namespace aether::ui
