#include "ui/panels/nodegraph/node_canvas.hpp"

#include "core/log.hpp"
#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace aether::ui {
namespace {

// ---------------------------------------------------------------------------------------------
// Built-in node templates. Pin names/types mirror `crates/aether-plugin-host/src/graph/builtins.rs`
// (`resolve_ports`) so every link the canvas authors compiles on the host.
// ---------------------------------------------------------------------------------------------
constexpr PinDataKind kF = PinDataKind::Float;
constexpr PinDataKind kB = PinDataKind::Bool;
constexpr PinDataKind kV = PinDataKind::Vec3;
constexpr PinDataKind kP = PinDataKind::Pose;
constexpr PinDataKind kE = PinDataKind::Expression;
constexpr PinDataKind kVis = PinDataKind::Viseme;
constexpr PinDataKind kEv = PinDataKind::Event;
constexpr PinDataKind kAud = PinDataKind::AudioFrame;

struct PinSpec {
    const char* name;
    PinDataKind kind;
};

struct NodeTemplate {
    const char* type_id;
    const char* title;
    NodeCategory category;
    const char* menu_group;
    std::array<PinSpec, kMaxNodePins> in;
    uint8_t n_in;
    std::array<PinSpec, kMaxNodePins> out;
    uint8_t n_out;
};

constexpr NodeCategory kSrc = NodeCategory::Source;
constexpr NodeCategory kSol = NodeCategory::Solver;
constexpr NodeCategory kBl = NodeCategory::Blend;
constexpr NodeCategory kOut = NodeCategory::Output;

constexpr std::array<NodeTemplate, 39> kTemplates = {{
    // Sources (crimson)
    {"aether.source.npu_pose@1",
     "NPU Pose In",
     kSrc,
     "Sources",
     {},
     0,
     {{{"pose", kP}, {"expressions", kE}, {"face_conf", kF}, {"hand_conf", kF}, {"body_conf", kF}, {"ep_id", kF}}},
     6},
    {"aether.source.dsp_audio@1",
     "DSP Audio In",
     kSrc,
     "Sources",
     {},
     0,
     {{{"audio", kAud}, {"rms_db", kF}, {"voice_active", kB}}},
     3},
    {"aether.source.clock@1",
     "Studio Clock",
     kSrc,
     "Sources",
     {},
     0,
     {{{"time_s", kF}, {"dt_s", kF}, {"tick", kF}}},
     3},
    {"aether.source.osc_in@1", "OSC In", kSrc, "Sources", {}, 0, {{{"value", kF}, {"trigger", kEv}}}, 2},
    {"aether.source.hotkey@1", "Hotkey", kSrc, "Sources", {}, 0, {{{"pressed", kEv}, {"held", kB}}}, 2},
    // Solvers / filters / IK (function blue)
    {"aether.ik.visibility_first@1",
     "Visibility-First IK",
     kSol,
     "Solvers & Filters",
     {{{"pose", kP}, {"face_conf", kF}, {"hand_conf", kF}, {"body_conf", kF}}},
     4,
     {{{"pose", kP}}},
     1},
    {"aether.solver.look_at@1",
     "LookAt Solver",
     kSol,
     "Solvers & Filters",
     {{{"target", kV}, {"weight", kF}}},
     2,
     {{{"head_yaw", kF}, {"head_pitch", kF}}},
     2},
    {"aether.solver.idle_breath@1",
     "Idle Breath",
     kSol,
     "Solvers & Filters",
     {{{"rate_hz", kF}, {"amplitude", kF}}},
     2,
     {{{"pose", kP}}},
     1},
    {"aether.solver.idle_attractor@1",
     "Idle Attractor",
     kSol,
     "Solvers & Filters",
     {{{"blend", kF}}},
     1,
     {{{"pose", kP}}},
     1},
    {"aether.expr.formant_lipsync@1",
     "Formant Lip-Sync",
     kSol,
     "Solvers & Filters",
     {{{"audio", kAud}, {"gain", kF}}},
     2,
     {{{"visemes", kVis}}},
     1},
    {"aether.math.smooth@1", "1-Euro Smooth", kSol, "Solvers & Filters", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    {"aether.math.spring@1", "Scalar Spring", kSol, "Solvers & Filters", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    // Blend / math / procedural (emerald teal)
    {"aether.solver.foot_planting@1",
     "CoM Foot Plant",
     kBl,
     "Blend & Procedural",
     {{{"pose", kP}, {"floor_y", kF}}},
     2,
     {{{"pose", kP}}},
     1},
    {"aether.expr.mixer@1",
     "Expression Mixer",
     kBl,
     "Blend & Procedural",
     {{{"a", kE}, {"b", kE}, {"weight", kF}}},
     3,
     {{{"expressions", kE}}},
     1},
    {"aether.expr.trigger@1",
     "Expression Override",
     kBl,
     "Blend & Procedural",
     {{{"trigger", kEv}}},
     1,
     {{{"expressions", kE}, {"active", kF}}},
     2},
    {"aether.expr.auto_blink@1",
     "Auto Blink",
     kBl,
     "Blend & Procedural",
     {{{"face_conf", kF}, {"interval_s", kF}}},
     2,
     {{{"expressions", kE}, {"blink", kF}}},
     2},
    {"aether.expr.remap@1",
     "Blendshape Remap",
     kBl,
     "Blend & Procedural",
     {{{"expressions", kE}}},
     1,
     {{{"expressions", kE}}},
     1},
    {"aether.math.add@1", "Add", kBl, "Math & Logic", {{{"a", kF}, {"b", kF}}}, 2, {{{"out", kF}}}, 1},
    {"aether.math.multiply@1", "Multiply", kBl, "Math & Logic", {{{"a", kF}, {"b", kF}}}, 2, {{{"out", kF}}}, 1},
    {"aether.math.lerp@1", "Lerp", kBl, "Math & Logic", {{{"a", kF}, {"b", kF}, {"t", kF}}}, 3, {{{"out", kF}}}, 1},
    {"aether.math.clamp@1", "Clamp", kBl, "Math & Logic", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    {"aether.math.remap@1", "Remap Range", kBl, "Math & Logic", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    {"aether.math.curve@1", "Curve", kBl, "Math & Logic", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    {"aether.math.compare@1", "Compare", kBl, "Math & Logic", {{{"a", kF}, {"b", kF}}}, 2, {{{"out", kB}}}, 1},
    {"aether.math.select@1",
     "Select",
     kBl,
     "Math & Logic",
     {{{"cond", kB}, {"a", kF}, {"b", kF}}},
     3,
     {{{"out", kF}}},
     1},
    {"aether.math.and@1", "And", kBl, "Math & Logic", {{{"a", kB}, {"b", kB}}}, 2, {{{"out", kB}}}, 1},
    {"aether.math.not@1", "Not", kBl, "Math & Logic", {{{"in", kB}}}, 1, {{{"out", kB}}}, 1},
    {"aether.math.random@1", "Random", kBl, "Math & Logic", {{{"trigger", kEv}}}, 1, {{{"out", kF}}}, 1},
    {"aether.math.delay@1", "Delay (1 tick)", kBl, "Math & Logic", {{{"in", kF}}}, 1, {{{"out", kF}}}, 1},
    // Output sinks (royal purple)
    {"aether.output.pose@1", "Output Pose", kOut, "Outputs", {{{"pose", kP}}}, 1, {}, 0},
    {"aether.output.expressions@1", "Output Expressions", kOut, "Outputs", {{{"expressions", kE}}}, 1, {}, 0},
    {"aether.output.camera_control@1",
     "Camera Control",
     kOut,
     "Outputs",
     {{{"focal_mm", kF}, {"flatness", kF}}},
     2,
     {},
     0},
    {"aether.output.light_control@1",
     "Light Control",
     kOut,
     "Outputs",
     {{{"direction", kV}, {"intensity", kF}}},
     2,
     {},
     0},
    {"aether.output.osc_out@1", "OSC Out", kOut, "Outputs", {{{"value", kF}, {"trigger", kEv}}}, 2, {}, 0},
    // Phase 3: Non-Destructive Model Material & Texture Nodes
    {"aether.model.source@1",
     "Model Source",
     kSrc,
     "Model & Materials",
     {},
     0,
     {{{"skeleton_pose", kP},
       {"morph_bus", kE},
       {"material_table", PinDataKind::MaterialTable},
       {"material_count", kF}}},
     4},
    {"aether.material.slot_override@1",
     "Material Slot Override",
     kBl,
     "Model & Materials",
     {{{"material_table", PinDataKind::MaterialTable},
       {"texture", PinDataKind::TextureHandle},
       {"shade_shift", kF},
       {"sss_fringe", kF},
       {"outline_scale", kF},
       {"dynamic_driver", PinDataKind::MaterialOverride}}},
     6,
     {{{"override_out", PinDataKind::MaterialOverride}, {"slot_index", kF}}},
     2},
    {"aether.texture.asset@1",
     "Texture Asset",
     kSrc,
     "Model & Materials",
     {},
     0,
     {{{"texture", PinDataKind::TextureHandle}, {"ready", kB}}},
     2},
    {"aether.texture.grade@1",
     "Texture Grade",
     kBl,
     "Model & Materials",
     {{{"texture", PinDataKind::TextureHandle},
       {"hue_shift", kF},
       {"saturation", kF},
       {"brightness", kF},
       {"contrast", kF}}},
     5,
     {{{"texture", PinDataKind::TextureHandle}}},
     1},
    {"aether.material.dynamic_driver@1",
     "Dynamic Material Driver",
     kSol,
     "Model & Materials",
     {{{"signal_in", kF},
       {"audio_rms", kF},
       {"expression_weight", kF},
       {"circadian_clock", kF},
       {"material_table", PinDataKind::MaterialTable}}},
     5,
     {{{"driver_out", PinDataKind::MaterialOverride}, {"driven_value", kF}}},
     2},
}};

constexpr std::array<const char*, 7> kMenuGroups = {
    "Sources", "Model & Materials", "Solvers & Filters", "Blend & Procedural", "Math & Logic", "Outputs", nullptr};

std::string_view type_base(std::string_view type_id) noexcept {
    const size_t at = type_id.find('@');
    return at == std::string_view::npos ? type_id : type_id.substr(0, at);
}

const NodeTemplate* find_template(const char* type_id) noexcept {
    if (type_id == nullptr) {
        return nullptr;
    }
    const std::string_view base = type_base(type_id);
    for (const auto& tpl : kTemplates) {
        if (type_base(tpl.type_id) == base) {
            return &tpl;
        }
    }
    return nullptr;
}

/** Rust `default_param_for_input` fallbacks, shown in inline boxes until the user overrides them. */
float default_input_value(const char* pin_name) noexcept {
    const std::string_view n(pin_name);
    if (n == "gain" || n == "weight" || n == "rate_hz" || n == "intensity" || n == "outline_scale" ||
        n == "uv_scale_x" || n == "uv_scale_y" || n == "saturation" || n == "contrast") {
        return 1.0f;
    }
    if (n == "sss_fringe") {
        return 0.5f;
    }
    if (n == "max_val") {
        return 2.0f;
    }
    if (n == "amplitude") {
        return 0.05f;
    }
    if (n == "interval_s") {
        return 4.0f;
    }
    if (n == "focal_mm") {
        return 50.0f;
    }
    if (n == "flatness") {
        return 0.45f;
    }
    return 0.0f;
}

const char* category_label(NodeCategory c) noexcept {
    switch (c) {
    case NodeCategory::Source:
        return "Source";
    case NodeCategory::Solver:
        return "Solver";
    case NodeCategory::Output:
        return "Output";
    default:
        return "Blend";
    }
}

const GradientPair& category_gradient(const PrecisionDarkTokens& t, NodeCategory c) noexcept {
    switch (c) {
    case NodeCategory::Source:
        return t.node_hdr_source;
    case NodeCategory::Solver:
        return t.node_hdr_solver;
    case NodeCategory::Output:
        return t.node_hdr_output;
    default:
        return t.node_hdr_blend;
    }
}

void populate_ports_for_kind(GraphNodeVm& n) noexcept {
    const NodeTemplate* tpl = find_template(n.kind.data());
    n.num_inputs = 0;
    n.num_outputs = 0;
    if (tpl != nullptr) {
        n.category = tpl->category;
        for (uint8_t i = 0; i < tpl->n_in; ++i) {
            std::snprintf(n.inputs[i].name.data(), n.inputs[i].name.size(), "%s", tpl->in[i].name);
            n.inputs[i].kind = tpl->in[i].kind;
        }
        for (uint8_t i = 0; i < tpl->n_out; ++i) {
            std::snprintf(n.outputs[i].name.data(), n.outputs[i].name.size(), "%s", tpl->out[i].name);
            n.outputs[i].kind = tpl->out[i].kind;
        }
        n.num_inputs = tpl->n_in;
        n.num_outputs = tpl->n_out;
    } else {
        // Unknown / plugin node: generic Float in -> Float out (matches the Rust WASM fallback ports).
        n.category = NodeCategory::Blend;
        std::snprintf(n.inputs[0].name.data(), n.inputs[0].name.size(), "in");
        n.inputs[0].kind = PinDataKind::Float;
        std::snprintf(n.outputs[0].name.data(), n.outputs[0].name.size(), "out");
        n.outputs[0].kind = PinDataKind::Float;
        n.num_inputs = 1;
        n.num_outputs = 1;
    }
    for (uint8_t i = 0; i < n.num_inputs; ++i) {
        if (!n.input_value_set[i]) {
            n.input_values[i] = default_input_value(n.inputs[i].name.data());
        }
    }
    n.layout_font_px = 0.0f;
    const PrecisionDarkTokens& t = tokens();
    const float rows = static_cast<float>(std::max<uint8_t>(1, std::max(n.num_inputs, n.num_outputs)));
    n.h = t.node_header_h + rows * t.node_row_h + t.node_footer_h;
}

bool icase_match(const char* hay, const char* needle) noexcept {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }
    const size_t nlen = std::strlen(needle);
    for (const char* p = hay; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

ImVec2 eval_bezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float u) noexcept {
    const float inv = 1.0f - u;
    const float b0 = inv * inv * inv;
    const float b1 = 3.0f * inv * inv * u;
    const float b2 = 3.0f * inv * u * u;
    const float b3 = u * u * u;
    return ImVec2(b0 * p0.x + b1 * c0.x + b2 * c1.x + b3 * p1.x, b0 * p0.y + b1 * c0.y + b2 * c1.y + b3 * p1.y);
}

float dist_sq_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float wx = p.x - a.x;
    const float wy = p.y - a.y;
    const float c1 = vx * wx + vy * wy;
    if (c1 <= 0.0f) {
        return wx * wx + wy * wy;
    }
    const float c2 = vx * vx + vy * vy;
    if (c2 <= c1) {
        const float dx = p.x - b.x;
        const float dy = p.y - b.y;
        return dx * dx + dy * dy;
    }
    const float tt = c1 / std::max(1e-5f, c2);
    const float dx = p.x - (a.x + tt * vx);
    const float dy = p.y - (a.y + tt * vy);
    return dx * dx + dy * dy;
}

/** UE5 wire control points: horizontal tangents leaving the output / entering the input (`§3.3`). */
void wire_controls(ImVec2 p0, ImVec2 p3, float zs, ImVec2& c1, ImVec2& c2) noexcept {
    const float dx = std::max(std::fabs(p3.x - p0.x) * 0.48f, 55.0f * zs);
    c1 = ImVec2(p0.x + dx, p0.y);
    c2 = ImVec2(p3.x - dx, p3.y);
}

float wire_distance_sq(ImVec2 mouse, ImVec2 p0, ImVec2 p3, float zs) noexcept {
    ImVec2 c1;
    ImVec2 c2;
    wire_controls(p0, p3, zs, c1, c2);
    constexpr int kSubdiv = 24;
    float best = 1e18f;
    ImVec2 prev = p0;
    for (int s = 1; s <= kSubdiv; ++s) {
        const ImVec2 cur = eval_bezier(p0, c1, c2, p3, static_cast<float>(s) / static_cast<float>(kSubdiv));
        best = std::min(best, dist_sq_to_segment(mouse, prev, cur));
        prev = cur;
    }
    return best;
}

/** Typed pin glyph (`§3.2`): hollow outline with dark center when unconnected, solid when connected. */
void draw_pin_glyph(ImDrawList* dl, ImVec2 c, float r, PinDataKind kind, ImU32 col, bool connected) noexcept {
    const PrecisionDarkTokens& t = tokens();
    const float stroke = std::max(1.2f, r * 0.36f);
    switch (kind) {
    case PinDataKind::AudioFrame: {
        // Waveform-accented diamond for DSP AudioFrame
        const ImVec2 pts[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
        if (connected) {
            dl->AddConvexPolyFilled(pts, 4, col);
        } else {
            dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.5f);
        }
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x + r * 0.45f, c.y),
                    connected ? tokens().col32_pin_hollow_fill : col, 1.4f);
        break;
    }
    case PinDataKind::Pose:
    case PinDataKind::MaterialTable: {
        const float k = r * 1.25f;
        const ImVec2 pts[4]{ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x, c.y + k), ImVec2(c.x - k, c.y)};
        dl->AddConvexPolyFilled(pts, 4, connected ? col : t.col32_pin_hollow_fill);
        if (!connected) {
            dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, stroke);
        }
        break;
    }
    case PinDataKind::Expression:
    case PinDataKind::Viseme:
    case PinDataKind::MaterialOverride:
    case PinDataKind::TextureHandle: {
        const ImVec2 a(c.x - r, c.y - r);
        const ImVec2 b(c.x + r, c.y + r);
        dl->AddRectFilled(a, b, connected ? col : t.col32_pin_hollow_fill, r * 0.45f);
        if (!connected) {
            dl->AddRect(a, b, col, r * 0.45f, 0, stroke);
        }
        break;
    }
    case PinDataKind::Bool: {
        const float k = r * 0.85f;
        const ImVec2 a(c.x - k, c.y - k);
        const ImVec2 b(c.x + k, c.y + k);
        dl->AddRectFilled(a, b, connected ? col : t.col32_pin_hollow_fill, 1.0f);
        if (!connected) {
            dl->AddRect(a, b, col, 1.0f, 0, stroke);
        }
        break;
    }
    case PinDataKind::Event: {
        const ImVec2 pts[5]{ImVec2(c.x - r, c.y - r), ImVec2(c.x + r * 0.2f, c.y - r), ImVec2(c.x + r * 1.1f, c.y),
                            ImVec2(c.x + r * 0.2f, c.y + r), ImVec2(c.x - r, c.y + r)};
        dl->AddConvexPolyFilled(pts, 5, connected ? col : t.col32_pin_hollow_fill);
        if (!connected) {
            dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, stroke);
        }
        break;
    }
    default: // Float / Vec3: circle
        if (connected) {
            dl->AddCircleFilled(c, r, col, 16);
            dl->AddCircleFilled(c, r * 0.38f, t.col32_pin_hollow_fill, 10);
        } else {
            dl->AddCircleFilled(c, r, t.col32_pin_hollow_fill, 16);
            dl->AddCircle(c, r, col, 16, stroke);
        }
        break;
    }
}

