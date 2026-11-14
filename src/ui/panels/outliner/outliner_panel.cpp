#include "ui/panels/outliner/outliner_panel.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

bool icase_contains(const char* haystack, const char* needle) noexcept {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    if (haystack == nullptr) {
        return false;
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

void set_selection(UiSelection& sel, SelectionKind kind, int32_t idx, const char* name) noexcept {
    sel.kind = kind;
    sel.item_index = idx;
    std::snprintf(sel.name.data(), sel.name.size(), "%s", name != nullptr ? name : "");
}

/** Returns the file-name component of `path` without allocating. */
const char* path_basename(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base[0] != '\0' ? base : nullptr;
}

/** Pushes selected-row colors (accent-tinted) for Selectable rows inside the outliner. */
struct ScopedRowColors {
    ScopedRowColors() noexcept {
        const PrecisionDarkTokens& t = tokens();
        ImGui::PushStyleColor(ImGuiCol_Header, t.bg_selected);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, t.bg_hover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, t.bg_selected);
    }
    ~ScopedRowColors() { ImGui::PopStyleColor(3); }
    ScopedRowColors(const ScopedRowColors&) = delete;
    ScopedRowColors& operator=(const ScopedRowColors&) = delete;
};

/** Copies `text` into `out`, eliding with "..." to fit `max_w` px. */
void elide_into(const char* text, float max_w, char* out, size_t cap) noexcept {
    std::snprintf(out, cap, "%s", text != nullptr ? text : "");
    if (ImGui::CalcTextSize(out).x <= max_w) {
        return;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    size_t len = std::strlen(out);
    while (len > 0) {
        out[--len] = '\0';
        if (ImGui::CalcTextSize(out).x + dots <= max_w) {
            break;
        }
    }
    std::snprintf(out + len, cap - len, "...");
}

} // namespace

void OutlinerPanel::set_sample_paths(const char* ref_vrm, const char* vrm1, const char* fbx, const char* pmx) noexcept {
    std::snprintf(sample_ref_vrm_.data(), sample_ref_vrm_.size(), "%s", ref_vrm != nullptr ? ref_vrm : "");
    std::snprintf(sample_vrm1_.data(), sample_vrm1_.size(), "%s", vrm1 != nullptr ? vrm1 : "");
    std::snprintf(sample_fbx_.data(), sample_fbx_.size(), "%s", fbx != nullptr ? fbx : "");
    std::snprintf(sample_pmx_.data(), sample_pmx_.size(), "%s", pmx != nullptr ? pmx : "");
}

void OutlinerPanel::set_avatar_path_buf(const char* path) noexcept {
    std::snprintf(path_buf_.data(), path_buf_.size(), "%s", path != nullptr ? path : "");
}

void OutlinerPanel::draw_scene_well(const DevShellAvatarView& avatar, const UiFontSet& fonts, UiSelection& selection,
                                    StudioActions& actions, float scale) {
    const PrecisionDarkTokens& t = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bg_well);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_default);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * scale, 6.0f * scale));
    const bool visible = ImGui::BeginChild("##SceneTreeWell", ImVec2(0.0f, std::round(t.scene_well_h_px * scale)),
                                           ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (visible) {
        if (!avatar.has_avatar && !avatar.is_loading) {
            // Centered empty-state card (replaces the old panel-height empty state and its dead gap).
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float line_h = ImGui::GetTextLineHeight();
            const float btn_h = ImGui::GetFrameHeight();
            const float card_h = line_h * 2.0f + style.ItemSpacing.y * 3.0f + btn_h * 2.0f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - card_h) * 0.5f));

            auto centered_text = [&](const ImVec4& col, const char* txt) {
                const float w = ImGui::CalcTextSize(txt).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - w) * 0.5f));
                ImGui::TextColored(col, "%s", txt);
            };
            centered_text(t.text_tertiary, icons::kBox);
            centered_text(t.text_secondary, tr("No avatar loaded"));
            ImGui::Spacing();

            const float btn_w = std::min(avail.x, 210.0f * scale);
            const float btn_x = ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - btn_w) * 0.5f);
            ImGui::SetCursorPosX(btn_x);
            ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.accent_hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.accent_pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
            if (ImGui::Button(tr("Open Avatar..."), ImVec2(btn_w, 0.0f))) {
                actions.request_open_avatar_dialog = true;
            }
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", tr("Browse for .vrm / .glb / .gltf / .fbx / .pmx (Ctrl+I)"));
            }

            ImGui::SetCursorPosX(btn_x);
            const bool has_ref = sample_ref_vrm_[0] != '\0';
            ImGui::BeginDisabled(!has_ref);
            if (ghost_button(tr("Load Reference VRM"), nullptr, ImVec2(btn_w, 0.0f))) {
                std::snprintf(path_buf_.data(), path_buf_.size(), "%s", sample_ref_vrm_.data());
                actions.viewport.request_load_path = path_buf_.data();
            }
            ImGui::EndDisabled();
            if (!has_ref && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", tr("samples/reference.vrm is not installed"));
            }
        } else {
            ScopedRowColors row_colors;
            bool any_visible = false;
            auto draw_node = [&](SelectionKind k, int32_t idx, const char* icon, const char* label, const char* badge) {
                if (!icase_contains(label, filter_buf_.data())) {
                    return;
                }
                any_visible = true;
                const bool sel = (selection.kind == k && selection.item_index == idx);
                char row_lbl[160]{};
                std::snprintf(row_lbl, sizeof(row_lbl), "%s  %s##scene%d", icon, label, static_cast<int>(k) * 64 + idx);
                if (ImGui::Selectable(row_lbl, sel)) {
                    set_selection(selection, k, idx, label);
                }
                if (badge != nullptr && badge[0] != '\0') {
                    const ImVec2 bsz = ImGui::CalcTextSize(badge);
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(rmax.x - bsz.x - 4.0f * scale, rmin.y + (rmax.y - rmin.y - bsz.y) * 0.5f),
                        t.col32_text_tertiary, badge);
                }
            };

            const char* avatar_name = path_basename(avatar.source_path);
            char avatar_badge[48]{};
            if (avatar.is_loading) {
                std::snprintf(avatar_badge, sizeof(avatar_badge), "%.0f%%",
                              static_cast<double>(avatar.load_progress * 100.0f));
            } else {
                std::snprintf(avatar_badge, sizeof(avatar_badge), "%s · %u/%u", avatar.format_name,
                              avatar.mapped_humanoid_bones, avatar.joint_count);
            }
            if (fonts.body_strong != nullptr) {
                ImGui::PushFont(fonts.body_strong);
            }
            draw_node(SelectionKind::Avatar, 0, icons::kBox, avatar_name != nullptr ? avatar_name : avatar.format_name,
                      avatar_badge);
            if (fonts.body_strong != nullptr) {
                ImGui::PopFont();
            }
            ImGui::Indent(10.0f * scale);
            draw_node(SelectionKind::LightKey, 0, icons::kSun, tr("Key Light"), "Key");
            draw_node(SelectionKind::LightFill, 0, icons::kSun, tr("Fill Light"), "Fill");
            draw_node(SelectionKind::LightRim, 0, icons::kSun, tr("Rim Light"), "Rim");
            draw_node(SelectionKind::Camera, 0, icons::kCamera, tr("Studio Camera"), nullptr);
            ImGui::Unindent(10.0f * scale);
            if (!any_visible) {
                ImGui::TextColored(t.text_tertiary, "%s", tr("No scene items match the search."));
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void OutlinerPanel::draw_expression_grid(std::span<ExpressionSlotInfo> expressions, const UiFontSet& fonts,
                                         UiSelection& selection, float scale) {
    const PrecisionDarkTokens& t = tokens();
    if (!ImGui::BeginTable("##ExprGrid", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = t.radius_control_px * scale;
    size_t shown = 0;
    for (size_t i = 0; i < expressions.size() && i < 8; ++i) {
        ExpressionSlotInfo& ex = expressions[i];
        if (!icase_contains(ex.name.data(), filter_buf_.data()) &&
            !icase_contains(ex.hotkey.data(), filter_buf_.data())) {
            continue;
        }
        ImGui::TableNextColumn();
        ++shown;
        ImGui::PushID(static_cast<int>(ex.slot));
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##expr", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 p1(p0.x + w, p0.y + h);
        const bool latched = ex.triggered_override;

        dl->AddRectFilled(p0, p1, latched ? t.col32_bg_selected : (hovered ? t.col32_bg_hover : t.col32_bg_raised),
                          rounding);
        dl->AddRect(p0, p1, latched ? t.col32_accent : t.col32_border_default, rounding, 0,
                    latched ? 1.5f * scale : 1.0f);
        if (ex.current_weight > 0.001f) {
            const float bar_w = (w - 2.0f) * std::clamp(ex.current_weight, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(p0.x + 1.0f, p1.y - 3.0f * scale), ImVec2(p0.x + 1.0f + bar_w, p1.y - 1.0f),
                              t.col32_accent);
        }

        // Key badge (mono) + elided label
        if (fonts.mono != nullptr) {
            ImGui::PushFont(fonts.mono);
        }
        const ImVec2 key_sz = ImGui::CalcTextSize(ex.hotkey.data());
        const float pad = 5.0f * scale;
        const ImVec2 b0(p0.x + pad, p0.y + (h - key_sz.y - 2.0f * scale) * 0.5f);
        const ImVec2 b1(b0.x + key_sz.x + 8.0f * scale, b0.y + key_sz.y + 2.0f * scale);
        dl->AddRectFilled(b0, b1, latched ? t.col32_accent : t.col32_bg_inset, 2.0f * scale);
        dl->AddText(ImVec2(b0.x + 4.0f * scale, b0.y + 1.0f * scale),
                    latched ? t.col32_text_on_accent : t.col32_text_secondary, ex.hotkey.data());
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        const float label_x = b1.x + 6.0f * scale;
        char lbl[48]{};
        elide_into(ex.name.data(), p1.x - label_x - pad, lbl, sizeof(lbl));
        const float text_h = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(label_x, p0.y + (h - text_h) * 0.5f),
                    latched ? t.col32_text_primary : t.col32_text_secondary, lbl);

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s\n%s · %s", ex.name.data(), ex.hotkey.data(),
                              latched ? tr("latched - click to release") : tr("click to latch"));
        }
        if (clicked) {
            ex.triggered_override = !ex.triggered_override;
            set_selection(selection, SelectionKind::Expression, static_cast<int32_t>(ex.slot), ex.name.data());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (shown == 0) {
        ImGui::TextColored(t.text_tertiary, "%s", tr("No expressions match the search."));
    }
}

void OutlinerPanel::draw_outputs(const DevShellOutputView& output_view, ParamStore& params, UndoStack& undo,
                                 const UiFontSet& fonts, UiSelection& selection, StudioActions& actions, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto out_row = [&](const char* label, ParamId pid, PillTone tone, const char* pill_text, const char* detail) {
        if (!icase_contains(label, filter_buf_.data())) {
            return;
        }
        ImGui::PushID(label);
        bool on = params.get_bool(pid);
        const float row_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::Checkbox(label, &on)) {
            const double old_v = params.get(pid);
            const double new_v = on ? 1.0 : 0.0;
            if (params.set(pid, new_v)) {
                undo.push_param(pid, old_v, new_v);
                actions.viewport.output_config_dirty = true;
            }
        }
        const ImVec2 cb_min = ImGui::GetItemRectMin();
        const ImVec2 cb_max = ImGui::GetItemRectMax();
        const ImVec2 pill_sz = status_pill_size(pill_text);
        const ImVec2 pill_min(row_right - pill_sz.x, cb_min.y + (cb_max.y - cb_min.y - pill_sz.y) * 0.5f);
        draw_status_pill_at(dl, pill_min, pill_text, tone);
        if (detail != nullptr && detail[0] != '\0' &&
            ImGui::IsMouseHoveringRect(pill_min, ImVec2(pill_min.x + pill_sz.x, pill_min.y + pill_sz.y))) {
            ImGui::SetTooltip("%s", detail);
        }
        ImGui::PopID();
    };

    auto tone_for = [](bool active, bool enabled) noexcept {
        return active ? PillTone::Ok : (enabled ? PillTone::Standby : PillTone::Off);
    };
    auto text_for = [](bool active, bool enabled) noexcept { return active ? "Active" : (enabled ? "Idle" : "Off"); };

    const bool spout_on = params.get_bool(ParamId::OutputSpoutEnabled);
    out_row(tr("Spout2"), ParamId::OutputSpoutEnabled, tone_for(output_view.spout_active, spout_on),
            text_for(output_view.spout_active, spout_on),
            output_view.spout_using_cpu_fallback ? tr("Publishing through CPU staging (cross-adapter fallback).")
                                                 : tr("Zero-copy GPU texture sharing."));
    if (spout_on && !output_view.spout_active && output_view.spout_adapter_mismatch) {
        if (draw_inline_error_banner(fonts, tr("Spout2 GPU sharing mismatch."), tr("OBS on another adapter."),
                                     tr("Use CPU sharing"))) {
            const double old_v = params.get(ParamId::OutputSpoutCpuFallback);
            if (params.set(ParamId::OutputSpoutCpuFallback, 1.0)) {
                undo.push_param(ParamId::OutputSpoutCpuFallback, old_v, 1.0);
                actions.viewport.output_config_dirty = true;
            }
        }
    }

    const bool vcam_on = params.get_bool(ParamId::OutputVcamEnabled);
    out_row(tr("Virtual Cam"), ParamId::OutputVcamEnabled, tone_for(output_view.vcam_active, vcam_on),
            text_for(output_view.vcam_active, vcam_on),
            output_view.vcam_rgb_pin_connected ? tr("A consumer is reading the RGB pin.")
                                               : tr("Aether DirectShow Virtual Camera."));

    const bool ndi_on = params.get_bool(ParamId::OutputNdiEnabled);
    if (ndi_on && !output_view.ndi_runtime_available) {
        out_row(tr("NDI"), ParamId::OutputNdiEnabled, PillTone::Error, "No Runtime",
                tr("NDI 6 Runtime not found (NDI_RUNTIME_DIR_V6)."));
    } else {
        out_row(tr("NDI"), ParamId::OutputNdiEnabled, tone_for(output_view.ndi_active, ndi_on),
                text_for(output_view.ndi_active, ndi_on), tr("Alpha video over the local network via NDI 6."));
    }

    const bool osc_on = params.get_bool(ParamId::OutputOscEnabled);
    out_row(tr("OSC 9001/9002"), ParamId::OutputOscEnabled, tone_for(output_view.osc_active, osc_on),
            text_for(output_view.osc_active, osc_on), tr("UDP telemetry out 127.0.0.1:9001, control in :9002."));

    ImGui::Spacing();
    char ghost_lbl[64]{};
    std::snprintf(ghost_lbl, sizeof(ghost_lbl), "%s##inspect_outputs", tr("Inspect Output Settings..."));
    if (ghost_button(ghost_lbl, icons::kChevronRight, ImVec2(-FLT_MIN, 0.0f))) {
        set_selection(selection, SelectionKind::OutputDest, 0, tr("Outputs & Audio DSP"));
        inspect_outputs_requested_ = true;
    }
    (void)scale;
}

void OutlinerPanel::draw(StudioMode mode, bool on_air, const DevShellAvatarView& avatar,
                         const DevShellOutputView& output_view, std::span<OutlinerBoneInfo> bones,
                         std::span<OutlinerMaterialInfo> materials, std::span<ExpressionSlotInfo> expressions,
                         std::span<const LoadedPluginVm> plugins, ParamStore& params, UndoStack& undo,
                         const UiFontSet& fonts, UiSelection& selection, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();

    if (ImGui::Begin(tr("Outliner & Project"))) {
        const float scale = ImGui::GetFontSize() / std::max(1.0f, t.type_body_px);

        if (avatar.is_loading) {
            char prog_buf[128]{};
            std::snprintf(prog_buf, sizeof(prog_buf), "Loading %s  %.0f%%", avatar.format_name,
                          static_cast<double>(avatar.load_progress * 100.0f));
            bool cancel = false;
            draw_panel_progress_bar(fonts, avatar.load_progress, prog_buf, true, &cancel);
            if (cancel) {
                actions.viewport.request_cancel_load = true;
            }
        }

        // Full-width live search (Ctrl+F)
        if (focus_filter_next_frame_) {
            ImGui::SetKeyboardFocusHere();
            focus_filter_next_frame_ = false;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##OutlinerFilter", tr("Search scene, bones, outputs"), filter_buf_.data(),
                                 filter_buf_.size());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", tr("Live-filters scene items, bones, materials, expressions and outputs (Ctrl+F)"));
        }

        draw_scene_well(avatar, fonts, selection, actions, scale);
        ImGui::Spacing();

        if (mode == StudioMode::Rigging && avatar.has_avatar) {
            // Clipped bone hierarchy (§12.15 ImGuiListClipper for >50 bones)
            if (ImGui::CollapsingHeader(tr("Skeleton"), ImGuiTreeNodeFlags_DefaultOpen)) {
                size_t visible_cnt = 0;
                for (size_t i = 0; i < bones.size() && visible_cnt < filtered_bone_indices_.size(); ++i) {
                    if (icase_contains(bones[i].name.data(), filter_buf_.data())) {
                        filtered_bone_indices_[visible_cnt++] = static_cast<uint32_t>(i);
                    }
                }

                if (ImGui::SmallButton(tr("Unhide All"))) {
                    for (auto& b : bones) {
                        b.hidden = false;
                    }
                }
                ImGui::SameLine();
                ImGui::TextColored(t.text_tertiary, "%zu bones", visible_cnt);

                ImGui::PushStyleColor(ImGuiCol_ChildBg, t.bg_well);
                if (ImGui::BeginChild("##BoneClip", ImVec2(0.0f, std::round(180.0f * scale)),
                                      ImGuiChildFlags_Borders)) {
                    ScopedRowColors row_colors;
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(visible_cnt));
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            OutlinerBoneInfo& b = bones[filtered_bone_indices_[static_cast<size_t>(row)]];
                            ImGui::PushID(static_cast<int>(b.bone_id));
                            if (renaming_bone_idx_ == row) {
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                if (ImGui::InputText("##ren", rename_buf_.data(), rename_buf_.size(),
                                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                                    std::snprintf(b.name.data(), b.name.size(), "%s", rename_buf_.data());
                                    renaming_bone_idx_ = -1;
                                }
                            } else {
                                const bool sel = (selection.kind == SelectionKind::Bone &&
                                                  selection.item_index == static_cast<int32_t>(b.bone_id));
                                const ImVec4 col =
                                    b.hidden ? t.text_disabled : (b.humanoid_mapped ? t.text_primary : t.text_secondary);
                                ImGui::PushStyleColor(ImGuiCol_Text, col);
                                char line[96]{};
                                std::snprintf(line, sizeof(line), "%s %s", b.hidden ? icons::kEyeOff : icons::kBone,
                                              b.name.data());
                                if (ImGui::Selectable(line, sel)) {
                                    set_selection(selection, SelectionKind::Bone, static_cast<int32_t>(b.bone_id),
                                                  b.name.data());
                                }
                                ImGui::PopStyleColor();
                                if (sel && ImGui::IsWindowFocused()) {
                                    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                                        renaming_bone_idx_ = row;
                                        std::snprintf(rename_buf_.data(), rename_buf_.size(), "%s", b.name.data());
                                    } else if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
                                        b.hidden = !b.hidden;
                                    }
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }

            if (ImGui::CollapsingHeader(tr("Materials"))) {
                ScopedRowColors row_colors;
                for (size_t i = 0; i < materials.size() && i < 64; ++i) {
                    auto& m = materials[i];
                    if (!icase_contains(m.name.data(), filter_buf_.data())) {
                        continue;
                    }
                    const bool sel = (selection.kind == SelectionKind::Material &&
                                      selection.item_index == static_cast<int32_t>(m.index));
                    char mlbl[96]{};
                    std::snprintf(mlbl, sizeof(mlbl), "%s %s##mat%u", icons::kLayers, m.name.data(), m.index);
                    if (ImGui::Selectable(mlbl, sel)) {
                        set_selection(selection, SelectionKind::Material, static_cast<int32_t>(m.index), m.name.data());
                    }
                }
            }
        }

        // Avatar source bar (disabled while ON AIR to protect the live stream §12.4)
        if (ImGui::CollapsingHeader(tr("Avatar Source"))) {
            ImGui::BeginDisabled(on_air || avatar.is_loading);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##AvatarPath", tr("model path (.vrm/.glb/.fbx/.pmx)"), path_buf_.data(),
                                     path_buf_.size());
            const float half_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(tr("Load"), ImVec2(half_w, 0.0f))) {
                actions.viewport.request_load_path = path_buf_.data();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Browse..."), ImVec2(half_w, 0.0f))) {
                actions.request_open_avatar_dialog = true;
            }
            if (sample_ref_vrm_[0] != '\0' || sample_fbx_[0] != '\0' || sample_pmx_[0] != '\0') {
                ImGui::TextColored(t.text_tertiary, "%s", tr("Samples"));
                ImGui::SameLine();
                if (sample_ref_vrm_[0] != '\0' && ImGui::SmallButton("Ref VRM")) {
                    std::snprintf(path_buf_.data(), path_buf_.size(), "%s", sample_ref_vrm_.data());
                    actions.viewport.request_load_path = path_buf_.data();
                }
                if (sample_fbx_[0] != '\0') {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("FBX")) {
                        std::snprintf(path_buf_.data(), path_buf_.size(), "%s", sample_fbx_.data());
                        actions.viewport.request_load_path = path_buf_.data();
                    }
                }
                if (sample_pmx_[0] != '\0') {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("PMX")) {
                        std::snprintf(path_buf_.data(), path_buf_.size(), "%s", sample_pmx_.data());
                        actions.viewport.request_load_path = path_buf_.data();
                    }
                }
            }
            ImGui::EndDisabled();
            if (on_air) {
                ImGui::TextColored(t.text_tertiary, "%s", tr("Avatar replacement locked while ON AIR."));
            }
            if (avatar.load_error != nullptr && avatar.load_error[0] != '\0') {
                (void)draw_inline_error_banner(fonts, tr("Avatar load failed."), avatar.load_error, tr("Retry"));
            }
        }

        // Quick expressions (F1-F8): compact key badge + label grid
        if (ImGui::CollapsingHeader(tr("Expressions (F1-F8)"), ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_expression_grid(expressions, fonts, selection, scale);
            ImGui::Spacing();
        }

        // Outputs quick status & toggle (§12.7)
        if (ImGui::CollapsingHeader(tr("Outputs"), ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_outputs(output_view, params, undo, fonts, selection, actions, scale);
            ImGui::Spacing();
        }

        // Plugins summary (§12.13 empty state when none installed)
        if (ImGui::CollapsingHeader(tr("Plugins"))) {
            if (plugins.empty()) {
                ImGui::TextColored(t.text_tertiary, "%s", tr("No plugins installed."));
                if (ghost_button(tr("Reload Plugins"), icons::kRefresh, ImVec2(-FLT_MIN, 0.0f))) {
                    actions.request_reload_plugins = true;
                }
            } else {
                ScopedRowColors row_colors;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (size_t i = 0; i < plugins.size(); ++i) {
                    const auto& plg = plugins[i];
                    if (!icase_contains(plg.name.data(), filter_buf_.data())) {
                        continue;
                    }
                    const bool sel =
                        (selection.kind == SelectionKind::Plugin && selection.item_index == static_cast<int32_t>(i));
                    char plbl[128]{};
                    std::snprintf(plbl, sizeof(plbl), "%s [%s] %s##plg%zu", icons::kLink, plg.kind.data(),
                                  plg.name.data(), i);
                    if (ImGui::Selectable(plbl, sel)) {
                        set_selection(selection, SelectionKind::Plugin, static_cast<int32_t>(i), plg.name.data());
                    }
                    const PillTone tone = plg.permanently_disabled ? PillTone::Error
                                          : plg.faulted            ? PillTone::Standby
                                          : plg.enabled            ? PillTone::Ok
                                                                   : PillTone::Off;
                    const char* st = plg.permanently_disabled ? "Disabled"
                                     : plg.faulted            ? "Faulted"
                                     : plg.enabled            ? "Active"
                                                              : "Off";
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    const ImVec2 psz = status_pill_size(st);
                    draw_status_pill_at(dl, ImVec2(rmax.x - psz.x, rmin.y + (rmax.y - rmin.y - psz.y) * 0.5f), st,
                                        tone);
                }
            }
        }
    }
    ImGui::End();
}

} // namespace aether::ui
