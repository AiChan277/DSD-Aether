#include "ui/modals/graphics_settings_modal.hpp"

#include "renderer/lighting_rig.hpp"
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

struct PresetParamValue {
    ParamId pid;
    double value;
};

struct QualityPresetDef {
    const char* name;
    const char* badge;
    const char* summary;
    std::array<PresetParamValue, 12> entries;
    size_t count;
};

constexpr std::array<QualityPresetDef, 4> kQualityPresets = {{
    QualityPresetDef{
        "Ultra 2D Live2D + RT",
        "High Fidelity",
        "Ray-traced shadows & AO, TAA-Lite smoothing, crisp cel bands & Iro-Tsuke ink at 60 FPS.",
        {{
            {ParamId::RenderRtEnabled, 1.0},
            {ParamId::RenderTargetFps, 1.0}, // 60 FPS
            {ParamId::RenderBackgroundEco, 1.0},
            {ParamId::RenderResolutionScalePct, 100.0},
            {ParamId::RenderAaMode, 2.0}, // TAA-Lite
            {ParamId::RenderFlatness, 0.80},
            {ParamId::RenderFaceNormalFlatten, 0.85},
            {ParamId::RenderCelBandSharpness, 0.025},
            {ParamId::RenderShadowWarmCool, 0.80},
            {ParamId::RenderIroTsukeStrength, 0.90},
            {ParamId::RenderCreaseStrength, 0.80},
            {ParamId::RenderHairHighlight, 0.90},
        }},
        12,
    },
    QualityPresetDef{
        "High 2D Live2D (Fast)",
        "Low GPU Load",
        "Authentic hand-drawn 2D cel illustration with FXAA and zero RT overhead.",
        {{
            {ParamId::RenderRtEnabled, 0.0},
            {ParamId::RenderTargetFps, 1.0},
            {ParamId::RenderBackgroundEco, 1.0},
            {ParamId::RenderResolutionScalePct, 100.0},
            {ParamId::RenderAaMode, 1.0}, // FXAA
            {ParamId::RenderFlatness, 0.78},
            {ParamId::RenderFaceNormalFlatten, 0.85},
            {ParamId::RenderCelBandSharpness, 0.025},
            {ParamId::RenderShadowWarmCool, 0.75},
            {ParamId::RenderIroTsukeStrength, 0.85},
            {ParamId::RenderCreaseStrength, 0.75},
            {ParamId::RenderHairHighlight, 0.85},
        }},
        12,
    },
    QualityPresetDef{
        "Balanced Streaming",
        "60 FPS Cap",
        "Tuned for OBS / Spout2 / VCam live streaming with cool GPU thermals.",
        {{
            {ParamId::RenderRtEnabled, 1.0},
            {ParamId::RenderTargetFps, 1.0},
            {ParamId::RenderBackgroundEco, 1.0},
            {ParamId::RenderResolutionScalePct, 100.0},
            {ParamId::RenderAaMode, 1.0}, // FXAA
            {ParamId::RenderFlatness, 0.75},
            {ParamId::RenderFaceNormalFlatten, 0.85},
            {ParamId::RenderCelBandSharpness, 0.030},
            {ParamId::RenderShadowWarmCool, 0.75},
            {ParamId::RenderIroTsukeStrength, 0.85},
            {ParamId::RenderCreaseStrength, 0.70},
            {ParamId::RenderHairHighlight, 0.80},
        }},
        12,
    },
    QualityPresetDef{
        "Eco / Low Power",
        "Battery Saver",
        "30 FPS cap + 85% scale + raster shadows for near-silent fan operation.",
        {{
            {ParamId::RenderRtEnabled, 0.0},
            {ParamId::RenderTargetFps, 0.0}, // 30 FPS
            {ParamId::RenderBackgroundEco, 1.0},
            {ParamId::RenderResolutionScalePct, 85.0},
            {ParamId::RenderAaMode, 1.0},
            {ParamId::RenderFlatness, 0.75},
            {ParamId::RenderFaceNormalFlatten, 0.85},
            {ParamId::RenderCelBandSharpness, 0.035},
            {ParamId::RenderShadowWarmCool, 0.70},
            {ParamId::RenderIroTsukeStrength, 0.80},
            {ParamId::RenderCreaseStrength, 0.55},
            {ParamId::RenderHairHighlight, 0.75},
        }},
        12,
    },
}};

