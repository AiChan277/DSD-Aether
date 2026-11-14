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

constexpr float kMinLabelFrac = 0.38f;
constexpr float kMaxLabelFrac = 0.48f;

// Depth of open `begin_property_table()` scopes; rows outside any scope open their own one-row table.
int g_property_table_depth = 0;

// Reset-dot slot at the right edge of the label column (px at 100%).
constexpr float kResetDotSlotPx = 12.0f;

struct LabelWidthCache {
    uint32_t font_generation{UINT32_MAX};
    const ImFont* font{nullptr};
    float font_size{-1.0f};
    float widest_px{0.0f};
};
LabelWidthCache g_label_cache{};

float widest_param_label_px() noexcept {
    const ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const uint32_t gen = ui_font_generation();
    if (g_label_cache.font_generation != gen || g_label_cache.font != font ||
        std::fabs(g_label_cache.font_size - fs) > 0.01f) {
        float widest = 0.0f;
        for (const ParamSpec& spec : kParamSpecs) {
            widest = std::max(widest, ImGui::CalcTextSize(tr(spec.label)).x);
        }
        g_label_cache.font_generation = gen;
        g_label_cache.font = font;
        g_label_cache.font_size = fs;
        g_label_cache.widest_px = widest;
    }
    return g_label_cache.widest_px;
}

float ui_scale() noexcept {
    return ImGui::GetFontSize() / std::max(1.0f, tokens().type_body_px);
}

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

/** Copies `text` into `out`, eliding with "..." so it fits `max_w` pixels. Returns true when elided. */
bool elide_to_width(const char* text, float max_w, char* out, size_t out_cap) noexcept {
    std::snprintf(out, out_cap, "%s", text);
    if (ImGui::CalcTextSize(out).x <= max_w) {
        return false;
    }
    const float dots_w = ImGui::CalcTextSize("...").x;
    size_t len = std::strlen(out);
    while (len > 0) {
        --len;
        out[len] = '\0';
        if (ImGui::CalcTextSize(out).x + dots_w <= max_w) {
            break;
        }
    }
    std::snprintf(out + len, out_cap - len, "...");
    return true;
}

/** Fill origin: 0 for unipolar ranges, the zero point for bipolar ranges (e.g. Position -5..5 fills from 0). */
float scrub_origin_frac(float v_min, float v_max) noexcept {
    return (v_min < 0.0f && v_max > 0.0f) ? (-v_min / (v_max - v_min)) : 0.0f;
}

/** Shared scrub-well chrome drawn before the transparent Drag widget: control well + inline fill bar. */
void draw_scrub_well(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float frac, float origin_frac, bool hovered, bool active,
                     bool editing) noexcept {
    const PrecisionDarkTokens& t = tokens();
    const float rounding = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p0, p1, (hovered || active) ? t.col32_bg_hover : t.col32_bg_inset, rounding);
    const float w = p1.x - p0.x;
    const float lo = std::clamp(std::min(frac, origin_frac), 0.0f, 1.0f);
    const float hi = std::clamp(std::max(frac, origin_frac), 0.0f, 1.0f);
    if (editing || hi - lo <= 0.001f) {
        return;
    }
    ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
    if (lo <= 0.001f) {
        corners |= ImDrawFlags_RoundCornersLeft;
    }
    if (hi >= 0.999f) {
        corners |= ImDrawFlags_RoundCornersRight;
    }
    dl->AddRectFilled(ImVec2(p0.x + w * lo, p0.y), ImVec2(p0.x + w * hi, p1.y),
                      active ? t.col32_scrub_fill_active : t.col32_scrub_fill, rounding, corners);
}

void draw_scrub_overlay(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* unit, bool focused, bool editing) noexcept {
    const PrecisionDarkTokens& t = tokens();
    if (focused) {
        dl->AddRect(p0, p1, t.col32_accent, ImGui::GetStyle().FrameRounding, 0, 1.0f);
    }
    if (!editing && unit != nullptr && unit[0] != '\0') {
        const ImVec2 sz = ImGui::CalcTextSize(unit);
        const float pad = ImGui::GetStyle().FramePadding.x;
        dl->AddText(ImVec2(p1.x - pad - sz.x, p0.y + (p1.y - p0.y - sz.y) * 0.5f), t.col32_text_tertiary, unit);
    }
}

// ImGui's DragBehavior already multiplies mouse deltas by 10x while Shift is held; 0.01 here makes
// Shift a net 0.1x fine-scrub, and Ctrl a 10x coarse scrub (Ctrl+click still opens text entry).
float drag_speed_modifier() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyShift ? 0.01f : (io.KeyCtrl ? 10.0f : 1.0f);
}

} // namespace

float property_label_column_width(float avail_w) noexcept {
    // Content width of the label column (ImGui adds CellPadding on both sides of each column itself).
    const float needed = widest_param_label_px() + kResetDotSlotPx * ui_scale();
    return std::clamp(needed, avail_w * kMinLabelFrac, avail_w * kMaxLabelFrac);
}

bool begin_property_table(const char* str_id) {
    const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float label_w = property_label_column_width(avail);
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(str_id, 2, kFlags)) {
        return false;
    }
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, label_w);
    ImGui::TableSetupColumn("##control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ++g_property_table_depth;
    return true;
}

