//! Authoring graph document schema and value type system (§7.2).

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

/// Value types flowing across runtime node graph wires (§7.2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ValueType {
    /// Edge-triggered tick count (`u32`), rendered with diamond pin.
    Event,
    /// Boolean flag (`0` / `1`), implicitly converts to `Float`.
    Bool,
    /// Scalar 32-bit float; implicitly converts to `Bool` or broadcasts to `Vec3`.
    Float,
    /// 3D vector `[x, y, z]`.
    Vec3,
    /// Unit quaternion `[x, y, z, w]`.
    Quat,
    /// Affine transform `[pos(3), rot(4), scale(3)]`; implicitly converts to `Vec3` (`pos`).
    Transform,
    /// Full Appendix A 55-joint pose (`pos`, `rot`, confidence).
    Pose,
    /// 64-channel Appendix B expression weights + override mask.
    ExprSet,
    /// 5-channel DSP lip-sync visemes `A, I, U, E, O`.
    VisemeSet,
    /// Linear RGBA color `[r, g, b, a]`.
    Color,
    /// Compile-time configuration parameter only (forbidden on runtime data wires).
    String,
}

impl ValueType {
    /// Returns whether an edge from `from` output type to `to` input type is valid under §7.2 coercion rules.
    #[must_use]
    pub const fn can_connect(from: Self, to: Self) -> bool {
        if matches!((from, to), (Self::String, _) | (_, Self::String)) {
            return false;
        }
        if matches!(
            (from, to),
            (Self::Event, Self::Event)
                | (Self::Bool | Self::Float, Self::Bool | Self::Float)
                | (Self::Vec3 | Self::Float | Self::Transform, Self::Vec3)
                | (Self::Quat, Self::Quat)
                | (Self::Transform, Self::Transform)
                | (Self::Pose, Self::Pose)
                | (Self::ExprSet, Self::ExprSet)
                | (Self::VisemeSet, Self::VisemeSet)
                | (Self::Color, Self::Color)
        ) {
            return true;
        }
        false
    }
}

/// Port direction inside a node graph descriptor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PortDirection {
    /// Input pin.
    Input,
    /// Output pin.
    Output,
}

/// Optional custom port declaration stored on authoring JSON nodes (required for WASM plugin nodes).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PortDecl {
    /// Port identifier (unique within the node).
    pub name: String,
    /// Port value type.
    #[serde(rename = "type", alias = "ty")]
    pub ty: ValueType,
    /// Port direction.
    pub direction: PortDirection,
}

/// Authored graph node instance (§7.2).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct GraphNode {
    /// Stable user-assigned node identifier (`>= 1` recommended; preserved across hot swap).
    pub id: u64,
    /// Versioned node type identifier (e.g. `"aether.ik.visibility_first@1"`).
    #[serde(rename = "type")]
    pub type_id: String,
    /// Canvas `[x, y]` coordinate in graph space.
    #[serde(default)]
    pub position: [f32; 2],
    /// Optional human-readable label override.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label: Option<String>,
    /// Typed parameters stored per node (scalars, strings, curve LUTs, targets).
    #[serde(default)]
    pub params: BTreeMap<String, serde_json::Value>,
    /// Explicit port list (used by dynamic plugin nodes or serialized tooling).
    #[serde(default)]
    pub ports: Vec<PortDecl>,
    /// Preserved unknown JSON fields for forward compatibility.
    #[serde(flatten)]
    pub extra: BTreeMap<String, serde_json::Value>,
}

/// Directed data connection from an output port to an input port.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct GraphEdge {
    /// Edge identifier.
    #[serde(default)]
    pub id: u64,
    /// Producer node ID.
    pub from_node: u64,
    /// Producer output port name.
    pub from_port: String,
    /// Consumer node ID.
    pub to_node: u64,
    /// Consumer input port name.
    pub to_port: String,
}

/// Visual canvas group box preserved alongside the authoring document (§7.7).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct GraphGroup {
    /// Group ID.
    pub id: u64,
    /// Group title.
    pub title: String,
    /// Top-left position.
    #[serde(default)]
    pub position: [f32; 2],
    /// Box width/height.
    #[serde(default)]
    pub size: [f32; 2],
    /// Member node IDs.
    #[serde(default)]
    pub nodes: Vec<u64>,
}

