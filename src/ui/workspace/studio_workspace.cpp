#include "ui/workspace/studio_workspace.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace aether::ui {
namespace {

void apply_custom_xform_payload(ParamStore* params, const std::string& payload) noexcept {
    if (params == nullptr) {
        return;
    }
    constexpr std::string_view kPrefix = "xform:";
    if (payload.size() <= kPrefix.size() || std::string_view(payload).substr(0, kPrefix.size()) != kPrefix) {
        return;
    }
    const char* cur = payload.data() + kPrefix.size();
    const char* end = payload.data() + payload.size();
    std::array<float, 5> vals{0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < vals.size(); ++i) {
        const auto res = std::from_chars(cur, end, vals[i]);
        if (res.ec != std::errc()) {
            return;
        }
        cur = res.ptr;
        if (i + 1 < vals.size()) {
            if (cur >= end || *cur != ';') {
                return;
            }
            ++cur;
        }
    }
    (void)params->set(ParamId::AvatarPosX, static_cast<double>(vals[0]));
    (void)params->set(ParamId::AvatarPosY, static_cast<double>(vals[1]));
    (void)params->set(ParamId::AvatarPosZ, static_cast<double>(vals[2]));
    (void)params->set(ParamId::AvatarRotY, static_cast<double>(vals[3]));
    (void)params->set(ParamId::AvatarScale, static_cast<double>(vals[4]));
}

const char* kPanelOutliner = "Outliner & Project";
const char* kPanelViewport = "Viewport";
const char* kPanelInspector = "Inspector";
const char* kPanelNodeGraph = "Node Graph";
const char* kPanelConsole = "Console";
const char* kPanelLightMixer = "Light Mixer";
const char* kPanelExpressions = "Expressions";
const char* kPanelTimeline = "Performance Timeline";

// v2: broadcast-overhaul dock geometry; older per-mode layouts are ignored so the new defaults apply once.
constexpr const char* kLayoutIniFmt = "mode_v2_%d.ini";
constexpr double kExpressionRampSeconds = 0.18;

void safe_copy_cstr(char* dst, size_t cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

} // namespace

StudioWorkspace::StudioWorkspace() {
    bones_.reserve(256);
    materials_.reserve(64);
    expressions_.reserve(16);
    plugins_.reserve(32);

    // Seed default 8 Quick Expression slots (§12.7)
    static constexpr std::array<const char*, 8> kDefaultExprs = {"Smile", "Wink L",       "Surprised", "Angry",
                                                                 "Sad",   "Sparkle Eyes", "Blush",     "Reset Neutral"};
    for (uint32_t i = 0; i < 8; ++i) {
        ExpressionSlotInfo e{};
        e.slot = i;
        std::snprintf(e.name.data(), e.name.size(), "%s", kDefaultExprs[i]);
        std::snprintf(e.hotkey.data(), e.hotkey.size(), "F%u", i + 1);
        expressions_.push_back(e);
    }
}

void StudioWorkspace::init(const RuntimePaths& paths, const HardwareInventory& hw, float dpi_scale,
                           const char* initial_avatar_path, bool allow_disk_config, core::SessionManager* session_mgr) {
    paths_ = paths;
    hw_ = hw;
    os_dpi_scale_ = std::max(0.75f, dpi_scale);
    allow_disk_config_ = allow_disk_config;
    session_mgr_ = session_mgr;
    config_dir_ = paths_.app_data_dir.empty() ? default_config_dir() : std::filesystem::path(paths_.app_data_dir);
    // Warm up the static `.alfx` preset catalog once off the hot path so Frame 0 performs 0 allocations.
    (void)core::AlfxSerializer::builtin_preset_catalog();

    std::string ref_vrm = resolve_asset_path(paths_, "samples/reference.vrm");
    if (!std::filesystem::exists(ref_vrm) && allow_disk_config_) {
#if defined(_WIN32)
        char* userprofile = nullptr;
        size_t up_len = 0;
        if (_dupenv_s(&userprofile, &up_len, "USERPROFILE") == 0 && userprofile != nullptr) {
            std::filesystem::path dl_vrm = std::filesystem::path(userprofile) / "Downloads" / "8590256991748008892.vrm";
            if (std::filesystem::is_regular_file(dl_vrm)) {
                ref_vrm = dl_vrm.string();
            }
            std::free(userprofile);
        }
#endif
    }
    std::string vrm1 = resolve_asset_path(paths_, "samples/sample_vrm1.vrm");
    std::string fbx = resolve_asset_path(paths_, "samples/sample_humanoid.fbx");
    std::string pmx = resolve_asset_path(paths_, "samples/sample_mmd.pmx");
    outliner_panel_.set_sample_paths(ref_vrm.c_str(), vrm1.c_str(), fbx.c_str(), pmx.c_str());

    if (initial_avatar_path != nullptr && initial_avatar_path[0] != '\0') {
        outliner_panel_.set_avatar_path_buf(initial_avatar_path);
    } else if (std::filesystem::exists(ref_vrm)) {
        outliner_panel_.set_avatar_path_buf(ref_vrm.c_str());
    }

    if (allow_disk_config_) {
        std::error_code ec;
        std::filesystem::create_directories(config_dir_ / "layouts", ec);
        if (session_mgr_ != nullptr) {
            load_app_settings_from_session(session_mgr_->state(), settings_, params_);
        } else {
            (void)load_app_settings_toml(config_dir_ / "settings.toml", settings_, params_);
        }
        (void)keymap_.load_toml(config_dir_ / "keymap.toml");
        viewport_panel_.set_persist_presets(true);
        viewport_panel_.preset_bank().set_storage_path(resolve_camera_presets_json_path(paths_));
        viewport_panel_.set_persist_presets(false);
        if (!viewport_panel_.preset_bank().load_default() && !paths_.assets_dir.empty()) {
            const auto shipped_cam =
                std::filesystem::path(paths_.assets_dir).parent_path() / "config" / "camera_presets.json";
            (void)viewport_panel_.preset_bank().load_from_file(shipped_cam.string());
        }

        // Open Hardware Optimizer Wizard cleanly on first launch or new hardware fingerprint
        const uint64_t inv_hash = HardwareOptimizerWizard::compute_inventory_hash(hw_);
        if (!settings_.wizard_completed ||
            (settings_.hw_inventory_hash != 0 && settings_.hw_inventory_hash != inv_hash)) {
            if (!params_.get_bool(ParamId::TrackingSyntheticCamera)) {
                // Only auto-open interactively when settings indicate desktop run
            }
            wizard_.open(hw_, rhi::SelectedGpuInfo{}, host_status_.on_air);
        }
    } else {
        viewport_panel_.set_persist_presets(false);
    }
    if (params_.get_int(ParamId::TrackingEpMode) == 4) {
        params_.set(ParamId::TrackingEpMode, 0.0);
    }
    last_synced_ep_mode_ = params_.get_int(ParamId::TrackingEpMode);
    last_synced_synthetic_cam_ = params_.get_bool(ParamId::TrackingSyntheticCamera);
    last_synced_hand_tracking_ = params_.get_bool(ParamId::TrackingHandEnabled);

    set_high_contrast_theme(params_.get_bool(ParamId::AppHighContrast));
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().IniFilename = nullptr;
        fonts_ = load_precision_dark_fonts(paths_, effective_dpi_scale());
        apply_precision_dark_style(ImGui::GetStyle(), effective_dpi_scale(),
                                   params_.get_bool(ParamId::AppHighContrast));
        if (allow_disk_config_) {
            load_mode_layout_ini(mode_);
        }
    }

    command_palette_.rebuild_entries(keymap_, settings_.recent_projects);
    lighting_stage_.set_config_dir(config_dir_);
    if (!paths_.assets_dir.empty()) {
        lighting_stage_.set_shipped_config_dir(std::filesystem::path(paths_.assets_dir).parent_path() / "config");
    }
    reset_dirty_tracking();
    synced_param_epoch_ = 0; // force initial ParamStore -> actions sync
    dock_layout_dirty_ = true;
}

void StudioWorkspace::shutdown() {
    if (allow_disk_config_) {
        save_mode_layout_ini(mode_);
        save_app_config_files();
    }
}

