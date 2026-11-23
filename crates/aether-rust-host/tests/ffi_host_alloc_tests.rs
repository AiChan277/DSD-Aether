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
    AETHER_HOST_CMD_SET_GRAPH_JSON, AETHER_HOST_EVT_GRAPH_COMPILED, AETHER_MAT_OVR_EMISSIVE,
    AETHER_MAT_OVR_GRADE, AETHER_MAT_OVR_PROFILE, AETHER_MAT_OVR_TEXTURE,
    AETHER_MAT_PROFILE_DUAL_LOBE_HAIR, AETHER_OK, AETHER_PIN_VALUE_FLOAT, AETHER_THREAD_ROLE_ANY,
    AETHER_THREAD_ROLE_MAIN, AETHER_THREAD_ROLE_T2, AETHER_THREAD_ROLE_UI, AetherGraphInputs,
    AetherGraphOutputs, AetherGraphTelemetryReadout, AetherHostCommand, AetherHostConfig,
    AetherHostEvent, AetherHostHandle, AetherMaterialOverrideBuffer,
};
use aether_plugin_host::GraphDocument;
use aether_plugin_host::graph::model::{GraphEdge, GraphNode};
use aether_rust_host::{
    aether_host_create, aether_host_destroy, aether_host_evaluate_active,
    aether_host_evaluate_active_ex, aether_host_get_telemetry_readout, aether_host_poll_events,
    aether_host_post_command, aether_host_set_model_metadata_json, aether_set_thread_role,
    aether_test_alloc_reset, aether_test_thread_alloc_calls,
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
    // SAFETY: `AetherGraphTelemetryReadout` is a POD C ABI struct valid when zero-initialized.
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
        .ok_or("node 63 output pin present")?;
    assert_eq!(readout.pin_kinds[idx63], AETHER_PIN_VALUE_FLOAT);
    assert!((readout.pin_values[idx63][0] - 31.5).abs() < 1e-3);

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    // SAFETY: `host` is live and owned by this test until destruction.
    unsafe { aether_host_destroy(host) };
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    Ok(())
}

#[test]
fn t2_material_texture_nodes_evaluate_with_zero_allocs() -> Result<(), Box<dyn std::error::Error>> {
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

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    let meta_cstr = std::ffi::CString::new(
        r#"{"format":1,"materials":["Face_00_SKIN","Hair_00_HAIR","Cardigan_00_CLOTH"]}"#,
    )?;
    // SAFETY: Valid host and NUL-terminated JSON string.
    let meta_st = unsafe { aether_host_set_model_metadata_json(host, meta_cstr.as_ptr()) };
    assert_eq!(meta_st, AETHER_OK);

    let graph_json = serde_json::json!({
        "schema_version": 1,
        "nodes": [
            { "id": 1, "type": "aether.model.source@1", "params": {} },
            { "id": 2, "type": "aether.texture.asset@1", "params": { "path": "tex/hair_ramp.png", "color_space": "srgb", "target_slot": "shade_tex" } },
            { "id": 3, "type": "aether.texture.grade@1", "params": { "hue_shift": 15.0, "saturation": 1.25, "brightness": 0.05, "contrast": 1.1 } },
            { "id": 4, "type": "aether.material.dynamic_driver@1", "params": { "signal_source": "audio_rms", "target_param": "emissive_intensity", "gain": 2.0 } },
            { "id": 5, "type": "aether.material.slot_override@1", "params": { "slot_name": "Hair_00_HAIR", "shader_profile": "Dual-Lobe Hair", "shade_shift": -0.12, "sss_fringe": 0.65, "outline_scale": 1.2 } }
        ],
        "edges": [
            { "id": 1, "from_node": 1, "from_port": "material_table", "to_node": 5, "to_port": "material_table" },
            { "id": 2, "from_node": 2, "from_port": "texture", "to_node": 3, "to_port": "texture" },
            { "id": 3, "from_node": 3, "from_port": "texture", "to_node": 5, "to_port": "texture" },
            { "id": 4, "from_node": 4, "from_port": "driver_out", "to_node": 5, "to_port": "dynamic_driver" }
        ]
    });
    let cstr = std::ffi::CString::new(graph_json.to_string())?;
    let cmd = AetherHostCommand {
        struct_size: size_of::<AetherHostCommand>() as u32,
        kind: AETHER_HOST_CMD_SET_GRAPH_JSON,
        target_id: 0,
        param_f32: 0.0,
        param_u32: 0,
        payload_len: cstr.as_bytes().len() as u64,
        payload_utf8: cstr.as_ptr(),
    };
    // SAFETY: Valid host and command pointer.
    assert_eq!(unsafe { aether_host_post_command(host, &cmd) }, AETHER_OK);

    let mut compiled = false;
    for _ in 0..40 {
        std::thread::sleep(Duration::from_millis(10));
        // SAFETY: `AetherHostEvent` is a POD C ABI struct valid when zero-initialized.
        let mut evs = [unsafe { core::mem::zeroed::<AetherHostEvent>() }; 8];
        let mut count = 0u32;
        // SAFETY: Valid host and event buffer.
        let _ = unsafe { aether_host_poll_events(host, evs.as_mut_ptr(), 8, &mut count) };
        for ev in &evs[..count as usize] {
            if ev.kind == AETHER_HOST_EVT_GRAPH_COMPILED {
                compiled = true;
            }
        }
        if compiled {
            break;
        }
    }
    assert!(compiled, "material graph must compile cleanly");

    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    let gin = AetherGraphInputs {
        tracking: ptr::null(),
        now_us: 1_000_000,
        dt_s: 1.0 / 60.0,
        tick_index: 10,
        dsp_visemes: [0.0; 5],
        dsp_rms_db: -15.0,
        keys_down: [0; 2],
        keys_pressed: [0; 2],
        external_params: [0.0; 64],
    };
    // SAFETY: `AetherGraphOutputs` and `AetherMaterialOverrideBuffer` are POD C ABI structs.
    let mut gout: AetherGraphOutputs = unsafe { core::mem::zeroed() };
    // SAFETY: `AetherMaterialOverrideBuffer` is a POD C ABI struct valid when zero-initialized.
    let mut gmats: AetherMaterialOverrideBuffer = unsafe { core::mem::zeroed() };

    aether_test_alloc_reset();
    for _ in 0..100 {
        // SAFETY: Valid T2 pointers and 64-byte aligned outputs.
        let st = unsafe { aether_host_evaluate_active_ex(host, &gin, 18.0, &mut gout, &mut gmats) };
        assert_eq!(st, AETHER_OK);
    }
    assert_eq!(
        aether_test_thread_alloc_calls(),
        0,
        "T2 evaluate_active_ex with all 5 material/texture nodes must perform 0 heap allocations"
    );
    assert_eq!(gmats.count, 1);
    let e0 = &gmats.entries[0];
    assert_eq!(e0.material_index, 1); // "Hair_00_HAIR" -> slot 1
    assert_eq!(e0.shader_profile, AETHER_MAT_PROFILE_DUAL_LOBE_HAIR);
    assert_ne!(
        e0.override_mask
            & (AETHER_MAT_OVR_PROFILE
                | AETHER_MAT_OVR_TEXTURE
                | AETHER_MAT_OVR_GRADE
                | AETHER_MAT_OVR_EMISSIVE),
        0
    );
    assert!((e0.grade_hsbc[0] - 15.0).abs() < 1e-4);
    assert!((e0.dynamic_params[0] - 1.5).abs() < 1e-3);

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    // SAFETY: Valid host owned by this test.
    unsafe { aether_host_destroy(host) };
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    Ok(())
}
