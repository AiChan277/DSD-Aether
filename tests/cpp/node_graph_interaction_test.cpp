// UE5 Blueprint-grade Node Graph interaction tests (broadcast overhaul §3.4 / §5.2):
//  (a) create links between compatible pins (and reject incompatible ones),
//  (b) disconnect via disconnect_link(link_id) / disconnect_pin_links(pin) (Alt+Click backend),
//  (c) detach + re-route an existing input link to a new pin,
//  (d) undo / redo restore deleted links through the shared UndoStack,
//  (e) JSON round trip in the Rust host schema (`type`, params, groups).
// The "real input" cases drive NodeCanvas::draw() through a headless ImGui context with synthetic
// mouse / keyboard events, so they exercise the same code path as the Studio.

#include "ui/panels/nodegraph/node_canvas.hpp"
#include "ui/params/project_settings.hpp"
#include "ui/params/undo_stack.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace aether::ui {
namespace {

// Default graph ids (NodeCanvas::reset_default_graph mirrors GraphDocument::default_project_graph)
constexpr uint64_t kNpuPose = 1;
constexpr uint64_t kIk = 2;
constexpr uint64_t kOutPose = 3;
constexpr uint64_t kDspAudio = 4;
constexpr uint64_t kLipSync = 5;
constexpr uint8_t kNpuOutPose = 0;
constexpr uint8_t kNpuOutFace = 2;
constexpr uint8_t kNpuOutHand = 3;
constexpr uint8_t kIkInPose = 0;
constexpr uint8_t kIkInFace = 1;
constexpr uint8_t kIkInHand = 2;

size_t count_links_into(const NodeCanvas& c, uint64_t node, uint8_t pin) {
    size_t n = 0;
    for (const auto& e : c.edges()) {
        n += (e.to_node == node && e.to_pin == pin) ? 1U : 0U;
    }
    return n;
}

UndoContext make_ctx(NodeCanvas& canvas) {
    UndoContext ctx{};
    ctx.apply_graph_json = [&canvas](const std::string& json) { canvas.set_graph_json(json); };
    return ctx;
}

/** Minimal headless ImGui host: one full-screen window containing the canvas. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class CanvasHarness {
  public:
    CanvasHarness() {
        IMGUI_CHECKVERSION();
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1400.0f, 800.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        unsigned char* px = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    }
    ~CanvasHarness() { ImGui::DestroyContext(ctx_); }
    CanvasHarness(const CanvasHarness&) = delete;
    CanvasHarness& operator=(const CanvasHarness&) = delete;

    void frame(NodeCanvas& canvas, UndoStack& undo, StudioActions& actions) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Node Graph", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        canvas.draw(host_, undo, keymap_, fonts_, 1.0f, selection_, actions);
        ImGui::End();
        ImGui::Render();
    }
    void frames(int n, NodeCanvas& canvas, UndoStack& undo, StudioActions& actions) {
        for (int i = 0; i < n; ++i) {
            frame(canvas, undo, actions);
        }
    }

    static void move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
    static void button(int b, bool down) { ImGui::GetIO().AddMouseButtonEvent(b, down); }
    static void alt(bool down) {
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, down);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftAlt, down);
    }
    static void key(ImGuiKey k, bool down) { ImGui::GetIO().AddKeyEvent(k, down); }

    /** Press at `from`, drag in steps to `to`, release. */
    void drag(NodeCanvas& canvas, UndoStack& undo, StudioActions& actions, ImVec2 from, ImVec2 to) {
        move(from);
        frame(canvas, undo, actions);
        button(0, true);
        frame(canvas, undo, actions);
        for (int s = 1; s <= 6; ++s) {
            const float u = static_cast<float>(s) / 6.0f;
            move(ImVec2(from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u));
            frame(canvas, undo, actions);
        }
        button(0, false);
        frame(canvas, undo, actions);
    }

    void click(NodeCanvas& canvas, UndoStack& undo, StudioActions& actions, ImVec2 at) {
        move(at);
        frame(canvas, undo, actions);
        button(0, true);
        frame(canvas, undo, actions);
        button(0, false);
        frame(canvas, undo, actions);
    }