/// Canvas annotation sticky comment preserved alongside the authoring document (§7.7).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct GraphComment {
    /// Comment ID.
    pub id: u64,
    /// UTF-8 comment text.
    pub text: String,
    /// Position on canvas.
    #[serde(default)]
    pub position: [f32; 2],
}

/// Full serialized `graph.json` authoring document (§7.7).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct GraphDocument {
    /// Schema revision (`1` in v1.0).
    #[serde(default = "default_schema_version")]
    pub schema_version: u32,
    /// Graph nodes.
    #[serde(default)]
    pub nodes: Vec<GraphNode>,
    /// Graph edges.
    #[serde(default)]
    pub edges: Vec<GraphEdge>,
    /// Visual group frames.
    #[serde(default)]
    pub groups: Vec<GraphGroup>,
    /// Visual comment annotations.
    #[serde(default)]
    pub comments: Vec<GraphComment>,
    /// Unknown JSON fields preserved cleanly across versions.
    #[serde(flatten)]
    pub extra: BTreeMap<String, serde_json::Value>,
}

const fn default_schema_version() -> u32 {
    1
}

impl Default for GraphDocument {
    fn default() -> Self {
        Self::default_project_graph()
    }
}

impl GraphDocument {
    /// Constructs the exact MTD default graph (§7.4):
    /// `[NPU Pose] -> [Visibility-First IK] -> [Output Pose]` and
    /// `[DSP Audio] -> [Formant LipSync] -> [Output Expressions]`.
    #[must_use]
    pub fn default_project_graph() -> Self {
        Self {
            schema_version: 1,
            nodes: vec![
                GraphNode {
                    id: 1,
                    type_id: "aether.source.npu_pose@1".to_owned(),
                    position: [60.0, 80.0],
                    label: Some("NPU Pose".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
                GraphNode {
                    id: 2,
                    type_id: "aether.ik.visibility_first@1".to_owned(),
                    position: [340.0, 80.0],
                    label: Some("Visibility-First IK".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
                GraphNode {
                    id: 3,
                    type_id: "aether.output.pose@1".to_owned(),
                    position: [640.0, 80.0],
                    label: Some("Output Pose".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
                GraphNode {
                    id: 4,
                    type_id: "aether.source.dsp_audio@1".to_owned(),
                    position: [60.0, 280.0],
                    label: Some("DSP Audio".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
                GraphNode {
                    id: 5,
                    type_id: "aether.expr.formant_lipsync@1".to_owned(),
                    position: [340.0, 280.0],
                    label: Some("Formant LipSync".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
                GraphNode {
                    id: 6,
                    type_id: "aether.output.expressions@1".to_owned(),
                    position: [640.0, 280.0],
                    label: Some("Output Expressions".to_owned()),
                    params: BTreeMap::new(),
                    ports: Vec::new(),
                    extra: BTreeMap::new(),
                },
            ],
            edges: vec![
                GraphEdge {
                    id: 1,
                    from_node: 1,
                    from_port: "pose".to_owned(),
                    to_node: 2,
                    to_port: "pose".to_owned(),
                },
                GraphEdge {
                    id: 2,
                    from_node: 1,
                    from_port: "face_conf".to_owned(),
                    to_node: 2,
                    to_port: "face_conf".to_owned(),
                },
                GraphEdge {
                    id: 3,
                    from_node: 1,
                    from_port: "hand_conf".to_owned(),
                    to_node: 2,
                    to_port: "hand_conf".to_owned(),
                },
                GraphEdge {
                    id: 4,
                    from_node: 1,
                    from_port: "body_conf".to_owned(),
                    to_node: 2,
                    to_port: "body_conf".to_owned(),
                },
                GraphEdge {
                    id: 5,
                    from_node: 2,
                    from_port: "pose".to_owned(),
                    to_node: 3,
                    to_port: "pose".to_owned(),
                },
                GraphEdge {
                    id: 6,
                    from_node: 4,
                    from_port: "visemes".to_owned(),
                    to_node: 5,
                    to_port: "visemes".to_owned(),
                },
                GraphEdge {
                    id: 7,
                    from_node: 5,
                    from_port: "expressions".to_owned(),
                    to_node: 6,
                    to_port: "expressions".to_owned(),
                },
            ],
            groups: Vec::new(),
            comments: Vec::new(),
            extra: BTreeMap::new(),
        }
    }
}