void format_adapter_label(const rhi::GpuAdapterDescriptor& ad, bool is_current, char* out, size_t cap) noexcept {
    const char* kind = (ad.device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)     ? "dGPU"
                       : (ad.device_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? "iGPU"
                                                                                    : "GPU";
    const char* rt_tag = (ad.supports_ray_query && ad.supports_accel_struct) ? " + RT" : "";
    const char* cur_tag = is_current ? " [Active]" : "";
    if (ad.dedicated_vram_mb > 0ULL) {
        std::snprintf(out, cap, "%s (%s · %llu MB%s)%s", ad.device_name.c_str(), kind,
                      static_cast<unsigned long long>(ad.dedicated_vram_mb), rt_tag, cur_tag);
    } else {
        std::snprintf(out, cap, "%s (%s · Shared%s)%s", ad.device_name.c_str(), kind, rt_tag, cur_tag);
    }
}

bool adapter_matches_current(const rhi::GpuAdapterDescriptor& ad, const rhi::SelectedGpuInfo& cur) noexcept {
    if (ad.luid_valid && cur.luid_valid) {
        return ad.luid == cur.luid;
    }
    return ad.vendor_id == cur.vendor_id && ad.device_id == cur.device_id;
}

} // namespace

void GraphicsSettingsModal::format_luid_hex(const std::array<uint8_t, 8>& luid, char* out, size_t cap) noexcept {
    if (out == nullptr || cap == 0) {
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < luid.size() && (pos + 2) < cap; ++i) {
        pos += static_cast<size_t>(std::snprintf(out + pos, cap - pos, "%02x", static_cast<unsigned>(luid[i])));
    }
    out[std::min(pos, cap - 1)] = '\0';
}

