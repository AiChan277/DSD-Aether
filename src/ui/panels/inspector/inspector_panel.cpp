#include "ui/panels/inspector/inspector_panel.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"
#include "ui/widgets/feedback.hpp"

#include <imgui.h>

#include <cstdio>

namespace aether::ui {

void InspectorPanel::draw(StudioMode mode, bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi,
                          bool ffi_layout_ok, const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                          const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                          const StudioHostStatus& host_status, std::span<const LoadedPluginVm> plugins,
                          const UiSelection& selection, ParamStore& params, UndoStack& undo, const UiFontSet& fonts,
                          PropertyRowActions& row_actions, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();

    if (ImGui::Begin(tr("Inspector"))) {
        // Header badge identifying current selection
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s", selection.name[0] != '\0' ? selection.name.data() : tr("Inspector"));
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Separator();

        // Contextual Plugin Health card when a plugin is selected
        if (selection.kind == SelectionKind::Plugin && selection.item_index >= 0 &&
            static_cast<size_t>(selection.item_index) < plugins.size()) {
            const LoadedPluginVm& plg = plugins[static_cast<size_t>(selection.item_index)];
            ImGui::TextColored(t.text_secondary, "ID: %s (%s)", plg.id.data(), plg.kind.data());
            ImGui::TextColored(plg.permanently_disabled ? t.status_error : (plg.faulted ? t.status_warn : t.status_ok),
                               "State: %s | Restarts: %u | Traps: %u", plg.status_text.data(), plg.restart_count,
                               plg.trap_count);
            if (plg.permanently_disabled && plg.native_slot >= 0) {
                if (ImGui::Button(tr("Retry Disabled Plugin"))) {
                    actions.request_reset_native_plugin = true;
                    actions.reset_native_slot = plg.native_slot;
                }
            }
            ImGui::Separator();
        }

        // Transform & Physics (§12.7 / Appendix D)
        const bool expand_xform = (selection.kind == SelectionKind::Avatar || selection.kind == SelectionKind::Bone ||
                                   mode == StudioMode::Rigging);
        if (ImGui::CollapsingHeader(tr("Transform & Physics"),
                                    expand_xform ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
            draw_property_row(ParamId::AvatarPosX, params, undo, fonts, row_actions, selection.multi_select);
            draw_property_row(ParamId::AvatarPosY, params, undo, fonts, row_actions, selection.multi_select);
            draw_property_row(ParamId::AvatarPosZ, params, undo, fonts, row_actions, selection.multi_select);
            draw_property_row(ParamId::AvatarRotY, params, undo, fonts, row_actions, selection.multi_select);
            draw_property_row(ParamId::AvatarScale, params, undo, fonts, row_actions, selection.multi_select);
            draw_property_row(ParamId::SimSpringStiffness, params, undo, fonts, row_actions);
            draw_property_row(ParamId::SimSpringDamping, params, undo, fonts, row_actions);
            draw_property_row(ParamId::SimGain, params, undo, fonts, row_actions);
            draw_property_row(ParamId::SimIkWeight, params, undo, fonts, row_actions);

            if (avatar.has_avatar) {
                ImGui::BeginDisabled(on_air);
                if (ImGui::Button(tr("Bake Face SDF Texture"), ImVec2(-1.0f, t.control_h_compact_px))) {
                    actions.viewport.request_bake_face_sdf = true;
                }
                ImGui::EndDisabled();
                if (render_view.baked_face_sdf_applied) {
                    ImGui::TextColored(t.status_ok, "%s %s", icons::kDomainSdf, tr("Baked UV Face SDF active"));
                }
            }
        }

        // NPR & Ray-Traced Lighting (§12.7)
        const bool expand_render =
            (selection.kind == SelectionKind::LightKey || selection.kind == SelectionKind::LightFill ||
             selection.kind == SelectionKind::LightRim || selection.kind == SelectionKind::Avatar);
        if (ImGui::CollapsingHeader(tr("Lighting & NPR"),
                                    expand_render ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
            if (draw_property_row(ParamId::RenderBackend, params, undo, fonts, row_actions, false, on_air)) {
                actions.viewport.render_backend = static_cast<uint8_t>(params.get_int(ParamId::RenderBackend));
            }
            if (render_view.render_backend == 1U) {
                ImGui::TextColored(t.text_tertiary, "%s %s | %.2f ms", tr("DirectX 11:"), render_view.backend_detail,
                                   static_cast<double>(render_view.dx11_render_ms));
            }
            const bool rt_blocked = !render_view.rt_supported || render_view.render_backend == 1U;
            const char* rt_reason = !render_view.rt_supported ? tr("Ray query unsupported on active GPU")
                                    : rt_blocked              ? tr("Ray tracing needs the Vulkan backend")
                                                              : nullptr;
            if (draw_property_row(ParamId::RenderRtEnabled, params, undo, fonts, row_actions, false, rt_blocked,
                                  rt_reason)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderFlatness, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderLightScale, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderKeyAzimuthDeg, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderKeyElevationDeg, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderKeyIntensity, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderFillIntensity, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderRimIntensity, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderOutlineScale, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            if (draw_property_row(ParamId::RenderBloomIntensity, params, undo, fonts, row_actions)) {
                actions.viewport.lighting_dirty = true;
            }
            const char* den_err =
                render_view.denoiser_fallback_triggered ? render_view.denoiser_fallback_reason : nullptr;
            if (draw_property_row(ParamId::RenderDenoiser, params, undo, fonts, row_actions, false, false, den_err)) {
                actions.viewport.request_denoiser_apply = true;
            }
            draw_property_row(ParamId::RenderDebugView, params, undo, fonts, row_actions);
            ImGui::TextColored(t.text_tertiary, "Active Denoiser: %s", render_view.denoiser_active_name);
        }

        // Tracking & Execution Provider
        if (ImGui::CollapsingHeader(tr("Tracking & Pipeline"))) {
            if (draw_property_row(ParamId::TrackingEpMode, params, undo, fonts, row_actions)) {
                const int mode_idx = params.get_int(ParamId::TrackingEpMode);
                actions.viewport.request_ep_override = (mode_idx == 0) ? -1 : (mode_idx - 1);
            }
            draw_property_row(ParamId::TrackingSmoothingCutoffHz, params, undo, fonts, row_actions);
            if (draw_property_row(ParamId::TrackingSyntheticCamera, params, undo, fonts, row_actions)) {
                actions.viewport.request_toggle_camera_source = true;
            }
            draw_property_row(ParamId::TrackingAntiFlicker, params, undo, fonts, row_actions);
            draw_property_row(ParamId::TrackingExposureLock, params, undo, fonts, row_actions);
            ImGui::TextColored(t.text_secondary, "EP: %s | Frames: %llu", tracking.active_ep_label,
                               static_cast<unsigned long long>(tracking.frames_processed));
        }

        // Output & Audio-DSP
        const bool expand_output = (selection.kind == SelectionKind::OutputDest);
        if (ImGui::CollapsingHeader(tr("Output & Audio-DSP"),
                                    expand_output ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
            bool out_changed = false;
            out_changed |= draw_property_row(ParamId::OutputResolution, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputFps, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputPremulAlpha, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputNonAlphaBg, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputSpoutEnabled, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputSpoutCpuFallback, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputVcamEnabled, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputNdiEnabled, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputNdiFormat, params, undo, fonts, row_actions);
            out_changed |= draw_property_row(ParamId::OutputOscEnabled, params, undo, fonts, row_actions);
            if (draw_property_row(ParamId::OutputWsEnabled, params, undo, fonts, row_actions)) {
                actions.request_toggle_ws = true;
                actions.target_ws_enabled = params.get_bool(ParamId::OutputWsEnabled);
            }
            out_changed |= draw_property_row(ParamId::DspUserOffsetMs, params, undo, fonts, row_actions);
            if (out_changed) {
                actions.viewport.output_config_dirty = true;
            }
            ImGui::TextColored(output_view.dsp_connected ? t.status_ok : t.text_tertiary,
                               "DSP: %s | Rec Delay %.1f ms | Peak %.1f dB",
                               output_view.dsp_connected ? "Linked" : "Offline",
                               static_cast<double>(output_view.dsp_recommended_delay_ms),
                               static_cast<double>(output_view.dsp_input_rms_db));
        }

        // Hardware & Contracts summary (compact collapsible)
        if (ImGui::CollapsingHeader(tr("Hardware & Contracts"))) {
            if (fonts.mono != nullptr) {
                ImGui::PushFont(fonts.mono);
            }
            ImGui::TextColored(t.text_secondary, "GC: %llu pending | %llu reclaimed | %.1f MB",
                               static_cast<unsigned long long>(render_view.gc_retired_pending),
                               static_cast<unsigned long long>(render_view.gc_reclaimed_total),
                               static_cast<double>(render_view.gc_bytes_reclaimed) / (1024.0 * 1024.0));
            ImGui::TextColored(t.text_secondary, "CPU: %s (%uP/%uE)", hw.cpu.brand_string.c_str(), hw.cpu.p_core_count,
                               hw.cpu.e_core_count);
            ImGui::TextColored(t.text_secondary, "GPU: %s", rhi.gpu_info().device_name.c_str());
            ImGui::TextColored(hw.npu.available ? t.status_ok : t.text_tertiary, "NPU: %s",
                               hw.npu.available
                                   ? (hw.npu.device_name.empty() ? "Available" : hw.npu.device_name.c_str())
                                   : "Not detected");
            ImGui::TextColored(ffi_layout_ok ? t.status_ok : t.status_error, "FFI ABI: %s | Host: %s",
                               ffi_layout_ok ? "VERIFIED" : "MISMATCH", host_status.host_alive ? "Online" : "Offline");
            if (fonts.mono != nullptr) {
                ImGui::PopFont();
            }
            if (ImGui::Button(tr("Force Device Lost (Test)"), ImVec2(-1.0f, t.control_h_compact_px))) {
                actions.request_force_device_lost = true;
            }
        }
    }
    ImGui::End();
}

} // namespace aether::ui