void StudioWorkspace::save_app_config_files() {
    if (!allow_disk_config_) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(config_dir_ / "layouts", ec);
    if (session_mgr_ != nullptr) {
        save_app_settings_to_session(settings_, params_, session_mgr_->state());
        (void)session_mgr_->save_session_state();
    } else {
        (void)save_app_settings_toml(config_dir_ / "settings.toml", settings_, params_);
    }
    (void)keymap_.save_toml(config_dir_ / "keymap.toml");
    settings_dirty_ = false;
}

void StudioWorkspace::save_mode_layout_ini(StudioMode mode) const {
    if (!allow_disk_config_ || ImGui::GetCurrentContext() == nullptr || layout_maximized_) {
        return;
    }
    std::array<char, 32> fname{};
    std::snprintf(fname.data(), fname.size(), kLayoutIniFmt, static_cast<int>(mode));
    const auto p = config_dir_ / "layouts" / fname.data();
    const std::string p_str = p.string();
    ImGui::SaveIniSettingsToDisk(p_str.c_str());
}

void StudioWorkspace::load_mode_layout_ini(StudioMode mode) {
    if (!allow_disk_config_ || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    std::array<char, 32> fname{};
    std::snprintf(fname.data(), fname.size(), kLayoutIniFmt, static_cast<int>(mode));
    const auto p = config_dir_ / "layouts" / fname.data();
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        const std::string p_str = p.string();
        ImGui::LoadIniSettingsFromDisk(p_str.c_str());
        dock_layout_dirty_ = false;
    } else {
        dock_layout_dirty_ = true;
    }
}

void StudioWorkspace::on_dpi_or_theme_changed(float os_dpi_scale) {
    os_dpi_scale_ = std::max(0.75f, os_dpi_scale);
    const bool hc = params_.get_bool(ParamId::AppHighContrast);
    set_high_contrast_theme(hc);
    if (ImGui::GetCurrentContext() != nullptr) {
        apply_precision_dark_style(ImGui::GetStyle(), effective_dpi_scale(), hc);
        fonts_ = load_precision_dark_fonts(paths_, effective_dpi_scale());
    }
}

void StudioWorkspace::rebuild_fonts_if_needed(const RuntimePaths& paths, float effective_scale) {
    if (ImGui::GetCurrentContext() != nullptr) {
        fonts_ = load_precision_dark_fonts(paths, std::max(0.75f, effective_scale));
        apply_precision_dark_style(ImGui::GetStyle(), std::max(0.75f, effective_scale),
                                   params_.get_bool(ParamId::AppHighContrast));
    }
}

void StudioWorkspace::on_log_drained(const LogRecord& rec, void* user) noexcept {
    auto* self = static_cast<StudioWorkspace*>(user);
    if (self == nullptr) {
        return;
    }
    self->recent_logs_[self->log_head_] = rec;
    self->log_head_ = (self->log_head_ + 1U) % kLogRingDisplayCap;
    if (self->log_count_ < kLogRingDisplayCap) {
        ++self->log_count_;
    }
}

void StudioWorkspace::drain_logs() noexcept {
    (void)LogSystem::drain(on_log_drained, this, 64);
}

void StudioWorkspace::set_graph_json_from_host(const std::string& json_utf8) {
    node_canvas_.set_graph_json(json_utf8);
}

void StudioWorkspace::sync_plugins(std::span<const LoadedPluginVm> plugins) {
    plugins_.assign(plugins.begin(), plugins.end());
}

void StudioWorkspace::sync_scene_items(std::span<const OutlinerBoneInfo> bones,
                                       std::span<const OutlinerMaterialInfo> materials,
                                       std::span<const ExpressionSlotInfo> expressions) {
    bones_.assign(bones.begin(), bones.end());
    materials_.assign(materials.begin(), materials.end());
    if (!expressions.empty()) {
        expressions_.assign(expressions.begin(), expressions.end());
    }
}

void StudioWorkspace::push_toast(ToastSeverity sev, const char* title, const char* detail, double now_s,
                                 ToastActionKind action, const char* action_label) noexcept {
    toasts_.push(sev, title, detail, now_s, action, action_label);
}

void StudioWorkspace::request_plugin_consent(const char* plugin_id, const char* plugin_name, const char* sha256_hex,
                                             uint32_t capability_mask) noexcept {
    consent_prompt_.open = true;
    safe_copy_cstr(consent_prompt_.plugin_id.data(), consent_prompt_.plugin_id.size(), plugin_id);
    safe_copy_cstr(consent_prompt_.plugin_name.data(), consent_prompt_.plugin_name.size(), plugin_name);
    safe_copy_cstr(consent_prompt_.sha256_hex.data(), consent_prompt_.sha256_hex.size(), sha256_hex);
    consent_prompt_.capability_mask = capability_mask;
}

void StudioWorkspace::open_hardware_wizard() noexcept {
    if (host_status_.on_air) {
        push_toast(ToastSeverity::Warning, "Blocked While ON AIR",
                   "Hardware Optimizer cannot run while broadcasting live.", last_now_s_);
        return;
    }
    wizard_.open(hw_, rhi::SelectedGpuInfo{}, host_status_.on_air);
}

void StudioWorkspace::switch_mode(StudioMode new_mode) {
    if (new_mode == mode_) {
        return;
    }
    save_mode_layout_ini(mode_);
    layout_maximized_ = false;
    mode_ = new_mode;
    synced_param_epoch_ = 0; // Rigging forces the skeleton overlay: resync viewport actions now
    if (allow_disk_config_) {
        load_mode_layout_ini(mode_);
    } else {
        dock_layout_dirty_ = true;
    }
}

void StudioWorkspace::reset_current_layout() {
    layout_maximized_ = false;
    dock_layout_dirty_ = true;
}

void StudioWorkspace::build_dock_layout(ImGuiID dockspace_id, StudioMode mode) {
    dock_layout_dirty_ = false;
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);

    // §2.2 dock geometry: Outliner 275 px * dpi, Inspector 325 px * dpi (wider in Rigging), never more
    // than ~30% of the window each. Nodes mode gives the bottom Node Graph 65% of the center height.
    const PrecisionDarkTokens& t = tokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float dpi = std::max(0.75f, effective_dpi_scale());
    ImVec2 avail_size = ImVec2(1440.0f, 900.0f);
    if (vp != nullptr) {
        const float top_h = std::max(ImGui::GetFrameHeight(), vp->WorkPos.y - vp->Pos.y);
        const float bot_h = std::round(t.status_bar_h_px * dpi);
        const ImVec2 dock_pos(vp->Pos.x, vp->Pos.y + top_h);
        avail_size = ImVec2(vp->Size.x, std::max(120.0f, vp->Size.y - top_h - bot_h));
        ImGui::DockBuilderSetNodePos(dockspace_id, dock_pos);
        ImGui::DockBuilderSetNodeSize(dockspace_id, avail_size);
    } else {
        ImGui::DockBuilderSetNodeSize(dockspace_id, avail_size);
    }

    ImGuiID main_id = dockspace_id;
    if (layout_maximized_ && maximized_window_[0] != '\0') {
        ImGui::DockBuilderDockWindow(maximized_window_.data(), main_id);
        ImGui::DockBuilderFinish(dockspace_id);
        return;
    }

    const float total_w = std::max(320.0f, avail_size.x);
    const float left_px = t.left_dock_w_px * dpi * (mode == StudioMode::Rigging ? 1.05f : 1.0f);
    const float right_px = t.right_dock_w_px * dpi * (mode == StudioMode::Rigging ? 1.06f : 1.0f);
    const float max_left_frac = (avail_size.x < 680.0f) ? 0.24f : 0.30f;
    const float max_right_frac = (avail_size.x < 680.0f) ? 0.26f : 0.36f;
    const float left_frac = std::clamp(left_px / total_w, 0.12f, max_left_frac);
    const float right_frac = std::clamp(right_px / (total_w * (1.0f - left_frac)), 0.14f, max_right_frac);
    float bottom_frac = (mode == StudioMode::Nodes) ? 0.65f : (mode == StudioMode::Rigging ? 0.24f : 0.28f);
    if (avail_size.y < 420.0f && mode != StudioMode::Nodes) {
        bottom_frac = std::min(bottom_frac, 0.22f);
    }

    ImGuiID left_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left, left_frac, nullptr, &main_id);
    ImGuiID right_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, right_frac, nullptr, &main_id);
    ImGuiID bottom_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Down, bottom_frac, nullptr, &main_id);

    ImGui::DockBuilderDockWindow(kPanelOutliner, left_id);
    ImGui::DockBuilderDockWindow(kPanelInspector, right_id);
    ImGui::DockBuilderDockWindow(kPanelViewport, main_id);
    ImGui::DockBuilderDockWindow(kPanelNodeGraph, bottom_id);
    ImGui::DockBuilderDockWindow(kPanelConsole, bottom_id);
    ImGui::DockBuilderDockWindow(kPanelLightMixer, bottom_id);
    ImGui::DockBuilderDockWindow(kPanelExpressions, bottom_id);
    ImGui::DockBuilderDockWindow(kPanelTimeline, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);

    // Bring the mode's primary bottom tab to front once its window exists.
    const char* focus_bottom = kPanelNodeGraph;
    if (mode == StudioMode::LiveStudio) {
        focus_bottom = kPanelConsole;
    } else if (mode == StudioMode::Lighting) {
        focus_bottom = kPanelLightMixer;
    }
    safe_copy_cstr(focus_tab_after_layout_.data(), focus_tab_after_layout_.size(), focus_bottom);
}