    [[nodiscard]] StudioHostStatus& host() noexcept { return host_; }

  private:
    ImGuiContext* ctx_{nullptr};
    StudioHostStatus host_{};
    KeymapRegistry keymap_{};
    UiFontSet fonts_{};
    UiSelection selection_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace

TEST_CASE("NodeCanvas pin type rules mirror the Rust ValueType::can_connect matrix", "[ui][nodegraph]") {
    REQUIRE(pin_kinds_compatible(PinDataKind::Float, PinDataKind::Float));
    REQUIRE(pin_kinds_compatible(PinDataKind::Bool, PinDataKind::Float));
    REQUIRE(pin_kinds_compatible(PinDataKind::Float, PinDataKind::Bool));
    REQUIRE(pin_kinds_compatible(PinDataKind::Float, PinDataKind::Vec3));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::Vec3, PinDataKind::Float));
    REQUIRE(pin_kinds_compatible(PinDataKind::Pose, PinDataKind::Pose));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::Float, PinDataKind::Pose));
    REQUIRE(pin_kinds_compatible(PinDataKind::AudioFrame, PinDataKind::AudioFrame));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::AudioFrame, PinDataKind::Viseme));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::AudioFrame, PinDataKind::Expression));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::AudioFrame, PinDataKind::Pose));
    REQUIRE(pin_kinds_compatible(PinDataKind::Viseme, PinDataKind::Viseme));
    REQUIRE(pin_kinds_compatible(PinDataKind::Viseme, PinDataKind::Expression));
    REQUIRE_FALSE(pin_kinds_compatible(PinDataKind::Viseme, PinDataKind::Pose));
    REQUIRE(pin_kinds_compatible(PinDataKind::Event, PinDataKind::Event));
}

TEST_CASE("(a) connect_pins links compatible pins, replaces input drivers, rejects mismatches", "[ui][nodegraph]") {
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};
    const size_t base_edges = canvas.edge_count();

    const uint64_t add = canvas.add_node("aether.math.add@1", 900.0f, 40.0f, undo, actions);
    REQUIRE(add != 0);
    uint64_t link = 0;
    REQUIRE(canvas.connect_pins(kNpuPose, kNpuOutFace, add, 0, undo, actions, &link) == ConnectResult::Connected);
    REQUIRE(link != 0);
    REQUIRE(canvas.edge_count() == base_edges + 1);
    REQUIRE(actions.request_apply_graph_json);
    REQUIRE(actions.pending_graph_json.find("\"from_port\":\"face_conf\"") != std::string::npos);

    // An input accepts exactly one driver: connecting again replaces the old wire.
    REQUIRE(canvas.connect_pins(kNpuPose, kNpuOutHand, add, 0, undo, actions) == ConnectResult::Replaced);
    REQUIRE(count_links_into(canvas, add, 0) == 1U);
    REQUIRE(canvas.find_input_link(add, 0)->from_pin == kNpuOutHand);

    // PoseStream -> Float is rejected without mutating the graph.
    const size_t before = canvas.edge_count();
    REQUIRE(canvas.connect_pins(kNpuPose, kNpuOutPose, add, 1, undo, actions) == ConnectResult::TypeMismatch);
    REQUIRE(canvas.connect_pins(add, 0, add, 1, undo, actions) == ConnectResult::SameNode);
    REQUIRE(canvas.edge_count() == before);
}

