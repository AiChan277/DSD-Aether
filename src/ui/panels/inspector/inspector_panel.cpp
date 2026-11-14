#include "ui/panels/inspector/inspector_panel.hpp"

#include "ui/modals/graphics_settings_modal.hpp"
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
                          StudioActions& actions) {
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

        // Lighting & NPR (§12.7)
        const bool expand_render =
            (selection.kind == SelectionKind::LightKey || selection.kind == SelectionKind::LightFill ||
             selection.kind == SelectionKind::LightRim || selection.kind == SelectionKind::Avatar);
        if (section_header(tr("Lighting & NPR"), expand_render, false)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(t.text_secondary, "%s", tr("Active GPU"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x));
            GraphicsSettingsModal::draw_gpu_selector_combo("##InspGpuCombo", on_air, rhi, render_view, settings, actions,
                                                           settings_dirty);
            if (ImGui::Button(tr("Graphics Settings..."), ImVec2(full_w, 0.0f))) {
                actions.request_open_graphics_settings = true;
            }
            ImGui::Spacing();
            if (begin_property_table("##lighting")) {
                if (draw_property_row(ParamId::RenderBackend, params, undo, fonts, row_actions, false, on_air)) {
                    actions.viewport.render_backend = static_cast<uint8_t>(params.get_int(ParamId::RenderBackend));
                }
                const bool rt_blocked = !render_view.rt_supported || render_view.render_backend == 1U;
                const char* rt_reason = !render_view.rt_supported ? tr("Ray query unsupported on active GPU")
                                        : rt_blocked              ? tr("Ray tracing needs the Vulkan backend")
                                                                  : nullptr;
                if (draw_property_row(ParamId::RenderRtEnabled, params, undo, fonts, row_actions, false, rt_blocked,
                                      rt_reason)) {
                    actions.viewport.lighting_dirty = true;
                }
                static constexpr ParamId kLightingRows[] = {
                    ParamId::RenderFlatness,        ParamId::RenderLightScale,   ParamId::RenderKeyAzimuthDeg,
                    ParamId::RenderKeyElevationDeg, ParamId::RenderKeyIntensity, ParamId::RenderFillIntensity,
                    ParamId::RenderRimIntensity,    ParamId::RenderOutlineScale, ParamId::RenderBloomIntensity,
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
                static constexpr ParamId kLive2dExtraRows[] = {
                    ParamId::RenderTargetFps,
                    ParamId::RenderAaMode,
                    ParamId::RenderFaceNormalFlatten,
                    ParamId::RenderCelBandSharpness,
                    ParamId::RenderShadowWarmCool,
                    ParamId::RenderIroTsukeStrength,
                    ParamId::RenderCreaseStrength,
                    ParamId::RenderHairHighlight,
                    ParamId::RenderRimStepStrength,
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
            ImGui::Spacing();
            draw_status_badge(fonts, tr("Active Denoiser"), render_view.denoiser_active_name,
                              render_view.denoiser_fallback_triggered ? t.status_warn : t.text_primary);
            if (render_view.render_backend == 1U) {
                char dx_buf[192]{};
                std::snprintf(dx_buf, sizeof(dx_buf), "%s · %.2f ms", render_view.backend_detail,
                              static_cast<double>(render_view.dx11_render_ms));
                draw_status_badge(fonts, tr("DirectX 11"), dx_buf, t.text_primary);
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
            ImGui::Spacing();
            char cam_buf[192]{};
            format_camera_status_badge(tracking, cam_buf, sizeof(cam_buf));
            draw_status_badge(fonts, tr("Camera"), cam_buf, tracking.camera_active ? t.status_ok : t.status_warn);
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
