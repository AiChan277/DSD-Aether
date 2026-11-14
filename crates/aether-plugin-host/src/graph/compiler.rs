//! Node graph compiler: type validation, implicit coercion lowering, Kahn sort, cycle detection,
//! constant folding, flat arena allocation, and hot-swap state migration (§7.2–§7.3).

use std::collections::{BTreeMap, HashMap, VecDeque};

use crate::error::HostError;
use crate::graph::builtins::{
    NodeKind, ResolvedPort, classify_node_type, is_stateful_kind, resolve_ports,
};
use crate::graph::eval::LATCHED_IO_FLOATS;
use crate::graph::model::{GraphDocument, GraphEdge, GraphNode, PortDirection, ValueType};

/// Pose payload matching Appendix A order (`55` joints).
#[derive(Clone, Copy, Debug)]
pub struct PoseData {
    /// World positions `xyz` + confidence `w`.
    pub pos: [[f32; 4]; 55],
    /// Local rotations `xyzw`.
    pub rot: [[f32; 4]; 55],
    /// `true` when this pose originates from the live NPU tracking frame (`NpuPose`) rather than
    /// an additive procedural source. `OutputPose` then emits only the graph's *changes* relative to
    /// tracking, because the C++ Visibility-First solver already consumes (and filters) tracking itself.
    pub from_tracking: bool,
}

impl Default for PoseData {
    fn default() -> Self {
        Self {
            pos: [[0.0; 4]; 55],
            rot: [[0.0, 0.0, 0.0, 1.0]; 55],
            from_tracking: false,
        }
    }
}

/// 64-channel Appendix B expression weights + override mask.
#[derive(Clone, Copy, Debug)]
pub struct ExprSetData {
    /// Blend weights in `[0, 1]`.
    pub weights: [f32; 64],
    /// Override masks (`0` = additive/keep base, `1` = override).
    pub override_mask: [f32; 64],
}

