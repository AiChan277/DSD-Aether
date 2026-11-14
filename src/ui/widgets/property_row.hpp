#pragma once

#include "ui/params/param_registry.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/theme/precision_dark.hpp"

#include <imgui.h>

#include <cstdint>

namespace aether::ui {

struct PropertyRowActions {
    const char* expose_graph_param_id{nullptr};
    const char* copied_feedback_msg{nullptr};
    bool value_changed{false};
    ParamId changed_param{ParamId::Count};
};

/**
 * Opens a DaVinci Resolve-style 2-column property table (`§2.2`): label column >= 38% of the
 * available width (widened up to 48% only when the longest registered label would otherwise be
 * elided) and a stretch control column. Every `draw_property_row()` until `end_property_table()`
 * becomes one table row. Call `end_property_table()` only when this returned true (ImGui convention).
 */
bool begin_property_table(const char* str_id);
void end_property_table();

/** Width of the label column `begin_property_table()` uses for `avail_w` at the current font size. */
[[nodiscard]] float property_label_column_width(float avail_w) noexcept;

/**
 * Scrubbable drag-slider (`§1 BUG-06`): full-width control well with an inline accent fill bar
 * proportional to `(v - min) / (max - min)`, click-drag scrubbing, double-click / Ctrl+click
 * keyboard entry, and a right-aligned muted unit suffix. Shift = 0.1x, Ctrl = 10x speed.
 */
bool scrub_slider_float(const char* str_id, float* v, float v_min, float v_max, float speed, const char* value_fmt,
                        const char* unit, bool mixed = false);
bool scrub_slider_int(const char* str_id, int* v, int v_min, int v_max, float speed, const char* unit,
                      bool mixed = false);

/** Full-width bordered status badge row; long values wrap instead of overflowing the panel (`§1 BUG-05`). */
void draw_status_badge(const UiFontSet& fonts, const char* label, const char* value, const ImVec4& value_col);

/**
 * Standard Inspector property row (`§12.5`, `§12.12`, `§12.13`, `§12.15`):
 * [label + amber reset dot when != default] [full-width control column]
 * Supports Shift (0.1x) / Ctrl (10x) drag precision, 450 ms hover tooltips,
 * context menu (Reset / Copy / Paste / Copy ID / Expose / Copy OSC),
 * mixed-selection "--" display, inline error bar, and zero per-frame allocations.
 * Callable inside or outside `begin_property_table()` (opens a one-row table when outside).
 */
bool draw_property_row(ParamId id, ParamStore& store, UndoStack& undo, const UiFontSet& fonts,
                       PropertyRowActions& actions, bool mixed = false, bool read_only = false,
                       const char* inline_error = nullptr, const char* error_action_label = nullptr,
                       bool* out_error_action_clicked = nullptr);

} // namespace aether::ui
