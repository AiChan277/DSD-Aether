#include "ui/workspace/studio_workspace.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace aether::ui {
namespace {

const char* kPanelOutliner = "Outliner & Project";
const char* kPanelViewport = "Viewport";
const char* kPanelInspector = "Inspector";
const char* kPanelNodeGraph = "Node Graph";
const char* kPanelConsole = "Console";
const char* kPanelExpressions = "Expressions";
const char* kPanelTimeline = "Performance Timeline";

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
                           const char* initial_avatar_path, bool allow_disk_config) {
    paths_ = paths;
    hw_ = hw;
    os_dpi_scale_ = std::max(0.75f, dpi_scale);
    allow_disk_config_ = allow_disk_config;
    config_dir_ = paths_.app_data_dir.empty() ? default_config_dir() : std::filesystem::path(paths_.app_data_dir);

    std::string ref_vrm = resolve_asset_path(paths_, "samples/reference.vrm");
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
        (void)load_app_settings_toml(config_dir_ / "settings.toml", settings_, params_);
        (void)keymap_.load_toml(config_dir_ / "keymap.toml");

        // Open Hardware Optimizer Wizard cleanly on first launch or new hardware fingerprint
        const uint64_t inv_hash = HardwareOptimizerWizard::compute_inventory_hash(hw_);
        if (!settings_.wizard_completed ||
            (settings_.hw_inventory_hash != 0 && settings_.hw_inventory_hash != inv_hash)) {
            if (!params_.get_bool(ParamId::TrackingSyntheticCamera)) {
                // Only auto-open interactively when settings indicate desktop run
            }
            wizard_.open(hw_, rhi::SelectedGpuInfo{}, host_status_.on_air);
        }
    }

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
    (void)save_app_settings_toml(config_dir_ / "settings.toml", settings_, params_);
    (void)keymap_.save_toml(config_dir_ / "keymap.toml");
    settings_dirty_ = false;
}

void StudioWorkspace::save_mode_layout_ini(StudioMode mode) const {
    if (!allow_disk_config_ || ImGui::GetCurrentContext() == nullptr || layout_maximized_) {
        return;
    }
    std::array<char, 32> fname{};
    std::snprintf(fname.data(), fname.size(), "mode_%d.ini", static_cast<int>(mode));
    const auto p = config_dir_ / "layouts" / fname.data();
    const std::string p_str = p.string();
    ImGui::SaveIniSettingsToDisk(p_str.c_str());
}

void StudioWorkspace::load_mode_layout_ini(StudioMode mode) {
    if (!allow_disk_config_ || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    std::array<char, 32> fname{};
    std::snprintf(fname.data(), fname.size(), "mode_%d.ini", static_cast<int>(mode));
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

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 avail_size = vp != nullptr ? vp->WorkSize : ImVec2(1440.0f, 900.0f);
    ImGui::DockBuilderSetNodeSize(dockspace_id, avail_size);

    ImGuiID main_id = dockspace_id;
    if (layout_maximized_ && maximized_window_[0] != '\0') {
        ImGui::DockBuilderDockWindow(maximized_window_.data(), main_id);
        ImGui::DockBuilderFinish(dockspace_id);
        return;
    }

    const float left_frac = (mode == StudioMode::Rigging) ? 0.21f : (mode == StudioMode::Nodes ? 0.17f : 0.19f);
    const float right_frac = (mode == StudioMode::Rigging) ? 0.25f : 0.23f;
    const float bottom_frac = (mode == StudioMode::Nodes) ? 0.22f : 0.25f;

    ImGuiID left_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left, left_frac, nullptr, &main_id);
    ImGuiID right_id =
        ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, right_frac / (1.0f - left_frac), nullptr, &main_id);
    ImGuiID bottom_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Down, bottom_frac, nullptr, &main_id);

    ImGui::DockBuilderDockWindow(kPanelOutliner, left_id);
    ImGui::DockBuilderDockWindow(kPanelInspector, right_id);

    if (mode == StudioMode::Nodes) {
        ImGuiID preview_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left, 0.36f, nullptr, &main_id);
        ImGui::DockBuilderDockWindow(kPanelViewport, preview_id);
        ImGui::DockBuilderDockWindow(kPanelNodeGraph, main_id);
        ImGui::DockBuilderDockWindow(kPanelConsole, bottom_id);
        ImGui::DockBuilderDockWindow(kPanelTimeline, bottom_id);
        ImGui::DockBuilderDockWindow(kPanelExpressions, bottom_id);
    } else {
        ImGui::DockBuilderDockWindow(kPanelViewport, main_id);
        ImGui::DockBuilderDockWindow(kPanelConsole, bottom_id);
        ImGui::DockBuilderDockWindow(kPanelExpressions, bottom_id);
        ImGui::DockBuilderDockWindow(kPanelTimeline, bottom_id);
        ImGui::DockBuilderDockWindow(kPanelNodeGraph, bottom_id);
    }

    ImGui::DockBuilderFinish(dockspace_id);
}

