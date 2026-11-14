#![deny(clippy::pedantic)]
#![allow(clippy::module_name_repetitions, clippy::pub_underscore_fields)]

//! Frozen C ABI contract definitions for AETHER-2.5D (`AETHER-TS-001` §5).
//!
//! `cbindgen` processes this crate directly to generate and verify `include/aether_ffi_contract.h`.

pub mod layout_asserts;
pub mod status;
pub mod types;

use core::ffi::c_char;

pub use status::*;
pub use types::*;

/// Build-time generated `layout_table.json` (`REQ-FFI-005`).
pub const LAYOUT_TABLE_JSON: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/layout_table.json"));

// Declarations used by `cbindgen` to emit the authoritative C ABI header (`include/aether_ffi_contract.h`).
// Implementations are exported by the single staticlib facade `crates/aether-rust-host` (`REQ-CORE-001`).
unsafe extern "C" {
    /// Returns `(AETHER_FFI_VERSION_MAJOR << 16) | AETHER_FFI_VERSION_MINOR`. `[any]`
    pub fn aether_ffi_version() -> u32;

    /// Copies the thread-local last error message (UTF-8, NUL-terminated) into `buf`.
    /// Returns the total byte length of the error message excluding the NUL terminator. `[any]`
    pub fn aether_last_error(buf: *mut c_char, len: usize) -> usize;

    /// Sets the calling thread's role tag (`AETHER_THREAD_ROLE_*`) for debug affinity checks (`REQ-FFI-003`). `[any]`
    pub fn aether_set_thread_role(role: u32);

    /// Initializes the global Rust runtime (tracing subscriber, worker configuration). `[main]`
    pub fn aether_runtime_init(cfg: *const AetherRuntimeConfig) -> AetherStatus;

    /// Shuts down the global Rust runtime. `[main]`
    pub fn aether_runtime_shutdown();

    /// Opens an avatar model file (mmap + header parse, zero bulk decode). `[loader]`
    pub fn aether_loader_open(
        req: *const AetherLoadRequest,
        out: *mut AetherLoaderHandle,
    ) -> AetherStatus;

    /// Queries exact byte requirements per `AETHER_STREAM_*` slot. `[loader]`
    pub fn aether_loader_query(
        loader: AetherLoaderHandle,
        out: *mut AetherAssetRequirements,
    ) -> AetherStatus;

    /// Validates C++-lent arena slices and decodes/bakes zero-copy directly into them. `[loader]`
    pub fn aether_loader_fill(
        loader: AetherLoaderHandle,
        slices: *const AetherArenaSlice,
        count: u32,
        out: *mut AetherAvatarDesc,
    ) -> AetherStatus;

    /// Returns load progress in `[0.0, 1.0]`. `[any]`
    pub fn aether_loader_progress(loader: AetherLoaderHandle) -> f32;

    /// Requests cooperative cancellation of an active load job. `[any]`
    pub fn aether_loader_cancel(loader: AetherLoaderHandle);

    /// Closes a loader handle and releases its Rust-side state/mmap. `[loader]`
    pub fn aether_loader_close(loader: AetherLoaderHandle);

    /// Creates the Rust plugin host and node graph engine. `[main]`
    pub fn aether_host_create(
        cfg: *const AetherHostConfig,
        out: *mut AetherHostHandle,
    ) -> AetherStatus;

    /// Starts asynchronous project loading. `[ui]`
    pub fn aether_host_load_project(
        host: AetherHostHandle,
        path_utf8: *const c_char,
    ) -> AetherStatus;

    /// Starts asynchronous atomic project saving. `[ui]`
    pub fn aether_host_save_project(
        host: AetherHostHandle,
        path_utf8: *const c_char,
    ) -> AetherStatus;

    /// Enqueues a structural host/graph command from the UI thread. `[ui]`
    pub fn aether_host_post_command(
        host: AetherHostHandle,
        cmd: *const AetherHostCommand,
    ) -> AetherStatus;

    /// Drains pending plugin host events to the UI thread. `[ui]`
    pub fn aether_host_poll_events(
        host: AetherHostHandle,
        out: *mut AetherHostEvent,
        cap: u32,
        count: *mut u32,
    ) -> AetherStatus;

    /// Compiles a node graph JSON document into an allocation-free evaluation plan. `[plugin]`
    pub fn aether_graph_compile(
        host: AetherHostHandle,
        json: *const u8,
        len: usize,
        out: *mut AetherPlanHandle,
    ) -> AetherStatus;

    /// Evaluates a compiled graph plan deterministically with zero allocation in <= 100 us. `[T2]`
    pub fn aether_graph_evaluate(
        plan: AetherPlanHandle,
        inputs: *const AetherGraphInputs,
        outputs: *mut AetherGraphOutputs,
    ) -> AetherStatus;

    /// Releases a compiled graph plan after T2 epoch acknowledgement. `[any]`
    pub fn aether_graph_release(plan: AetherPlanHandle);

    /// Destroys the Rust plugin host and joins its workers. `[main]`
    pub fn aether_host_destroy(host: AetherHostHandle);

    /// Wait-free, allocation-free T2 evaluation of the currently published `CompiledGraphPlan` (ADR-0014). `[T2]`
    pub fn aether_host_evaluate_active(
        host: AetherHostHandle,
        inputs: *const AetherGraphInputs,
        outputs: *mut AetherGraphOutputs,
    ) -> AetherStatus;

    /// Copies the normalized loaded/active project & runtime JSON snapshot into `out_buf` (ADR-0014). `[ui]`
    pub fn aether_host_copy_project_json(
        host: AetherHostHandle,
        out_buf: *mut u8,
        cap: usize,
        out_len: *mut usize,
    ) -> AetherStatus;

    /// Populates live compiled node graph stats & per-node values for UI inspection (ADR-0014). `[ui]`
    pub fn aether_host_get_plan_stats(
        host: AetherHostHandle,
        out_stats: *mut AetherGraphPlanStats,
    ) -> AetherStatus;

    /// Populates 64-byte aligned per-node nanosecond timings and output pin values for UI watches (`§7.2`). `[ui]`
    pub fn aether_host_get_telemetry_readout(
        host: AetherHostHandle,
        out_readout: *mut AetherGraphTelemetryReadout,
    ) -> AetherStatus;

    /// Returns pointer and byte length to `layout_table.json` embedded at build time (`REQ-FFI-005`). `[any]`
    pub fn aether_ffi_layout_table_json(out_len: *mut usize) -> *const u8;
}