namespace {

[[nodiscard]] aether::DirtyDomain dirty_domain_for_param(std::string_view id) noexcept {
    using aether::DirtyDomain;
    if (id.starts_with("avatar.") || id.starts_with("sim.")) {
        return DirtyDomain::AvatarState;
    }
    if (id.starts_with("render.circadian_") || id == "render.light_scale") {
        return DirtyDomain::LightingRig;
    }
    if (id == "render.denoiser" || id == "render.backend" || id == "render.target_fps" || id == "render.vsync" ||
        id == "render.background_eco" || id == "render.resolution_scale_pct" || id == "render.aa_mode" ||
        id == "render.debug_view") {
        return DirtyDomain::None; // Global workstation / display preferences
    }
    if (id.starts_with("render.")) {
        return DirtyDomain::EffectsFx;
    }
    if (id == "dsp.user_offset_ms") {
        return DirtyDomain::OutputsCfg;
    }
    if (id.starts_with("output.")) {
        return (id == "output.spout_cpu_fallback" || id == "output.osc_enabled" || id == "output.ws_enabled")
                   ? DirtyDomain::None
                   : DirtyDomain::OutputsCfg;
    }
    if (id == "tracking.hand_enabled" || id == "tracking.smoothing_cutoff_hz" || id == "tracking.audio_assist" ||
        id.starts_with("tracking.mic_") || id.starts_with("tracking.npu_")) {
        return DirtyDomain::TrackingCfg;
    }
    if (id == "app.viewport_aspect" || id == "app.camera_transition_speed") {
        return DirtyDomain::CameraState;
    }
    return DirtyDomain::None;
}

} // namespace

void StudioWorkspace::reset_dirty_tracking() noexcept {
    dirty_param_epoch_ = params_.epoch();
    for (size_t i = 0; i < kParamCount; ++i) {
        dirty_param_snapshot_[i] = params_.get(static_cast<ParamId>(i));
    }
}

void StudioWorkspace::sync_params_to_actions(StudioActions& actions) noexcept {
    // Wireframe, Rigging collider overlays, and live AvatarRoot transform stay synced every frame
    // even when StudioActions was freshly default-constructed without a ParamStore epoch bump.
    actions.viewport.wireframe = viewport_panel_.wireframe_enabled();
    actions.viewport.show_rig_colliders = viewport_panel_.rig_colliders_enabled() || (mode_ == StudioMode::Rigging);
    actions.viewport.show_skeleton_heatmap =
        params_.get_bool(ParamId::AppSkeletonHeatmap) || (mode_ == StudioMode::Rigging);
    actions.viewport.avatar_pos[0] = params_.get_float(ParamId::AvatarPosX);
    actions.viewport.avatar_pos[1] = params_.get_float(ParamId::AvatarPosY);
    actions.viewport.avatar_pos[2] = params_.get_float(ParamId::AvatarPosZ);
    actions.viewport.avatar_rot_y_deg = params_.get_float(ParamId::AvatarRotY);
    actions.viewport.avatar_scale = params_.get_float(ParamId::AvatarScale);
    actions.viewport.viewport_aspect_mode =
        static_cast<core::ViewportAspectMode>(std::clamp(params_.get_int(ParamId::AppViewportAspect), 0, 3));
    actions.viewport.camera_transition_speed_s = params_.get_float(ParamId::AppCameraTransitionSpeed);

    if (synced_param_epoch_ == params_.epoch()) {
        return;
    }
    synced_param_epoch_ = params_.epoch();

    auto& v = actions.viewport;
    v.live2d_flatness = params_.get_float(ParamId::RenderFlatness);
    v.outline_scale = params_.get_float(ParamId::RenderOutlineScale);
    v.bloom_intensity = params_.get_float(ParamId::RenderBloomIntensity);
    v.face_normal_flatten = params_.get_float(ParamId::RenderFaceNormalFlatten);
    v.neck_seam_blend = params_.get_float(ParamId::RenderNeckSeamBlend);
    v.cel_band_sharpness = params_.get_float(ParamId::RenderCelBandSharpness);
    v.shadow_warm_cool_shift = params_.get_float(ParamId::RenderShadowWarmCool);
    v.iro_tsuke_strength = params_.get_float(ParamId::RenderIroTsukeStrength);
    v.crease_strength = params_.get_float(ParamId::RenderCreaseStrength);
    v.hair_highlight_intensity = params_.get_float(ParamId::RenderHairHighlight);
    v.rim_step_strength = params_.get_float(ParamId::RenderRimStepStrength);
    v.lighting_mode = static_cast<uint8_t>(std::clamp(params_.get_int(ParamId::RenderLightingMode), 0, 2));
    v.indirect_bleed_strength = params_.get_float(ParamId::RenderIndirectBleedStrength);
    v.rim_depth_offset_px = params_.get_float(ParamId::RenderRimDepthOffsetPx);
    v.filter_2d_pro = params_.get_bool(ParamId::Render2DFilterPro);
    v.filter_kuwahara_strength = params_.get_float(ParamId::RenderFilterKuwaharaStrength);
    v.filter_dog_ink_boost = params_.get_float(ParamId::RenderFilterDogInkBoost);
    v.filter_paper_warmth = params_.get_bool(ParamId::RenderFilterPaperWarmth);
    v.filter_paper_warmth_strength = params_.get_float(ParamId::RenderFilterPaperWarmthStrength);
    v.eye_catchlight_intensity = params_.get_float(ParamId::RenderEyeCatchlightIntensity);
    v.eye_catchlight_size = params_.get_float(ParamId::RenderEyeCatchlightSize);
    v.crease_disguise_enabled = params_.get_bool(ParamId::RenderCreaseDisguiseEnabled);
    v.crease_disguise_strength = params_.get_float(ParamId::RenderCreaseDisguiseStrength);
    v.circadian_enabled = params_.get_bool(ParamId::RenderCircadianEnabled);
    v.circadian_timezone_idx = params_.get_int(ParamId::RenderCircadianTimezone);
    v.circadian_time_override_min = params_.get_int(ParamId::RenderCircadianTimeMinutes);
    v.satsuei_light_wrap = params_.get_float(ParamId::RenderSatsueiLightWrap);
    v.satsuei_halation = params_.get_float(ParamId::RenderSatsueiHalation);
    v.satsuei_backscatter = params_.get_float(ParamId::RenderSatsueiBackscatter);
    v.holo_avatar_contrast = params_.get_float(ParamId::RenderHoloAvatarContrast);
    v.holo_bg_exposure = params_.get_float(ParamId::RenderHoloBgExposure);
    v.npu_bounds_enabled = params_.get_bool(ParamId::TrackingNpuBoundsEnabled);
    v.npu_anti_clip_enabled = params_.get_bool(ParamId::TrackingNpuAntiClipEnabled);
    v.npu_bounds_half = {params_.get_float(ParamId::TrackingNpuBoundsX), params_.get_float(ParamId::TrackingNpuBoundsY),
                         params_.get_float(ParamId::TrackingNpuBoundsZ)};
    v.npu_soft_elasticity = params_.get_float(ParamId::TrackingNpuSoftElasticity);
    v.npu_max_pitch_deg = params_.get_float(ParamId::TrackingNpuMaxPitchDeg);
    v.npu_anti_clip_margin = params_.get_float(ParamId::TrackingNpuAntiClipMargin);
    v.npu_bounds_dirty = true;
    v.target_fps_mode = static_cast<uint8_t>(params_.get_int(ParamId::RenderTargetFps));
    v.vsync_enabled = params_.get_bool(ParamId::RenderVsync);
    v.background_eco_mode = params_.get_bool(ParamId::RenderBackgroundEco);
    v.render_resolution_scale =
        std::clamp(static_cast<float>(params_.get_int(ParamId::RenderResolutionScalePct)) * 0.01f, 0.50f, 2.00f);
    v.aa_mode = static_cast<uint8_t>(params_.get_int(ParamId::RenderAaMode));
    v.rt_enabled = params_.get_bool(ParamId::RenderRtEnabled);
    v.debug_view = static_cast<uint8_t>(params_.get_int(ParamId::RenderDebugView));
    v.denoiser_kind = static_cast<uint8_t>(params_.get_int(ParamId::RenderDenoiser));
    v.render_backend = static_cast<uint8_t>(params_.get_int(ParamId::RenderBackend));
    // Rigging mode always shows the joint/bone overlay in the viewport.
    v.show_skeleton_heatmap = params_.get_bool(ParamId::AppSkeletonHeatmap) || mode_ == StudioMode::Rigging;
    v.show_checkerboard = params_.get_bool(ParamId::AppCheckerboardBg);
    v.show_orbit_orb = params_.get_bool(ParamId::AppOrbitOrbVisible);
    v.lighting_dirty = true;

    v.output_res_preset = static_cast<uint8_t>(params_.get_int(ParamId::OutputResolution));
    v.output_fps = static_cast<uint32_t>(std::max(30, params_.get_int(ParamId::OutputFps)));
    v.output_premul_alpha = params_.get_bool(ParamId::OutputPremulAlpha);
    v.output_non_alpha_bg = static_cast<uint8_t>(params_.get_int(ParamId::OutputNonAlphaBg));
    v.spout_enabled = params_.get_bool(ParamId::OutputSpoutEnabled);
    v.spout_cpu_fallback = params_.get_bool(ParamId::OutputSpoutCpuFallback);
    v.vcam_enabled = params_.get_bool(ParamId::OutputVcamEnabled);
    v.ndi_enabled = params_.get_bool(ParamId::OutputNdiEnabled);
    v.ndi_format = static_cast<uint8_t>(params_.get_int(ParamId::OutputNdiFormat));
    v.osc_enabled = params_.get_bool(ParamId::OutputOscEnabled);
    v.dsp_user_offset_ms = params_.get_float(ParamId::DspUserOffsetMs);
    v.output_config_dirty = true;
    v.audio_assist_enabled = params_.get_bool(ParamId::TrackingAudioAssist);
    v.mic_gain = params_.get_float(ParamId::TrackingMicGain);
    v.mic_noise_gate_db = params_.get_float(ParamId::TrackingMicNoiseGate);
    v.audio_lipsync_dirty = true;

    if (params_.get_bool(ParamId::RndDebugForceDeviceLost)) {
        params_.set(ParamId::RndDebugForceDeviceLost, 0.0);
        actions.request_force_device_lost = true;
    }

    const int ep_mode = params_.get_int(ParamId::TrackingEpMode);
    if (ep_mode != last_synced_ep_mode_) {
        last_synced_ep_mode_ = ep_mode;
        v.request_ep_override = tracking_ep_mode_to_router_override(ep_mode);
    }
    const bool synth_cam = params_.get_bool(ParamId::TrackingSyntheticCamera);
    if (synth_cam != last_synced_synthetic_cam_) {
        last_synced_synthetic_cam_ = synth_cam;
        v.request_set_synthetic_camera = true;
        v.target_synthetic_camera = synth_cam;
    }
    const bool hand_en = params_.get_bool(ParamId::TrackingHandEnabled);
    if (hand_en != last_synced_hand_tracking_) {
        last_synced_hand_tracking_ = hand_en;
        v.request_set_hand_tracking = true;
        v.target_hand_tracking_enabled = hand_en;
    }
}

