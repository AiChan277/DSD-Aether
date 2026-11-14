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
 * Standard 26 px Inspector property row (`§12.5`, `§12.12`, `§12.13`, `§12.15`):
 * [104 px label] [control column] [8 px amber reset dot when != default]
 * Supports Shift (0.1x) / Ctrl (10x) drag precision, 450 ms hover tooltips,
 * context menu (Reset / Copy / Paste / Copy ID / Expose / Copy OSC),
 * mixed-selection "--" display, inline error bar, and zero per-frame allocations.
 */
bool draw_property_row(ParamId id, ParamStore& store, UndoStack& undo, const UiFontSet& fonts,
                       PropertyRowActions& actions, bool mixed = false, bool read_only = false,
                       const char* inline_error = nullptr, const char* error_action_label = nullptr,
                       bool* out_error_action_clicked = nullptr);

} // namespace aether::ui
