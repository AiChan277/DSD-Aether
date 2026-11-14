#include "ui/wizard/hardware_optimizer_wizard.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace aether::ui {
namespace {

uint64_t fnv1a_step(uint64_t h, uint64_t v) noexcept {
    h ^= v;
    return h * 1099511628211ULL;
}

uint64_t fnv1a_str(uint64_t h, const std::string& s) noexcept {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

uint64_t HardwareOptimizerWizard::compute_inventory_hash(const HardwareInventory& hw) noexcept {
    uint64_t h = 14695981039346656037ULL;
    h = fnv1a_str(h, hw.cpu.brand_string);
    h = fnv1a_step(h, hw.cpu.logical_processors);
    h = fnv1a_step(h, hw.total_ram_mb);
    h = fnv1a_step(h, hw.npu.available ? 1ULL : 0ULL);
    for (const auto& g : hw.gpus) {
        h = fnv1a_step(h, (static_cast<uint64_t>(g.vendor_id) << 32) | g.device_id);
        h = fnv1a_step(h, g.dedicated_vram_mb);
    }
    h = fnv1a_step(h, hw.cameras.size());
    return h;
}

WizardRecommendation HardwareOptimizerWizard::recommend_for_hardware(const HardwareInventory& hw,
                                                                     const rhi::SelectedGpuInfo& gpu) noexcept {
    WizardRecommendation rec{};

    // Tier selection (§4 / §12.13): Tier 1 NPU -> Tier 2A dGPU -> Tier 2B iGPU -> Tier 3 CPU
    bool has_large_dgpu = gpu.is_discrete && gpu.dedicated_vram_mb >= 4096;
    if (!has_large_dgpu) {
        for (const auto& g : hw.gpus) {
            if (!g.is_software && g.dedicated_vram_mb >= 4096) {
                has_large_dgpu = true;
                break;
            }
        }
    }

    if (hw.npu.available) {
        rec.tracking_ep_mode = 1; // NPU (OpenVINO)
        rec.ep_override = AETHER_EP_INTEL_NPU;
        std::snprintf(rec.ep_reason.data(), rec.ep_reason.size(),
                      "Tier 1 NPU (%s) detected; isolates ORT inference from GPU render.",
                      hw.npu.device_name.empty() ? "OpenVINO" : hw.npu.device_name.c_str());
    } else if (has_large_dgpu) {
        rec.tracking_ep_mode = 2; // dGPU (DirectML)
        rec.ep_override = AETHER_EP_DIRECTML_GPU;
        std::snprintf(rec.ep_reason.data(), rec.ep_reason.size(),
                      "Tier 2A discrete GPU (>= 4 GiB VRAM) selected via DirectML.");
    } else if (!hw.gpus.empty() && !hw.gpus[0].is_software) {
        rec.tracking_ep_mode = 3; // iGPU (DirectML)
        rec.ep_override = AETHER_EP_DIRECTML_GPU;
        std::snprintf(rec.ep_reason.data(), rec.ep_reason.size(),
                      "Tier 2B integrated/compact GPU selected to offload CPU threads.");
    } else {
        rec.tracking_ep_mode = 4; // CPU
        rec.ep_override = AETHER_EP_CPU;
        std::snprintf(rec.ep_reason.data(), rec.ep_reason.size(), "Tier 3 CPU AVX2 fallback selected cleanly.");
    }

    // Denoiser & Resolution based on GPU capabilities
    rec.rt_enabled = gpu.rt_enabled;
    if (gpu.vendor_id == 0x10DE && gpu.rt_enabled) {
        rec.denoiser_kind = 0; // Auto (prefers NRD when present, else SVGF-Lite)
        rec.output_res_preset = (gpu.dedicated_vram_mb >= 8192) ? 1 : 0;
        rec.output_fps = 60;
        std::snprintf(rec.render_reason.data(), rec.render_reason.size(),
                      "Hardware ray-query enabled; Auto denoiser + %s 60 fps.",
                      rec.output_res_preset == 1 ? "1080p" : "720p");
    } else if (gpu.rt_enabled) {
        rec.denoiser_kind = 1; // Built-in SVGF-Lite (vendor-neutral fast path)
        rec.output_res_preset = 1;
        rec.output_fps = 60;
        std::snprintf(rec.render_reason.data(), rec.render_reason.size(),
                      "Inline ray-query enabled with Built-in SVGF-Lite denoiser at 1080p60.");
    } else {
        rec.denoiser_kind = 1;     // Built-in
        rec.output_res_preset = 0; // 720p60 safe budget without RT hardware
        rec.output_fps = 60;
        std::snprintf(rec.render_reason.data(), rec.render_reason.size(),
                      "No hardware ray-query; NPR + 720p60 targets <= 20.0 ms M2P.");
    }

    return rec;
}

bool HardwareOptimizerWizard::adjust_for_budget(WizardRecommendation& rec, float p95_m2p_ms) noexcept {
    if (p95_m2p_ms <= 20.0f) {
        return false;
    }
    bool changed = false;
    if (rec.output_res_preset > 0) {
        rec.output_res_preset = 0; // Drop to 720p
        changed = true;
    } else if (rec.rt_enabled) {
        rec.rt_enabled = false;
        changed = true;
    } else if (rec.denoiser_kind != 1) {
        rec.denoiser_kind = 1;
        changed = true;
    }
    if (changed) {
        rec.downshifted_by_budget = true;
        std::snprintf(rec.render_reason.data(), rec.render_reason.size(),
                      "Measured %.1f ms (> 20.0 ms): downshifted preset to stay in live budget.",
                      static_cast<double>(p95_m2p_ms));
    }
    return changed;
}

void HardwareOptimizerWizard::open(const HardwareInventory& hw, const rhi::SelectedGpuInfo& gpu, bool on_air) noexcept {
    if (on_air) {
        return;
    }
    open_ = true;
    step_ = 0;
    rec_ = recommend_for_hardware(hw, gpu);
    selected_camera_idx_ = hw.cameras.empty() ? -1 : 0;
    synthetic_feed_ = hw.cameras.empty();
    camera_fps_ = 60;
    anti_flicker_ = 2;
    benchmark_running_ = false;
    benchmark_samples_ = 0;
    benchmark_max_m2p_ = 0.0f;
    benchmark_sum_m2p_ = 0.0;
    benchmark_avg_m2p_ = 0.0f;
    benchmark_done_ = false;
}

bool HardwareOptimizerWizard::draw(bool on_air, const HardwareInventory& hw, const rhi::RhiDevice& rhi,
                                   const TelemetrySnapshot& telemetry, const DevShellTrackingView& tracking,
                                   const DevShellRenderView& render_view, ParamStore& params, AppSettings& settings,
                                   const UiFontSet& fonts, float dpi_scale, StudioActions& actions) {
    if (!open_) {
        return false;
    }
    if (on_air) {
        open_ = false;
        return false;
    }

    bool applied = false;
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList(vp);
    bg_dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                         ImGui::ColorConvertFloat4ToU32(t.scrim_modal));

    const float mw = std::min(t.wizard_w_px * scale, vp->Size.x - 32.0f * scale);
    const float mh = std::min(t.wizard_h_px * scale, vp->Size.y - 32.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.bg_overlay);
    ImGui::PushStyleColor(ImGuiCol_Border, t.border_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, t.radius_modal_px);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

    bool keep_open = open_;
    if (ImGui::Begin(tr("AETHER Hardware Optimizer"), &keep_open, kFlags)) {
        const ImVec2 wmin = ImGui::GetWindowPos();
        draw_elevation_shadow(bg_dl, wmin, ImVec2(wmin.x + mw, wmin.y + mh), t.radius_modal_px);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            keep_open = false;
        }

        // Stepper header
        const std::array<const char*, 5> step_names = {
            tr("1. Scan"), tr("2. Recommend"), tr("3. Camera"), tr("4. Benchmark"), tr("5. Finish"),
        };
        if (fonts.title != nullptr) {
            ImGui::PushFont(fonts.title);
        }
        ImGui::TextColored(t.text_primary, "%s", tr("Studio Setup & Latency Calibration"));
        if (fonts.title != nullptr) {
            ImGui::PopFont();
        }
        ImGui::Spacing();
        for (int i = 0; i < 5; ++i) {
            if (i > 0) {
                ImGui::SameLine(0.0f, 8.0f * scale);
            }
            const bool active = (i == step_);
            const bool done = (i < step_);
            const ImVec4 col = active ? t.accent : (done ? t.status_ok : t.text_tertiary);
            ImGui::TextColored(col, "%s", step_names[static_cast<size_t>(i)]);
        }
        ImGui::Separator();
        ImGui::Spacing();

        const float body_h = mh - 134.0f * scale;
        if (ImGui::BeginChild("##WizardBody", ImVec2(-1.0f, body_h), ImGuiChildFlags_None)) {
            const auto& gpu = rhi.gpu_info();
            if (step_ == 0) {
                ImGui::TextColored(t.text_secondary, "%s",
                                   tr("Detected host compute, graphics, neural processor, and capture hardware:"));
                ImGui::Spacing();
                ImGui::Text("CPU: %s (%u P-cores / %u E-cores, %u logical)", hw.cpu.brand_string.c_str(),
                            hw.cpu.p_core_count, hw.cpu.e_core_count, hw.cpu.logical_processors);
                ImGui::Text("Memory: %.1f GiB system RAM", static_cast<double>(hw.total_ram_mb) / 1024.0);
                ImGui::Text("Active GPU: %s (%.1f GiB VRAM, Ray-Query: %s)", gpu.device_name.c_str(),
                            static_cast<double>(gpu.dedicated_vram_mb) / 1024.0,
                            gpu.rt_enabled ? "Supported" : "Unavailable");
                ImGui::Text("NPU Probe: %s",
                            hw.npu.available ? (hw.npu.device_name.empty() ? "Available" : hw.npu.device_name.c_str())
                                             : "Not detected (will use GPU/CPU fallback)");
                ImGui::Text("Webcams Enumerated: %zu device(s) (not opened until selected)", hw.cameras.size());
            } else if (step_ == 1) {
                const std::array<const char*, 5> ep_names = {"Auto", "Tier 1 NPU (OpenVINO)", "Tier 2A dGPU (DirectML)",
                                                             "Tier 2B iGPU (DirectML)", "Tier 3 CPU"};
                const std::array<const char*, 5> dn_names = {"Auto", "Built-in SVGF-Lite", "NRD", "OIDN", "FidelityFX"};
                const std::array<const char*, 4> res_names = {"1280x720", "1920x1080", "2560x1440", "3840x2160"};

                ImGui::TextColored(t.text_primary, "%s", tr("Recommended Live Configuration:"));
                ImGui::Spacing();
                ImGui::Combo(tr("Tracking Execution Provider"), &rec_.tracking_ep_mode, ep_names.data(),
                             static_cast<int>(ep_names.size()));
                rec_.ep_override = (rec_.tracking_ep_mode == 0) ? -1 : (rec_.tracking_ep_mode - 1);
                ImGui::TextColored(t.text_tertiary, "%s", rec_.ep_reason.data());
                ImGui::Spacing();

                int dn = rec_.denoiser_kind;
                if (ImGui::Combo(tr("RT Shadow Denoiser"), &dn, dn_names.data(), static_cast<int>(dn_names.size()))) {
                    rec_.denoiser_kind = static_cast<uint8_t>(dn);
                }
                int rp = rec_.output_res_preset;
                if (ImGui::Combo(tr("Output Resolution"), &rp, res_names.data(), static_cast<int>(res_names.size()))) {
                    rec_.output_res_preset = static_cast<uint8_t>(rp);
                }
                ImGui::Checkbox(tr("Enable Ray-Traced Shadows & Bounce"), &rec_.rt_enabled);
                ImGui::TextColored(t.text_tertiary, "%s", rec_.render_reason.data());
            } else if (step_ == 2) {
                ImGui::TextColored(t.text_secondary, "%s",
                                   tr("Choose camera source. Streams stay strictly local (HR-12):"));
                ImGui::Spacing();
                ImGui::Checkbox(tr("Use Deterministic Synthetic Camera (Offline / Headless Safe)"), &synthetic_feed_);
                if (!hw.cameras.empty()) {
                    for (int i = 0; i < static_cast<int>(hw.cameras.size()); ++i) {
                        const bool sel = (!synthetic_feed_ && selected_camera_idx_ == i);
                        if (ImGui::RadioButton(hw.cameras[static_cast<size_t>(i)].friendly_name.c_str(), sel)) {
                            selected_camera_idx_ = i;
                            synthetic_feed_ = false;
                        }
                    }
                } else {
                    ImGui::TextColored(t.text_tertiary, "%s",
                                       tr("No physical webcam detected; synthetic feed enabled."));
                }
                ImGui::Separator();
                int fps_idx = (camera_fps_ == 30) ? 0 : 1;
                const std::array<const char*, 2> fps_opts = {"30 fps", "60 fps"};
                if (ImGui::Combo(tr("Camera Target FPS"), &fps_idx, fps_opts.data(), 2)) {
                    camera_fps_ = (fps_idx == 0) ? 30 : 60;
                }
                const std::array<const char*, 3> af_opts = {"Off", "50 Hz", "60 Hz"};
                ImGui::Combo(tr("Anti-Flicker"), &anti_flicker_, af_opts.data(), 3);
            } else if (step_ == 3) {
                ImGui::TextColored(
                    t.text_secondary, "%s",
                    tr("Measure 120 live pipeline frames against the <= 20.0 ms motion-to-photon budget:"));
                ImGui::Spacing();

                if (!benchmark_running_ &&
                    ImGui::Button(benchmark_done_ ? tr("Re-run 120-Frame Check") : tr("Run 120-Frame Check"))) {
                    benchmark_running_ = true;
                    benchmark_done_ = false;
                    benchmark_samples_ = 0;
                    benchmark_sum_m2p_ = 0.0;
                    benchmark_max_m2p_ = 0.0f;
                }

                if (benchmark_running_) {
                    // Non-blocking per-frame accumulator on T3 (zero Sleep / zero stall, HR-11)
                    const float m2p_now = telemetry.m2p.p95_ms > 0.1f
                                              ? telemetry.m2p.p95_ms
                                              : (tracking.inference_ms + render_view.total_gpu_ms + 3.2f);
                    benchmark_sum_m2p_ += static_cast<double>(m2p_now);
                    benchmark_max_m2p_ = std::max(benchmark_max_m2p_, m2p_now);
                    ++benchmark_samples_;
                    ImGui::ProgressBar(static_cast<float>(benchmark_samples_) / 120.0f, ImVec2(-1.0f, 18.0f * scale));
                    if (benchmark_samples_ >= 120) {
                        benchmark_running_ = false;
                        benchmark_done_ = true;
                        benchmark_avg_m2p_ = static_cast<float>(benchmark_sum_m2p_ / 120.0);
                        (void)adjust_for_budget(rec_, benchmark_max_m2p_);
                    }
                } else if (benchmark_done_) {
                    const bool pass = benchmark_max_m2p_ <= 20.0f;
                    ImGui::TextColored(pass ? t.status_ok : t.status_warn,
                                       "120-Frame Mean: %.2f ms  |  Peak/p95: %.2f ms  (Target <= 20.0 ms)",
                                       static_cast<double>(benchmark_avg_m2p_),
                                       static_cast<double>(benchmark_max_m2p_));
                    if (rec_.downshifted_by_budget) {
                        ImGui::TextColored(t.status_warn, "%s", rec_.render_reason.data());
                    }
                } else {
                    ImGui::Text("Current Telemetry: T1 %.2f ms | GPU %.2f ms | M2P p95 %.2f ms",
                                static_cast<double>(tracking.inference_ms),
                                static_cast<double>(render_view.total_gpu_ms),
                                static_cast<double>(telemetry.m2p.p95_ms));
                }
            } else {
                ImGui::TextColored(t.status_ok, "%s %s", icons::kCheck, tr("Configuration Ready"));
                ImGui::Spacing();
                ImGui::BulletText("Tracking EP mode: %d (%s)", rec_.tracking_ep_mode, rec_.ep_reason.data());
                ImGui::BulletText("Ray Tracing: %s | Denoiser: %u", rec_.rt_enabled ? "On" : "Off",
                                  static_cast<unsigned>(rec_.denoiser_kind));
                ImGui::BulletText("Output: Preset %u @ %u fps", static_cast<unsigned>(rec_.output_res_preset),
                                  rec_.output_fps);
                ImGui::BulletText("Camera: %s (%d fps)",
                                  (!synthetic_feed_ && selected_camera_idx_ >= 0 &&
                                   selected_camera_idx_ < static_cast<int>(hw.cameras.size()))
                                      ? hw.cameras[static_cast<size_t>(selected_camera_idx_)].friendly_name.c_str()
                                      : "Synthetic Feed",
                                  camera_fps_);
                ImGui::Spacing();
                ImGui::TextColored(t.text_tertiary, "%s",
                                   tr("Applies settings atomically to settings.toml without interrupting OBS scenes."));
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button(tr("Skip for now"))) {
            keep_open = false;
        }
        ImGui::SameLine();
        if (step_ > 0) {
            if (ImGui::Button(tr("Back"))) {
                --step_;
            }
            ImGui::SameLine();
        }
        if (step_ < 4) {
            if (ImGui::Button(tr("Next"))) {
                ++step_;
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, t.accent_button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.accent_hover);
            if (ImGui::Button(tr("Apply & Finish"))) {
                params.set(ParamId::TrackingEpMode, static_cast<double>(rec_.tracking_ep_mode));
                params.set(ParamId::RenderDenoiser, static_cast<double>(rec_.denoiser_kind));
                params.set(ParamId::RenderRtEnabled, rec_.rt_enabled ? 1.0 : 0.0);
                params.set(ParamId::OutputResolution, static_cast<double>(rec_.output_res_preset));
                params.set(ParamId::OutputFps, static_cast<double>(rec_.output_fps));
                params.set(ParamId::TrackingCameraFps, static_cast<double>(camera_fps_));
                params.set(ParamId::TrackingAntiFlicker, static_cast<double>(anti_flicker_));
                params.set(ParamId::TrackingSyntheticCamera, synthetic_feed_ ? 1.0 : 0.0);

                settings.wizard_completed = true;
                settings.hw_inventory_hash = compute_inventory_hash(hw);
                if (!synthetic_feed_ && selected_camera_idx_ >= 0 &&
                    selected_camera_idx_ < static_cast<int>(hw.cameras.size())) {
                    settings.preferred_camera_link =
                        hw.cameras[static_cast<size_t>(selected_camera_idx_)].friendly_name;
                } else {
                    settings.preferred_camera_link.clear();
                }

                actions.request_wizard_apply = true;
                actions.wizard_ep_override = rec_.ep_override;
                actions.wizard_denoiser = rec_.denoiser_kind;
                actions.wizard_res_preset = rec_.output_res_preset;
                actions.wizard_fps = rec_.output_fps;

                applied = true;
                keep_open = false;
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    open_ = keep_open;
    return applied;
}

} // namespace aether::ui