void StudioWorkspace::execute_palette_command(const PaletteInvokeResult& res, double now_s, StudioActions& actions) {
    if (!res.invoked) {
        return;
    }
    switch (res.tag) {
    case PaletteCommandTag::ModeLiveStudio:
        switch_mode(StudioMode::LiveStudio);
        break;
    case PaletteCommandTag::ModeRigging:
        switch_mode(StudioMode::Rigging);
        break;
    case PaletteCommandTag::ModeLighting:
        switch_mode(StudioMode::Lighting);
        break;
    case PaletteCommandTag::ModeNodes:
        switch_mode(StudioMode::Nodes);
        break;
    case PaletteCommandTag::ToggleLightingQuadView:
        if (mode_ != StudioMode::Lighting) {
            switch_mode(StudioMode::Lighting);
        }
        lighting_stage_.toggle_quad_view();
        break;
    case PaletteCommandTag::ToggleLightingPilotLight: {
        if (mode_ != StudioMode::Lighting) {
            switch_mode(StudioMode::Lighting);
        }
        lighting_stage_.toggle_pilot_light(actions.viewport, {0.0f, 1.32f, 0.0f}, {0.716f, 1.110f, 1.961f});
        break;
    }
    case PaletteCommandTag::OpenProjectManager:
        if (!host_status_.on_air) {
            actions.request_open_project_manager = true;
        }
        break;
    case PaletteCommandTag::ExportAlfx:
        actions.request_export_alfx_dialog = true;
        break;
    case PaletteCommandTag::ImportAlfx:
        actions.request_import_alfx_dialog = true;
        break;
    case PaletteCommandTag::ProjectNew:
        if (!host_status_.on_air) {
            actions.request_new_project = true;
        }
        break;
    case PaletteCommandTag::ProjectOpen:
        if (!host_status_.on_air) {
            actions.request_open_project_dialog = true;
        }
        break;
    case PaletteCommandTag::ProjectSave:
        actions.request_save_project = true;
        break;
    case PaletteCommandTag::ProjectSaveAs:
        actions.request_save_project_as = true;
        break;
    case PaletteCommandTag::ImportAvatar:
        actions.request_open_avatar_dialog = true;
        break;
    case PaletteCommandTag::OpenRecentFile:
        if (!host_status_.on_air && res.payload[0] != '\0') {
            actions.request_open_project_path = true;
            std::snprintf(actions.project_path_buf.data(), actions.project_path_buf.size(), "%s", res.payload.data());
        }
        break;
    case PaletteCommandTag::AddNodeByType:
        if (mode_ != StudioMode::Nodes) {
            switch_mode(StudioMode::Nodes);
        }
        if (res.payload[0] != '\0') {
            node_canvas_.add_exposed_param_node(res.payload.data());
            actions.pending_graph_json = node_canvas_.serialize_graph_json();
            actions.request_apply_graph_json = true;
        }
        break;
    case PaletteCommandTag::ToggleOnAir:
        actions.request_set_on_air = true;
        actions.target_on_air = !host_status_.on_air;
        break;
    case PaletteCommandTag::ToggleRayTracing:
        undo_.push_param(ParamId::RenderRtEnabled, params_.get(ParamId::RenderRtEnabled),
                         params_.get_bool(ParamId::RenderRtEnabled) ? 0.0 : 1.0);
        params_.set(ParamId::RenderRtEnabled, params_.get_bool(ParamId::RenderRtEnabled) ? 0.0 : 1.0);
        break;
    case PaletteCommandTag::ToggleHeatmap:
        params_.set(ParamId::AppSkeletonHeatmap, params_.get_bool(ParamId::AppSkeletonHeatmap) ? 0.0 : 1.0);
        break;
    case PaletteCommandTag::ToggleOrbitOrb:
        params_.set(ParamId::AppOrbitOrbVisible, params_.get_bool(ParamId::AppOrbitOrbVisible) ? 0.0 : 1.0);
        break;
    case PaletteCommandTag::ToggleTrackingHud:
        params_.set(ParamId::AppTrackingHudVisible, params_.get_bool(ParamId::AppTrackingHudVisible) ? 0.0 : 1.0);
        settings_dirty_ = true;
        break;
    case PaletteCommandTag::ToggleHandTracking: {
        const bool next = !params_.get_bool(ParamId::TrackingHandEnabled);
        params_.set(ParamId::TrackingHandEnabled, next ? 1.0 : 0.0);
        actions.viewport.request_set_hand_tracking = true;
        actions.viewport.target_hand_tracking_enabled = next;
        settings_dirty_ = true;
        break;
    }
    case PaletteCommandTag::ToggleCheckerboard:
        params_.set(ParamId::AppCheckerboardBg, params_.get_bool(ParamId::AppCheckerboardBg) ? 0.0 : 1.0);
        break;
    case PaletteCommandTag::PauseTracking:
        actions.request_pause_tracking = true;
        break;
    case PaletteCommandTag::CalibrateNeutral:
        actions.viewport.request_calibrate_neutral = true;
        break;
    case PaletteCommandTag::Undo: {
        UndoContext ctx{};
        ctx.params = &params_;
        ctx.apply_graph_json = [&](const std::string& json) {
            node_canvas_.set_graph_json(json);
            actions.pending_graph_json = json;
            actions.request_apply_graph_json = true;
        };
        ctx.apply_custom_payload = [&](const std::string& payload) { apply_custom_xform_payload(&params_, payload); };
        if (undo_.undo(ctx)) {
            push_toast(ToastSeverity::Info, "Undo", undo_.redo_label(), now_s);
        }
        break;
    }
    case PaletteCommandTag::Redo: {
        UndoContext ctx{};
        ctx.params = &params_;
        ctx.apply_graph_json = [&](const std::string& json) {
            node_canvas_.set_graph_json(json);
            actions.pending_graph_json = json;
            actions.request_apply_graph_json = true;
        };
        ctx.apply_custom_payload = [&](const std::string& payload) { apply_custom_xform_payload(&params_, payload); };
        if (undo_.redo(ctx)) {
            push_toast(ToastSeverity::Info, "Redo", undo_.undo_label(), now_s);
        }
        break;
    }
    case PaletteCommandTag::ResetLayout:
        reset_current_layout();
        break;
    case PaletteCommandTag::OpenWizard:
        open_hardware_wizard();
        break;
    case PaletteCommandTag::OpenPreferences:
        prefs_modal_.open(0);
        break;
    case PaletteCommandTag::OpenGraphicsSettings:
        graphics_modal_.open(0);
        break;
    case PaletteCommandTag::OpenShortcuts:
        shortcut_overlay_.open();
        break;
    case PaletteCommandTag::ReloadPlugins:
        actions.request_reload_plugins = true;
        break;
    case PaletteCommandTag::ForceDeviceLost:
        if (!host_status_.on_air) {
            actions.request_force_device_lost = true;
        }
        break;
    case PaletteCommandTag::ToggleWsControl:
        params_.set(ParamId::OutputWsEnabled, params_.get_bool(ParamId::OutputWsEnabled) ? 0.0 : 1.0);
        actions.request_toggle_ws = true;
        actions.target_ws_enabled = params_.get_bool(ParamId::OutputWsEnabled);
        break;
    default:
        break;
    }
}

