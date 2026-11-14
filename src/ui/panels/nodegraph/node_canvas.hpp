#pragma once

#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"
#include "ui/workspace/studio_types.hpp"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aether::ui {

/** UI pin data types mirroring the Rust graph `ValueType`s that the canvas can author (`§3.2`). */
enum class PinDataKind : uint8_t {
    Float = 0,      // Float / confidence (cyan circle)
    Bool = 1,       // Bool (rose square)
    Vec3 = 2,       // Vec3 / direction (orange circle)
    Pose = 3,       // PoseStream, 55 joints (pink diamond)
    Expression = 4, // Blendshape52 ExprSet (gold rounded square)
    Viseme = 5,     // Viseme5 lip-sync weights (amber rounded square)
    Event = 6,      // Event / trigger exec (white arrow)
    AudioFrame = 7, // DSP audio frame: envelope & F1/F2 formants (purple waveform diamond)
};

/** Mirrors Rust `ValueType::can_connect` so the canvas only creates links the compiler accepts. */
[[nodiscard]] bool pin_kinds_compatible(PinDataKind from_output, PinDataKind to_input) noexcept;
[[nodiscard]] const char* pin_kind_name(PinDataKind kind) noexcept;

/** Blueprint header category (`§3.1`): drives the gradient banner color. */
enum class NodeCategory : uint8_t {
    Source = 0, // Input / sensor sources: crimson
    Solver = 1, // Solvers / filters / IK: function blue
    Blend = 2,  // Blend / math / procedural: emerald teal
    Output = 3, // Output sinks: royal purple
};

inline constexpr size_t kMaxNodePins = 6;

struct GraphPinVm {
    std::array<char, 24> name{};
    PinDataKind kind{PinDataKind::Float};
};

struct GraphNodeVm {
    uint64_t id{1};
    std::array<char, 64> kind{};
    std::array<char, 48> title{};
    std::array<char, 64> param_id{};
    NodeCategory category{NodeCategory::Blend};
    float x{0.0f};
    float y{0.0f};
    float w{168.0f}; // world units; recomputed from pin/label text by the canvas layout pass
    float h{68.0f};
    bool selected{false};
    bool bypassed{false};
    bool has_error{false};
    uint8_t num_inputs{0};
    uint8_t num_outputs{0};
    std::array<GraphPinVm, kMaxNodePins> inputs{};
    std::array<GraphPinVm, kMaxNodePins> outputs{};
    /** Unconnected scalar input defaults, serialized as `params.<pin_name>` (read by the Rust compiler). */
    std::array<float, kMaxNodePins> input_values{};
    std::array<bool, kMaxNodePins> input_value_set{};
    float layout_font_px{0.0f}; // font size the cached width was measured at (0 = needs layout)
    /** Unmanaged `params` / `ports` JSON preserved verbatim across UI round trips (set at load time only). */
    std::string extra_params_json{};
    std::string ports_json{};
};

struct GraphEdgeVm {
    uint64_t id{1};
    uint64_t from_node{0};
    uint8_t from_pin{0};
    uint64_t to_node{0};
    uint8_t to_pin{0};
    PinDataKind kind{PinDataKind::Float};
};

/** Visual comment box; serialized as a Rust `GraphGroup` (title, position, size, member nodes). */
struct GraphCommentVm {
    uint64_t id{1};
    std::array<char, 48> title{"Group"};
    float x{0.0f};
    float y{0.0f};
    float w{260.0f};
    float h{180.0f};
    bool sticky{false}; // true: Rust `GraphComment` sticky note; false: `GraphGroup` box
};

/** Identifies one pin on one node. */
struct PinRef {
    uint64_t node{0};
    uint8_t pin{0};
    bool output{false};
};

enum class ConnectResult : uint8_t {
    Connected = 0, // new link created
    Replaced = 1,  // new link created, previous link into that input removed
    AlreadyLinked = 2,
    TypeMismatch = 3,
    SameNode = 4,
    InvalidPin = 5,
};

[[nodiscard]] constexpr bool connect_succeeded(ConnectResult r) noexcept {
    return r == ConnectResult::Connected || r == ConnectResult::Replaced;
}

/**
 * Z4 Unreal Engine 5 Blueprint-grade Node Graph (`§3`, `§7.2`, `§12.7`, `REQ-UI-020`).
 *
 * Visuals: category-gradient 26 px headers, strict 2-column pin rows (24 px) with labeled inputs left /
 * right-aligned labeled outputs, inline default boxes on unconnected scalar inputs, 18 px status footer,
 * node height = header + max(in, out) * row + footer, typed pin shapes (hollow when unconnected), and
 * cubic Bezier wires (shadow + core, amber when selected, animated pulse while the plan evaluates).
 *
 * Wire editing (BUG-01, `§3.4`): Alt+click pin/wire breaks links; dragging a connected input detaches and
 * re-routes its wire; right-click menus on pins, wires and nodes; wire selection + Delete/Backspace;
 * every edit is one graph-JSON snapshot on the shared `UndoStack` (Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z) and
 * requests an immediate host recompile. Edit operations are public so they are unit-testable headless.
 */
