#include "ui/params/undo_stack.hpp"

#include <cmath>
#include <utility>

namespace aether::ui {

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
    dirty_counter_ = 0;
    saved_counter_ = 0;
    dragging_ = false;
    drag_param_ = ParamId::Count;
}

void UndoStack::mark_saved() noexcept {
    saved_counter_ = dirty_counter_;
}

void UndoStack::push_entry(UndoEntry&& entry) {
    entry.revision_id = next_revision_++;
    undo_.push_back(std::move(entry));
    while (undo_.size() > kMaxEntries) {
        undo_.pop_front();
    }
    redo_.clear();
    ++dirty_counter_;
}

void UndoStack::push_param(ParamId id, double old_val, double new_val) {
    if (std::fabs(old_val - new_val) <= 1e-9) {
        return;
    }
    const auto& spec = param_spec(id);
    if ((spec.flags & PARAM_FLAG_UNDOABLE) == 0u) {
        return;
    }
    UndoEntry e{};
    e.kind = UndoKind::ParamEdit;
    e.label = spec.label;
    e.param_id = id;
    e.old_value = old_val;
    e.new_value = new_val;
    push_entry(std::move(e));
}

void UndoStack::begin_drag(ParamId id, double initial_val) {
    dragging_ = true;
    drag_param_ = id;
    drag_start_value_ = initial_val;
}

void UndoStack::commit_drag(ParamId id, double final_val) {
    if (!dragging_ || drag_param_ != id) {
        return;
    }
    dragging_ = false;
    push_param(id, drag_start_value_, final_val);
    drag_param_ = ParamId::Count;
}

void UndoStack::cancel_drag() noexcept {
    dragging_ = false;
    drag_param_ = ParamId::Count;
}

void UndoStack::push_graph_edit(std::string label, std::string before_json, std::string after_json) {
    if (before_json == after_json) {
        return;
    }
    UndoEntry e{};
    e.kind = UndoKind::GraphEdit;
    e.label = std::move(label);
    e.before_payload = std::move(before_json);
    e.after_payload = std::move(after_json);
    push_entry(std::move(e));
}

void UndoStack::push_custom_edit(std::string label, std::string before_payload, std::string after_payload) {
    if (before_payload == after_payload) {
        return;
    }
    UndoEntry e{};
    e.kind = UndoKind::CustomEdit;
    e.label = std::move(label);
    e.before_payload = std::move(before_payload);
    e.after_payload = std::move(after_payload);
    push_entry(std::move(e));
}

bool UndoStack::undo(const UndoContext& ctx) {
    if (undo_.empty()) {
        return false;
    }
    UndoEntry entry = std::move(undo_.back());
    undo_.pop_back();

    switch (entry.kind) {
    case UndoKind::ParamEdit:
        if (ctx.params != nullptr) {
            (void)ctx.params->set(entry.param_id, entry.old_value);
        }
        break;
    case UndoKind::GraphEdit:
        if (ctx.apply_graph_json) {
            ctx.apply_graph_json(entry.before_payload);
        }
        break;
    case UndoKind::CustomEdit:
        if (ctx.apply_custom_payload) {
            ctx.apply_custom_payload(entry.before_payload);
        }
        break;
    }

    redo_.push_back(std::move(entry));
    while (redo_.size() > kMaxEntries) {
        redo_.pop_front();
    }
    ++dirty_counter_;
    return true;
}

bool UndoStack::redo(const UndoContext& ctx) {
    if (redo_.empty()) {
        return false;
    }
    UndoEntry entry = std::move(redo_.back());
    redo_.pop_back();

    switch (entry.kind) {
    case UndoKind::ParamEdit:
        if (ctx.params != nullptr) {
            (void)ctx.params->set(entry.param_id, entry.new_value);
        }
        break;
    case UndoKind::GraphEdit:
        if (ctx.apply_graph_json) {
            ctx.apply_graph_json(entry.after_payload);
        }
        break;
    case UndoKind::CustomEdit:
        if (ctx.apply_custom_payload) {
            ctx.apply_custom_payload(entry.after_payload);
        }
        break;
    }

    undo_.push_back(std::move(entry));
    while (undo_.size() > kMaxEntries) {
        undo_.pop_front();
    }
    ++dirty_counter_;
    return true;
}

} // namespace aether::ui
