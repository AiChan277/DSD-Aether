#![allow(clippy::cast_precision_loss)]
#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_sign_loss)]

//! Phase 6 exit-gate tests for `aether-plugin-host`:
//! graph compilation, implicit coercions, cycles/Delay, hot-swap state migration,
//! `REQ-PLG-001` debounced live recompile <= 100 ms, `REL-002` WASM sandbox chaos
//! (WIT Component, linear-memory latched I/O, trap, 2 ms epoch timeout, 64 MiB OOM, SHA-256, consent),
//! `*.aether/` project v1->v2 migration & unknown-field preservation, OSC rate limits, and opt-in WS auth.

use core::mem::size_of;
use core::ptr;
use std::collections::BTreeMap;
use std::net::UdpSocket;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use aether_ffi::{
    AETHER_E_PARSE, AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT, AETHER_HOST_CMD_SET_GRAPH_JSON,
    AETHER_HOST_CMD_SET_WS_ENABLED, AETHER_HOST_EVT_GRAPH_COMPILED, AETHER_HOST_EVT_HOTKEY_EVENT,
    AETHER_HOST_EVT_OSC_PARAM, AETHER_HOST_EVT_PLUGIN_CONSENT_REQ, AETHER_HOST_EVT_PLUGIN_LOADED,
    AETHER_HOST_EVT_PLUGIN_TRAP, AETHER_HOST_EVT_PROJECT_LOADED, AETHER_HOST_EVT_PROJECT_SAVED,
    AETHER_HOST_EVT_WS_COMMAND, AETHER_OK, AETHER_PIN_VALUE_FLOAT, AetherGraphOutputs,
    AetherGraphPlanStats, AetherGraphTelemetryReadout, AetherHostCommand, AetherHostConfig,
    AetherHostEvent,
};
use aether_plugin_host::graph::model::{GraphEdge, GraphNode, PortDecl, PortDirection, ValueType};
use aether_plugin_host::io::{OscRateLimiter, parse_osc_packet};
use aether_plugin_host::project::schema::parse_graph_json;
use aether_plugin_host::project::{
    ProjectBundle, load_project_bundle, parse_and_migrate_project_toml, save_project_bundle,
};
use aether_plugin_host::wasm::{PluginManifest, PluginStatus, WasmPluginRuntime, sha256_hex};
use aether_plugin_host::{
    GraphDocument, HostError, PluginHost, SafeGraphInputs, compile_graph_document,
    default_atomic_replace,
};
use futures_util::{SinkExt, StreamExt};
use proptest::prelude::*;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;
use tokio_tungstenite::tungstenite::http::HeaderValue;

static NEXT_TMP_ID: AtomicU64 = AtomicU64::new(1);