void end_property_table() {
    if (g_property_table_depth > 0) {
        --g_property_table_depth;
    }
    ImGui::EndTable();
}

bool scrub_slider_float(const char* str_id, float* v, float v_min, float v_max, float speed, const char* value_fmt,
                        const char* unit, bool mixed) {
    const float w = ImGui::CalcItemWidth();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + ImGui::GetFrameHeight());
    const ImGuiID id = ImGui::GetID(str_id);
    const bool editing = ImGui::TempInputIsActive(id);
    const bool hovered = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered();
    const bool active = (ImGui::GetActiveID() == id);
    const float frac = (!mixed && v_max > v_min) ? std::clamp((*v - v_min) / (v_max - v_min), 0.0f, 1.0f) : 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_scrub_well(dl, p0, p1, frac, scrub_origin_frac(v_min, v_max), hovered, active, editing);

    const ImVec4 clear = tokens().transparent;
    if (!editing) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
    }
    const bool changed = ImGui::DragFloat(str_id, v, speed * drag_speed_modifier(), v_min, v_max,
                                          mixed ? "--" : value_fmt, ImGuiSliderFlags_AlwaysClamp);
    if (!editing) {
        ImGui::PopStyleColor(3);
    }
    draw_scrub_overlay(dl, p0, p1, unit, ImGui::IsItemActive() || ImGui::IsItemFocused(), editing);
    return changed;
}

bool scrub_slider_int(const char* str_id, int* v, int v_min, int v_max, float speed, const char* unit, bool mixed) {
    const float w = ImGui::CalcItemWidth();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + ImGui::GetFrameHeight());
    const ImGuiID id = ImGui::GetID(str_id);
    const bool editing = ImGui::TempInputIsActive(id);
    const bool hovered = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered();
    const bool active = (ImGui::GetActiveID() == id);
    const float frac = (!mixed && v_max > v_min)
                           ? std::clamp(static_cast<float>(*v - v_min) / static_cast<float>(v_max - v_min), 0.0f, 1.0f)
                           : 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_scrub_well(dl, p0, p1, frac, scrub_origin_frac(static_cast<float>(v_min), static_cast<float>(v_max)), hovered,
                    active, editing);

    const ImVec4 clear = tokens().transparent;
    if (!editing) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clear);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clear);
    }
    const bool changed = ImGui::DragInt(str_id, v, std::max(0.05f, speed * drag_speed_modifier()), v_min, v_max,
                                        mixed ? "--" : "%d", ImGuiSliderFlags_AlwaysClamp);
    if (!editing) {
        ImGui::PopStyleColor(3);
    }
    draw_scrub_overlay(dl, p0, p1, unit, ImGui::IsItemActive() || ImGui::IsItemFocused(), editing);
    return changed;
}

void draw_status_badge(const UiFontSet& fonts, const char* label, const char* value, const ImVec4& value_col) {
    const PrecisionDarkTokens& t = tokens();
    if (fonts.caption != nullptr) {
        ImGui::PushFont(fonts.caption);
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad_x = style.FramePadding.x;
    const float pad_y = style.FramePadding.y * 0.8f;
    const float avail_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float inner_w = std::max(1.0f, avail_w - pad_x * 2.0f);

    const ImVec2 label_sz = ImGui::CalcTextSize(label);
    const float gap = style.ItemInnerSpacing.x;
    const float value_wrap = std::max(1.0f, inner_w - label_sz.x - gap);
    const ImVec2 value_sz = ImGui::CalcTextSize(value, nullptr, false, value_wrap);
    const float body_h = std::max(label_sz.y, value_sz.y);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + avail_w, p0.y + body_h + pad_y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, t.col32_bg_inset, t.radius_control_px * ui_scale());
    dl->AddRect(p0, p1, t.col32_border_default, t.radius_control_px * ui_scale(), 0, 1.0f);
    dl->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y), t.col32_text_secondary, label);
    dl->AddText(nullptr, 0.0f, ImVec2(p0.x + pad_x + label_sz.x + gap, p0.y + pad_y),
                ImGui::ColorConvertFloat4ToU32(value_col), value, nullptr, value_wrap);
    ImGui::Dummy(ImVec2(avail_w, p1.y - p0.y));
    if (fonts.caption != nullptr) {
        ImGui::PopFont();
    }
}

