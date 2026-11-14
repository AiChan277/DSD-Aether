#pragma once

#include "ui/params/param_registry.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace aether::ui {

enum class UndoKind : uint8_t {
    ParamEdit = 0,
    GraphEdit = 1,
    CustomEdit = 2,
};

struct UndoEntry {
    UndoKind kind{UndoKind::ParamEdit};
    std::string label{};
    ParamId param_id{ParamId::Count};
    double old_value{0.0};
    double new_value{0.0};
    std::string before_payload{};
    std::string after_payload{};
    uint64_t revision_id{0};
};

struct UndoContext {
    ParamStore* params{nullptr};
    std::function<void(const std::string&)> apply_graph_json{};
    std::function<void(const std::string&)> apply_custom_payload{};
};

/**
 * 200-entry project command-pattern Undo/Redo stack (`§12.12`).
 * Supports interactive drag coalescing (one entry committed per slider drag gesture).
 */
class UndoStack {
  public:
    static constexpr size_t kMaxEntries = 200;

    void clear() noexcept;
    void mark_saved() noexcept;
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_counter_ != saved_counter_; }

    void push_param(ParamId id, double old_val, double new_val);
    void begin_drag(ParamId id, double initial_val);
    void commit_drag(ParamId id, double final_val);
    void cancel_drag() noexcept;

    void push_graph_edit(std::string label, std::string before_json, std::string after_json);
    void push_custom_edit(std::string label, std::string before_payload, std::string after_payload);

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] const char* undo_label() const noexcept { return undo_.empty() ? "" : undo_.back().label.c_str(); }
    [[nodiscard]] const char* redo_label() const noexcept { return redo_.empty() ? "" : redo_.back().label.c_str(); }
    [[nodiscard]] size_t undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] size_t redo_count() const noexcept { return redo_.size(); }

    [[nodiscard]] bool undo(const UndoContext& ctx);
    [[nodiscard]] bool redo(const UndoContext& ctx);

  private:
    void push_entry(UndoEntry&& entry);

    std::deque<UndoEntry> undo_{};
    std::deque<UndoEntry> redo_{};
    uint64_t next_revision_{1};
    uint64_t dirty_counter_{0};
    uint64_t saved_counter_{0};

    bool dragging_{false};
    ParamId drag_param_{ParamId::Count};
    double drag_start_value_{0.0};
};

} // namespace aether::ui
