#pragma once

#include "ui/palette/command_palette.hpp"
#include "ui/panels/inspector/inspector_panel.hpp"
#include "ui/panels/nodegraph/node_canvas.hpp"
#include "ui/panels/nodegraph/timeline_console.hpp"
#include "ui/panels/outliner/outliner_panel.hpp"
#include "ui/panels/telemetry/telemetry_bar.hpp"
#include "ui/panels/viewport/viewport_panel.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/widgets/feedback.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/wizard/hardware_optimizer_wizard.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aether::ui {

/**
 * Coordinates the 5-zone Precision Dark Studio workspace (`§12`, `REQ-UI-001..020`),
 * single-source `ParamStore`, 200-entry `UndoStack`, `KeymapRegistry`, Z1-Z5 modular panels,
 * `Ctrl+K` Command Palette, 5-step Hardware Optimizer Wizard, and non-blocking `ToastStack`.
 */
class StudioWorkspace {
  public:
    static constexpr size_t kLogRingDisplayCap = 128;

    StudioWorkspace();

    void init(const RuntimePaths& paths, const HardwareInventory& hw, float dpi_scale,
              const char* initial_avatar_path = nullptr, bool allow_disk_config = true);

    void shutdown();

    void on_dpi_or_theme_changed(float os_dpi_scale);
    void rebuild_fonts_if_needed(const RuntimePaths& paths, float effective_scale);

    void drain_logs() noexcept;

    void set_host_status(const StudioHostStatus& status) noexcept { host_status_ = status; }
    void set_graph_json_from_host(const std::string& json_utf8);
    void sync_plugins(std::span<const LoadedPluginVm> plugins);
    void sync_scene_items(std::span<const OutlinerBoneInfo> bones, std::span<const OutlinerMaterialInfo> materials,
                          std::span<const ExpressionSlotInfo> expressions);

    void push_toast(ToastSeverity sev, const char* title, const char* detail, double now_s,
                    ToastActionKind action = ToastActionKind::None, const char* action_label = nullptr) noexcept;

    void request_plugin_consent(const char* plugin_id, const char* plugin_name, const char* sha256_hex,
                                uint32_t capability_mask) noexcept;

    void open_hardware_wizard() noexcept;
    void switch_mode(StudioMode new_mode);
    void reset_current_layout();

    void render(double now_s, const rhi::RhiDevice& rhi, const TelemetrySnapshot& telemetry,
                const DevShellInputEcho& input_echo, const ImGuiArenaAllocator& arena, bool ffi_layout_ok,
                const DevShellAvatarView& avatar, const DevShellTrackingView& tracking,
                const DevShellRenderView& render_view, const DevShellOutputView& output_view, StudioActions& actions);

    [[nodiscard]] StudioMode mode() const noexcept { return mode_; }
    [[nodiscard]] ParamStore& params() noexcept { return params_; }
    [[nodiscard]] const ParamStore& params() const noexcept { return params_; }
    [[nodiscard]] UndoStack& undo_stack() noexcept { return undo_; }
    [[nodiscard]] const UndoStack& undo_stack() const noexcept { return undo_; }
    [[nodiscard]] KeymapRegistry& keymap() noexcept { return keymap_; }
    [[nodiscard]] const KeymapRegistry& keymap() const noexcept { return keymap_; }
    [[nodiscard]] AppSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const AppSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] NodeCanvas& node_canvas() noexcept { return node_canvas_; }
    [[nodiscard]] ViewportPanel& viewport_panel() noexcept { return viewport_panel_; }
    [[nodiscard]] const UiFontSet& fonts() const noexcept { return fonts_; }
    [[nodiscard]] float effective_dpi_scale() const noexcept {
        return os_dpi_scale_ * (static_cast<float>(params_.get_int(ParamId::AppUiScalePct)) / 100.0f);
    }

  private:
    void build_dock_layout(ImGuiID dockspace_id, StudioMode mode);
    void save_mode_layout_ini(StudioMode mode) const;
    void load_mode_layout_ini(StudioMode mode);
    void sync_params_to_actions(StudioActions& actions) noexcept;
    void handle_global_shortcuts(double now_s, StudioActions& actions);
    void execute_palette_command(const PaletteInvokeResult& res, double now_s, StudioActions& actions);
    void save_app_config_files();
    static void on_log_drained(const LogRecord& rec, void* user) noexcept;

    RuntimePaths paths_{};
    HardwareInventory hw_{};
    UiFontSet fonts_{};
    bool allow_disk_config_{true};
    std::filesystem::path config_dir_{};

    float os_dpi_scale_{1.0f};
    double last_now_s_{0.0};

    StudioMode mode_{StudioMode::LiveStudio};
    bool dock_layout_dirty_{true};
    bool layout_maximized_{false};
    std::array<char, 48> maximized_window_{};
    std::array<char, 48> last_hovered_zone_window_{"Viewport"};

    ParamStore params_{};
    uint64_t synced_param_epoch_{0};
    UndoStack undo_{};
    KeymapRegistry keymap_{};
    AppSettings settings_{};
    bool settings_dirty_{false};

    UiSelection selection_{};
    StudioHostStatus host_status_{};

    OutlinerPanel outliner_panel_{};
    ViewportPanel viewport_panel_{};
    InspectorPanel inspector_panel_{};
    NodeCanvas node_canvas_{};
    TimelineConsole timeline_console_{};
    TelemetryBar telemetry_bar_{};

    CommandPalette command_palette_{};
    PreferencesModal prefs_modal_{};
    ShortcutOverlayModal shortcut_overlay_{};
    HardwareOptimizerWizard wizard_{};
    ToastStack toasts_{};
    PluginConsentPrompt consent_prompt_{};

    // Pre-reserved view-model backing storage (zero per-frame allocs in render)
    std::vector<OutlinerBoneInfo> bones_{};
    std::vector<OutlinerMaterialInfo> materials_{};
    std::vector<ExpressionSlotInfo> expressions_{};
    std::vector<LoadedPluginVm> plugins_{};

    std::array<LogRecord, kLogRingDisplayCap> recent_logs_{};
    size_t log_head_{0};
    size_t log_count_{0};

    bool demo_bones_seeded_{false};
};

} // namespace aether::ui
