//! Built-in node catalog and port signatures covering every §7.4 v1 node type.

use crate::graph::model::{GraphNode, PortDecl, PortDirection, ValueType};

/// Internal discriminant for every built-in and plugin node type (§7.4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind {
    // Sources
    NpuPose,
    DspAudio,
    Hotkey,
    Clock,
    OscIn,
    WsIn,
    // Solvers
    VisibilityFirstIk,
    LookAt,
    IdleBreath,
    IdleAttractor,
    FootPlanting,
    // Expressions
    FormantLipSync,
    ExpressionTrigger,
    ExpressionMixer,
    AutoBlink,
    BlendshapeRemap,
    // Math & Logic
    Add,
    Multiply,
    Lerp,
    Remap,
    Clamp,
    Smooth,
    Spring,
    Compare,
    And,
    Or,
    Not,
    Select,
    Delay,
    Random,
    Curve,
    // Outputs
    OutputPose,
    OutputExpressions,
    CameraControl,
    LightControl,
    PropAttach,
    OscOut,
    WsOut,
    // Model, Material & Texture Management (§6.2)
    ModelSource,
    MaterialSlotOverride,
    TextureAsset,
    TextureGrade,
    DynamicMaterialDriver,
    // External Plugins (WASM / Native latched bridge)
    WasmPlugin,
}

/// Describes one node port with a static string name.
#[derive(Clone, Debug)]
pub struct ResolvedPort {
    /// Port name.
    pub name: String,
    /// Port value type.
    pub ty: ValueType,
    /// Input or output.
    pub direction: PortDirection,
}

/// Resolves a versioned type identifier (`"aether.math.add@1"` or short alias) to a [`NodeKind`].
#[must_use]
pub fn classify_node_type(type_id: &str) -> Option<NodeKind> {
    let base = type_id.split('@').next().unwrap_or(type_id);
    match base {
        "aether.source.npu_pose" | "npu_pose" => Some(NodeKind::NpuPose),
        "aether.source.dsp_audio" | "dsp_audio" => Some(NodeKind::DspAudio),
        "aether.source.hotkey" | "hotkey" => Some(NodeKind::Hotkey),
        "aether.source.clock" | "clock" => Some(NodeKind::Clock),
        "aether.source.osc_in" | "osc_in" => Some(NodeKind::OscIn),
        "aether.source.ws_in" | "ws_in" => Some(NodeKind::WsIn),

        "aether.ik.visibility_first" | "visibility_first_ik" => Some(NodeKind::VisibilityFirstIk),
        "aether.solver.look_at" | "look_at" => Some(NodeKind::LookAt),
        "aether.solver.idle_breath" | "idle_breath" => Some(NodeKind::IdleBreath),
        "aether.solver.idle_attractor" | "idle_attractor" => Some(NodeKind::IdleAttractor),
        "aether.solver.foot_planting" | "foot_planting" => Some(NodeKind::FootPlanting),

        "aether.expr.formant_lipsync" | "formant_lipsync" => Some(NodeKind::FormantLipSync),
        "aether.expr.trigger" | "expression_trigger" => Some(NodeKind::ExpressionTrigger),
        "aether.expr.mixer" | "expression_mixer" => Some(NodeKind::ExpressionMixer),
        "aether.expr.auto_blink" | "auto_blink" => Some(NodeKind::AutoBlink),
        "aether.expr.remap" | "blendshape_remap" => Some(NodeKind::BlendshapeRemap),

        "aether.math.add" | "add" => Some(NodeKind::Add),
        "aether.math.multiply" | "multiply" => Some(NodeKind::Multiply),
        "aether.math.lerp" | "lerp" => Some(NodeKind::Lerp),
        "aether.math.remap" | "remap" => Some(NodeKind::Remap),
        "aether.math.clamp" | "clamp" => Some(NodeKind::Clamp),
        "aether.math.smooth" | "smooth" => Some(NodeKind::Smooth),
        "aether.math.spring" | "spring" => Some(NodeKind::Spring),
        "aether.math.compare" | "compare" => Some(NodeKind::Compare),
        "aether.math.and" | "and" => Some(NodeKind::And),
        "aether.math.or" | "or" => Some(NodeKind::Or),
        "aether.math.not" | "not" => Some(NodeKind::Not),
        "aether.math.select" | "select" => Some(NodeKind::Select),
        "aether.math.delay" | "delay" => Some(NodeKind::Delay),
        "aether.math.random" | "random" => Some(NodeKind::Random),
        "aether.math.curve" | "curve" => Some(NodeKind::Curve),

        "aether.output.pose" | "output_pose" => Some(NodeKind::OutputPose),
        "aether.output.expressions" | "output_expressions" => Some(NodeKind::OutputExpressions),
        "aether.output.camera_control" | "camera_control" => Some(NodeKind::CameraControl),
        "aether.output.light_control" | "light_control" => Some(NodeKind::LightControl),
        "aether.output.prop_attach" | "prop_attach" => Some(NodeKind::PropAttach),
        "aether.output.osc_out" | "osc_out" => Some(NodeKind::OscOut),
        "aether.output.ws_out" | "ws_out" => Some(NodeKind::WsOut),

        "aether.model.source" | "model_source" => Some(NodeKind::ModelSource),
        "aether.material.slot_override" | "material_slot_override" => {
            Some(NodeKind::MaterialSlotOverride)
        }
        "aether.texture.asset" | "texture_asset" => Some(NodeKind::TextureAsset),
        "aether.texture.grade" | "texture_grade" => Some(NodeKind::TextureGrade),
        "aether.material.dynamic_driver" | "dynamic_material_driver" => {
            Some(NodeKind::DynamicMaterialDriver)
        }

        _ if base == "aether.plugin.wasm" || base.starts_with("plugin.") => {
            Some(NodeKind::WasmPlugin)
        }
        _ => None,
    }
}

