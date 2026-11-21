#include "ui/panels/inspector/inspector_panel.hpp"

#include "renderer/lighting_rig.hpp"
#include "ui/modals/graphics_settings_modal.hpp"
#include "ui/panels/viewport/camera_presets.hpp"
#include "ui/panels/viewport/viewport_panel.hpp"
#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

#include <cstdio>

namespace aether::ui {
namespace {

const char* selection_kind_label(SelectionKind k) noexcept {
    switch (k) {
    case SelectionKind::Avatar:
        return "Avatar";
    case SelectionKind::LightKey:
    case SelectionKind::LightFill:
    case SelectionKind::LightRim:
        return "Light";
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
                          StudioActions& actions, ViewportPanel* viewport_for_presets) {
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

        // Contextual Plugin Health card when a plugin is selected
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
                              plg.permanently_disabled ? t.status_error : (plg.faulted ? t.status_warn : t.status_ok));
            if (plg.permanently_disabled && plg.native_slot >= 0) {
                if (ImGui::Button(tr("Retry Disabled Plugin"), ImVec2(full_w, 0.0f))) {
                    actions.request_reset_native_plugin = true;
                    actions.reset_native_slot = plg.native_slot;
                }
            }
            ImGui::Spacing();
        }

        // Transform & Physics (§12.7 / Appendix D)
        const bool expand_xform = (selection.kind == SelectionKind::Avatar || selection.kind == SelectionKind::Bone ||
                                   mode == StudioMode::Rigging);
        if (section_header(tr("Transform & Physics"), expand_xform, false)) {
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

        // Camera & Framing Presets (Tasks 1 & 3)
        const bool expand_camera = (selection.kind == SelectionKind::Camera);
        if (section_header(tr("Camera & Framing"), expand_camera, false)) {
            if (begin_property_table("##camera_framing")) {
                if (draw_property_row(ParamId::AppViewportAspect, params, undo, fonts, row_actions) ||
                    draw_property_row(ParamId::AppCameraTransitionSpeed, params, undo, fonts, row_actions)) {
                    settings_dirty = true;
                }
                end_property_table();
            }
            if (viewport_for_presets != nullptr) {
                ImGui::Spacing();
                const float avail_w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
                const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
                const float half_w = std::floor((avail_w - spacing_x) * 0.5f);
                const int32_t active_bi = viewport_for_presets->active_builtin_preset();
                for (int32_t i = 0; i < static_cast<int32_t>(kBuiltinFramingPresetCount); ++i) {
                    const bool is_active = (active_bi == i);
                    if (is_active) {
                        ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
                        ImGui::PushStyleColor(ImGuiCol_Text, t.text_on_accent);
                    }
                    char btn_lbl[48]{};
                    std::snprintf(btn_lbl, sizeof(btn_lbl), "%s##cam_bi_%d", builtin_framing_preset_shortcut(i), i);
                    if (ImGui::Button(btn_lbl, ImVec2(half_w, 0.0f))) {
                        viewport_for_presets->apply_builtin_preset(i, avatar, params, actions);
                    }
                    if (is_active) {
                        ImGui::PopStyleColor(2);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                        ImGui::SetTooltip("%s (%s)", builtin_framing_preset_name(i),
                                          builtin_framing_preset_shortcut(i));
                    }
                    if ((i % 2) == 0 && i + 1 < static_cast<int32_t>(kBuiltinFramingPresetCount)) {
                        ImGui::SameLine();
                    }
                }
                ImGui::Spacing();
                if (ImGui::Button(tr("Reset Camera"), ImVec2(half_w, 0.0f))) {
                    viewport_for_presets->reset_camera(actions);
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("+ Save Preset"), ImVec2(half_w, 0.0f))) {
                    viewport_for_presets->open_save_preset_modal();
                }

                const auto& customs = viewport_for_presets->preset_bank().custom_presets();
                if (!customs.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(t.text_secondary, "%s", tr("Custom Presets"));
                    const float btn_save_w = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    const float btn_del_w = ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    const float name_w = std::max(60.0f, avail_w - btn_save_w - btn_del_w - spacing_x * 2.0f);
                    for (size_t i = 0; i < customs.size(); ++i) {
                        const auto& cp = customs[i];
                        const bool is_cur = (viewport_for_presets->active_preset_name() == cp.name && active_bi < 0);
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
                            viewport_for_presets->preset_bank().remove_at(i, viewport_for_presets->persist_presets());
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

        // Lighting & NPR (§12.7)
        const bool expand_render =
            (selection.kind == SelectionKind::LightKey || selection.kind == SelectionKind::LightFill ||
             selection.kind == SelectionKind::LightRim || selection.kind == SelectionKind::Avatar);
        if (section_header(tr("Lighting & NPR"), expand_render, false)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(t.text_secondary, "%s", tr("Active GPU"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x));
            GraphicsSettingsModal::draw_gpu_selector_combo("##InspGpuCombo", on_air, rhi, render_view, settings,
                                                           actions, settings_dirty, &params);
            if (ImGui::Button(tr("Graphics Settings..."), ImVec2(full_w, 0.0f))) {
                actions.request_open_graphics_settings = true;
            }
            ImGui::Spacing();
            if (begin_property_table("##lighting")) {
                if (draw_property_row(ParamId::RenderBackend, params, undo, fonts, row_actions, false, on_air)) {
                    actions.viewport.render_backend = static_cast<uint8_t>(params.get_int(ParamId::RenderBackend));
                }
                const bool dx11_blocked = (render_view.render_backend == 1U);
                const bool rt_blocked = !render_view.rt_supported || dx11_blocked;
                const char* rt_reason = !render_view.rt_supported
                                            ? tr("Ray query / DXR unsupported on active GPU (using Traditional)")
                                        : dx11_blocked ? tr("Ray tracing unavailable on DirectX 11")
                                                       : nullptr;
                if (draw_property_row(ParamId::RenderLightingMode, params, undo, fonts, row_actions)) {
                    if (params.get_int(ParamId::RenderLightingMode) > 0 && rt_blocked) {
                        (void)params.set(ParamId::RenderLightingMode, 0.0);
                    }
                    (void)params.set(ParamId::RenderRtEnabled,
                                     params.get_int(ParamId::RenderLightingMode) > 0 ? 1.0 : 0.0);
                    actions.viewport.lighting_dirty = true;
                    settings_dirty = true;
                }
                if (draw_property_row(ParamId::RenderRtEnabled, params, undo, fonts, row_actions, false, rt_blocked,
                                      rt_reason)) {
                    const bool on = params.get_bool(ParamId::RenderRtEnabled);
                    if (!on) {
                        (void)params.set(ParamId::RenderLightingMode, 0.0);
                    } else if (params.get_int(ParamId::RenderLightingMode) == 0) {
                        (void)params.set(ParamId::RenderLightingMode, 1.0);
                    }
                    actions.viewport.lighting_dirty = true;
                    settings_dirty = true;
                }
                if (draw_property_row(ParamId::RenderCircadianEnabled, params, undo, fonts, row_actions) ||
                    draw_property_row(ParamId::RenderCircadianTimezone, params, undo, fonts, row_actions) ||
                    draw_property_row(ParamId::RenderCircadianTimeMinutes, params, undo, fonts, row_actions)) {
                    actions.viewport.lighting_dirty = true;
                    settings_dirty = true;
                }
                static constexpr ParamId kLightingRows[] = {
                    ParamId::RenderPhotometricBeamAngleDeg,
                    ParamId::RenderEyeCatchlightIntensity,
                    ParamId::RenderEyeCatchlightSize,
                    ParamId::RenderCreaseDisguiseEnabled,
                    ParamId::RenderCreaseDisguiseStrength,
                    ParamId::RenderFlatness,
                    ParamId::RenderLightScale,
                    ParamId::RenderKeyAzimuthDeg,
                    ParamId::RenderKeyElevationDeg,
                    ParamId::RenderKeyIntensity,
                    ParamId::RenderFillIntensity,
                    ParamId::RenderRimIntensity,
                    ParamId::RenderOutlineScale,
                    ParamId::RenderBloomIntensity,
                };
                for (const ParamId pid : kLightingRows) {
                    if (draw_property_row(pid, params, undo, fonts, row_actions)) {
                        actions.viewport.lighting_dirty = true;
                    }
                }
                const char* den_err =
                    render_view.denoiser_fallback_triggered ? render_view.denoiser_fallback_reason : nullptr;
                if (draw_property_row(ParamId::RenderDenoiser, params, undo, fonts, row_actions, false, false,
                                      den_err)) {
                    actions.viewport.denoiser_kind = static_cast<uint8_t>(params.get_int(ParamId::RenderDenoiser));
                    actions.viewport.request_denoiser_apply = true;
                }
                if (draw_property_row(ParamId::RenderDebugView, params, undo, fonts, row_actions)) {
                    actions.viewport.debug_view = static_cast<uint8_t>(params.get_int(ParamId::RenderDebugView));
                }
                static constexpr ParamId kCoreRenderRows[] = {
                    ParamId::RenderTargetFps,
                    ParamId::RenderAaMode,
                };
                for (const ParamId pid : kCoreRenderRows) {
                    if (draw_property_row(pid, params, undo, fonts, row_actions)) {
                        actions.viewport.lighting_dirty = true;
                    }
                }
                end_property_table();
            }
            if (ImGui::TreeNodeEx(tr("Lighting & Shading Detail"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                if (begin_property_table("##shading_detail")) {
                    static constexpr ParamId kLive2dExtraRows[] = {
                        ParamId::RenderFaceNormalFlatten,
                        ParamId::RenderNeckSeamBlend,
                        ParamId::RenderCelBandSharpness,
                        ParamId::RenderShadowWarmCool,
                        ParamId::RenderIndirectBleedStrength,
                        ParamId::RenderRimDepthOffsetPx,
                        ParamId::RenderIroTsukeStrength,
                        ParamId::RenderCreaseStrength,
                        ParamId::RenderHairHighlight,
                        ParamId::RenderRimStepStrength,
                        ParamId::Render2DFilterPro,
                        ParamId::RenderFilterKuwaharaStrength,
                        ParamId::RenderFilterDogInkBoost,
                        ParamId::RenderFilterPaperWarmth,
                        ParamId::RenderFilterPaperWarmthStrength,
                        ParamId::RenderStudioFixture,
                        ParamId::RenderSoftboxEnabled,
                        ParamId::RenderGridEnabled,
                        ParamId::RenderGridAngleDeg,
                        ParamId::RenderBarnDoorsEnabled,
                        ParamId::RenderDiffusionEnabled,
                        ParamId::RenderDiffusionDensity,
                        ParamId::RenderGelEnabled,
                        ParamId::RenderGelPreset,
                        ParamId::RenderSatsueiLightWrap,
                        ParamId::RenderSatsueiHalation,
                        ParamId::RenderSatsueiBackscatter,
                        ParamId::RenderHoloAvatarContrast,
                        ParamId::RenderHoloBgExposure,
                    };
                    bool style_changed = false;
                    for (const ParamId pid : kLive2dExtraRows) {
                        style_changed |= draw_property_row(pid, params, undo, fonts, row_actions);
                    }
                    if (style_changed) {
                        actions.viewport.lighting_dirty = true;
                    }
                    end_property_table();
                }
                ImGui::TreePop();
            }
            ImGui::Spacing();
            if (params.get_bool(ParamId::RenderCircadianEnabled)) {
                const int ov_min = params.get_int(ParamId::RenderCircadianTimeMinutes);
                const float hr = (ov_min >= 0) ? (static_cast<float>(ov_min) / 60.0f) : render_view.circadian_hour_24;
                const uint8_t ph = (ov_min >= 0) ? ((hr >= 5.0f && hr < 7.0f)     ? 1u
                                                    : (hr >= 7.0f && hr < 15.5f)  ? 2u
                                                    : (hr >= 15.5f && hr < 18.0f) ? 3u
                                                                                  : 0u)
                                                 : render_view.circadian_phase;
                static constexpr const char* kPhaseNames[4] = {
                    "Night Studio",
                    "Dawn",
                    "Daytime",
                    "Golden Hour",
                };
                const int hh = static_cast<int>(hr) % 24;
                const int mm = std::clamp(static_cast<int>((hr - static_cast<float>(hh)) * 60.0f), 0, 59);
                char cbuf[160]{};
                std::snprintf(cbuf, sizeof(cbuf), "%02d:%02d · %s (Key %.0fK)", hh, mm,
                              kPhaseNames[std::min<uint8_t>(ph, 3u)],
                              static_cast<double>(render_view.circadian_key_kelvin));
                draw_status_badge(fonts, tr("Solar Time"), cbuf, t.status_ok);
            }
            draw_status_badge(fonts, tr("Active Denoiser"), render_view.denoiser_active_name,
                              render_view.denoiser_fallback_triggered ? t.status_warn : t.text_primary);
            if (render_view.render_backend == 1U || render_view.render_backend == 2U) {
                char dx_buf[192]{};
                const float ms =
                    (render_view.render_backend == 2U) ? render_view.dx12_render_ms : render_view.dx11_render_ms;
                std::snprintf(dx_buf, sizeof(dx_buf), "%s · %.2f ms", render_view.backend_detail,
                              static_cast<double>(ms));
                draw_status_badge(fonts, tr(render_view.render_backend == 2U ? "DirectX 12" : "DirectX 11"), dx_buf,
                                  t.text_primary);
            }
            ImGui::Spacing();
        }

        // Tracking & Execution Provider
        if (section_header(tr("Tracking & Pipeline"), false, false)) {
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
                              static_cast<double>(tracking.audio_rms_db), static_cast<double>(tracking.audio_open_y));
            }
            draw_status_badge(fonts, tr("Audio Lip-Sync"), mic_buf,
                              tracking.audio_vad ? t.status_ok
                                                 : (tracking.audio_mic_available ? t.text_primary : t.status_warn));
            char ep_buf[128]{};
            std::snprintf(ep_buf, sizeof(ep_buf), "%s · %llu frames", tracking.active_ep_label,
                          static_cast<unsigned long long>(tracking.frames_processed));
            draw_status_badge(fonts, tr("Execution Provider"), ep_buf, tracking.ready ? t.status_ok : t.text_tertiary);
            ImGui::Spacing();
        }

        // Output & Audio-DSP (target of Outliner "Inspect Output Settings...")
        const bool expand_output = (selection.kind == SelectionKind::OutputDest);
        if (section_header(tr("Output & Audio-DSP"), expand_output, focus_output)) {
            if (focus_output) {
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
        focus_output_pending_ = false;

        // Hardware & Contracts summary
        if (section_header(tr("Hardware & Contracts"), false, false)) {
            char buf[192]{};
            std::snprintf(buf, sizeof(buf), "%s (%uP / %uE)", hw.cpu.brand_string.c_str(), hw.cpu.p_core_count,
                          hw.cpu.e_core_count);
            draw_status_badge(fonts, "CPU", buf, t.text_primary);
            draw_status_badge(fonts, "GPU", rhi.gpu_info().device_name.c_str(), t.text_primary);
            draw_status_badge(fonts, "NPU",
                              hw.npu.available ? (hw.npu.device_name.empty() ? "Available" : hw.npu.device_name.c_str())
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
    }
    ImGui::End();
}

} // namespace aether::ui