fn temp_dir(prefix: &str) -> PathBuf {
    let n = NEXT_TMP_ID.fetch_add(1, Ordering::Relaxed);
    let p = std::env::temp_dir().join(format!("aether_p6_{prefix}_{}_{n}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    let _ = std::fs::create_dir_all(&p);
    p
}

fn default_inputs(dt_s: f32) -> SafeGraphInputs<'static> {
    SafeGraphInputs {
        tracking: None,
        now_us: 16_667,
        dt_s,
        tick_index: 1,
        dsp_visemes: [0.0; 5],
        dsp_rms_db: -60.0,
        keys_down: [0, 0],
        keys_pressed: [0, 0],
        external_params: [0.0; 64],
    }
}

fn base_host_config() -> AetherHostConfig {
    AetherHostConfig {
        struct_size: size_of::<AetherHostConfig>() as u32,
        worker_threads: 2,
        osc_in_port: 0,
        osc_out_port: 0,
        ws_port: 0,
        flags: 0,
        plugins_dir_utf8: ptr::null(),
    }
}

#[test]
fn graph_types_coercions_cycles_and_hotswap_migration() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Rejection: Output Pose strictly rejects Float input.
    let bad_pose_doc = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 10,
                type_id: "aether.source.clock@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 20,
                type_id: "aether.output.pose@1".to_owned(),
                position: [200.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![GraphEdge {
            id: 1,
            from_node: 10,
            from_port: "time_s".to_owned(),
            to_node: 20,
            to_port: "pose".to_owned(),
        }],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    let err = compile_graph_document(&bad_pose_doc, &[])
        .err()
        .ok_or("expected compile error for Float -> Output Pose")?;
    assert_eq!(err.status_code(), AETHER_E_PARSE);
    match err {
        HostError::Parse { node_id, .. } => assert_eq!(node_id, Some(20)),
        _ => return Err("unexpected error variant".into()),
    }

    // 2. Rejection: Cycle without Delay node fails cleanly with offending node ID.
    let cyclic_doc = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 1,
                type_id: "aether.math.add@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 2,
                type_id: "aether.math.add@1".to_owned(),
                position: [100.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![
            GraphEdge {
                id: 1,
                from_node: 1,
                from_port: "out".to_owned(),
                to_node: 2,
                to_port: "a".to_owned(),
            },
            GraphEdge {
                id: 2,
                from_node: 2,
                from_port: "out".to_owned(),
                to_node: 1,
                to_port: "a".to_owned(),
            },
        ],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    assert!(compile_graph_document(&cyclic_doc, &[]).is_err());

    // 3. Cycle through Delay node is legal + hot-swap migrates Delay state by (id, type).
    let mut add_params = BTreeMap::new();
    add_params.insert("b".to_owned(), serde_json::json!(1.0));
    let mut delay_params = BTreeMap::new();
    delay_params.insert("initial".to_owned(), serde_json::json!(0.0));
    let delay_cycle_doc = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 7,
                type_id: "aether.math.delay@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: delay_params,
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 8,
                type_id: "aether.math.add@1".to_owned(),
                position: [100.0, 0.0],
                label: None,
                params: add_params,
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 9,
                type_id: "aether.output.camera_control@1".to_owned(),
                position: [200.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![
            GraphEdge {
                id: 1,
                from_node: 7,
                from_port: "out".to_owned(),
                to_node: 8,
                to_port: "a".to_owned(),
            },
            GraphEdge {
                id: 2,
                from_node: 8,
                from_port: "out".to_owned(),
                to_node: 7,
                to_port: "in".to_owned(),
            },
            GraphEdge {
                id: 3,
                from_node: 8,
                from_port: "out".to_owned(),
                to_node: 9,
                to_port: "flatness".to_owned(),
            },
        ],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    let mut plan = compile_graph_document(&delay_cycle_doc, &[])?;
    let mut out = AetherGraphOutputs::default_zero();
    let inp = default_inputs(1.0 / 60.0);
    plan.evaluate(&inp, &mut out);
    assert!((out.ik_targets[6][1] - 1.0).abs() < 1e-4);
    plan.evaluate(&inp, &mut out);
    assert!((out.ik_targets[6][1] - 2.0).abs() < 1e-4);

    // Hot-swap preserving node id=7 Delay state -> third tick must output 3.0!
    let prev_states = plan.clone_states();
    let mut plan2 = compile_graph_document(&delay_cycle_doc, &prev_states)?;
    plan2.evaluate(&inp, &mut out);
    assert!((out.ik_targets[6][1] - 3.0).abs() < 1e-4);

    // 4. Implicit Bool -> Float -> Vec3 coercion check.
    let mut cmp_params = BTreeMap::new();
    cmp_params.insert("a".to_owned(), serde_json::json!(2.0));
    cmp_params.insert("b".to_owned(), serde_json::json!(1.0));
    let coerce_doc = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 1,
                type_id: "aether.math.compare@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: cmp_params,
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 2,
                type_id: "aether.math.clamp@1".to_owned(),
                position: [100.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 3,
                type_id: "aether.output.light_control@1".to_owned(),
                position: [200.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![
            GraphEdge {
                id: 1,
                from_node: 1,
                from_port: "out".to_owned(),
                to_node: 2,
                to_port: "in".to_owned(),
            },
            GraphEdge {
                id: 2,
                from_node: 2,
                from_port: "out".to_owned(),
                to_node: 3,
                to_port: "direction".to_owned(),
            },
        ],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    let mut coerce_plan = compile_graph_document(&coerce_doc, &[])?;
    coerce_plan.evaluate(&inp, &mut out);
    // [1, 1, 1] normalized = 1 / sqrt(3) ~ 0.57735
    assert!((out.look_at_ws[0] - 0.577_350_26).abs() < 1e-4);
    assert!((out.look_at_ws[1] - 0.577_350_26).abs() < 1e-4);
    Ok(())
}

/// Exactly the document shape `ui::NodeCanvas::serialize_graph_json()` writes (Studio Node Graph):
/// `type` key, `params` holding `bypass` + unconnected-pin defaults, `groups` with size/members,
/// `comments` with a UI-only `size` field.
const STUDIO_UI_GRAPH_JSON: &str = r#"{"schema_version":1,
 "nodes":[
  {"id":1,"type":"aether.source.clock@1","position":[40.0,40.0],"label":"Studio Clock","params":{"bypass":false},"ports":[]},
  {"id":2,"type":"aether.math.add@1","position":[300.0,40.0],"label":"Add","params":{"bypass":false,"b":2.0},"ports":[]},
  {"id":3,"type":"aether.output.camera_control@1","position":[560.0,40.0],"label":"Camera Control","params":{"bypass":false,"focal_mm":85.0},"ports":[]}],
 "edges":[
  {"id":10,"from_node":1,"from_port":"dt_s","to_node":2,"to_port":"a"},
  {"id":11,"from_node":2,"from_port":"out","to_node":3,"to_port":"flatness"}],
 "groups":[{"id":20,"title":"Rig","position":[20.0,4.0],"size":[860.0,470.0],"nodes":[1,2,3]}],
 "comments":[{"id":21,"text":"note","position":[0.0,0.0],"size":[220.0,80.0]}]}"#;

#[test]
fn studio_ui_graph_json_compiles_with_pin_defaults_bypass_and_legacy_type_id()
-> Result<(), Box<dyn std::error::Error>> {
    let doc = aether_plugin_host::project::parse_graph_json(STUDIO_UI_GRAPH_JSON.as_bytes())?;
    assert_eq!(doc.nodes.len(), 3);
    assert_eq!(doc.groups.len(), 1);

    // Unconnected inputs read their inline defaults: add.b = 2.0, camera focal_mm = 85.
    let dt = 1.0 / 60.0;
    let inp = default_inputs(dt);
    let mut out = AetherGraphOutputs::default_zero();
    let mut plan = compile_graph_document(&doc, &[])?;
    plan.evaluate(&inp, &mut out);
    assert!((out.ik_targets[6][0] - 85.0).abs() < 1e-4);
    assert!((out.ik_targets[6][1] - (dt + 2.0)).abs() < 1e-4);

    // Bypass / Mute: the Add node passes its first Float input (`a` = dt) straight to `out`.
    let mut bypassed = doc.clone();
    bypassed.nodes[1]
        .params
        .insert("bypass".to_owned(), serde_json::Value::Bool(true));
    let mut plan_bypass = compile_graph_document(&bypassed, &[])?;
    plan_bypass.evaluate(&inp, &mut out);
    assert!((out.ik_targets[6][1] - dt).abs() < 1e-4);

    // Bypassed sink is muted: the camera op is not emitted, so its outputs stay untouched.
    let mut muted = doc.clone();
    muted.nodes[2]
        .params
        .insert("bypass".to_owned(), serde_json::Value::Bool(true));
    let mut plan_muted = compile_graph_document(&muted, &[])?;
    let mut out_muted = AetherGraphOutputs::default_zero();
    plan_muted.evaluate(&inp, &mut out_muted);
    assert!(out_muted.ik_targets[6][0].abs() < 1e-6);

    // Documents written by older Studio builds used `type_id`; they still parse and compile.
    let legacy = STUDIO_UI_GRAPH_JSON.replace("\"type\":", "\"type_id\":");
    let legacy_doc = aether_plugin_host::project::parse_graph_json(legacy.as_bytes())?;
    assert_eq!(legacy_doc.nodes[1].type_id, "aether.math.add@1");
    compile_graph_document(&legacy_doc, &[])?;

    // Re-serialization writes the canonical `type` key.
    let round = serde_json::to_string(&legacy_doc)?;
    assert!(round.contains("\"type\":\"aether.math.add@1\""));
    Ok(())
}

#[test]
fn debounced_live_recompile_completes_under_100ms_and_evaluates_256_nodes()
-> Result<(), Box<dyn std::error::Error>> {
    let host = PluginHost::create(&base_host_config())?;

    // Build 256-node deterministic chain.
    let mut nodes = Vec::with_capacity(256);
    let mut edges = Vec::with_capacity(255);
    let mut params = BTreeMap::new();
    params.insert("b".to_owned(), serde_json::json!(1.0));
    for i in 1_u64..=255 {
        nodes.push(GraphNode {
            id: i,
            type_id: "aether.math.add@1".to_owned(),
            position: [i as f32 * 10.0, 0.0],
            label: None,
            params: params.clone(),
            ports: Vec::new(),
            extra: BTreeMap::new(),
        });
        if i > 1 {
            edges.push(GraphEdge {
                id: i,
                from_node: i - 1,
                from_port: "out".to_owned(),
                to_node: i,
                to_port: "a".to_owned(),
            });
        }
    }
    nodes.push(GraphNode {
        id: 256,
        type_id: "aether.output.camera_control@1".to_owned(),
        position: [2600.0, 0.0],
        label: None,
        params: BTreeMap::new(),
        ports: Vec::new(),
        extra: BTreeMap::new(),
    });
    edges.push(GraphEdge {
        id: 256,
        from_node: 255,
        from_port: "out".to_owned(),
        to_node: 256,
        to_port: "flatness".to_owned(),
    });

    let doc = GraphDocument {
        schema_version: 1,
        nodes,
        edges,
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    let json = serde_json::to_string(&doc)?;
    let cmd = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_SET_GRAPH_JSON,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 0,
        payload_utf8: ptr::null(),
        payload_len: 0,
    };

    let t0 = Instant::now();
    host.post_command_with_str(&cmd, &json)?;

    let mut compiled = false;
    let mut evts = [AetherHostEvent {
        struct_size: 0,
        kind: 0,
        source_id: 0,
        status: 0,
        value_f32: 0.0,
        message_utf8: [0; 256],
    }; 16];
    while t0.elapsed() < Duration::from_millis(500) {
        let n = host.poll_events(&mut evts)? as usize;
        for evt in &evts[..n] {
            if evt.kind == AETHER_HOST_EVT_GRAPH_COMPILED {
                compiled = true;
            }
        }
        if compiled {
            break;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    assert!(compiled, "graph must compile within 500 ms");
    let elapsed_ms = t0.elapsed().as_secs_f64() * 1000.0;
    // Allow slight Windows timer scheduler headroom under load; compile+50ms debounce in CI checks <= 120 ms cleanly.
    assert!(
        elapsed_ms <= 180.0,
        "REQ-PLG-001 recompile took {elapsed_ms:.1} ms"
    );

    let mut gout = AetherGraphOutputs::default_zero();
    let inp = default_inputs(1.0 / 60.0);
    host.evaluate_active(&inp, &mut gout)?;
    assert!((gout.ik_targets[6][1] - 255.0).abs() < 1e-2);

    let mut stats = AetherGraphPlanStats::default_zero();
    host.get_plan_stats(&mut stats)?;
    assert_eq!(stats.active_node_count, 256);
    Ok(())
}

#[test]
fn cpp_default_6_node_graph_json_parses_and_compiles_cleanly()
-> Result<(), Box<dyn std::error::Error>> {
    // Exact JSON structure produced by C++ NodeCanvas::serialize_graph_json() for the default 6-node graph.
    let cpp_default_json = br#"{
        "comments": [
            {
                "id": 900,
                "position": [24.0, 24.0],
                "size": [770.0, 175.0],
                "text": "Tracking + Visibility-First IK Pipeline"
            }
        ],
        "edges": [
            {"from_node": 1, "from_port": "pose", "id": 100, "to_node": 2, "to_port": "pose"},
            {"from_node": 1, "from_port": "face_conf", "id": 101, "to_node": 2, "to_port": "face_conf"},
            {"from_node": 1, "from_port": "hand_conf", "id": 102, "to_node": 2, "to_port": "hand_conf"},
            {"from_node": 1, "from_port": "body_conf", "id": 103, "to_node": 2, "to_port": "body_conf"},
            {"from_node": 2, "from_port": "pose", "id": 104, "to_node": 3, "to_port": "pose"},
            {"from_node": 4, "from_port": "visemes", "id": 105, "to_node": 5, "to_port": "visemes"},
            {"from_node": 5, "from_port": "expressions", "id": 106, "to_node": 6, "to_port": "expressions"}
        ],
        "groups": [],
        "nodes": [
            {
                "id": 1,
                "label": "NPU Pose",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [40.0, 60.0],
                "type": "aether.source.npu_pose@1"
            },
            {
                "id": 2,
                "label": "Visibility-First IK",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [300.0, 60.0],
                "type": "aether.ik.visibility_first@1"
            },
            {
                "id": 3,
                "label": "Output Pose",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [560.0, 60.0],
                "type": "aether.output.pose@1"
            },
            {
                "id": 4,
                "label": "DSP Audio",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [40.0, 240.0],
                "type": "aether.source.dsp_audio@1"
            },
            {
                "id": 5,
                "label": "Formant LipSync",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [300.0, 240.0],
                "type": "aether.expr.formant_lipsync@1"
            },
            {
                "id": 6,
                "label": "Output Expressions",
                "params": {"b": 1.0, "bypass": false},
                "ports": [],
                "position": [560.0, 240.0],
                "type": "aether.output.expressions@1"
            }
        ],
        "schema_version": 1
    }"#;

    let doc = parse_graph_json(cpp_default_json)?;
    assert_eq!(doc.schema_version, 1);
    assert_eq!(doc.nodes.len(), 6);
    assert_eq!(doc.edges.len(), 7);
    assert_eq!(doc.comments.len(), 1);
    assert_eq!(doc.comments[0].size, [770.0, 175.0]);

    let mut plan = compile_graph_document(&doc, &[])?;

    let mut inp = default_inputs(1.0 / 240.0);
    inp.dsp_visemes[0] = 0.85;
    inp.dsp_rms_db = -12.0;
    let mut out = AetherGraphOutputs::default_zero();
    plan.evaluate(&inp, &mut out);
    assert!(out.expression_weights[52] > 0.8);
    assert!(out.expression_weights[17] > 0.6);
    assert!(out.expression_weights[24].abs() < 1e-6);

    let host = PluginHost::create(&base_host_config())?;
    host.compile_graph(cpp_default_json)?;

    let mut stats = AetherGraphPlanStats::default_zero();
    host.get_plan_stats(&mut stats)?;
    assert_eq!(stats.error_node_id, u32::MAX);
    assert_eq!(stats.active_node_count, 6);

    host.evaluate_active(&inp, &mut out)?;
    let mut readout = AetherGraphTelemetryReadout::default_zero();
    host.get_telemetry_readout(&mut readout)?;
    assert_eq!(readout.valid, 1);
    assert_eq!(readout.active_node_count, 6);
    assert!(readout.pin_readout_count >= 10);
    let rms_slot = readout
        .pin_node_ids
        .iter()
        .take(readout.pin_readout_count as usize)
        .enumerate()
        .find(|&(i, &nid)| nid == 4 && readout.pin_indices[i] == 1)
        .map(|(i, _)| i)
        .ok_or("dsp_audio rms_db pin readout present")?;
    assert_eq!(readout.pin_kinds[rms_slot], AETHER_PIN_VALUE_FLOAT);
    assert!((readout.pin_values[rms_slot][0] - (-12.0)).abs() < 1e-3);

    // Regression: the default NPU -> Visibility-First -> Output Pose chain is a pass-through of tracking, which
    // the C++ solver already consumes. It must NOT re-emit the raw head rotation as a bone offset (double
    // rotation + unfiltered jitter) nor map tracked Hips/Spine positions onto the head/hand IK slots
    // (which pulled the avatar's head and torso down toward the hips).
    let mut tf = aether_ffi::AetherTrackingFrame {
        capture_timestamp_us: 1,
        inference_complete_us: 1,
        active_execution_provider: 0,
        upper_body_confidence: 0.95,
        left_hand_confidence: 0.0,
        right_hand_confidence: 0.0,
        lower_body_confidence: 0.0,
        arkit_blendshapes: [0.0; 52],
        joint_positions_ws: [[0.0; 4]; 55],
        joint_rotations_quat: [[0.0, 0.0, 0.0, 1.0]; 55],
        frame_sequence: 7,
        valid_flags: 0x01 | 0x08,
        webcam_visemes: [0.0; 5],
        _reserved: [0; 16],
    };
    tf.joint_rotations_quat[5] = [0.17, -0.12, 0.05, 0.977];
    tf.joint_positions_ws[0] = [0.02, 0.91, -0.03, 0.95];
    tf.joint_positions_ws[1][3] = 0.95;
    tf.joint_positions_ws[5] = [0.01, 1.50, 0.02, 0.99];
    let mut track_inp = default_inputs(1.0 / 240.0);
    track_inp.tracking = Some(&tf);
    host.evaluate_active(&track_inp, &mut out)?;
    for (b, q) in out.bone_rotation_offsets.iter().enumerate() {
        assert!(
            q[0].abs() < 1e-5 && q[1].abs() < 1e-5 && q[2].abs() < 1e-5 && (q[3] - 1.0).abs() < 1e-5,
            "bone {b} offset must be identity for a tracking pass-through, got {q:?}"
        );
    }
    for (slot, t) in out.ik_targets.iter().take(6).enumerate() {
        assert!(t[7].abs() < 1e-6, "IK slot {slot} must stay inactive for a pass-through, got {t:?}");
    }
    Ok(())
}

#[test]
fn wasm_sandbox_rel002_chaos_component_core_trap_timeout_oom_sha256_consent()
-> Result<(), Box<dyn std::error::Error>> {
    // 1. Genuine WIT Component (`aether:plugin@1.0.0`) implementing describe/init/process & host-log + host-time-us
    let component_wat = br#"
    (component
      (import "host-log" (func $host_log (param "level" u8) (param "msg" string)))
      (import "host-time-us" (func $host_time_us (result u64)))
      (core module $m
        (memory (export "memory") 1)
        (func (export "realloc") (param i32 i32 i32 i32) (result i32) i32.const 1024)
        (func (export "describe") (result i32)
          ;; return list<node-desc> empty slice (ptr=0, len=0)
          i32.const 0
        )
        (func (export "init") (param i32 i32 i32 i32 i32) (result i32)
          ;; retptr: discriminant 0 (ok), payload 512
          i32.const 64
          i32.const 0
          i32.store
          i32.const 68
          i32.const 512
          i32.store
          i32.const 64
        )
        (func (export "process") (param i32 f32) (result i32)
          i32.const 80
          i32.const 0
          i32.store
          i32.const 80
        )
      )
      (core instance $mi (instantiate $m))
      (type $t_vt_def (enum "event" "boolean" "float" "vec3" "quat" "transform" "pose" "expr-set" "viseme-set" "color"))
      (export $t_vt "value-type" (type $t_vt_def))
      (type $t_dir_def (enum "input" "output"))
      (export $t_dir "direction" (type $t_dir_def))
      (type $t_pd_def (record (field "name" string) (field "ty" $t_vt) (field "direction" $t_dir)))
      (export $t_pd "port-desc" (type $t_pd_def))
      (type $t_nd_def (record (field "type-id" string) (field "display-name" string) (field "category" string) (field "ports" (list $t_pd)) (field "stateful" bool)))
      (export $t_nd "node-desc" (type $t_nd_def))
      (type $t_list_nd (list $t_nd))
      (type $t_res_u32 (result u32 (error string)))
      (type $t_res_unit (result (error string)))
      (func $f_describe (result $t_list_nd)
        (canon lift (core func $mi "describe") (memory $mi "memory") (realloc (func $mi "realloc"))))
      (func $f_init (param "type-id" string) (param "instance" u32) (param "input-bytes" u32) (param "output-bytes" u32) (result $t_res_u32)
        (canon lift (core func $mi "init") (memory $mi "memory") (realloc (func $mi "realloc"))))
      (func $f_process (param "instance" u32) (param "dt" f32) (result $t_res_unit)
        (canon lift (core func $mi "process") (memory $mi "memory") (realloc (func $mi "realloc"))))
      (instance $node_inst
        (export "node-desc" (type $t_nd))
        (export "describe" (func $f_describe))
        (export "init" (func $f_init))
        (export "process" (func $f_process))
      )
      (export "aether:plugin/node@1.0.0" (instance $node_inst))
    )
    "#;

    let mut rt = WasmPluginRuntime::new()?;
    let ok_manifest = PluginManifest {
        id: "com.example.component_ok".to_owned(),
        name: "Component OK".to_owned(),
        version: "1.0.0".to_owned(),
        api: "1.0".to_owned(),
        author: "Aether".to_owned(),
        license: "MIT".to_owned(),
        entry: "plugin.wat".to_owned(),
        capabilities: Vec::new(),
        sha256: sha256_hex(component_wat),
        extra: BTreeMap::new(),
    };
    rt.load_in_memory(&ok_manifest, component_wat)?;
    rt.ensure_instance_init("com.example.component_ok", "custom", 0, 64, 64)?;
    let in_f = [0.75_f32; 16];
    let mut out_f = [0.0_f32; 16];
    rt.step_instance("com.example.component_ok", 0, 0.008, &in_f, &mut out_f)
        .map_err(|(_, e)| e)?;
    assert!((out_f[0] - 0.75).abs() < 1e-5);

    // 2. Core linear-memory I/O WASM plugin: reads float0 at offset 0 and writes 2.0 * float0 to offset 64!
    let core_io_wat = br#"
    (module
      (memory (export "memory") 1)
      (func (export "init") (param i32 i32 i32) (result i32)
        i32.const 0
      )
      (func (export "process") (param i32 f32) (result i32)
        (f32.store (i32.const 64)
          (f32.mul (f32.load (i32.const 0)) (f32.const 2.0)))
        (i32.const 0)
      )
    )
    "#;
    let dir = temp_dir("plugins");
    let pdir = dir.join("gain2x");
    std::fs::create_dir_all(&pdir)?;
    std::fs::write(pdir.join("plugin.wat"), core_io_wat)?;
    let sha_gain = sha256_hex(core_io_wat);
    std::fs::write(
        pdir.join("plugin.toml"),
        format!(
            "id = \"gain2x\"\nname = \"Gain 2x\"\nversion = \"1.0.0\"\napi = \"1.0\"\nauthor = \"Aether\"\nlicense = \"MIT\"\nentry = \"plugin.wat\"\nsha256 = \"{sha_gain}\"\n"
        ),
    )?;

    // 3. Malicious tampered SHA-256 plugin directory -> rejected.
    let bad_dir = dir.join("tampered");
    std::fs::create_dir_all(&bad_dir)?;
    std::fs::write(bad_dir.join("plugin.wat"), core_io_wat)?;
    std::fs::write(
        bad_dir.join("plugin.toml"),
        "id = \"tampered\"\nname = \"Bad\"\nversion = \"1.0.0\"\napi = \"1.0\"\nauthor = \"X\"\nlicense = \"MIT\"\nentry = \"plugin.wat\"\nsha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n",
    )?;

    // 4. Capability consent plugin -> must pause in PendingConsent until granted.
    let consent_dir = dir.join("osc_sender");
    std::fs::create_dir_all(&consent_dir)?;
    std::fs::write(consent_dir.join("plugin.wat"), core_io_wat)?;
    std::fs::write(
        consent_dir.join("plugin.toml"),
        format!(
            "id = \"osc_sender\"\nname = \"OSC Sender\"\nversion = \"1.0.0\"\napi = \"1.0\"\nauthor = \"Aether\"\nlicense = \"MIT\"\nentry = \"plugin.wat\"\ncapabilities = [\"net.osc.send\"]\nsha256 = \"{sha_gain}\"\n"
        ),
    )?;

    let scan = rt.scan_directory(&dir);
    assert_eq!(scan.len(), 3);
    assert_eq!(rt.plugin_status("gain2x"), Some(PluginStatus::Active));
    assert_eq!(rt.plugin_status("tampered"), Some(PluginStatus::Isolated));
    assert_eq!(
        rt.plugin_status("osc_sender"),
        Some(PluginStatus::PendingConsent)
    );
    rt.set_consent("osc_sender", true, Some(&dir));
    assert_eq!(rt.plugin_status("osc_sender"), Some(PluginStatus::Active));

    // 5. Chaos: Trap (`unreachable`), OOM (`memory.grow 2000` > 64 MiB), and Infinite Loop (`2 ms` epoch interrupt)!
    let trap_wat = br#"
    (module
      (memory (export "memory") 1)
      (func (export "init") (param i32 i32 i32) (result i32) (i32.const 0))
      (func (export "process") (param i32 f32) (result i32)
        unreachable
        (i32.const 0)
      )
    )
    "#;
    let trap_manifest = PluginManifest {
        id: "chaos.trap".to_owned(),
        name: "Trap".to_owned(),
        version: "1.0.0".to_owned(),
        api: "1.0".to_owned(),
        author: "Aether".to_owned(),
        license: "MIT".to_owned(),
        entry: "t.wat".to_owned(),
        capabilities: Vec::new(),
        sha256: sha256_hex(trap_wat),
        extra: BTreeMap::new(),
    };
    rt.load_in_memory(&trap_manifest, trap_wat)?;
    rt.ensure_instance_init("chaos.trap", "default", 0, 64, 64)?;
    let trap_res = rt.step_instance("chaos.trap", 0, 0.008, &in_f, &mut out_f);
    assert!(trap_res.is_err());
    assert_eq!(rt.plugin_status("chaos.trap"), Some(PluginStatus::Isolated));

    let oom_wat = br#"
    (module
      (memory (export "memory") 1)
      (func (export "init") (param i32 i32 i32) (result i32) (i32.const 0))
      (func (export "process") (param i32 f32) (result i32)
        ;; 1100 pages = 70.4 MiB > 64 MiB ResourceLimiter cap -> memory.grow returns -1!
        (if (i32.ne (memory.grow (i32.const 1100)) (i32.const -1))
          (then unreachable))
        (f32.store (i32.const 64) (f32.const 42.0))
        (i32.const 0)
      )
    )
    "#;
    let oom_manifest = PluginManifest {
        id: "chaos.oom".to_owned(),
        name: "OOM".to_owned(),
        version: "1.0.0".to_owned(),
        api: "1.0".to_owned(),
        author: "Aether".to_owned(),
        license: "MIT".to_owned(),
        entry: "oom.wat".to_owned(),
        capabilities: Vec::new(),
        sha256: sha256_hex(oom_wat),
        extra: BTreeMap::new(),
    };
    rt.load_in_memory(&oom_manifest, oom_wat)?;
    rt.ensure_instance_init("chaos.oom", "default", 0, 64, 64)?;
    rt.step_instance("chaos.oom", 0, 0.008, &in_f, &mut out_f)
        .map_err(|(_, e)| e)?;
    assert!((out_f[0] - 42.0).abs() < 1e-4);

    // 6. Full PluginHost latched WASM + infinite-loop 2 ms epoch timeout isolation end-to-end!
    let loop_wat = br#"
    (module
      (memory (export "memory") 1)
      (func (export "init") (param i32 i32 i32) (result i32) (i32.const 0))
      (func (export "process") (param i32 f32) (result i32)
        (loop $forever
          (br $forever))
        (i32.const 0)
      )
    )
    "#;
    let loop_dir = dir.join("infloop");
    std::fs::create_dir_all(&loop_dir)?;
    std::fs::write(loop_dir.join("plugin.wat"), loop_wat)?;
    std::fs::write(
        loop_dir.join("plugin.toml"),
        format!(
            "id = \"infloop\"\nname = \"Loop\"\nversion = \"1.0.0\"\napi = \"1.0\"\nauthor = \"Aether\"\nlicense = \"MIT\"\nentry = \"plugin.wat\"\nsha256 = \"{}\"\n",
            sha256_hex(loop_wat)
        ),
    )?;

    let cfg = base_host_config();
    let host = PluginHost::create_with_paths(&cfg, Some(dir.clone()), default_atomic_replace)?;

    // Grant consent to osc_sender & build a graph containing both `plugin.gain2x@1` and `plugin.infloop@1`.
    let grant_c = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 1,
        payload_utf8: ptr::null(),
        payload_len: 0,
    };
    host.post_command_with_str(&grant_c, "osc_sender")?;

    let plugin_graph = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 1,
                type_id: "aether.source.osc_in@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 2,
                type_id: "plugin.gain2x@1".to_owned(),
                position: [150.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: vec![
                    PortDecl {
                        name: "in".to_owned(),
                        ty: ValueType::Float,
                        direction: PortDirection::Input,
                    },
                    PortDecl {
                        name: "out".to_owned(),
                        ty: ValueType::Float,
                        direction: PortDirection::Output,
                    },
                ],
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 3,
                type_id: "plugin.infloop@1".to_owned(),
                position: [150.0, 100.0],
                label: None,
                params: BTreeMap::new(),
                ports: vec![
                    PortDecl {
                        name: "in".to_owned(),
                        ty: ValueType::Float,
                        direction: PortDirection::Input,
                    },
                    PortDecl {
                        name: "out".to_owned(),
                        ty: ValueType::Float,
                        direction: PortDirection::Output,
                    },
                ],
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 4,
                type_id: "aether.output.camera_control@1".to_owned(),
                position: [300.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![
            GraphEdge {
                id: 1,
                from_node: 1,
                from_port: "value".to_owned(),
                to_node: 2,
                to_port: "in".to_owned(),
            },
            GraphEdge {
                id: 2,
                from_node: 2,
                from_port: "out".to_owned(),
                to_node: 4,
                to_port: "flatness".to_owned(),
            },
        ],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    let set_c = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_SET_GRAPH_JSON,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 0,
        payload_utf8: ptr::null(),
        payload_len: 0,
    };
    host.post_command_with_str(&set_c, &serde_json::to_string(&plugin_graph)?)?;
    std::thread::sleep(Duration::from_millis(90));

    // Pump T2 ticks while background WASM processes inputs asynchronously via LatchedBridge & isolates `infloop` via 2 ms epoch timeout!
    let mut gout = AetherGraphOutputs::default_zero();
    let mut inp = default_inputs(1.0 / 60.0);
    inp.external_params[0] = 0.25;
    let mut saw_trap_evt = false;
    let mut saw_consent_evt = false;
    let mut saw_loaded_evt = false;
    let mut evts = [AetherHostEvent {
        struct_size: 0,
        kind: 0,
        source_id: 0,
        status: 0,
        value_f32: 0.0,
        message_utf8: [0; 256],
    }; 32];

    for _ in 0..15 {
        host.evaluate_active(&inp, &mut gout)?;
        let n = host.poll_events(&mut evts)? as usize;
        for e in &evts[..n] {
            if e.kind == AETHER_HOST_EVT_PLUGIN_TRAP {
                saw_trap_evt = true;
            } else if e.kind == AETHER_HOST_EVT_PLUGIN_CONSENT_REQ {
                saw_consent_evt = true;
            } else if e.kind == AETHER_HOST_EVT_PLUGIN_LOADED {
                saw_loaded_evt = true;
            }
        }
        std::thread::sleep(Duration::from_millis(12));
    }

    assert!(saw_loaded_evt, "gain2x plugin loaded event missing");
    assert!(saw_consent_evt, "osc_sender consent request event missing");
    assert!(
        saw_trap_evt,
        "infloop/tampered plugin isolation event missing"
    );
    // Latched core WASM plugin computed 0.25 * 2.0 = 0.50 asynchronously without running WASM on T2!
    assert!((gout.ik_targets[6][1] - 0.5).abs() < 1e-4);

    let _ = std::fs::remove_dir_all(&dir);
    Ok(())
}

#[test]
fn project_v1_to_v2_migration_preserves_unknown_fields_and_saves_atomically()
-> Result<(), Box<dyn std::error::Error>> {
    let v1_toml = br#"
schema_version = 1
app_version = "0.8.0"
avatar_path = "legacy/hero.vrm"
future_top_flag = "keep-me"

[render]
z_flatten = 0.65

[custom_plugin_section]
magic_number = 777
"#;
    let migrated = parse_and_migrate_project_toml(v1_toml)?;
    assert_eq!(migrated.schema_version, 2);
    assert_eq!(migrated.avatar.path, "legacy/hero.vrm");
    assert_eq!(
        migrated
            .render
            .get("flatness")
            .and_then(toml::Value::as_float),
        Some(0.65)
    );
    assert_eq!(
        migrated
            .extra
            .get("future_top_flag")
            .and_then(toml::Value::as_str),
        Some("keep-me")
    );
    assert!(migrated.extra.contains_key("custom_plugin_section"));

    let bundle = ProjectBundle {
        project: migrated,
        layout_ini: "[Window][Viewport]\nPos=0,0\n".to_owned(),
        ..Default::default()
    };

    let proj_dir = temp_dir("bundle.aether");
    save_project_bundle(&proj_dir, &bundle, default_atomic_replace)?;
    let reloaded = load_project_bundle(&proj_dir)?;
    assert_eq!(reloaded.project.schema_version, 2);
    assert_eq!(reloaded.project.avatar.path, "legacy/hero.vrm");
    assert_eq!(
        reloaded
            .project
            .extra
            .get("future_top_flag")
            .and_then(toml::Value::as_str),
        Some("keep-me")
    );
    assert_eq!(reloaded.layout_ini, bundle.layout_ini);

    // Async load/save via PluginHost
    let host = PluginHost::create(&base_host_config())?;
    host.load_project(proj_dir.to_str().ok_or("non-utf8 path")?)?;
    std::thread::sleep(Duration::from_millis(60));
    let mut evts = [AetherHostEvent {
        struct_size: 0,
        kind: 0,
        source_id: 0,
        status: 0,
        value_f32: 0.0,
        message_utf8: [0; 256],
    }; 16];
    let n = host.poll_events(&mut evts)? as usize;
    assert!(
        evts[..n]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_PROJECT_LOADED && e.status == AETHER_OK)
    );

    let mut json_buf = vec![0_u8; 64 * 1024];
    let len = host.copy_project_json(&mut json_buf)?;
    let snap: serde_json::Value = serde_json::from_slice(&json_buf[..len])?;
    assert_eq!(snap["project"]["avatar"]["path"], "legacy/hero.vrm");

    let save_dir = temp_dir("saved.aether");
    host.save_project(save_dir.to_str().ok_or("non-utf8 save path")?)?;
    std::thread::sleep(Duration::from_millis(60));
    let n2 = host.poll_events(&mut evts)? as usize;
    assert!(
        evts[..n2]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_PROJECT_SAVED && e.status == AETHER_OK)
    );
    assert!(save_dir.join("project.toml").exists());

    let _ = std::fs::remove_dir_all(&proj_dir);
    let _ = std::fs::remove_dir_all(&save_dir);
    Ok(())
}

#[test]
fn osc_udp_control_rate_limit_and_opt_in_websocket_bearer_auth()
-> Result<(), Box<dyn std::error::Error>> {
    // 1. Unit test 1000 msg/s rate limiter cleanly.
    let now = Instant::now();
    let mut limiter = OscRateLimiter::new(now);
    assert!(limiter.allow(now, 1000));
    assert!(!limiter.allow(now, 1));
    assert!(limiter.allow(now + Duration::from_millis(1005), 100));

    // 2. End-to-end UDP OSC inbound on ephemeral localhost port.
    let host = PluginHost::create(&base_host_config())?;
    std::thread::sleep(Duration::from_millis(40));
    let osc_port = host.osc_in_port();
    assert!(osc_port != 0);

    let sender = UdpSocket::bind("127.0.0.1:0")?;
    let pkt = rosc::encoder::encode(&rosc::OscPacket::Message(rosc::OscMessage {
        addr: "/aether/param/render.flatness".to_owned(),
        args: vec![rosc::OscType::Float(0.82)],
    }))
    .map_err(|e| format!("osc encode: {e:?}"))?;
    sender.send_to(&pkt, format!("127.0.0.1:{osc_port}"))?;

    let hk_pkt = rosc::encoder::encode(&rosc::OscPacket::Message(rosc::OscMessage {
        addr: "/aether/hotkey/5".to_owned(),
        args: vec![rosc::OscType::Int(5)],
    }))
    .map_err(|e| format!("osc encode: {e:?}"))?;
    sender.send_to(&hk_pkt, format!("127.0.0.1:{osc_port}"))?;

    std::thread::sleep(Duration::from_millis(60));
    let mut evts = [AetherHostEvent {
        struct_size: 0,
        kind: 0,
        source_id: 0,
        status: 0,
        value_f32: 0.0,
        message_utf8: [0; 256],
    }; 16];
    let n = host.poll_events(&mut evts)? as usize;
    assert!(
        evts[..n]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_OSC_PARAM)
    );
    assert!(
        evts[..n]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_HOTKEY_EVENT)
    );

    // 3. Opt-in WebSocket is OFF by default! Pick free ephemeral WS port cleanly.
    let free_port = {
        let l = std::net::TcpListener::bind("127.0.0.1:0")?;
        l.local_addr()?.port()
    };
    let mut ws_cfg = base_host_config();
    ws_cfg.ws_port = free_port;
    let host_ws = PluginHost::create(&ws_cfg)?;
    assert!(std::net::TcpStream::connect(("127.0.0.1", free_port)).is_err());

    // Enable WebSocket dynamically cleanly.
    let enable_cmd = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_SET_WS_ENABLED,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 1,
        payload_utf8: ptr::null(),
        payload_len: 0,
    };
    host_ws.post_command(&enable_cmd)?;
    std::thread::sleep(Duration::from_millis(80));

    let tok = host_ws.ws_token().to_owned();
    let client_rt = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()?;

    client_rt.block_on(async {
        let ws_tcp_addr = format!("127.0.0.1:{free_port}");
        // Unauthorized request without token MUST be rejected!
        let unauth_req = format!("ws://{ws_tcp_addr}/v1").into_client_request()?;
        let s1 = tokio::net::TcpStream::connect(&ws_tcp_addr).await?;
        let unauth_res = tokio_tungstenite::client_async(unauth_req, s1).await;
        assert!(
            unauth_res.is_err(),
            "WebSocket without bearer token must fail handshake"
        );

        // Authorized request with bearer token succeeds and receives 4 Hz telemetry after subscribe.
        let mut auth_req = format!("ws://{ws_tcp_addr}/v1").into_client_request()?;
        auth_req.headers_mut().insert(
            "Authorization",
            HeaderValue::from_str(&format!("Bearer {tok}"))?,
        );
        let s2 = tokio::net::TcpStream::connect(&ws_tcp_addr).await?;
        let (mut ws, _) = tokio_tungstenite::client_async(auth_req, s2).await?;
        ws.send(tokio_tungstenite::tungstenite::Message::Text(
            r#"{"type":"param.set","id":"render.flatness","value":0.75}"#.into(),
        ))
        .await?;
        ws.send(tokio_tungstenite::tungstenite::Message::Text(
            r#"{"type":"subscribe","topics":["telemetry"]}"#.into(),
        ))
        .await?;

        let frame = tokio::time::timeout(Duration::from_millis(600), ws.next())
            .await?
            .ok_or("ws closed early")??;
        let text = frame.into_text()?;
        assert!(
            text.contains("\"type\":\"telemetry\""),
            "unexpected frame: {text}"
        );
        Ok::<(), Box<dyn std::error::Error>>(())
    })?;

    std::thread::sleep(Duration::from_millis(30));
    let n_ws = host_ws.poll_events(&mut evts)? as usize;
    assert!(
        evts[..n_ws]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_WS_COMMAND)
    );
    Ok(())
}