void GraphicsSettingsModal::draw_gpu_selector_combo(const char* widget_id, bool on_air, const rhi::RhiDevice& rhi,
                                                    const DevShellRenderView& render_view, AppSettings& settings,
                                                    StudioActions& actions, bool& out_settings_dirty,
                                                    ParamStore* opt_params) {
    const auto& cur = rhi.gpu_info();
    char preview[192]{};
    std::snprintf(preview, sizeof(preview), "%s (%s)", cur.device_name.empty() ? tr("Unknown GPU") : cur.device_name.c_str(),
                  cur.is_discrete ? "dGPU" : "iGPU");

    ImGui::BeginDisabled(on_air);
    if (ImGui::BeginCombo(widget_id, preview, ImGuiComboFlags_HeightRegular)) {
        if (render_view.gpu_adapters == nullptr || render_view.gpu_adapters->empty()) {
            ImGui::TextDisabled("%s", tr("Only active Vulkan device available"));
        } else {
            for (int i = 0; i < static_cast<int>(render_view.gpu_adapters->size()); ++i) {
                const auto& ad = (*render_view.gpu_adapters)[static_cast<size_t>(i)];
                const bool is_cur = adapter_matches_current(ad, cur);
                char item_lbl[224]{};
                format_adapter_label(ad, is_cur, item_lbl, sizeof(item_lbl));
                char selectable_id[256]{};
                std::snprintf(selectable_id, sizeof(selectable_id), "%s##gpu_%d", item_lbl, i);
                if (ImGui::Selectable(selectable_id, is_cur) && !is_cur) {
                    actions.request_switch_gpu = true;
                    actions.target_gpu_vendor_id = ad.vendor_id;
                    actions.target_gpu_device_id = ad.device_id;
                    actions.target_gpu_luid = ad.luid;
                    actions.target_gpu_luid_valid = ad.luid_valid;
                    std::snprintf(actions.target_gpu_name.data(), actions.target_gpu_name.size(), "%s",
                                  ad.device_name.c_str());

                    if (ad.luid_valid) {
                        char luid_hex[32]{};
                        format_luid_hex(ad.luid, luid_hex, sizeof(luid_hex));
                        settings.preferred_gpu_luid = luid_hex;
                    } else {
                        settings.preferred_gpu_luid.clear();
                    }
                    settings.preferred_gpu_name = ad.device_name;
                    if (!ad.supports_ray_query || ad.recommended_lighting_mode == 0u || ad.is_intel_igpu ||
                        ad.is_uma || ad.device_type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                        if (opt_params != nullptr) {
                            (void)opt_params->set(ParamId::RenderLightingMode, 0.0);
                            (void)opt_params->set(ParamId::RenderRtEnabled, 0.0);
                        }
                        actions.viewport.lighting_mode = 0u;
                        actions.viewport.rt_enabled = false;
                        actions.viewport.lighting_dirty = true;
                    }
                    out_settings_dirty = true;
                }
                if (is_cur) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (on_air && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("Switching the rendering GPU is blocked while ON AIR"));
    }
}

void GraphicsSettingsModal::draw(bool on_air, const rhi::RhiDevice& rhi, const DevShellRenderView& render_view,
                                 ParamStore& params, UndoStack& undo, AppSettings& settings, const UiFontSet& fonts,
                                 float dpi_scale, PropertyRowActions& row_actions, StudioActions& actions,
                                 bool& out_settings_dirty) {
    if (!open_) {
        return;
    }

    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList(vp);
    bg_dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                         ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float modal_w = std::min(780.0f * scale, vp->Size.x - 40.0f * scale);
    const float modal_h = std::min(620.0f * scale, vp->Size.y - 40.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(modal_w, modal_h), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 12.0f * scale));

    bool keep_open = open_;
    if (ImGui::Begin(tr("Graphics Settings"), &keep_open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        const ImVec2 wmin = ImGui::GetWindowPos();
        draw_elevation_shadow(bg_dl, wmin, ImVec2(wmin.x + modal_w, wmin.y + modal_h), t.radius_modal_px);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            keep_open = false;
        }

        // Top live GPU telemetry strip
        if (fonts.mono != nullptr) {
            ImGui::PushFont(fonts.mono);
        }
        const auto& cur_gpu = rhi.gpu_info();
        char summary[256]{};
        std::snprintf(summary, sizeof(summary), "%s  ·  %.1f FPS  ·  GPU %.2f ms  ·  VRAM %llu / %llu MB",
                      cur_gpu.device_name.empty() ? tr("Unknown GPU") : cur_gpu.device_name.c_str(),
                      static_cast<double>(render_view.present_fps), static_cast<double>(render_view.total_gpu_ms),
                      static_cast<unsigned long long>(render_view.vram_usage_mb),
                      static_cast<unsigned long long>(render_view.vram_budget_mb));
        ImGui::TextColored(t.accent, "%s", summary);
        if (fonts.mono != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Separator();

        // Quality Presets row
        if (fonts.body_strong != nullptr) {
            ImGui::PushFont(fonts.body_strong);
        }
        ImGui::TextColored(t.text_primary, "%s", tr("Quality Presets"));
        if (fonts.body_strong != nullptr) {
            ImGui::PopFont();
        }

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float btn_w = std::max(120.0f * scale, (avail_w - ImGui::GetStyle().ItemSpacing.x * 3.0f) * 0.25f);
        for (size_t pi = 0; pi < kQualityPresets.size(); ++pi) {
            if (pi > 0) {
                ImGui::SameLine();
            }
            const auto& preset = kQualityPresets[pi];
            const bool selected = (last_preset_applied_ == static_cast<int>(pi));
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
            }
            if (ImGui::Button(preset.name, ImVec2(btn_w, 0.0f))) {
                for (size_t ei = 0; ei < preset.count; ++ei) {
                    const auto& e = preset.entries[ei];
                    const double prev = params.get(e.pid);
                    if (params.set(e.pid, e.value)) {
                        undo.push_param(e.pid, prev, e.value);
                    }
                }
                actions.viewport.lighting_dirty = true;
                actions.viewport.request_denoiser_apply = true;
                out_settings_dirty = true;
                last_preset_applied_ = static_cast<int>(pi);
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s: %s", preset.badge, preset.summary);
            }
        }

        ImGui::Spacing();

        const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f * scale;
        if (ImGui::BeginChild("##GraphicsBody", ImVec2(0.0f, -footer_h), false)) {
            if (ImGui::BeginTabBar("##GraphicsTabs")) {
                if (ImGui::BeginTabItem(tr("Display & GPU"), nullptr,
                                        active_tab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    active_tab_ = 0;
                    ImGui::Spacing();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(t.text_secondary, "%s", tr("Rendering GPU"));
                    ImGui::SameLine(180.0f * scale);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    draw_gpu_selector_combo("##GraphicsGpuSelector", on_air, rhi, render_view, settings, actions,
                                            out_settings_dirty, &params);
                    if (cur_gpu.is_discrete) {
                        ImGui::TextColored(t.text_tertiary, "%s",
                                           tr("Dedicated discrete GPU active (supports hardware ray tracing & Tensor/Shader cores)."));
                    } else {
                        ImGui::TextColored(t.status_warn, "%s",
                                           tr("Integrated GPU active — great for battery/iGPU testing; switch to dGPU for full RT speed."));
                    }
                    ImGui::Spacing();

                    if (begin_property_table("##gfx_display")) {
                        out_settings_dirty |=
                            draw_property_row(ParamId::RenderTargetFps, params, undo, fonts, row_actions);
                        out_settings_dirty |= draw_property_row(ParamId::RenderVsync, params, undo, fonts, row_actions);
                        out_settings_dirty |=
                            draw_property_row(ParamId::RenderBackgroundEco, params, undo, fonts, row_actions);
                        out_settings_dirty |=
                            draw_property_row(ParamId::RenderResolutionScalePct, params, undo, fonts, row_actions);
                        out_settings_dirty |=
                            draw_property_row(ParamId::RenderAaMode, params, undo, fonts, row_actions);
                        if (draw_property_row(ParamId::RenderBackend, params, undo, fonts, row_actions, false,
                                              on_air)) {
                            actions.viewport.render_backend = static_cast<uint8_t>(params.get_int(ParamId::RenderBackend));
                            out_settings_dirty = true;
                        }
                        end_property_table();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(tr("2D Illustration Style"), nullptr,
                                        active_tab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    active_tab_ = 1;
                    ImGui::Spacing();
                    if (begin_property_table("##gfx_live2d")) {
                        static constexpr ParamId kStyleRows[] = {
                            ParamId::RenderEyeCatchlightIntensity,
                            ParamId::RenderEyeCatchlightSize,
                            ParamId::RenderFlatness,
                            ParamId::RenderFaceNormalFlatten,
                            ParamId::RenderNeckSeamBlend,
                            ParamId::RenderCelBandSharpness,
                            ParamId::RenderShadowWarmCool,
                            ParamId::RenderIndirectBleedStrength,
                            ParamId::RenderRimDepthOffsetPx,
                            ParamId::RenderOutlineScale,
                            ParamId::RenderIroTsukeStrength,
                            ParamId::RenderCreaseStrength,
                            ParamId::RenderHairHighlight,
                            ParamId::RenderRimStepStrength,
                            ParamId::RenderBloomIntensity,
                            ParamId::RenderLightScale,
                            ParamId::Render2DFilterPro,
                            ParamId::RenderFilterKuwaharaStrength,
                            ParamId::RenderFilterDogInkBoost,
                            ParamId::RenderFilterPaperWarmth,
                            ParamId::RenderFilterPaperWarmthStrength,
                        };
                        bool any = false;
                        for (const ParamId pid : kStyleRows) {
                            any |= draw_property_row(pid, params, undo, fonts, row_actions);
                        }
                        if (any) {
                            actions.viewport.lighting_dirty = true;
                        }
                        end_property_table();
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(tr("Lighting, Circadian & RT"), nullptr,
                                        active_tab_ == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    active_tab_ = 2;
                    ImGui::Spacing();
                    const bool dx11_blocked = (render_view.render_backend == 1U);
                    const bool rt_blocked = !render_view.rt_supported || dx11_blocked;
                    const char* rt_reason = !render_view.rt_supported
                                                ? tr("Ray query / DXR unsupported on active GPU (using Traditional)")
                                                : (dx11_blocked ? tr("Ray tracing unavailable on DirectX 11") : nullptr);
                    if (begin_property_table("##gfx_rt")) {
                        if (draw_property_row(ParamId::RenderLightingMode, params, undo, fonts, row_actions, false,
                                              false)) {
                            const int lm = params.get_int(ParamId::RenderLightingMode);
                            if (lm > 0 && rt_blocked) {
                                (void)params.set(ParamId::RenderLightingMode, 0.0);
                            }
                            (void)params.set(ParamId::RenderRtEnabled,
                                             params.get_int(ParamId::RenderLightingMode) > 0 ? 1.0 : 0.0);
                            actions.viewport.lighting_dirty = true;
                            out_settings_dirty = true;
                        }
                        if (draw_property_row(ParamId::RenderRtEnabled, params, undo, fonts, row_actions, false,
                                              rt_blocked, rt_reason)) {
                            const bool on = params.get_bool(ParamId::RenderRtEnabled);
                            if (!on) {
                                (void)params.set(ParamId::RenderLightingMode, 0.0);
                            } else if (params.get_int(ParamId::RenderLightingMode) == 0) {
                                (void)params.set(ParamId::RenderLightingMode, 1.0);
                            }
                            actions.viewport.lighting_dirty = true;
                            out_settings_dirty = true;
                        }
                        if (draw_property_row(ParamId::RenderCircadianEnabled, params, undo, fonts, row_actions)) {
                            actions.viewport.lighting_dirty = true;
                            out_settings_dirty = true;
                        }
                        if (draw_property_row(ParamId::RenderCircadianTimezone, params, undo, fonts, row_actions)) {
                            actions.viewport.lighting_dirty = true;
                            out_settings_dirty = true;
                        }
                        if (draw_property_row(ParamId::RenderCircadianTimeMinutes, params, undo, fonts, row_actions)) {
                            actions.viewport.lighting_dirty = true;
                            out_settings_dirty = true;
                        }
                        if (draw_property_row(ParamId::RenderDenoiser, params, undo, fonts, row_actions, false,
                                              on_air)) {
                            actions.viewport.denoiser_kind =
                                static_cast<uint8_t>(params.get_int(ParamId::RenderDenoiser));
                            actions.viewport.request_denoiser_apply = true;
                            out_settings_dirty = true;
                        }
                        draw_property_row(ParamId::RenderDebugView, params, undo, fonts, row_actions);
                        end_property_table();
                    }

                    ImGui::Spacing();
                    if (params.get_bool(ParamId::RenderCircadianEnabled)) {
                        const int ov_min = params.get_int(ParamId::RenderCircadianTimeMinutes);
                        const float hr = (ov_min >= 0) ? (static_cast<float>(ov_min) / 60.0f)
                                                       : render_view.circadian_hour_24;
                        const uint8_t ph =
                            (ov_min >= 0)
                                ? ((hr >= 5.0f && hr < 7.0f)     ? 1u
                                   : (hr >= 7.0f && hr < 15.5f)  ? 2u
                                   : (hr >= 15.5f && hr < 18.0f) ? 3u
                                                                 : 0u)
                                : render_view.circadian_phase;
                        static constexpr const char* kPhaseNames[4] = {
                            "Night Studio (Monitor + Moonlight)",
                            "Dawn",
                            "Daytime",
                            "Golden Hour Sunset",
                        };
                        const int hh = static_cast<int>(hr) % 24;
                        const int mm = std::clamp(static_cast<int>((hr - static_cast<float>(hh)) * 60.0f), 0, 59);
                        char cbuf[160]{};
                        std::snprintf(cbuf, sizeof(cbuf), "%02d:%02d · %s (Key %.0fK)", hh, mm,
                                      kPhaseNames[std::min<uint8_t>(ph, 3u)],
                                      static_cast<double>(render_view.circadian_key_kelvin));
                        draw_status_badge(fonts, tr("Solar Mood"), cbuf, t.status_ok);
                    }
                    draw_status_badge(fonts, tr("Active Denoiser"), render_view.denoiser_active_name,
                                      render_view.denoiser_fallback_triggered ? t.status_warn : t.status_ok);
                    if (render_view.denoiser_fallback_triggered && render_view.denoiser_fallback_reason[0] != '\0') {
                        draw_status_badge(fonts, tr("Fallback Note"), render_view.denoiser_fallback_reason,
                                          t.status_warn);
                    }
                    ImGui::Spacing();
                    if (render_view.timing_valid) {
                        char pass_buf[192]{};
                        std::snprintf(pass_buf, sizeof(pass_buf),
                                      "RT %.2f ms · Denoise %.2f ms (p95 %.2f) · NPR %.2f ms · Post %.2f ms · Encode %.2f ms",
                                      static_cast<double>(render_view.rt_gpu_ms),
                                      static_cast<double>(render_view.denoise_gpu_ms),
                                      static_cast<double>(render_view.denoise_p95_120f_ms),
                                      static_cast<double>(render_view.npr_gpu_ms),
                                      static_cast<double>(render_view.post_gpu_ms),
                                      static_cast<double>(render_view.encode_gpu_ms));
                        draw_status_badge(fonts, tr("Pass Breakdown"), pass_buf, t.text_primary);
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button(tr("Reset Render Defaults"))) {
            static constexpr ParamId kResetIds[] = {
                ParamId::RenderTargetFps,          ParamId::RenderVsync,
                ParamId::RenderBackgroundEco,      ParamId::RenderResolutionScalePct,
                ParamId::RenderAaMode,             ParamId::RenderLightingMode,
                ParamId::RenderFlatness,           ParamId::RenderFaceNormalFlatten,
                ParamId::RenderNeckSeamBlend,      ParamId::RenderCelBandSharpness,
                ParamId::RenderShadowWarmCool,
                ParamId::RenderIndirectBleedStrength, ParamId::RenderRimDepthOffsetPx,
                ParamId::RenderOutlineScale,       ParamId::RenderIroTsukeStrength,
                ParamId::RenderCreaseStrength,     ParamId::RenderHairHighlight,
                ParamId::RenderRimStepStrength,    ParamId::RenderBloomIntensity,
                ParamId::RenderRtEnabled,          ParamId::Render2DFilterPro,
                ParamId::RenderFilterKuwaharaStrength, ParamId::RenderFilterDogInkBoost,
                ParamId::RenderLightingFixture,    ParamId::RenderPhotometricBeamAngleDeg,
                ParamId::RenderEyeCatchlightIntensity, ParamId::RenderEyeCatchlightSize,
                ParamId::RenderCreaseDisguiseEnabled,  ParamId::RenderCreaseDisguiseStrength,
                ParamId::RenderFilterPaperWarmth,  ParamId::RenderFilterPaperWarmthStrength,
                ParamId::RenderCircadianEnabled,   ParamId::RenderCircadianTimezone,
                ParamId::RenderCircadianTimeMinutes,
            };
            for (const ParamId pid : kResetIds) {
                const double def_v = param_spec(pid).default_val;
                const double prev = params.get(pid);
                if (params.set(pid, def_v)) {
                    undo.push_param(pid, prev, def_v);
                }
            }
            actions.viewport.lighting_dirty = true;
            last_preset_applied_ = -1;
            out_settings_dirty = true;
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 108.0f * scale);
        if (ImGui::Button(tr("Close"), ImVec2(92.0f * scale, 0.0f))) {
            keep_open = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    open_ = keep_open;
}

} // namespace aether::ui
