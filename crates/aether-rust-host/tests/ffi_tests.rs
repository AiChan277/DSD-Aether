#![allow(clippy::pedantic)]

//! Rust-side contract, panic-boundary, thread-role, zero-copy borrow, and allocator tests.

use core::ffi::c_char;
use core::mem::{align_of, size_of};
use core::ptr;

#[cfg(feature = "test-hooks")]
use aether_ffi::{
    AETHER_E_BUFFER_TOO_SMALL, AETHER_E_INVALID_ARG, AETHER_E_PANIC, AETHER_STREAM_INDEX_U32,
    AETHER_STREAM_SKIN, AETHER_STREAM_SURFACE, AETHER_STREAM_VERTEX25D, AETHER_THREAD_ROLE_T2,
    AetherArenaSlice, AetherAssetRequirements, AetherAvatarDesc, AetherGraphInputs,
    AetherGraphOutputs, AetherPlanHandle,
};
use aether_ffi::{
    AETHER_E_INTERNAL, AETHER_FFI_VERSION_MAJOR, AETHER_FFI_VERSION_MINOR, AETHER_OK,
    AETHER_THREAD_ROLE_ANY, AETHER_THREAD_ROLE_LOADER, AETHER_THREAD_ROLE_MAIN,
    AETHER_THREAD_ROLE_UI, AetherDspSyncPacket, AetherGraphPlanStats, AetherHostConfig,
    AetherHostHandle, AetherLoadRequest, AetherLoaderHandle, AetherRuntimeConfig,
    AetherTrackingFrame,
};

use aether_rust_host::{
    aether_ffi_layout_table_json, aether_ffi_version, aether_host_create, aether_host_destroy,
    aether_host_get_plan_stats, aether_last_error, aether_loader_open, aether_runtime_init,
    aether_runtime_shutdown, aether_set_thread_role,
};
#[cfg(feature = "test-hooks")]
use aether_rust_host::{
    aether_graph_evaluate, aether_graph_release, aether_loader_close, aether_loader_fill,
    aether_loader_progress, aether_loader_query, aether_test_alloc_reset,
    aether_test_graph_noop_plan, aether_test_inject_panic, aether_test_loader_open_synthetic,
    aether_test_thread_alloc_bytes, aether_test_thread_alloc_calls,
};

fn read_tls_error() -> String {
    let mut buf = [0u8; 512];
    // SAFETY: `buf` is valid writable storage for `buf.len()` bytes.
    let n = unsafe { aether_last_error(buf.as_mut_ptr().cast::<c_char>(), buf.len()) };
    let used = n.min(buf.len() - 1);
    String::from_utf8_lossy(&buf[..used]).into_owned()
}

#[test]
fn version_and_layout_table_present() {
    assert_eq!(
        aether_ffi_version(),
        (AETHER_FFI_VERSION_MAJOR << 16) | AETHER_FFI_VERSION_MINOR
    );

    let mut len: usize = 0;
    // SAFETY: `len` is valid writable stack storage.
    let ptr = unsafe { aether_ffi_layout_table_json(&mut len) };
    assert!(!ptr.is_null());
    assert!(len > 100);
    // SAFETY: `aether_ffi_layout_table_json` returns a `'static` slice pointer of length `len`.
    let bytes = unsafe { core::slice::from_raw_parts(ptr, len) };
    let parsed = core::str::from_utf8(bytes);
    assert!(parsed.is_ok());
    let text = parsed.unwrap_or("");
    assert!(text.contains("\"AetherVertex25D\""));
    assert!(text.contains("\"AetherTrackingFrame\""));
    assert!(text.contains("\"AetherDspSyncPacket\""));

    assert_eq!(size_of::<AetherTrackingFrame>(), 2048);
    assert_eq!(align_of::<AetherTrackingFrame>(), 64);
    assert_eq!(size_of::<AetherDspSyncPacket>(), 128);
    assert_eq!(align_of::<AetherDspSyncPacket>(), 64);
}