ImU32 pin_color(PinDataKind k) noexcept {
    return tokens().col32_pin_types[static_cast<size_t>(k) % tokens().col32_pin_types.size()];
}

float text_w(ImFont* font, float size, const char* text) noexcept {
    return font != nullptr ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x : ImGui::CalcTextSize(text).x;
}

} // namespace

// =============================================================================================
// Pin type rules
// =============================================================================================

bool pin_kinds_compatible(PinDataKind from, PinDataKind to) noexcept {
    switch (to) {
    case PinDataKind::Float:
    case PinDataKind::Bool:
        return from == PinDataKind::Float || from == PinDataKind::Bool;
    case PinDataKind::Vec3:
        return from == PinDataKind::Vec3 || from == PinDataKind::Float;
    case PinDataKind::Expression:
        return from == PinDataKind::Expression || from == PinDataKind::Viseme;
    case PinDataKind::AudioFrame:
        return from == PinDataKind::AudioFrame;
    default:
        return from == to;
    }
}

const char* pin_kind_name(PinDataKind kind) noexcept {
    switch (kind) {
    case PinDataKind::Float:
        return "Float";
    case PinDataKind::Bool:
        return "Bool";
    case PinDataKind::Vec3:
        return "Vec3";
    case PinDataKind::Pose:
        return "PoseStream";
    case PinDataKind::Expression:
        return "Blendshape52";
    case PinDataKind::Viseme:
        return "Viseme5";
    case PinDataKind::Event:
        return "Event";
    case PinDataKind::AudioFrame:
        return "AudioFrame";
    case PinDataKind::MaterialTable:
        return "MaterialTable";
    case PinDataKind::MaterialOverride:
        return "MaterialOverride";
    case PinDataKind::TextureHandle:
        return "TextureHandle";
    }
    return "Float";
}

bool NodeCanvas::is_root_node_kind(const char* type_id) noexcept {
    if (type_id == nullptr) {
        return false;
    }
    const std::string_view base = type_base(type_id);
    return base == "aether.output.pose" || base == "aether.output.expressions";
}

NodeCategory NodeCanvas::category_for_kind(const char* type_id) noexcept {
    const NodeTemplate* tpl = find_template(type_id);
    return tpl != nullptr ? tpl->category : NodeCategory::Blend;
}

// =============================================================================================
// Model
// =============================================================================================

NodeCanvas::NodeCanvas() {
    nodes_.reserve(96);
    edges_.reserve(128);
    comments_.reserve(16);
    reset_default_graph();
}