impl Default for ExprSetData {
    fn default() -> Self {
        Self {
            weights: [0.0; 64],
            override_mask: [0.0; 64],
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct AudioFrameData {
    pub env: f32,
    pub rms_db: f32,
    pub voiced: f32,
    pub f1_hz: f32,
    pub f2_hz: f32,
    pub visemes: [f32; 5],
}

/// Type-segregated flat slot arena sized once at compile time (`HR-01`).
#[derive(Clone, Debug, Default)]
pub struct ValuesArena {
    pub events: Vec<u32>,
    pub bools: Vec<u32>,
    pub floats: Vec<f32>,
    pub vec3s: Vec<[f32; 3]>,
    pub quats: Vec<[f32; 4]>,
    pub transforms: Vec<[f32; 10]>,
    pub poses: Vec<PoseData>,
    pub expr_sets: Vec<ExprSetData>,
    pub viseme_sets: Vec<[f32; 5]>,
    pub audio_frames: Vec<AudioFrameData>,
    pub colors: Vec<[f32; 4]>,
    pub luts: Vec<[f32; 64]>,
}

/// Typed slot reference inside [`ValuesArena`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlotRef {
    Event(usize),
    Bool(usize),
    Float(usize),
    Vec3(usize),
    Quat(usize),
    Transform(usize),
    Pose(usize),
    ExprSet(usize),
    VisemeSet(usize),
    AudioFrame(usize),
    Color(usize),
}

impl SlotRef {
    const fn as_usize(self) -> usize {
        match self {
            Self::Event(i)
            | Self::Bool(i)
            | Self::Float(i)
            | Self::Vec3(i)
            | Self::Quat(i)
            | Self::Transform(i)
            | Self::Pose(i)
            | Self::ExprSet(i)
            | Self::VisemeSet(i)
            | Self::AudioFrame(i)
            | Self::Color(i) => i,
        }
    }
}

impl ValuesArena {
    fn alloc_slot(&mut self, ty: ValueType, init_f32: Option<f32>) -> Result<SlotRef, HostError> {
        match ty {
            ValueType::Event => {
                let idx = self.events.len();
                self.events.push(0);
                Ok(SlotRef::Event(idx))
            }
            ValueType::Bool => {
                let idx = self.bools.len();
                self.bools
                    .push(u32::from(init_f32.unwrap_or(0.0).abs() > 0.5));
                Ok(SlotRef::Bool(idx))
            }
            ValueType::Float => {
                let idx = self.floats.len();
                self.floats.push(init_f32.unwrap_or(0.0));
                Ok(SlotRef::Float(idx))
            }
            ValueType::Vec3 => {
                let idx = self.vec3s.len();
                let v = init_f32.unwrap_or(0.0);
                self.vec3s.push([v, v, v]);
                Ok(SlotRef::Vec3(idx))
            }
            ValueType::Quat => {
                let idx = self.quats.len();
                self.quats.push([0.0, 0.0, 0.0, 1.0]);
                Ok(SlotRef::Quat(idx))
            }
            ValueType::Transform => {
                let idx = self.transforms.len();
                self.transforms
                    .push([0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0]);
                Ok(SlotRef::Transform(idx))
            }
            ValueType::Pose => {
                let idx = self.poses.len();
                self.poses.push(PoseData::default());
                Ok(SlotRef::Pose(idx))
            }
            ValueType::ExprSet => {
                let idx = self.expr_sets.len();
                self.expr_sets.push(ExprSetData::default());
                Ok(SlotRef::ExprSet(idx))
            }
            ValueType::VisemeSet => {
                let idx = self.viseme_sets.len();
                self.viseme_sets.push([0.0; 5]);
                Ok(SlotRef::VisemeSet(idx))
            }
            ValueType::AudioFrame => {
                let idx = self.audio_frames.len();
                self.audio_frames.push(AudioFrameData::default());
                Ok(SlotRef::AudioFrame(idx))
            }
            ValueType::Color => {
                let idx = self.colors.len();
                self.colors.push([1.0; 4]);
                Ok(SlotRef::Color(idx))
            }
            ValueType::String => Err(HostError::parse(
                "ValueType::String is config-only and cannot have a runtime slot",
            )),
        }
    }
}

/// Comparison mode for `Compare` node.
#[derive(Clone, Copy, Debug)]
pub enum CompareOp {
    Gt,
    Lt,
    Ge,
    Le,
    Eq,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SmoothState {
    pub prev: f32,
    pub initialized: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SpringState {
    pub pos: f32,
    pub vel: f32,
    pub initialized: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct DelayState {
    pub prev: f32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct TriggerState {
    pub active: bool,
    pub elapsed_s: f32,
    pub prev_event: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct AutoBlinkState {
    pub timer_s: f32,
    pub blink_s: f32,
}

#[derive(Clone, Copy, Debug)]
pub struct RandomState {
    pub seed: u64,
    pub last: f32,
}

impl Default for RandomState {
    fn default() -> Self {
        Self {
            seed: 0x1234_5678_9ABC_DEF0,
            last: 0.0,
        }
    }
}

/// Persistent per-node state migrated by `(node_id, type_id)` across hot swaps (§7.3 step 5).
#[derive(Clone, Copy, Debug)]
pub enum NodeState {
    Smooth(SmoothState),
    Spring(SpringState),
    Delay(DelayState),
    Trigger(TriggerState),
    AutoBlink(AutoBlinkState),
    Random(RandomState),
    Latched { instance_slot: usize },
}

/// Linear allocation-free instruction emitted by the graph compiler (§7.3 step 3).
#[derive(Clone, Debug)]
pub enum Op {
    CopyFloat {
        src: usize,
        dst: usize,
    },
    CopyBool {
        src: usize,
        dst: usize,
    },
    CopyEvent {
        src: usize,
        dst: usize,
    },
    CopyVec3 {
        src: usize,
        dst: usize,
    },
    CopyQuat {
        src: usize,
        dst: usize,
    },
    CopyTransform {
        src: usize,
        dst: usize,
    },
    CopyPose {
        src: usize,
        dst: usize,
    },
    CopyExprSet {
        src: usize,
        dst: usize,
    },
    CopyVisemeSet {
        src: usize,
        dst: usize,
    },
    CopyAudioFrame {
        src: usize,
        dst: usize,
    },
    VisemeToExprSet {
        src: usize,
        dst: usize,
    },
    CopyColor {
        src: usize,
        dst: usize,
    },
    CastBoolToFloat {
        src: usize,
        dst: usize,
    },
    CastFloatToBool {
        src: usize,
        dst: usize,
    },
    BroadcastFloatToVec3 {
        src: usize,
        dst: usize,
    },
    TransformPosToVec3 {
        src: usize,
        dst: usize,
    },
    NpuPose {
        out_pose: usize,
        out_expr: usize,
        out_face_conf: usize,
        out_hand_conf: usize,
        out_body_conf: usize,
        out_ep_id: usize,
    },
    DspAudio {
        out_audio: usize,
        out_rms_db: usize,
        out_voice_active: usize,
    },
    Hotkey {
        key_code: u32,
        out_pressed: usize,
        out_held: usize,
    },
    Clock {
        out_time_s: usize,
        out_dt_s: usize,
        out_tick: usize,
    },
    ExternalParam {
        slot: usize,
        hotkey_slot: u32,
        out_value: usize,
        out_trigger: usize,
    },
    VisibilityFirstIk {
        in_pose: usize,
        in_hand_conf: usize,
        in_body_conf: usize,
        out_pose: usize,
    },
    LookAt {
        in_target: usize,
        in_weight: usize,
        out_yaw: usize,
        out_pitch: usize,
    },
    IdleBreath {
        in_rate_hz: usize,
        in_amp: usize,
        out_pose: usize,
    },
    IdleAttractor {
        in_blend: usize,
        out_pose: usize,
    },
    FootPlanting {
        in_pose: usize,
        in_floor_y: usize,
        out_pose: usize,
    },
    FormantLipSync {
        in_audio: usize,
        in_gain: usize,
        out_visemes: usize,
    },
    ExpressionTrigger {
        in_trigger: usize,
        slot: usize,
        fade_in_ms: f32,
        hold_ms: f32,
        fade_out_ms: f32,
        target_weight: f32,
        state_idx: usize,
        out_expr: usize,
        out_active: usize,
    },
    ExpressionMixer {
        in_a: usize,
        in_b: usize,
        in_weight: usize,
        out_expr: usize,
    },
    AutoBlink {
        in_face_conf: usize,
        in_interval_s: usize,
        state_idx: usize,
        out_expr: usize,
        out_blink: usize,
    },
    BlendshapeRemap {
        in_expr: usize,
        slot: usize,
        in_min: f32,
        in_max: f32,
        out_min: f32,
        out_max: f32,
        out_expr: usize,
    },
    AddFloat {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    AddVec3 {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    MulFloat {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    MulVec3 {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    LerpFloat {
        in_a: usize,
        in_b: usize,
        in_t: usize,
        out: usize,
    },
    LerpVec3 {
        in_a: usize,
        in_b: usize,
        in_t: usize,
        out: usize,
    },
    LerpQuat {
        in_a: usize,
        in_b: usize,
        in_t: usize,
        out: usize,
    },
    Remap {
        src: usize,
        in_min: f32,
        in_max: f32,
        out_min: f32,
        out_max: f32,
        clamp: bool,
        dst: usize,
    },
    Clamp {
        src: usize,
        min_v: f32,
        max_v: f32,
        dst: usize,
    },
    Smooth {
        src: usize,
        cutoff_hz: f32,
        state_idx: usize,
        dst: usize,
    },
    Spring {
        src: usize,
        stiffness: f32,
        damping: f32,
        state_idx: usize,
        dst: usize,
    },
    Compare {
        in_a: usize,
        in_b: usize,
        cmp: CompareOp,
        out: usize,
    },
    And {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    Or {
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    Not {
        src: usize,
        out: usize,
    },
    Select {
        in_cond: usize,
        in_a: usize,
        in_b: usize,
        out: usize,
    },
    DelayRead {
        state_idx: usize,
        dst: usize,
    },
    DelayWrite {
        src: usize,
        state_idx: usize,
    },
    Random {
        in_trigger: usize,
        state_idx: usize,
        dst: usize,
    },
    Curve {
        src: usize,
        lut_idx: usize,
        dst: usize,
    },
    OutputPose {
        in_pose: usize,
    },
    OutputExpressions {
        in_expr: usize,
    },
    CameraControl {
        in_focal: usize,
        in_flat: usize,
    },
    LightControl {
        in_dir: usize,
        in_intensity: usize,
    },
    PropAttach {
        in_transform: usize,
    },
    AsyncOutputSignal {
        in_val: usize,
        in_trigger: usize,
        event_tag: u32,
    },
    Latched {
        instance_slot: usize,
        in_floats: Box<[Option<usize>; LATCHED_IO_FLOATS]>,
        out_floats: Box<[Option<usize>; LATCHED_IO_FLOATS]>,
    },
}

/// Active WASM latched instance metadata attached to a compiled plan so background workers know which slots are live.
#[derive(Clone, Debug)]
pub struct LatchedBinding {
    /// Instance slot `[0..64)` in `LatchedBridge`.
    pub instance_slot: usize,
    /// Graph node ID.
    pub node_id: u64,
    /// Full plugin/node type ID string (`aether.plugin.wasm@1` or `<plugin_id>::<node_type>`).
    pub type_id: String,
    /// Number of scalar inputs/outputs.
    pub input_count: u32,
    pub output_count: u32,
}

/// Output pin readout mapping precomputed at compile time (`T2` hot path reads without allocating).
#[derive(Clone, Copy, Debug)]
pub struct CompiledPinSlot {
    pub(crate) node_id: u64,
    pub(crate) pin_idx: u8,
    pub(crate) slot: Option<SlotRef>,
}

/// Allocation-free compiled graph evaluation plan executed on `T2` (§7.3).
#[derive(Clone, Debug)]
pub struct CompiledGraphPlan {
    pub(crate) arena: ValuesArena,
    pub(crate) ops: Vec<Op>,
    pub(crate) states: Vec<NodeState>,
    pub(crate) state_keys: Vec<(u64, String)>,
    pub(crate) node_ids: Vec<u64>,
    pub(crate) op_to_node_slot: Vec<Option<usize>>,
    pub(crate) node_eval_ns: Vec<u32>,
    pub(crate) node_eval_us: Vec<f32>,
    pub(crate) node_live_values: Vec<f32>,
    pub(crate) last_total_eval_ns: u64,
    pub(crate) output_pins: Vec<CompiledPinSlot>,
    pub(crate) pin_kinds: Vec<u8>,
    pub(crate) pin_values: Vec<[f32; 4]>,
    pub(crate) latched_bindings: Vec<LatchedBinding>,
}

impl CompiledGraphPlan {
    /// Returns an allocation-free default plan used before a custom graph is posted.
    #[must_use]
    pub fn empty_fallback() -> Self {
        Self {
            arena: ValuesArena::default(),
            ops: Vec::new(),
            states: Vec::new(),
            state_keys: Vec::new(),
            node_ids: Vec::new(),
            op_to_node_slot: Vec::new(),
            node_eval_ns: Vec::new(),
            node_eval_us: Vec::new(),
            node_live_values: Vec::new(),
            last_total_eval_ns: 0,
            output_pins: Vec::new(),
            pin_kinds: Vec::new(),
            pin_values: Vec::new(),
            latched_bindings: Vec::new(),
        }
    }

    /// Deterministic no-op graph plan strictly under `test-hooks` to verify zero allocations (`HR-01`).
    #[cfg(feature = "test-hooks")]
    #[must_use]
    pub fn new_test_noop() -> Self {
        Self::empty_fallback()
    }

    /// Clones `(node_id, type_id, state)` tuples for hot-swap state migration.
    #[must_use]
    pub fn clone_states(&self) -> Vec<(u64, String, NodeState)> {
        self.state_keys
            .iter()
            .zip(self.states.iter())
            .map(|((id, ty), st)| (*id, ty.clone(), *st))
            .collect()
    }

    /// Returns latched WASM bindings registered in this compiled plan.
    #[must_use]
    pub fn latched_bindings(&self) -> &[LatchedBinding] {
        &self.latched_bindings
    }
}

fn param_f32(node: &GraphNode, key: &str, default: f32) -> f32 {
    node.params
        .get(key)
        .and_then(serde_json::Value::as_f64)
        .map_or(default, |v| {
            let f = v as f32;
            if f.is_finite() { f } else { default }
        })
}

fn param_u32(node: &GraphNode, key: &str, default: u32) -> u32 {
    node.params
        .get(key)
        .and_then(serde_json::Value::as_u64)
        .and_then(|v| u32::try_from(v).ok())
        .unwrap_or(default)
}

fn param_bool(node: &GraphNode, key: &str, default: bool) -> bool {
    node.params
        .get(key)
        .and_then(serde_json::Value::as_bool)
        .unwrap_or(default)
}

fn default_param_for_input(node: &GraphNode, port_name: &str) -> Option<f32> {
    if let Some(v) = node
        .params
        .get(port_name)
        .and_then(serde_json::Value::as_f64)
    {
        let f = v as f32;
        if f.is_finite() {
            return Some(f);
        }
    }
    match port_name {
        "gain" | "weight" | "amplitude" | "rate_hz" | "intensity" | "interval_s" => {
            Some(match port_name {
                "amplitude" => 0.05,
                "interval_s" => 4.0,
                _ => 1.0,
            })
        }
        "focal_mm" => Some(50.0),
        "flatness" => Some(0.45),
        _ => None,
    }
}

fn emit_copy_or_coerce(
    ops: &mut Vec<Op>,
    op_slots: &mut Vec<Option<usize>>,
    src: SlotRef,
    dst: SlotRef,
) -> Result<(), HostError> {
    match (src, dst) {
        (SlotRef::Float(s), SlotRef::Float(d)) => ops.push(Op::CopyFloat { src: s, dst: d }),
        (SlotRef::Bool(s), SlotRef::Bool(d)) => ops.push(Op::CopyBool { src: s, dst: d }),
        (SlotRef::Event(s), SlotRef::Event(d)) => ops.push(Op::CopyEvent { src: s, dst: d }),
        (SlotRef::Vec3(s), SlotRef::Vec3(d)) => ops.push(Op::CopyVec3 { src: s, dst: d }),
        (SlotRef::Quat(s), SlotRef::Quat(d)) => ops.push(Op::CopyQuat { src: s, dst: d }),
        (SlotRef::Transform(s), SlotRef::Transform(d)) => {
            ops.push(Op::CopyTransform { src: s, dst: d });
        }
        (SlotRef::Pose(s), SlotRef::Pose(d)) => ops.push(Op::CopyPose { src: s, dst: d }),
        (SlotRef::ExprSet(s), SlotRef::ExprSet(d)) => {
            ops.push(Op::CopyExprSet { src: s, dst: d });
        }
        (SlotRef::VisemeSet(s), SlotRef::VisemeSet(d)) => {
            ops.push(Op::CopyVisemeSet { src: s, dst: d });
        }
        (SlotRef::VisemeSet(s), SlotRef::ExprSet(d)) => {
            ops.push(Op::VisemeToExprSet { src: s, dst: d });
        }
        (SlotRef::AudioFrame(s), SlotRef::AudioFrame(d)) => {
            ops.push(Op::CopyAudioFrame { src: s, dst: d });
        }
        (SlotRef::Color(s), SlotRef::Color(d)) => ops.push(Op::CopyColor { src: s, dst: d }),
        (SlotRef::Bool(s), SlotRef::Float(d)) => ops.push(Op::CastBoolToFloat { src: s, dst: d }),
        (SlotRef::Float(s), SlotRef::Bool(d)) => ops.push(Op::CastFloatToBool { src: s, dst: d }),
        (SlotRef::Float(s), SlotRef::Vec3(d)) => {
            ops.push(Op::BroadcastFloatToVec3 { src: s, dst: d });
        }
        (SlotRef::Transform(s), SlotRef::Vec3(d)) => {
            ops.push(Op::TransformPosToVec3 { src: s, dst: d });
        }
        _ => {
            return Err(HostError::parse("incompatible slot coercion"));
        }
    }
    op_slots.push(None);
    Ok(())
}

/// Validates types, breaks cycles strictly at `Delay` nodes via Kahn topological sort,
/// allocates flat arena slots, and emits an allocation-free [`CompiledGraphPlan`] (§7.2–§7.3).
pub fn compile_graph_document(
    doc: &GraphDocument,
    prev_states: &[(u64, String, NodeState)],
) -> Result<CompiledGraphPlan, HostError> {
    if doc.nodes.len() > 1024 {
        return Err(HostError::LimitExceeded(
            "node graph exceeds maximum of 1024 nodes".to_owned(),
        ));
    }

    let mut id_to_idx: HashMap<u64, usize> = HashMap::with_capacity(doc.nodes.len());
    let mut kinds: Vec<NodeKind> = Vec::with_capacity(doc.nodes.len());
    let mut ports_per_node: Vec<Vec<ResolvedPort>> = Vec::with_capacity(doc.nodes.len());

    for (idx, node) in doc.nodes.iter().enumerate() {
        if id_to_idx.insert(node.id, idx).is_some() {
            return Err(HostError::compile(node.id, "duplicate node ID in graph"));
        }
        let kind = classify_node_type(&node.type_id).ok_or_else(|| {
            HostError::compile(node.id, format!("unknown node type '{}'", node.type_id))
        })?;
        let resolved = resolve_ports(kind, node);
        kinds.push(kind);
        ports_per_node.push(resolved);
    }

    let normalize_compat_port = |node_idx: usize, dir: PortDirection, port_name: &str| -> String {
        if ports_per_node[node_idx]
            .iter()
            .any(|p| p.direction == dir && p.name == port_name)
        {
            return port_name.to_owned();
        }
        match (kinds[node_idx], dir, port_name) {
            (NodeKind::DspAudio, PortDirection::Output, "visemes")
            | (NodeKind::FormantLipSync, PortDirection::Input, "visemes") => "audio".to_owned(),
            (NodeKind::FormantLipSync, PortDirection::Output, "expressions") => {
                "visemes".to_owned()
            }
            _ => port_name.to_owned(),
        }
    };
    let mut normalized_edges: Vec<GraphEdge> = Vec::with_capacity(doc.edges.len());
    for edge in &doc.edges {
        let from_idx = *id_to_idx.get(&edge.from_node).ok_or_else(|| {
            HostError::compile(edge.from_node, "edge references missing source node")
        })?;
        let to_idx = *id_to_idx.get(&edge.to_node).ok_or_else(|| {
            HostError::compile(edge.to_node, "edge references missing target node")
        })?;
        normalized_edges.push(GraphEdge {
            id: edge.id,
            from_node: edge.from_node,
            from_port: normalize_compat_port(from_idx, PortDirection::Output, &edge.from_port),
            to_node: edge.to_node,
            to_port: normalize_compat_port(to_idx, PortDirection::Input, &edge.to_port),
        });
    }

    // Validate edges, types, String runtime wire ban, OutputPose strict Pose rule, and single driver per input.
    let mut driven_inputs: HashMap<(u64, String), usize> =
        HashMap::with_capacity(normalized_edges.len());
    let mut incoming_for_node: Vec<Vec<&GraphEdge>> = vec![Vec::new(); doc.nodes.len()];
    let mut adj: Vec<Vec<usize>> = vec![Vec::new(); doc.nodes.len()];
    let mut in_degree: Vec<usize> = vec![0; doc.nodes.len()];

    for (edge_idx, edge) in normalized_edges.iter().enumerate() {
        let from_idx = id_to_idx[&edge.from_node];
        let to_idx = id_to_idx[&edge.to_node];

        let from_port = ports_per_node[from_idx]
            .iter()
            .find(|p| p.direction == PortDirection::Output && p.name == edge.from_port)
            .ok_or_else(|| {
                HostError::compile(
                    edge.from_node,
                    format!("missing output port '{}'", edge.from_port),
                )
            })?;
        let to_port = ports_per_node[to_idx]
            .iter()
            .find(|p| p.direction == PortDirection::Input && p.name == edge.to_port)
            .ok_or_else(|| {
                HostError::compile(
                    edge.to_node,
                    format!("missing input port '{}'", edge.to_port),
                )
            })?;

        if from_port.ty == ValueType::String || to_port.ty == ValueType::String {
            return Err(HostError::compile(
                edge.to_node,
                "String parameters cannot carry runtime data edges (§7.2)",
            ));
        }
        if kinds[to_idx] == NodeKind::OutputPose && from_port.ty != ValueType::Pose {
            return Err(HostError::compile(
                edge.to_node,
                "Only Pose type may feed Output Pose (§7.2)",
            ));
        }
        if !ValueType::can_connect(from_port.ty, to_port.ty) {
            return Err(HostError::compile(
                edge.to_node,
                format!(
                    "incompatible edge types {:?} -> {:?} on port '{}'",
                    from_port.ty, to_port.ty, edge.to_port
                ),
            ));
        }
        if driven_inputs
            .insert((edge.to_node, edge.to_port.clone()), edge_idx)
            .is_some()
        {
            return Err(HostError::compile(
                edge.to_node,
                format!("input port '{}' has multiple incoming edges", edge.to_port),
            ));
        }

        incoming_for_node[to_idx].push(edge);
        // Delay node inputs break feedback cycles (§7.2).
        if kinds[to_idx] != NodeKind::Delay {
            adj[from_idx].push(to_idx);
            in_degree[to_idx] += 1;
        }
    }

    // Kahn topological sort.
    let mut ready = VecDeque::new();
    for (idx, &deg) in in_degree.iter().enumerate() {
        if deg == 0 {
            ready.push_back(idx);
        }
    }
    let mut topo_order: Vec<usize> = Vec::with_capacity(doc.nodes.len());
    while let Some(u) = ready.pop_front() {
        topo_order.push(u);
        for &v in &adj[u] {
            in_degree[v] -= 1;
            if in_degree[v] == 0 {
                ready.push_back(v);
            }
        }
    }
    if topo_order.len() != doc.nodes.len() {
        let cycle_node = in_degree
            .iter()
            .enumerate()
            .filter(|&(_, &deg)| deg > 0)
            .map(|(i, _)| doc.nodes[i].id)
            .min()
            .unwrap_or(0);
        return Err(HostError::compile(
            cycle_node,
            "graph contains a cycle not broken by a Delay node (§7.2)",
        ));
    }

    // Build flat type-segregated ValuesArena and state table.
    let mut arena = ValuesArena::default();
    let mut states: Vec<NodeState> = Vec::new();
    let mut state_keys: Vec<(u64, String)> = Vec::new();
    let mut ops: Vec<Op> = Vec::with_capacity(doc.nodes.len() * 2);
    let mut op_to_node_slot: Vec<Option<usize>> = Vec::with_capacity(doc.nodes.len() * 2);
    let mut node_ids: Vec<u64> = Vec::with_capacity(doc.nodes.len());
    let mut latched_bindings: Vec<LatchedBinding> = Vec::new();
    let mut next_latched_slot = 0_usize;

    let mut port_slots: BTreeMap<(u64, String, bool), SlotRef> = BTreeMap::new();
    let mut delay_nodes: Vec<(usize, u64)> = Vec::new();
    let mut delay_ui_slots: HashMap<u64, usize> = HashMap::new();
    let mut output_pins: Vec<CompiledPinSlot> = Vec::new();

    // Allocate every node's input/output slots upfront.
    for (idx, node) in doc.nodes.iter().enumerate() {
        for port in &ports_per_node[idx] {
            if port.ty == ValueType::String {
                continue;
            }
            let init = if port.direction == PortDirection::Input {
                default_param_for_input(node, &port.name)
            } else {
                None
            };
            let slot = arena.alloc_slot(port.ty, init)?;
            port_slots.insert(
                (
                    node.id,
                    port.name.clone(),
                    port.direction == PortDirection::Output,
                ),
                slot,
            );
        }
    }

    // Allocate/migrate state slots in deterministic topological order.
    let mut state_idx_for_node: HashMap<u64, usize> = HashMap::new();
    for &idx in &topo_order {
        let node = &doc.nodes[idx];
        let kind = kinds[idx];
        if is_stateful_kind(kind) {
            let migrated = prev_states
                .iter()
                .find(|(pid, pty, _)| *pid == node.id && pty == &node.type_id)
                .map(|(_, _, st)| *st);
            let st = match (kind, migrated) {
                (NodeKind::Smooth, Some(s @ NodeState::Smooth(_)))
                | (NodeKind::Spring, Some(s @ NodeState::Spring(_)))
                | (NodeKind::Delay, Some(s @ NodeState::Delay(_)))
                | (NodeKind::ExpressionTrigger, Some(s @ NodeState::Trigger(_)))
                | (NodeKind::AutoBlink, Some(s @ NodeState::AutoBlink(_)))
                | (NodeKind::Random, Some(s @ NodeState::Random(_))) => s,
                (NodeKind::Smooth, _) => NodeState::Smooth(SmoothState::default()),
                (NodeKind::Spring, _) => NodeState::Spring(SpringState::default()),
                (NodeKind::Delay, _) => NodeState::Delay(DelayState {
                    prev: param_f32(node, "initial", 0.0),
                }),
                (NodeKind::ExpressionTrigger, _) => NodeState::Trigger(TriggerState::default()),
                (NodeKind::AutoBlink, _) => NodeState::AutoBlink(AutoBlinkState::default()),
                (NodeKind::Random, _) => NodeState::Random(RandomState {
                    seed: u64::from(param_u32(node, "seed", 1337)).max(1) ^ (node.id << 16),
                    last: 0.0,
                }),
                (NodeKind::WasmPlugin, _) => {
                    if next_latched_slot >= crate::graph::eval::MAX_LATCHED_INSTANCES {
                        return Err(HostError::LimitExceeded(
                            "exceeded maximum of 64 latched plugin instances (§7.5)".to_owned(),
                        ));
                    }
                    let slot = next_latched_slot;
                    next_latched_slot += 1;
                    NodeState::Latched {
                        instance_slot: slot,
                    }
                }
                _ => continue,
            };
            let s_idx = states.len();
            states.push(st);
            state_keys.push((node.id, node.type_id.clone()));
            state_idx_for_node.insert(node.id, s_idx);
        }
    }

    // Emit initial DelayRead ops first so feedback Delay values represent previous tick consistently.
    for &idx in &topo_order {
        if kinds[idx] == NodeKind::Delay {
            let node = &doc.nodes[idx];
            let s_idx = state_idx_for_node[&node.id];
            let out_slot = port_slots[&(node.id, "out".to_owned(), true)].as_usize();
            ops.push(Op::DelayRead {
                state_idx: s_idx,
                dst: out_slot,
            });
            op_to_node_slot.push(None);
            delay_nodes.push((idx, node.id));
        }
    }

    let mut async_tag = 1_u32;
    for &idx in &topo_order {
        let node = &doc.nodes[idx];
        let kind = kinds[idx];
        let ui_slot = node_ids.len();
        node_ids.push(node.id);
        let mut out_pin_idx = 0_u8;
        for port in ports_per_node[idx]
            .iter()
            .filter(|p| p.direction == PortDirection::Output)
        {
            let slot = if port.ty == ValueType::String {
                None
            } else {
                port_slots.get(&(node.id, port.name.clone(), true)).copied()
            };
            if output_pins.len() < aether_ffi::AETHER_GRAPH_MAX_DEBUG_PINS {
                output_pins.push(CompiledPinSlot {
                    node_id: node.id,
                    pin_idx: out_pin_idx,
                    slot,
                });
            }
            out_pin_idx = out_pin_idx.saturating_add(1);
        }

        // Emit wire copies / coercions into this node's input slots cleanly (Delay input deferred).
        if kind != NodeKind::Delay {
            for edge in &incoming_for_node[idx] {
                let src = port_slots[&(edge.from_node, edge.from_port.clone(), true)];
                let dst = port_slots[&(edge.to_node, edge.to_port.clone(), false)];
                emit_copy_or_coerce(&mut ops, &mut op_to_node_slot, src, dst)?;
            }
        }

        // Studio "Bypass / Mute Node": each output passes through the first input of the same value
        // type (pose -> pose, expressions -> expressions, a -> out ...); outputs without a matching
        // input keep their zero/default slot value, and the node's own op is not emitted. Sources and
        // sinks are therefore muted. Delay and WASM plugin nodes keep their state wiring and ignore it.
        let bypassed = node
            .params
            .get("bypass")
            .and_then(serde_json::Value::as_bool)
            .unwrap_or(false);
        if bypassed && !matches!(kind, NodeKind::Delay | NodeKind::WasmPlugin) {
            for out_port in ports_per_node[idx]
                .iter()
                .filter(|p| p.direction == PortDirection::Output && p.ty != ValueType::String)
            {
                if let Some(in_port) = ports_per_node[idx]
                    .iter()
                    .find(|p| p.direction == PortDirection::Input && p.ty == out_port.ty)
                {
                    let src = port_slots[&(node.id, in_port.name.clone(), false)];
                    let dst = port_slots[&(node.id, out_port.name.clone(), true)];
                    emit_copy_or_coerce(&mut ops, &mut op_to_node_slot, src, dst)?;
                }
            }
            continue;
        }

        let in_slot = |name: &str| port_slots[&(node.id, name.to_owned(), false)].as_usize();
        let out_slot = |name: &str| port_slots[&(node.id, name.to_owned(), true)].as_usize();

        let emitted = match kind {
            NodeKind::Delay => {
                delay_ui_slots.insert(node.id, ui_slot);
                // DelayRead already emitted before DAG loop; DelayWrite emits at end after all producers execute.
                continue;
            }
            NodeKind::NpuPose => Op::NpuPose {
                out_pose: out_slot("pose"),
                out_expr: out_slot("expressions"),
                out_face_conf: out_slot("face_conf"),
                out_hand_conf: out_slot("hand_conf"),
                out_body_conf: out_slot("body_conf"),
                out_ep_id: out_slot("ep_id"),
            },
            NodeKind::DspAudio => Op::DspAudio {
                out_audio: out_slot("audio"),
                out_rms_db: out_slot("rms_db"),
                out_voice_active: out_slot("voice_active"),
            },
            NodeKind::Hotkey => Op::Hotkey {
                key_code: param_u32(node, "key_code", 0),
                out_pressed: out_slot("pressed"),
                out_held: out_slot("held"),
            },
            NodeKind::Clock => Op::Clock {
                out_time_s: out_slot("time_s"),
                out_dt_s: out_slot("dt_s"),
                out_tick: out_slot("tick"),
            },
            NodeKind::OscIn | NodeKind::WsIn => Op::ExternalParam {
                slot: usize::try_from(param_u32(node, "slot", 0))
                    .unwrap_or(0)
                    .min(63),
                hotkey_slot: param_u32(node, "hotkey_id", 64).min(127),
                out_value: out_slot("value"),
                out_trigger: out_slot("trigger"),
            },
            NodeKind::VisibilityFirstIk => Op::VisibilityFirstIk {
                in_pose: in_slot("pose"),
                in_hand_conf: in_slot("hand_conf"),
                in_body_conf: in_slot("body_conf"),
                out_pose: out_slot("pose"),
            },
            NodeKind::LookAt => Op::LookAt {
                in_target: in_slot("target"),
                in_weight: in_slot("weight"),
                out_yaw: out_slot("head_yaw"),
                out_pitch: out_slot("head_pitch"),
            },
            NodeKind::IdleBreath => Op::IdleBreath {
                in_rate_hz: in_slot("rate_hz"),
                in_amp: in_slot("amplitude"),
                out_pose: out_slot("pose"),
            },
            NodeKind::IdleAttractor => Op::IdleAttractor {
                in_blend: in_slot("blend"),
                out_pose: out_slot("pose"),
            },
            NodeKind::FootPlanting => Op::FootPlanting {
                in_pose: in_slot("pose"),
                in_floor_y: in_slot("floor_y"),
                out_pose: out_slot("pose"),
            },
            NodeKind::FormantLipSync => Op::FormantLipSync {
                in_audio: in_slot("audio"),
                in_gain: in_slot("gain"),
                out_visemes: out_slot("visemes"),
            },
            NodeKind::ExpressionTrigger => Op::ExpressionTrigger {
                in_trigger: in_slot("trigger"),
                slot: usize::try_from(param_u32(node, "slot", 0))
                    .unwrap_or(0)
                    .min(63),
                fade_in_ms: param_f32(node, "fade_in_ms", 100.0),
                hold_ms: param_f32(node, "hold_ms", 500.0),
                fade_out_ms: param_f32(node, "fade_out_ms", 200.0),
                target_weight: param_f32(node, "target_weight", 1.0),
                state_idx: state_idx_for_node[&node.id],
                out_expr: out_slot("expressions"),
                out_active: out_slot("active"),
            },
            NodeKind::ExpressionMixer => Op::ExpressionMixer {
                in_a: in_slot("a"),
                in_b: in_slot("b"),
                in_weight: in_slot("weight"),
                out_expr: out_slot("expressions"),
            },
            NodeKind::AutoBlink => Op::AutoBlink {
                in_face_conf: in_slot("face_conf"),
                in_interval_s: in_slot("interval_s"),
                state_idx: state_idx_for_node[&node.id],
                out_expr: out_slot("expressions"),
                out_blink: out_slot("blink"),
            },
            NodeKind::BlendshapeRemap => Op::BlendshapeRemap {
                in_expr: in_slot("expressions"),
                slot: usize::try_from(param_u32(node, "slot", 0))
                    .unwrap_or(0)
                    .min(63),
                in_min: param_f32(node, "in_min", 0.0),
                in_max: param_f32(node, "in_max", 1.0),
                out_min: param_f32(node, "out_min", 0.0),
                out_max: param_f32(node, "out_max", 1.0),
                out_expr: out_slot("expressions"),
            },
            NodeKind::Add => match port_slots[&(node.id, "out".to_owned(), true)] {
                SlotRef::Vec3(out) => Op::AddVec3 {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    out,
                },
                SlotRef::Float(out) => Op::AddFloat {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    out,
                },
                _ => return Err(HostError::compile(node.id, "invalid Add output type")),
            },
            NodeKind::Multiply => match port_slots[&(node.id, "out".to_owned(), true)] {
                SlotRef::Vec3(out) => Op::MulVec3 {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    out,
                },
                SlotRef::Float(out) => Op::MulFloat {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    out,
                },
                _ => return Err(HostError::compile(node.id, "invalid Multiply output type")),
            },
            NodeKind::Lerp => match port_slots[&(node.id, "out".to_owned(), true)] {
                SlotRef::Vec3(out) => Op::LerpVec3 {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    in_t: in_slot("t"),
                    out,
                },
                SlotRef::Quat(out) => Op::LerpQuat {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    in_t: in_slot("t"),
                    out,
                },
                SlotRef::Float(out) => Op::LerpFloat {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    in_t: in_slot("t"),
                    out,
                },
                _ => return Err(HostError::compile(node.id, "invalid Lerp output type")),
            },
            NodeKind::Remap => Op::Remap {
                src: in_slot("in"),
                in_min: param_f32(node, "in_min", 0.0),
                in_max: param_f32(node, "in_max", 1.0),
                out_min: param_f32(node, "out_min", 0.0),
                out_max: param_f32(node, "out_max", 1.0),
                clamp: param_bool(node, "clamp", true),
                dst: out_slot("out"),
            },
            NodeKind::Clamp => Op::Clamp {
                src: in_slot("in"),
                min_v: param_f32(node, "min", 0.0),
                max_v: param_f32(node, "max", 1.0),
                dst: out_slot("out"),
            },
            NodeKind::Smooth => Op::Smooth {
                src: in_slot("in"),
                cutoff_hz: param_f32(node, "cutoff_hz", 5.0),
                state_idx: state_idx_for_node[&node.id],
                dst: out_slot("out"),
            },
            NodeKind::Spring => Op::Spring {
                src: in_slot("in"),
                stiffness: param_f32(node, "stiffness", 120.0),
                damping: param_f32(node, "damping", 14.0),
                state_idx: state_idx_for_node[&node.id],
                dst: out_slot("out"),
            },
            NodeKind::Compare => {
                let mode = node
                    .params
                    .get("op")
                    .and_then(serde_json::Value::as_str)
                    .unwrap_or("gt");
                let cmp = match mode {
                    "lt" | "<" => CompareOp::Lt,
                    "ge" | ">=" => CompareOp::Ge,
                    "le" | "<=" => CompareOp::Le,
                    "eq" | "==" => CompareOp::Eq,
                    _ => CompareOp::Gt,
                };
                Op::Compare {
                    in_a: in_slot("a"),
                    in_b: in_slot("b"),
                    cmp,
                    out: out_slot("out"),
                }
            }
            NodeKind::And => Op::And {
                in_a: in_slot("a"),
                in_b: in_slot("b"),
                out: out_slot("out"),
            },
            NodeKind::Or => Op::Or {
                in_a: in_slot("a"),
                in_b: in_slot("b"),
                out: out_slot("out"),
            },
            NodeKind::Not => Op::Not {
                src: in_slot("in"),
                out: out_slot("out"),
            },
            NodeKind::Select => Op::Select {
                in_cond: in_slot("cond"),
                in_a: in_slot("a"),
                in_b: in_slot("b"),
                out: out_slot("out"),
            },
            NodeKind::Random => Op::Random {
                in_trigger: in_slot("trigger"),
                state_idx: state_idx_for_node[&node.id],
                dst: out_slot("out"),
            },
            NodeKind::Curve => {
                let mut lut = [0.0_f32; 64];
                if let Some(arr) = node.params.get("lut").and_then(serde_json::Value::as_array) {
                    for (i, dst) in lut.iter_mut().enumerate() {
                        *dst = arr
                            .get(i)
                            .and_then(serde_json::Value::as_f64)
                            .map_or(i as f32 / 63.0, |v| v as f32);
                    }
                } else {
                    for (i, dst) in lut.iter_mut().enumerate() {
                        *dst = i as f32 / 63.0;
                    }
                }
                let lut_idx = arena.luts.len();
                arena.luts.push(lut);
                Op::Curve {
                    src: in_slot("in"),
                    lut_idx,
                    dst: out_slot("out"),
                }
            }
            NodeKind::OutputPose => Op::OutputPose {
                in_pose: in_slot("pose"),
            },
            NodeKind::OutputExpressions => Op::OutputExpressions {
                in_expr: in_slot("expressions"),
            },
            NodeKind::CameraControl => Op::CameraControl {
                in_focal: in_slot("focal_mm"),
                in_flat: in_slot("flatness"),
            },
            NodeKind::LightControl => Op::LightControl {
                in_dir: in_slot("direction"),
                in_intensity: in_slot("intensity"),
            },
            NodeKind::PropAttach => Op::PropAttach {
                in_transform: in_slot("transform"),
            },
            NodeKind::OscOut | NodeKind::WsOut => {
                let tag = param_u32(node, "event_id", async_tag);
                async_tag = async_tag.saturating_add(1);
                Op::AsyncOutputSignal {
                    in_val: in_slot("value"),
                    in_trigger: in_slot("trigger"),
                    event_tag: tag,
                }
            }
            NodeKind::WasmPlugin => {
                let s_idx = state_idx_for_node[&node.id];
                let NodeState::Latched { instance_slot } = states[s_idx] else {
                    return Err(HostError::Internal(
                        "missing latched state for WASM plugin node".to_owned(),
                    ));
                };
                let mut in_floats = [None; LATCHED_IO_FLOATS];
                let mut out_floats = [None; LATCHED_IO_FLOATS];
                let mut in_c = 0_usize;
                let mut out_c = 0_usize;
                for p in &ports_per_node[idx] {
                    if let Some(SlotRef::Float(slot)) = port_slots
                        .get(&(
                            node.id,
                            p.name.clone(),
                            p.direction == PortDirection::Output,
                        ))
                        .copied()
                    {
                        if p.direction == PortDirection::Input && in_c < LATCHED_IO_FLOATS {
                            in_floats[in_c] = Some(slot);
                            in_c += 1;
                        } else if p.direction == PortDirection::Output && out_c < LATCHED_IO_FLOATS
                        {
                            out_floats[out_c] = Some(slot);
                            out_c += 1;
                        }
                    }
                }
                latched_bindings.push(LatchedBinding {
                    instance_slot,
                    node_id: node.id,
                    type_id: node.type_id.clone(),
                    input_count: u32::try_from(in_c).unwrap_or(0),
                    output_count: u32::try_from(out_c).unwrap_or(0),
                });
                Op::Latched {
                    instance_slot,
                    in_floats: Box::new(in_floats),
                    out_floats: Box::new(out_floats),
                }
            }
        };

        ops.push(emitted);
        op_to_node_slot.push(Some(ui_slot));
    }

    // Finally capture Delay node inputs cleanly after all producers ran.
    for (idx, delay_id) in delay_nodes {
        let s_idx = state_idx_for_node[&delay_id];
        for edge in &incoming_for_node[idx] {
            let src = port_slots[&(edge.from_node, edge.from_port.clone(), true)];
            let dst = port_slots[&(edge.to_node, edge.to_port.clone(), false)];
            emit_copy_or_coerce(&mut ops, &mut op_to_node_slot, src, dst)?;
        }
        let in_f = port_slots[&(delay_id, "in".to_owned(), false)].as_usize();
        ops.push(Op::DelayWrite {
            src: in_f,
            state_idx: s_idx,
        });
        op_to_node_slot.push(delay_ui_slots.get(&delay_id).copied());
    }

    let node_count = node_ids.len();
    let pin_count = output_pins.len();
    Ok(CompiledGraphPlan {
        arena,
        ops,
        states,
        state_keys,
        node_ids,
        op_to_node_slot,
        node_eval_ns: vec![0; node_count],
        node_eval_us: vec![0.0; node_count],
        node_live_values: vec![0.0; node_count],
        last_total_eval_ns: 0,
        output_pins,
        pin_kinds: vec![0; pin_count],
        pin_values: vec![[0.0; 4]; pin_count],
        latched_bindings,
    })
}