class NodeCanvas {
  public:
    NodeCanvas();

    void reset_default_graph();
    void load_sample_64_nodes();

    /** Replaces graph state from `aether_host_copy_project_json` or an Undo/Redo payload. */
    void set_graph_json(const std::string& json_utf8);
    [[nodiscard]] std::string serialize_graph_json() const;

    /** Exposes a parameter inside the graph (`PropertyRow` right-click action). */
    void add_exposed_param_node(const char* param_id);

    // ---- Graph editing API (each call records one undo snapshot + requests a recompile) ----
    uint64_t add_node(const char* type_id, float world_x, float world_y, UndoStack& undo, StudioActions& actions);
    ConnectResult connect_pins(uint64_t from_node, uint8_t from_pin, uint64_t to_node, uint8_t to_pin, UndoStack& undo,
                               StudioActions& actions, uint64_t* out_link_id = nullptr);
    bool disconnect_link(uint64_t link_id, UndoStack& undo, StudioActions& actions);
    size_t disconnect_pin_links(const PinRef& pin, UndoStack& undo, StudioActions& actions);
    /** Moves the input end of `link_id` to `new_to_node.new_to_pin` (Mechanic 2 drop on another input). */
    ConnectResult reroute_link(uint64_t link_id, uint64_t new_to_node, uint8_t new_to_pin, UndoStack& undo,
                               StudioActions& actions);
    /** Splits `link_id` with a type-matched pass-through filter node; returns the new node id (0 on failure). */
    uint64_t insert_reroute_node(uint64_t link_id, UndoStack& undo, StudioActions& actions);
    size_t break_node_links(uint64_t node_id, UndoStack& undo, StudioActions& actions);
    bool delete_node(uint64_t node_id, UndoStack& undo, StudioActions& actions);
    bool set_node_bypassed(uint64_t node_id, bool bypassed, UndoStack& undo, StudioActions& actions);
    bool set_input_default(uint64_t node_id, uint8_t pin, float value, UndoStack& undo, StudioActions& actions);
    bool reset_input_default(uint64_t node_id, uint8_t pin, UndoStack& undo, StudioActions& actions);
    void duplicate_selected(UndoStack& undo, StudioActions& actions);
    /** Deletes selected links and selected non-root nodes in one undo step. */
    void delete_selected(UndoStack& undo, StudioActions& actions);
    void wrap_selected_in_comment(UndoStack& undo, StudioActions& actions);
    void toggle_bypass_selected(UndoStack& undo, StudioActions& actions);

    void select_link(uint64_t link_id) noexcept;
    void clear_selection() noexcept;
    [[nodiscard]] uint64_t selected_link_id() const noexcept { return selected_link_id_; }

    static constexpr size_t kMaxWatchedPins = 32;
    [[nodiscard]] bool is_pin_watched(const PinRef& pin) const noexcept;
    bool set_pin_watched(const PinRef& pin, bool watched) noexcept;
    bool toggle_pin_watched(const PinRef& pin) noexcept;
    [[nodiscard]] size_t watched_pin_count() const noexcept { return watched_pin_count_; }
    size_t format_watched_pin_value(const PinRef& pin, const StudioHostStatus& host_status, char* out,
                                      size_t cap) const noexcept;

    void open_add_node_popup() noexcept { open_add_popup_ = true; }
    /** Smoothly pans/zooms to fit every node on the next draw (`F`). */
    void frame_all() noexcept { frame_request_ = true; }

    void draw(const StudioHostStatus& host_status, UndoStack& undo, const KeymapRegistry& keymap,
              const UiFontSet& fonts, float dpi_scale, UiSelection& selection, StudioActions& actions);

    /** Screen position of a pin / wire midpoint as laid out by the last `draw()` (tests, tooling). */
    [[nodiscard]] ImVec2 pin_screen_position(const PinRef& pin) const noexcept;
    [[nodiscard]] ImVec2 link_midpoint_screen(uint64_t link_id) const noexcept;

