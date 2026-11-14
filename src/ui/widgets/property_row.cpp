#include "ui/widgets/property_row.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aether::ui {
namespace {

bool extract_enum_item(const char* list, int idx, char* out, size_t out_cap) noexcept {
    if (list == nullptr || out == nullptr || out_cap == 0) {
        return false;
    }
    int cur = 0;
    const char* p = list;
    while (*p != '\0') {
        const char* bar = std::strchr(p, '|');
        const size_t len = bar != nullptr ? static_cast<size_t>(bar - p) : std::strlen(p);
        if (cur == idx) {
            const size_t n = std::min(len, out_cap - 1);
            std::memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        if (bar == nullptr) {
            break;
        }
        p = bar + 1;
        ++cur;
    }
    return false;
}

int count_enum_items(const char* list) noexcept {
    if (list == nullptr || list[0] == '\0') {
        return 0;
    }
    int count = 1;
    for (const char* p = list; *p != '\0'; ++p) {
        if (*p == '|') {
            ++count;
        }
    }
    return count;
}

} // namespace

bool draw_property_row(ParamId id, ParamStore& store, UndoStack& undo, const UiFontSet& fonts,
                       PropertyRowActions& actions, bool mixed, bool read_only, const char* inline_error,
                       const char* error_action_label, bool* out_error_action_clicked) {
    const PrecisionDarkTokens& t = tokens();
    const ParamSpec& spec = param_spec(id);
    const ImGuiIO& io = ImGui::GetIO();

    const float scale = ImGui::GetFontSize() / std::max(1.0f, t.type_body_px);
    const float row_h = std::round(t.property_row_h_px * scale);
    const float label_col_w = std::round(t.property_label_col_px * scale);
    const float reset_col_w = std::round(16.0f * scale);

    ImGui::PushID(spec.id);
    const ImVec2 start_pos = ImGui::GetCursorScreenPos();
    const float total_w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const float ctrl_w = std::max(48.0f, total_w - label_col_w - reset_col_w - t.unit_px * 2.0f);

    const bool modified = !mixed && store.is_modified(id);
    bool edited = false;
    const double cur_val = store.get(id);

    // Column 1: fixed 104 px label
    ImGui::AlignTextToFramePadding();
    const ImVec4 label_col = read_only ? t.text_disabled : (modified ? t.text_primary : t.text_secondary);
    ImGui::PushStyleColor(ImGuiCol_Text, label_col);
    const ImVec2 label_max(start_pos.x + label_col_w, start_pos.y + row_h);
    ImGui::PushClipRect(start_pos, label_max, true);
    ImGui::TextUnformatted(tr(spec.label));
    ImGui::PopClipRect();
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered() && GImGui != nullptr && GImGui->HoveredIdTimer >= 0.45f) {
        char tip_buf[256]{};
        std::snprintf(tip_buf, sizeof(tip_buf), "%s\n%s [%s]", tr(spec.tooltip), spec.id, param_owner_name(spec.owner));
        ImGui::SetTooltip("%s", tip_buf);
    }

    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(start_pos.x + label_col_w + t.unit_px, start_pos.y));
    ImGui::SetNextItemWidth(ctrl_w);

    ImGui::BeginDisabled(read_only);
    if (spec.type == ParamType::Bool) {
        bool v = !mixed && (cur_val >= 0.5);
        if (mixed) {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        if (ImGui::Checkbox("##ctrl", &v)) {
            const double next_val = v ? 1.0 : 0.0;
            if (store.set(id, next_val)) {
                undo.push_param(id, cur_val, next_val);
                edited = true;
            }
        }
        if (mixed) {
            ImGui::PopItemFlag();
        }
    } else if (spec.type == ParamType::Enum && spec.enum_labels != nullptr) {
        const int item_count = count_enum_items(spec.enum_labels);
        int cur_idx = std::clamp(static_cast<int>(std::llround(cur_val)), 0, std::max(0, item_count - 1));
        char preview[64]{"—"};
        if (!mixed) {
            (void)extract_enum_item(spec.enum_labels, cur_idx, preview, sizeof(preview));
        }
        if (ImGui::BeginCombo("##ctrl", preview)) {
            for (int i = 0; i < item_count; ++i) {
                char item_txt[64]{};
                (void)extract_enum_item(spec.enum_labels, i, item_txt, sizeof(item_txt));
                const bool sel = (!mixed && i == cur_idx);
                if (ImGui::Selectable(item_txt, sel)) {
                    const double next_val = static_cast<double>(i);
                    if (store.set(id, next_val)) {
                        undo.push_param(id, cur_val, next_val);
                        edited = true;
                    }
                }
                if (sel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    } else if (spec.type == ParamType::Int) {
        int v = static_cast<int>(std::llround(cur_val));
        const int v_min = static_cast<int>(std::llround(spec.min_val));
        const int v_max = static_cast<int>(std::llround(spec.max_val));
        const float mod_scale = io.KeyShift ? 0.1f : (io.KeyCtrl ? 10.0f : 1.0f);
        const float speed = std::max(0.1f, static_cast<float>(spec.step) * 0.25f * mod_scale);
        char fmt[32]{};
        if (mixed) {
            std::snprintf(fmt, sizeof(fmt), "—");
        } else if (spec.unit != nullptr && spec.unit[0] != '\0') {
            std::snprintf(fmt, sizeof(fmt), "%%d %s", spec.unit);
        } else {
            std::snprintf(fmt, sizeof(fmt), "%%d");
        }
        if (ImGui::DragInt("##ctrl", &v, speed, v_min, v_max, fmt, ImGuiSliderFlags_AlwaysClamp)) {
            const double next_val = static_cast<double>(v);
            if (store.set(id, next_val)) {
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            undo.begin_drag(id, cur_val);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo.commit_drag(id, store.get(id));
        }
    } else {
        float v = static_cast<float>(cur_val);
        const float v_min = static_cast<float>(spec.min_val);
        const float v_max = static_cast<float>(spec.max_val);
        const float mod_scale = io.KeyShift ? 0.1f : (io.KeyCtrl ? 10.0f : 1.0f);
        const float speed = std::max(0.001f, static_cast<float>(spec.step) * 0.5f * mod_scale);
        char fmt[32]{};
        if (mixed) {
            std::snprintf(fmt, sizeof(fmt), "—");
        } else if (spec.unit != nullptr && spec.unit[0] != '\0') {
            std::snprintf(fmt, sizeof(fmt), "%%.%uf %s", static_cast<unsigned>(spec.precision), spec.unit);
        } else {
            std::snprintf(fmt, sizeof(fmt), "%%.%uf", static_cast<unsigned>(spec.precision));
        }
        if (ImGui::DragFloat("##ctrl", &v, speed, v_min, v_max, fmt, ImGuiSliderFlags_AlwaysClamp)) {
            const double next_val = static_cast<double>(v);
            if (store.set(id, next_val)) {
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            undo.begin_drag(id, cur_val);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo.commit_drag(id, store.get(id));
        }
    }

    if (ImGui::IsItemHovered() && GImGui != nullptr && GImGui->HoveredIdTimer >= 0.45f) {
        char tip_buf[256]{};
        std::snprintf(tip_buf, sizeof(tip_buf), "%s\n(%s · %s)", tr(spec.tooltip), spec.id,
                      param_owner_name(spec.owner));
        ImGui::SetTooltip("%s", tip_buf);
    }

    // Standard property right-click context menu (§12.5)
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem(tr("Reset to Default"), nullptr, false, !read_only && modified)) {
            if (store.set(id, spec.default_val)) {
                undo.push_param(id, cur_val, spec.default_val);
                edited = true;
            }
        }
        if (ImGui::MenuItem(tr("Copy Value"))) {
            char val_buf[64]{};
            std::snprintf(val_buf, sizeof(val_buf), "%.6g", cur_val);
            ImGui::SetClipboardText(val_buf);
            actions.copied_feedback_msg = tr("Copied parameter value");
        }
        if (ImGui::MenuItem(tr("Paste Value"), nullptr, false, !read_only)) {
            if (const char* clip = ImGui::GetClipboardText()) {
                char* end_ptr = nullptr;
                const double parsed = std::strtod(clip, &end_ptr);
                if (end_ptr != clip && std::isfinite(parsed)) {
                    if (store.set(id, parsed)) {
                        undo.push_param(id, cur_val, store.get(id));
                        edited = true;
                    }
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Copy Param ID"))) {
            ImGui::SetClipboardText(spec.id);
            actions.copied_feedback_msg = tr("Copied parameter ID");
        }
        const bool can_osc = (spec.flags & PARAM_FLAG_OSC_EXPOSED) != 0u;
        if (ImGui::MenuItem(tr("Copy OSC Address"), nullptr, false, can_osc)) {
            char osc_buf[128]{};
            std::snprintf(osc_buf, sizeof(osc_buf), "/aether/param/%s", spec.id);
            ImGui::SetClipboardText(osc_buf);
            actions.copied_feedback_msg = tr("Copied OSC endpoint");
        }
        const bool can_expose = (spec.flags & PARAM_FLAG_GRAPH_EXPOSABLE) != 0u;
        if (ImGui::MenuItem(tr("Expose in Node Graph"), nullptr, false, can_expose)) {
            actions.expose_graph_param_id = spec.id;
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    // Column 3: 8 px amber modified dot + click-to-reset
    ImGui::SameLine();
    const ImVec2 dot_pos(start_pos.x + label_col_w + ctrl_w + t.unit_px * 2.0f, start_pos.y + (row_h - 14.0f) * 0.5f);
    ImGui::SetCursorScreenPos(dot_pos);
    if (modified && !read_only) {
        if (ImGui::InvisibleButton("##reset_dot", ImVec2(14.0f, 14.0f))) {
            if (store.set(id, spec.default_val)) {
                undo.push_param(id, cur_val, spec.default_val);
                edited = true;
            }
        }
        const bool dot_hov = ImGui::IsItemHovered();
        const ImVec2 center(dot_pos.x + 7.0f, dot_pos.y + 7.0f);
        ImGui::GetWindowDrawList()->AddCircleFilled(center, dot_hov ? 4.5f : 4.0f,
                                                    dot_hov ? t.col32_text_primary : t.col32_status_warn);
        if (dot_hov) {
            ImGui::SetTooltip("%s", tr("Reset to default"));
        }
    } else {
        ImGui::Dummy(ImVec2(14.0f, 14.0f));
    }

    ImGui::SetCursorScreenPos(ImVec2(start_pos.x, start_pos.y + row_h));

    // Inline error banner beneath row (§12.13)
    if (inline_error != nullptr && inline_error[0] != '\0') {
        if (fonts.caption != nullptr) {
            ImGui::PushFont(fonts.caption);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, t.status_error);
        ImGui::TextWrapped("%s %s", icons::kErrorCircle, inline_error);
        ImGui::PopStyleColor();
        if (error_action_label != nullptr && error_action_label[0] != '\0') {
            ImGui::SameLine();
            if (ImGui::SmallButton(error_action_label) && out_error_action_clicked != nullptr) {
                *out_error_action_clicked = true;
            }
        }
        if (fonts.caption != nullptr) {
            ImGui::PopFont();
        }
    }

    ImGui::PopID();

    if (edited) {
        actions.value_changed = true;
        actions.changed_param = id;
    }
    return edited;
}

} // namespace aether::ui