bool draw_property_row(ParamId id, ParamStore& store, UndoStack& undo, const UiFontSet& fonts,
                       PropertyRowActions& actions, bool mixed, bool read_only, const char* inline_error,
                       const char* error_action_label, bool* out_error_action_clicked) {
    const PrecisionDarkTokens& t = tokens();
    const ParamSpec& spec = param_spec(id);
    const float scale = ui_scale();

    const bool own_table = (g_property_table_depth == 0);
    if (own_table && !begin_property_table(spec.id)) {
        return false;
    }

    ImGui::PushID(spec.id);
    ImGui::TableNextRow();

    const bool modified = !mixed && store.is_modified(id);
    bool edited = false;
    const double cur_val = store.get(id);

    // Column 1: label (elided only if the panel is narrower than the 48% cap) + amber reset dot.
    ImGui::TableSetColumnIndex(0);
    const float label_col_x = ImGui::GetCursorScreenPos().x;
    const float label_col_w = ImGui::GetContentRegionAvail().x;
    const float dot_w = kResetDotSlotPx * scale;
    ImGui::AlignTextToFramePadding();
    char label_buf[96]{};
    const bool elided =
        elide_to_width(tr(spec.label), std::max(8.0f, label_col_w - dot_w), label_buf, sizeof(label_buf));
    const ImVec4 label_col = read_only ? t.text_disabled : (modified ? t.text_primary : t.text_secondary);
    ImGui::PushStyleColor(ImGuiCol_Text, label_col);
    ImGui::TextUnformatted(label_buf);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s%s%s\n%s [%s]", elided ? tr(spec.label) : "", elided ? "\n" : "", tr(spec.tooltip),
                          spec.id, param_owner_name(spec.owner));
    }

    if (modified && !read_only) {
        const float frame_h = ImGui::GetFrameHeight();
        ImGui::SameLine();
        const ImVec2 row_top(label_col_x, ImGui::GetItemRectMin().y - ImGui::GetStyle().FramePadding.y);
        const ImVec2 dot_min(label_col_x + label_col_w - dot_w, row_top.y + (frame_h - dot_w) * 0.5f);
        ImGui::SetCursorScreenPos(dot_min);
        if (ImGui::InvisibleButton("##reset_dot", ImVec2(dot_w, dot_w))) {
            if (store.set(id, spec.default_val)) {
                undo.push_param(id, cur_val, spec.default_val);
                edited = true;
            }
        }
        const bool dot_hov = ImGui::IsItemHovered();
        const ImVec2 center(dot_min.x + dot_w * 0.5f, dot_min.y + dot_w * 0.5f);
        ImGui::GetWindowDrawList()->AddCircleFilled(center, (dot_hov ? 4.5f : 3.5f) * scale,
                                                    dot_hov ? t.col32_text_primary : t.col32_status_warn);
        if (dot_hov) {
            ImGui::SetTooltip("%s", tr("Reset to default"));
        }
    }

    // Column 2: full-width control (combo boxes / scrubbers fill the column, never clipped).
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(read_only);
    if (spec.type == ParamType::Bool) {
        bool v = !mixed && (cur_val >= 0.5);
        if (mixed) {
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        }
        if (ImGui::Checkbox(v ? "On###ctrl" : "Off###ctrl", &v)) {
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
        const int cur_idx = std::clamp(static_cast<int>(std::llround(cur_val)), 0, std::max(0, item_count - 1));
        char preview[64]{"--"};
        if (!mixed) {
            (void)extract_enum_item(spec.enum_labels, cur_idx, preview, sizeof(preview));
        }
        if (ImGui::BeginCombo("##ctrl", preview, ImGuiComboFlags_HeightLarge)) {
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
        const float speed = std::max(0.1f, static_cast<float>(spec.step) * 0.25f);
        if (scrub_slider_int("##ctrl", &v, v_min, v_max, speed, spec.unit, mixed)) {
            if (store.set(id, static_cast<double>(v))) {
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            undo.begin_drag(id, cur_val);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo.commit_drag(id, store.get(id));
        } else if (ImGui::IsItemDeactivated()) {
            undo.cancel_drag();
        }
    } else {
        float v = static_cast<float>(cur_val);
        const float speed = std::max(0.001f, static_cast<float>(spec.step) * 0.5f);
        char fmt[16]{};
        std::snprintf(fmt, sizeof(fmt), "%%.%uf", static_cast<unsigned>(spec.precision));
        if (scrub_slider_float("##ctrl", &v, static_cast<float>(spec.min_val), static_cast<float>(spec.max_val), speed,
                               fmt, spec.unit, mixed)) {
            if (store.set(id, static_cast<double>(v))) {
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            undo.begin_drag(id, cur_val);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            undo.commit_drag(id, store.get(id));
        } else if (ImGui::IsItemDeactivated()) {
            undo.cancel_drag();
        }
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s\n(%s · %s)", tr(spec.tooltip), spec.id, param_owner_name(spec.owner));
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

    // Inline error banner beneath the control (§12.13)
    if (inline_error != nullptr && inline_error[0] != '\0') {
        if (fonts.caption != nullptr) {
            ImGui::PushFont(fonts.caption);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, t.status_error);
        ImGui::TextWrapped("%s %s", icons::kErrorCircle, inline_error);
        ImGui::PopStyleColor();
        if (error_action_label != nullptr && error_action_label[0] != '\0') {
            if (ImGui::SmallButton(error_action_label) && out_error_action_clicked != nullptr) {
                *out_error_action_clicked = true;
            }
        }
        if (fonts.caption != nullptr) {
            ImGui::PopFont();
        }
    }

    ImGui::PopID();
    if (own_table) {
        end_property_table();
    }

    if (edited) {
        actions.value_changed = true;
        actions.changed_param = id;
    }
    return edited;
}

} // namespace aether::ui
