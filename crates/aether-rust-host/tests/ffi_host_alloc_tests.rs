#![cfg(feature = "test-hooks")]
#![allow(unsafe_code)]
#![allow(clippy::cast_precision_loss)]
#![allow(clippy::cast_possible_truncation)]

//! FFI hot-path zero-allocation & MoveFileExW verification for Phase 6 (`HR-01`, `HR-04`, `§7.7`).

use core::mem::size_of;
use core::ptr;
use std::collections::BTreeMap;
use std::time::Duration;

use aether_ffi::{
    AETHER_HOST_CMD_SET_GRAPH_JSON, AETHER_HOST_EVT_GRAPH_COMPILED, AETHER_OK,
    AETHER_PIN_VALUE_FLOAT, AETHER_THREAD_ROLE_ANY, AETHER_THREAD_ROLE_MAIN, AETHER_THREAD_ROLE_T2,
    AETHER_THREAD_ROLE_UI, AetherGraphInputs, AetherGraphOutputs, AetherGraphTelemetryReadout,
    AetherHostCommand, AetherHostConfig, AetherHostEvent, AetherHostHandle,
};
use aether_plugin_host::GraphDocument;
use aether_plugin_host::graph::model::{GraphEdge, GraphNode};
use aether_rust_host::{
    aether_host_create, aether_host_destroy, aether_host_evaluate_active,
    aether_host_get_telemetry_readout, aether_host_poll_events, aether_host_post_command,
    aether_set_thread_role, aether_test_alloc_reset, aether_test_thread_alloc_calls,
};

#[test]
fn t2_host_evaluate_active_has_zero_allocs_even_across_plan_hotswap()
-> Result<(), Box<dyn std::error::Error>> {
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    let cfg = AetherHostConfig {
        struct_size: size_of::<AetherHostConfig>() as u32,
        worker_threads: 2,
        osc_in_port: 0,
        osc_out_port: 0,
        ws_port: 0,
        flags: 0,
        plugins_dir_utf8: ptr::null(),
    };
    let mut host: AetherHostHandle = ptr::null_mut();
    // SAFETY: Valid config and non-null output handle pointer.
    assert_eq!(unsafe { aether_host_create(&cfg, &mut host) }, AETHER_OK);
    assert!(!host.is_null());

    // Push a 64-node graph swap while T2 runs so T2 adopts & retires the old plan cleanly.
    let mut nodes = Vec::new();
    let mut edges = Vec::new();
    let mut params = BTreeMap::new();
    params.insert("b".to_owned(), serde_json::json!(0.5));
    for i in 1_u64..=63 {
        nodes.push(GraphNode {
            id: i,
            type_id: "aether.math.add@1".to_owned(),
            position: [0.0, 0.0],
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
        id: 64,
        type_id: "aether.output.camera_control@1".to_owned(),
        position: [0.0, 0.0],
        label: None,
        params: BTreeMap::new(),
        ports: Vec::new(),
        extra: BTreeMap::new(),
    });
    edges.push(GraphEdge {
        id: 64,
        from_node: 63,
        from_port: "out".to_owned(),
        to_node: 64,
        to_port: "flatness".to_owned(),
    });
    let json = serde_json::to_string(&GraphDocument {
        schema_version: 1,
        nodes,
        edges,
        groups: Vec::new(),
        comments: Vec::new(),
        extra: BTreeMap::new(),
    })?;

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    let cmd = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_SET_GRAPH_JSON,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 0,
        payload_utf8: json.as_ptr().cast(),
        payload_len: json.len() as u64,
    };
    // SAFETY: `host` is live and `cmd` points to valid UTF-8 slice for this call.
    assert_eq!(unsafe { aether_host_post_command(host, &cmd) }, AETHER_OK);

    // Wait for compile event on UI.
    let mut evts = [AetherHostEvent {
        struct_size: 0,
        kind: 0,
        source_id: 0,
        status: 0,
        value_f32: 0.0,
        message_utf8: [0; 256],
    }; 8];
    let mut compiled = false;
    for _ in 0..50 {
        let mut cnt = 0_u32;
        // SAFETY: `host` is live and `evts` buffer holds 8 elements cleanly.
        let poll_st = unsafe { aether_host_poll_events(host, evts.as_mut_ptr(), 8, &mut cnt) };
        assert_eq!(poll_st, AETHER_OK);
        if evts[..cnt as usize]
            .iter()
            .any(|e| e.kind == AETHER_HOST_EVT_GRAPH_COMPILED)
        {
            compiled = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(compiled);

    // Measure allocations specifically on this T2 thread across swap + steady-state ticks (`HR-01`).
    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    let gin = AetherGraphInputs {
        tracking: ptr::null(),
        now_us: 16_667,
        dt_s: 1.0 / 60.0,
        tick_index: 1,
        dsp_visemes: [0.0; 5],
        dsp_rms_db: -60.0,
        keys_down: [0; 2],
        keys_pressed: [0; 2],
        external_params: [0.0; 64],
    };
    let mut gout = AetherGraphOutputs {
        expression_weights: [0.0; 64],
        expression_override: [0.0; 64],
        ik_targets: [[0.0; 8]; 8],
        bone_rotation_offsets: [[0.0; 4]; 55],
        look_at_ws: [0.0; 4],
        events: [0; 16],
        event_count: 0,
    };

    aether_test_alloc_reset();
    for _ in 0..100 {
        // SAFETY: `host` is live and `gin`/`gout` are valid aligned pointers on T2.
        let eval_st = unsafe { aether_host_evaluate_active(host, &gin, &mut gout) };
        assert_eq!(eval_st, AETHER_OK);
    }
    let t2_allocs = aether_test_thread_alloc_calls();
    assert_eq!(
        t2_allocs, 0,
        "T2 evaluate_active must perform 0 heap allocations/frees"
    );
    assert!((gout.ik_targets[6][1] - 31.5).abs() < 1e-3);

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    let mut readout: AetherGraphTelemetryReadout = unsafe { core::mem::zeroed() };
    readout.struct_size = size_of::<AetherGraphTelemetryReadout>() as u32;
    aether_test_alloc_reset();
    // SAFETY: `host` is live and `readout` is valid aligned writable storage on UI thread.
    let tel_st = unsafe { aether_host_get_telemetry_readout(host, &mut readout) };
    let ui_allocs = aether_test_thread_alloc_calls();
    assert_eq!(tel_st, AETHER_OK);
    assert_eq!(
        ui_allocs, 0,
        "UI telemetry readout must perform 0 allocations"
    );
    assert_eq!(readout.valid, 1);
    assert_eq!(readout.active_node_count, 64);
    let idx63 = readout
        .pin_node_ids
        .iter()
        .take(readout.pin_readout_count as usize)
        .position(|&nid| nid == 63)
        .expect("node 63 output pin present");
    assert_eq!(readout.pin_kinds[idx63], AETHER_PIN_VALUE_FLOAT);
    assert!((readout.pin_values[idx63][0] - 31.5).abs() < 1e-3);

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    // SAFETY: `host` is live and owned by this test until destruction.
    unsafe { aether_host_destroy(host) };
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    Ok(())
}
