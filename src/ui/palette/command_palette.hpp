#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aether::ui {

enum class PaletteCategory : uint8_t {
    Actions = 0,
    PanelsAndSettings = 1,
    Nodes = 2,
    Files = 3,
};

enum class PaletteCommandTag : uint16_t {
    None = 0,
    ModeLiveStudio,
    ModeRigging,
    ModeNodes,
    ProjectNew,
    ProjectOpen,
    ProjectSave,
    ProjectSaveAs,
    ImportAvatar,
    OpenRecentFile,
    AddNodeByType,
    ToggleOnAir,
    ToggleRayTracing,
    ToggleHeatmap,
    ToggleOrbitOrb,
    ToggleCheckerboard,
    PauseTracking,
    CalibrateNeutral,
    Undo,
    Redo,
    ResetLayout,
    OpenWizard,
    OpenPreferences,
    OpenGraphicsSettings,
    OpenShortcuts,
    ReloadPlugins,
    ForceDeviceLost,
    ToggleWsControl,
};

struct PaletteEntry {
    PaletteCategory category{PaletteCategory::Actions};
    PaletteCommandTag tag{PaletteCommandTag::None};
    std::array<char, 64> id{};
    std::array<char, 80> label{};
    std::array<char, 32> shortcut{};
    std::array<char, 256> payload{};
    uint32_t mru_uses{0};
};

struct PaletteMatch {
    uint16_t entry_idx{0};
    int32_t score{0};
    uint8_t match_count{0};
    std::array<uint8_t, 48> match_positions{};
};

struct PaletteInvokeResult {
    bool invoked{false};
    PaletteCommandTag tag{PaletteCommandTag::None};
    std::array<char, 256> payload{};
};

/**
 * Ctrl+K / Ctrl+Shift+P Command Palette (`§12.10`, `REQ-UI-020`).
 * Zero per-frame heap allocation while open or filtering (`ImGuiListClipper`).
 */
class CommandPalette {
  public:
    CommandPalette();

    void open() noexcept;
    void close() noexcept { is_open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return is_open_; }

    void rebuild_entries(const KeymapRegistry& keymap, std::span<const std::string> recent_files);

    [[nodiscard]] PaletteInvokeResult draw(const KeymapRegistry& keymap, bool on_air, const UiFontSet& fonts,
                                           float dpi_scale);

    [[nodiscard]] static int32_t fuzzy_score(const char* query, const char* text,
                                             std::array<uint8_t, 48>& out_positions, uint8_t& out_count) noexcept;

  private:
    void refilter();

    bool is_open_{false};
    bool focus_input_next_frame_{false};
    std::array<char, 96> query_buf_{};
    std::array<char, 96> last_query_{};
    int32_t selected_row_{0};
    uint8_t active_category_filter_{255}; // 255 = All

    std::vector<PaletteEntry> entries_{};
    std::vector<PaletteMatch> filtered_{};
};

/**
 * Studio Preferences & Keymap Editor modal (`§12.11`, `§12.14`, `§12.15`).
 */
class PreferencesModal {
  public:
    void open(int initial_tab = 0) noexcept;
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    void draw(bool on_air, ParamStore& params, UndoStack& undo, KeymapRegistry& keymap, AppSettings& settings,
              const UiFontSet& fonts, float dpi_scale, PropertyRowActions& row_actions, StudioActions& actions,
              bool& out_settings_dirty);

  private:
    bool open_{false};
    int active_tab_{0}; // 0=General & Quality, 1=Keybindings, 2=Remote & Accessibility
    int32_t editing_action_{-1};
    std::array<char, 32> chord_edit_buf_{};
    std::array<char, 64> ws_token_buf_{};
    bool token_buf_init_{false};
};

/**
 * Keyboard Shortcut Reference Cheat Sheet (`?` / `Ctrl+/`).
 */
class ShortcutOverlayModal {
  public:
    void open() noexcept { open_ = true; }
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    void draw(const KeymapRegistry& keymap, const UiFontSet& fonts, float dpi_scale);

  private:
    bool open_{false};
};

} // namespace aether::ui