#[test]
fn production_stubs_return_unsupported_and_runtime_init_validates() {
    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    let cfg = AetherRuntimeConfig {
        struct_size: size_of::<AetherRuntimeConfig>() as u32,
        log_level: 1,
        worker_threads: 0,
        flags: 0,
        app_data_dir_utf8: ptr::null(),
        cache_dir_utf8: ptr::null(),
    };
    // SAFETY: `cfg` points to valid initialized `AetherRuntimeConfig`.
    assert_eq!(unsafe { aether_runtime_init(&cfg) }, AETHER_OK);

    let host_cfg = AetherHostConfig {
        struct_size: size_of::<AetherHostConfig>() as u32,
        worker_threads: 2,
        osc_in_port: 0,
        osc_out_port: 9001,
        ws_port: 19190,
        flags: 0,
        plugins_dir_utf8: ptr::null(),
    };
    let mut host: AetherHostHandle = ptr::null_mut();
    // SAFETY: Valid config and output handle slot on stack.
    let host_rc = unsafe { aether_host_create(&host_cfg, &mut host) };
    assert_eq!(host_rc, AETHER_OK);
    assert!(!host.is_null());

    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    // SAFETY: C-ABI plain-data struct safely zero-initialized.
    let mut stats: AetherGraphPlanStats = unsafe { core::mem::zeroed() };
    stats.struct_size = size_of::<AetherGraphPlanStats>() as u32;
    // SAFETY: `host` is live and `stats` is initialized writable storage.
    let stats_rc = unsafe { aether_host_get_plan_stats(host, &mut stats) };
    assert_eq!(stats_rc, AETHER_OK);
    assert_eq!(stats.active_node_count, 6);

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    // SAFETY: `host` is valid and owned by this test until destruction.
    unsafe { aether_host_destroy(host) };

    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    let path = b"sample.vrm\0";
    let req = AetherLoadRequest {
        path_utf8: path.as_ptr().cast::<c_char>(),
        flags: 0,
        max_threads: 0,
    };
    let mut loader: AetherLoaderHandle = ptr::null_mut();
    // SAFETY: Valid request and output handle slot on stack.
    let loader_rc = unsafe { aether_loader_open(&req, &mut loader) };
    assert_eq!(loader_rc, aether_ffi::AETHER_E_IO);
    assert!(loader.is_null());
    assert!(read_tls_error().contains("open"));

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
    aether_runtime_shutdown();
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}

#[test]
fn debug_thread_role_violation_caught() {
    if !cfg!(debug_assertions) {
        return;
    }
    aether_set_thread_role(AETHER_THREAD_ROLE_UI);
    let path = b"sample.vrm\0";
    let req = AetherLoadRequest {
        path_utf8: path.as_ptr().cast::<c_char>(),
        flags: 0,
        max_threads: 0,
    };
    let mut loader: AetherLoaderHandle = ptr::null_mut();
    // SAFETY: Valid request and output handle slot; testing debug role violation response.
    let role_rc = unsafe { aether_loader_open(&req, &mut loader) };
    assert_eq!(role_rc, AETHER_E_INTERNAL);
    assert!(read_tls_error().contains("thread-role"));
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}