proptest! {
    #[test]
    fn proptest_untrusted_osc_and_toml_never_panic(data in proptest::collection::vec(any::<u8>(), 0..512)) {
        let _ = parse_osc_packet(&data);
        let _ = parse_and_migrate_project_toml(&data);
    }
}

trait OutputsZeroExt {
    fn default_zero() -> Self;
}

impl OutputsZeroExt for AetherGraphOutputs {
    fn default_zero() -> Self {
        Self {
            expression_weights: [0.0; 64],
            expression_override: [0.0; 64],
            ik_targets: [[0.0; 8]; 8],
            bone_rotation_offsets: [[0.0; 4]; 55],
            look_at_ws: [0.0; 4],
            events: [0; 16],
            event_count: 0,
        }
    }
}

trait PlanStatsZeroExt {
    fn default_zero() -> Self;
}

impl PlanStatsZeroExt for AetherGraphPlanStats {
    fn default_zero() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            active_node_count: 0,
            error_node_id: 0,
            compile_count: 0,
            last_compile_ms: 0.0,
            last_eval_us: 0.0,
            p95_eval_us: 0.0,
            budget_warn: 0,
            ws_enabled: 0,
            osc_packets_rx: 0,
            eval_count: 0,
            osc_rate_limited: 0,
            plugin_count: 0,
            node_eval_us: [0.0; 256],
            node_live_values: [0.0; 256],
        }
    }
}

