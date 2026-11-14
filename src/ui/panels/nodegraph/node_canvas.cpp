#include "ui/panels/nodegraph/node_canvas.hpp"

#include "ui/theme/icons.hpp"
#include "ui/theme/tokens.hpp"
#include "ui/theme/tr.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::ui {
namespace {

struct NodeTemplateSpec {
    const char* type_id;
    const char* title;
    const char* category;
};

constexpr std::array<NodeTemplateSpec, 20> kNodeTemplates = {{
    {"aether.source.npu_pose@1", "NPU Pose In", "Sources"},
    {"aether.source.dsp_audio@1", "DSP Audio In", "Sources"},
    {"aether.source.clock@1", "Studio Clock", "Sources"},
    {"aether.source.osc_in@1", "OSC In", "Sources"},
    {"aether.ik.visibility_first@1", "Visibility-First IK", "Solvers"},
    {"aether.solver.look_at@1", "LookAt Solver", "Solvers"},
    {"aether.solver.idle_breath@1", "Idle Breath", "Solvers"},
    {"aether.expr.formant_lipsync@1", "Formant Lip-Sync", "Expressions"},
    {"aether.expr.mixer@1", "Expression Mixer", "Expressions"},
    {"aether.expr.auto_blink@1", "Auto Blink", "Expressions"},
    {"aether.math.add@1", "Add (Float)", "Math"},
    {"aether.math.multiply@1", "Multiply (Float)", "Math"},
    {"aether.math.lerp@1", "Lerp", "Math"},
    {"aether.math.clamp@1", "Clamp", "Math"},
    {"aether.math.smooth@1", "1-Euro / Smooth", "Math"},
    {"aether.math.spring@1", "Scalar Spring", "Math"},
    {"aether.output.pose@1", "Output Pose", "Outputs"},
    {"aether.output.expressions@1", "Output Expressions", "Outputs"},
    {"aether.output.camera_control@1", "Camera Control", "Outputs"},
    {"aether.output.light_control@1", "Light Control", "Outputs"},
}};

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

void draw_pin_glyph(ImDrawList* dl, ImVec2 center, float r, PinDataKind kind, ImU32 col, bool filled) noexcept {
    switch (kind) {
    case PinDataKind::Bool:
    case PinDataKind::Trigger: {
        const ImVec2 a(center.x - r, center.y - r);
        const ImVec2 b(center.x + r, center.y + r);
        if (filled) {
            dl->AddRectFilled(a, b, col, 1.0f);
        } else {
            dl->AddRect(a, b, col, 1.0f, 0, 1.5f);
        }
        break;
    }
    case PinDataKind::Pose:
    case PinDataKind::Vec3: {
        const ImVec2 pts[4]{
            ImVec2(center.x, center.y - r * 1.15f),
            ImVec2(center.x + r * 1.15f, center.y),
            ImVec2(center.x, center.y + r * 1.15f),
            ImVec2(center.x - r * 1.15f, center.y),
        };
        if (filled) {
            dl->AddConvexPolyFilled(pts, 4, col);
        } else {
            dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.5f);
        }
        break;
    }
    case PinDataKind::Expression: {
        const ImVec2 a(center.x - r, center.y - r);
        const ImVec2 b(center.x - r, center.y + r);
        const ImVec2 c(center.x + r * 1.2f, center.y);
        if (filled) {
            dl->AddTriangleFilled(a, b, c, col);
        } else {
            dl->AddTriangle(a, b, c, col, 1.5f);
        }
        break;
    }
    case PinDataKind::Light: {
        std::array<ImVec2, 6> pts{};
        for (int i = 0; i < 6; ++i) {
            const float ang = static_cast<float>(i) * 1.04719755f;
            pts[static_cast<size_t>(i)] =
                ImVec2(center.x + r * 1.1f * std::cos(ang), center.y + r * 1.1f * std::sin(ang));
        }
        if (filled) {
            dl->AddConvexPolyFilled(pts.data(), 6, col);
        } else {
            dl->AddPolyline(pts.data(), 6, col, ImDrawFlags_Closed, 1.5f);
        }
        break;
    }
    default:
        if (filled) {
            dl->AddCircleFilled(center, r, col, 12);
        } else {
            dl->AddCircle(center, r, col, 12, 1.5f);
        }
        break;
    }
}