fn p_in(name: &str, ty: ValueType) -> ResolvedPort {
    ResolvedPort {
        name: name.to_owned(),
        ty,
        direction: PortDirection::Input,
    }
}

fn p_out(name: &str, ty: ValueType) -> ResolvedPort {
    ResolvedPort {
        name: name.to_owned(),
        ty,
        direction: PortDirection::Output,
    }
}

/// Returns whether a node needs persistent state migrated across hot swaps (§7.3 step 5).
#[must_use]
pub const fn is_stateful_kind(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::ExpressionTrigger
            | NodeKind::AutoBlink
            | NodeKind::Smooth
            | NodeKind::Spring
            | NodeKind::Delay
            | NodeKind::Random
            | NodeKind::WasmPlugin
    )
}

/// Resolves port signatures for built-in nodes or merges declared ports for plugin/dynamic nodes.
#[must_use]
pub fn resolve_ports(kind: NodeKind, node: &GraphNode) -> Vec<ResolvedPort> {
    if !node.ports.is_empty() && matches!(kind, NodeKind::WasmPlugin) {
        return node
            .ports
            .iter()
            .map(|p: &PortDecl| ResolvedPort {
                name: p.name.clone(),
                ty: p.ty,
                direction: p.direction,
            })
            .collect();
    }

    let float_or_vec3 = || {
        if node
            .params
            .get("value_type")
            .and_then(serde_json::Value::as_str)
            .is_some_and(|s| s.eq_ignore_ascii_case("vec3"))
        {
            ValueType::Vec3
        } else {
            ValueType::Float
        }
    };

    match kind {
        NodeKind::NpuPose => vec![
            p_out("pose", ValueType::Pose),
            p_out("expressions", ValueType::ExprSet),
            p_out("face_conf", ValueType::Float),
            p_out("hand_conf", ValueType::Float),
            p_out("body_conf", ValueType::Float),
            p_out("ep_id", ValueType::Float),
        ],
        NodeKind::DspAudio => vec![
            p_out("audio", ValueType::AudioFrame),
            p_out("rms_db", ValueType::Float),
            p_out("voice_active", ValueType::Bool),
        ],
        NodeKind::Hotkey => vec![
            p_out("pressed", ValueType::Event),
            p_out("held", ValueType::Bool),
        ],
        NodeKind::Clock => vec![
            p_out("time_s", ValueType::Float),
            p_out("dt_s", ValueType::Float),
            p_out("tick", ValueType::Float),
        ],
        NodeKind::OscIn | NodeKind::WsIn => {
            vec![
                p_out("value", ValueType::Float),
                p_out("trigger", ValueType::Event),
            ]
        }

        NodeKind::VisibilityFirstIk => vec![
            p_in("pose", ValueType::Pose),
            p_in("face_conf", ValueType::Float),
            p_in("hand_conf", ValueType::Float),
            p_in("body_conf", ValueType::Float),
            p_out("pose", ValueType::Pose),
        ],
        NodeKind::LookAt => vec![
            p_in("target", ValueType::Vec3),
            p_in("weight", ValueType::Float),
            p_out("head_yaw", ValueType::Float),
            p_out("head_pitch", ValueType::Float),
        ],
        NodeKind::IdleBreath => vec![
            p_in("rate_hz", ValueType::Float),
            p_in("amplitude", ValueType::Float),
            p_out("pose", ValueType::Pose),
        ],
        NodeKind::IdleAttractor => vec![
            p_in("blend", ValueType::Float),
            p_out("pose", ValueType::Pose),
        ],
        NodeKind::FootPlanting => vec![
            p_in("pose", ValueType::Pose),
            p_in("floor_y", ValueType::Float),
            p_out("pose", ValueType::Pose),
        ],

        NodeKind::FormantLipSync => vec![
            p_in("audio", ValueType::AudioFrame),
            p_in("gain", ValueType::Float),
            p_out("visemes", ValueType::VisemeSet),
        ],
        NodeKind::ExpressionTrigger => vec![
            p_in("trigger", ValueType::Event),
            p_out("expressions", ValueType::ExprSet),
            p_out("active", ValueType::Float),
        ],
        NodeKind::ExpressionMixer => vec![
            p_in("a", ValueType::ExprSet),
            p_in("b", ValueType::ExprSet),
            p_in("weight", ValueType::Float),
            p_out("expressions", ValueType::ExprSet),
        ],
        NodeKind::AutoBlink => vec![
            p_in("face_conf", ValueType::Float),
            p_in("interval_s", ValueType::Float),
            p_out("expressions", ValueType::ExprSet),
            p_out("blink", ValueType::Float),
        ],
        NodeKind::BlendshapeRemap => vec![
            p_in("expressions", ValueType::ExprSet),
            p_out("expressions", ValueType::ExprSet),
        ],

        NodeKind::Add | NodeKind::Multiply => {
            let ty = float_or_vec3();
            vec![p_in("a", ty), p_in("b", ty), p_out("out", ty)]
        }
        NodeKind::Lerp => {
            let ty = node
                .params
                .get("value_type")
                .and_then(serde_json::Value::as_str)
                .map_or(ValueType::Float, |s| match s {
                    "vec3" => ValueType::Vec3,
                    "quat" => ValueType::Quat,
                    _ => ValueType::Float,
                });
            vec![
                p_in("a", ty),
                p_in("b", ty),
                p_in("t", ValueType::Float),
                p_out("out", ty),
            ]
        }
        NodeKind::Remap
        | NodeKind::Clamp
        | NodeKind::Smooth
        | NodeKind::Spring
        | NodeKind::Delay
        | NodeKind::Curve => vec![p_in("in", ValueType::Float), p_out("out", ValueType::Float)],
        NodeKind::Compare => vec![
            p_in("a", ValueType::Float),
            p_in("b", ValueType::Float),
            p_out("out", ValueType::Bool),
        ],
        NodeKind::And | NodeKind::Or => vec![
            p_in("a", ValueType::Bool),
            p_in("b", ValueType::Bool),
            p_out("out", ValueType::Bool),
        ],
        NodeKind::Not => vec![p_in("in", ValueType::Bool), p_out("out", ValueType::Bool)],
        NodeKind::Select => vec![
            p_in("cond", ValueType::Bool),
            p_in("a", ValueType::Float),
            p_in("b", ValueType::Float),
            p_out("out", ValueType::Float),
        ],
        NodeKind::Random => vec![
            p_in("trigger", ValueType::Event),
            p_out("out", ValueType::Float),
        ],

        NodeKind::OutputPose => vec![p_in("pose", ValueType::Pose)],
        NodeKind::OutputExpressions => vec![p_in("expressions", ValueType::ExprSet)],
        NodeKind::CameraControl => {
            vec![
                p_in("focal_mm", ValueType::Float),
                p_in("flatness", ValueType::Float),
            ]
        }
        NodeKind::LightControl => {
            vec![
                p_in("direction", ValueType::Vec3),
                p_in("intensity", ValueType::Float),
            ]
        }
        NodeKind::PropAttach => vec![p_in("transform", ValueType::Transform)],
        NodeKind::OscOut | NodeKind::WsOut => {
            vec![
                p_in("value", ValueType::Float),
                p_in("trigger", ValueType::Event),
            ]
        }

        NodeKind::ModelSource => vec![
            p_out("skeleton_pose", ValueType::Pose),
            p_out("morph_bus", ValueType::ExprSet),
            p_out("material_table", ValueType::MaterialTable),
            p_out("material_count", ValueType::Float),
        ],
        NodeKind::MaterialSlotOverride => vec![
            p_in("material_table", ValueType::MaterialTable),
            p_in("texture", ValueType::TextureHandle),
            p_in("shade_shift", ValueType::Float),
            p_in("sss_fringe", ValueType::Float),
            p_in("outline_scale", ValueType::Float),
            p_in("dynamic_driver", ValueType::MaterialOverride),
            p_out("override_out", ValueType::MaterialOverride),
            p_out("slot_index", ValueType::Float),
        ],
        NodeKind::TextureAsset => vec![
            p_out("texture", ValueType::TextureHandle),
            p_out("ready", ValueType::Bool),
        ],
        NodeKind::TextureGrade => vec![
            p_in("texture", ValueType::TextureHandle),
            p_in("hue_shift", ValueType::Float),
            p_in("saturation", ValueType::Float),
            p_in("brightness", ValueType::Float),
            p_in("contrast", ValueType::Float),
            p_in("tint", ValueType::Color),
            p_out("texture", ValueType::TextureHandle),
        ],
        NodeKind::DynamicMaterialDriver => vec![
            p_in("signal_in", ValueType::Float),
            p_in("audio_rms", ValueType::Float),
            p_in("expression_weight", ValueType::Float),
            p_in("circadian_clock", ValueType::Float),
            p_in("material_table", ValueType::MaterialTable),
            p_out("driver_out", ValueType::MaterialOverride),
            p_out("driven_value", ValueType::Float),
        ],

        NodeKind::WasmPlugin => {
            if !node.ports.is_empty() {
                return node
                    .ports
                    .iter()
                    .map(|p| ResolvedPort {
                        name: p.name.clone(),
                        ty: p.ty,
                        direction: p.direction,
                    })
                    .collect();
            }
            vec![p_in("in", ValueType::Float), p_out("out", ValueType::Float)]
        }
    }
}
