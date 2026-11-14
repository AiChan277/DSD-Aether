#![deny(clippy::pedantic)]
#![allow(clippy::missing_safety_doc)]
#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_ptr_alignment)]

//! Single `staticlib` C ABI host facade for AETHER-2.5D (`AETHER-TS-001` §5, `REQ-CORE-001`).
//!
//! Exports every frozen C ABI function declared in `crates/aether-ffi/src/lib.rs`, wraps
//! every FFI entry point in `std::panic::catch_unwind` (`HR-04`), maintains allocation-free
//! thread-local last-error diagnostics, and checks debug thread-role affinity (`REQ-FFI-003`).

use core::cell::{Cell, RefCell};
use core::ffi::{CStr, c_char};
use core::mem::size_of;
use core::ptr;
use core::slice;
use core::sync::atomic::{AtomicBool, Ordering};
use std::borrow::Cow;
use std::os::windows::ffi::OsStrExt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use aether_asset_loader::ffi_slice::{borrow_arena_slices, borrow_utf8_cstr};
use aether_asset_loader::{AssetLoader, LoaderError};
use aether_ffi::{
    AETHER_E_INTERNAL, AETHER_E_INVALID_ARG, AETHER_E_PANIC, AETHER_E_UNSUPPORTED,
    AETHER_FFI_VERSION_MAJOR, AETHER_FFI_VERSION_MINOR, AETHER_OK, AETHER_THREAD_ROLE_ANY,
    AETHER_THREAD_ROLE_LOADER, AETHER_THREAD_ROLE_MAIN, AETHER_THREAD_ROLE_PLUGIN,
    AETHER_THREAD_ROLE_T2, AETHER_THREAD_ROLE_UI, AetherArenaSlice, AetherAssetRequirements,
    AetherAvatarDesc, AetherGraphInputs, AetherGraphOutputs, AetherGraphPlanStats,
    AetherGraphTelemetryReadout, AetherHostCommand, AetherHostConfig, AetherHostEvent,
    AetherHostHandle, AetherLoadRequest,
    AetherLoaderHandle, AetherPlanHandle, AetherRuntimeConfig, AetherStatus, AetherTrackingFrame,
    LAYOUT_TABLE_JSON,
};
use aether_plugin_host::{CompiledGraphPlan, HostError, PluginHost, SafeGraphInputs};

const ERR_CAP: usize = 512;

struct TlsErrorBuf {
    bytes: [u8; ERR_CAP],
    len: usize,
}

impl TlsErrorBuf {
    const fn new() -> Self {
        Self {
            bytes: [0; ERR_CAP],
            len: 0,
        }
    }

    fn clear(&mut self) {
        self.len = 0;
        self.bytes[0] = 0;
    }

    fn set_str(&mut self, msg: &str) {
        let src = msg.as_bytes();
        let copy_len = src.len().min(ERR_CAP - 1);
        self.bytes[..copy_len].copy_from_slice(&src[..copy_len]);
        self.bytes[copy_len] = 0;
        self.len = copy_len;
    }
}

thread_local! {
    static THREAD_ROLE: Cell<u32> = const { Cell::new(AETHER_THREAD_ROLE_ANY) };
    static LAST_ERROR: RefCell<TlsErrorBuf> = const { RefCell::new(TlsErrorBuf::new()) };
}

static RUNTIME_INITIALIZED: AtomicBool = AtomicBool::new(false);

fn win32_move_file_replace_write_through(tmp_path: &Path, dst_path: &Path) -> std::io::Result<()> {
    const MOVEFILE_REPLACE_EXISTING: u32 = 0x0000_0001;
    const MOVEFILE_WRITE_THROUGH: u32 = 0x0000_0008;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn MoveFileExW(
            lp_existing_file_name: *const u16,
            lp_new_file_name: *const u16,
            dw_flags: u32,
        ) -> i32;
    }

    let mut src_w: Vec<u16> = tmp_path.as_os_str().encode_wide().collect();
    src_w.push(0);
    let mut dst_w: Vec<u16> = dst_path.as_os_str().encode_wide().collect();
    dst_w.push(0);

    // SAFETY: `src_w` and `dst_w` are NUL-terminated UTF-16 buffers alive for the duration of the Win32 call.
    let ok = unsafe {
        MoveFileExW(
            src_w.as_ptr(),
            dst_w.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

fn clear_last_error() {
    let _ = LAST_ERROR.try_with(|cell| {
        if let Ok(mut buf) = cell.try_borrow_mut() {
            buf.clear();
        }
    });
}

fn set_last_error(msg: &str) {
    let _ = LAST_ERROR.try_with(|cell| {
        if let Ok(mut buf) = cell.try_borrow_mut() {
            buf.set_str(msg);
        }
    });
}

fn format_panic_payload(payload: &(dyn core::any::Any + Send)) -> &'static str {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        s
    } else if payload.downcast_ref::<String>().is_some() {
        "rust panic with dynamic string payload at FFI boundary"
    } else {
        "rust panic at FFI boundary"
    }
}