void StudioWorkspace::sync_params_to_actions(StudioActions& actions) noexcept {
    if (synced_param_epoch_ == params_.epoch()) {
        return;
    }
    synced_param_epoch_ = params_.epoch();

    auto& v = actions.viewport;
    v.live2d_flatness = params_.get_float(ParamId::RenderFlatness);
    v.key_azimuth_deg = params_.get_float(ParamId::RenderKeyAzimuthDeg);
    v.key_elevation_deg = params_.get_float(ParamId::RenderKeyElevationDeg);
    v.key_intensity = params_.get_float(ParamId::RenderKeyIntensity);
    v.fill_intensity = params_.get_float(ParamId::RenderFillIntensity);
    v.rim_intensity = params_.get_float(ParamId::RenderRimIntensity);
    v.outline_scale = params_.get_float(ParamId::RenderOutlineScale);
    v.bloom_intensity = params_.get_float(ParamId::RenderBloomIntensity);
    v.rt_enabled = params_.get_bool(ParamId::RenderRtEnabled);
    v.debug_view = static_cast<uint8_t>(params_.get_int(ParamId::RenderDebugView));
    v.denoiser_kind = static_cast<uint8_t>(params_.get_int(ParamId::RenderDenoiser));
    v.render_backend = static_cast<uint8_t>(params_.get_int(ParamId::RenderBackend));
    v.show_skeleton_heatmap = params_.get_bool(ParamId::AppSkeletonHeatmap);
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

    if (params_.get_bool(ParamId::RndDebugForceDeviceLost)) {
        params_.set(ParamId::RndDebugForceDeviceLost, 0.0);
        actions.request_force_device_lost = true;
    }

    const int ep_mode = params_.get_int(ParamId::TrackingEpMode);
    if (ep_mode == 0) {
        v.request_ep_override = -1;
    } else if (ep_mode >= 1 && ep_mode <= 4) {
        v.request_ep_override = ep_mode - 1;
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
    case PaletteCommandTag::ModeNodes:
        switch_mode(StudioMode::Nodes);
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

void StudioWorkspace::handle_global_shortcuts(double now_s, StudioActions& actions) {
    if (keymap_.is_pressed(ActionId::OpenPalette)) {
        if (command_palette_.is_open()) {
            command_palette_.close();
        } else {
            command_palette_.rebuild_entries(keymap_, settings_.recent_projects);
            command_palette_.open();
        }
        return;
    }

    if (command_palette_.is_open() || prefs_modal_.is_open() || wizard_.is_open() || consent_prompt_.open) {
        return;
    }

    const bool typing = ImGui::GetIO().WantTextInput;
    if (keymap_.is_pressed(ActionId::ProjectSave)) {
        actions.request_save_project = true;
    } else if (keymap_.is_pressed(ActionId::ProjectSaveAs)) {
        actions.request_save_project_as = true;
    } else if (keymap_.is_pressed(ActionId::ProjectOpen) && !host_status_.on_air) {
        actions.request_open_project_dialog = true;
    } else if (keymap_.is_pressed(ActionId::ProjectNew) && !host_status_.on_air) {
        actions.request_new_project = true;
    } else if (keymap_.is_pressed(ActionId::Undo)) {
        PaletteInvokeResult r{true, PaletteCommandTag::Undo, {}};
        execute_palette_command(r, now_s, actions);
    } else if (keymap_.is_pressed(ActionId::Redo)) {
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
    } else if (keymap_.is_pressed(ActionId::ModeNodes)) {
        switch_mode(StudioMode::Nodes);
    } else if (keymap_.is_pressed(ActionId::MaximizeHoveredPanel) && !typing) {
        if (layout_maximized_) {
            layout_maximized_ = false;
        } else {
            layout_maximized_ = true;
            maximized_window_ = last_hovered_zone_window_;
        }
        dock_layout_dirty_ = true;
    } else if (!typing) {
        if (keymap_.is_pressed(ActionId::ViewportHeatmap)) {
            params_.set(ParamId::AppSkeletonHeatmap, params_.get_bool(ParamId::AppSkeletonHeatmap) ? 0.0 : 1.0);
        } else if (keymap_.is_pressed(ActionId::ViewportOrbitOrb)) {
            params_.set(ParamId::AppOrbitOrbVisible, params_.get_bool(ParamId::AppOrbitOrbVisible) ? 0.0 : 1.0);
        } else if (keymap_.is_pressed(ActionId::ToggleRayTracing)) {
            params_.set(ParamId::RenderRtEnabled, params_.get_bool(ParamId::RenderRtEnabled) ? 0.0 : 1.0);
        } else if (keymap_.is_pressed(ActionId::OutlinerFilter)) {
            outliner_panel_.set_focus_filter_requested();
        } else if (keymap_.is_pressed(ActionId::GizmoSelect)) {
            viewport_panel_.set_gizmo_tool(GizmoTool::Select);
        } else if (keymap_.is_pressed(ActionId::GizmoMove)) {
            viewport_panel_.set_gizmo_tool(GizmoTool::Move);
        } else if (keymap_.is_pressed(ActionId::GizmoRotate)) {
            viewport_panel_.set_gizmo_tool(GizmoTool::Rotate);
        } else if (keymap_.is_pressed(ActionId::GizmoScale)) {
            viewport_panel_.set_gizmo_tool(GizmoTool::Scale);
        } else if (keymap_.is_pressed(ActionId::GizmoLocalWorld)) {
            viewport_panel_.toggle_local_world();
        } else if (keymap_.is_pressed(ActionId::ViewportGrid)) {
            viewport_panel_.toggle_grid();
        } else if (keymap_.is_pressed(ActionId::ViewportFrameHead) || keymap_.is_pressed(ActionId::ViewportFrameBody)) {
            actions.viewport.reset_camera = true;
        }

        // Camera bookmarks 1-5 & Ctrl+Alt+1-5
        for (size_t i = 0; i < 5; ++i) {
            const auto cam_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::ViewportCam1) + i);
            const auto store_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::ViewportStoreCam1) + i);
            if (keymap_.is_pressed(store_aid)) {
                viewport_panel_.store_camera_bookmark(i);
            } else if (keymap_.is_pressed(cam_aid)) {
                viewport_panel_.recall_camera_bookmark(i, actions);
            }
        }

        // F1-F8 Quick Expressions
        for (size_t i = 0; i < 8 && i < expressions_.size(); ++i) {
            const auto expr_aid = static_cast<ActionId>(static_cast<size_t>(ActionId::Expression1) + i);
            if (keymap_.is_pressed(expr_aid)) {
                auto& ex = expressions_[i];
                ex.triggered_override = !ex.triggered_override;
                ex.current_weight = ex.triggered_override ? 1.0f : 0.0f;
                actions.request_trigger_expression = true;
                actions.expression_slot = static_cast<int32_t>(i);
                actions.expression_weight = ex.current_weight;
            }
        }
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

    handle_global_shortcuts(now_s, actions);

    // Z5 Top Menu & Mode Bar
    bool mode_changed = false;
    StudioMode requested_mode = mode_;
    const TopMenuAction top_act = telemetry_bar_.draw_top_bar(requested_mode, mode_changed, rhi, ffi_layout_ok,
                                                              host_status_, undo_, fonts_, scale, actions);
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
    case TopMenuAction::OpenShortcuts:
        shortcut_overlay_.open();
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
    default:
        break;
    }

    // Main Dockspace inside viewport (leaving space cleanly for Top Menu + Bottom Status Strip)
    const PrecisionDarkTokens& t = tokens();
    const ImGuiViewport* main_vp = ImGui::GetMainViewport();
    const float top_h = ImGui::GetFrameHeight();
    const float bot_h = t.status_bar_h_px * scale;
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
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    if (dock_layout_dirty_) {
        build_dock_layout(dockspace_id, mode_);
    }
    ImGui::End();

    auto track_hovered_zone = [&](const char* win_name) {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            safe_copy_cstr(last_hovered_zone_window_.data(), last_hovered_zone_window_.size(), win_name);
        }
    };

    // Z1 Outliner (OutlinerPanel manages its own ImGui::Begin("Outliner & Project")/End)
    outliner_panel_.draw(mode_, host_status_.on_air, avatar, output_view, bones_, materials_, expressions_, plugins_,
                         params_, undo_, fonts_, selection_, actions);

    // Z2 Viewport (ViewportPanel manages its own ImGui::Begin("Viewport")/End)
    viewport_panel_.draw(avatar, tracking, render_view, input_echo, host_status_.tracking_paused, params_, undo_,
                         fonts_, scale, actions);

    // Z3 Inspector (InspectorPanel manages its own ImGui::Begin("Inspector")/End)
    PropertyRowActions row_actions{};
    inspector_panel_.draw(mode_, host_status_.on_air, hw_, rhi, ffi_layout_ok, avatar, tracking, render_view,
                          output_view, host_status_, plugins_, selection_, params_, undo_, fonts_, row_actions,
                          actions);
    if (const ImGuiWindow* hov = GImGui != nullptr ? GImGui->HoveredWindow : nullptr;
        hov != nullptr && hov->RootWindow != nullptr) {
        const char* rname = hov->RootWindow->Name;
        for (const char* zname : {kPanelOutliner, kPanelViewport, kPanelInspector, kPanelNodeGraph, kPanelConsole,
                                  kPanelExpressions, kPanelTimeline}) {
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
        node_canvas_.draw(host_status_, undo_, fonts_, scale, selection_, actions);
    }
    ImGui::End();

    // Z4 Auxiliary Tabs (Console, Expressions, Timeline)
    if (ImGui::Begin(kPanelConsole)) {
        track_hovered_zone(kPanelConsole);
        const char* copied = nullptr;
        timeline_console_.draw_console_tab(recent_logs_, log_head_, log_count_, fonts_, copied);
        if (copied != nullptr) {
            push_toast(ToastSeverity::Info, "Log Copied", copied, now_s);
        }
    }
    ImGui::End();

    if (ImGui::Begin(kPanelExpressions)) {
        track_hovered_zone(kPanelExpressions);
        timeline_console_.draw_expressions_tab(expressions_, fonts_, actions);
    }
    ImGui::End();

    if (ImGui::Begin(kPanelTimeline)) {
        track_hovered_zone(kPanelTimeline);
        timeline_console_.draw_timeline_tab(telemetry, render_view, tracking, params_, fonts_);
    }
    ImGui::End();

    // Z5 Bottom Status Strip (4 Hz sampling + 500 ms color hysteresis)
    telemetry_bar_.draw_bottom_bar(now_s, telemetry, arena, rhi, tracking, render_view, output_view, fonts_, scale);

    // Modals & Overlays
    const PaletteInvokeResult pal_res = command_palette_.draw(keymap_, host_status_.on_air, fonts_, scale);
    execute_palette_command(pal_res, now_s, actions);

    PropertyRowActions pref_row_actions{};
    prefs_modal_.draw(host_status_.on_air, params_, undo_, keymap_, settings_, fonts_, scale, pref_row_actions, actions,
                      settings_dirty_);

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

    // Propagate ParamStore epochs cleanly into Render / Tracking / Output actions same frame
    sync_params_to_actions(actions);

    if (settings_dirty_) {
        save_app_config_files();
    }
}

} // namespace aether::ui