TEST_CASE("(b) disconnect_link and disconnect_pin_links remove exactly the targeted wires", "[ui][nodegraph]") {
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};

    const GraphEdgeVm* face = canvas.find_input_link(kIk, kIkInFace);
    REQUIRE(face != nullptr);
    const uint64_t face_id = face->id;
    REQUIRE(canvas.disconnect_link(face_id, undo, actions));
    REQUIRE(canvas.find_link(face_id) == nullptr);
    REQUIRE_FALSE(canvas.disconnect_link(face_id, undo, actions));

    // Fan the NPU pose output to a second consumer, then break every link on that output pin.
    const uint64_t plant = canvas.add_node("aether.solver.foot_planting@1", 600.0f, 500.0f, undo, actions);
    REQUIRE(connect_succeeded(canvas.connect_pins(kNpuPose, kNpuOutPose, plant, 0, undo, actions)));
    const size_t before = canvas.edge_count();
    REQUIRE(canvas.disconnect_pin_links(PinRef{kNpuPose, kNpuOutPose, true}, undo, actions) == 2U);
    REQUIRE(canvas.edge_count() == before - 2U);
    REQUIRE(canvas.find_input_link(kIk, kIkInPose) == nullptr);
    REQUIRE(canvas.find_input_link(plant, 0) == nullptr);
    REQUIRE(canvas.disconnect_pin_links(PinRef{kNpuPose, kNpuOutPose, true}, undo, actions) == 0U);
}

TEST_CASE("(c) reroute_link moves the input end and keeps the wire on a type mismatch", "[ui][nodegraph]") {
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};
    const uint64_t add = canvas.add_node("aether.math.add@1", 900.0f, 40.0f, undo, actions);

    const uint64_t hand_id = canvas.find_input_link(kIk, kIkInHand)->id;
    REQUIRE(connect_succeeded(canvas.reroute_link(hand_id, add, 1, undo, actions)));
    REQUIRE(canvas.find_input_link(kIk, kIkInHand) == nullptr);
    const GraphEdgeVm* moved = canvas.find_input_link(add, 1);
    REQUIRE(moved != nullptr);
    REQUIRE(moved->from_node == kNpuPose);
    REQUIRE(moved->from_pin == kNpuOutHand);

    // Float wire cannot land on the Output Pose input: the link stays where it was.
    const uint64_t moved_id = moved->id;
    REQUIRE(canvas.reroute_link(moved_id, kOutPose, 0, undo, actions) == ConnectResult::TypeMismatch);
    REQUIRE(canvas.find_link(moved_id) != nullptr);
    REQUIRE(canvas.find_input_link(add, 1) != nullptr);
}

TEST_CASE("(d) undo/redo restores deleted links, nodes and pin defaults", "[ui][nodegraph][undo]") {
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};
    const UndoContext ctx = make_ctx(canvas);
    const size_t base_edges = canvas.edge_count();
    const size_t base_nodes = canvas.node_count();

    const uint64_t pose_link = canvas.find_input_link(kIk, kIkInPose)->id;
    REQUIRE(canvas.disconnect_link(pose_link, undo, actions));
    REQUIRE(canvas.delete_node(kLipSync, undo, actions));
    REQUIRE_FALSE(canvas.delete_node(kOutPose, undo, actions)); // root sink is protected
    REQUIRE(canvas.node_count() == base_nodes - 1U);
    REQUIRE(canvas.edge_count() == base_edges - 3U); // pose link + lip-sync in/out

    REQUIRE(undo.undo(ctx)); // restore lip-sync node + its two links
    REQUIRE(canvas.node_count() == base_nodes);
    REQUIRE(canvas.edge_count() == base_edges - 1U);
    REQUIRE(undo.undo(ctx)); // restore pose link
    REQUIRE(canvas.edge_count() == base_edges);
    REQUIRE(canvas.find_input_link(kIk, kIkInPose) != nullptr);

    REQUIRE(undo.redo(ctx));
    REQUIRE(canvas.find_input_link(kIk, kIkInPose) == nullptr);
    REQUIRE(undo.redo(ctx));
    REQUIRE(canvas.find_node(kLipSync) == nullptr);
    REQUIRE(undo.undo(ctx)); // lip-sync back again

    REQUIRE_FALSE(canvas.set_input_default(kDspAudio, 0, 0.0f, undo, actions)); // source node has no inputs
    REQUIRE(canvas.set_input_default(kLipSync, 1, 1.75f, undo, actions));       // gain
    REQUIRE(canvas.find_node(kLipSync)->input_value_set[1]);
    REQUIRE(undo.undo(ctx));
    REQUIRE_FALSE(canvas.find_node(kLipSync)->input_value_set[1]);
}