fn check_thread_role(expected_role: u32) -> Result<(), AetherStatus> {
    if !cfg!(debug_assertions) || expected_role == AETHER_THREAD_ROLE_ANY {
        return Ok(());
    }
    let actual = THREAD_ROLE
        .try_with(Cell::get)
        .unwrap_or(AETHER_THREAD_ROLE_ANY);
    if actual != AETHER_THREAD_ROLE_ANY && actual != expected_role {
        set_last_error("thread-role affinity violation (REQ-FFI-003)");
        return Err(AETHER_E_INTERNAL);
    }
    Ok(())
}

fn ffi_guard_cow<F>(expected_role: u32, body: F) -> AetherStatus
where
    F: FnOnce() -> Result<(), (AetherStatus, Cow<'static, str>)>,
{
    clear_last_error();
    if let Err(code) = check_thread_role(expected_role) {
        return code;
    }
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(())) => AETHER_OK,
        Ok(Err((code, msg))) => {
            set_last_error(msg.as_ref());
            code
        }
        Err(payload) => {
            let msg = format_panic_payload(&*payload);
            set_last_error(msg);
            AETHER_E_PANIC
        }
    }
}

fn ffi_guard<F>(expected_role: u32, body: F) -> AetherStatus
where
    F: FnOnce() -> Result<(), (AetherStatus, &'static str)>,
{
    ffi_guard_cow(expected_role, || {
        body().map_err(|(code, msg)| (code, Cow::Borrowed(msg)))
    })
}

const fn map_loader_error(err: &LoaderError) -> (AetherStatus, &'static str) {
    let msg = match err {
        LoaderError::InvalidArg(m)
        | LoaderError::Io(m)
        | LoaderError::LimitExceeded(m)
        | LoaderError::Parse(m)
        | LoaderError::Unsupported(m) => *m,
        LoaderError::BufferTooSmall { .. } => {
            "provided arena slice smaller than queried requirement"
        }
        LoaderError::Cancelled => "asset load cancelled",
    };
    (err.status_code(), msg)
}

fn map_host_error(err: &HostError) -> (AetherStatus, Cow<'static, str>) {
    (err.status_code(), Cow::Owned(err.to_string()))
}

/// Returns `(AETHER_FFI_VERSION_MAJOR << 16) | AETHER_FFI_VERSION_MINOR`. `[any]`
#[unsafe(no_mangle)]
pub extern "C" fn aether_ffi_version() -> u32 {
    (AETHER_FFI_VERSION_MAJOR << 16) | AETHER_FFI_VERSION_MINOR
}

/// Copies the thread-local last error message into `buf`. `[any]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_last_error(buf: *mut c_char, len: usize) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        LAST_ERROR
            .try_with(|cell| {
                let Ok(err) = cell.try_borrow() else {
                    return 0usize;
                };
                if !buf.is_null() && len > 0 {
                    let copy_len = err.len.min(len - 1);
                    // SAFETY: Caller guarantees `[buf, buf + len)` is writable for this call.
                    unsafe {
                        ptr::copy_nonoverlapping(err.bytes.as_ptr(), buf.cast::<u8>(), copy_len);
                        *buf.add(copy_len) = 0;
                    }
                }
                err.len
            })
            .unwrap_or(0)
    }))
    .unwrap_or_default()
}

/// Sets the calling thread's role tag (`AETHER_THREAD_ROLE_*`) for debug affinity checks. `[any]`
#[unsafe(no_mangle)]
pub extern "C" fn aether_set_thread_role(role: u32) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let clamped = match role {
            AETHER_THREAD_ROLE_MAIN
            | AETHER_THREAD_ROLE_LOADER
            | AETHER_THREAD_ROLE_PLUGIN
            | AETHER_THREAD_ROLE_T2
            | AETHER_THREAD_ROLE_UI => role,
            _ => AETHER_THREAD_ROLE_ANY,
        };
        let _ = THREAD_ROLE.try_with(|cell| cell.set(clamped));
    }));
}