void populate_ports_for_kind(GraphNodeVm& n) noexcept {
    auto set_pin = [](GraphPinVm& p, const char* name, PinDataKind k) noexcept {
        std::snprintf(p.name.data(), p.name.size(), "%s", name);
        p.kind = k;
    };

    const std::string_view k(n.kind.data());
    n.num_inputs = 0;
    n.num_outputs = 0;
    if (k == "aether.source.npu_pose@1") {
        n.num_outputs = 3;
        set_pin(n.outputs[0], "pose", PinDataKind::Pose);
        set_pin(n.outputs[1], "face_conf", PinDataKind::Float);
        set_pin(n.outputs[2], "hand_conf", PinDataKind::Float);
    } else if (k == "aether.source.dsp_audio@1") {
        n.num_outputs = 3;
        set_pin(n.outputs[0], "visemes", PinDataKind::Expression);
        set_pin(n.outputs[1], "rms_db", PinDataKind::Float);
        set_pin(n.outputs[2], "voice_active", PinDataKind::Bool);
    } else if (k == "aether.source.clock@1") {
        n.num_outputs = 2;
        set_pin(n.outputs[0], "time_s", PinDataKind::Float);
        set_pin(n.outputs[1], "dt_s", PinDataKind::Float);
    } else if (k == "aether.source.osc_in@1") {
        n.num_outputs = 2;
        set_pin(n.outputs[0], "value", PinDataKind::Float);
        set_pin(n.outputs[1], "trigger", PinDataKind::Trigger);
    } else if (k == "aether.ik.visibility_first@1") {
        n.num_inputs = 3;
        set_pin(n.inputs[0], "pose", PinDataKind::Pose);
        set_pin(n.inputs[1], "face_conf", PinDataKind::Float);
        set_pin(n.inputs[2], "hand_conf", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "pose", PinDataKind::Pose);
    } else if (k == "aether.solver.look_at@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "target", PinDataKind::Vec3);
        set_pin(n.inputs[1], "weight", PinDataKind::Float);
        n.num_outputs = 2;
        set_pin(n.outputs[0], "head_yaw", PinDataKind::Float);
        set_pin(n.outputs[1], "head_pitch", PinDataKind::Float);
    } else if (k == "aether.solver.idle_breath@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "rate_hz", PinDataKind::Float);
        set_pin(n.inputs[1], "amplitude", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "pose", PinDataKind::Pose);
    } else if (k == "aether.expr.formant_lipsync@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "visemes", PinDataKind::Expression);
        set_pin(n.inputs[1], "gain", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "expressions", PinDataKind::Expression);
    } else if (k == "aether.expr.mixer@1") {
        n.num_inputs = 3;
        set_pin(n.inputs[0], "a", PinDataKind::Expression);
        set_pin(n.inputs[1], "b", PinDataKind::Expression);
        set_pin(n.inputs[2], "weight", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "expressions", PinDataKind::Expression);
    } else if (k == "aether.expr.auto_blink@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "face_conf", PinDataKind::Float);
        set_pin(n.inputs[1], "interval_s", PinDataKind::Float);
        n.num_outputs = 2;
        set_pin(n.outputs[0], "expressions", PinDataKind::Expression);
        set_pin(n.outputs[1], "blink", PinDataKind::Float);
    } else if (k == "aether.math.add@1" || k == "aether.math.multiply@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "a", PinDataKind::Float);
        set_pin(n.inputs[1], "b", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "out", PinDataKind::Float);
    } else if (k == "aether.math.lerp@1") {
        n.num_inputs = 3;
        set_pin(n.inputs[0], "a", PinDataKind::Float);
        set_pin(n.inputs[1], "b", PinDataKind::Float);
        set_pin(n.inputs[2], "t", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "out", PinDataKind::Float);
    } else if (k == "aether.output.pose@1") {
        n.num_inputs = 1;
        set_pin(n.inputs[0], "pose", PinDataKind::Pose);
    } else if (k == "aether.output.expressions@1") {
        n.num_inputs = 1;
        set_pin(n.inputs[0], "expressions", PinDataKind::Expression);
    } else if (k == "aether.output.camera_control@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "focal_mm", PinDataKind::Float);
        set_pin(n.inputs[1], "flatness", PinDataKind::Float);
    } else if (k == "aether.output.light_control@1") {
        n.num_inputs = 2;
        set_pin(n.inputs[0], "direction", PinDataKind::Light);
        set_pin(n.inputs[1], "intensity", PinDataKind::Float);
    } else {
        n.num_inputs = 1;
        set_pin(n.inputs[0], "in", PinDataKind::Float);
        n.num_outputs = 1;
        set_pin(n.outputs[0], "out", PinDataKind::Float);
    }
    const size_t max_pins = std::max<size_t>(n.num_inputs, n.num_outputs);
    n.h = std::max(64.0f, 36.0f + static_cast<float>(max_pins) * 18.0f);
}

ImVec2 eval_bezier_pt(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float u) noexcept {
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
    const float t = c1 / std::max(1e-5f, c2);
    const float px = a.x + t * vx;
    const float py = a.y + t * vy;
    const float dx = p.x - px;
    const float dy = p.y - py;
    return dx * dx + dy * dy;
}

} // namespace

NodeCanvas::NodeCanvas() {
    reset_default_graph();
}

void NodeCanvas::invalidate_edge_cache() noexcept {
    for (auto& e : edges_) {
        e.cache_dirty = true;
    }
}

