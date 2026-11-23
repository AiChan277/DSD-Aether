#include "ui/panels/inspector/inspector_panel.hpp"

#include "core/alfx_serializer.hpp"
#include "renderer/lighting_rig.hpp"
#include "ui/panels/nodegraph/node_canvas.hpp"
#include "ui/panels/viewport/camera_presets.hpp"
#include "ui/panels/viewport/viewport_panel.hpp"
#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

const char* selection_kind_label(SelectionKind k) noexcept {
    switch (k) {
    case SelectionKind::Avatar:
        return "Avatar";
    case SelectionKind::StudioLight:
        return "Studio Light";
    case SelectionKind::Camera:
        return "Camera";
    case SelectionKind::Bone:
        return "Bone";
    case SelectionKind::Material:
        return "Material";
    case SelectionKind::Expression:
        return "Expression";
    case SelectionKind::Node:
        return "Graph Node";
    case SelectionKind::Plugin:
        return "Plugin";
    case SelectionKind::OutputDest:
        return "Output";
    default:
        return "Scene";
    }
}

/** Collapsing section header; `force_open` re-opens a section the user collapsed (Inspect Output Settings). */
bool section_header(const char* label, bool default_open, bool force_open) {
    if (force_open) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    return ImGui::CollapsingHeader(label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
}

} // namespace