int NodeCanvas::find_node_index(uint64_t node_id) const noexcept {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].id == node_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int NodeCanvas::find_link_index(uint64_t link_id) const noexcept {
    for (size_t i = 0; i < edges_.size(); ++i) {
        if (edges_[i].id == link_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const GraphNodeVm* NodeCanvas::find_node(uint64_t node_id) const noexcept {
    const int i = find_node_index(node_id);
    return i >= 0 ? &nodes_[static_cast<size_t>(i)] : nullptr;
}

const GraphEdgeVm* NodeCanvas::find_link(uint64_t link_id) const noexcept {
    const int i = find_link_index(link_id);
    return i >= 0 ? &edges_[static_cast<size_t>(i)] : nullptr;
}

const GraphEdgeVm* NodeCanvas::find_input_link(uint64_t node_id, uint8_t pin) const noexcept {
    for (const auto& e : edges_) {
        if (e.to_node == node_id && e.to_pin == pin) {
            return &e;
        }
    }
    return nullptr;
}

bool NodeCanvas::pin_has_link(const PinRef& pin) const noexcept {
    for (const auto& e : edges_) {
        if (pin.output ? (e.from_node == pin.node && e.from_pin == pin.pin)
                       : (e.to_node == pin.node && e.to_pin == pin.pin)) {
            return true;
        }
    }
    return false;
}

PinDataKind NodeCanvas::pin_kind(const PinRef& pin) const noexcept {
    const GraphNodeVm* n = find_node(pin.node);
    if (n == nullptr) {
        return PinDataKind::Float;
    }
    return pin.output ? n->outputs[std::min<size_t>(pin.pin, kMaxNodePins - 1)].kind
                      : n->inputs[std::min<size_t>(pin.pin, kMaxNodePins - 1)].kind;
}

void NodeCanvas::reset_default_graph() {
    nodes_.clear();
    edges_.clear();
    comments_.clear();
    selected_link_id_ = 0;
    watched_pin_count_ = 0;

    auto make_node = [this](uint64_t id, const char* kind, const char* title, float x, float y) {
        GraphNodeVm n{};
        n.id = id;
        std::snprintf(n.kind.data(), n.kind.size(), "%s", kind);
        std::snprintf(n.title.data(), n.title.size(), "%s", title);
        n.x = x;
        n.y = y;
        populate_ports_for_kind(n);
        nodes_.push_back(n);
    };

    // Matches `GraphDocument::default_project_graph()` (§7.4) on the Rust host.
    make_node(1, "aether.source.npu_pose@1", "NPU Pose In", 40.0f, 40.0f);
    make_node(2, "aether.ik.visibility_first@1", "Visibility-First IK", 330.0f, 40.0f);
    make_node(3, "aether.output.pose@1", "Output Pose", 640.0f, 40.0f);
    make_node(4, "aether.source.dsp_audio@1", "DSP Audio In", 40.0f, 300.0f);
    make_node(5, "aether.expr.formant_lipsync@1", "Formant Lip-Sync", 330.0f, 300.0f);
    make_node(6, "aether.output.expressions@1", "Output Expressions", 640.0f, 300.0f);

    edges_.push_back(GraphEdgeVm{10, 1, 0, 2, 0, PinDataKind::Pose});  // pose -> pose
    edges_.push_back(GraphEdgeVm{11, 1, 2, 2, 1, PinDataKind::Float}); // face_conf -> face_conf
    edges_.push_back(GraphEdgeVm{12, 1, 3, 2, 2, PinDataKind::Float}); // hand_conf -> hand_conf
    edges_.push_back(GraphEdgeVm{13, 1, 4, 2, 3, PinDataKind::Float}); // body_conf -> body_conf
    edges_.push_back(GraphEdgeVm{14, 2, 0, 3, 0, PinDataKind::Pose});
    edges_.push_back(GraphEdgeVm{15, 4, 0, 5, 0, PinDataKind::AudioFrame});
    edges_.push_back(GraphEdgeVm{16, 5, 0, 6, 0, PinDataKind::Viseme});

    comments_.push_back(GraphCommentVm{20, {"Tracking & Audio Rig"}, 20.0f, 4.0f, 860.0f, 470.0f, false});
    next_id_ = 100;
    last_committed_json_ = serialize_graph_json();
}

void NodeCanvas::load_sample_64_nodes() {
    nodes_.clear();
    edges_.clear();
    comments_.clear();
    selected_link_id_ = 0;

    uint64_t nid = 1;
    uint64_t eid = 1000;
    auto push_node = [&](const char* kind, const char* title, uint32_t slot) -> GraphNodeVm& {
        GraphNodeVm n{};
        n.id = nid++;
        std::snprintf(n.kind.data(), n.kind.size(), "%s", kind);
        std::snprintf(n.title.data(), n.title.size(), "%s", title);
        n.x = 20.0f + static_cast<float>(slot % 8U) * 230.0f;
        const uint32_t row = slot / 8U;
        n.y = 40.0f + static_cast<float>(row) * 140.0f;
        populate_ports_for_kind(n);
        nodes_.push_back(n);
        return nodes_.back();
    };

    push_node("aether.source.clock@1", "Studio Clock", 0);
    char title[48]{};
    for (uint32_t i = 1; i < 62; ++i) {
        const char* kind =
            (i % 3 == 0) ? "aether.math.smooth@1" : ((i % 2 == 0) ? "aether.math.clamp@1" : "aether.math.add@1");
        std::snprintf(title, sizeof(title), "Node #%u", i);
        const GraphNodeVm& n = push_node(kind, title, i);
        edges_.push_back(GraphEdgeVm{eid++, n.id - 1, 0, n.id, 0, PinDataKind::Float});
    }
    const uint64_t last_chain = nid - 1;
    const GraphNodeVm& cam = push_node("aether.output.camera_control@1", "Camera Out", 62);
    edges_.push_back(GraphEdgeVm{eid++, last_chain, 0, cam.id, 1, PinDataKind::Float});
    const GraphNodeVm& light = push_node("aether.output.light_control@1", "Light Out", 63);
    edges_.push_back(GraphEdgeVm{eid++, last_chain - 1, 0, light.id, 1, PinDataKind::Float});

    next_id_ = 2000;
    last_committed_json_ = serialize_graph_json();
}

void NodeCanvas::set_graph_json(const std::string& json_utf8) {
    if (json_utf8.empty()) {
        return;
    }
    const auto parsed = nlohmann::json::parse(json_utf8, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return;
    }
    const nlohmann::json& doc = parsed.contains("graph") && parsed["graph"].is_object() ? parsed["graph"] : parsed;
    if (!doc.contains("nodes") || !doc["nodes"].is_array()) {
        return;
    }

    nodes_.clear();
    edges_.clear();
    comments_.clear();
    selected_link_id_ = 0;
    interaction_ = Interaction::None;
    wire_has_detached_ = false;

    uint64_t max_seen = 100;
    for (const auto& jn : doc["nodes"]) {
        if (!jn.is_object() || !jn.contains("id")) {
            continue;
        }
        // Rust serializes `type`; documents from older UI builds used `type_id`.
        const char* type_key = jn.contains("type") ? "type" : (jn.contains("type_id") ? "type_id" : nullptr);
        if (type_key == nullptr || !jn[type_key].is_string()) {
            continue;
        }
        GraphNodeVm n{};
        n.id = jn.value("id", uint64_t{1});
        max_seen = std::max(max_seen, n.id + 1);
        const std::string tid = jn[type_key].get<std::string>();
        std::snprintf(n.kind.data(), n.kind.size(), "%s", tid.c_str());
        const NodeTemplate* tpl = find_template(tid.c_str());
        const std::string lbl = jn.contains("label") && jn["label"].is_string() ? jn["label"].get<std::string>()
                                : tpl != nullptr                                ? std::string(tpl->title)
                                                                                : tid;
        std::snprintf(n.title.data(), n.title.size(), "%s", lbl.c_str());
        if (jn.contains("position") && jn["position"].is_array() && jn["position"].size() >= 2 &&
            jn["position"][0].is_number() && jn["position"][1].is_number()) {
            n.x = jn["position"][0].get<float>();
            n.y = jn["position"][1].get<float>();
        }
        populate_ports_for_kind(n);
        if (jn.contains("params") && jn["params"].is_object()) {
            nlohmann::json extra = jn["params"];
            if (extra.contains("bypass") && extra["bypass"].is_boolean()) {
                n.bypassed = extra["bypass"].get<bool>();
            }
            extra.erase("bypass");
            if (extra.contains("param_id") && extra["param_id"].is_string()) {
                std::snprintf(n.param_id.data(), n.param_id.size(), "%s", extra["param_id"].get<std::string>().c_str());
            }
            extra.erase("param_id");
            for (uint8_t p = 0; p < n.num_inputs; ++p) {
                const char* pname = n.inputs[p].name.data();
                if (extra.contains(pname) && extra[pname].is_number()) {
                    n.input_values[p] = extra[pname].get<float>();
                    n.input_value_set[p] = true;
                    extra.erase(pname);
                }
            }
            if (!extra.empty()) {
                n.extra_params_json = extra.dump();
            }
        }
        if (jn.contains("ports") && jn["ports"].is_array() && !jn["ports"].empty()) {
            n.ports_json = jn["ports"].dump();
        }
        nodes_.push_back(std::move(n));
    }

    if (doc.contains("edges") && doc["edges"].is_array()) {
        for (const auto& je : doc["edges"]) {
            if (!je.is_object()) {
                continue;
            }
            GraphEdgeVm e{};
            e.id = je.value("id", uint64_t{0});
            e.from_node = je.value("from_node", uint64_t{0});
            e.to_node = je.value("to_node", uint64_t{0});
            const std::string fp = je.value("from_port", std::string{});
            const std::string tp = je.value("to_port", std::string{});
            const int fi = find_node_index(e.from_node);
            const int ti = find_node_index(e.to_node);
            bool from_ok = false;
            bool to_ok = false;
            if (fi >= 0) {
                const auto& fn = nodes_[static_cast<size_t>(fi)];
                std::string_view eff_fp = fp;
                const std::string_view fn_tid(fn.kind.data());
                if (fp == "visemes" && (fn_tid == "aether.source.dsp_audio@1" || fn_tid == "dsp_audio")) {
                    eff_fp = "audio";
                } else if (fp == "expressions" &&
                           (fn_tid == "aether.expr.formant_lipsync@1" || fn_tid == "formant_lipsync")) {
                    eff_fp = "visemes";
                }
                for (uint8_t p = 0; p < fn.num_outputs; ++p) {
                    if (eff_fp == fn.outputs[p].name.data() || fp == fn.outputs[p].name.data()) {
                        e.from_pin = p;
                        e.kind = fn.outputs[p].kind;
                        from_ok = true;
                        break;
                    }
                }
            }
            if (ti >= 0) {
                const auto& tn = nodes_[static_cast<size_t>(ti)];
                std::string_view eff_tp = tp;
                const std::string_view tn_tid(tn.kind.data());
                if (tp == "visemes" && (tn_tid == "aether.expr.formant_lipsync@1" || tn_tid == "formant_lipsync")) {
                    eff_tp = "audio";
                }
                for (uint8_t p = 0; p < tn.num_inputs; ++p) {
                    if (eff_tp == tn.inputs[p].name.data() || tp == tn.inputs[p].name.data()) {
                        e.to_pin = p;
                        to_ok = true;
                        break;
                    }
                }
            }
            if (!from_ok || !to_ok) {
                AETHER_LOG_WARN("ui", "Node graph: dropped edge %llu (%s -> %s): port not found",
                                static_cast<unsigned long long>(e.id), fp.c_str(), tp.c_str());
                continue;
            }
            if (e.id == 0) {
                e.id = max_seen++;
            }
            max_seen = std::max(max_seen, e.id + 1);
            edges_.push_back(e);
        }
    }

    auto read_box = [&](const nlohmann::json& jc, const char* title_key, bool sticky) {
        if (!jc.is_object()) {
            return;
        }
        GraphCommentVm c{};
        c.sticky = sticky;
        c.id = jc.value("id", uint64_t{0});
        if (c.id == 0) {
            c.id = max_seen++;
        }
        max_seen = std::max(max_seen, c.id + 1);
        const std::string txt = jc.contains(title_key) && jc[title_key].is_string() ? jc[title_key].get<std::string>()
                                                                                    : std::string{"Group"};
        std::snprintf(c.title.data(), c.title.size(), "%s", txt.c_str());
        if (jc.contains("position") && jc["position"].is_array() && jc["position"].size() >= 2) {
            c.x = jc["position"][0].get<float>();
            c.y = jc["position"][1].get<float>();
        }
        if (jc.contains("size") && jc["size"].is_array() && jc["size"].size() >= 2) {
            c.w = std::max(60.0f, jc["size"][0].get<float>());
            c.h = std::max(40.0f, jc["size"][1].get<float>());
        } else if (sticky) {
            c.w = 220.0f;
            c.h = 80.0f;
        }
        comments_.push_back(c);
    };
    if (doc.contains("groups") && doc["groups"].is_array()) {
        for (const auto& jg : doc["groups"]) {
            read_box(jg, "title", false);
        }
    }
    if (doc.contains("comments") && doc["comments"].is_array()) {
        for (const auto& jc : doc["comments"]) {
            read_box(jc, "text", true);
        }
    }

    next_id_ = std::max(next_id_, max_seen);
    prune_watched_pins();
    last_committed_json_ = serialize_graph_json();
}

std::string NodeCanvas::serialize_graph_json() const {
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json jnodes = nlohmann::json::array();
    for (const auto& n : nodes_) {
        nlohmann::json prms = nlohmann::json::object();
        if (!n.extra_params_json.empty()) {
            auto extra = nlohmann::json::parse(n.extra_params_json, nullptr, false);
            if (extra.is_object()) {
                prms = std::move(extra);
            }
        }
        prms["bypass"] = n.bypassed;
        if (n.param_id[0] != '\0') {
            prms["param_id"] = n.param_id.data();
        }
        for (uint8_t p = 0; p < n.num_inputs; ++p) {
            if (n.input_value_set[p]) {
                prms[n.inputs[p].name.data()] = n.input_values[p];
            }
        }
        nlohmann::json ports = nlohmann::json::array();
        if (!n.ports_json.empty()) {
            auto pj = nlohmann::json::parse(n.ports_json, nullptr, false);
            if (pj.is_array()) {
                ports = std::move(pj);
            }
        }
        jnodes.push_back({
            {"id", n.id},
            {"type", n.kind.data()},
            {"position", {n.x, n.y}},
            {"label", n.title.data()},
            {"params", std::move(prms)},
            {"ports", std::move(ports)},
        });
    }

    nlohmann::json jedges = nlohmann::json::array();
    for (const auto& e : edges_) {
        const int fi = find_node_index(e.from_node);
        const int ti = find_node_index(e.to_node);
        if (fi < 0 || ti < 0) {
            continue;
        }
        const auto& fn = nodes_[static_cast<size_t>(fi)];
        const auto& tn = nodes_[static_cast<size_t>(ti)];
        if (e.from_pin >= fn.num_outputs || e.to_pin >= tn.num_inputs) {
            continue;
        }
        jedges.push_back({
            {"id", e.id},
            {"from_node", e.from_node},
            {"from_port", fn.outputs[e.from_pin].name.data()},
            {"to_node", e.to_node},
            {"to_port", tn.inputs[e.to_pin].name.data()},
        });
    }

    nlohmann::json jgroups = nlohmann::json::array();
    nlohmann::json jcomments = nlohmann::json::array();
    for (const auto& c : comments_) {
        if (c.sticky) {
            jcomments.push_back(
                {{"id", c.id}, {"text", c.title.data()}, {"position", {c.x, c.y}}, {"size", {c.w, c.h}}});
            continue;
        }
        nlohmann::json members = nlohmann::json::array();
        for (const auto& n : nodes_) {
            if (n.x >= c.x && n.y >= c.y && n.x + n.w <= c.x + c.w && n.y + n.h <= c.y + c.h) {
                members.push_back(n.id);
            }
        }
        jgroups.push_back({{"id", c.id},
                           {"title", c.title.data()},
                           {"position", {c.x, c.y}},
                           {"size", {c.w, c.h}},
                           {"nodes", std::move(members)}});
    }

    root["nodes"] = std::move(jnodes);
    root["edges"] = std::move(jedges);
    root["groups"] = std::move(jgroups);
    root["comments"] = std::move(jcomments);
    return root.dump();
}

void NodeCanvas::commit_edit(const char* label, UndoStack& undo, StudioActions& actions) {
    std::string next_json = serialize_graph_json();
    if (next_json != last_committed_json_) {
        undo.push_graph_edit(label, last_committed_json_, next_json);
        last_committed_json_ = next_json;
    }
    // Every structural edit recompiles the runtime plan immediately (host debounces to <= 100 ms).
    actions.request_apply_graph_json = true;
    actions.pending_graph_json = std::move(next_json);
}

void NodeCanvas::add_exposed_param_node(const char* param_id) {
    if (param_id == nullptr || param_id[0] == '\0') {
        return;
    }
    const uint64_t id = add_node_no_commit("aether.math.add@1", 120.0f - pan_.x, 120.0f - pan_.y);
    const int idx = find_node_index(id);
    if (idx < 0) {
        return;
    }
    GraphNodeVm& n = nodes_[static_cast<size_t>(idx)];
    std::snprintf(n.title.data(), n.title.size(), "Param: %s", param_id);
    std::snprintf(n.param_id.data(), n.param_id.size(), "%s", param_id);
    n.input_values[1] = 0.0f;
    n.input_value_set[1] = true;
    n.layout_font_px = 0.0f;
}

uint64_t NodeCanvas::add_node_no_commit(const char* type_id, float world_x, float world_y) {
    if (type_id == nullptr || type_id[0] == '\0') {
        return 0;
    }
    GraphNodeVm n{};
    n.id = next_id_++;
    std::snprintf(n.kind.data(), n.kind.size(), "%s", type_id);
    const NodeTemplate* tpl = find_template(type_id);
    std::snprintf(n.title.data(), n.title.size(), "%s", tpl != nullptr ? tpl->title : type_id);
    n.x = world_x;
    n.y = world_y;
    populate_ports_for_kind(n);
    nodes_.push_back(n);
    return n.id;
}

uint64_t NodeCanvas::add_node(const char* type_id, float world_x, float world_y, UndoStack& undo,
                              StudioActions& actions) {
    const uint64_t id = add_node_no_commit(type_id, world_x, world_y);
    if (id != 0) {
        commit_edit("Add Node", undo, actions);
    }
    return id;
}

ConnectResult NodeCanvas::connect_no_commit(uint64_t from_node, uint8_t from_pin, uint64_t to_node, uint8_t to_pin,
                                            uint64_t* out_link_id) {
    if (from_node == to_node) {
        return ConnectResult::SameNode;
    }
    const int fi = find_node_index(from_node);
    const int ti = find_node_index(to_node);
    if (fi < 0 || ti < 0 || from_pin >= nodes_[static_cast<size_t>(fi)].num_outputs ||
        to_pin >= nodes_[static_cast<size_t>(ti)].num_inputs) {
        return ConnectResult::InvalidPin;
    }
    const PinDataKind from_kind = nodes_[static_cast<size_t>(fi)].outputs[from_pin].kind;
    const PinDataKind to_kind = nodes_[static_cast<size_t>(ti)].inputs[to_pin].kind;
    if (!pin_kinds_compatible(from_kind, to_kind)) {
        return ConnectResult::TypeMismatch;
    }
    bool replaced = false;
    for (size_t i = 0; i < edges_.size(); ++i) {
        GraphEdgeVm& e = edges_[i];
        if (e.to_node == to_node && e.to_pin == to_pin) {
            if (e.from_node == from_node && e.from_pin == from_pin) {
                if (out_link_id != nullptr) {
                    *out_link_id = e.id;
                }
                return ConnectResult::AlreadyLinked;
            }
            // Inputs accept exactly one driver (Rust compiler rule): the new wire replaces the old one.
            if (selected_link_id_ == e.id) {
                selected_link_id_ = 0;
            }
            edges_.erase(edges_.begin() + static_cast<std::ptrdiff_t>(i));
            replaced = true;
            break;
        }
    }
    const uint64_t id = next_id_++;
    edges_.push_back(GraphEdgeVm{id, from_node, from_pin, to_node, to_pin, from_kind});
    if (out_link_id != nullptr) {
        *out_link_id = id;
    }
    return replaced ? ConnectResult::Replaced : ConnectResult::Connected;
}

ConnectResult NodeCanvas::connect_pins(uint64_t from_node, uint8_t from_pin, uint64_t to_node, uint8_t to_pin,
                                       UndoStack& undo, StudioActions& actions, uint64_t* out_link_id) {
    const ConnectResult r = connect_no_commit(from_node, from_pin, to_node, to_pin, out_link_id);
    if (connect_succeeded(r)) {
        commit_edit("Connect Wire", undo, actions);
    }
    return r;
}

bool NodeCanvas::disconnect_link(uint64_t link_id, UndoStack& undo, StudioActions& actions) {
    const int li = find_link_index(link_id);
    if (li < 0) {
        return false;
    }
    edges_.erase(edges_.begin() + li);
    if (selected_link_id_ == link_id) {
        selected_link_id_ = 0;
    }
    commit_edit("Break Link", undo, actions);
    return true;
}

size_t NodeCanvas::disconnect_pin_links(const PinRef& pin, UndoStack& undo, StudioActions& actions) {
    const size_t before = edges_.size();
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const GraphEdgeVm& e) {
                                    const bool hit = pin.output ? (e.from_node == pin.node && e.from_pin == pin.pin)
                                                                : (e.to_node == pin.node && e.to_pin == pin.pin);
                                    if (hit && e.id == selected_link_id_) {
                                        selected_link_id_ = 0;
                                    }
                                    return hit;
                                }),
                 edges_.end());
    const size_t removed = before - edges_.size();
    if (removed > 0) {
        commit_edit(removed > 1 ? "Break Pin Links" : "Break Link", undo, actions);
    }
    return removed;
}

ConnectResult NodeCanvas::reroute_link(uint64_t link_id, uint64_t new_to_node, uint8_t new_to_pin, UndoStack& undo,
                                       StudioActions& actions) {
    const int li = find_link_index(link_id);
    if (li < 0) {
        return ConnectResult::InvalidPin;
    }
    const GraphEdgeVm old = edges_[static_cast<size_t>(li)];
    if (old.to_node == new_to_node && old.to_pin == new_to_pin) {
        return ConnectResult::AlreadyLinked;
    }
    edges_.erase(edges_.begin() + li);
    const ConnectResult r = connect_no_commit(old.from_node, old.from_pin, new_to_node, new_to_pin, nullptr);
    if (!connect_succeeded(r)) {
        edges_.insert(edges_.begin() + li, old); // restore on failure (type mismatch etc.)
        return r;
    }
    commit_edit("Re-route Wire", undo, actions);
    return r;
}

uint64_t NodeCanvas::insert_reroute_node(uint64_t link_id, UndoStack& undo, StudioActions& actions) {
    const int li = find_link_index(link_id);
    if (li < 0) {
        return 0;
    }
    const GraphEdgeVm old = edges_[static_cast<size_t>(li)];
    const GraphNodeVm* fn = find_node(old.from_node);
    const GraphNodeVm* tn = find_node(old.to_node);
    if (fn == nullptr || tn == nullptr) {
        return 0;
    }
    // Typed pass-through filters: each keeps the wire's value type end to end.
    const char* kind = nullptr;
    switch (old.kind) {
    case PinDataKind::Pose:
        kind = "aether.solver.foot_planting@1";
        break;
    case PinDataKind::Expression:
        kind = "aether.expr.remap@1";
        break;
    case PinDataKind::Float:
        kind = "aether.math.smooth@1";
        break;
    case PinDataKind::Bool:
        kind = "aether.math.not@1";
        break;
    default:
        return 0; // no single-in/single-out filter exists for Vec3 / Viseme / Event wires
    }
    const float mx = (fn->x + fn->w + tn->x) * 0.5f - 80.0f;
    const float my = (fn->y + tn->y) * 0.5f + 30.0f;
    const uint64_t rid = add_node_no_commit(kind, mx, my);
    edges_.erase(edges_.begin() + find_link_index(link_id));
    (void)connect_no_commit(old.from_node, old.from_pin, rid, 0, nullptr);
    (void)connect_no_commit(rid, 0, old.to_node, old.to_pin, nullptr);
    if (selected_link_id_ == link_id) {
        selected_link_id_ = 0;
    }
    commit_edit("Insert Filter Node", undo, actions);
    return rid;
}

size_t NodeCanvas::break_node_links(uint64_t node_id, UndoStack& undo, StudioActions& actions) {
    const size_t before = edges_.size();
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const GraphEdgeVm& e) { return e.from_node == node_id || e.to_node == node_id; }),
                 edges_.end());
    const size_t removed = before - edges_.size();
    if (removed > 0) {
        selected_link_id_ = find_link_index(selected_link_id_) >= 0 ? selected_link_id_ : 0;
        commit_edit("Break All Node Links", undo, actions);
    }
    return removed;
}

bool NodeCanvas::delete_node(uint64_t node_id, UndoStack& undo, StudioActions& actions) {
    const int ni = find_node_index(node_id);
    if (ni < 0 || is_root_node_kind(nodes_[static_cast<size_t>(ni)].kind.data())) {
        return false;
    }
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const GraphEdgeVm& e) { return e.from_node == node_id || e.to_node == node_id; }),
                 edges_.end());
    nodes_.erase(nodes_.begin() + ni);
    selected_link_id_ = find_link_index(selected_link_id_) >= 0 ? selected_link_id_ : 0;
    prune_watched_pins();
    commit_edit("Delete Node", undo, actions);
    return true;
}

bool NodeCanvas::set_node_bypassed(uint64_t node_id, bool bypassed, UndoStack& undo, StudioActions& actions) {
    const int ni = find_node_index(node_id);
    if (ni < 0 || nodes_[static_cast<size_t>(ni)].bypassed == bypassed) {
        return false;
    }
    nodes_[static_cast<size_t>(ni)].bypassed = bypassed;
    commit_edit(bypassed ? "Bypass Node" : "Enable Node", undo, actions);
    return true;
}