/// Initializes the global Rust runtime. `[main]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_runtime_init(cfg: *const AetherRuntimeConfig) -> AetherStatus {
    ffi_guard(AETHER_THREAD_ROLE_MAIN, || {
        if !cfg.is_null() {
            // SAFETY: Caller guarantees non-null `cfg` points to valid `AetherRuntimeConfig`.
            let cfg_ref = unsafe { &*cfg };
            if cfg_ref.struct_size != size_of::<AetherRuntimeConfig>() as u32 {
                return Err((
                    AETHER_E_INVALID_ARG,
                    "AetherRuntimeConfig struct_size mismatch",
                ));
            }
            if !cfg_ref.app_data_dir_utf8.is_null() {
                // SAFETY: Non-null C string pointer supplied in config for this call.
                let _ = unsafe { CStr::from_ptr(cfg_ref.app_data_dir_utf8) }
                    .to_str()
                    .map_err(|_| (AETHER_E_INVALID_ARG, "invalid UTF-8 app_data_dir_utf8"))?;
            }
            if !cfg_ref.cache_dir_utf8.is_null() {
                // SAFETY: Non-null C string pointer supplied in config for this call.
                let dir_str = unsafe { CStr::from_ptr(cfg_ref.cache_dir_utf8) }
                    .to_str()
                    .map_err(|_| (AETHER_E_INVALID_ARG, "invalid UTF-8 cache_dir_utf8"))?;
                if !dir_str.is_empty() {
                    aether_asset_loader::bc7_cache::set_runtime_cache_dir(std::path::Path::new(
                        dir_str,
                    ));
                }
            }
        }
        RUNTIME_INITIALIZED.store(true, Ordering::Release);
        Ok(())
    })
}

/// Shuts down the global Rust runtime. `[main]`
#[unsafe(no_mangle)]
pub extern "C" fn aether_runtime_shutdown() {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        RUNTIME_INITIALIZED.store(false, Ordering::Release);
    }));
}

/// Opens and parses metadata for an avatar model file (zero-copy phase 1). `[loader]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_open(
    req: *const AetherLoadRequest,
    out: *mut AetherLoaderHandle,
) -> AetherStatus {
    ffi_guard(AETHER_THREAD_ROLE_LOADER, || {
        if req.is_null() || out.is_null() {
            return Err((AETHER_E_INVALID_ARG, "null req or out pointer"));
        }
        // SAFETY: Output slot is valid writable pointer verified non-null above.
        unsafe { *out = ptr::null_mut() };
        // SAFETY: `req` is non-null and valid for the duration of this call.
        let req_ref = unsafe { &*req };
        // SAFETY: Validates UTF-8 C string before passing to safe crate.
        let path =
            unsafe { borrow_utf8_cstr(req_ref.path_utf8) }.map_err(|e| map_loader_error(&e))?;

        match AssetLoader::open(path, req_ref) {
            Ok(loader) => {
                let boxed = Box::new(loader);
                // SAFETY: Caller receives exclusive opaque handle freed via `aether_loader_close`.
                unsafe { *out = Box::into_raw(boxed).cast() };
                Ok(())
            }
            Err(err) => Err(map_loader_error(&err)),
        }
    })
}

/// Queries exact byte requirements per `AETHER_STREAM_*` slot. `[loader]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_query(
    loader: AetherLoaderHandle,
    out: *mut AetherAssetRequirements,
) -> AetherStatus {
    ffi_guard(AETHER_THREAD_ROLE_LOADER, || {
        if loader.is_null() || out.is_null() {
            return Err((AETHER_E_INVALID_ARG, "null loader or out pointer"));
        }
        // SAFETY: `loader` was created by `Box::into_raw` and remains exclusively owned until close.
        let loader_ref = unsafe { &*loader.cast::<AssetLoader>() };
        // SAFETY: `out` is valid writable memory provided by C++.
        unsafe { *out = *loader_ref.requirements() };
        Ok(())
    })
}

/// Validates C++-lent arena slices and writes decoded geometry directly into them. `[loader]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_fill(
    loader: AetherLoaderHandle,
    slices: *const AetherArenaSlice,
    count: u32,
    out: *mut AetherAvatarDesc,
) -> AetherStatus {
    ffi_guard(AETHER_THREAD_ROLE_LOADER, || {
        if loader.is_null() || out.is_null() {
            return Err((AETHER_E_INVALID_ARG, "null loader or out pointer"));
        }
        // SAFETY: `loader` handle is valid until `aether_loader_close`.
        let loader_ref = unsafe { &*loader.cast::<AssetLoader>() };
        // SAFETY: Validates non-nullness, alignment, bounds, and pairwise non-aliasing before borrowing.
        let mut arena = unsafe { borrow_arena_slices(slices, count, loader_ref.requirements()) }
            .map_err(|e| map_loader_error(&e))?;
        let desc = loader_ref
            .fill(&mut arena)
            .map_err(|e| map_loader_error(&e))?;
        // SAFETY: `out` is valid writable memory provided by C++.
        unsafe { *out = desc };
        Ok(())
    })
}

