#pragma once

#include "aether_ffi_contract.h"
#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/theme/precision_dark.hpp"

#include <cstdio>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aether::ui {

enum class SelectionKind : uint8_t {
    None = 0,
    Avatar = 1,
    Camera = 5,
    Bone = 6,
    Material = 7,
    Expression = 8,
    Node = 9,
    Plugin = 10,
    OutputDest = 11,
    StudioLight = 12,
};

struct UiSelection {
    SelectionKind kind{SelectionKind::Avatar};
    int32_t item_index{-1};
    bool multi_select{false};
    std::array<char, 64> name{"Avatar Root"};
};

enum class GizmoTool : uint8_t {
    Select = 0,
    Move = 1,
    Rotate = 2,
    Scale = 3,
};

struct OutlinerBoneInfo {
    uint32_t bone_id{0};
    std::array<char, 64> name{};
    bool humanoid_mapped{false};
    bool hidden{false};
    float confidence{1.0f};
};

struct OutlinerMaterialInfo {
    uint32_t index{0};
    std::array<char, 64> name{};
    bool hidden{false};
};

struct ExpressionSlotInfo {
    uint32_t slot{0};
    std::array<char, 32> name{};
    std::array<char, 16> hotkey{};
    float current_weight{0.0f};
    bool triggered_override{false};
};

struct LoadedPluginVm {
    std::array<char, 64> id{};
    std::array<char, 64> name{};
    std::array<char, 16> kind{"WASM"}; // "WASM" or "Native"
    std::array<char, 128> status_text{};
    bool enabled{true};
    bool faulted{false};
    bool permanently_disabled{false};
    uint32_t restart_count{0};
    uint32_t trap_count{0};
    uint32_t capability_mask{0};
    int32_t native_slot{-1};
};