int NodeCanvas::find_node_index(uint64_t node_id) const noexcept {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].id == node_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void NodeCanvas::reset_default_graph() {
    nodes_.clear();
    edges_.clear();
    comments_.clear();

    auto make_node = [](uint64_t id, const char* kind, const char* title, float x, float y) {
        GraphNodeVm n{};
        n.id = id;
        std::snprintf(n.kind.data(), n.kind.size(), "%s", kind);
        std::snprintf(n.title.data(), n.title.size(), "%s", title);
        n.x = x;
        n.y = y;
        populate_ports_for_kind(n);
        return n;
    };

    nodes_.push_back(make_node(1, "aether.source.npu_pose@1", "NPU Pose In", 40.0f, 30.0f));
    nodes_.push_back(make_node(2, "aether.ik.visibility_first@1", "Visibility-First IK", 260.0f, 30.0f));
    nodes_.push_back(make_node(3, "aether.output.pose@1", "Output Pose", 500.0f, 30.0f));
    nodes_.push_back(make_node(4, "aether.source.dsp_audio@1", "DSP Audio In", 40.0f, 160.0f));
    nodes_.push_back(make_node(5, "aether.expr.formant_lipsync@1", "Formant Lip-Sync", 260.0f, 160.0f));
    nodes_.push_back(make_node(6, "aether.output.expressions@1", "Output Expressions", 500.0f, 160.0f));

    edges_.push_back(GraphEdgeVm{10, 1, 0, 2, 0, PinDataKind::Pose});
    edges_.push_back(GraphEdgeVm{11, 1, 1, 2, 1, PinDataKind::Float});
    edges_.push_back(GraphEdgeVm{12, 2, 0, 3, 0, PinDataKind::Pose});
    edges_.push_back(GraphEdgeVm{13, 4, 0, 5, 0, PinDataKind::Expression});
    edges_.push_back(GraphEdgeVm{14, 5, 0, 6, 0, PinDataKind::Expression});

    comments_.push_back(GraphCommentVm{20, {"Tracking & Audio Rig"}, 20.0f, 8.0f, 670.0f, 265.0f});
    next_id_ = 100;
    last_committed_json_ = serialize_graph_json();
}