/// Returns load progress in `[0.0, 1.0]`. `[any]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_progress(loader: AetherLoaderHandle) -> f32 {
    if loader.is_null() {
        return 0.0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Caller guarantees `loader` handle is live while polling progress.
        let loader_ref = unsafe { &*loader.cast::<AssetLoader>() };
        loader_ref.progress()
    }))
    .unwrap_or(0.0)
}

/// Requests cooperative cancellation of an active load job. `[any]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_cancel(loader: AetherLoaderHandle) {
    if loader.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Caller guarantees `loader` handle is live; cancellation is atomic.
        let loader_ref = unsafe { &*loader.cast::<AssetLoader>() };
        loader_ref.cancel();
    }));
}

/// Closes a loader handle and releases Rust-side state. `[loader]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_loader_close(loader: AetherLoaderHandle) {
    if loader.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if check_thread_role(AETHER_THREAD_ROLE_LOADER).is_ok() {
            // SAFETY: Ownership transfers back from C++ handle; handle must not be reused.
            let boxed: Box<AssetLoader> = unsafe { Box::from_raw(loader.cast()) };
            drop(boxed);
        }
    }));
}

/// Creates the Rust plugin host and node graph engine (§7). `[main]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_create(
    cfg: *const AetherHostConfig,
    out: *mut AetherHostHandle,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_MAIN, || {
        if cfg.is_null() || out.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null cfg or out pointer"),
            ));
        }
        // SAFETY: Output slot is valid writable pointer verified non-null above.
        unsafe { *out = ptr::null_mut() };
        // SAFETY: `cfg` is non-null valid pointer for the call duration.
        let cfg_ref = unsafe { &*cfg };
        let plugins_dir = if cfg_ref.plugins_dir_utf8.is_null() {
            None
        } else {
            // SAFETY: Caller guarantees NUL-terminated UTF-8 `plugins_dir_utf8` string for this call.
            let dir_str = unsafe { CStr::from_ptr(cfg_ref.plugins_dir_utf8) }
                .to_str()
                .map_err(|_| {
                    (
                        AETHER_E_INVALID_ARG,
                        Cow::Borrowed("plugins_dir_utf8 is not valid UTF-8"),
                    )
                })?;
            (!dir_str.is_empty()).then(|| PathBuf::from(dir_str))
        };
        match PluginHost::create_with_paths(
            cfg_ref,
            plugins_dir,
            win32_move_file_replace_write_through,
        ) {
            Ok(host) => {
                // SAFETY: Caller assumes ownership until `aether_host_destroy`.
                unsafe { *out = Box::into_raw(Box::new(host)).cast() };
                Ok(())
            }
            Err(err) => Err(map_host_error(&err)),
        }
    })
}

/// Starts asynchronous `*.aether` project loading (§7.7). `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_load_project(
    host: AetherHostHandle,
    path_utf8: *const c_char,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if host.is_null() || path_utf8.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null host or path pointer"),
            ));
        }
        // SAFETY: Caller guarantees `path_utf8` is valid C string for this call.
        let path = unsafe { CStr::from_ptr(path_utf8) }.to_str().map_err(|_| {
            (
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("project path is not valid UTF-8"),
            )
        })?;
        // SAFETY: Caller guarantees live `host` handle.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        host_ref.load_project(path).map_err(|e| map_host_error(&e))
    })
}

/// Starts asynchronous atomic `*.aether` project saving (§7.7). `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_save_project(
    host: AetherHostHandle,
    path_utf8: *const c_char,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if host.is_null() || path_utf8.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null host or path pointer"),
            ));
        }
        // SAFETY: Caller guarantees `path_utf8` is valid C string for this call.
        let path = unsafe { CStr::from_ptr(path_utf8) }.to_str().map_err(|_| {
            (
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("project path is not valid UTF-8"),
            )
        })?;
        // SAFETY: Caller guarantees live `host` handle.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        host_ref.save_project(path).map_err(|e| map_host_error(&e))
    })
}

