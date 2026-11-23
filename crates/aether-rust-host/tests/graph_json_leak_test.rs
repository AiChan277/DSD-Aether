//! Process-isolated 100x graph JSON serialization/deserialization leak test (`test-hooks`).
#![cfg(feature = "test-hooks")]

use core::ffi::c_char;
use core::mem::size_of;
use core::ptr;

use aether_ffi::{
    AETHER_OK, AETHER_THREAD_ROLE_ANY, AETHER_THREAD_ROLE_MAIN, AETHER_THREAD_ROLE_UI,
    AetherGraphPlanStats, AetherHostConfig, AetherHostHandle,
};
use aether_rust_host::{
    aether_graph_deserialize_json, aether_graph_serialize_json, aether_host_create,
    aether_host_destroy, aether_host_get_plan_stats, aether_set_thread_role, aether_string_free,
    aether_test_alloc_live_bytes,
};

const SAMPLE_GRAPH_JSON: &str = r#"{
    "schema_version": 1,
    "nodes": [
        { "id": 1, "type": "aether.source.npu_pose@1", "position": [10.0, 20.0] },
        { "id": 2, "type": "aether.output.pose@1", "position": [240.0, 20.0] }
    ],
    "edges": [
        { "id": 10, "from_node": 1, "from_port": "pose", "to_node": 2, "to_port": "pose" }
    ],
    "groups": [],
    "comments": []
}"#;

fn create_test_host() -> AetherHostHandle {
    let host_cfg = AetherHostConfig {
        struct_size: size_of::<AetherHostConfig>() as u32,
        worker_threads: 1,
        osc_in_port: 0,
        osc_out_port: 0,
        ws_port: 0,
        flags: 0,
        plugins_dir_utf8: ptr::null(),
    };
    let mut host: AetherHostHandle = ptr::null_mut();
    // SAFETY: Valid config struct and output slot on MAIN role.
    let rc = unsafe { aether_host_create(&host_cfg, &mut host) };
    assert_eq!(rc, AETHER_OK);
    assert!(!host.is_null());
    host
}

fn wait_for_compiled_nodes(host: AetherHostHandle, expected_nodes: u32) {
    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    for _ in 0..500_000 {
        // SAFETY: Plain-data C struct zero-initialized with struct_size set.
        let mut stats: AetherGraphPlanStats = unsafe { core::mem::zeroed() };
        stats.struct_size = size_of::<AetherGraphPlanStats>() as u32;
        // SAFETY: Valid host and writable stats pointer on UI role.
        if unsafe { aether_host_get_plan_stats(host, &mut stats) } == AETHER_OK
            && stats.active_node_count == expected_nodes
        {
            aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
            return;
        }
        std::thread::yield_now();
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
}

#[test]
fn rev31_graph_json_100x_no_heap_leak() {
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);

    // Warmup cycle to initialize static tokio / watch / serde slabs before baseline capture.
    {
        let warmup_host = create_test_host();
        // SAFETY: Valid host and UTF-8 JSON buffer.
        let rc_in = unsafe {
            aether_graph_deserialize_json(
                warmup_host,
                SAMPLE_GRAPH_JSON.as_ptr().cast::<c_char>(),
                SAMPLE_GRAPH_JSON.len(),
            )
        };
        assert_eq!(rc_in, AETHER_OK);
        wait_for_compiled_nodes(warmup_host, 2);
        let mut ptr_out: *mut c_char = ptr::null_mut();
        let mut len_out: usize = 0;
        // SAFETY: Valid host and output pointers.
        let rc_out =
            unsafe { aether_graph_serialize_json(warmup_host, &mut ptr_out, &mut len_out) };
        assert_eq!(rc_out, AETHER_OK);
        // SAFETY: Owned pointer from `aether_graph_serialize_json`.
        unsafe { aether_string_free(ptr_out) };
        // SAFETY: Destroys warmup host and joins runtime.
        unsafe { aether_host_destroy(warmup_host) };
    }

    // 1. Live host steady-state check: serialize + aether_string_free x 100
    //    (compare post-100 warmup vs next 100 so async tokio startup slabs have already settled).
    let ser_host = create_test_host();
    // SAFETY: Valid host and UTF-8 JSON buffer.
    let rc_seed = unsafe {
        aether_graph_deserialize_json(
            ser_host,
            SAMPLE_GRAPH_JSON.as_ptr().cast::<c_char>(),
            SAMPLE_GRAPH_JSON.len(),
        )
    };
    assert_eq!(rc_seed, AETHER_OK);
    wait_for_compiled_nodes(ser_host, 2);
    for _ in 0..100 {
        let mut ptr_out: *mut c_char = ptr::null_mut();
        let mut len_out: usize = 0;
        // SAFETY: Valid host and writable output slots.
        let rc_out = unsafe { aether_graph_serialize_json(ser_host, &mut ptr_out, &mut len_out) };
        assert_eq!(rc_out, AETHER_OK);
        // SAFETY: Owned pointer from `aether_graph_serialize_json`.
        unsafe { aether_string_free(ptr_out) };
    }
    let ser_live_mid = aether_test_alloc_live_bytes();
    for _ in 0..100 {
        let mut ptr_out: *mut c_char = ptr::null_mut();
        let mut len_out: usize = 0;
        // SAFETY: Valid host and writable output slots.
        let rc_out = unsafe { aether_graph_serialize_json(ser_host, &mut ptr_out, &mut len_out) };
        assert_eq!(rc_out, AETHER_OK);
        assert!(len_out > 0);
        // SAFETY: Owned pointer from `aether_graph_serialize_json`.
        unsafe { aether_string_free(ptr_out) };
    }
    let ser_live_after = aether_test_alloc_live_bytes();
    assert!(
        ser_live_after <= ser_live_mid,
        "heap growth across 100x serialize+free: before={ser_live_mid}, after={ser_live_after}"
    );
    // SAFETY: Destroys host created by `create_test_host`.
    unsafe { aether_host_destroy(ser_host) };

    // 2. Full create -> 100x deserialize + serialize + free -> destroy must leave <= baseline bytes.
    let live_before = aether_test_alloc_live_bytes();
    let host = create_test_host();
    for _ in 0..100 {
        // SAFETY: Valid host and immutable UTF-8 JSON buffer.
        let rc_in = unsafe {
            aether_graph_deserialize_json(
                host,
                SAMPLE_GRAPH_JSON.as_ptr().cast::<c_char>(),
                SAMPLE_GRAPH_JSON.len(),
            )
        };
        assert_eq!(rc_in, AETHER_OK);
        let mut ptr_out: *mut c_char = ptr::null_mut();
        let mut len_out: usize = 0;
        // SAFETY: Valid host and writable output slots.
        let rc_out = unsafe { aether_graph_serialize_json(host, &mut ptr_out, &mut len_out) };
        assert_eq!(rc_out, AETHER_OK);
        // SAFETY: Owned pointer from `aether_graph_serialize_json`.
        unsafe { aether_string_free(ptr_out) };
    }
    // SAFETY: Destroys host created by `create_test_host`.
    unsafe { aether_host_destroy(host) };
    let live_after = aether_test_alloc_live_bytes();
    assert!(
        live_after <= live_before,
        "heap growth across 100x deserialize/serialize/free: before={live_before}, after={live_after}"
    );

    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}