void StudioWorkspace::handle_global_shortcuts(double now_s, const DevShellAvatarView& avatar, StudioActions& actions) {
    if (keymap_.is_pressed(ActionId::OpenPalette)) {
        if (command_palette_.is_open()) {
            command_palette_.close();
        } else {
            command_palette_.rebuild_entries(keymap_, settings_.recent_projects);
            command_palette_.open();
        }
        return;
    }

    if (command_palette_.is_open() || prefs_modal_.is_open() || graphics_modal_.is_open() || wizard_.is_open() ||
        consent_prompt_.open) {
        return;
    }

    const bool typing = ImGui::GetIO().WantTextInput;
    if (keymap_.is_pressed(ActionId::OpenProjectManager) && !host_status_.on_air) {
        actions.request_open_project_manager = true;
    } else if (keymap_.is_pressed(ActionId::ProjectSave)) {
        actions.request_save_project = true;
    } else if (keymap_.is_pressed(ActionId::ProjectSaveAs)) {
        actions.request_save_project_as = true;
    } else if (keymap_.is_pressed(ActionId::ProjectOpen) && !host_status_.on_air) {
        actions.request_open_project_dialog = true;
    } else if (keymap_.is_pressed(ActionId::ProjectNew) && !host_status_.on_air) {
        actions.request_new_project = true;
    } else if (keymap_.is_pressed(ActionId::Undo) && !typing) {
        PaletteInvokeResult r{true, PaletteCommandTag::Undo, {}};
        execute_palette_command(r, now_s, actions);
    } else if ((keymap_.is_pressed(ActionId::Redo) || keymap_.is_pressed(ActionId::RedoAlt)) && !typing) {
        PaletteInvokeResult r{true, PaletteCommandTag::Redo, {}};
        execute_palette_command(r, now_s, actions);
    } else if (keymap_.is_pressed(ActionId::Preferences)) {
        prefs_modal_.open(0);
    } else if (keymap_.is_pressed(ActionId::ShortcutOverlay) && !typing) {
        shortcut_overlay_.open();
    } else if (keymap_.is_pressed(ActionId::ToggleOnAir)) {
        actions.request_set_on_air = true;
        actions.target_on_air = !host_status_.on_air;
    } else if (keymap_.is_pressed(ActionId::PauseTracking) && !typing) {
        actions.request_pause_tracking = true;
    } else if (keymap_.is_pressed(ActionId::CalibrateNeutral)) {
        actions.viewport.request_calibrate_neutral = true;
    } else if (keymap_.is_pressed(ActionId::ModeLiveStudio)) {
        switch_mode(StudioMode::LiveStudio);
    } else if (keymap_.is_pressed(ActionId::ModeRigging)) {
        switch_mode(StudioMode::Rigging);
    } else if (keymap_.is_pressed(ActionId::ModeLighting)) {
        switch_mode(StudioMode::Lighting);
    } else if (keymap_.is_pressed(ActionId::ModeNodes)) {
        switch_mode(StudioMode::Nodes);
    } else if (keymap_.is_pressed(ActionId::LightingQuadView) && !typing) {
        if (mode_ != StudioMode::Lighting) {
            switch_mode(StudioMode::Lighting);
        }
        lighting_stage_.toggle_quad_view();
    } else if (keymap_.is_pressed(ActionId::LightingPilotLight) && !typing) {
        if (mode_ != StudioMode::Lighting) {
            switch_mode(StudioMode::Lighting);
        }
        lighting_stage_.toggle_pilot_light(actions.viewport, {0.0f, 1.32f, 0.0f}, avatar.camera_eye);
    } else if (keymap_.is_pressed(ActionId::MaximizeHoveredPanel) && !typing) {
        if (layout_maximized_) {
            layout_maximized_ = false;
        } else {
            layout_maximized_ = true;
            maximized_window_ = last_hovered_zone_window_;
        }
        dock_layout_dirty_ = true;
    } else if (!typing) {
        // Global toggles (any panel)
        auto toggle_param = [&](ParamId pid) {
            const double old_v = params_.get(pid);
            const double new_v = params_.get_bool(pid) ? 0.0 : 1.0;
            if (params_.set(pid, new_v)) {
                undo_.push_param(pid, old_v, new_v);
            }
        };
        if (keymap_.is_pressed(ActionId::ViewportHeatmap)) {
            toggle_param(ParamId::AppSkeletonHeatmap);
        } else if (keymap_.is_pressed(ActionId::ViewportOrbitOrb)) {
            toggle_param(ParamId::AppOrbitOrbVisible);
        } else if (keymap_.is_pressed(ActionId::ToggleRayTracing)) {
            toggle_param(ParamId::RenderRtEnabled);
        } else if (keymap_.is_pressed(ActionId::OutlinerFilter)) {
            outliner_panel_.set_focus_filter_requested();
        }
        for (size_t i = 0; i < kBuiltinFramingPresetCount; ++i) {
            const auto preset_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::ViewportPreset1) + i);
            if (keymap_.is_pressed(preset_aid)) {
                viewport_panel_.apply_builtin_preset(static_cast<int32_t>(i), avatar, params_, actions);
            }
        }

        // Viewport-scoped keys (Q/W/E/R/X/F/G/1-5): Q/W/E/R/X work whenever Viewport is focused or hovered;
        // F/G/1-5 respect the hovered zone so Node Graph F (Frame Graph) stays conflict-free.
        const bool vp_hovered = (std::strcmp(last_hovered_zone_window_.data(), kPanelViewport) == 0);
        if (viewport_panel_.is_focused() || vp_hovered) {
            if (keymap_.is_pressed(ActionId::GizmoSelect)) {
                viewport_panel_.set_gizmo_tool(GizmoTool::Select);
            } else if (keymap_.is_pressed(ActionId::GizmoMove)) {
                viewport_panel_.set_gizmo_tool(GizmoTool::Move);
            } else if (keymap_.is_pressed(ActionId::GizmoRotate)) {
                viewport_panel_.set_gizmo_tool(GizmoTool::Rotate);
            } else if (keymap_.is_pressed(ActionId::GizmoScale)) {
                viewport_panel_.set_gizmo_tool(GizmoTool::Scale);
            } else if (keymap_.is_pressed(ActionId::GizmoLocalWorld)) {
                viewport_panel_.toggle_local_world();
            }
        }
        if (vp_hovered ||
            (viewport_panel_.is_focused() && std::strcmp(last_hovered_zone_window_.data(), kPanelNodeGraph) != 0)) {
            if (keymap_.is_pressed(ActionId::ViewportGrid)) {
                viewport_panel_.toggle_grid();
            } else if (keymap_.is_pressed(ActionId::ViewportFrameHead) ||
                       keymap_.is_pressed(ActionId::ViewportFrameBody)) {
                viewport_panel_.reset_camera(actions);
            }
            for (size_t i = 0; i < 5; ++i) {
                const auto cam_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::ViewportCam1) + i);
                const auto store_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::ViewportStoreCam1) + i);
                if (keymap_.is_pressed(store_aid)) {
                    viewport_panel_.store_camera_bookmark(i);
                    if (state_store_ != nullptr) {
                        state_store_->mark_dirty(aether::DirtyDomain::CameraState);
                    }
                } else if (keymap_.is_pressed(cam_aid)) {
                    viewport_panel_.recall_camera_bookmark(i, actions);
                    if (state_store_ != nullptr) {
                        state_store_->mark_dirty(aether::DirtyDomain::CameraState);
                    }
                }
            }
        }

        // F1-F8 Quick Expressions: latch / release; weights ramp in update_expression_ramps()
        for (size_t i = 0; i < StudioActions::kQuickExpressionSlots && i < expressions_.size(); ++i) {
            const auto expr_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::Expression1) + i);
            if (keymap_.is_pressed(expr_aid)) {
                expressions_[i].triggered_override = !expressions_[i].triggered_override;
            }
        }
    }
}