bool NodeCanvas::set_input_default(uint64_t node_id, uint8_t pin, float value, UndoStack& undo,
                                   StudioActions& actions) {
    const int ni = find_node_index(node_id);
    if (ni < 0 || pin >= nodes_[static_cast<size_t>(ni)].num_inputs || !std::isfinite(value)) {
        return false;
    }
    GraphNodeVm& n = nodes_[static_cast<size_t>(ni)];
    n.input_values[pin] = value;
    n.input_value_set[pin] = true;
    commit_edit("Edit Pin Default", undo, actions);
    return true;
}

bool NodeCanvas::reset_input_default(uint64_t node_id, uint8_t pin, UndoStack& undo, StudioActions& actions) {
    const int ni = find_node_index(node_id);
    if (ni < 0 || pin >= nodes_[static_cast<size_t>(ni)].num_inputs) {
        return false;
    }
    GraphNodeVm& n = nodes_[static_cast<size_t>(ni)];
    if (!n.input_value_set[pin]) {
        return false;
    }
    n.input_value_set[pin] = false;
    n.input_values[pin] = default_input_value(n.inputs[pin].name.data());
    commit_edit("Reset Pin Default", undo, actions);
    return true;
}

void NodeCanvas::select_link(uint64_t link_id) noexcept {
    selected_link_id_ = find_link_index(link_id) >= 0 ? link_id : 0;
}

void NodeCanvas::clear_selection() noexcept {
    for (auto& n : nodes_) {
        n.selected = false;
    }
    selected_link_id_ = 0;
}

bool NodeCanvas::is_pin_watched(const PinRef& pin) const noexcept {
    for (size_t i = 0; i < watched_pin_count_; ++i) {
        if (watched_pins_[i].node == pin.node && watched_pins_[i].pin == pin.pin &&
            watched_pins_[i].output == pin.output) {
            return true;
        }
    }
    return false;
}

bool NodeCanvas::set_pin_watched(const PinRef& pin, bool watched) noexcept {
    const GraphNodeVm* n = find_node(pin.node);
    if (n == nullptr) {
        return false;
    }
    const uint8_t limit = pin.output ? n->num_outputs : n->num_inputs;
    if (pin.pin >= limit) {
        return false;
    }
    for (size_t i = 0; i < watched_pin_count_; ++i) {
        if (watched_pins_[i].node == pin.node && watched_pins_[i].pin == pin.pin &&
            watched_pins_[i].output == pin.output) {
            if (!watched) {
                watched_pins_[i] = watched_pins_[--watched_pin_count_];
                return true;
            }
            return false;
        }
    }
    if (!watched || watched_pin_count_ >= kMaxWatchedPins) {
        return false;
    }
    watched_pins_[watched_pin_count_++] = pin;
    return true;
}

bool NodeCanvas::toggle_pin_watched(const PinRef& pin) noexcept {
    const bool currently_watched = is_pin_watched(pin);
    return set_pin_watched(pin, !currently_watched);
}

void NodeCanvas::prune_watched_pins() noexcept {
    size_t write = 0;
    for (size_t i = 0; i < watched_pin_count_; ++i) {
        const PinRef wp = watched_pins_[i];
        const GraphNodeVm* n = find_node(wp.node);
        if (n != nullptr && wp.pin < (wp.output ? n->num_outputs : n->num_inputs)) {
            watched_pins_[write++] = wp;
        }
    }
    watched_pin_count_ = write;
}

size_t NodeCanvas::format_watched_pin_value(const PinRef& pin, const StudioHostStatus& host_status, char* out,
                                            size_t cap) const noexcept {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    const GraphNodeVm* n = find_node(pin.node);
    if (n == nullptr) {
        return 0;
    }
    uint64_t src_node = pin.node;
    uint8_t src_pin = pin.pin;
    if (!pin.output) {
        if (const GraphEdgeVm* e = find_input_link(pin.node, pin.pin)) {
            src_node = e->from_node;
            src_pin = e->from_pin;
        } else {
            if (pin.pin < n->num_inputs && n->inputs[pin.pin].kind == PinDataKind::Float) {
                const int w = std::snprintf(out, cap, "%.3f", static_cast<double>(n->input_values[pin.pin]));
                return w > 0 ? std::min(static_cast<size_t>(w), cap - 1) : 0;
            }
            const int w = std::snprintf(out, cap, "--");
            return w > 0 ? std::min(static_cast<size_t>(w), cap - 1) : 0;
        }
    }

    uint8_t val_kind = AETHER_PIN_VALUE_INVALID;
    std::array<float, 4> vals{};
    if (!host_status.pin_readout_for(src_node, src_pin, val_kind, vals)) {
        const int w = std::snprintf(out, cap, "--");
        return w > 0 ? std::min(static_cast<size_t>(w), cap - 1) : 0;
    }

    int written = 0;
    switch (val_kind) {
    case AETHER_PIN_VALUE_FLOAT:
        written = std::snprintf(out, cap, "%.3f", static_cast<double>(vals[0]));
        break;
    case AETHER_PIN_VALUE_BOOL:
        written = std::snprintf(out, cap, "%s", vals[0] >= 0.5f ? "true" : "false");
        break;
    case AETHER_PIN_VALUE_EVENT:
        written = std::snprintf(out, cap, "%s", vals[0] >= 0.5f ? "fired" : "idle");
        break;
    case AETHER_PIN_VALUE_VEC3:
        written = std::snprintf(out, cap, "(%.2f, %.2f, %.2f)", static_cast<double>(vals[0]),
                                static_cast<double>(vals[1]), static_cast<double>(vals[2]));
        break;
    case AETHER_PIN_VALUE_QUAT:
        written =
            std::snprintf(out, cap, "(%.2f, %.2f, %.2f, %.2f)", static_cast<double>(vals[0]),
                          static_cast<double>(vals[1]), static_cast<double>(vals[2]), static_cast<double>(vals[3]));
        break;
    case AETHER_PIN_VALUE_TRANSFORM:
        written =
            std::snprintf(out, cap, "p(%.2f, %.2f, %.2f) s%.2f", static_cast<double>(vals[0]),
                          static_cast<double>(vals[1]), static_cast<double>(vals[2]), static_cast<double>(vals[3]));
        break;
    case AETHER_PIN_VALUE_POSE:
        written = std::snprintf(out, cap, "H(%.2f, %.2f) c=%.2f", static_cast<double>(vals[0]),
                                static_cast<double>(vals[1]), static_cast<double>(vals[3]));
        break;
    case AETHER_PIN_VALUE_EXPR: {
        const int active = static_cast<int>(std::lround(vals[1]));
        written = std::snprintf(out, cap, "max %.2f (%d)", static_cast<double>(vals[0]), active);
        break;
    }
    case AETHER_PIN_VALUE_VISEME: {
        static constexpr std::array<char, 5> kVisLabels{'A', 'I', 'U', 'E', 'O'};
        const size_t idx =
            std::min<size_t>(4, static_cast<size_t>(std::max(0, static_cast<int>(std::lround(vals[1])))));
        written = std::snprintf(out, cap, "%c %.2f", kVisLabels[idx], static_cast<double>(vals[0]));
        break;
    }
    case AETHER_PIN_VALUE_AUDIO:
        written = std::snprintf(out, cap, "%.1fdB F1:%.0f", static_cast<double>(vals[0]), static_cast<double>(vals[2]));
        break;
    default:
        written = std::snprintf(out, cap, "--");
        break;
    }
    return written > 0 ? std::min(static_cast<size_t>(written), cap - 1) : 0;
}

void NodeCanvas::duplicate_selected(UndoStack& undo, StudioActions& actions) {
    std::array<std::pair<uint64_t, uint64_t>, 96> remap{};
    size_t remap_n = 0;
    const size_t orig = nodes_.size();
    for (size_t i = 0; i < orig && remap_n < remap.size(); ++i) {
        if (!nodes_[i].selected || is_root_node_kind(nodes_[i].kind.data())) {
            continue;
        }
        GraphNodeVm dup = nodes_[i];
        dup.id = next_id_++;
        dup.x += 36.0f;
        dup.y += 36.0f;
        nodes_[i].selected = false;
        dup.selected = true;
        remap[remap_n++] = {nodes_[i].id, dup.id};
        nodes_.push_back(std::move(dup));
    }
    if (remap_n == 0) {
        return;
    }
    // Duplicate links whose both ends were duplicated (internal wiring is preserved).
    const size_t edge_cnt = edges_.size();
    auto mapped = [&](uint64_t id) -> uint64_t {
        for (size_t k = 0; k < remap_n; ++k) {
            if (remap[k].first == id) {
                return remap[k].second;
            }
        }
        return 0;
    };
    for (size_t i = 0; i < edge_cnt; ++i) {
        const GraphEdgeVm e = edges_[i];
        const uint64_t nf = mapped(e.from_node);
        const uint64_t nt = mapped(e.to_node);
        if (nf != 0 && nt != 0) {
            edges_.push_back(GraphEdgeVm{next_id_++, nf, e.from_pin, nt, e.to_pin, e.kind});
        }
    }
    commit_edit("Duplicate Node", undo, actions);
}

void NodeCanvas::delete_selected(UndoStack& undo, StudioActions& actions) {
    bool any = false;
    if (selected_link_id_ != 0) {
        const int li = find_link_index(selected_link_id_);
        if (li >= 0) {
            edges_.erase(edges_.begin() + li);
            any = true;
        }
        selected_link_id_ = 0;
    }
    const auto doomed = [](const GraphNodeVm& n) { return n.selected && !is_root_node_kind(n.kind.data()); };
    bool any_nodes = false;
    for (const auto& n : nodes_) {
        any_nodes = any_nodes || doomed(n);
    }
    if (any_nodes) {
        edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                    [&](const GraphEdgeVm& e) {
                                        const int fi = find_node_index(e.from_node);
                                        const int ti = find_node_index(e.to_node);
                                        return (fi >= 0 && doomed(nodes_[static_cast<size_t>(fi)])) ||
                                               (ti >= 0 && doomed(nodes_[static_cast<size_t>(ti)]));
                                    }),
                     edges_.end());
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), doomed), nodes_.end());
        any = true;
    }
    if (any) {
        commit_edit("Delete Selection", undo, actions);
    }
}

void NodeCanvas::wrap_selected_in_comment(UndoStack& undo, StudioActions& actions) {
    float min_x = 1e9f;
    float min_y = 1e9f;
    float max_x = -1e9f;
    float max_y = -1e9f;
    bool any = false;
    for (const auto& n : nodes_) {
        if (!n.selected) {
            continue;
        }
        any = true;
        min_x = std::min(min_x, n.x - 16.0f);
        min_y = std::min(min_y, n.y - 36.0f);
        max_x = std::max(max_x, n.x + n.w + 16.0f);
        max_y = std::max(max_y, n.y + n.h + 16.0f);
    }
    if (!any) {
        return;
    }
    GraphCommentVm c{};
    c.id = next_id_++;
    std::snprintf(c.title.data(), c.title.size(), "Node Group");
    c.x = min_x;
    c.y = min_y;
    c.w = std::max(140.0f, max_x - min_x);
    c.h = std::max(90.0f, max_y - min_y);
    comments_.push_back(c);
    commit_edit("Create Comment Box", undo, actions);
}

void NodeCanvas::toggle_bypass_selected(UndoStack& undo, StudioActions& actions) {
    bool any = false;
    for (auto& n : nodes_) {
        if (n.selected) {
            n.bypassed = !n.bypassed;
            any = true;
        }
    }
    if (any) {
        commit_edit("Toggle Bypass", undo, actions);
    }
}

// =============================================================================================
// Layout & hit-testing
// =============================================================================================

void NodeCanvas::layout_node(GraphNodeVm& n, const UiFontSet& fonts) const {
    const PrecisionDarkTokens& t = tokens();
    ImFont* body = fonts.body != nullptr ? fonts.body : ImGui::GetFont();
    ImFont* strong = fonts.body_strong != nullptr ? fonts.body_strong : body;
    ImFont* caption = fonts.caption != nullptr ? fonts.caption : body;
    const float body_px = body->FontSize;
    // Text is measured at dpi-scaled font size; world units are dpi-free (world * zoom * dpi = px).
    const float dpi = std::max(0.5f, body_px / t.type_body_px);
    const float label_px = body_px * 0.92f;

    const float title_w = text_w(strong, strong->FontSize, n.title.data()) / dpi;
    const float tag_w = text_w(caption, caption->FontSize, category_label(n.category)) / dpi;
    float in_w = 0.0f;
    for (uint8_t i = 0; i < n.num_inputs; ++i) {
        float w = text_w(body, label_px, n.inputs[i].name.data()) / dpi;
        if (n.inputs[i].kind == PinDataKind::Float) {
            w += t.node_inline_box_w + 6.0f;
        }
        in_w = std::max(in_w, w);
    }
    float out_w = 0.0f;
    for (uint8_t i = 0; i < n.num_outputs; ++i) {
        out_w = std::max(out_w, text_w(body, label_px, n.outputs[i].name.data()) / dpi);
    }
    const float header_need = 24.0f + title_w + 14.0f + tag_w + 10.0f;
    const float rows_need = t.node_label_inset + in_w + 20.0f + out_w + t.node_label_inset;
    const char* eval_sample = is_root_node_kind(n.kind.data()) ? "Root · Eval: 99.9 \xC2\xB5s" : "Eval: 99.9 \xC2\xB5s";
    const float footer_need = 24.0f + text_w(caption, caption->FontSize, n.bypassed ? "Bypassed" : "Active") / dpi +
                              12.0f + text_w(caption, caption->FontSize, eval_sample) / dpi + 10.0f;
    n.w = std::ceil(std::max({t.node_min_w, header_need, rows_need, footer_need}));
    const float rows = static_cast<float>(std::max<uint8_t>(1, std::max(n.num_inputs, n.num_outputs)));
    n.h = t.node_header_h + rows * t.node_row_h + t.node_footer_h;
    n.layout_font_px = body_px;
}

ImVec2 NodeCanvas::pin_screen_position(const PinRef& pin) const noexcept {
    const GraphNodeVm* n = find_node(pin.node);
    if (n == nullptr) {
        return ImVec2(0.0f, 0.0f);
    }
    const CanvasXform xf{last_canvas_origin_, zoom_ * last_dpi_, pan_};
    return pin_screen_pos(xf, *n, pin.output, pin.pin);
}

ImVec2 NodeCanvas::link_midpoint_screen(uint64_t link_id) const noexcept {
    const GraphEdgeVm* e = find_link(link_id);
    if (e == nullptr) {
        return ImVec2(0.0f, 0.0f);
    }
    const ImVec2 p0 = pin_screen_position(PinRef{e->from_node, e->from_pin, true});
    const ImVec2 p3 = pin_screen_position(PinRef{e->to_node, e->to_pin, false});
    ImVec2 c1;
    ImVec2 c2;
    wire_controls(p0, p3, zoom_ * last_dpi_, c1, c2);
    return eval_bezier(p0, c1, c2, p3, 0.5f);
}

ImVec2 NodeCanvas::pin_screen_pos(const CanvasXform& xf, const GraphNodeVm& n, bool output,
                                  uint8_t pin) const noexcept {
    const PrecisionDarkTokens& t = tokens();
    const float wx = output ? (n.x + n.w - t.node_pin_inset) : (n.x + t.node_pin_inset);
    const float wy = n.y + t.node_header_h + (static_cast<float>(pin) + 0.5f) * t.node_row_h;
    return xf.to_screen(wx, wy);
}