/// Enqueues a structural UI command to the plugin host actor. `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_post_command(
    host: AetherHostHandle,
    cmd: *const AetherHostCommand,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if host.is_null() || cmd.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null host or cmd pointer"),
            ));
        }
        // SAFETY: Valid `host` handle and `cmd` pointer guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        // SAFETY: `cmd` checked non-null above.
        let cmd_ref = unsafe { &*cmd };
        if usize::try_from(cmd_ref.struct_size) != Ok(size_of::<AetherHostCommand>()) {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("AetherHostCommand struct_size mismatch"),
            ));
        }
        let payload = if cmd_ref.payload_utf8.is_null() {
            ""
        } else if cmd_ref.payload_len > 0 {
            let len = usize::try_from(cmd_ref.payload_len)
                .map_err(|_| (AETHER_E_INVALID_ARG, Cow::Borrowed("payload_len overflow")))?;
            // SAFETY: Caller guarantees `[payload_utf8, payload_utf8 + len)` readable UTF-8 for this call.
            let bytes = unsafe { slice::from_raw_parts(cmd_ref.payload_utf8.cast::<u8>(), len) };
            core::str::from_utf8(bytes).map_err(|_| {
                (
                    AETHER_E_INVALID_ARG,
                    Cow::Borrowed("payload_utf8 is not valid UTF-8"),
                )
            })?
        } else {
            // SAFETY: Caller guarantees NUL-terminated `payload_utf8` C string when `payload_len == 0`.
            unsafe { CStr::from_ptr(cmd_ref.payload_utf8) }
                .to_str()
                .map_err(|_| {
                    (
                        AETHER_E_INVALID_ARG,
                        Cow::Borrowed("payload_utf8 is not valid UTF-8"),
                    )
                })?
        };
        host_ref
            .post_command_with_str(cmd_ref, payload)
            .map_err(|e| map_host_error(&e))
    })
}

/// Drains pending plugin host events without blocking. `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_poll_events(
    host: AetherHostHandle,
    out: *mut AetherHostEvent,
    cap: u32,
    count: *mut u32,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if !count.is_null() {
            // SAFETY: Valid writable counter pointer when non-null.
            unsafe { *count = 0 };
        }
        if host.is_null() || count.is_null() || (cap > 0 && out.is_null()) {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("invalid poll_events arguments"),
            ));
        }
        // SAFETY: Valid `host` handle guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        let buf = if cap == 0 {
            &mut []
        } else {
            // SAFETY: Non-null `out` with capacity `cap` elements guaranteed by caller.
            unsafe { slice::from_raw_parts_mut(out, cap as usize) }
        };
        let written = host_ref.poll_events(buf).map_err(|e| map_host_error(&e))?;
        // SAFETY: Valid writable `count` verified non-null above.
        unsafe { *count = written };
        Ok(())
    })
}

/// Compiles a standalone node graph JSON document into an allocation-free evaluation plan. `[plugin]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_graph_compile(
    host: AetherHostHandle,
    json: *const u8,
    len: usize,
    out: *mut AetherPlanHandle,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_PLUGIN, || {
        if !out.is_null() {
            // SAFETY: Valid writable output slot verified non-null above.
            unsafe { *out = ptr::null_mut() };
        }
        if host.is_null() || out.is_null() || (len > 0 && json.is_null()) {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("invalid aether_graph_compile arguments"),
            ));
        }
        // SAFETY: Valid `host` handle guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        let bytes = if len == 0 {
            &[]
        } else {
            // SAFETY: Caller guarantees `[json, json + len)` readable for this call.
            unsafe { slice::from_raw_parts(json, len) }
        };
        match host_ref.compile_graph(bytes) {
            Ok(plan) => {
                // SAFETY: Caller receives ownership freed via `aether_graph_release`.
                unsafe { *out = Box::into_raw(Box::new(plan)).cast() };
                Ok(())
            }
            Err(err) => Err(map_host_error(&err)),
        }
    })
}

unsafe fn borrow_graph_io<'a>(
    inputs: *const AetherGraphInputs,
    outputs: *mut AetherGraphOutputs,
) -> Result<(SafeGraphInputs<'a>, &'a mut AetherGraphOutputs), (AetherStatus, &'static str)> {
    if inputs.is_null() || outputs.is_null() {
        return Err((AETHER_E_INVALID_ARG, "null inputs or outputs pointer"));
    }
    // SAFETY: Caller guarantees valid aligned `inputs` and exclusively writable `outputs`.
    let in_ref = unsafe { &*inputs };
    // SAFETY: Caller guarantees exclusively writable `outputs` for the duration of this call.
    let out_ref = unsafe { &mut *outputs };

    let tracking: Option<&AetherTrackingFrame> = if in_ref.tracking.is_null() {
        None
    } else {
        let addr = in_ref.tracking as usize;
        if addr % core::mem::align_of::<AetherTrackingFrame>() != 0 {
            return Err((AETHER_E_INVALID_ARG, "unaligned tracking pointer"));
        }
        // SAFETY: Non-null, aligned `AetherTrackingFrame` pointer lent by caller for this call.
        Some(unsafe { &*in_ref.tracking })
    };

    let safe_inputs = SafeGraphInputs {
        tracking,
        now_us: in_ref.now_us,
        dt_s: in_ref.dt_s,
        tick_index: in_ref.tick_index,
        dsp_visemes: in_ref.dsp_visemes,
        dsp_rms_db: in_ref.dsp_rms_db,
        keys_down: in_ref.keys_down,
        keys_pressed: in_ref.keys_pressed,
        external_params: in_ref.external_params,
    };
    Ok((safe_inputs, out_ref))
}