void StudioWorkspace::update_expression_ramps(double now_s, StudioActions& actions) noexcept {
    const double dt = std::clamp(now_s - last_ramp_s_, 0.0, 0.1);
    last_ramp_s_ = now_s;
    const bool instant = params_.get_bool(ParamId::AppReduceMotion);
    // "Reset Neutral" is momentary: pressing it releases every latched expression (they ramp out smoothly).
    constexpr size_t kReset = StudioActions::kResetNeutralQuickSlot;
    if (kReset < expressions_.size() && expressions_[kReset].triggered_override) {
        for (auto& ex : expressions_) {
            ex.triggered_override = false;
        }
    }
    for (size_t i = 0; i < StudioActions::kQuickExpressionSlots && i < expressions_.size(); ++i) {
        ExpressionSlotInfo& ex = expressions_[i];
        const float target = ex.triggered_override ? 1.0f : 0.0f;
        float& u = expr_ramp_u_[i];
        if (u == target) {
            continue;
        }
        const float step = instant ? 1.0f : static_cast<float>(dt / kExpressionRampSeconds);
        u = target > u ? std::min(target, u + step) : std::max(target, u - step);
        // Cubic Hermite (smoothstep) ease: zero velocity at both ends of the blend.
        const float w = u * u * (3.0f - 2.0f * u);
        ex.current_weight = w;
        actions.expression_weights[i] = w;
        actions.expression_dirty_mask = static_cast<uint8_t>(actions.expression_dirty_mask | (1u << i));
    }
}

void StudioWorkspace::ensure_broadcast_outputs_enabled(double now_s) {
    // Go Live = master broadcast transmission: Spout2 + Virtual Cam must be transmitting.
    bool changed = false;
    for (const ParamId pid : {ParamId::OutputSpoutEnabled, ParamId::OutputVcamEnabled}) {
        if (!params_.get_bool(pid)) {
            const double old_v = params_.get(pid);
            if (params_.set(pid, 1.0)) {
                undo_.push_param(pid, old_v, 1.0);
                changed = true;
            }
        }
    }
    if (changed) {
        push_toast(ToastSeverity::Info, "Broadcast Outputs Enabled", "Spout2 and Virtual Cam started for Go Live.",
                   now_s);
    }
}