void NodeCanvas::load_sample_64_nodes() {
    nodes_.clear();
    edges_.clear();
    comments_.clear();

    uint64_t nid = 1;
    uint64_t eid = 1000;
    {
        GraphNodeVm clk{};
        clk.id = nid++;
        std::snprintf(clk.kind.data(), clk.kind.size(), "aether.source.clock@1");
        std::snprintf(clk.title.data(), clk.title.size(), "Studio Clock");
        clk.x = 20.0f;
        clk.y = 40.0f;
        populate_ports_for_kind(clk);
        nodes_.push_back(clk);
    }

    for (uint32_t i = 1; i < 62; ++i) {
        GraphNodeVm n{};
        n.id = nid++;
        const char* kind =
            (i % 3 == 0) ? "aether.math.smooth@1" : ((i % 2 == 0) ? "aether.math.clamp@1" : "aether.math.add@1");
        std::snprintf(n.kind.data(), n.kind.size(), "%s", kind);
        std::snprintf(n.title.data(), n.title.size(), "Node #%u", i);
        const uint32_t col = i % 8u;
        const uint32_t row = i / 8u;
        n.x = 20.0f + static_cast<float>(col) * 210.0f;
        n.y = 40.0f + static_cast<float>(row) * 112.0f;
        populate_ports_for_kind(n);
        nodes_.push_back(n);

        const uint64_t src_id = (i > 1) ? (n.id - 1) : 1;
        edges_.push_back(GraphEdgeVm{eid++, src_id, 0, n.id, 0, PinDataKind::Float});
    }

    // Two final outputs closing the 64-node graph cleanly
    for (int k = 0; k < 2; ++k) {
        GraphNodeVm out{};
        out.id = nid++;
        const char* kind = (k == 0) ? "aether.output.camera_control@1" : "aether.output.light_control@1";
        std::snprintf(out.kind.data(), out.kind.size(), "%s", kind);
        std::snprintf(out.title.data(), out.title.size(), k == 0 ? "Camera Out" : "Light Out");
        const int col = (62 + k) % 8;
        const int row = (62 + k) / 8;
        out.x = 20.0f + static_cast<float>(col) * 210.0f;
        out.y = 40.0f + static_cast<float>(row) * 112.0f;
        populate_ports_for_kind(out);
        nodes_.push_back(out);
        edges_.push_back(GraphEdgeVm{eid++, nid - 3, 0, out.id, 1, PinDataKind::Float});
    }

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

    uint64_t max_seen = 100;
    for (const auto& jn : doc["nodes"]) {
        if (!jn.is_object() || !jn.contains("id") || !jn.contains("type_id")) {
            continue;
        }
        GraphNodeVm n{};
        n.id = jn.value("id", uint64_t{1});
        max_seen = std::max(max_seen, n.id + 1);
        const std::string tid = jn.value("type_id", std::string{"aether.math.add@1"});
        std::snprintf(n.kind.data(), n.kind.size(), "%s", tid.c_str());
        const std::string lbl = jn.contains("label") && jn["label"].is_string() ? jn["label"].get<std::string>() : tid;
        std::snprintf(n.title.data(), n.title.size(), "%s", lbl.c_str());
        if (jn.contains("position") && jn["position"].is_array() && jn["position"].size() >= 2) {
            n.x = jn["position"][0].get<float>();
            n.y = jn["position"][1].get<float>();
        }
        if (jn.contains("params") && jn["params"].is_object()) {
            const auto& prms = jn["params"];
            if (prms.contains("bypass") && prms["bypass"].is_boolean()) {
                n.bypassed = prms["bypass"].get<bool>();
            }
            if (prms.contains("param_id") && prms["param_id"].is_string()) {
                std::snprintf(n.param_id.data(), n.param_id.size(), "%s", prms["param_id"].get<std::string>().c_str());
            }
            if (prms.contains("b") && prms["b"].is_number()) {
                n.constant_val = prms["b"].get<float>();
            }
        }
        populate_ports_for_kind(n);
        nodes_.push_back(n);
    }

    if (doc.contains("edges") && doc["edges"].is_array()) {
        for (const auto& je : doc["edges"]) {
            if (!je.is_object()) {
                continue;
            }
            GraphEdgeVm e{};
            e.id = je.value("id", next_id_++);
            max_seen = std::max(max_seen, e.id + 1);
            e.from_node = je.value("from_node", uint64_t{0});
            e.to_node = je.value("to_node", uint64_t{0});
            const std::string fp = je.value("from_port", std::string{});
            const std::string tp = je.value("to_port", std::string{});

            const int fi = find_node_index(e.from_node);
            const int ti = find_node_index(e.to_node);
            if (fi >= 0) {
                const auto& fn = nodes_[static_cast<size_t>(fi)];
                for (uint8_t p = 0; p < fn.num_outputs; ++p) {
                    if (fp == fn.outputs[p].name.data()) {
                        e.from_pin = p;
                        e.kind = fn.outputs[p].kind;
                        break;
                    }
                }
            }
            if (ti >= 0) {
                const auto& tn = nodes_[static_cast<size_t>(ti)];
                for (uint8_t p = 0; p < tn.num_inputs; ++p) {
                    if (tp == tn.inputs[p].name.data()) {
                        e.to_pin = p;
                        break;
                    }
                }
            }
            edges_.push_back(e);
        }
    }

    if (doc.contains("comments") && doc["comments"].is_array()) {
        for (const auto& jc : doc["comments"]) {
            GraphCommentVm c{};
            c.id = jc.value("id", next_id_++);
            const std::string txt = jc.value("text", std::string{"Group"});
            std::snprintf(c.title.data(), c.title.size(), "%s", txt.c_str());
            if (jc.contains("position") && jc["position"].is_array() && jc["position"].size() >= 2) {
                c.x = jc["position"][0].get<float>();
                c.y = jc["position"][1].get<float>();
            }
            if (jc.contains("size") && jc["size"].is_array() && jc["size"].size() >= 2) {
                c.w = jc["size"][0].get<float>();
                c.h = jc["size"][1].get<float>();
            }
            comments_.push_back(c);
        }
    }

    next_id_ = std::max(next_id_, max_seen);
    last_committed_json_ = serialize_graph_json();
}