#[cfg(feature = "test-hooks")]
#[test]
fn panic_boundary_and_zero_copy_borrow_and_zero_alloc_eval() {
    // 1. Panic boundary test (HR-04).
    let status = aether_test_inject_panic();
    assert_eq!(status, AETHER_E_PANIC);
    assert!(read_tls_error().contains("intentional boundary panic"));

    // 2. Two-phase zero-copy borrow + slice rejection tests (HR-05, REQ-FFI-010).
    aether_set_thread_role(AETHER_THREAD_ROLE_LOADER);
    let mut loader: AetherLoaderHandle = ptr::null_mut();
    // SAFETY: Stack output slot valid for synthetic loader handle.
    let open_rc = unsafe { aether_test_loader_open_synthetic(64, 192, &mut loader) };
    assert_eq!(open_rc, AETHER_OK);
    assert!(!loader.is_null());

    let mut req: AetherAssetRequirements = bytemuck::Zeroable::zeroed();
    // SAFETY: Valid `loader` handle and writable stack `req`.
    assert_eq!(unsafe { aether_loader_query(loader, &mut req) }, AETHER_OK);
    assert_eq!(req.vertex_count, 64);
    assert_eq!(req.index_count, 192);
    assert_eq!(req.stream_bytes[AETHER_STREAM_VERTEX25D as usize], 64 * 64);

    #[repr(C, align(64))]
    #[derive(Clone, Copy)]
    struct Align64([u8; 64]);

    let mut v25_buf = vec![Align64([0; 64]); 64];
    let mut surf_buf = vec![0u64; 256];
    let mut skin_buf = vec![0u64; 256];
    let mut idx_buf = vec![0u32; 192];

    let slices = [
        AetherArenaSlice {
            ptr: v25_buf.as_mut_ptr().cast::<u8>(),
            size_bytes: req.stream_bytes[AETHER_STREAM_VERTEX25D as usize],
            stream_kind: AETHER_STREAM_VERTEX25D,
            _pad: 0,
        },
        AetherArenaSlice {
            ptr: surf_buf.as_mut_ptr().cast::<u8>(),
            size_bytes: req.stream_bytes[AETHER_STREAM_SURFACE as usize],
            stream_kind: AETHER_STREAM_SURFACE,
            _pad: 0,
        },
        AetherArenaSlice {
            ptr: skin_buf.as_mut_ptr().cast::<u8>(),
            size_bytes: req.stream_bytes[AETHER_STREAM_SKIN as usize],
            stream_kind: AETHER_STREAM_SKIN,
            _pad: 0,
        },
        AetherArenaSlice {
            ptr: idx_buf.as_mut_ptr().cast::<u8>(),
            size_bytes: req.stream_bytes[AETHER_STREAM_INDEX_U32 as usize],
            stream_kind: AETHER_STREAM_INDEX_U32,
            _pad: 0,
        },
    ];

    // SAFETY: C-ABI plain-data struct safely zero-initialized.
    let mut desc: AetherAvatarDesc = unsafe { core::mem::zeroed() };

    let mut short_slices = slices;
    short_slices[0].size_bytes -= 4;
    // SAFETY: Stack descriptors valid for checking short buffer rejection.
    let short_rc = unsafe { aether_loader_fill(loader, short_slices.as_ptr(), 4, &mut desc) };
    assert_eq!(short_rc, AETHER_E_BUFFER_TOO_SMALL);

    let mut overlap_slices = slices;
    // SAFETY: Computes interior pointer within `v25_buf` purely to test overlap rejection.
    overlap_slices[1].ptr = unsafe { overlap_slices[0].ptr.add(64) };
    // SAFETY: Descriptors valid; overlap detector must reject before borrowing.
    let overlap_rc = unsafe { aether_loader_fill(loader, overlap_slices.as_ptr(), 4, &mut desc) };
    assert_eq!(overlap_rc, AETHER_E_INVALID_ARG);

    aether_test_alloc_reset();
    // SAFETY: Disjoint, aligned, owned stack `Vec` buffers valid across `aether_loader_fill`.
    let fill_rc = unsafe { aether_loader_fill(loader, slices.as_ptr(), 4, &mut desc) };
    assert_eq!(fill_rc, AETHER_OK);
    assert_eq!(aether_test_thread_alloc_calls(), 0);
    assert_eq!(aether_test_thread_alloc_bytes(), 0);
    // SAFETY: Valid live `loader`.
    assert!((unsafe { aether_loader_progress(loader) } - 1.0).abs() < f32::EPSILON);
    assert_eq!(
        desc.debug_stream_ptrs[AETHER_STREAM_VERTEX25D as usize],
        slices[0].ptr as usize as u64
    );
    assert_eq!(
        desc.debug_stream_ptrs[AETHER_STREAM_INDEX_U32 as usize],
        slices[3].ptr as usize as u64
    );
    assert_eq!(idx_buf[191], 191 % 64);

    // SAFETY: Transfers ownership of `loader` back for cleanup.
    unsafe { aether_loader_close(loader) };

    // 3. Allocation-free graph evaluation test (HR-01).
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    let mut plan: AetherPlanHandle = ptr::null_mut();
    // SAFETY: Stack output slot valid for synthetic graph plan handle.
    assert_eq!(unsafe { aether_test_graph_noop_plan(&mut plan) }, AETHER_OK);
    assert!(!plan.is_null());

    aether_set_thread_role(AETHER_THREAD_ROLE_T2);
    // SAFETY: C-ABI POD struct safely zero-initialized.
    let mut tracking: AetherTrackingFrame = unsafe { core::mem::zeroed() };
    tracking.arkit_blendshapes[0] = 0.75;
    let inputs = AetherGraphInputs {
        tracking: &tracking,
        now_us: 1000,
        dt_s: 1.0 / 240.0,
        tick_index: 1,
        dsp_visemes: [0.5, 0.0, 0.0, 0.0, 0.0],
        dsp_rms_db: -24.0,
        keys_down: [0; 2],
        keys_pressed: [0; 2],
        external_params: [0.0; 64],
    };
    // SAFETY: C-ABI POD struct safely zero-initialized.
    let mut outputs: AetherGraphOutputs = unsafe { core::mem::zeroed() };

    aether_test_alloc_reset();
    // SAFETY: Valid plan handle, immutable inputs, exclusive stack outputs.
    let eval_rc = unsafe { aether_graph_evaluate(plan, &inputs, &mut outputs) };
    assert_eq!(eval_rc, AETHER_OK);
    assert_eq!(aether_test_thread_alloc_calls(), 0);
    assert_eq!(aether_test_thread_alloc_bytes(), 0);
    assert!((outputs.expression_weights[0] - 0.75).abs() < 1e-6);
    assert!((outputs.expression_weights[52] - 0.5).abs() < 1e-6);

    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
    // SAFETY: Releases ownership of test plan handle.
    unsafe { aether_graph_release(plan) };
}