/// Evaluates a compiled graph plan deterministically with zero allocation (`HR-01`). `[T2]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_graph_evaluate(
    plan: AetherPlanHandle,
    inputs: *const AetherGraphInputs,
    outputs: *mut AetherGraphOutputs,
) -> AetherStatus {
    ffi_guard(AETHER_THREAD_ROLE_T2, || {
        if plan.is_null() {
            return Err((AETHER_E_INVALID_ARG, "null plan pointer"));
        }
        // SAFETY: Caller guarantees `plan` handle is valid and exclusively evaluated on T2 for this call.
        let plan_ref = unsafe { &mut *plan.cast::<CompiledGraphPlan>() };
        // SAFETY: Caller guarantees `inputs` and `outputs` validity for this call.
        let (safe_inputs, out_ref) = unsafe { borrow_graph_io(inputs, outputs)? };
        plan_ref.evaluate(&safe_inputs, out_ref);
        Ok(())
    })
}

/// Releases a compiled graph plan. `[any]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_graph_release(plan: AetherPlanHandle) {
    if plan.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Ownership transfers back from C++ handle; handle must not be reused.
        let boxed: Box<CompiledGraphPlan> = unsafe { Box::from_raw(plan.cast()) };
        drop(boxed);
    }));
}

/// Wait-free, allocation-free evaluation of the host's currently published graph plan. `[T2]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_evaluate_active(
    host: AetherHostHandle,
    inputs: *const AetherGraphInputs,
    outputs: *mut AetherGraphOutputs,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_T2, || {
        if host.is_null() {
            return Err((AETHER_E_INVALID_ARG, Cow::Borrowed("null host pointer")));
        }
        // SAFETY: Valid `host` handle guaranteed by caller for this call.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        // SAFETY: Caller guarantees `inputs` and `outputs` validity for this call.
        let (safe_inputs, out_ref) =
            unsafe { borrow_graph_io(inputs, outputs) }.map_err(|(c, m)| (c, Cow::Borrowed(m)))?;
        host_ref
            .evaluate_active(&safe_inputs, out_ref)
            .map_err(|e| map_host_error(&e))
    })
}

/// Copies normalized project/runtime JSON snapshot into `out_buf`. `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_copy_project_json(
    host: AetherHostHandle,
    out_buf: *mut u8,
    cap: usize,
    out_len: *mut usize,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if !out_len.is_null() {
            // SAFETY: Valid writable counter pointer verified non-null above.
            unsafe { *out_len = 0 };
        }
        if host.is_null() || out_len.is_null() || (cap > 0 && out_buf.is_null()) {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("invalid aether_host_copy_project_json arguments"),
            ));
        }
        // SAFETY: Valid `host` handle guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        let dst = if cap == 0 {
            &mut []
        } else {
            // SAFETY: Non-null `out_buf` with `cap` writable bytes guaranteed by caller.
            unsafe { slice::from_raw_parts_mut(out_buf, cap) }
        };
        match host_ref.copy_project_json(dst) {
            Ok(written) => {
                // SAFETY: Valid writable `out_len` verified non-null above.
                unsafe { *out_len = written };
                Ok(())
            }
            Err(HostError::BufferTooSmall { needed }) => {
                // SAFETY: Valid writable `out_len` verified non-null above.
                unsafe { *out_len = needed };
                Err(map_host_error(&HostError::BufferTooSmall { needed }))
            }
            Err(err) => Err(map_host_error(&err)),
        }
    })
}

/// Populates live compiled node graph stats and per-node live values for UI inspection. `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_get_plan_stats(
    host: AetherHostHandle,
    out_stats: *mut AetherGraphPlanStats,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if host.is_null() || out_stats.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null host or out_stats pointer"),
            ));
        }
        // SAFETY: Valid `host` handle and exclusively writable `out_stats` guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        // SAFETY: `out_stats` verified non-null above.
        let stats_ref = unsafe { &mut *out_stats };
        host_ref
            .get_plan_stats(stats_ref)
            .map_err(|e| map_host_error(&e))
    })
}