NodeCanvas::HoverInfo NodeCanvas::compute_hover(const CanvasXform& xf, ImVec2 mouse, float dpi) const {
    const PrecisionDarkTokens& t = tokens();
    HoverInfo h{};
    const float hit_r = std::max(6.0f, t.node_pin_hit_r * xf.zs);
    const float hit_r_sq = hit_r * hit_r;

    // Topmost first: selected nodes draw last, so test them first, then the rest back-to-front.
    for (int pass = 0; pass < 2 && h.node_idx < 0 && !h.pin_valid; ++pass) {
        for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
            const GraphNodeVm& n = nodes_[static_cast<size_t>(i)];
            if (n.selected != (pass == 0)) {
                continue;
            }
            const ImVec2 n_min = xf.to_screen(n.x, n.y);
            const ImVec2 n_max = xf.to_screen(n.x + n.w, n.y + n.h);
            if (mouse.x < n_min.x - hit_r || mouse.x > n_max.x + hit_r || mouse.y < n_min.y || mouse.y > n_max.y) {
                continue;
            }
            for (uint8_t p = 0; p < n.num_inputs; ++p) {
                const ImVec2 pp = pin_screen_pos(xf, n, false, p);
                const float dx = mouse.x - pp.x;
                const float dy = mouse.y - pp.y;
                if (dx * dx + dy * dy <= hit_r_sq) {
                    h.pin = PinRef{n.id, p, false};
                    h.pin_valid = true;
                    h.node_idx = i;
                    return h;
                }
            }
            for (uint8_t p = 0; p < n.num_outputs; ++p) {
                const ImVec2 pp = pin_screen_pos(xf, n, true, p);
                const float dx = mouse.x - pp.x;
                const float dy = mouse.y - pp.y;
                if (dx * dx + dy * dy <= hit_r_sq) {
                    h.pin = PinRef{n.id, p, true};
                    h.pin_valid = true;
                    h.node_idx = i;
                    return h;
                }
            }
            if (mouse.x >= n_min.x && mouse.x <= n_max.x) {
                h.node_idx = i;
                h.on_header = mouse.y <= n_min.y + t.node_header_h * xf.zs;
                return h;
            }
        }
    }

    // Wires (point-to-cubic distance, 24 subdivisions, 6.5 px * dpi) when not over a pin or node.
    const float wire_r = t.wire_hit_r * dpi;
    float best = wire_r * wire_r;
    for (const auto& e : edges_) {
        const int fi = find_node_index(e.from_node);
        const int ti = find_node_index(e.to_node);
        if (fi < 0 || ti < 0) {
            continue;
        }
        const ImVec2 p0 = pin_screen_pos(xf, nodes_[static_cast<size_t>(fi)], true, e.from_pin);
        const ImVec2 p3 = pin_screen_pos(xf, nodes_[static_cast<size_t>(ti)], false, e.to_pin);
        const float pad = 60.0f * xf.zs + wire_r;
        if (mouse.x < std::min(p0.x, p3.x) - pad || mouse.x > std::max(p0.x, p3.x) + pad ||
            mouse.y < std::min(p0.y, p3.y) - pad || mouse.y > std::max(p0.y, p3.y) + pad) {
            continue;
        }
        const float d = wire_distance_sq(mouse, p0, p3, xf.zs);
        if (d <= best) {
            best = d;
            h.link_id = e.id;
        }
    }
    return h;
}

void NodeCanvas::apply_frame_animation(ImVec2 canvas_size, float dpi) {
    if (frame_request_) {
        frame_request_ = false;
        if (nodes_.empty()) {
            frame_target_pan_ = ImVec2(24.0f, 24.0f);
            frame_target_zoom_ = 1.0f;
        } else {
            float min_x = 1e9f;
            float min_y = 1e9f;
            float max_x = -1e9f;
            float max_y = -1e9f;
            for (const auto& n : nodes_) {
                min_x = std::min(min_x, n.x);
                min_y = std::min(min_y, n.y);
                max_x = std::max(max_x, n.x + n.w);
                max_y = std::max(max_y, n.y + n.h);
            }
            const float margin = 40.0f * dpi;
            const float bw = std::max(1.0f, max_x - min_x);
            const float bh = std::max(1.0f, max_y - min_y);
            const float fit =
                std::min((canvas_size.x - margin * 2.0f) / bw, (canvas_size.y - margin * 2.0f) / bh) / dpi;
            frame_target_zoom_ = std::clamp(fit, 0.3f, 1.25f);
            const float zs = frame_target_zoom_ * dpi;
            frame_target_pan_ = ImVec2(canvas_size.x / (2.0f * zs) - (min_x + max_x) * 0.5f,
                                       canvas_size.y / (2.0f * zs) - (min_y + max_y) * 0.5f);
        }
        frame_animating_ = true;
    }
    if (!frame_animating_) {
        return;
    }
    // Critically-damped style exponential approach (~180 ms to settle).
    const float k = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f);
    pan_.x += (frame_target_pan_.x - pan_.x) * k;
    pan_.y += (frame_target_pan_.y - pan_.y) * k;
    zoom_ += (frame_target_zoom_ - zoom_) * k;
    if (std::fabs(frame_target_zoom_ - zoom_) < 0.001f && std::fabs(frame_target_pan_.x - pan_.x) < 0.25f &&
        std::fabs(frame_target_pan_.y - pan_.y) < 0.25f) {
        pan_ = frame_target_pan_;
        zoom_ = frame_target_zoom_;
        frame_animating_ = false;
    }
}

void NodeCanvas::restore_detached_link() {
    if (wire_has_detached_) {
        edges_.push_back(wire_detached_);
        wire_has_detached_ = false;
    }
}

// =============================================================================================
// Interaction
// =============================================================================================

void NodeCanvas::finish_wire_drop(const CanvasXform& xf, const HoverInfo& hov, float dpi, UndoStack& undo,
                                  StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const PinDataKind anchor_kind = pin_kind(wire_anchor_);

    // Magnetic snap: nearest opposite-direction compatible pin within 14 px * dpi.
    PinRef target{};
    bool have_target = false;
    if (hov.pin_valid && hov.pin.output != wire_anchor_.output) {
        target = hov.pin;
        have_target = true;
    } else {
        const float snap_sq = (t.node_snap_r * dpi) * (t.node_snap_r * dpi);
        float best = snap_sq;
        for (const auto& n : nodes_) {
            const uint8_t cnt = wire_anchor_.output ? n.num_inputs : n.num_outputs;
            for (uint8_t p = 0; p < cnt; ++p) {
                const PinDataKind k = wire_anchor_.output ? n.inputs[p].kind : n.outputs[p].kind;
                if (!(wire_anchor_.output ? pin_kinds_compatible(anchor_kind, k)
                                          : pin_kinds_compatible(k, anchor_kind))) {
                    continue;
                }
                const ImVec2 pp = pin_screen_pos(xf, n, !wire_anchor_.output, p);
                const float d = (pp.x - mouse.x) * (pp.x - mouse.x) + (pp.y - mouse.y) * (pp.y - mouse.y);
                if (d <= best) {
                    best = d;
                    target = PinRef{n.id, p, !wire_anchor_.output};
                    have_target = true;
                }
            }
        }
    }

    // Dropping on a node body auto-connects to its first compatible pin (UE5 behaviour).
    if (!have_target && hov.node_idx >= 0 && !hov.pin_valid) {
        const GraphNodeVm& n = nodes_[static_cast<size_t>(hov.node_idx)];
        const uint8_t cnt = wire_anchor_.output ? n.num_inputs : n.num_outputs;
        for (uint8_t p = 0; p < cnt && !have_target; ++p) {
            const PinDataKind k = wire_anchor_.output ? n.inputs[p].kind : n.outputs[p].kind;
            if (wire_anchor_.output ? pin_kinds_compatible(anchor_kind, k) : pin_kinds_compatible(k, anchor_kind)) {
                target = PinRef{n.id, p, !wire_anchor_.output};
                have_target = true;
            }
        }
        if (!have_target) {
            restore_detached_link();
            return;
        }
    }

    if (have_target) {
        const PinRef from = wire_anchor_.output ? wire_anchor_ : target;
        const PinRef to = wire_anchor_.output ? target : wire_anchor_;
        if (wire_has_detached_ && wire_detached_.to_node == to.node && wire_detached_.to_pin == to.pin) {
            restore_detached_link(); // dropped back onto the original input: no-op
            return;
        }
        const ConnectResult r = connect_no_commit(from.node, from.pin, to.node, to.pin, nullptr);
        if (connect_succeeded(r) || r == ConnectResult::AlreadyLinked) {
            const bool rerouted = wire_has_detached_;
            wire_has_detached_ = false;
            commit_edit(rerouted ? "Re-route Wire" : "Connect Wire", undo, actions);
        } else {
            restore_detached_link(); // type mismatch: the red badge already explained why
        }
        return;
    }

    // Released over empty canvas: open the context-sensitive spawn menu. A detached wire is already
    // removed; the deletion is committed when the menu resolves (Disconnect / dismiss / spawn).
    spawn_from_wire_ = true;
    spawn_pending_detach_commit_ = wire_has_detached_;
    wire_has_detached_ = false;
    spawn_world_pos_ = xf.to_world(mouse);
    add_filter_[0] = '\0';
    ImGui::OpenPopup("##SpawnFromWire");
}

void NodeCanvas::handle_mouse(const CanvasXform& xf, const HoverInfo& hov, bool canvas_hovered, float dpi,
                              UndoStack& undo, UiSelection& selection, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const float drag_px = t.wire_detach_threshold * dpi;
    const auto dist_from_press = [&]() {
        const float dx = mouse.x - press_pos_.x;
        const float dy = mouse.y - press_pos_.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    const auto select_node = [&](GraphNodeVm& n) {
        n.selected = true;
        selection.kind = SelectionKind::Node;
        selection.item_index = static_cast<int32_t>(n.id);
        std::snprintf(selection.name.data(), selection.name.size(), "%s", n.title.data());
    };

    // Zoom about the cursor
    if (canvas_hovered && std::fabs(io.MouseWheel) > 1e-4f) {
        const ImVec2 before = xf.to_world(mouse);
        zoom_ = std::clamp(zoom_ * std::exp(io.MouseWheel * 0.12f), 0.3f, 2.0f);
        const float zs = zoom_ * dpi;
        pan_.x = (mouse.x - xf.origin.x) / zs - before.x;
        pan_.y = (mouse.y - xf.origin.y) / zs - before.y;
        frame_animating_ = false;
    }

    // ---- Press ----
    if (canvas_hovered && interaction_ == Interaction::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            press_pos_ = mouse;
            if (io.KeyAlt) {
                // Mechanic 1: Alt+click breaks every link on a pin, or the clicked wire.
                if (hov.pin_valid) {
                    (void)disconnect_pin_links(hov.pin, undo, actions);
                } else if (hov.link_id != 0) {
                    (void)disconnect_link(hov.link_id, undo, actions);
                }
                return;
            }
            if (hov.pin_valid) {
                if (!hov.pin.output && pin_has_link(hov.pin)) {
                    interaction_ = Interaction::PressInputPin; // Mechanic 2 (detach after threshold)
                    press_pin_ = hov.pin;
                } else {
                    interaction_ = Interaction::Wiring;
                    wire_anchor_ = hov.pin;
                    wire_has_detached_ = false;
                }
            } else if (hov.node_idx >= 0) {
                GraphNodeVm& n = nodes_[static_cast<size_t>(hov.node_idx)];
                if (io.KeyCtrl) {
                    n.selected = !n.selected;
                } else if (!n.selected) {
                    if (!io.KeyShift) {
                        clear_selection();
                    }
                    select_node(n);
                } else {
                    select_node(n);
                }
                interaction_ = Interaction::PressNode;
                nodes_moved_ = false;
            } else if (hov.link_id != 0) {
                // Mechanic 4: click selects a wire (amber); double-click inserts a filter node.
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    (void)insert_reroute_node(hov.link_id, undo, actions);
                } else {
                    if (!io.KeyShift) {
                        for (auto& n : nodes_) {
                            n.selected = false;
                        }
                    }
                    selected_link_id_ = hov.link_id;
                }
            } else {
                if (!io.KeyShift && !io.KeyCtrl) {
                    clear_selection();
                }
                interaction_ = Interaction::BoxSelect;
                box_start_ = mouse;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            press_pos_ = mouse;
            interaction_ = Interaction::PressRightCanvas;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            interaction_ = Interaction::PanCanvas;
            frame_animating_ = false;
        }
    }

    // ---- Drag ----
    switch (interaction_) {
    case Interaction::PressNode:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dist_from_press() > drag_px) {
            interaction_ = Interaction::DragNodes;
        }
        break;
    case Interaction::DragNodes:
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
            for (auto& n : nodes_) {
                if (n.selected) {
                    n.x += io.MouseDelta.x / xf.zs;
                    n.y += io.MouseDelta.y / xf.zs;
                    nodes_moved_ = true;
                }
            }
        }
        break;
    case Interaction::PressInputPin:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dist_from_press() > drag_px) {
            const GraphEdgeVm* link = find_input_link(press_pin_.node, press_pin_.pin);
            if (link != nullptr) {
                wire_detached_ = *link;
                wire_has_detached_ = true;
                wire_anchor_ = PinRef{link->from_node, link->from_pin, true};
                if (selected_link_id_ == link->id) {
                    selected_link_id_ = 0;
                }
                edges_.erase(edges_.begin() + find_link_index(link->id));
                interaction_ = Interaction::Wiring;
            } else {
                interaction_ = Interaction::None;
            }
        }
        break;
    case Interaction::PressRightCanvas:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && dist_from_press() > drag_px) {
            interaction_ = Interaction::PanCanvas;
            frame_animating_ = false;
        }
        break;
    case Interaction::PanCanvas:
        pan_.x += io.MouseDelta.x / xf.zs;
        pan_.y += io.MouseDelta.y / xf.zs;
        break;
    default:
        break;
    }

    if (interaction_ == Interaction::Wiring && KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape, false)) {
        restore_detached_link();
        interaction_ = Interaction::None;
        return;
    }

    // ---- Release ----
    switch (interaction_) {
    case Interaction::PressNode:
    case Interaction::PressInputPin:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction_ = Interaction::None;
        }
        break;
    case Interaction::DragNodes:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction_ = Interaction::None;
            if (nodes_moved_) {
                commit_edit("Move Node", undo, actions);
            }
        }
        break;
    case Interaction::Wiring:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction_ = Interaction::None;
            finish_wire_drop(xf, hov, dpi, undo, actions);
        }
        break;
    case Interaction::BoxSelect:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            interaction_ = Interaction::None;
            const ImVec2 a(std::min(box_start_.x, mouse.x), std::min(box_start_.y, mouse.y));
            const ImVec2 b(std::max(box_start_.x, mouse.x), std::max(box_start_.y, mouse.y));
            if (b.x - a.x > 2.0f || b.y - a.y > 2.0f) {
                for (auto& n : nodes_) {
                    const ImVec2 n_min = xf.to_screen(n.x, n.y);
                    const ImVec2 n_max = xf.to_screen(n.x + n.w, n.y + n.h);
                    if (n_max.x >= a.x && n_min.x <= b.x && n_max.y >= a.y && n_min.y <= b.y) {
                        select_node(n);
                    }
                }
            }
        }
        break;
    case Interaction::PressRightCanvas:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            interaction_ = Interaction::None;
            // Mechanic 3: context menus on pins, wires, nodes; empty canvas opens Add Node.
            if (hov.pin_valid) {
                ctx_pin_ = hov.pin;
                open_pin_ctx_ = true;
            } else if (hov.node_idx >= 0) {
                ctx_node_id_ = nodes_[static_cast<size_t>(hov.node_idx)].id;
                open_node_ctx_ = true;
            } else if (hov.link_id != 0) {
                ctx_link_id_ = hov.link_id;
                selected_link_id_ = hov.link_id;
                open_link_ctx_ = true;
            } else {
                spawn_world_pos_ = xf.to_world(mouse);
                spawn_from_wire_ = false;
                open_canvas_ctx_ = true;
            }
        }
        break;
    case Interaction::PanCanvas:
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            interaction_ = Interaction::None;
        }
        break;
    default:
        break;
    }
}