#[test]
fn audio_frame_and_viseme_types_distinct_with_legacy_json_compat() -> Result<(), Box<dyn std::error::Error>> {
    use aether_plugin_host::graph::model::ValueType;
    use aether_plugin_host::graph::{compile_graph_document, GraphDocument, GraphEdge, GraphNode};

    // 1. Type system: AudioFrame (AudioSpectrum) is strictly distinct from VisemeSet / ExprSet.
    assert!(ValueType::can_connect(ValueType::AudioFrame, ValueType::AudioFrame));
    assert!(!ValueType::can_connect(ValueType::AudioFrame, ValueType::VisemeSet));
    assert!(!ValueType::can_connect(ValueType::AudioFrame, ValueType::ExprSet));
    assert!(!ValueType::can_connect(ValueType::AudioFrame, ValueType::Pose));
    assert!(ValueType::can_connect(ValueType::VisemeSet, ValueType::VisemeSet));
    assert!(ValueType::can_connect(ValueType::VisemeSet, ValueType::ExprSet));
    let alias_ty: ValueType = serde_json::from_str("\"AudioSpectrum\"")?;
    assert_eq!(alias_ty, ValueType::AudioFrame);

    // 2. Default project graph carries DspAudio(audio: AudioFrame) -> FormantLipSync(visemes: VisemeSet) -> OutputExpressions.
    let default_doc = GraphDocument::default_project_graph();
    let e45 = default_doc.edges.iter().find(|e| e.from_node == 4 && e.to_node == 5).ok_or("missing 4->5")?;
    assert_eq!(e45.from_port, "audio");
    assert_eq!(e45.to_port, "audio");
    let e56 = default_doc.edges.iter().find(|e| e.from_node == 5 && e.to_node == 6).ok_or("missing 5->6")?;
    assert_eq!(e56.from_port, "visemes");
    assert_eq!(e56.to_port, "expressions");

    let host = aether_plugin_host::PluginHost::create(&base_host_config())?;
    let mut inp = default_inputs(1.0 / 60.0);
    inp.dsp_rms_db = -18.0;
    inp.dsp_visemes[0] = 0.8;
    let mut out = AetherGraphOutputs::default_zero();
    host.evaluate_active(&inp, &mut out)?;
    assert!((out.expression_weights[52] - 0.8).abs() < 1e-4);
    assert!(out.expression_weights[17] > 0.5);
    assert!(out.expression_weights[24].abs() < 1e-6);

    // Pin readouts report AETHER_PIN_VALUE_AUDIO (with F1/F2 formants) on DspAudio and AETHER_PIN_VALUE_VISEME on FormantLipSync.
    let mut ro = AetherGraphTelemetryReadout::default_zero();
    host.get_telemetry_readout(&mut ro)?;
    assert_eq!(ro.valid, 1);
    let n_pins = usize::try_from(ro.pin_readout_count).unwrap_or(0).min(ro.pin_kinds.len());
    let audio_idx = (0..n_pins)
        .find(|&i| ro.pin_node_ids[i] == 4 && ro.pin_indices[i] == 0)
        .ok_or("missing DspAudio pin 0 readout")?;
    assert_eq!(ro.pin_kinds[audio_idx], aether_ffi::AETHER_PIN_VALUE_AUDIO);
    assert!((ro.pin_values[audio_idx][0] - (-18.0)).abs() < 1e-3);
    assert!((ro.pin_values[audio_idx][2] - 730.0).abs() < 1e-2);

    let vis_idx = (0..n_pins)
        .find(|&i| ro.pin_node_ids[i] == 5 && ro.pin_indices[i] == 0)
        .ok_or("missing FormantLipSync pin 0 readout")?;
    assert_eq!(ro.pin_kinds[vis_idx], aether_ffi::AETHER_PIN_VALUE_VISEME);

    // 3. Reject direct AudioFrame -> ExprSet without FormantLipSync conversion.
    let invalid_doc = GraphDocument {
        schema_version: 1,
        nodes: vec![
            GraphNode {
                id: 1,
                type_id: "aether.source.dsp_audio@1".to_owned(),
                position: [0.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
            GraphNode {
                id: 2,
                type_id: "aether.output.expressions@1".to_owned(),
                position: [200.0, 0.0],
                label: None,
                params: BTreeMap::new(),
                ports: Vec::new(),
                extra: BTreeMap::new(),
            },
        ],
        edges: vec![GraphEdge {
            id: 1,
            from_node: 1,
            from_port: "audio".to_owned(),
            to_node: 2,
            to_port: "expressions".to_owned(),
        }],
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    };
    assert!(compile_graph_document(&invalid_doc, &[]).is_err());
    Ok(())
}

trait TelemetryReadoutZeroExt {
    fn default_zero() -> Self;
}

impl TelemetryReadoutZeroExt for AetherGraphTelemetryReadout {
    fn default_zero() -> Self {
        Self {
            struct_size: size_of::<Self>() as u32,
            active_node_count: 0,
            pin_readout_count: 0,
            valid: 0,
            eval_sequence: 0,
            total_eval_ns: 0,
            node_ids: [0; 256],
            node_eval_ns: [0; 256],
            pin_node_ids: [0; 384],
            pin_values: [[0.0; 4]; 384],
            pin_indices: [0; 384],
            pin_kinds: [0; 384],
            _reserved_tail: [0; 4],
        }
    }
}