#[test]
fn rev31_graph_json_roundtrip_100x_and_error_handling() {
    use aether_ffi::AETHER_E_PARSE;
    use aether_rust_host::{
        aether_graph_deserialize_json, aether_graph_serialize_json, aether_string_free,
    };
    use core::ffi::CStr;

    aether_set_thread_role(AETHER_THREAD_ROLE_MAIN);
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
    // SAFETY: Valid config and writable host output slot on main thread.
    let rc_create = unsafe { aether_host_create(&host_cfg, &mut host) };
    assert_eq!(rc_create, AETHER_OK);
    assert!(!host.is_null());

    // Malformed JSON must fail cleanly with AETHER_E_PARSE without corrupting host state.
    let bad_json = b"{not-valid-graph-json";
    // SAFETY: Valid host handle and bounded byte buffer.
    let bad_rc = unsafe {
        aether_graph_deserialize_json(host, bad_json.as_ptr().cast::<c_char>(), bad_json.len())
    };
    assert_eq!(bad_rc, AETHER_E_PARSE);

    let sample_json = r#"{
        "schema_version": 1,
        "nodes": [
            { "id": 1, "type": "aether.source.npu_pose@1", "position": [10.0, 20.0] },
            { "id": 2, "type": "aether.output.pose@1", "position": [240.0, 20.0] },
            { "id": 3, "type": "aether.output.expressions@1", "position": [240.0, 140.0] }
        ],
        "edges": [
            { "id": 10, "from_node": 1, "from_port": "pose", "to_node": 2, "to_port": "pose" },
            { "id": 11, "from_node": 1, "from_port": "expressions", "to_node": 3, "to_port": "expressions" }
        ],
        "groups": [
            { "id": 20, "title": "Tracking", "position": [0.0, 0.0], "size": [320.0, 220.0], "nodes": [1, 2, 3] }
        ],
        "comments": []
    }"#;

    for iter_idx in 0..100 {
        // SAFETY: Valid host handle and immutable UTF-8 JSON buffer for this call.
        let rc_in = unsafe {
            aether_graph_deserialize_json(
                host,
                sample_json.as_ptr().cast::<c_char>(),
                sample_json.len(),
            )
        };
        assert_eq!(
            rc_in, AETHER_OK,
            "deserialize failed on iteration {iter_idx}"
        );

        let mut out_ptr: *mut c_char = ptr::null_mut();
        let mut out_len: usize = 0;
        // SAFETY: Valid host handle and writable output slots.
        let rc_out = unsafe { aether_graph_serialize_json(host, &mut out_ptr, &mut out_len) };
        assert_eq!(
            rc_out, AETHER_OK,
            "serialize failed on iteration {iter_idx}"
        );
        assert!(!out_ptr.is_null());
        assert!(out_len > 0);

        // SAFETY: `out_ptr` is NUL-terminated UTF-8 owned until `aether_string_free`.
        let c_bytes = unsafe { CStr::from_ptr(out_ptr) }.to_bytes();
        assert_eq!(c_bytes.len(), out_len);
        let parsed: Result<serde_json::Value, _> = serde_json::from_slice(c_bytes);
        assert!(parsed.is_ok());
        if let Ok(doc) = parsed {
            assert_eq!(doc["nodes"].as_array().map_or(0, Vec::len), 3);
            assert_eq!(doc["edges"].as_array().map_or(0, Vec::len), 2);
            assert_eq!(doc["groups"].as_array().map_or(0, Vec::len), 1);
        }

        // SAFETY: Frees CString returned by `aether_graph_serialize_json`.
        unsafe { aether_string_free(out_ptr) };
    }

    // SAFETY: Null pointer is a documented no-op.
    unsafe { aether_string_free(ptr::null_mut()) };

    // SAFETY: Valid live host owned by this test.
    unsafe { aether_host_destroy(host) };
    aether_set_thread_role(AETHER_THREAD_ROLE_ANY);
}