struct CameraBookmark {
    bool valid{false};
    float yaw_rad{0.0f};
    float pitch_rad{0.0f};
    float log_zoom{0.0f}; // accumulated `zoom_delta` (distance scale = exp(-log_zoom))
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct StudioHostStatus {
    bool host_alive{false};
    bool on_air{false};
    bool ws_enabled{false};
    bool tracking_paused{false};
    bool project_dirty{false};
    std::array<char, 128> project_path{};
    std::array<char, 64> project_title{"Untitled"};
    std::array<char, 64> active_alfx_name{"Studio Clean Daylight"};
    uint32_t compiled_node_count{0};
    float last_compile_ms{0.0f};
    float last_eval_us{0.0f};
    bool eval_budget_warn{false};
    uint64_t error_node_id{0};
    std::array<char, 128> graph_error_msg{};
    AetherGraphPlanStats plan_stats{};
    AetherGraphTelemetryReadout telemetry{};

    [[nodiscard]] bool node_eval_us_for(uint64_t node_id, float& out_us) const noexcept {
        if (telemetry.valid == 0u) {
            return false;
        }
        const uint32_t count = telemetry.active_node_count < AETHER_GRAPH_MAX_DEBUG_NODES
                                   ? telemetry.active_node_count
                                   : static_cast<uint32_t>(AETHER_GRAPH_MAX_DEBUG_NODES);
        for (uint32_t i = 0; i < count; ++i) {
            if (telemetry.node_ids[i] == node_id) {
                out_us = static_cast<float>(telemetry.node_eval_ns[i]) * 0.001f;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool pin_readout_for(uint64_t node_id, uint8_t pin_idx, uint8_t& out_kind,
                                       std::array<float, 4>& out_values) const noexcept {
        if (telemetry.valid == 0u) {
            return false;
        }
        const uint32_t count = telemetry.pin_readout_count < AETHER_GRAPH_MAX_DEBUG_PINS
                                   ? telemetry.pin_readout_count
                                   : static_cast<uint32_t>(AETHER_GRAPH_MAX_DEBUG_PINS);
        for (uint32_t i = 0; i < count; ++i) {
            if (telemetry.pin_node_ids[i] == node_id && telemetry.pin_indices[i] == pin_idx) {
                out_kind = telemetry.pin_kinds[i];
                for (size_t lane = 0; lane < 4; ++lane) {
                    out_values[lane] = telemetry.pin_values[i][lane];
                }
                return out_kind != AETHER_PIN_VALUE_INVALID;
            }
        }
        return false;
    }
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/** Extended actions emitted by StudioWorkspace back to Application / PipelineSkeleton. */
struct StudioActions {
    DevShellViewportActions viewport{};

    bool request_set_on_air{false};
    bool target_on_air{false};
    bool request_toggle_ws{false};
    bool target_ws_enabled{false};
    bool request_pause_tracking{false};

    bool request_new_project{false};
    bool request_open_project_dialog{false};
    bool request_open_project_path{false};
    std::array<char, 512> project_path_buf{};
    bool request_save_project{false};
    bool request_save_project_as{false};
    bool request_open_project_manager{false};
    bool request_export_alfx_dialog{false};
    bool request_import_alfx_dialog{false};
    int32_t request_apply_alfx_preset_index{-1};
    uint8_t request_apply_alfx_mode{0};
    bool request_switch_mode{false};
    StudioMode target_mode{StudioMode::LiveStudio};

    bool request_open_avatar_dialog{false};
    bool request_apply_graph_json{false};
    bool request_recompile_graph_now{false};
    std::string pending_graph_json{};

    // Quick-expression overrides (F1-F8). The workspace ramps each latched slot with a cubic Hermite
    // (smoothstep) curve and sets bit `i` of `expression_dirty_mask` whenever slot `i`'s weight changed.
    static constexpr size_t kQuickExpressionSlots = 8;
    /// Index of the momentary "Reset Neutral" quick slot (F8): it releases every other latched expression.
    static constexpr size_t kResetNeutralQuickSlot = 7;
    /// Graph expression slot each quick slot drives. Graph slots `52..58` feed the dedicated VRM emotion inputs
    /// `57..63` (Joy, Angry, Sorrow, Surprised, Blush, Sparkle, Custom); slot 0 is ARKit eyeBlinkLeft (VRM Blink_L).
    /// The quick slot index itself must never be sent: slots 0..7 are ARKit eye channels (F1 used to close an eye).
    static constexpr std::array<int32_t, kQuickExpressionSlots> kQuickExpressionGraphSlot = {
        52, // F1 Smile        -> Joy
        0,  // F2 Wink L       -> eyeBlinkLeft / Blink_L
        55, // F3 Surprised    -> Surprised
        53, // F4 Angry        -> Angry
        54, // F5 Sad          -> Sorrow
        57, // F6 Sparkle Eyes -> Sparkle
        56, // F7 Blush        -> Blush
        -1, // F8 Reset Neutral (momentary, clears the others)
    };
    std::array<float, kQuickExpressionSlots> expression_weights{};
    uint8_t expression_dirty_mask{0};

    bool request_plugin_consent_reply{false};
    bool plugin_consent_granted{false};
    std::array<char, 128> plugin_consent_id{};

    bool request_reload_plugins{false};
    bool request_reset_native_plugin{false};
    int32_t reset_native_slot{-1};

    bool request_force_device_lost{false};
    bool request_switch_gpu{false};
    uint32_t target_gpu_vendor_id{0};
    uint32_t target_gpu_device_id{0};
    std::array<uint8_t, 8> target_gpu_luid{};
    bool target_gpu_luid_valid{false};
    std::array<char, 128> target_gpu_name{};
    bool request_open_graphics_settings{false};
    bool request_reapply_theme_or_dpi{false};
    bool request_wizard_apply{false};
    int32_t wizard_ep_override{-1};
    uint8_t wizard_denoiser{0};
    uint8_t wizard_res_preset{1};
    uint32_t wizard_fps{60};

    /** Clears per-frame transient flags/deltas without freeing persistent string buffers. */
    void reset_per_frame() noexcept {
        viewport = DevShellViewportActions{};
        request_set_on_air = false;
        target_on_air = false;
        request_toggle_ws = false;
        target_ws_enabled = false;
        request_pause_tracking = false;
        request_new_project = false;
        request_open_project_dialog = false;
        request_open_project_path = false;
        project_path_buf[0] = '\0';
        request_save_project = false;
        request_save_project_as = false;
        request_open_avatar_dialog = false;
        request_apply_graph_json = false;
        request_recompile_graph_now = false;
        pending_graph_json.clear();
        expression_weights.fill(0.0f);
        expression_dirty_mask = 0;
        request_plugin_consent_reply = false;
        plugin_consent_granted = false;
        plugin_consent_id[0] = '\0';
        request_reload_plugins = false;
        request_reset_native_plugin = false;
        reset_native_slot = -1;
        request_force_device_lost = false;
        request_reapply_theme_or_dpi = false;
        request_wizard_apply = false;
        wizard_ep_override = -1;
        wizard_denoiser = 0;
        wizard_res_preset = 1;
        wizard_fps = 60;
    }
};

[[nodiscard]] inline int active_camera_device_index(const HardwareInventory& hw, const DevShellTrackingView& tracking,
                                                    const AppSettings& settings) noexcept {
    if (hw.cameras.empty()) {
        return -1;
    }
    const char* active_link = (tracking.camera_symlink != nullptr && tracking.camera_symlink[0] != '\0')
                                  ? tracking.camera_symlink
                                  : settings.preferred_camera_link.c_str();
    if (active_link != nullptr && active_link[0] != '\0') {
        for (int i = 0; i < static_cast<int>(hw.cameras.size()); ++i) {
            const auto& cam = hw.cameras[static_cast<size_t>(i)];
            if (cam.symbolic_link == active_link || cam.friendly_name == active_link) {
                return i;
            }
        }
    }
    return 0;
}

inline void select_camera_device_action(const CameraDeviceInfo& cam, ParamStore& params, AppSettings& settings,
                                        bool& settings_dirty, StudioActions& actions) noexcept {
    settings.preferred_camera_link = cam.symbolic_link.empty() ? cam.friendly_name : cam.symbolic_link;
    settings_dirty = true;
    (void)params.set(ParamId::TrackingSyntheticCamera, 0.0);
    actions.viewport.request_set_synthetic_camera = true;
    actions.viewport.target_synthetic_camera = false;
    actions.viewport.request_set_camera_active = true;
    actions.viewport.target_camera_active = true;
    actions.viewport.request_select_camera = true;
    actions.viewport.select_camera_fps = std::max(30, params.get_int(ParamId::TrackingCameraFps));
    std::snprintf(actions.viewport.select_camera_link.data(), actions.viewport.select_camera_link.size(), "%s",
                  settings.preferred_camera_link.c_str());
}

[[nodiscard]] inline int active_audio_device_index(const HardwareInventory& hw, const DevShellTrackingView& tracking,
                                                   const AppSettings& settings) noexcept {
    if (hw.audio_inputs.empty()) {
        return -1;
    }
    const char* active_id = (tracking.audio_device_id != nullptr && tracking.audio_device_id[0] != '\0')
                                ? tracking.audio_device_id
                                : settings.preferred_mic_id.c_str();
    if (active_id != nullptr && active_id[0] != '\0') {
        for (int i = 0; i < static_cast<int>(hw.audio_inputs.size()); ++i) {
            if (hw.audio_inputs[static_cast<size_t>(i)].device_id == active_id) {
                return i;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(hw.audio_inputs.size()); ++i) {
        if (hw.audio_inputs[static_cast<size_t>(i)].is_default) {
            return i;
        }
    }
    return 0;
}

inline void select_audio_device_action(std::string_view device_id, AppSettings& settings, bool& settings_dirty,
                                       StudioActions& actions) noexcept {
    settings.preferred_mic_id.assign(device_id);
    settings_dirty = true;
    actions.viewport.request_select_mic = true;
    std::snprintf(actions.viewport.select_mic_id.data(), actions.viewport.select_mic_id.size(), "%s",
                  settings.preferred_mic_id.c_str());
}

inline void format_camera_status_badge(const DevShellTrackingView& tracking, char* out, size_t out_cap) noexcept {
    if (out == nullptr || out_cap == 0U) {
        return;
    }
    const double face_pct = static_cast<double>(tracking.face_confidence * 100.0f);
    if (tracking.using_synthetic_camera) {
        std::snprintf(out, out_cap, "Synthetic Feed (%ux%u @ %u fps) · Face %3.0f%%",
                      tracking.camera_width > 0U ? tracking.camera_width : 1280U,
                      tracking.camera_height > 0U ? tracking.camera_height : 720U,
                      tracking.camera_fps > 0U ? tracking.camera_fps : 60U, face_pct);
    } else if (!tracking.camera_active) {
        std::snprintf(out, out_cap, "Camera Stopped · Face %3.0f%%", face_pct);
    } else if (tracking.camera_width > 0U && tracking.camera_height > 0U) {
        std::snprintf(out, out_cap, "%s (%ux%u @ %u fps) · Face %3.0f%%", tracking.camera_label, tracking.camera_width,
                      tracking.camera_height, tracking.camera_fps, face_pct);
    } else {
        std::snprintf(out, out_cap, "%s · Face %3.0f%%", tracking.camera_label, face_pct);
    }
}

} // namespace aether::ui