void InspectorPanel::draw(StudioMode mode, bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi,
                          bool ffi_layout_ok, const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                          const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                          const StudioHostStatus& host_status, std::span<const LoadedPluginVm> plugins,
                          const UiSelection& selection, ParamStore& params, UndoStack& undo, AppSettings& settings,
                          bool& settings_dirty, const UiFontSet& fonts, PropertyRowActions& row_actions,
                          StudioActions& actions, ViewportPanel* viewport_for_presets,
                          std::span<OutlinerBoneInfo> bones, std::span<OutlinerMaterialInfo> materials,
                          NodeCanvas* node_canvas) {
    const PrecisionDarkTokens& t = tokens();
    const bool focus_output = focus_output_pending_;
    if (focus_output) {
        ImGui::SetNextWindowFocus();
    }

    if (ImGui::Begin(tr("Inspector"))) {
        const float full_w = -FLT_MIN;

        // Header: selection title + kind tag
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s", selection.name[0] != '\0' ? selection.name.data() : tr("Inspector"));
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::SameLine();
        ImGui::TextColored(t.text_tertiary, "%s", selection_kind_label(selection.kind));
        ImGui::Spacing();

        auto draw_selected_bone_or_material = [&]() {
            if (selection.kind == SelectionKind::Bone && selection.item_index >= 0) {
                for (auto& b : bones) {
                    if (b.bone_id == static_cast<uint32_t>(selection.item_index)) {
                        char id_buf[32]{};
                        std::snprintf(id_buf, sizeof(id_buf), "#%u", b.bone_id);
                        draw_status_badge(fonts, tr("Bone ID"), id_buf, t.text_primary);
                        draw_status_badge(fonts, tr("Humanoid"),
                                          b.humanoid_mapped ? tr("Mapped (55-Joint Rig)") : tr("Secondary / Accessory"),
                                          b.humanoid_mapped ? t.status_ok : t.text_secondary);
                        char conf_buf[32]{};
                        std::snprintf(conf_buf, sizeof(conf_buf), "%.0f%%", static_cast<double>(b.confidence * 100.0f));
                        draw_status_badge(fonts, tr("Tracking Conf"), conf_buf, t.text_primary);
                        ImGui::Checkbox(tr("Hide Bone in Viewport"), &b.hidden);
                        ImGui::Spacing();
                        break;
                    }
                }
            } else if (selection.kind == SelectionKind::Material && selection.item_index >= 0) {
                for (auto& m : materials) {
                    if (m.index == static_cast<uint32_t>(selection.item_index)) {
                        char idx_buf[32]{};
                        std::snprintf(idx_buf, sizeof(idx_buf), "Slot %u", m.index);
                        draw_status_badge(fonts, tr("Material"), idx_buf, t.text_primary);
                        ImGui::Checkbox(tr("Hide Material Submeshes"), &m.hidden);
                        ImGui::Spacing();
                        break;
                    }
                }
            }
        };

        auto draw_plugin_health = [&]() {
            if (selection.kind == SelectionKind::Plugin && selection.item_index >= 0 &&
                static_cast<size_t>(selection.item_index) < plugins.size()) {
                const LoadedPluginVm& plg = plugins[static_cast<size_t>(selection.item_index)];
                char id_buf[160]{};
                std::snprintf(id_buf, sizeof(id_buf), "%s (%s)", plg.id.data(), plg.kind.data());
                draw_status_badge(fonts, tr("ID"), id_buf, t.text_primary);
                char st_buf[192]{};
                std::snprintf(st_buf, sizeof(st_buf), "%s · Restarts %u · Traps %u", plg.status_text.data(),
                              plg.restart_count, plg.trap_count);
                draw_status_badge(fonts, tr("State"), st_buf,
                                  plg.permanently_disabled ? t.status_error
                                                           : (plg.faulted ? t.status_warn : t.status_ok));
                if (plg.permanently_disabled && plg.native_slot >= 0) {
                    if (ImGui::Button(tr("Retry Disabled Plugin"), ImVec2(full_w, 0.0f))) {
                        actions.request_reset_native_plugin = true;
                        actions.reset_native_slot = plg.native_slot;
                    }
                }
                ImGui::Spacing();
            }
        };

        auto draw_transform_physics = [&](bool default_open) {
            if (section_header(tr("Transform & Physics"), default_open, false)) {
                if (begin_property_table("##xform")) {
                    draw_property_row(ParamId::AvatarPosX, params, undo, fonts, row_actions, selection.multi_select);
                    draw_property_row(ParamId::AvatarPosY, params, undo, fonts, row_actions, selection.multi_select);
                    draw_property_row(ParamId::AvatarPosZ, params, undo, fonts, row_actions, selection.multi_select);
                    draw_property_row(ParamId::AvatarRotY, params, undo, fonts, row_actions, selection.multi_select);
                    draw_property_row(ParamId::AvatarScale, params, undo, fonts, row_actions, selection.multi_select);
                    draw_property_row(ParamId::SimSpringStiffness, params, undo, fonts, row_actions);
                    draw_property_row(ParamId::SimSpringDamping, params, undo, fonts, row_actions);
                    draw_property_row(ParamId::SimGain, params, undo, fonts, row_actions);
                    draw_property_row(ParamId::SimIkWeight, params, undo, fonts, row_actions);
                    end_property_table();
                }
                if (avatar.has_avatar) {
                    ImGui::BeginDisabled(on_air);
                    if (ImGui::Button(tr("Bake Face SDF Texture"), ImVec2(full_w, 0.0f))) {
                        actions.viewport.request_bake_face_sdf = true;
                    }
                    ImGui::EndDisabled();
                    if (render_view.baked_face_sdf_applied) {
                        draw_status_badge(fonts, icons::kDomainSdf, tr("Baked UV Face SDF active"), t.status_ok);
                    }
                }
                ImGui::Spacing();
            }
        };

        auto draw_camera_framing = [&](bool default_open) {
            // Rev 3.1 §1.2-1.3: Viewport toolbar owns aspect lock & framing preset grid; Inspector keeps SmoothDamp &
            // Custom Presets.
            if (section_header(tr("Camera & Framing"), default_open, false)) {
                if (begin_property_table("##camera_framing")) {
                    if (draw_property_row(ParamId::AppCameraTransitionSpeed, params, undo, fonts, row_actions)) {
                        settings_dirty = true;
                    }
                    end_property_table();
                }
                if (viewport_for_presets != nullptr) {
                    ImGui::Spacing();
                    const float avail_w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
                    const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
                    const float half_w = std::floor((avail_w - spacing_x) * 0.5f);
                    if (ImGui::Button(tr("Reset Camera"), ImVec2(half_w, 0.0f))) {
                        viewport_for_presets->reset_camera(actions);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(tr("+ Save Preset"), ImVec2(half_w, 0.0f))) {
                        viewport_for_presets->open_save_preset_modal();
                    }

                    const int32_t active_bi = viewport_for_presets->active_builtin_preset();
                    const auto& customs = viewport_for_presets->preset_bank().custom_presets();
                    if (!customs.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(t.text_secondary, "%s", tr("Custom Presets"));
                        const float btn_save_w =
                            ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float btn_del_w = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                        const float name_w = std::max(60.0f, avail_w - btn_save_w - btn_del_w - spacing_x * 2.0f);
                        for (size_t i = 0; i < customs.size(); ++i) {
                            const auto& cp = customs[i];
                            const bool is_cur =
                                (viewport_for_presets->active_preset_name() == cp.name && active_bi < 0);
                            char apply_lbl[80]{};
                            std::snprintf(apply_lbl, sizeof(apply_lbl), "%s##cp_apply_%zu", cp.name.c_str(), i);
                            if (is_cur) {
                                ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
                                ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
                            }
                            if (ImGui::Button(apply_lbl, ImVec2(name_w, 0.0f))) {
                                viewport_for_presets->apply_custom_preset(i, params, undo, actions);
                            }
                            if (is_cur) {
                                ImGui::PopStyleColor(2);
                            }
                            ImGui::SameLine();
                            char save_lbl[32]{};
                            std::snprintf(save_lbl, sizeof(save_lbl), "Save##cp_save_%zu", i);
                            if (ImGui::Button(save_lbl, ImVec2(btn_save_w, 0.0f))) {
                                viewport_for_presets->save_custom_preset_from_current(cp.name, avatar, params);
                                break;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                                ImGui::SetTooltip("%s", tr("Overwrite this preset with current camera & AvatarRoot"));
                            }
                            ImGui::SameLine();
                            char del_lbl[32]{};
                            std::snprintf(del_lbl, sizeof(del_lbl), "x##cp_del_%zu", i);
                            if (ImGui::Button(del_lbl, ImVec2(btn_del_w, 0.0f))) {
                                viewport_for_presets->preset_bank().remove_at(i,
                                                                              viewport_for_presets->persist_presets());
                                break;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                                ImGui::SetTooltip("%s", tr("Delete custom preset"));
                            }
                        }
                    }
                }
                ImGui::Spacing();
            }
        };

        auto draw_lighting_summary = [&](bool default_open) {
            // Active Lighting & FX (.alfx) summary + quick preset selector (Rev 3.1: single authoring home is Lighting
            // tab)
            if (section_header(tr("Lighting & Effects (.alfx)"), default_open, false)) {
                static constexpr const char* kModeLabels[3] = {"Traditional (Fast)", "Hybrid Ray Tracing",
                                                               "2.5D Path Tracing"};
                const int lm = std::clamp(params.get_int(ParamId::RenderLightingMode), 0, 2);
                const char* cur_alfx =
                    host_status.active_alfx_name[0] != '\0' ? host_status.active_alfx_name.data() : "Custom Look";
                const auto& alfx_catalog = core::AlfxSerializer::builtin_preset_catalog();
                ImGui::SetNextItemWidth(full_w);
                if (ImGui::BeginCombo("##InspAlfxPreset", cur_alfx)) {
                    for (size_t p = 0; p < alfx_catalog.size(); ++p) {
                        const bool sel = (std::strcmp(cur_alfx, alfx_catalog[p].display_name) == 0);
                        if (ImGui::Selectable(alfx_catalog[p].display_name, sel)) {
                            actions.request_apply_alfx_preset_index = static_cast<int32_t>(p);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::Selectable(tr("Import .alfx..."))) {
                        actions.request_import_alfx_dialog = true;
                    }
                    ImGui::EndCombo();
                }

                char summary_buf[160]{};
                std::snprintf(summary_buf, sizeof(summary_buf), "%s · %s", cur_alfx, kModeLabels[lm]);
                draw_status_badge(fonts, tr("Active Look"), summary_buf, t.text_primary);
                if (params.get_bool(ParamId::RenderCircadianEnabled)) {
                    draw_status_badge(fonts, tr("Circadian"), tr("Active in Lighting Workspace"), t.status_ok);
                }
                if (ImGui::Button(tr("Edit in Lighting Workspace ->"), ImVec2(full_w, 0.0f))) {
                    actions.request_switch_mode = true;
                    actions.target_mode = StudioMode::Lighting;
                }
                ImGui::Spacing();
            }
        };

        auto draw_tracking_pipeline = [&](bool default_open) {
            if (section_header(tr("Tracking & Pipeline"), default_open, false)) {
                if (begin_property_table("##tracking")) {
                    if (draw_property_row(ParamId::TrackingEpMode, params, undo, fonts, row_actions)) {
                        const int mode_idx = params.get_int(ParamId::TrackingEpMode);
                        actions.viewport.request_ep_override = tracking_ep_mode_to_router_override(mode_idx);
                        settings_dirty = true;
                    }
                    draw_property_row(ParamId::TrackingSmoothingCutoffHz, params, undo, fonts, row_actions);
                    if (draw_property_row(ParamId::TrackingSyntheticCamera, params, undo, fonts, row_actions)) {
                        actions.viewport.request_set_synthetic_camera = true;
                        actions.viewport.target_synthetic_camera = params.get_bool(ParamId::TrackingSyntheticCamera);
                        settings_dirty = true;
                    }
                    draw_property_row(ParamId::TrackingAntiFlicker, params, undo, fonts, row_actions);
                    draw_property_row(ParamId::TrackingExposureLock, params, undo, fonts, row_actions);
                    if (draw_property_row(ParamId::TrackingHandEnabled, params, undo, fonts, row_actions)) {
                        actions.viewport.request_set_hand_tracking = true;
                        actions.viewport.target_hand_tracking_enabled = params.get_bool(ParamId::TrackingHandEnabled);
                        settings_dirty = true;
                    }
                    if (draw_property_row(ParamId::TrackingAudioAssist, params, undo, fonts, row_actions) ||
                        draw_property_row(ParamId::TrackingMicGain, params, undo, fonts, row_actions) ||
                        draw_property_row(ParamId::TrackingMicNoiseGate, params, undo, fonts, row_actions)) {
                        actions.viewport.audio_assist_enabled = params.get_bool(ParamId::TrackingAudioAssist);
                        actions.viewport.mic_gain = params.get_float(ParamId::TrackingMicGain);
                        actions.viewport.mic_noise_gate_db = params.get_float(ParamId::TrackingMicNoiseGate);
                        actions.viewport.audio_lipsync_dirty = true;
                        settings_dirty = true;
                    }
                    static constexpr ParamId kNpuBoundRows[] = {
                        ParamId::TrackingNpuBoundsEnabled,  ParamId::TrackingNpuAntiClipEnabled,
                        ParamId::TrackingNpuSafeBoxPreview, ParamId::TrackingNpuBoundsX,
                        ParamId::TrackingNpuBoundsY,        ParamId::TrackingNpuBoundsZ,
                        ParamId::TrackingNpuSoftElasticity, ParamId::TrackingNpuMaxPitchDeg,
                        ParamId::TrackingNpuAntiClipMargin,
                    };
                    bool bounds_changed = false;
                    for (const ParamId pid : kNpuBoundRows) {
                        bounds_changed |= draw_property_row(pid, params, undo, fonts, row_actions);
                    }
                    if (bounds_changed) {
                        actions.viewport.npu_bounds_enabled = params.get_bool(ParamId::TrackingNpuBoundsEnabled);
                        actions.viewport.npu_anti_clip_enabled = params.get_bool(ParamId::TrackingNpuAntiClipEnabled);
                        actions.viewport.npu_bounds_half = {params.get_float(ParamId::TrackingNpuBoundsX),
                                                            params.get_float(ParamId::TrackingNpuBoundsY),
                                                            params.get_float(ParamId::TrackingNpuBoundsZ)};
                        actions.viewport.npu_soft_elasticity = params.get_float(ParamId::TrackingNpuSoftElasticity);
                        actions.viewport.npu_max_pitch_deg = params.get_float(ParamId::TrackingNpuMaxPitchDeg);
                        actions.viewport.npu_anti_clip_margin = params.get_float(ParamId::TrackingNpuAntiClipMargin);
                        actions.viewport.npu_bounds_dirty = true;
                        settings_dirty = true;
                    }
                    end_property_table();
                }
                ImGui::Spacing();
                const int active_cam = active_camera_device_index(hw, tracking, settings);
                const char* cam_preview = (active_cam >= 0 && active_cam < static_cast<int>(hw.cameras.size()))
                                              ? hw.cameras[static_cast<size_t>(active_cam)].friendly_name.c_str()
                                              : tr("No Webcam Detected");
                ImGui::SetNextItemWidth(full_w * 0.65f);
                if (ImGui::BeginCombo("##InspCamCombo", cam_preview)) {
                    for (int i = 0; i < static_cast<int>(hw.cameras.size()); ++i) {
                        const auto& cam = hw.cameras[static_cast<size_t>(i)];
                        char lbl[192]{};
                        std::snprintf(lbl, sizeof(lbl), "%s%s##icam_%d", cam.friendly_name.c_str(),
                                      cam.likely_virtual ? " [Virtual]" : "", i);
                        const bool is_sel = (i == active_cam) && !tracking.using_synthetic_camera;
                        if (ImGui::Selectable(lbl, is_sel)) {
                            select_camera_device_action(cam, params, settings, settings_dirty, actions);
                        }
                        if (is_sel) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button(tracking.camera_active ? tr("Stop Cam") : tr("Start Cam"))) {
                    actions.viewport.request_set_camera_active = true;
                    actions.viewport.target_camera_active = !tracking.camera_active;
                }
                const int active_mic = active_audio_device_index(hw, tracking, settings);
                const char* mic_preview =
                    settings.preferred_mic_id.empty()
                        ? (active_mic >= 0 && active_mic < static_cast<int>(hw.audio_inputs.size())
                               ? hw.audio_inputs[static_cast<size_t>(active_mic)].friendly_name.c_str()
                               : tr("System Default"))
                        : (active_mic >= 0 && active_mic < static_cast<int>(hw.audio_inputs.size())
                               ? hw.audio_inputs[static_cast<size_t>(active_mic)].friendly_name.c_str()
                               : tracking.audio_device_label);
                ImGui::SetNextItemWidth(full_w * 0.65f);
                if (ImGui::BeginCombo("##InspMicCombo", mic_preview)) {
                    const bool def_sel = settings.preferred_mic_id.empty();
                    if (ImGui::Selectable(tr("System Default##imic_def"), def_sel)) {
                        select_audio_device_action("", settings, settings_dirty, actions);
                    }
                    for (int i = 0; i < static_cast<int>(hw.audio_inputs.size()); ++i) {
                        const auto& mic = hw.audio_inputs[static_cast<size_t>(i)];
                        char lbl[192]{};
                        std::snprintf(lbl, sizeof(lbl), "%s%s##imic_%d", mic.friendly_name.c_str(),
                                      mic.is_default ? " [Default]" : "", i);
                        const bool is_sel = !def_sel && (mic.device_id == settings.preferred_mic_id);
                        if (ImGui::Selectable(lbl, is_sel)) {
                            select_audio_device_action(mic.device_id, settings, settings_dirty, actions);
                        }
                        if (is_sel) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(tr("Audio Input"));
                ImGui::Spacing();
                char cam_buf[192]{};
                format_camera_status_badge(tracking, cam_buf, sizeof(cam_buf));
                draw_status_badge(fonts, tr("Camera"), cam_buf, tracking.camera_active ? t.status_ok : t.status_warn);
                char mic_buf[160]{};
                if (!tracking.audio_assist_enabled) {
                    std::snprintf(mic_buf, sizeof(mic_buf), "Disabled");
                } else if (!tracking.audio_mic_available) {
                    std::snprintf(mic_buf, sizeof(mic_buf), "%s (Webcam Fallback)",
                                  tracking.audio_device_label != nullptr ? tracking.audio_device_label : "No Mic");
                } else {
                    std::snprintf(mic_buf, sizeof(mic_buf), "%s · %.1f dB · Open %.2f",
                                  tracking.audio_vad ? "VAD Active" : "Silence Lock",
                                  static_cast<double>(tracking.audio_rms_db),
                                  static_cast<double>(tracking.audio_open_y));
                }
                draw_status_badge(fonts, tr("Audio Lip-Sync"), mic_buf,
                                  tracking.audio_vad ? t.status_ok
                                                     : (tracking.audio_mic_available ? t.text_primary : t.status_warn));
                char ep_buf[128]{};
                std::snprintf(ep_buf, sizeof(ep_buf), "%s · %llu frames", tracking.active_ep_label,
                              static_cast<unsigned long long>(tracking.frames_processed));
                draw_status_badge(fonts, tr("Execution Provider"), ep_buf,
                                  tracking.ready ? t.status_ok : t.text_tertiary);
                ImGui::Spacing();
            }
        };

        auto draw_output_dsp = [&](bool default_open, bool force_open) {
            if (section_header(tr("Output & Audio-DSP"), default_open, force_open)) {
                if (force_open) {
                    ImGui::SetScrollHereY(0.0f);
                }
                bool out_changed = false;
                if (begin_property_table("##output")) {
                    static constexpr ParamId kOutputRows[] = {
                        ParamId::OutputResolution,  ParamId::OutputFps,          ParamId::OutputPremulAlpha,
                        ParamId::OutputNonAlphaBg,  ParamId::OutputSpoutEnabled, ParamId::OutputSpoutCpuFallback,
                        ParamId::OutputVcamEnabled, ParamId::OutputNdiEnabled,   ParamId::OutputNdiFormat,
                        ParamId::OutputOscEnabled,
                    };
                    for (const ParamId pid : kOutputRows) {
                        out_changed |= draw_property_row(pid, params, undo, fonts, row_actions);
                    }
                    if (draw_property_row(ParamId::OutputWsEnabled, params, undo, fonts, row_actions)) {
                        actions.request_toggle_ws = true;
                        actions.target_ws_enabled = params.get_bool(ParamId::OutputWsEnabled);
                    }
                    out_changed |= draw_property_row(ParamId::DspUserOffsetMs, params, undo, fonts, row_actions);
                    end_property_table();
                }
                if (out_changed) {
                    actions.viewport.output_config_dirty = true;
                }
                ImGui::Spacing();
                char dsp_buf[128]{};
                std::snprintf(dsp_buf, sizeof(dsp_buf), "%s · Rec Delay %.1f ms · Peak %.1f dB",
                              output_view.dsp_connected ? "Linked" : "Offline",
                              static_cast<double>(output_view.dsp_recommended_delay_ms),
                              static_cast<double>(output_view.dsp_input_rms_db));
                draw_status_badge(fonts, tr("Audio-DSP"), dsp_buf,
                                  output_view.dsp_connected ? t.status_ok : t.text_tertiary);
                ImGui::Spacing();
            }
        };

        auto draw_hardware_contracts = [&]() {
            if (section_header(tr("Hardware & Contracts"), false, false)) {
                char buf[192]{};
                std::snprintf(buf, sizeof(buf), "%s (%uP / %uE)", hw.cpu.brand_string.c_str(), hw.cpu.p_core_count,
                              hw.cpu.e_core_count);
                draw_status_badge(fonts, "CPU", buf, t.text_primary);
                draw_status_badge(fonts, "GPU", rhi.gpu_info().device_name.c_str(), t.text_primary);
                draw_status_badge(fonts, "NPU",
                                  hw.npu.available
                                      ? (hw.npu.device_name.empty() ? "Available" : hw.npu.device_name.c_str())
                                      : "Not detected",
                                  hw.npu.available ? t.status_ok : t.text_tertiary);
                std::snprintf(buf, sizeof(buf), "%llu pending · %llu reclaimed · %.1f MB",
                              static_cast<unsigned long long>(render_view.gc_retired_pending),
                              static_cast<unsigned long long>(render_view.gc_reclaimed_total),
                              static_cast<double>(render_view.gc_bytes_reclaimed) / (1024.0 * 1024.0));
                draw_status_badge(fonts, "GC", buf, t.text_primary);
                std::snprintf(buf, sizeof(buf), "%s · Host %s", ffi_layout_ok ? "VERIFIED" : "MISMATCH",
                              host_status.host_alive ? "Online" : "Offline");
                draw_status_badge(fonts, "FFI ABI", buf, ffi_layout_ok ? t.status_ok : t.status_error);
                ImGui::Spacing();
                ImGui::BeginDisabled(on_air);
                if (ImGui::Button(tr("Force Device Lost (Test)"), ImVec2(full_w, 0.0f))) {
                    actions.request_force_device_lost = true;
                }
                ImGui::EndDisabled();
            }
        };

        if (mode == StudioMode::Nodes) {
            if (node_canvas != nullptr) {
                const uint64_t sel_node_id = (selection.kind == SelectionKind::Node && selection.item_index > 0)
                                                 ? static_cast<uint64_t>(selection.item_index)
                                                 : 0ULL;
                node_canvas->draw_selected_node_inspector(sel_node_id, fonts, undo, actions);
            } else {
                ImGui::TextColored(t.text_tertiary, "%s", tr("Select a node in the canvas to inspect properties."));
            }
            if (selection.kind == SelectionKind::OutputDest || focus_output) {
                ImGui::Spacing();
                draw_output_dsp(true, focus_output);
            }
        } else if (mode == StudioMode::Rigging) {
            draw_selected_bone_or_material();
            draw_transform_physics(true);
            if (section_header(tr("Rigging & Safety Bounds"), true, false)) {
                if (begin_property_table("##rig_bounds")) {
                    static constexpr ParamId kBoundRows[] = {
                        ParamId::TrackingNpuBoundsEnabled,  ParamId::TrackingNpuAntiClipEnabled,
                        ParamId::TrackingNpuSafeBoxPreview, ParamId::TrackingNpuBoundsX,
                        ParamId::TrackingNpuBoundsY,        ParamId::TrackingNpuBoundsZ,
                        ParamId::TrackingNpuSoftElasticity, ParamId::TrackingNpuMaxPitchDeg,
                        ParamId::TrackingNpuAntiClipMargin,
                    };
                    bool bounds_changed = false;
                    for (const ParamId pid : kBoundRows) {
                        bounds_changed |= draw_property_row(pid, params, undo, fonts, row_actions);
                    }
                    if (bounds_changed) {
                        actions.viewport.npu_bounds_enabled = params.get_bool(ParamId::TrackingNpuBoundsEnabled);
                        actions.viewport.npu_anti_clip_enabled = params.get_bool(ParamId::TrackingNpuAntiClipEnabled);
                        actions.viewport.npu_bounds_half = {params.get_float(ParamId::TrackingNpuBoundsX),
                                                            params.get_float(ParamId::TrackingNpuBoundsY),
                                                            params.get_float(ParamId::TrackingNpuBoundsZ)};
                        actions.viewport.npu_soft_elasticity = params.get_float(ParamId::TrackingNpuSoftElasticity);
                        actions.viewport.npu_max_pitch_deg = params.get_float(ParamId::TrackingNpuMaxPitchDeg);
                        actions.viewport.npu_anti_clip_margin = params.get_float(ParamId::TrackingNpuAntiClipMargin);
                        actions.viewport.npu_bounds_dirty = true;
                        settings_dirty = true;
                    }
                    end_property_table();
                }
            }
            if (selection.kind == SelectionKind::OutputDest || focus_output) {
                draw_output_dsp(true, focus_output);
            }
        } else {
            // LiveStudio contextual Inspector (Rev 3.1 §1.3: Selected entity only)
            draw_plugin_health();
            draw_selected_bone_or_material();
            if (selection.kind == SelectionKind::OutputDest || focus_output) {
                draw_output_dsp(true, focus_output);
            } else if (selection.kind == SelectionKind::Camera) {
                draw_camera_framing(true);
            } else if (selection.kind == SelectionKind::StudioLight) {
                draw_lighting_summary(true);
            } else if (selection.kind == SelectionKind::Plugin) {
                draw_lighting_summary(false);
            } else {
                draw_transform_physics(selection.kind == SelectionKind::Avatar ||
                                       selection.kind == SelectionKind::Bone);
                draw_lighting_summary(selection.kind == SelectionKind::Avatar);
                draw_camera_framing(false);
                draw_tracking_pipeline(false);
                draw_hardware_contracts();
            }
        }
        focus_output_pending_ = false;
    }
    ImGui::End();
}

} // namespace aether::ui