void NodeCanvas::handle_keyboard(const CanvasXform& xf, const KeymapRegistry& keymap, UndoStack& undo,
                                 StudioActions& actions) {
    if (ImGui::GetIO().WantTextInput || interaction_ == Interaction::Wiring) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool no_mods = !io.KeyCtrl && !io.KeyAlt && !io.KeyShift;
    if (keymap.is_pressed(ActionId::GraphDelete) ||
        (no_mods && KeymapRegistry::is_raw_key_pressed(ImGuiKey_Backspace, false))) {
        delete_selected(undo, actions);
    } else if (keymap.is_pressed(ActionId::GraphDuplicate)) {
        duplicate_selected(undo, actions);
    } else if (keymap.is_pressed(ActionId::GraphComment)) {
        wrap_selected_in_comment(undo, actions);
    } else if (keymap.is_pressed(ActionId::GraphBypass) ||
               (no_mods && KeymapRegistry::is_raw_key_pressed(ImGuiKey_M, false))) {
        toggle_bypass_selected(undo, actions);
    } else if (keymap.is_pressed(ActionId::GraphFrame)) {
        frame_all();
    } else if (keymap.is_pressed(ActionId::GraphRecompile)) {
        actions.request_recompile_graph_now = true;
    } else if (keymap.is_pressed(ActionId::GraphAddNode) ||
               (no_mods && KeymapRegistry::is_raw_key_pressed(ImGuiKey_Space, false))) {
        spawn_world_pos_ = xf.to_world(io.MousePos);
        open_canvas_ctx_ = true;
        spawn_from_wire_ = false;
    } else if (no_mods && KeymapRegistry::is_raw_key_pressed(ImGuiKey_Escape, false)) {
        clear_selection();
    }
}

// =============================================================================================
// Rendering
// =============================================================================================

void NodeCanvas::draw_node(ImDrawList* dl, const CanvasXform& xf, GraphNodeVm& n, bool hovered, const UiFontSet& fonts,
                           const StudioHostStatus& host_status, float dpi, UndoStack& undo, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float zs = xf.zs;
    const ImVec2 n_min = xf.to_screen(n.x, n.y);
    const ImVec2 n_max = xf.to_screen(n.x + n.w, n.y + n.h);
    const float r = t.radius_node_px * zs;
    const float hdr_h = t.node_header_h * zs;
    const float foot_h = t.node_footer_h * zs;
    const bool is_err = n.has_error || (host_status.error_node_id != 0 && host_status.error_node_id == n.id);

    // Body + soft drop shadow
    dl->AddRectFilled(ImVec2(n_min.x + 2.0f * zs, n_min.y + 4.0f * zs),
                      ImVec2(n_max.x + 2.0f * zs, n_max.y + 5.0f * zs), t.col32_node_shadow, r);
    dl->AddRectFilled(n_min, n_max, t.col32_node_body, r);

    // Category gradient header banner (left saturated -> right dark tint) + 1 px separator
    const GradientPair& grad = category_gradient(t, n.category);
    const ImVec2 hdr_max(n_max.x, n_min.y + hdr_h);
    const int vtx0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(n_min, hdr_max, grad.left, r, ImDrawFlags_RoundCornersTop);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, dl->VtxBuffer.Size, n_min, ImVec2(n_max.x, n_min.y),
                                                  grad.left, grad.right);
    dl->AddLine(ImVec2(n_min.x, hdr_max.y), ImVec2(n_max.x, hdr_max.y), t.col32_node_header_sep, 1.0f);

    // Footer strip
    dl->AddRectFilled(ImVec2(n_min.x, n_max.y - foot_h), n_max, t.col32_node_footer, r, ImDrawFlags_RoundCornersBottom);

    ImFont* body = fonts.body != nullptr ? fonts.body : ImGui::GetFont();
    ImFont* strong = fonts.body_strong != nullptr ? fonts.body_strong : body;
    ImFont* caption = fonts.caption != nullptr ? fonts.caption : body;
    const bool show_text = zoom_ >= 0.45f;
    const float z = zoom_;

    if (show_text) {
        // Title row: category diamond, title, right-aligned category tag
        const float title_px = strong->FontSize * z;
        const float ty = n_min.y + (hdr_h - title_px) * 0.5f;
        const ImVec2 dc(n_min.x + 12.0f * zs, n_min.y + hdr_h * 0.5f);
        const float dk = 4.0f * zs;
        dl->AddQuadFilled(ImVec2(dc.x, dc.y - dk), ImVec2(dc.x + dk, dc.y), ImVec2(dc.x, dc.y + dk),
                          ImVec2(dc.x - dk, dc.y), t.col32_text_primary);
        dl->AddText(strong, title_px, ImVec2(n_min.x + 22.0f * zs, ty),
                    n.bypassed ? t.col32_text_secondary : t.col32_text_primary, n.title.data());
        const float cap_px = caption->FontSize * z;
        const char* tag = category_label(n.category);
        const float tag_w = text_w(caption, cap_px, tag);
        dl->AddText(caption, cap_px, ImVec2(n_max.x - 10.0f * zs - tag_w, n_min.y + (hdr_h - cap_px) * 0.5f),
                    col32_with_alpha(t.col32_text_primary, 0x99), tag);
    }

    // Pin rows: labeled inputs left, right-aligned labeled outputs right (fixed 24 px rows)
    const float pin_r = std::max(2.5f, t.node_pin_r * zs);
    const float label_px = body->FontSize * 0.92f * z;
    const bool inline_widgets = z >= 0.75f && z <= 1.6f;
    for (uint8_t p = 0; p < n.num_inputs; ++p) {
        const ImVec2 pp = pin_screen_pos(xf, n, false, p);
        const bool linked = find_input_link(n.id, p) != nullptr;
        draw_pin_glyph(dl, pp, pin_r, n.inputs[p].kind, pin_color(n.inputs[p].kind), linked);
        if (!show_text) {
            continue;
        }
        const float lx = n_min.x + t.node_label_inset * zs;
        dl->AddText(body, label_px, ImVec2(lx, pp.y - label_px * 0.5f), t.col32_text_secondary,
                    n.inputs[p].name.data());
        if (!linked && n.inputs[p].kind == PinDataKind::Float) {
            const float bx = lx + text_w(body, label_px, n.inputs[p].name.data()) + 6.0f * zs;
            const float bw = t.node_inline_box_w * zs;
            const float bh = std::min(t.node_row_h * zs - 4.0f * zs, ImGui::GetFrameHeight());
            if (inline_widgets) {
                ImGui::SetCursorScreenPos(ImVec2(bx, pp.y - bh * 0.5f));
                ImGui::PushID(static_cast<int>(n.id * 8U + p));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                    ImVec2(3.0f * zs, std::max(0.0f, (bh - ImGui::GetFontSize()) * 0.5f)));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, t.bg_inset);
                ImGui::SetNextItemWidth(bw);
                float v = n.input_values[p];
                if (ImGui::DragFloat("##pin_default", &v, 0.01f, 0.0f, 0.0f, "%.2f")) {
                    n.input_values[p] = v;
                    n.input_value_set[p] = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    commit_edit("Edit Pin Default", undo, actions);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s default · drag to scrub, double-click to type", n.inputs[p].name.data());
                }
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                ImGui::PopID();
            } else {
                char vb[24]{};
                std::snprintf(vb, sizeof(vb), "[%.2f]", static_cast<double>(n.input_values[p]));
                dl->AddText(body, label_px, ImVec2(bx, pp.y - label_px * 0.5f), t.col32_text_tertiary, vb);
            }
        }
    }
    for (uint8_t p = 0; p < n.num_outputs; ++p) {
        const ImVec2 pp = pin_screen_pos(xf, n, true, p);
        draw_pin_glyph(dl, pp, pin_r, n.outputs[p].kind, pin_color(n.outputs[p].kind),
                       pin_has_link(PinRef{n.id, p, true}));
        if (show_text) {
            const float w = text_w(body, label_px, n.outputs[p].name.data());
            dl->AddText(body, label_px, ImVec2(n_max.x - t.node_label_inset * zs - w, pp.y - label_px * 0.5f),
                        t.col32_text_secondary, n.outputs[p].name.data());
        }
    }

    // Footer: live compile status + per-node Eval timing (`§7.2`)
    if (show_text) {
        const char* status = "Active";
        ImU32 dot = t.col32_status_ok;
        if (is_err) {
            status = "Compile Error";
            dot = t.col32_status_error;
        } else if (n.bypassed) {
            status = "Bypassed";
            dot = t.col32_status_off;
        } else if (host_status.compiled_node_count == 0) {
            status = "Not compiled";
            dot = t.col32_status_warn;
        }
        const float cap_px = caption->FontSize * z;
        const float fy = n_max.y - foot_h * 0.5f;
        dl->AddCircleFilled(ImVec2(n_min.x + 12.0f * zs, fy), 2.8f * zs, dot, 10);
        dl->AddText(caption, cap_px, ImVec2(n_min.x + 20.0f * zs, fy - cap_px * 0.5f), t.col32_text_tertiary, status);

        float node_us = 0.0f;
        const bool has_eval = host_status.node_eval_us_for(n.id, node_us);
        char right_buf[48]{};
        if (has_eval) {
            if (is_root_node_kind(n.kind.data())) {
                std::snprintf(right_buf, sizeof(right_buf), "Root · Eval: %.1f \xC2\xB5s",
                              static_cast<double>(node_us));
            } else {
                std::snprintf(right_buf, sizeof(right_buf), "Eval: %.1f \xC2\xB5s", static_cast<double>(node_us));
            }
        } else if (is_root_node_kind(n.kind.data())) {
            std::snprintf(right_buf, sizeof(right_buf), "Root");
        } else {
            std::snprintf(right_buf, sizeof(right_buf), "Eval: -- \xC2\xB5s");
        }
        const float rw = text_w(caption, cap_px, right_buf);
        const ImU32 rcol = has_eval ? t.col32_text_secondary : t.col32_text_tertiary;
        dl->AddText(caption, cap_px, ImVec2(n_max.x - 10.0f * zs - rw, fy - cap_px * 0.5f), rcol, right_buf);
    }

    if (n.bypassed) {
        dl->AddRectFilled(n_min, n_max, t.col32_node_bypass_tint, r);
    }

    // Border: 1.5 px default, brighter on hover, 2 px amber when selected, red on compile error
    const ImU32 border = is_err       ? t.col32_status_error
                         : n.selected ? t.col32_node_selected
                         : hovered    ? t.col32_node_border_hover
                                      : t.col32_node_border;
    dl->AddRect(n_min, n_max, border, r, 0, (n.selected || is_err ? 2.0f : 1.5f) * dpi);
}

void NodeCanvas::draw_add_node_menu(UndoStack& undo, StudioActions& actions) {
    ImGui::SetNextItemWidth(220.0f * (ImGui::GetFontSize() / tokens().type_body_px));
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##nf", tr("Search node types..."), add_filter_.data(), add_filter_.size());
    ImGui::Separator();

    const PinDataKind anchor_kind = spawn_from_wire_ ? pin_kind(wire_anchor_) : PinDataKind::Float;
    auto fits_anchor = [&](const NodeTemplate& tpl, uint8_t& pin_out) {
        if (!spawn_from_wire_) {
            return true;
        }
        const uint8_t cnt = wire_anchor_.output ? tpl.n_in : tpl.n_out;
        for (uint8_t p = 0; p < cnt; ++p) {
            const PinDataKind k = wire_anchor_.output ? tpl.in[p].kind : tpl.out[p].kind;
            if (wire_anchor_.output ? pin_kinds_compatible(anchor_kind, k) : pin_kinds_compatible(k, anchor_kind)) {
                pin_out = p;
                return true;
            }
        }
        return false;
    };
    auto spawn = [&](const NodeTemplate& tpl, uint8_t pin) {
        const uint64_t id = add_node_no_commit(tpl.type_id, spawn_world_pos_.x, spawn_world_pos_.y);
        if (spawn_from_wire_ && id != 0) {
            if (wire_anchor_.output) {
                (void)connect_no_commit(wire_anchor_.node, wire_anchor_.pin, id, pin, nullptr);
            } else {
                (void)connect_no_commit(id, pin, wire_anchor_.node, wire_anchor_.pin, nullptr);
            }
        }
        commit_edit(spawn_pending_detach_commit_ ? "Re-route to New Node"
                    : spawn_from_wire_           ? "Add Connected Node"
                                                 : "Add Node",
                    undo, actions);
        spawn_pending_detach_commit_ = false;
        spawn_from_wire_ = false;
        ImGui::CloseCurrentPopup();
    };

    const bool filtering = add_filter_[0] != '\0';
    for (const char* group : kMenuGroups) {
        if (group == nullptr) {
            break;
        }
        bool any = false;
        uint8_t dummy = 0;
        for (const auto& tpl : kTemplates) {
            any = any || (std::strcmp(tpl.menu_group, group) == 0 && fits_anchor(tpl, dummy) &&
                          (icase_match(tpl.title, add_filter_.data()) || icase_match(tpl.type_id, add_filter_.data())));
        }
        if (!any) {
            continue;
        }
        const bool open = filtering ? true : ImGui::BeginMenu(group);
        if (filtering) {
            ImGui::TextColored(tokens().text_tertiary, "%s", group);
        }
        if (open) {
            for (const auto& tpl : kTemplates) {
                uint8_t pin = 0;
                if (std::strcmp(tpl.menu_group, group) != 0 || !fits_anchor(tpl, pin) ||
                    !(icase_match(tpl.title, add_filter_.data()) || icase_match(tpl.type_id, add_filter_.data()))) {
                    continue;
                }
                if (ImGui::MenuItem(tpl.title)) {
                    spawn(tpl, pin);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetTooltip("%s", tpl.type_id);
                }
            }
            if (!filtering) {
                ImGui::EndMenu();
            }
        }
    }
}