std::string NodeCanvas::serialize_graph_json() const {
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json jnodes = nlohmann::json::array();
    for (const auto& n : nodes_) {
        nlohmann::json prms = nlohmann::json::object();
        prms["bypass"] = n.bypassed;
        prms["b"] = n.constant_val;
        if (n.param_id[0] != '\0') {
            prms["param_id"] = n.param_id.data();
        }
        jnodes.push_back({
            {"id", n.id},
            {"type_id", n.kind.data()},
            {"position", {n.x, n.y}},
            {"label", n.title.data()},
            {"params", prms},
            {"ports", nlohmann::json::array()},
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
        const char* fport = e.from_pin < fn.num_outputs ? fn.outputs[e.from_pin].name.data() : "out";
        const char* tport = e.to_pin < tn.num_inputs ? tn.inputs[e.to_pin].name.data() : "in";
        jedges.push_back({
            {"id", e.id},
            {"from_node", e.from_node},
            {"from_port", fport},
            {"to_node", e.to_node},
            {"to_port", tport},
        });
    }

    nlohmann::json jcomments = nlohmann::json::array();
    for (const auto& c : comments_) {
        jcomments.push_back({
            {"id", c.id},
            {"text", c.title.data()},
            {"position", {c.x, c.y}},
            {"size", {c.w, c.h}},
        });
    }

    root["nodes"] = std::move(jnodes);
    root["edges"] = std::move(jedges);
    root["groups"] = nlohmann::json::array();
    root["comments"] = std::move(jcomments);
    return root.dump();
}

void NodeCanvas::commit_edit(const char* label, UndoStack& undo, StudioActions& actions) {
    std::string next_json = serialize_graph_json();
    if (next_json != last_committed_json_) {
        undo.push_graph_edit(label, last_committed_json_, next_json);
        last_committed_json_ = next_json;
    }
    actions.request_apply_graph_json = true;
    actions.pending_graph_json = std::move(next_json);
}

void NodeCanvas::add_exposed_param_node(const char* param_id) {
    if (param_id == nullptr || param_id[0] == '\0') {
        return;
    }
    GraphNodeVm n{};
    n.id = next_id_++;
    std::snprintf(n.kind.data(), n.kind.size(), "aether.math.add@1");
    std::snprintf(n.title.data(), n.title.size(), "Param: %s", param_id);
    std::snprintf(n.param_id.data(), n.param_id.size(), "%s", param_id);
    n.x = 120.0f - pan_.x;
    n.y = 120.0f - pan_.y;
    n.constant_val = 0.0f;
    populate_ports_for_kind(n);
    nodes_.push_back(n);
}

void NodeCanvas::add_builtin_node(const char* kind, float world_x, float world_y, UndoStack& undo,
                                  StudioActions& actions) {
    GraphNodeVm n{};
    n.id = next_id_++;
    std::snprintf(n.kind.data(), n.kind.size(), "%s", kind);
    const char* title = kind;
    for (const auto& tpl : kNodeTemplates) {
        if (std::strcmp(tpl.type_id, kind) == 0) {
            title = tpl.title;
            break;
        }
    }
    std::snprintf(n.title.data(), n.title.size(), "%s", title);
    n.x = world_x;
    n.y = world_y;
    populate_ports_for_kind(n);
    nodes_.push_back(n);
    commit_edit("Add Node", undo, actions);
}

void NodeCanvas::frame_all() noexcept {
    if (nodes_.empty()) {
        pan_ = ImVec2(24.0f, 24.0f);
        zoom_ = 1.0f;
        invalidate_edge_cache();
        return;
    }
    float min_x = nodes_[0].x;
    float min_y = nodes_[0].y;
    for (const auto& n : nodes_) {
        min_x = std::min(min_x, n.x);
        min_y = std::min(min_y, n.y);
    }
    pan_ = ImVec2(32.0f - min_x, 32.0f - min_y);
    zoom_ = 1.0f;
    invalidate_edge_cache();
}

void NodeCanvas::duplicate_selected(UndoStack& undo, StudioActions& actions) {
    size_t added = 0;
    const size_t orig = nodes_.size();
    for (size_t i = 0; i < orig; ++i) {
        if (nodes_[i].selected) {
            GraphNodeVm dup = nodes_[i];
            dup.id = next_id_++;
            dup.x += 36.0f;
            dup.y += 36.0f;
            nodes_[i].selected = false;
            dup.selected = true;
            nodes_.push_back(dup);
            ++added;
        }
    }
    if (added > 0) {
        commit_edit("Duplicate Node", undo, actions);
    }
}

void NodeCanvas::delete_selected(UndoStack& undo, StudioActions& actions) {
    bool any = false;
    for (const auto& n : nodes_) {
        if (n.selected) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const GraphEdgeVm& e) {
                                    const int fi = find_node_index(e.from_node);
                                    const int ti = find_node_index(e.to_node);
                                    return (fi >= 0 && nodes_[static_cast<size_t>(fi)].selected) ||
                                           (ti >= 0 && nodes_[static_cast<size_t>(ti)].selected);
                                }),
                 edges_.end());
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [](const GraphNodeVm& n) { return n.selected; }),
                 nodes_.end());
    invalidate_edge_cache();
    commit_edit("Delete Node", undo, actions);
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
        min_y = std::min(min_y, n.y - 28.0f);
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

