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

/** Copies `text` into `out`, eliding with "..." to fit `max_w` px without splitting a UTF-8 code point
 *  (Japanese file / bone names). Returns true when the text was shortened. */
bool elide_into(const char* text, float max_w, char* out, size_t cap) noexcept {
    std::snprintf(out, cap, "%s", text != nullptr ? text : "");
    if (ImGui::CalcTextSize(out).x <= max_w) {
        return false;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    size_t len = std::strlen(out);
    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<unsigned char>(out[len]) & 0xC0U) == 0x80U) {
            --len; // back up to the lead byte of this code point
        }
        out[len] = '\0';
        if (ImGui::CalcTextSize(out).x + dots <= max_w) {
            break;
        }
    }
    if (len + 4U <= cap) {
        std::snprintf(out + len, cap - len, "...");
    }
    return true;
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
                char row_id[32]{};
                std::snprintf(row_id, sizeof(row_id), "##scene%d", static_cast<int>(k) * 64 + idx);
                if (ImGui::Selectable(row_id, sel)) {
                    set_selection(selection, k, idx, label);
                }
                const bool row_hovered = ImGui::IsItemHovered();
                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                ImDrawList* row_dl = ImGui::GetWindowDrawList();
                // Badge is right-aligned; the label is elided so it always ends before the badge (no overdraw).
                float badge_w = 0.0f;
                if (badge != nullptr && badge[0] != '\0') {
                    const ImVec2 bsz = ImGui::CalcTextSize(badge);
                    badge_w = bsz.x + 12.0f * scale;
                    row_dl->AddText(ImVec2(std::floor(rmax.x - bsz.x - 4.0f * scale),
                                           std::floor(rmin.y + (rmax.y - rmin.y - bsz.y) * 0.5f)),
                                    t.col32_text_tertiary, badge);
                }
                char full_lbl[192]{};
                std::snprintf(full_lbl, sizeof(full_lbl), "%s  %s", icon, label);
                char vis_lbl[192]{};
                const bool elided =
                    elide_into(full_lbl, std::max(8.0f, rmax.x - rmin.x - badge_w), vis_lbl, sizeof(vis_lbl));
                const float text_h = ImGui::GetTextLineHeight();
                row_dl->AddText(ImVec2(rmin.x, std::floor(rmin.y + (rmax.y - rmin.y - text_h) * 0.5f)),
                                ImGui::GetColorU32(ImGuiCol_Text), vis_lbl);
                if (elided && row_hovered) {
                    ImGui::SetTooltip("%s", label);
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
            draw_node(SelectionKind::StudioLight, 0, icons::kSun, tr("Studio Lighting Rig"), "16-Slot");
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
    (void)on_air;
    (void)expressions;
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
                                const ImVec4 col = b.hidden ? t.text_disabled
                                                            : (b.humanoid_mapped ? t.text_primary : t.text_secondary);
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
                                    if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_F2, false)) {
                                        renaming_bone_idx_ = row;
                                        std::snprintf(rename_buf_.data(), rename_buf_.size(), "%s", b.name.data());
                                    } else if (KeymapRegistry::is_raw_key_pressed(ImGuiKey_H, false)) {
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

        // Outliner owns scene hierarchy, outputs, and plugins in Rev 3.1.
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