void StudioWorkspace::render(double now_s, const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry,
                             const DevShellInputEcho& input_echo, const ImGuiArenaAllocator& arena, bool ffi_layout_ok,
                             const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                             const DevShellRenderView& render_view, const DevShellOutputView& output_view,
                             StudioActions& actions) {
    last_now_s_ = now_s;
    drain_logs();

    const float scale = std::max(0.75f, effective_dpi_scale());

    // Push telemetry into Performance Timeline at live cadence (unless paused)
    timeline_console_.push_sample(telemetry, render_view, tracking, params_.get_bool(ParamId::AppTelemetryPaused));

    // Sync any newly parsed avatar bone count into skeleton tree if empty cleanly
    if (!demo_bones_seeded_ && avatar.has_avatar && bones_.empty() && avatar.joint_count > 0) {
        demo_bones_seeded_ = true;
        const uint32_t count = std::min<uint32_t>(avatar.joint_count, 180U);
        for (uint32_t i = 0; i < count; ++i) {
            OutlinerBoneInfo b{};
            b.bone_id = i;
            std::snprintf(b.name.data(), b.name.size(), "Joint_%03u", i);
            b.humanoid_mapped = (i < avatar.mapped_humanoid_bones);
            b.confidence = (i % 5 == 0) ? 0.45f : 0.96f;
            bones_.push_back(b);
        }
    }

    handle_global_shortcuts(now_s, avatar, actions);

    // Z5 Top Menu & Mode Bar
    bool mode_changed = false;
    StudioMode requested_mode = mode_;
    const TopMenuAction top_act =
        telemetry_bar_.draw_top_bar(now_s, requested_mode, mode_changed, rhi, ffi_layout_ok, host_status_, undo_, hw_,
                                    tracking, params_, settings_, settings_dirty_, fonts_, scale, actions);
    if (mode_changed) {
        switch_mode(requested_mode);
    }
    switch (top_act) {
    case TopMenuAction::OpenPalette:
        command_palette_.rebuild_entries(keymap_, settings_.recent_projects);
        command_palette_.open();
        break;
    case TopMenuAction::OpenWizard:
        open_hardware_wizard();
        break;
    case TopMenuAction::OpenPreferences:
        prefs_modal_.open(0);
        break;
    case TopMenuAction::OpenGraphicsSettings:
        graphics_modal_.open(0);
        break;
    case TopMenuAction::OpenShortcuts:
        shortcut_overlay_.open();
        break;
    case TopMenuAction::OpenProjectManager:
        if (!host_status_.on_air) {
            actions.request_open_project_manager = true;
        }
        break;
    case TopMenuAction::ExportAlfx:
        actions.request_export_alfx_dialog = true;
        break;
    case TopMenuAction::ImportAlfx:
        actions.request_import_alfx_dialog = true;
        break;
    case TopMenuAction::NewProject:
        if (!host_status_.on_air) {
            actions.request_new_project = true;
        }
        break;
    case TopMenuAction::OpenProject:
        if (!host_status_.on_air) {
            actions.request_open_project_dialog = true;
        }
        break;
    case TopMenuAction::SaveProject:
        actions.request_save_project = true;
        break;
    case TopMenuAction::SaveProjectAs:
        actions.request_save_project_as = true;
        break;
    case TopMenuAction::ImportAvatar:
        actions.request_open_avatar_dialog = true;
        break;
    case TopMenuAction::ResetLayout:
        reset_current_layout();
        break;
    case TopMenuAction::Undo: {
        PaletteInvokeResult r{true, PaletteCommandTag::Undo, {}};
        execute_palette_command(r, now_s, actions);
        break;
    }
    case TopMenuAction::Redo: {
        PaletteInvokeResult r{true, PaletteCommandTag::Redo, {}};
        execute_palette_command(r, now_s, actions);
        break;
    }
    default:
        break;
    }

    // Main Dockspace inside viewport (leaving space cleanly for Top Menu + Bottom Status Strip)
    const PrecisionDarkTokens& t = tokens();
    const ImGuiViewport* main_vp = ImGui::GetMainViewport();
    const float top_h = std::max(ImGui::GetFrameHeight(), main_vp->WorkPos.y - main_vp->Pos.y);
    const float bot_h = std::round(t.status_bar_h_px * scale);
    const ImVec2 dock_pos(main_vp->Pos.x, main_vp->Pos.y + top_h);
    const ImVec2 dock_sz(main_vp->Size.x, std::max(120.0f, main_vp->Size.y - top_h - bot_h));

    ImGui::SetNextWindowPos(dock_pos);
    ImGui::SetNextWindowSize(dock_sz);
    ImGui::SetNextWindowViewport(main_vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                            ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##StudioDockHost", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace_id = ImGui::GetID("##AetherDockSpace");
    const bool host_resized =
        (std::fabs(dock_sz.x - last_dock_host_w_) > 1.5f || std::fabs(dock_sz.y - last_dock_host_h_) > 1.5f);
    if (host_resized) {
        dock_resize_check_frames_ = 2u;
    }
    if (!dock_layout_dirty_ && !layout_maximized_ && dock_resize_check_frames_ > 0u) {
        if (const ImGuiDockNode* cn = ImGui::DockBuilderGetCentralNode(dockspace_id); cn != nullptr) {
            const bool starved_x = (cn->Size.x > 0.0f && cn->Size.x < 120.0f && dock_sz.x >= 320.0f);
            const bool starved_y = (cn->Size.y > 0.0f && cn->Size.y < 110.0f && dock_sz.y >= 240.0f);
            if (starved_x || starved_y) {
                dock_layout_dirty_ = true;
                dock_resize_check_frames_ = 0u;
            } else {
                --dock_resize_check_frames_;
            }
        } else {
            --dock_resize_check_frames_;
        }
    }
    // Minimum-size guard (checked when no splitter drag is in progress): Viewport >= 320x200, Outliner >= 200 px,
    // Inspector >= 260 px. A collapsed zone rebuilds the mode layout instead of producing a zero / NaN canvas.
    if (!dock_layout_dirty_ && !layout_maximized_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        dock_sz.x >= 1000.0f && dock_sz.y >= 360.0f) {
        auto node_of = [](const char* win) -> const ImGuiDockNode* {
            const ImGuiWindow* w = ImGui::FindWindowByName(win);
            return (w != nullptr && w->DockIsActive) ? w->DockNode : nullptr;
        };
        const ImGuiDockNode* n_out = node_of(kPanelOutliner);
        const ImGuiDockNode* n_ins = node_of(kPanelInspector);
        const ImGuiDockNode* n_vp = node_of(kPanelViewport);
        const bool out_small = n_out != nullptr && n_out->Size.x > 0.0f && n_out->Size.x < 200.0f * 0.75f;
        const bool ins_small = n_ins != nullptr && n_ins->Size.x > 0.0f && n_ins->Size.x < 260.0f * 0.75f;
        const bool vp_small =
            n_vp != nullptr && n_vp->Size.x > 0.0f && (n_vp->Size.x < 320.0f || n_vp->Size.y < 200.0f);
        // One rebuild per host size: never loop if the default layout itself cannot satisfy the minimums.
        const bool already_tried =
            std::fabs(min_guard_rebuild_w_ - dock_sz.x) < 1.0f && std::fabs(min_guard_rebuild_h_ - dock_sz.y) < 1.0f;
        if ((out_small || ins_small || vp_small) && !already_tried) {
            min_guard_rebuild_w_ = dock_sz.x;
            min_guard_rebuild_h_ = dock_sz.y;
            dock_layout_dirty_ = true;
        }
    }
    if (dock_layout_dirty_) {
        build_dock_layout(dockspace_id, mode_);
        dock_resize_check_frames_ = 0u;
    }
    last_dock_host_w_ = dock_sz.x;
    last_dock_host_h_ = dock_sz.y;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    auto track_hovered_zone = [&](const char* win_name) {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            safe_copy_cstr(last_hovered_zone_window_.data(), last_hovered_zone_window_.size(), win_name);
        }
    };

    // Z1 Outliner
    if (mode_ == StudioMode::Lighting) {
        if (ImGui::Begin(kPanelOutliner)) {
            track_hovered_zone(kPanelOutliner);
            lighting_stage_.draw_outliner(avatar, fonts_, selection_, actions);
        }
        ImGui::End();
    } else {
        outliner_panel_.draw(mode_, host_status_.on_air, avatar, output_view, bones_, materials_, expressions_,
                             plugins_, params_, undo_, fonts_, selection_, actions);
        if (outliner_panel_.consume_inspect_outputs_request()) {
            inspector_panel_.request_focus_output_section();
        }
    }

    // Z2 Viewport (Broadcast Viewport vs Isolated Lighting Stage Viewport)
    if (mode_ == StudioMode::Lighting) {
        // NoScrollbar | NoScrollWithMouse: the stage canvas fills the remaining content region exactly, so the
        // window must never scroll (a wheel-zoom used to scroll the panel and push the canvas out of view).
        if (ImGui::Begin(kPanelViewport, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            track_hovered_zone(kPanelViewport);
            lighting_stage_.draw_stage_viewport(avatar, fonts_, scale, selection_, actions);
        }
        ImGui::End();
    } else {
        viewport_panel_.draw(mode_, hw_, avatar, tracking, render_view, input_echo, host_status_.tracking_paused,
                             params_, undo_, settings_, settings_dirty_, fonts_, scale, actions,
                             host_status_.active_alfx_name.data());
    }

    // Z3 Inspector
    PropertyRowActions row_actions{};
    if (mode_ == StudioMode::Lighting) {
        if (ImGui::Begin(kPanelInspector)) {
            track_hovered_zone(kPanelInspector);
            lighting_stage_.draw_inspector(avatar, fonts_, params_, selection_, actions);
        }
        ImGui::End();
    } else {
        inspector_panel_.draw(mode_, host_status_.on_air, hw_, rhi, ffi_layout_ok, avatar, tracking, render_view,
                              output_view, host_status_, plugins_, selection_, params_, undo_, settings_,
                              settings_dirty_, fonts_, row_actions, actions, &viewport_panel_, bones_, materials_,
                              &node_canvas_);
    }
    if (const ImGuiWindow* hov = GImGui != nullptr ? GImGui->HoveredWindow : nullptr;
        hov != nullptr && hov->RootWindow != nullptr) {
        const char* rname = hov->RootWindow->Name;
        for (const char* zname : {kPanelOutliner, kPanelViewport, kPanelInspector, kPanelNodeGraph, kPanelConsole,
                                  kPanelLightMixer, kPanelExpressions, kPanelTimeline}) {
            if (std::strcmp(rname, zname) == 0) {
                safe_copy_cstr(last_hovered_zone_window_.data(), last_hovered_zone_window_.size(), zname);
                break;
            }
        }
    }
    if (row_actions.expose_graph_param_id != nullptr) {
        node_canvas_.add_exposed_param_node(row_actions.expose_graph_param_id);
        actions.pending_graph_json = node_canvas_.serialize_graph_json();
        actions.request_apply_graph_json = true;
        switch_mode(StudioMode::Nodes);
        push_toast(ToastSeverity::Success, "Exposed in Graph", row_actions.expose_graph_param_id, now_s);
    }
    if (row_actions.copied_feedback_msg != nullptr) {
        push_toast(ToastSeverity::Info, "Clipboard", row_actions.copied_feedback_msg, now_s);
    }

    // Z4 Node Graph
    if (ImGui::Begin(kPanelNodeGraph)) {
        track_hovered_zone(kPanelNodeGraph);
        node_canvas_.draw(host_status_, undo_, keymap_, fonts_, scale, selection_, actions);
    }
    ImGui::End();

    // Z4 Console, Light Mixer & Performance Tabs
    if (mode_ == StudioMode::Lighting) {
        if (ImGui::Begin(kPanelLightMixer)) {
            track_hovered_zone(kPanelLightMixer);
            lighting_stage_.draw_mixer_console(avatar, fonts_, selection_, actions);
        }
        ImGui::End();
        lighting_stage_.export_stage_actions(avatar, actions.viewport);
    } else {
        actions.viewport.request_stage_render = false;
    }

    if (ImGui::Begin(kPanelConsole)) {
        track_hovered_zone(kPanelConsole);
        const ConsoleTabResult res = timeline_console_.draw_console_tab(recent_logs_, log_head_, log_count_, fonts_);
        if (res.copied_notice != nullptr) {
            push_toast(ToastSeverity::Info, "Console", res.copied_notice, now_s);
        }
        if (res.clear_requested) {
            log_head_ = 0;
            log_count_ = 0;
        }
    }
    ImGui::End();

    if (ImGui::Begin(kPanelExpressions)) {
        track_hovered_zone(kPanelExpressions);
        timeline_console_.draw_expressions_tab(expressions_, tracking, fonts_);
    }
    ImGui::End();

    if (ImGui::Begin(kPanelTimeline)) {
        track_hovered_zone(kPanelTimeline);
        timeline_console_.draw_timeline_tab(telemetry, render_view, tracking, params_, fonts_);
    }
    ImGui::End();

    if (focus_tab_after_layout_[0] != '\0') {
        ImGui::SetWindowFocus(focus_tab_after_layout_.data());
        focus_tab_after_layout_[0] = '\0';
    }

    // Quick-expression latches from F-keys, the Outliner grid or the Expressions tab ramp smoothly.
    update_expression_ramps(now_s, actions);

    // Z5 Bottom Status Strip (4 Hz sampling + 500 ms color hysteresis)
    telemetry_bar_.draw_bottom_bar(now_s, telemetry, arena, rhi, tracking, render_view, output_view, host_status_,
                                   fonts_, scale, actions);

    // Modals & Overlays
    const PaletteInvokeResult pal_res = command_palette_.draw(keymap_, host_status_.on_air, fonts_, scale);
    execute_palette_command(pal_res, now_s, actions);

    PropertyRowActions pref_row_actions{};
    prefs_modal_.draw(host_status_.on_air, params_, undo_, keymap_, settings_, fonts_, scale, pref_row_actions, actions,
                      settings_dirty_);

    if (actions.request_open_graphics_settings) {
        actions.request_open_graphics_settings = false;
        graphics_modal_.open(0);
    }
    PropertyRowActions gfx_row_actions{};
    graphics_modal_.draw(host_status_.on_air, rhi, render_view, params_, undo_, settings_, fonts_, scale,
                         gfx_row_actions, actions, settings_dirty_);

    shortcut_overlay_.draw(keymap_, fonts_, scale);

    if (wizard_.draw(host_status_.on_air, hw_, rhi, telemetry, tracking, render_view, params_, settings_, fonts_, scale,
                     actions)) {
        settings_dirty_ = true;
        push_toast(ToastSeverity::Success, "Hardware Profile Applied", "Optimal EP and quality profile active.", now_s);
    }

    const ConsentDecision decision = draw_plugin_consent_modal(consent_prompt_, fonts_);
    if (decision != ConsentDecision::Pending) {
        actions.request_plugin_consent_reply = true;
        actions.plugin_consent_granted = (decision == ConsentDecision::TrustAndEnable);
        actions.plugin_consent_id = consent_prompt_.plugin_id;
        push_toast(decision == ConsentDecision::TrustAndEnable ? ToastSeverity::Success : ToastSeverity::Info,
                   decision == ConsentDecision::TrustAndEnable ? "Plugin Enabled" : "Plugin Denied",
                   consent_prompt_.plugin_name.data(), now_s);
    }

    // Non-blocking bottom-right toast notifications (§12.13)
    const ToastActionKind toast_act = toasts_.draw(now_s, fonts_, scale, params_.get_bool(ParamId::AppReduceMotion));
    if (toast_act == ToastActionKind::Undo) {
        PaletteInvokeResult r{true, PaletteCommandTag::Undo, {}};
        execute_palette_command(r, now_s, actions);
    } else if (toast_act == ToastActionKind::OpenWizard) {
        open_hardware_wizard();
    }

    // Reapply Precision Dark style colors when Accessibility High-Contrast changed (font rebuild deferred to T3 between
    // frames)
    if (is_high_contrast_theme() != params_.get_bool(ParamId::AppHighContrast)) {
        const bool hc = params_.get_bool(ParamId::AppHighContrast);
        set_high_contrast_theme(hc);
        apply_precision_dark_style(ImGui::GetStyle(), effective_dpi_scale(), hc);
        actions.request_reapply_theme_or_dpi = true;
    }

    // Mode transition requests triggered by Inspector cards or panels.
    if (actions.request_switch_mode) {
        switch_mode(actions.target_mode);
        actions.request_switch_mode = false;
    }

    // Track project-scoped DirtyDomain changes cleanly on StudioStateStore.
    if (state_store_ != nullptr) {
        if (dirty_param_epoch_ != params_.epoch()) {
            for (size_t i = 0; i < kParamCount; ++i) {
                const double cur = params_.get(static_cast<ParamId>(i));
                if (cur != dirty_param_snapshot_[i]) {
                    dirty_param_snapshot_[i] = cur;
                    const aether::DirtyDomain dd = dirty_domain_for_param(kParamSpecs[i].id);
                    if (dd != aether::DirtyDomain::None) {
                        state_store_->mark_dirty(dd);
                    }
                }
            }
            dirty_param_epoch_ = params_.epoch();
        }
        if (actions.viewport.lighting_dirty) {
            state_store_->mark_dirty(aether::DirtyDomain::LightingRig);
        }
        if (actions.request_apply_graph_json) {
            state_store_->mark_dirty(aether::DirtyDomain::NodeGraph);
        }
        if (actions.viewport.request_apply_camera_preset) {
            state_store_->mark_dirty(aether::DirtyDomain::CameraState);
        }
        if (actions.viewport.output_config_dirty) {
            state_store_->mark_dirty(aether::DirtyDomain::OutputsCfg);
        }
    } else if (dirty_param_epoch_ != params_.epoch()) {
        dirty_param_epoch_ = params_.epoch();
    }

    // Go Live (top bar, Ctrl+Shift+L or palette) starts the Spout2 + Virtual Cam transmitters.
    if (actions.request_set_on_air && actions.target_on_air && !host_status_.on_air) {
        ensure_broadcast_outputs_enabled(now_s);
    }

    // Propagate ParamStore epochs cleanly into Render / Tracking / Output actions same frame
    sync_params_to_actions(actions);

    if (settings_dirty_) {
        save_app_config_files();
    }
}

} // namespace aether::ui