void NodeCanvas::draw(const StudioHostStatus& host_status, UndoStack& undo, const UiFontSet& fonts, float dpi_scale,
                      UiSelection& selection, StudioActions& actions) {
    const PrecisionDarkTokens& t = tokens();
    const float scale = std::max(0.75f, dpi_scale);

    // Compact header bar (compile telemetry + quick actions)
    if (ImGui::Button(tr("+ Add Node"))) {
        open_add_popup_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Recompile"))) {
        actions.request_recompile_graph_now = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Frame"))) {
        frame_all();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("64-Node Stress"))) {
        load_sample_64_nodes();
        actions.request_apply_graph_json = true;
        actions.pending_graph_json = last_committed_json_;
    }
    ImGui::SameLine();
    if (fonts.mono != nullptr) {
        ImGui::PushFont(fonts.mono);
    }
    ImGui::TextColored(host_status.eval_budget_warn ? t.status_warn : t.text_secondary,
                       "Nodes: %zu | Active: %u | Compile: %.2f ms | Eval: %.1f us", nodes_.size(),
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

    const ImVec2 canvas_avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvas_size(std::max(120.0f, canvas_avail.x), std::max(120.0f, canvas_avail.y));

    ImGui::BeginChild("##GraphCanvasSurface", canvas_size, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_max(origin.x + canvas_size.x, origin.y + canvas_size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(origin, canvas_max, t.col32_bg_canvas);

    // Draw background grid (culling cleanly to canvas bounds)
    const float grid_step = std::max(16.0f, 32.0f * zoom_ * scale);
    const float off_x = std::fmod(pan_.x * zoom_ * scale, grid_step);
    const float off_y = std::fmod(pan_.y * zoom_ * scale, grid_step);
    for (float x = origin.x + off_x; x < canvas_max.x; x += grid_step) {
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, canvas_max.y), t.col32_grid_minor);
    }
    for (float y = origin.y + off_y; y < canvas_max.y; y += grid_step) {
        dl->AddLine(ImVec2(origin.x, y), ImVec2(canvas_max.x, y), t.col32_grid_minor);
    }

    if (std::fabs(pan_.x - last_pan_x_) > 0.01f || std::fabs(pan_.y - last_pan_y_) > 0.01f ||
        std::fabs(zoom_ - last_zoom_) > 0.001f || std::fabs(origin.x - last_canvas_origin_.x) > 0.5f ||
        std::fabs(origin.y - last_canvas_origin_.y) > 0.5f) {
        invalidate_edge_cache();
        last_pan_x_ = pan_.x;
        last_pan_y_ = pan_.y;
        last_zoom_ = zoom_;
        last_canvas_origin_ = origin;
    }

    auto to_screen = [&](float wx, float wy) noexcept -> ImVec2 {
        return ImVec2(origin.x + (wx + pan_.x) * zoom_ * scale, origin.y + (wy + pan_.y) * zoom_ * scale);
    };

    const ImGuiIO& io = ImGui::GetIO();
    const bool canvas_hovered = ImGui::IsWindowHovered();

    // Pan & Zoom
    if (canvas_hovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
            (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            pan_.x += io.MouseDelta.x / (zoom_ * scale);
            pan_.y += io.MouseDelta.y / (zoom_ * scale);
        }
        if (std::fabs(io.MouseWheel) > 1e-4f) {
            zoom_ = std::clamp(zoom_ + io.MouseWheel * 0.08f, 0.35f, 2.0f);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            open_add_popup_ = true;
        }
    }

    if (nodes_.empty()) {
        dl->AddText(ImVec2(origin.x + canvas_size.x * 0.32f, origin.y + canvas_size.y * 0.45f), t.col32_text_secondary,
                    tr("Graph is empty — press Tab to add a node"));
    }

    // Comment boxes (background layer)
    for (const auto& c : comments_) {
        const ImVec2 p0 = to_screen(c.x, c.y);
        const ImVec2 p1 = to_screen(c.x + c.w, c.y + c.h);
        if (p1.x < origin.x || p0.x > canvas_max.x || p1.y < origin.y || p0.y > canvas_max.y) {
            continue;
        }
        dl->AddRectFilled(p0, p1, t.col32_bg_panel, t.radius_control_px * scale);
        dl->AddRect(p0, p1, t.col32_border_default, t.radius_control_px * scale);
        dl->AddText(ImVec2(p0.x + 8.0f, p0.y + 4.0f), t.col32_text_secondary, c.title.data());
    }

    auto pin_screen_pos = [&](const GraphNodeVm& n, bool is_out, uint8_t pin_idx) noexcept -> ImVec2 {
        const float px = is_out ? (n.x + n.w) : n.x;
        const float py = n.y + 34.0f + static_cast<float>(pin_idx) * 18.0f;
        return to_screen(px, py);
    };

    // Cached Bezier wires + culling + Alt+Click delete + Double-click insert Reroute (§12.15)
    int32_t edge_to_delete = -1;
    int32_t edge_to_reroute = -1;
    for (size_t ei = 0; ei < edges_.size(); ++ei) {
        GraphEdgeVm& e = edges_[ei];
        const int fi = find_node_index(e.from_node);
        const int ti = find_node_index(e.to_node);
        if (fi < 0 || ti < 0) {
            continue;
        }
        const GraphNodeVm& fn = nodes_[static_cast<size_t>(fi)];
        const GraphNodeVm& tn = nodes_[static_cast<size_t>(ti)];

        const ImVec2 p0 = pin_screen_pos(fn, true, e.from_pin);
        const ImVec2 p1 = pin_screen_pos(tn, false, e.to_pin);
        const float min_x = std::min(p0.x, p1.x) - 64.0f;
        const float max_x = std::max(p0.x, p1.x) + 64.0f;
        const float min_y = std::min(p0.y, p1.y) - 64.0f;
        const float max_y = std::max(p0.y, p1.y) + 64.0f;
        if (max_x < origin.x || min_x > canvas_max.x || max_y < origin.y || min_y > canvas_max.y) {
            continue;
        }

        if (e.cache_dirty) {
            const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.45f);
            const ImVec2 c0(p0.x + dx, p0.y);
            const ImVec2 c1(p1.x - dx, p1.y);
            for (size_t s = 0; s < e.cached_pts.size(); ++s) {
                const float u = static_cast<float>(s) / static_cast<float>(e.cached_pts.size() - 1);
                e.cached_pts[s] = eval_bezier_pt(p0, c0, c1, p1, u);
            }
            e.cache_dirty = false;
        }

        const ImU32 wire_col = t.col32_pin_types[static_cast<size_t>(e.kind) & 7u];
        dl->AddPolyline(e.cached_pts.data(), static_cast<int>(e.cached_pts.size()), wire_col, ImDrawFlags_None,
                        2.0f * zoom_ * scale);

        if (canvas_hovered) {
            bool hit = false;
            for (size_t s = 0; s + 1 < e.cached_pts.size(); ++s) {
                if (dist_sq_to_segment(io.MousePos, e.cached_pts[s], e.cached_pts[s + 1]) <= 36.0f) {
                    hit = true;
                    break;
                }
            }
            if (hit) {
                if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    edge_to_delete = static_cast<int32_t>(ei);
                } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    edge_to_reroute = static_cast<int32_t>(ei);
                }
            }
        }
    }

    if (edge_to_delete >= 0) {
        edges_.erase(edges_.begin() + edge_to_delete);
        commit_edit("Remove Wire", undo, actions);
    } else if (edge_to_reroute >= 0 && static_cast<size_t>(edge_to_reroute) < edges_.size()) {
        const GraphEdgeVm old_e = edges_[static_cast<size_t>(edge_to_reroute)];
        const int fi = find_node_index(old_e.from_node);
        const int ti = find_node_index(old_e.to_node);
        if (fi >= 0 && ti >= 0) {
            GraphNodeVm rr{};
            rr.id = next_id_++;
            const char* rr_kind =
                old_e.kind == PinDataKind::Pose
                    ? "aether.ik.visibility_first@1"
                    : (old_e.kind == PinDataKind::Expression ? "aether.expr.remap@1" : "aether.math.add@1");
            std::snprintf(rr.kind.data(), rr.kind.size(), "%s", rr_kind);
            std::snprintf(rr.title.data(), rr.title.size(), "Reroute");
            rr.constant_val = 0.0f;
            rr.x = (nodes_[static_cast<size_t>(fi)].x + nodes_[static_cast<size_t>(ti)].x) * 0.5f;
            rr.y = (nodes_[static_cast<size_t>(fi)].y + nodes_[static_cast<size_t>(ti)].y) * 0.5f;
            populate_ports_for_kind(rr);
            nodes_.push_back(rr);

            edges_.erase(edges_.begin() + edge_to_reroute);
            edges_.push_back(GraphEdgeVm{next_id_++, old_e.from_node, old_e.from_pin, rr.id, 0, old_e.kind});
            edges_.push_back(GraphEdgeVm{next_id_++, rr.id, 0, old_e.to_node, old_e.to_pin, old_e.kind});
            invalidate_edge_cache();
            commit_edit("Insert Reroute Node", undo, actions);
        }
    }

    // Frustum-culled nodes + interactive drag & pin connect
    const float pin_r = std::max(3.5f, 4.5f * zoom_ * scale);
    const float pin_hit_r_sq = 100.0f;
    bool handled_mouse_down = false;

    for (size_t ni = 0; ni < nodes_.size(); ++ni) {
        GraphNodeVm& n = nodes_[ni];
        const ImVec2 n_min = to_screen(n.x, n.y);
        const ImVec2 n_max = to_screen(n.x + n.w, n.y + n.h);
        if (n_max.x < origin.x || n_min.x > canvas_max.x || n_max.y < origin.y || n_min.y > canvas_max.y) {
            continue;
        }

        const bool is_err = n.has_error || (host_status.error_node_id == n.id);
        const ImU32 border_col = is_err ? t.col32_status_error : (n.selected ? t.col32_accent : t.col32_border_default);
        const ImU32 body_col = n.bypassed ? t.col32_bg_panel : t.col32_bg_raised;
        const float round_px = t.radius_popover_px * zoom_ * scale;

        dl->AddRectFilled(n_min, n_max, body_col, round_px);
        const ImVec2 hdr_max(n_max.x, n_min.y + 22.0f * zoom_ * scale);
        dl->AddRectFilled(n_min, hdr_max, t.col32_bg_overlay, round_px, ImDrawFlags_RoundCornersTop);
        dl->AddRect(n_min, n_max, border_col, round_px, 0, n.selected ? 2.0f : hairline_px(scale));

        if (zoom_ >= 0.55f) {
            if (n.bypassed) {
                dl->AddText(ImVec2(n_min.x + 6.0f, n_min.y + 3.0f), t.col32_text_tertiary, "[BYP]");
                dl->AddText(ImVec2(n_min.x + 42.0f, n_min.y + 3.0f), t.col32_text_secondary, n.title.data());
            } else {
                dl->AddText(ImVec2(n_min.x + 6.0f, n_min.y + 3.0f), t.col32_text_primary, n.title.data());
            }
        }

        // Inputs on left edge
        for (uint8_t pi = 0; pi < n.num_inputs; ++pi) {
            const ImVec2 pp = pin_screen_pos(n, false, pi);
            const ImU32 pcol = t.col32_pin_types[static_cast<size_t>(n.inputs[pi].kind) & 7u];
            draw_pin_glyph(dl, pp, pin_r, n.inputs[pi].kind, pcol, false);
            if (zoom_ >= 0.65f) {
                dl->AddText(ImVec2(pp.x + 8.0f, pp.y - 6.0f), t.col32_text_secondary, n.inputs[pi].name.data());
            }
            if (wiring_active_ && canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const float dx = io.MousePos.x - pp.x;
                const float dy = io.MousePos.y - pp.y;
                if ((dx * dx + dy * dy) <= pin_hit_r_sq && wire_start_node_ != n.id) {
                    edges_.push_back(
                        GraphEdgeVm{next_id_++, wire_start_node_, wire_start_pin_, n.id, pi, wire_start_kind_});
                    invalidate_edge_cache();
                    wiring_active_ = false;
                    commit_edit("Connect Wire", undo, actions);
                }
            }
        }

        // Outputs on right edge
        for (uint8_t po = 0; po < n.num_outputs; ++po) {
            const ImVec2 pp = pin_screen_pos(n, true, po);
            const ImU32 pcol = t.col32_pin_types[static_cast<size_t>(n.outputs[po].kind) & 7u];
            draw_pin_glyph(dl, pp, pin_r, n.outputs[po].kind, pcol, true);
            if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
                const float dx = io.MousePos.x - pp.x;
                const float dy = io.MousePos.y - pp.y;
                if ((dx * dx + dy * dy) <= pin_hit_r_sq) {
                    wiring_active_ = true;
                    wire_start_node_ = n.id;
                    wire_start_pin_ = po;
                    wire_start_kind_ = n.outputs[po].kind;
                    handled_mouse_down = true;
                }
            }
        }

        // Node body click/drag
        if (!handled_mouse_down && canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
            if (io.MousePos.x >= n_min.x && io.MousePos.x <= n_max.x && io.MousePos.y >= n_min.y &&
                io.MousePos.y <= n_max.y) {
                if (!io.KeyShift) {
                    for (auto& other : nodes_) {
                        other.selected = false;
                    }
                }
                n.selected = true;
                dragging_node_ = true;
                dragged_node_idx_ = static_cast<int32_t>(ni);
                selection.kind = SelectionKind::Node;
                selection.item_index = static_cast<int32_t>(n.id);
                std::snprintf(selection.name.data(), selection.name.size(), "%s", n.title.data());
                handled_mouse_down = true;
            }
        }
    }

    if (dragging_node_) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const float dx = io.MouseDelta.x / (zoom_ * scale);
            const float dy = io.MouseDelta.y / (zoom_ * scale);
            for (auto& n : nodes_) {
                if (n.selected) {
                    n.x += dx;
                    n.y += dy;
                }
            }
            invalidate_edge_cache();
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            dragging_node_ = false;
            dragged_node_idx_ = -1;
            commit_edit("Move Node", undo, actions);
        }
    }

    if (wiring_active_) {
        const int fi = find_node_index(wire_start_node_);
        if (fi >= 0) {
            const ImVec2 start_pt = pin_screen_pos(nodes_[static_cast<size_t>(fi)], true, wire_start_pin_);
            dl->AddLine(start_pt, io.MousePos, t.col32_pin_types[static_cast<size_t>(wire_start_kind_) & 7u], 2.0f);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            wiring_active_ = false;
        }
    }

    // Add Node searchable popup (Tab / Space / right-click)
    if (open_add_popup_) {
        ImGui::OpenPopup("##AddNodePopup");
        open_add_popup_ = false;
    }
    if (ImGui::BeginPopup("##AddNodePopup")) {
        ImGui::InputTextWithHint("##nf", tr("Filter node types..."), add_filter_.data(), add_filter_.size());
        ImGui::Separator();
        for (const auto& tpl : kNodeTemplates) {
            if (!icase_match(tpl.title, add_filter_.data()) && !icase_match(tpl.category, add_filter_.data())) {
                continue;
            }
            char item_lbl[96]{};
            std::snprintf(item_lbl, sizeof(item_lbl), "[%s] %s", tpl.category, tpl.title);
            if (ImGui::Selectable(item_lbl)) {
                add_builtin_node(tpl.type_id, 140.0f - pan_.x, 90.0f - pan_.y, undo, actions);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

} // namespace aether::ui
