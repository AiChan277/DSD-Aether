#pragma once

#include "ui/params/param_registry.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aether::ui {

enum class KeyScope : uint8_t {
    Global = 0,
    Viewport = 1,
    NodeGraph = 2,
    Outliner = 3,
};

enum class ActionId : uint16_t {
    OpenPalette = 0,
    ModeLiveStudio,
    ModeRigging,
    ModeNodes,
    ProjectNew,
    ProjectOpen,
    ProjectSave,
    ProjectSaveAs,
    ImportAvatar,
    Undo,
    Redo,
    Preferences,
    ShortcutOverlay,
    ToggleOnAir,
    PauseTracking,
    CalibrateNeutral,
    ToggleRayTracing,
    MaximizeHoveredPanel,
    Expression1,
    Expression2,
    Expression3,
    Expression4,
    Expression5,
    Expression6,
    Expression7,
    Expression8,

    ViewportHeatmap,
    ViewportOrbitOrb,
    ViewportFrameHead,
    ViewportFrameBody,
    ViewportCam1,
    ViewportCam2,
    ViewportCam3,
    ViewportCam4,
    ViewportCam5,
    ViewportStoreCam1,
    ViewportStoreCam2,
    ViewportStoreCam3,
    ViewportStoreCam4,
    ViewportStoreCam5,
    GizmoSelect,
    GizmoMove,
    GizmoRotate,
    GizmoScale,
    GizmoLocalWorld,
    ViewportGrid,

    GraphAddNode,
    GraphDuplicate,
    GraphDelete,
    GraphFrame,
    GraphComment,
    GraphBypass,
    GraphRecompile,

    OutlinerRename,
    OutlinerFilter,
    OutlinerHide,
    OutlinerUnhideAll,
    OutlinerDelete,

    Count,
};

struct KeyBindingSpec {
    ActionId id;
    const char* name;
    const char* label;
    KeyScope scope;
    const char* default_chord;
    bool global_hotkey_capable;
};

struct KeymapConflict {
    ActionId first{ActionId::Count};
    ActionId second{ActionId::Count};
    std::string chord{};
};

class KeymapRegistry {
  public:
    static constexpr size_t kActionCount = static_cast<size_t>(ActionId::Count);

    KeymapRegistry();

    void reset_defaults();
    [[nodiscard]] const std::string& chord(ActionId id) const noexcept;
    [[nodiscard]] bool global_hotkey_enabled(ActionId id) const noexcept;
    void set_chord(ActionId id, const std::string& new_chord);
    void set_global_hotkey_enabled(ActionId id, bool enabled);

    [[nodiscard]] std::vector<KeymapConflict> detect_conflicts() const;
    [[nodiscard]] bool is_pressed(ActionId id) const noexcept;

    [[nodiscard]] bool load_toml(const std::filesystem::path& path);
    [[nodiscard]] bool save_toml(const std::filesystem::path& path) const;

    [[nodiscard]] static const KeyBindingSpec& spec(ActionId id) noexcept;
    [[nodiscard]] static const std::array<KeyBindingSpec, kActionCount>& all_specs() noexcept;
    [[nodiscard]] static const char* scope_name(KeyScope scope) noexcept;
    [[nodiscard]] static std::string normalize_chord(std::string_view raw);

  private:
    std::array<std::string, kActionCount> chords_{};
    std::array<bool, kActionCount> global_hotkeys_{};
};

struct AppSettings {
    bool wizard_completed{false};
    uint64_t hw_inventory_hash{0};
    std::string preferred_camera_link{};
    std::string preferred_gpu_luid{};
    std::string preferred_gpu_name{};
    std::string active_project_path{};
    std::string ws_auth_token{"aether-local-dev-token"};
    std::vector<std::string> recent_projects{};
    bool telemetry_show_substats{true};
    /// Settings-file migration revision. Files written before revision 2 may carry a `tracking.ep_mode = CPU`
    /// that an older hardware wizard auto-selected; those are migrated back to Auto (NPU -> GPU -> CPU) once.
    uint32_t settings_revision{kCurrentRevision};
    static constexpr uint32_t kCurrentRevision = 2;

    void record_recent_project(std::string_view path);
};

[[nodiscard]] std::filesystem::path default_config_dir();
[[nodiscard]] bool atomic_write_text_file(const std::filesystem::path& target, std::string_view utf8_content);
[[nodiscard]] bool load_app_settings_toml(const std::filesystem::path& path, AppSettings& settings, ParamStore& params);
[[nodiscard]] bool save_app_settings_toml(const std::filesystem::path& path, const AppSettings& settings,
                                          const ParamStore& params);

} // namespace aether::ui
