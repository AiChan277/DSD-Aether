#pragma once

#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aether::ui {

enum class PinDataKind : uint8_t {
    Float = 0,
    Bool = 1,
    Vec3 = 2,
    Pose = 3,
    Expression = 4,
    Light = 5,
    Trigger = 6,
    Event = 7,
};

struct GraphPinVm {
    std::array<char, 24> name{};
    PinDataKind kind{PinDataKind::Float};
};

struct GraphNodeVm {
    uint64_t id{1};
    std::array<char, 32> kind{};
    std::array<char, 40> title{};
    std::array<char, 64> param_id{};
    float x{0.0f};
    float y{0.0f};
    float w{164.0f};
    float h{78.0f};
    bool selected{false};
    bool bypassed{false};
    uint8_t num_inputs{1};
    uint8_t num_outputs{1};
    std::array<GraphPinVm, 4> inputs{};
    std::array<GraphPinVm, 4> outputs{};
    float constant_val{1.0f};
    double last_us{0.0};
    float last_output{0.0f};
    bool has_error{false};
};

struct GraphEdgeVm {
    uint64_t id{1};
    uint64_t from_node{0};
    uint8_t from_pin{0};
    uint64_t to_node{0};
    uint8_t to_pin{0};
    PinDataKind kind{PinDataKind::Float};
    std::array<ImVec2, 16> cached_pts{};
    bool cache_dirty{true};
};

struct GraphCommentVm {
    uint64_t id{1};
    std::array<char, 48> title{"Group"};
    float x{0.0f};
    float y{0.0f};
    float w{260.0f};
    float h{180.0f};
};

/**
 * Z4 Interactive Node Graph Canvas (`§7.2`, `§12.7`, `§12.14`, `REQ-UI-020`).
 * - Culls off-screen nodes & wires and caches Bezier tessellation per edge.
 * - Uses 8-slot CVD pin palette + distinct pin shapes (Circle/Square/Diamond/Triangle/Hexagon).
 * - Supports pan/zoom, wire connect, double-click reroute insert, Alt+click wire delete,
 *   Tab/Space quick Add Node, Ctrl+D duplicate, Ctrl+G comment box, Ctrl+B bypass,
 *   Ctrl+Enter recompile, F frame, and exposes zero per-frame allocations (`<= 0.4 ms`).
 */
class NodeCanvas {
  public:
    NodeCanvas();

    void reset_default_graph();
    void load_sample_64_nodes();

    /** Replaces graph state from `aether_host_copy_project_json` or Undo/Redo payload cleanly. */
    void set_graph_json(const std::string& json_utf8);
    [[nodiscard]] std::string serialize_graph_json() const;

    /** Expose a parameter inside the graph cleanly (`PropertyRow` right-click action). */
    void add_exposed_param_node(const char* param_id);

    void open_add_node_popup() noexcept { open_add_popup_ = true; }
    void frame_all() noexcept;
    void duplicate_selected(UndoStack& undo, StudioActions& actions);
    void delete_selected(UndoStack& undo, StudioActions& actions);
    void wrap_selected_in_comment(UndoStack& undo, StudioActions& actions);
    void toggle_bypass_selected(UndoStack& undo, StudioActions& actions);

    void draw(const StudioHostStatus& host_status, UndoStack& undo, const UiFontSet& fonts, float dpi_scale,
              UiSelection& selection, StudioActions& actions);

    [[nodiscard]] size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] size_t edge_count() const noexcept { return edges_.size(); }

  private:
    void invalidate_edge_cache() noexcept;
    void commit_edit(const char* label, UndoStack& undo, StudioActions& actions);
    void add_builtin_node(const char* kind, float world_x, float world_y, UndoStack& undo, StudioActions& actions);
    [[nodiscard]] int find_node_index(uint64_t node_id) const noexcept;

    std::vector<GraphNodeVm> nodes_{};
    std::vector<GraphEdgeVm> edges_{};
    std::vector<GraphCommentVm> comments_{};
    uint64_t next_id_{100};

    ImVec2 pan_{24.0f, 24.0f};
    float zoom_{1.0f};
    float last_pan_x_{24.0f};
    float last_pan_y_{24.0f};
    float last_zoom_{1.0f};
    ImVec2 last_canvas_origin_{0.0f, 0.0f};

    bool dragging_node_{false};
    int32_t dragged_node_idx_{-1};
    bool wiring_active_{false};
    uint64_t wire_start_node_{0};
    uint8_t wire_start_pin_{0};
    PinDataKind wire_start_kind_{PinDataKind::Float};

    bool open_add_popup_{false};
    std::array<char, 48> add_filter_{};
    std::string last_committed_json_{};
};

} // namespace aether::ui