TEST_CASE("(e) graph JSON round-trips in the Rust host schema", "[ui][nodegraph][json]") {
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};
    REQUIRE(canvas.set_input_default(kLipSync, 1, 1.5f, undo, actions));
    REQUIRE(canvas.set_node_bypassed(kIk, true, undo, actions));

    const std::string json = canvas.serialize_graph_json();
    const auto doc = nlohmann::json::parse(json);
    REQUIRE(doc["schema_version"] == 1);
    REQUIRE(doc["nodes"][0].contains("type")); // Rust `GraphNode` key
    REQUIRE_FALSE(doc["nodes"][0].contains("type_id"));
    REQUIRE(doc["groups"].is_array());
    REQUIRE(doc["groups"][0]["nodes"].size() == 6U); // comment box serialized as a GraphGroup with members

    bool saw_gain = false;
    bool saw_bypass = false;
    for (const auto& n : doc["nodes"]) {
        if (n["id"] == kLipSync) {
            saw_gain = n["params"]["gain"].get<float>() == 1.5f;
        }
        if (n["id"] == kIk) {
            saw_bypass = n["params"]["bypass"].get<bool>();
        }
    }
    REQUIRE(saw_gain);
    REQUIRE(saw_bypass);

    // Host-authored params the canvas does not manage survive the round trip; legacy `type_id` loads.
    auto host_doc = doc;
    host_doc["nodes"][0]["params"]["cutoff_hz"] = 3.5;
    host_doc["nodes"][1].erase("type");
    host_doc["nodes"][1]["type_id"] = "aether.ik.visibility_first@1";
    NodeCanvas reloaded;
    reloaded.set_graph_json(host_doc.dump());
    REQUIRE(reloaded.node_count() == canvas.node_count());
    REQUIRE(reloaded.edge_count() == canvas.edge_count());
    const auto again = nlohmann::json::parse(reloaded.serialize_graph_json());
    REQUIRE(again["nodes"][0]["params"]["cutoff_hz"].get<double>() == 3.5);
    REQUIRE(again["nodes"][1]["type"] == "aether.ik.visibility_first@1");

    NodeCanvas third;
    third.set_graph_json(reloaded.serialize_graph_json());
    REQUIRE(third.serialize_graph_json() == reloaded.serialize_graph_json());
}