/// Populates 64-byte aligned per-node nanosecond timings and output pin readout values for UI watches. `[ui]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_get_telemetry_readout(
    host: AetherHostHandle,
    out_readout: *mut AetherGraphTelemetryReadout,
) -> AetherStatus {
    ffi_guard_cow(AETHER_THREAD_ROLE_UI, || {
        if host.is_null() || out_readout.is_null() {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("null host or out_readout pointer"),
            ));
        }
        // SAFETY: Valid `host` handle and writable `out_readout` pointer guaranteed by caller.
        let host_ref = unsafe { &*host.cast::<PluginHost>() };
        // SAFETY: `out_readout` verified non-null above.
        let readout_ref = unsafe { &mut *out_readout };
        let expected_size = size_of::<AetherGraphTelemetryReadout>() as u32;
        if readout_ref.struct_size != 0 && readout_ref.struct_size != expected_size {
            return Err((
                AETHER_E_INVALID_ARG,
                Cow::Borrowed("unexpected AetherGraphTelemetryReadout struct_size"),
            ));
        }
        host_ref
            .get_telemetry_readout(readout_ref)
            .map_err(|e| map_host_error(&e))
    })
}

/// Destroys the Rust plugin host. `[main]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_host_destroy(host: AetherHostHandle) {
    if host.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if check_thread_role(AETHER_THREAD_ROLE_MAIN).is_ok() {
            // SAFETY: Ownership transfers back from C++ handle; handle must not be reused.
            let boxed: Box<PluginHost> = unsafe { Box::from_raw(host.cast()) };
            drop(boxed);
        }
    }));
}

/// Returns pointer and byte length to embedded `layout_table.json` (`REQ-FFI-005`). `[any]`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_ffi_layout_table_json(out_len: *mut usize) -> *const u8 {
    if !out_len.is_null() {
        // SAFETY: Caller guarantees writable `out_len` when non-null.
        unsafe { *out_len = LAYOUT_TABLE_JSON.len() };
    }
    LAYOUT_TABLE_JSON.as_ptr()
}

// Sanity check for unused constant if Phase 1 host stub stays unimplemented.
const _: AetherStatus = AETHER_E_UNSUPPORTED;

// ============================================================================
// Test-only FFI hooks (ADR-0002) — compiled ONLY with `--features test-hooks`.
// ============================================================================
#[cfg(feature = "test-hooks")]
pub use test_hooks::*;

#[cfg(feature = "test-hooks")]
mod test_hooks {
    use core::alloc::{GlobalAlloc, Layout};
    use core::ptr;
    use core::sync::atomic::{AtomicU64, Ordering};
    use std::alloc::System;
    use std::panic::catch_unwind;

    use aether_asset_loader::AssetLoader;
    use aether_ffi::{
        AETHER_E_INVALID_ARG, AETHER_THREAD_ROLE_ANY, AETHER_THREAD_ROLE_LOADER,
        AETHER_THREAD_ROLE_PLUGIN, AetherLoaderHandle, AetherPlanHandle, AetherStatus,
    };
    use aether_plugin_host::CompiledGraphPlan;

    use super::{ffi_guard, map_loader_error};

    struct CountingAllocator;

    static ALLOC_CALLS: AtomicU64 = AtomicU64::new(0);
    static ALLOC_BYTES_TOTAL: AtomicU64 = AtomicU64::new(0);
    static LIVE_BYTES: AtomicU64 = AtomicU64::new(0);
    static PEAK_BYTES: AtomicU64 = AtomicU64::new(0);

    thread_local! {
        static THREAD_ALLOC_CALLS: core::cell::Cell<u64> = const { core::cell::Cell::new(0) };
        static THREAD_ALLOC_BYTES: core::cell::Cell<u64> = const { core::cell::Cell::new(0) };
    }

    fn record_alloc(size: usize) {
        let size_u64 = size as u64;
        let _ = THREAD_ALLOC_CALLS.try_with(|c| c.set(c.get().saturating_add(1)));
        let _ = THREAD_ALLOC_BYTES.try_with(|c| c.set(c.get().saturating_add(size_u64)));
        ALLOC_CALLS.fetch_add(1, Ordering::Relaxed);
        ALLOC_BYTES_TOTAL.fetch_add(size_u64, Ordering::Relaxed);
        let live = LIVE_BYTES.fetch_add(size_u64, Ordering::Relaxed) + size_u64;
        let mut peak = PEAK_BYTES.load(Ordering::Relaxed);
        while live > peak {
            match PEAK_BYTES.compare_exchange_weak(peak, live, Ordering::Relaxed, Ordering::Relaxed)
            {
                Ok(_) => break,
                Err(cur) => peak = cur,
            }
        }
    }

    fn record_dealloc(size: usize) {
        let size_u64 = size as u64;
        let mut cur = LIVE_BYTES.load(Ordering::Relaxed);
        loop {
            let next = cur.saturating_sub(size_u64);
            match LIVE_BYTES.compare_exchange_weak(cur, next, Ordering::Relaxed, Ordering::Relaxed)
            {
                Ok(_) => break,
                Err(actual) => cur = actual,
            }
        }
    }