    [[nodiscard]] size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] size_t edge_count() const noexcept { return edges_.size(); }
    [[nodiscard]] std::span<const GraphNodeVm> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::span<const GraphEdgeVm> edges() const noexcept { return edges_; }
    [[nodiscard]] const GraphNodeVm* find_node(uint64_t node_id) const noexcept;
    [[nodiscard]] const GraphEdgeVm* find_link(uint64_t link_id) const noexcept;
    /** Link currently driving `node.pin` input, or nullptr. */
    [[nodiscard]] const GraphEdgeVm* find_input_link(uint64_t node_id, uint8_t pin) const noexcept;
    /** Root sink nodes (`Output Pose`, `Output Expressions`) cannot be deleted or duplicated. */
    [[nodiscard]] static bool is_root_node_kind(const char* type_id) noexcept;
    [[nodiscard]] static NodeCategory category_for_kind(const char* type_id) noexcept;

  private:
    enum class Interaction : uint8_t {
        None = 0,
        PressNode, // left-pressed a node body; becomes DragNodes past the threshold
        DragNodes,
        Wiring,        // dragging a new / detached wire
        PressInputPin, // left-pressed a connected input; becomes Wiring (detach) past the threshold
        BoxSelect,
        PanCanvas,
        PressRightCanvas,
    };

    struct HoverInfo {
        int32_t node_idx{-1};
        bool on_header{false};
        PinRef pin{};
        bool pin_valid{false};
        uint64_t link_id{0};
    };

    struct CanvasXform {
        ImVec2 origin{};
        float zs{1.0f}; // zoom * dpi
        ImVec2 pan{};
        [[nodiscard]] ImVec2 to_screen(float wx, float wy) const noexcept {
            return ImVec2(origin.x + (wx + pan.x) * zs, origin.y + (wy + pan.y) * zs);
        }
        [[nodiscard]] ImVec2 to_world(ImVec2 sp) const noexcept {
            return ImVec2((sp.x - origin.x) / zs - pan.x, (sp.y - origin.y) / zs - pan.y);
        }
    };

    void commit_edit(const char* label, UndoStack& undo, StudioActions& actions);
    [[nodiscard]] int find_node_index(uint64_t node_id) const noexcept;
    [[nodiscard]] int find_link_index(uint64_t link_id) const noexcept;
    [[nodiscard]] bool pin_has_link(const PinRef& pin) const noexcept;
    [[nodiscard]] PinDataKind pin_kind(const PinRef& pin) const noexcept;
    ConnectResult connect_no_commit(uint64_t from_node, uint8_t from_pin, uint64_t to_node, uint8_t to_pin,
                                    uint64_t* out_link_id);
    uint64_t add_node_no_commit(const char* type_id, float world_x, float world_y);

    void layout_node(GraphNodeVm& n, const UiFontSet& fonts) const;
    [[nodiscard]] ImVec2 pin_screen_pos(const CanvasXform& xf, const GraphNodeVm& n, bool output,
                                        uint8_t pin) const noexcept;
    [[nodiscard]] HoverInfo compute_hover(const CanvasXform& xf, ImVec2 mouse, float dpi) const;
    void handle_mouse(const CanvasXform& xf, const HoverInfo& hov, bool canvas_hovered, float dpi, UndoStack& undo,
                      UiSelection& selection, StudioActions& actions);
    void handle_keyboard(const CanvasXform& xf, const KeymapRegistry& keymap, UndoStack& undo, StudioActions& actions);
    void apply_frame_animation(ImVec2 canvas_size, float dpi);
    void finish_wire_drop(const CanvasXform& xf, const HoverInfo& hov, float dpi, UndoStack& undo,
                          StudioActions& actions);
    void restore_detached_link();
    void prune_watched_pins() noexcept;
    void draw_node(ImDrawList* dl, const CanvasXform& xf, GraphNodeVm& n, bool hovered, const UiFontSet& fonts,
                   const StudioHostStatus& host_status, float dpi, UndoStack& undo, StudioActions& actions);
    void draw_context_menus(const CanvasXform& xf, UndoStack& undo, StudioActions& actions);
    void draw_add_node_menu(UndoStack& undo, StudioActions& actions);

    std::vector<GraphNodeVm> nodes_{};
    std::vector<GraphEdgeVm> edges_{};
    std::vector<GraphCommentVm> comments_{};
    std::array<PinRef, kMaxWatchedPins> watched_pins_{};
    size_t watched_pin_count_{0};
    uint64_t next_id_{100};
    std::string last_committed_json_{};

    // View
    ImVec2 pan_{24.0f, 24.0f};
    float zoom_{1.0f};
    bool frame_request_{false};
    bool frame_animating_{false};
    ImVec2 frame_target_pan_{};
    float frame_target_zoom_{1.0f};
    ImVec2 last_canvas_size_{0.0f, 0.0f};
    ImVec2 last_canvas_origin_{0.0f, 0.0f};
    float last_dpi_{1.0f};

    // Interaction
    Interaction interaction_{Interaction::None};
    ImVec2 press_pos_{};
    PinRef press_pin_{};
    bool nodes_moved_{false};
    ImVec2 box_start_{};
    uint64_t selected_link_id_{0};
    uint64_t hovered_link_id_{0};

    // Live wire (anchor pin; detached link restored on cancel)
    PinRef wire_anchor_{};
    bool wire_has_detached_{false};
    GraphEdgeVm wire_detached_{};

    // Popups
    bool open_add_popup_{false};
    bool spawn_from_wire_{false};
    bool spawn_pending_detach_commit_{false};
    ImVec2 spawn_world_pos_{};
    std::array<char, 48> add_filter_{};
    PinRef ctx_pin_{};
    uint64_t ctx_link_id_{0};
    uint64_t ctx_node_id_{0};
    bool open_pin_ctx_{false};
    bool open_link_ctx_{false};
    bool open_node_ctx_{false};
    bool open_canvas_ctx_{false};
};

} // namespace aether::ui