TEST_CASE("Mechanics 1/2/4/5 work through real ImGui mouse + keyboard input", "[ui][nodegraph][input]") {
    CanvasHarness h;
    NodeCanvas canvas;
    UndoStack undo;
    StudioActions actions{};
    const UndoContext ctx = make_ctx(canvas);
    h.frames(3, canvas, undo, actions);

    SECTION("Mechanic 1: Alt+Click on a connected input pin breaks its wire") {
        const ImVec2 pin = canvas.pin_screen_position(PinRef{kIk, kIkInPose, false});
        CanvasHarness::alt(true);
        h.click(canvas, undo, actions, pin);
        CanvasHarness::alt(false);
        h.frame(canvas, undo, actions);
        REQUIRE(canvas.find_input_link(kIk, kIkInPose) == nullptr);
        REQUIRE(undo.can_undo());
        REQUIRE(undo.undo(ctx));
        REQUIRE(canvas.find_input_link(kIk, kIkInPose) != nullptr);
    }

    SECTION("Mechanic 1: Alt+Click on a wire breaks only that wire") {
        const uint64_t face_id = canvas.find_input_link(kIk, kIkInFace)->id;
        const size_t before = canvas.edge_count();
        CanvasHarness::alt(true);
        h.click(canvas, undo, actions, canvas.link_midpoint_screen(face_id));
        CanvasHarness::alt(false);
        h.frame(canvas, undo, actions);
        REQUIRE(canvas.find_link(face_id) == nullptr);
        REQUIRE(canvas.edge_count() == before - 1U);
    }

    SECTION("Mechanic 2: drag from a connected input detaches and re-routes onto another input") {
        const uint64_t add = canvas.add_node("aether.math.add@1", 360.0f, 520.0f, undo, actions);
        h.frames(2, canvas, undo, actions);
        h.drag(canvas, undo, actions, canvas.pin_screen_position(PinRef{kIk, kIkInHand, false}),
               canvas.pin_screen_position(PinRef{add, 0, false}));
        REQUIRE(canvas.find_input_link(kIk, kIkInHand) == nullptr);
        const GraphEdgeVm* moved = canvas.find_input_link(add, 0);
        REQUIRE(moved != nullptr);
        REQUIRE(moved->from_node == kNpuPose);
        REQUIRE(moved->from_pin == kNpuOutHand);
        REQUIRE(std::string(undo.undo_label()) == "Re-route Wire");
    }

    SECTION("Mechanic 2: dropping a detached wire on empty canvas disconnects it") {
        const ImVec2 from = canvas.pin_screen_position(PinRef{kIk, kIkInFace, false});
        h.drag(canvas, undo, actions, from, ImVec2(from.x + 40.0f, 760.0f));
        REQUIRE(canvas.find_input_link(kIk, kIkInFace) == nullptr); // removed while the spawn menu is open
        // Dismiss the spawn menu by clicking elsewhere: the disconnect is committed as one undo step.
        h.click(canvas, undo, actions, ImVec2(1300.0f, 120.0f));
        h.frames(2, canvas, undo, actions);
        REQUIRE(canvas.find_input_link(kIk, kIkInFace) == nullptr);
        REQUIRE(std::string(undo.undo_label()) == "Disconnect Wire");
        REQUIRE(undo.undo(ctx));
        REQUIRE(canvas.find_input_link(kIk, kIkInFace) != nullptr);
    }

    SECTION("Mechanic 4: click selects a wire, Delete removes it; Ctrl+Z restores it") {
        const uint64_t lip_out = canvas.find_input_link(kLipSync + 1U, 0)->id; // lip-sync -> Output Expressions
        h.click(canvas, undo, actions, canvas.link_midpoint_screen(lip_out));
        REQUIRE(canvas.selected_link_id() == lip_out);
        CanvasHarness::key(ImGuiKey_Delete, true);
        h.frame(canvas, undo, actions);
        CanvasHarness::key(ImGuiKey_Delete, false);
        h.frame(canvas, undo, actions);
        REQUIRE(canvas.find_link(lip_out) == nullptr);
        REQUIRE(undo.undo(ctx));
        REQUIRE(canvas.find_link(lip_out) != nullptr);
    }

    SECTION("Dragging from an output onto a compatible input creates a wire; mismatches are refused") {
        const uint64_t add = canvas.add_node("aether.math.add@1", 360.0f, 520.0f, undo, actions);
        h.frames(2, canvas, undo, actions);
        const size_t before = canvas.edge_count();
        h.drag(canvas, undo, actions, canvas.pin_screen_position(PinRef{kDspAudio, 1, true}), // rms_db (Float)
               canvas.pin_screen_position(PinRef{add, 1, false}));
        REQUIRE(canvas.edge_count() == before + 1U);
        REQUIRE(canvas.find_input_link(add, 1) != nullptr);

        h.drag(canvas, undo, actions, canvas.pin_screen_position(PinRef{kDspAudio, 0, true}), // audio frame
               canvas.pin_screen_position(PinRef{kOutPose, 0, false}));
        REQUIRE(canvas.find_input_link(kOutPose, 0)->from_node == kIk); // untouched
    }

    SECTION("AudioFrame (purple) vs Viseme (amber) schema and legacy saved graph compatibility") {
        REQUIRE(tokens().col32_pin_types[static_cast<size_t>(PinDataKind::AudioFrame)] == hex_col32(0xA855F7FF));
        REQUIRE(tokens().col32_pin_types[static_cast<size_t>(PinDataKind::Viseme)] == hex_col32(0xEAB308FF));

        const auto* audio_edge = canvas.find_input_link(5, 0);
        REQUIRE(audio_edge != nullptr);
        REQUIRE(audio_edge->from_node == kDspAudio);
        REQUIRE(audio_edge->kind == PinDataKind::AudioFrame);

        const auto* viseme_edge = canvas.find_input_link(6, 0);
        REQUIRE(viseme_edge != nullptr);
        REQUIRE(viseme_edge->from_node == 5);
        REQUIRE(viseme_edge->kind == PinDataKind::Viseme);

        // Legacy saved graph using pre-AudioFrame port names ("visemes" -> "visemes", "expressions" -> "expressions")
        const std::string legacy_json = R"({
            "schema_version": 1,
            "nodes": [
                {"id": 4, "type_id": "aether.source.dsp_audio@1", "position": [0.0, 0.0]},
                {"id": 5, "type_id": "aether.expr.formant_lipsync@1", "position": [200.0, 0.0]},
                {"id": 6, "type_id": "aether.output.expressions@1", "position": [400.0, 0.0]}
            ],
            "edges": [
                {"id": 10, "from_node": 4, "from_port": "visemes", "to_node": 5, "to_port": "visemes"},
                {"id": 11, "from_node": 5, "from_port": "expressions", "to_node": 6, "to_port": "expressions"}
            ]
        })";
        canvas.set_graph_json(legacy_json);
        const auto* migrated_in = canvas.find_input_link(5, 0);
        const auto* migrated_out = canvas.find_input_link(6, 0);
        REQUIRE(migrated_in != nullptr);
        REQUIRE(migrated_in->kind == PinDataKind::AudioFrame);
        REQUIRE(migrated_out != nullptr);
        REQUIRE(migrated_out->kind == PinDataKind::Viseme);
    }

    SECTION("Watch Pin Value toggles live pin badges without heap allocs and renders node footer Eval") {
        h.host().compiled_node_count = 6;
        h.host().telemetry.valid = 1;
        h.host().telemetry.active_node_count = 2;
        h.host().telemetry.node_ids[0] = kDspAudio;
        h.host().telemetry.node_eval_ns[0] = 4200; // 4.2 us
        h.host().telemetry.pin_readout_count = 2;
        h.host().telemetry.pin_node_ids[0] = kDspAudio;
        h.host().telemetry.pin_indices[0] = 1; // rms_db
        h.host().telemetry.pin_kinds[0] = AETHER_PIN_VALUE_FLOAT;
        h.host().telemetry.pin_values[0][0] = -18.5f;
        h.host().telemetry.pin_node_ids[1] = kDspAudio;
        h.host().telemetry.pin_indices[1] = 2; // voice_active
        h.host().telemetry.pin_kinds[1] = AETHER_PIN_VALUE_BOOL;
        h.host().telemetry.pin_values[1][0] = 1.0f;

        float eval_us = 0.0f;
        REQUIRE(h.host().node_eval_us_for(kDspAudio, eval_us));
        REQUIRE(std::abs(eval_us - 4.2f) < 1e-4f);

        const PinRef rms_out{kDspAudio, 1, true};
        REQUIRE(!canvas.is_pin_watched(rms_out));
        REQUIRE(canvas.toggle_pin_watched(rms_out));
        REQUIRE(canvas.is_pin_watched(rms_out));
        REQUIRE(canvas.watched_pin_count() == 1U);

        char buf[48]{};
        REQUIRE(canvas.format_watched_pin_value(rms_out, h.host(), buf, sizeof(buf)) > 0U);
        REQUIRE(std::string(buf) == "-18.500");

        const PinRef bool_out{kDspAudio, 2, true};
        REQUIRE(canvas.toggle_pin_watched(bool_out));
        REQUIRE(canvas.format_watched_pin_value(bool_out, h.host(), buf, sizeof(buf)) > 0U);
        REQUIRE(std::string(buf) == "true");

        // Render multiple frames with watched badges + live Eval footer
        h.frames(2, canvas, undo, actions);

        REQUIRE(canvas.toggle_pin_watched(rms_out));
        REQUIRE(!canvas.is_pin_watched(rms_out));
        REQUIRE(canvas.watched_pin_count() == 1U);
    }
}

} // namespace aether::ui