    // SAFETY: Delegates actual allocation/deallocation directly to `std::alloc::System`
    // while maintaining lock-free atomic counters.
    unsafe impl GlobalAlloc for CountingAllocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            // SAFETY: Caller meets `GlobalAlloc::alloc` requirements.
            let p = unsafe { System.alloc(layout) };
            if !p.is_null() {
                record_alloc(layout.size());
            }
            p
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            record_dealloc(layout.size());
            // SAFETY: Caller passes pointer & layout originally returned by `System.alloc`.
            unsafe { System.dealloc(ptr, layout) };
        }

        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            // SAFETY: Caller meets `GlobalAlloc::alloc_zeroed` requirements.
            let p = unsafe { System.alloc_zeroed(layout) };
            if !p.is_null() {
                record_alloc(layout.size());
            }
            p
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            // SAFETY: Caller passes valid prior allocation and non-zero `new_size`.
            let p = unsafe { System.realloc(ptr, layout, new_size) };
            if !p.is_null() {
                record_dealloc(layout.size());
                record_alloc(new_size);
            }
            p
        }
    }

    #[global_allocator]
    static GLOBAL_ALLOC: CountingAllocator = CountingAllocator;

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_alloc_reset() {
        let _ = THREAD_ALLOC_CALLS.try_with(|c| c.set(0));
        let _ = THREAD_ALLOC_BYTES.try_with(|c| c.set(0));
        ALLOC_CALLS.store(0, Ordering::Release);
        ALLOC_BYTES_TOTAL.store(0, Ordering::Release);
        PEAK_BYTES.store(LIVE_BYTES.load(Ordering::Acquire), Ordering::Release);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_thread_alloc_calls() -> u64 {
        THREAD_ALLOC_CALLS
            .try_with(core::cell::Cell::get)
            .unwrap_or(0)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_thread_alloc_bytes() -> u64 {
        THREAD_ALLOC_BYTES
            .try_with(core::cell::Cell::get)
            .unwrap_or(0)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_alloc_calls() -> u64 {
        ALLOC_CALLS.load(Ordering::Acquire)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_alloc_total_bytes() -> u64 {
        ALLOC_BYTES_TOTAL.load(Ordering::Acquire)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_alloc_peak_bytes() -> u64 {
        PEAK_BYTES.load(Ordering::Acquire)
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_alloc_live_bytes() -> u64 {
        LIVE_BYTES.load(Ordering::Acquire)
    }

    /// Deliberately triggers a Rust panic inside `ffi_guard` to verify `AETHER_E_PANIC` (`HR-04`).
    #[allow(clippy::panic, clippy::missing_panics_doc)]
    #[unsafe(no_mangle)]
    pub extern "C" fn aether_test_inject_panic() -> AetherStatus {
        ffi_guard(AETHER_THREAD_ROLE_ANY, || {
            panic!("aether_test_inject_panic intentional boundary panic");
        })
    }

    /// Creates a synthetic two-phase zero-copy loader handle strictly for test verification.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn aether_test_loader_open_synthetic(
        vertex_count: u32,
        index_count: u32,
        out: *mut AetherLoaderHandle,
    ) -> AetherStatus {
        ffi_guard(AETHER_THREAD_ROLE_LOADER, || {
            if out.is_null() {
                return Err((AETHER_E_INVALID_ARG, "null out pointer"));
            }
            // SAFETY: `out` checked non-null above.
            unsafe { *out = ptr::null_mut() };
            let loader = AssetLoader::open_synthetic_for_test(vertex_count, index_count)
                .map_err(|e| map_loader_error(&e))?;
            // SAFETY: Ownership transfers to caller until `aether_loader_close`.
            unsafe { *out = Box::into_raw(Box::new(loader)).cast() };
            Ok(())
        })
    }

    /// Creates a deterministic no-op graph plan strictly for hot-path zero-alloc testing.
    #[unsafe(no_mangle)]
    pub unsafe extern "C" fn aether_test_graph_noop_plan(
        out: *mut AetherPlanHandle,
    ) -> AetherStatus {
        ffi_guard(AETHER_THREAD_ROLE_PLUGIN, || {
            if out.is_null() {
                return Err((AETHER_E_INVALID_ARG, "null out pointer"));
            }
            let plan = CompiledGraphPlan::new_test_noop();
            // SAFETY: Ownership transfers to caller until `aether_graph_release`.
            unsafe { *out = Box::into_raw(Box::new(plan)).cast() };
            Ok(())
        })
    }

    // Silence unused import warning for `catch_unwind` if not needed directly in module.
    const _: fn(fn()) -> std::thread::Result<()> = catch_unwind;
}
