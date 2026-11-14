#include "ui/panels/outliner/outliner_panel.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

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

void OutlinerPanel::draw(StudioMode mode, bool on_air, const DevShellAvatarView& avatar,
                         const DevShellOutputView& output_view, std::span<OutlinerBoneInfo> bones,
                         std::span<OutlinerMaterialInfo> materials, std::span<ExpressionSlotInfo> expressions,
                         std::span<const LoadedPluginVm> plugins, ParamStore& params, UndoStack& undo,
                         const UiFontSet& fonts, UiSelection& selection, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();

    if (ImGui::Begin(tr("Outliner & Project"))) {
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

        // Filter row (Ctrl+F focused)
        if (focus_filter_next_frame_) {
            ImGui::SetKeyboardFocusHere();
            focus_filter_next_frame_ = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##OutlinerFilter", tr("Search scene, bones, outputs..."), filter_buf_.data(),
                                 filter_buf_.size());

        ImGui::Spacing();
        if (!avatar.has_avatar && !avatar.is_loading) {
            if (draw_empty_state(fonts, tr("No avatar loaded"), tr("Open Avatar..."))) {
                actions.request_open_avatar_dialog = true;
            }
            if (sample_ref_vrm_[0] != '\0') {
                ImGui::Spacing();
                if (ImGui::Button(tr("Load Reference VRM"), ImVec2(-1.0f, t.control_h_compact_px))) {
                    std::snprintf(path_buf_.data(), path_buf_.size(), "%s", sample_ref_vrm_.data());
                    actions.viewport.request_load_path = path_buf_.data();
                }
            }
        } else {
            // Scene hierarchy tree (§12.7)
            if (ImGui::CollapsingHeader(tr("Scene"), ImGuiTreeNodeFlags_DefaultOpen)) {
                auto draw_node = [&](SelectionKind k, int32_t idx, const char* icon, const char* label,
                                     const char* badge) {
                    if (!icase_contains(label, filter_buf_.data())) {
                        return;
                    }
                    const bool sel = (selection.kind == k && selection.item_index == idx);
                    char row_lbl[128]{};
                    if (badge != nullptr && badge[0] != '\0') {
                        std::snprintf(row_lbl, sizeof(row_lbl), "%s %s  [%s]##%d", icon, label, badge,
                                      static_cast<int>(k) * 64 + idx);
                    } else {
                        std::snprintf(row_lbl, sizeof(row_lbl), "%s %s##%d", icon, label,
                                      static_cast<int>(k) * 64 + idx);
                    }
                    if (ImGui::Selectable(row_lbl, sel)) {
                        set_selection(selection, k, idx, label);
                    }
                };

                char avatar_badge[32]{};
                std::snprintf(avatar_badge, sizeof(avatar_badge), "%u/%u", avatar.mapped_humanoid_bones,
                              avatar.joint_count);
                draw_node(SelectionKind::Avatar, 0, icons::kBox, avatar.format_name, avatar_badge);
                draw_node(SelectionKind::LightKey, 0, icons::kSun, tr("Key Light"), nullptr);
                draw_node(SelectionKind::LightRim, 0, icons::kSun, tr("Rim Light"), nullptr);
                draw_node(SelectionKind::LightFill, 0, icons::kSun, tr("Fill Light"), nullptr);
                draw_node(SelectionKind::Camera, 0, icons::kCamera, tr("Studio Camera"), nullptr);
            }

            if (mode == StudioMode::Rigging) {
                // Clipped bone hierarchy (§12.15 ImGuiListClipper for >50 bones)
                if (ImGui::CollapsingHeader(tr("Skeleton"), ImGuiTreeNodeFlags_DefaultOpen)) {
                    static std::array<uint32_t, 512> s_filtered_indices{};
                    size_t visible_cnt = 0;
                    for (size_t i = 0; i < bones.size() && visible_cnt < s_filtered_indices.size(); ++i) {
                        if (icase_contains(bones[i].name.data(), filter_buf_.data())) {
                            s_filtered_indices[visible_cnt++] = static_cast<uint32_t>(i);
                        }
                    }

                    if (ImGui::SmallButton(tr("Unhide All"))) {
                        for (auto& b : bones) {
                            b.hidden = false;
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(t.text_tertiary, "%zu bones", visible_cnt);

                    ImGui::BeginChild("##BoneClip", ImVec2(0.0f, 180.0f), true);
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(visible_cnt));
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            OutlinerBoneInfo& b = bones[s_filtered_indices[static_cast<size_t>(row)]];
                            ImGui::PushID(static_cast<int>(b.bone_id));
                            if (renaming_bone_idx_ == row) {
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
                    ImGui::EndChild();
                }

                if (ImGui::CollapsingHeader(tr("Materials"))) {
                    for (size_t i = 0; i < materials.size() && i < 64; ++i) {
                        auto& m = materials[i];
                        const bool sel = (selection.kind == SelectionKind::Material &&
                                          selection.item_index == static_cast<int32_t>(m.index));
                        char mlbl[96]{};
                        std::snprintf(mlbl, sizeof(mlbl), "%s %s##mat%u", icons::kLayers, m.name.data(), m.index);
                        if (ImGui::Selectable(mlbl, sel)) {
                            set_selection(selection, SelectionKind::Material, static_cast<int32_t>(m.index),
                                          m.name.data());
                        }
                    }
                }
            }
        }

        // Avatar quick source bar (disabled while ON AIR to protect live stream §12.4)
        if (ImGui::CollapsingHeader(tr("Avatar Source"))) {
            ImGui::BeginDisabled(on_air || avatar.is_loading);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##AvatarPath", tr("model path (.vrm/.glb/.fbx/.pmx)"), path_buf_.data(),
                                     path_buf_.size());
            if (ImGui::Button(tr("Load"), ImVec2(64.0f, t.control_h_compact_px))) {
                actions.viewport.request_load_path = path_buf_.data();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Browse..."), ImVec2(76.0f, t.control_h_compact_px))) {
                actions.request_open_avatar_dialog = true;
            }
            if (on_air) {
                ImGui::TextColored(t.text_tertiary, "%s", tr("Avatar replacement locked while ON AIR."));
            }
            if (sample_ref_vrm_[0] != '\0' || sample_fbx_[0] != '\0' || sample_pmx_[0] != '\0') {
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

            if (avatar.load_error != nullptr && avatar.load_error[0] != '\0') {
                (void)draw_inline_error_banner(fonts, tr("Avatar load failed."), avatar.load_error, tr("Retry"));
            }
        }

        // Expressions trigger pad (F1-F8)
        if (ImGui::CollapsingHeader(tr("Expressions (F1-F8)"), ImGuiTreeNodeFlags_DefaultOpen)) {
            const float btn_w = std::max(72.0f, (ImGui::GetContentRegionAvail().x - t.unit_px) * 0.5f);
            for (size_t i = 0; i < expressions.size() && i < 8; ++i) {
                auto& ex = expressions[i];
                if ((i % 2) != 0) {
                    ImGui::SameLine();
                }
                ImGui::PushStyleColor(ImGuiCol_Button, ex.triggered_override ? t.bg_selected : t.bg_raised);
                ImGui::PushStyleColor(ImGuiCol_Border, ex.triggered_override ? t.accent : t.border_default);
                char blbl[64]{};
                std::snprintf(blbl, sizeof(blbl), "%s %s", ex.hotkey.data(), ex.name.data());
                if (ImGui::Button(blbl, ImVec2(btn_w, t.control_h_compact_px))) {
                    actions.request_trigger_expression = true;
                    actions.expression_slot = static_cast<int32_t>(ex.slot);
                    actions.expression_weight = ex.triggered_override ? 0.0f : 1.0f;
                    set_selection(selection, SelectionKind::Expression, static_cast<int32_t>(ex.slot), ex.name.data());
                }
                ImGui::PopStyleColor(2);
            }
        }

        // Outputs quick status & toggle (§12.7)
        if (ImGui::CollapsingHeader(tr("Outputs"), ImGuiTreeNodeFlags_DefaultOpen)) {
            auto out_toggle = [&](const char* label, ParamId pid, bool live_active, const char* status_note) {
                bool on = params.get_bool(pid);
                if (ImGui::Checkbox(label, &on)) {
                    const double old_v = params.get(pid);
                    const double new_v = on ? 1.0 : 0.0;
                    if (params.set(pid, new_v)) {
                        undo.push_param(pid, old_v, new_v);
                        actions.viewport.output_config_dirty = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextColored(live_active ? t.status_ok : (on ? t.status_warn : t.text_disabled), "%s %s",
                                   icons::kRadioOn, status_note);
            };

            out_toggle(tr("Spout2"), ParamId::OutputSpoutEnabled, output_view.spout_active,
                       output_view.spout_using_cpu_fallback ? "CPU fallback"
                                                            : (output_view.spout_active ? "Active" : "Idle"));
            if (params.get_bool(ParamId::OutputSpoutEnabled) && !output_view.spout_active &&
                output_view.spout_adapter_mismatch) {
                if (draw_inline_error_banner(fonts, tr("Spout2 GPU sharing mismatch."), tr("OBS on another adapter."),
                                             tr("Use CPU sharing"))) {
                    const double old_v = params.get(ParamId::OutputSpoutCpuFallback);
                    if (params.set(ParamId::OutputSpoutCpuFallback, 1.0)) {
                        undo.push_param(ParamId::OutputSpoutCpuFallback, old_v, 1.0);
                        actions.viewport.output_config_dirty = true;
                    }
                }
            }
            out_toggle(tr("Virtual Cam"), ParamId::OutputVcamEnabled, output_view.vcam_active,
                       output_view.vcam_active ? "Streaming" : "Idle");
            out_toggle(tr("NDI"), ParamId::OutputNdiEnabled, output_view.ndi_active,
                       output_view.ndi_runtime_available ? (output_view.ndi_active ? "Online" : "Ready")
                                                         : "No NDI runtime");
            out_toggle(tr("OSC 9001/9002"), ParamId::OutputOscEnabled, output_view.osc_active,
                       output_view.osc_active ? "Listening" : "Off");

            if (ImGui::Selectable(tr("Inspect Output Settings..."), selection.kind == SelectionKind::OutputDest)) {
                set_selection(selection, SelectionKind::OutputDest, 0, tr("Outputs & Audio DSP"));
            }
        }

        // Plugins summary (§12.13 empty state when none installed)
        if (ImGui::CollapsingHeader(tr("Plugins"))) {
            if (plugins.empty()) {
                if (draw_empty_state(fonts, tr("No plugins installed"), tr("Reload Plugins"))) {
                    actions.request_reload_plugins = true;
                }
            } else {
                for (size_t i = 0; i < plugins.size(); ++i) {
                    const auto& plg = plugins[i];
                    const bool sel =
                        (selection.kind == SelectionKind::Plugin && selection.item_index == static_cast<int32_t>(i));
                    const ImVec4 st_col =
                        plg.permanently_disabled ? t.status_error : (plg.faulted ? t.status_warn : t.status_ok);
                    ImGui::PushStyleColor(ImGuiCol_Text, st_col);
                    char plbl[128]{};
                    std::snprintf(plbl, sizeof(plbl), "%s [%s] %s", icons::kLink, plg.kind.data(), plg.name.data());
                    const bool clicked = ImGui::Selectable(plbl, sel);
                    ImGui::PopStyleColor();
                    if (clicked) {
                        set_selection(selection, SelectionKind::Plugin, static_cast<int32_t>(i), plg.name.data());
                    }
                }
            }
        }
    }
    ImGui::End();
}

} // namespace aether::ui
