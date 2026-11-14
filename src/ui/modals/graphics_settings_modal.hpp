#pragma once

#include "rhi/rhi_device.hpp"
#include "ui/dev_shell.hpp"
#include "ui/params/param_registry.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/theme/precision_dark.hpp"
#include "ui/widgets/property_row.hpp"
#include "ui/workspace/studio_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aether::ui {

/**
 * Dedicated Graphics, Multi-GPU, Live2D Style, and Ray Tracing Settings modal.
 */
class GraphicsSettingsModal {
  public:
    void open(int initial_tab = 0) noexcept {
        open_ = true;
        active_tab_ = initial_tab;
    }
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    void draw(bool on_air, const rhi::RhiDevice& rhi, const DevShellRenderView& render_view, ParamStore& params,
              UndoStack& undo, AppSettings& settings, const UiFontSet& fonts, float dpi_scale,
              PropertyRowActions& row_actions, StudioActions& actions, bool& out_settings_dirty);

    /** Shared GPU device selector combo used by both GraphicsSettingsModal and InspectorPanel. */
    static void draw_gpu_selector_combo(const char* widget_id, bool on_air, const rhi::RhiDevice& rhi,
                                        const DevShellRenderView& render_view, AppSettings& settings,
                                        StudioActions& actions, bool& out_settings_dirty);

    /** Formats an 8-byte adapter LUID as 16 lowercase hex characters. */
    static void format_luid_hex(const std::array<uint8_t, 8>& luid, char* out, size_t cap) noexcept;

  private:
    bool open_{false};
    int active_tab_{0};
    int last_preset_applied_{-1};
};

} // namespace aether::ui
