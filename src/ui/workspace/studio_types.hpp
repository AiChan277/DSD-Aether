#pragma once

#include "aether_ffi_contract.h"
#include "core/hw_enum.hpp"
#include "core/paths.hpp"
#include "core/telemetry.hpp"
#include "rhi/rhi_device.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/theme/precision_dark.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aether::ui {

enum class SelectionKind : uint8_t {
    None = 0,
    Avatar = 1,
    LightKey = 2,
    LightFill = 3,
    LightRim = 4,
    Camera = 5,
    Bone = 6,
    Material = 7,
    Expression = 8,
    Node = 9,
    Plugin = 10,
    OutputDest = 11,
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
    std::array<char, 128> project_path{};
    std::array<char, 64> project_title{"Untitled"};
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

    bool request_open_avatar_dialog{false};
    bool request_apply_graph_json{false};
    bool request_recompile_graph_now{false};
    std::string pending_graph_json{};

    // Quick-expression overrides (F1-F8). The workspace ramps each latched slot with a cubic Hermite
    // (smoothstep) curve and sets bit `i` of `expression_dirty_mask` whenever slot `i`'s weight changed.
    static constexpr size_t kQuickExpressionSlots = 8;
    std::array<float, kQuickExpressionSlots> expression_weights{};
    uint8_t expression_dirty_mask{0};

    bool request_plugin_consent_reply{false};
    bool plugin_consent_granted{false};
    std::array<char, 128> plugin_consent_id{};

    bool request_reload_plugins{false};
    bool request_reset_native_plugin{false};
    int32_t reset_native_slot{-1};

    bool request_force_device_lost{false};
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

} // namespace aether::ui