void NodeCanvas::draw_node_extra_controls(GraphNodeVm& n, float field_w, UndoStack& undo, StudioActions& actions) {
    const std::string_view kbase = type_base(n.kind.data());
    auto parse_extra = [&]() -> nlohmann::json {
        if (n.extra_params_json.empty()) {
            return nlohmann::json::object();
        }
        auto j = nlohmann::json::parse(n.extra_params_json, nullptr, false);
        return j.is_object() ? j : nlohmann::json::object();
    };
    if (kbase == "aether.material.slot_override") {
        nlohmann::json ex = parse_extra();
        bool changed = false;
        char slot_buf[64]{};
        std::snprintf(slot_buf, sizeof(slot_buf), "%s", ex.value("slot_name", std::string{"*"}).c_str());
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputText(tr("Material Slot"), slot_buf, sizeof(slot_buf))) {
            ex["slot_name"] = std::string(slot_buf);
            changed = true;
        }
        static constexpr std::array<const char*, 8> kProfiles = {
            "keep",        "2.5D Skin SDF", "Dual-Lobe Hair", "Micro-PBR Cloth",
            "Eye Caustic", "Metallic",      "Glass",          "Emissive"};
        const std::string cur_prof = ex.value("shader_profile", std::string{"keep"});
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::BeginCombo(tr("Shader Profile"), cur_prof.c_str())) {
            for (const char* p : kProfiles) {
                const bool sel = (cur_prof == p);
                if (ImGui::Selectable(p, sel)) {
                    ex["shader_profile"] = std::string(p);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        float sc[3]{0.72f, 0.75f, 0.85f};
        if (ex.contains("shade_color") && ex["shade_color"].is_array() && ex["shade_color"].size() >= 3) {
            sc[0] = ex["shade_color"][0].get<float>();
            sc[1] = ex["shade_color"][1].get<float>();
            sc[2] = ex["shade_color"][2].get<float>();
        }
        if (ImGui::ColorEdit3(tr("Shade Color"), sc, ImGuiColorEditFlags_NoInputs)) {
            ex["shade_color"] = {sc[0], sc[1], sc[2], 1.0f};
            changed = true;
        }
        uint32_t rt_mask = ex.value("rt_shadow_mask", 0x7u);
        bool rt_hair = (rt_mask & 1u) != 0u;
        bool rt_hands = (rt_mask & 2u) != 0u;
        bool rt_self = (rt_mask & 4u) != 0u;
        const bool rt_hair_changed = ImGui::Checkbox(tr("RT Hair Shadow"), &rt_hair);
        const bool rt_hand_changed = ImGui::Checkbox(tr("RT Hand Shadow"), &rt_hands);
        const bool rt_self_changed = ImGui::Checkbox(tr("RT Self Shadow"), &rt_self);
        if (rt_hair_changed || rt_hand_changed || rt_self_changed) {
            rt_mask = (rt_hair ? 1u : 0u) | (rt_hands ? 2u : 0u) | (rt_self ? 4u : 0u);
            ex["rt_shadow_mask"] = rt_mask;
            changed = true;
        }
        if (changed) {
            n.extra_params_json = ex.dump();
            commit_edit("Edit Material Slot Override", undo, actions);
        }
        ImGui::Separator();
    } else if (kbase == "aether.texture.asset") {
        nlohmann::json ex = parse_extra();
        bool changed = false;
        char path_buf[256]{};
        std::snprintf(path_buf, sizeof(path_buf), "%s", ex.value("path", std::string{}).c_str());
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::InputText(tr("Texture Path"), path_buf, sizeof(path_buf))) {
            ex["path"] = std::string(path_buf);
            changed = true;
        }
        bool is_srgb = (ex.value("color_space", std::string{"srgb"}) != "linear");
        if (ImGui::Checkbox(tr("sRGB Color Space"), &is_srgb)) {
            ex["color_space"] = is_srgb ? "srgb" : "linear";
            changed = true;
        }
        bool is_spout = ex.value("use_spout2", ex.value("spout_enabled", false));
        if (ImGui::Checkbox(tr("Spout2 Live Receiver"), &is_spout)) {
            ex["use_spout2"] = is_spout;
            changed = true;
        }
        static constexpr std::array<const char*, 6> kTexSlots = {"albedo", "shade", "normal",
                                                                 "matcap", "toon",  "emissive"};
        const std::string cur_slot = ex.value("target_slot", std::string{"albedo"});
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::BeginCombo(tr("Target Slot"), cur_slot.c_str())) {
            for (const char* s : kTexSlots) {
                if (ImGui::Selectable(s, cur_slot == s)) {
                    ex["target_slot"] = std::string(s);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (changed) {
            n.extra_params_json = ex.dump();
            commit_edit("Edit Texture Asset Node", undo, actions);
        }
        ImGui::Separator();
    } else if (kbase == "aether.texture.grade") {
        nlohmann::json ex = parse_extra();
        float tint[3]{1.0f, 1.0f, 1.0f};
        if (ex.contains("tint") && ex["tint"].is_array() && ex["tint"].size() >= 3) {
            tint[0] = ex["tint"][0].get<float>();
            tint[1] = ex["tint"][1].get<float>();
            tint[2] = ex["tint"][2].get<float>();
        }
        if (ImGui::ColorEdit3(tr("Tint Multiply"), tint, ImGuiColorEditFlags_NoInputs)) {
            ex["tint"] = {tint[0], tint[1], tint[2], 1.0f};
            n.extra_params_json = ex.dump();
            commit_edit("Edit Texture Grade Tint", undo, actions);
        }
        ImGui::Separator();
    } else if (kbase == "aether.material.dynamic_driver") {
        nlohmann::json ex = parse_extra();
        bool changed = false;
        static constexpr std::array<const char*, 5> kSignals = {"audio_rms", "circadian_time", "blink_l", "mouth_open",
                                                                "joy"};
        static constexpr std::array<const char*, 3> kTargets = {"emissive_intensity", "blush_alpha", "rim_boost"};
        const std::string cur_sig = ex.value("signal_source", std::string{"audio_rms"});
        const std::string cur_tgt = ex.value("target_param", std::string{"emissive_intensity"});
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::BeginCombo(tr("Signal Source"), cur_sig.c_str())) {
            for (const char* s : kSignals) {
                if (ImGui::Selectable(s, cur_sig == s)) {
                    ex["signal_source"] = std::string(s);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::BeginCombo(tr("Target Param"), cur_tgt.c_str())) {
            for (const char* tg : kTargets) {
                if (ImGui::Selectable(tg, cur_tgt == tg)) {
                    ex["target_param"] = std::string(tg);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (changed) {
            n.extra_params_json = ex.dump();
            commit_edit("Edit Dynamic Material Driver", undo, actions);
        }
        ImGui::Separator();
    }
}

void NodeCanvas::draw_selected_node_inspector(uint64_t preferred_node_id, const UiFontSet& fonts, UndoStack& undo,
                                              StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    int ni = (preferred_node_id != 0) ? find_node_index(preferred_node_id) : -1;
    if (ni < 0) {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].selected) {
                ni = static_cast<int>(i);
                break;
            }
        }
    }
    if (ni < 0) {
        ImGui::TextColored(t.text_tertiary, "%s", tr("Select a node in the Node Graph canvas to inspect parameters."));
        return;
    }

    GraphNodeVm& n = nodes_[static_cast<size_t>(ni)];
    const bool root = is_root_node_kind(n.kind.data());
    ImGui::PushID(static_cast<int>(n.id & 0x7FFFFFFFull));

    ImGui::TextColored(t.text_primary, "%s", n.title.data());
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::TextColored(t.text_tertiary, "%s", n.kind.data());
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
    if (n.param_id[0] != '\0') {
        ImGui::TextColored(t.text_secondary, "Bound Param: %s", n.param_id.data());
    }
    ImGui::Separator();

    bool bypass = n.bypassed;
    if (ImGui::Checkbox(tr("Bypass / Mute Node"), &bypass)) {
        (void)set_node_bypassed(n.id, bypass, undo, actions);
    }

    const float field_w = std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.56f);
    draw_node_extra_controls(n, field_w, undo, actions);

    bool any_pin_shown = false;
    for (uint8_t p = 0; p < n.num_inputs && p < kMaxNodePins; ++p) {
        const auto& pin = n.inputs[p];
        if (pin.kind != PinDataKind::Float && pin.kind != PinDataKind::Bool) {
            continue;
        }
        any_pin_shown = true;
        const bool wired = (find_input_link(n.id, p) != nullptr);
        ImGui::PushID(static_cast<int>(p));
        if (wired) {
            ImGui::BeginDisabled(true);
            ImGui::LabelText(pin.name.data(), "%s", tr("<Wired>"));
            ImGui::EndDisabled();
        } else if (pin.kind == PinDataKind::Bool) {
            bool bval = (n.input_values[p] >= 0.5f);
            if (ImGui::Checkbox(pin.name.data(), &bval)) {
                n.input_values[p] = bval ? 1.0f : 0.0f;
                n.input_value_set[p] = true;
                commit_edit("Edit Node Input", undo, actions);
            }
        } else {
            float fval = n.input_values[p];
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::DragFloat(pin.name.data(), &fval, 0.02f, -100.0f, 100.0f, "%.3f")) {
                n.input_values[p] = fval;
                n.input_value_set[p] = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                n.input_value_set[p] = true;
                commit_edit("Edit Node Input", undo, actions);
            }
        }
        ImGui::PopID();
    }
    if (!any_pin_shown) {
        ImGui::TextColored(t.text_tertiary, "%s", tr("No editable scalar input pins."));
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Recompile Graph"), ImVec2(-FLT_MIN, 0.0f))) {
        actions.pending_graph_json = serialize_graph_json();
        actions.request_apply_graph_json = true;
        actions.request_recompile_graph_now = true;
    }
    if (!root) {
        if (ImGui::Button(tr("Break All Node Links"), ImVec2(-FLT_MIN, 0.0f))) {
            (void)break_node_links(n.id, undo, actions);
        }
    }
    ImGui::PopID();
}

void NodeCanvas::draw_context_menus(const CanvasXform& xf, UndoStack& undo, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    if (open_pin_ctx_) {
        ImGui::OpenPopup("##PinCtx");
        open_pin_ctx_ = false;
    }
    if (open_link_ctx_) {
        ImGui::OpenPopup("##LinkCtx");
        open_link_ctx_ = false;
    }
    if (open_node_ctx_) {
        ImGui::OpenPopup("##NodeCtx");
        open_node_ctx_ = false;
    }
    if (open_canvas_ctx_ || open_add_popup_) {
        if (open_add_popup_) {
            // Toolbar "+ Add Node": spawn at the visible canvas center.
            spawn_world_pos_ = xf.to_world(ImVec2(last_canvas_origin_.x + last_canvas_size_.x * 0.5f,
                                                  last_canvas_origin_.y + last_canvas_size_.y * 0.5f));
            spawn_from_wire_ = false;
        }
        add_filter_[0] = '\0';
        ImGui::OpenPopup("##AddNodePopup");
        open_canvas_ctx_ = false;
        open_add_popup_ = false;
    }

    // Pin menu: Break Link(s) (per connection), Break All, Reset Default Value
    if (ImGui::BeginPopup("##PinCtx")) {
        const GraphNodeVm* n = find_node(ctx_pin_.node);
        if (n == nullptr) {
            ImGui::CloseCurrentPopup();
        } else {
            const GraphPinVm& pin = ctx_pin_.output ? n->outputs[ctx_pin_.pin] : n->inputs[ctx_pin_.pin];
            ImGui::TextColored(t.text_primary, "%s", pin.name.data());
            ImGui::SameLine();
            ImGui::TextColored(t.text_tertiary, "%s %s", ctx_pin_.output ? "out" : "in", pin_kind_name(pin.kind));
            ImGui::Separator();
            size_t link_cnt = 0;
            for (const auto& e : edges_) {
                link_cnt += (ctx_pin_.output ? (e.from_node == n->id && e.from_pin == ctx_pin_.pin)
                                             : (e.to_node == n->id && e.to_pin == ctx_pin_.pin))
                                ? 1U
                                : 0U;
            }
            if (ImGui::BeginMenu(tr("Break Link(s)"), link_cnt > 0)) {
                uint64_t break_id = 0;
                for (const auto& e : edges_) {
                    const bool mine = ctx_pin_.output ? (e.from_node == n->id && e.from_pin == ctx_pin_.pin)
                                                      : (e.to_node == n->id && e.to_pin == ctx_pin_.pin);
                    if (!mine) {
                        continue;
                    }
                    const GraphNodeVm* other = find_node(ctx_pin_.output ? e.to_node : e.from_node);
                    if (other == nullptr) {
                        continue;
                    }
                    const char* other_pin =
                        ctx_pin_.output ? other->inputs[e.to_pin].name.data() : other->outputs[e.from_pin].name.data();
                    char lbl[112]{};
                    std::snprintf(lbl, sizeof(lbl), "%s %s.%s##brk%llu", ctx_pin_.output ? "->" : "<-",
                                  other->title.data(), other_pin, static_cast<unsigned long long>(e.id));
                    if (ImGui::MenuItem(lbl)) {
                        break_id = e.id;
                    }
                }
                ImGui::EndMenu();
                if (break_id != 0) {
                    (void)disconnect_link(break_id, undo, actions);
                }
            }
            if (ImGui::MenuItem(tr("Break All Pin Links"), "Alt+Click", false, link_cnt > 0)) {
                (void)disconnect_pin_links(ctx_pin_, undo, actions);
            }
            const bool can_reset = !ctx_pin_.output && link_cnt == 0 && n->input_value_set[ctx_pin_.pin];
            if (ImGui::MenuItem(tr("Reset Default Value"), nullptr, false, can_reset)) {
                (void)reset_input_default(n->id, ctx_pin_.pin, undo, actions);
            }
            const bool watching = is_pin_watched(ctx_pin_);
            if (ImGui::MenuItem(tr("Watch Pin Value"), nullptr, watching)) {
                (void)toggle_pin_watched(ctx_pin_);
            }
        }
        ImGui::EndPopup();
    }

    // Wire menu: Break Link, Insert Reroute / Filter Node
    if (ImGui::BeginPopup("##LinkCtx")) {
        const GraphEdgeVm* e = find_link(ctx_link_id_);
        if (e == nullptr) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextColored(t.text_tertiary, "%s wire", pin_kind_name(e->kind));
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Break Link"), "Alt+Click / Del")) {
                (void)disconnect_link(ctx_link_id_, undo, actions);
            }
            const bool can_filter = e->kind == PinDataKind::Pose || e->kind == PinDataKind::Expression ||
                                    e->kind == PinDataKind::Float || e->kind == PinDataKind::Bool;
            if (ImGui::MenuItem(tr("Insert Reroute / Filter Node"), "Double-Click", false, can_filter)) {
                (void)insert_reroute_node(ctx_link_id_, undo, actions);
            }
        }
        ImGui::EndPopup();
    }

    // Node menu: Break All Node Links, Bypass / Mute, Duplicate, Delete (non-root)
    if (ImGui::BeginPopup("##NodeCtx")) {
        const int ni = find_node_index(ctx_node_id_);
        if (ni < 0) {
            ImGui::CloseCurrentPopup();
        } else {
            GraphNodeVm& n = nodes_[static_cast<size_t>(ni)];
            const bool root = is_root_node_kind(n.kind.data());
            ImGui::TextColored(t.text_primary, "%s", n.title.data());
            ImGui::TextColored(t.text_tertiary, "%s", n.kind.data());
            ImGui::Separator();

            draw_node_extra_controls(n, 160.0f, undo, actions);

            if (ImGui::MenuItem(tr("Break All Node Links"))) {
                (void)break_node_links(ctx_node_id_, undo, actions);
            }
            if (ImGui::MenuItem(tr("Bypass / Mute Node"), "M", n.bypassed)) {
                (void)set_node_bypassed(ctx_node_id_, !n.bypassed, undo, actions);
            }
            if (ImGui::MenuItem(tr("Duplicate Node"), "Ctrl+D", false, !root)) {
                clear_selection();
                nodes_[static_cast<size_t>(ni)].selected = true;
                duplicate_selected(undo, actions);
            }
            if (ImGui::MenuItem(tr("Delete Node"), "Del", false, !root)) {
                (void)delete_node(ctx_node_id_, undo, actions);
            }
            if (root && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", tr("Root output nodes cannot be deleted"));
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AddNodePopup")) {
        draw_add_node_menu(undo, actions);
        ImGui::EndPopup();
    }

    // Spawn menu opened by dropping a wire on empty canvas
    if (ImGui::BeginPopup("##SpawnFromWire")) {
        if (spawn_pending_detach_commit_) {
            char lbl[48]{};
            std::snprintf(lbl, sizeof(lbl), "%s  %s", icons::kClose, tr("Disconnect Wire"));
            if (ImGui::MenuItem(lbl)) {
                spawn_pending_detach_commit_ = false;
                spawn_from_wire_ = false;
                commit_edit("Disconnect Wire", undo, actions);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
        }
        if (spawn_from_wire_) {
            draw_add_node_menu(undo, actions);
        }
        ImGui::EndPopup();
    } else if (spawn_from_wire_ || spawn_pending_detach_commit_) {
        // Menu dismissed without a choice: a detached wire stays disconnected (committed as one step).
        if (spawn_pending_detach_commit_) {
            commit_edit("Disconnect Wire", undo, actions);
        }
        spawn_pending_detach_commit_ = false;
        spawn_from_wire_ = false;
    }
}

void NodeCanvas::draw(const StudioHostStatus& host_status, UndoStack& undo, const KeymapRegistry& keymap,
                      const UiFontSet& fonts, float dpi_scale, UiSelection& selection, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float dpi = std::max(0.75f, dpi_scale);

    // ---- Toolbar ----
    if (ImGui::Button(tr("+ Add Node"))) {
        open_add_popup_ = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tr("Categorized node menu (Tab / Space / right-click canvas)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Recompile"))) {
        actions.request_recompile_graph_now = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tr("Send the graph to the host compiler now (Ctrl+Enter)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Frame"))) {
        frame_all();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tr("Pan & zoom to fit all nodes (F)"));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("64-Node Stress"))) {
        const std::string before = last_committed_json_;
        load_sample_64_nodes();
        undo.push_graph_edit("Load 64-Node Stress Graph", before, last_committed_json_);
        actions.request_apply_graph_json = true;
        actions.pending_graph_json = last_committed_json_;
        frame_all();
    }
    ImGui::SameLine();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(host_status.eval_budget_warn ? t.status_warn : t.text_secondary,
                       "Nodes %zu · Links %zu · Plan %u · Compile %.2f ms · Eval %.1f us", nodes_.size(), edges_.size(),
                       host_status.compiled_node_count, static_cast<double>(host_status.last_compile_ms),
                       static_cast<double>(host_status.last_eval_us));
    if (fonts.mono != nullptr) {
        ImGui::PopFont();
    }
    if (host_status.error_node_id != 0 || host_status.graph_error_msg[0] != '\0') {
        ImGui::TextColored(t.status_error, "%s Graph error (node %llu): %s", icons::kErrorCircle,
                           static_cast<unsigned long long>(host_status.error_node_id),
                           host_status.graph_error_msg.data());
    }

    // ---- Canvas surface ----
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvas_size(std::max(120.0f, avail.x), std::max(120.0f, avail.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##GraphCanvasSurface", canvas_size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_max(origin.x + canvas_size.x, origin.y + canvas_size.y);
    last_canvas_origin_ = origin;
    last_canvas_size_ = canvas_size;
    last_dpi_ = dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Canvas-wide input surface first; inline pin widgets submitted later may overlap it.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##GraphCanvasInput", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    // Hovered only when no inline pin widget sits on top and no other widget owns the mouse.
    const bool canvas_hovered = ImGui::IsItemHovered() && (!ImGui::IsAnyItemActive() || ImGui::IsItemActive());
    const bool graph_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    apply_frame_animation(canvas_size, dpi);
    for (auto& n : nodes_) {
        if (n.layout_font_px != (fonts.body != nullptr ? fonts.body->FontSize : ImGui::GetFontSize())) {
            layout_node(n, fonts);
        }
    }

    CanvasXform xf{origin, zoom_ * dpi, pan_};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool mouse_in_canvas =
        mouse.x >= origin.x && mouse.x <= canvas_max.x && mouse.y >= origin.y && mouse.y <= canvas_max.y;
    const HoverInfo hov = mouse_in_canvas ? compute_hover(xf, mouse, dpi) : HoverInfo{};
    hovered_link_id_ = hov.link_id;

    handle_mouse(xf, hov, canvas_hovered || interaction_ != Interaction::None, dpi, undo, selection, actions);
    if ((graph_focused || canvas_hovered) && interaction_ != Interaction::Wiring) {
        handle_keyboard(xf, keymap, undo, actions);
    }
    xf = CanvasXform{origin, zoom_ * dpi, pan_}; // pan/zoom may have changed this frame

    dl->PushClipRect(origin, canvas_max, true);
    dl->AddRectFilled(origin, canvas_max, t.col32_bg_canvas);

    // Grid: minor every 16 world units, major every 128 (UE5 style), culled to the canvas
    const float minor = 16.0f * xf.zs;
    if (minor >= 6.0f) {
        const float off_x = std::fmod(pan_.x * xf.zs, minor);
        const float off_y = std::fmod(pan_.y * xf.zs, minor);
        for (float x = origin.x + off_x; x < canvas_max.x; x += minor) {
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, canvas_max.y), t.col32_grid_minor);
        }
        for (float y = origin.y + off_y; y < canvas_max.y; y += minor) {
            dl->AddLine(ImVec2(origin.x, y), ImVec2(canvas_max.x, y), t.col32_grid_minor);
        }
    }
    const float major = 128.0f * xf.zs;
    {
        const float off_x = std::fmod(pan_.x * xf.zs, major);
        const float off_y = std::fmod(pan_.y * xf.zs, major);
        for (float x = origin.x + off_x; x < canvas_max.x; x += major) {
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, canvas_max.y), t.col32_grid_major);
        }
        for (float y = origin.y + off_y; y < canvas_max.y; y += major) {
            dl->AddLine(ImVec2(origin.x, y), ImVec2(canvas_max.x, y), t.col32_grid_major);
        }
    }

    ImFont* body = fonts.body != nullptr ? fonts.body : ImGui::GetFont();
    ImFont* strong = fonts.body_strong != nullptr ? fonts.body_strong : body;

    if (nodes_.empty()) {
        const char* msg = tr("Graph is empty - press Tab or right-click to add a node");
        const float w = text_w(body, body->FontSize, msg);
        dl->AddText(body, body->FontSize,
                    ImVec2(origin.x + (canvas_size.x - w) * 0.5f, origin.y + canvas_size.y * 0.45f),
                    t.col32_text_secondary, msg);
    }

    // Comment / group boxes (background layer)
    for (const auto& c : comments_) {
        const ImVec2 p0 = xf.to_screen(c.x, c.y);
        const ImVec2 p1 = xf.to_screen(c.x + c.w, c.y + c.h);
        if (p1.x < origin.x || p0.x > canvas_max.x || p1.y < origin.y || p0.y > canvas_max.y) {
            continue;
        }
        const float r = t.radius_node_px * xf.zs;
        const float hh = 24.0f * xf.zs;
        dl->AddRectFilled(p0, p1, t.col32_comment_body, r);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + hh), t.node_hdr_comment.left, r, ImDrawFlags_RoundCornersTop);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p0, ImVec2(p1.x, p0.y),
                                                      t.node_hdr_comment.left, t.node_hdr_comment.right);
        dl->AddRect(p0, p1, t.col32_comment_border, r, 0, 1.0f);
        if (zoom_ >= 0.4f) {
            const float px = strong->FontSize * zoom_;
            dl->AddText(strong, px, ImVec2(p0.x + 10.0f * xf.zs, p0.y + (hh - px) * 0.5f), t.col32_text_secondary,
                        c.title.data());
        }
    }

    // Wires: 25% shadow spline + 2.6 px core (3.8 px hovered/selected), amber when selected,
    // animated pulse while the compiled plan evaluates. No arrowheads into input labels.
    const bool evaluating = host_status.compiled_node_count > 0 && host_status.error_node_id == 0;
    const float time_s = static_cast<float>(ImGui::GetTime());
    for (const auto& e : edges_) {
        const int fi = find_node_index(e.from_node);
        const int ti = find_node_index(e.to_node);
        if (fi < 0 || ti < 0) {
            continue;
        }
        const ImVec2 p0 = pin_screen_pos(xf, nodes_[static_cast<size_t>(fi)], true, e.from_pin);
        const ImVec2 p3 = pin_screen_pos(xf, nodes_[static_cast<size_t>(ti)], false, e.to_pin);
        const float pad = 60.0f * xf.zs;
        if (std::max(p0.x, p3.x) + pad < origin.x || std::min(p0.x, p3.x) - pad > canvas_max.x ||
            std::max(p0.y, p3.y) + pad < origin.y || std::min(p0.y, p3.y) - pad > canvas_max.y) {
            continue;
        }
        ImVec2 c1;
        ImVec2 c2;
        wire_controls(p0, p3, xf.zs, c1, c2);
        const bool sel = (e.id == selected_link_id_);
        const bool hot = sel || (e.id == hovered_link_id_ && interaction_ == Interaction::None);
        const ImU32 col = sel ? t.col32_wire_selected : pin_color(e.kind);
        const float core = (hot ? t.wire_thickness_hot : t.wire_thickness) * std::max(0.6f, zoom_) * dpi;
        dl->AddBezierCubic(p0, c1, c2, p3, t.col32_wire_shadow, t.wire_shadow_thickness * std::max(0.6f, zoom_) * dpi,
                           32);
        if (sel) {
            // Selection halo keeps the amber-white selected wire distinct from gold Blendshape wires.
            dl->AddBezierCubic(p0, c1, c2, p3, col32_with_alpha(t.col32_wire_selected, 0x55), core * 2.6f, 32);
        }
        dl->AddBezierCubic(p0, c1, c2, p3, col, core, 32);
        if (evaluating && zoom_ >= 0.5f) {
            const float u = std::fmod(time_s * 0.45f + static_cast<float>(e.id % 97U) * 0.137f, 1.0f);
            dl->AddCircleFilled(eval_bezier(p0, c1, c2, p3, u), 2.2f * xf.zs, t.col32_wire_pulse, 8);
        }
    }

    // Nodes: unselected first, selected on top
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            GraphNodeVm& n = nodes_[i];
            if (n.selected != (pass == 1)) {
                continue;
            }
            const ImVec2 n_min = xf.to_screen(n.x, n.y);
            const ImVec2 n_max = xf.to_screen(n.x + n.w, n.y + n.h);
            if (n_max.x < origin.x || n_min.x > canvas_max.x || n_max.y < origin.y || n_min.y > canvas_max.y) {
                continue;
            }
            draw_node(dl, xf, n, hov.node_idx == static_cast<int32_t>(i), fonts, host_status, dpi, undo, actions);
        }
    }

    // Floating live Watch Pin Value badges (fixed-capacity array, zero per-frame allocations)
    if (watched_pin_count_ > 0 && zoom_ >= 0.4f) {
        ImFont* watch_font =
            fonts.mono != nullptr ? fonts.mono : (fonts.caption != nullptr ? fonts.caption : ImGui::GetFont());
        const float w_px = std::max(8.0f, watch_font->FontSize * zoom_);
        for (size_t wi = 0; wi < watched_pin_count_; ++wi) {
            const PinRef wp = watched_pins_[wi];
            const GraphNodeVm* wn = find_node(wp.node);
            if (wn == nullptr || wp.pin >= (wp.output ? wn->num_outputs : wn->num_inputs)) {
                continue;
            }
            const ImVec2 pp = pin_screen_pos(xf, *wn, wp.output, wp.pin);
            if (pp.x + 120.0f * dpi < origin.x || pp.x - 120.0f * dpi > canvas_max.x || pp.y + 30.0f * dpi < origin.y ||
                pp.y - 30.0f * dpi > canvas_max.y) {
                continue;
            }
            char val_buf[48]{};
            if (format_watched_pin_value(wp, host_status, val_buf, sizeof(val_buf)) == 0) {
                continue;
            }
            const float pad_x = 7.0f * dpi;
            const float pad_y = 3.0f * dpi;
            const float dot_r = 2.6f * dpi;
            const float text_width = text_w(watch_font, w_px, val_buf);
            const float bw = pad_x * 2.0f + dot_r * 2.0f + 5.0f * dpi + text_width;
            const float bh = w_px + pad_y * 2.0f;
            const float gap = 10.0f * xf.zs;
            const ImVec2 b0 =
                wp.output ? ImVec2(pp.x + gap, pp.y - bh * 0.5f) : ImVec2(pp.x - gap - bw, pp.y - bh * 0.5f);
            const ImVec2 b1(b0.x + bw, b0.y + bh);
            const ImU32 pcol = pin_color(pin_kind(wp));
            dl->AddLine(pp, ImVec2(wp.output ? b0.x : b1.x, pp.y), col32_with_alpha(pcol, 0xCC), 1.5f * dpi);
            dl->AddRectFilled(b0, b1, t.col32_pin_watch_bg, 4.0f * dpi);
            dl->AddRect(b0, b1, col32_with_alpha(pcol, 0xCC), 4.0f * dpi, 0, 1.0f * dpi);
            dl->AddCircleFilled(ImVec2(b0.x + pad_x + dot_r, pp.y), dot_r, pcol, 10);
            dl->AddText(watch_font, w_px, ImVec2(b0.x + pad_x + dot_r * 2.0f + 5.0f * dpi, pp.y - w_px * 0.5f),
                        t.col32_text_primary, val_buf);
        }
    }

    // Hovered pin highlight ring
    if (hov.pin_valid) {
        const GraphNodeVm* hn = find_node(hov.pin.node);
        if (hn != nullptr) {
            const ImVec2 pp = pin_screen_pos(xf, *hn, hov.pin.output, hov.pin.pin);
            dl->AddCircle(pp, std::max(4.0f, t.node_pin_r * xf.zs) + 3.0f * dpi,
                          col32_with_alpha(t.col32_text_primary, 0x80), 16, 1.0f);
        }
    }

    // Live wire preview + magnetic snap + compatibility badge
    if (interaction_ == Interaction::Wiring) {
        const GraphNodeVm* an = find_node(wire_anchor_.node);
        if (an != nullptr) {
            const PinDataKind ak = pin_kind(wire_anchor_);
            const ImVec2 anchor_pt = pin_screen_pos(xf, *an, wire_anchor_.output, wire_anchor_.pin);
            ImVec2 end_pt = mouse;
            const char* badge = nullptr;
            bool badge_ok = false;
            char badge_buf[96]{};

            // Snap target (nearest compatible opposite pin within 14 px * dpi)
            const float snap_sq = (t.node_snap_r * dpi) * (t.node_snap_r * dpi);
            float best = snap_sq;
            for (const auto& n : nodes_) {
                if (n.id == wire_anchor_.node) {
                    continue;
                }
                const uint8_t cnt = wire_anchor_.output ? n.num_inputs : n.num_outputs;
                for (uint8_t p = 0; p < cnt; ++p) {
                    const PinDataKind k = wire_anchor_.output ? n.inputs[p].kind : n.outputs[p].kind;
                    if (!(wire_anchor_.output ? pin_kinds_compatible(ak, k) : pin_kinds_compatible(k, ak))) {
                        continue;
                    }
                    const ImVec2 pp = pin_screen_pos(xf, n, !wire_anchor_.output, p);
                    const float d = (pp.x - mouse.x) * (pp.x - mouse.x) + (pp.y - mouse.y) * (pp.y - mouse.y);
                    if (d <= best) {
                        best = d;
                        end_pt = pp;
                        std::snprintf(badge_buf, sizeof(badge_buf), "%s  Connect %s", icons::kCheck, pin_kind_name(ak));
                        badge = badge_buf;
                        badge_ok = true;
                    }
                }
            }
            if (!badge_ok && hov.pin_valid && hov.pin.node != wire_anchor_.node &&
                hov.pin.output != wire_anchor_.output) {
                const PinDataKind hk = pin_kind(hov.pin);
                const PinDataKind from_k = wire_anchor_.output ? ak : hk;
                const PinDataKind to_k = wire_anchor_.output ? hk : ak;
                std::snprintf(badge_buf, sizeof(badge_buf), "%s  Type Mismatch: Cannot connect %s to %s", icons::kClose,
                              pin_kind_name(from_k), pin_kind_name(to_k));
                badge = badge_buf;
            }

            const ImVec2 p0 = wire_anchor_.output ? anchor_pt : end_pt;
            const ImVec2 p3 = wire_anchor_.output ? end_pt : anchor_pt;
            ImVec2 c1;
            ImVec2 c2;
            wire_controls(p0, p3, xf.zs, c1, c2);
            dl->AddBezierCubic(p0, c1, c2, p3, t.col32_wire_shadow, t.wire_shadow_thickness * dpi, 32);
            dl->AddBezierCubic(p0, c1, c2, p3, pin_color(ak), t.wire_thickness_hot * dpi, 32);
            if (badge != nullptr) {
                const ImVec2 bs = ImGui::CalcTextSize(badge);
                const ImVec2 b0(mouse.x + 16.0f * dpi, mouse.y + 14.0f * dpi);
                const ImVec2 b1(b0.x + bs.x + 16.0f * dpi, b0.y + bs.y + 8.0f * dpi);
                const ImU32 bcol = badge_ok ? t.col32_status_ok : t.col32_status_error;
                dl->AddRectFilled(b0, b1, col32_with_alpha(bcol, 0xE6), 4.0f * dpi);
                dl->AddText(ImVec2(b0.x + 8.0f * dpi, b0.y + 4.0f * dpi), t.col32_text_on_accent, badge);
            }
        }
    }

    // Marquee selection rectangle
    if (interaction_ == Interaction::BoxSelect) {
        const ImVec2 a(std::min(box_start_.x, mouse.x), std::min(box_start_.y, mouse.y));
        const ImVec2 b(std::max(box_start_.x, mouse.x), std::max(box_start_.y, mouse.y));
        dl->AddRectFilled(a, b, col32_with_alpha(t.col32_accent, 0x26));
        dl->AddRect(a, b, col32_with_alpha(t.col32_accent, 0xB3), 0.0f, 0, 1.0f);
    }

    // Hover tooltip for wires (how to edit them)
    if (hov.link_id != 0 && interaction_ == Interaction::None && !hov.pin_valid && hov.node_idx < 0 &&
        ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s",
                          tr("Click: select · Alt+Click: break · Double-click: insert filter · Right-click: menu"));
    }

    dl->PopClipRect();
    draw_context_menus(xf, undo, actions);
    ImGui::EndChild();
}

} // namespace aether::ui
