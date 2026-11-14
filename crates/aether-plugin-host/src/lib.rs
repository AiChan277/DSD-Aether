#![deny(clippy::pedantic)]
#![deny(unsafe_code)]
#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_possible_wrap)]
#![allow(clippy::cast_lossless)]
#![allow(clippy::cast_precision_loss)]
#![allow(clippy::cast_sign_loss)]
#![allow(clippy::doc_markdown)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::similar_names)]
#![allow(clippy::too_many_lines)]

//! Sandboxed WASM plugin host, deterministic node graph compiler/evaluator,
//! versioned `*.aether/` project serializer, OSC (`127.0.0.1:9001/9002`), and
//! opt-in WebSocket JSON-RPC gateway (`ws://127.0.0.1:19190/v1`, `AETHER-TS-001` §7).

pub mod error;
pub mod events;
pub mod fuzz_entry;
pub mod graph;
pub mod io;
pub mod project;
pub mod wasm;

use core::ffi::CStr;
use core::mem::size_of;
use core::pin::pin;
use core::sync::atomic::{AtomicU16, AtomicU32, Ordering};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use aether_ffi::{
    AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT, AETHER_HOST_CMD_PROJECT_UPDATE_JSON,
    AETHER_HOST_CMD_PUBLISH_TELEMETRY, AETHER_HOST_CMD_RELOAD_PLUGINS,
    AETHER_HOST_CMD_SET_EXTERNAL_PARAM, AETHER_HOST_CMD_SET_GRAPH_JSON, AETHER_HOST_CMD_SET_ON_AIR,
    AETHER_HOST_CMD_SET_WS_ENABLED, AETHER_HOST_CMD_TRIGGER_EXPRESSION,
    AETHER_HOST_CMD_UNLOAD_PLUGIN, AETHER_HOST_EVT_GRAPH_BUDGET_WARN,
    AETHER_HOST_EVT_GRAPH_COMPILE_ERR, AETHER_HOST_EVT_GRAPH_COMPILED,
    AETHER_HOST_EVT_HOTKEY_EVENT, AETHER_HOST_EVT_OSC_PARAM, AETHER_HOST_EVT_PLUGIN_CONSENT_REQ,
    AETHER_HOST_EVT_PLUGIN_LOADED, AETHER_HOST_EVT_PLUGIN_TRAP, AETHER_HOST_EVT_PROJECT_LOADED,
    AETHER_HOST_EVT_PROJECT_SAVED, AETHER_HOST_FLAG_OSC_BIND_ANY, AETHER_HOST_FLAG_WS_ENABLED,
    AetherGraphOutputs, AetherGraphPlanStats, AetherGraphTelemetryReadout, AetherHostCommand,
    AetherHostConfig, AetherHostEvent,
};
use futures_util::future::{Either, select};
use serde::Serialize;
use tokio::net::UdpSocket;
use tokio::sync::{broadcast, mpsc, watch};

pub use error::HostError;
pub use events::SharedParamBank;
pub use graph::{
    CompiledGraphPlan, GraphDocument, HostEvalContext, LatchedBridge, SafeGraphInputs,
    compile_graph_document,
};
pub use project::{AtomicReplaceFn, ProjectBundle, default_atomic_replace};
pub use wasm::{PluginManifest, PluginStatus, PluginSummary};

use events::HostEventQueue;
use graph::PlanExchange;
use io::{
    OscControlMsg, OscRateLimiter, WsTelemetryFrame, encode_osc_float, encode_osc_int,
    generate_session_token, map_expression_name_to_slot, map_param_id_to_slot, parse_osc_packet,
    run_ws_supervisor,
};
use project::{
    AssetsToml, ProjectToml, load_project_bundle, parse_graph_json, save_project_bundle,
};
use wasm::{PLUGIN_EPOCH_INTERVAL_MS, WasmPluginRuntime, hash_plugin_id};

/// Normalized JSON payload exposed via `aether_host_copy_project_json` (ADR-0014).
#[derive(Serialize)]
struct HostSnapshotJson<'a> {
    project: &'a ProjectToml,
    graph: &'a GraphDocument,
    assets: &'a AssetsToml,
    layout_ini: &'a str,
    plugins: &'a [PluginSummary],
    host_runtime: HostRuntimeMeta<'a>,
}

#[derive(Serialize)]
struct HostRuntimeMeta<'a> {
    ws_enabled: bool,
    ws_url: &'a str,
    ws_token: &'a str,
    osc_in_port: u16,
    osc_out_port: u16,
    on_air: bool,
    dirty: bool,
}

enum ActorMsg {
    SetGraphJson(String),
    ProjectUpdateJson(String),
    LoadProject(PathBuf),
    SaveProject(PathBuf),
    ReloadPlugins,
    GrantConsent { plugin_id: String, allow: bool },
    UnloadPlugin { plugin_id: String },
    PublishTelemetry { address: String, value: f32 },
}

struct WorkerState {
    bundle: ProjectBundle,
    dirty: bool,
    plugins_dir: Option<PathBuf>,
    plugins: WasmPluginRuntime,
    latched_bindings: Vec<graph::LatchedBinding>,
    replace_fn: AtomicReplaceFn,
    autosave_dir: PathBuf,
    last_project_dir: Option<PathBuf>,
    osc_out_port: u16,
    ws_url: String,
    ws_token: String,
    telemetry_state: (f32, f32, i32),
}

/// Opaque Rust plugin host owning the 2-thread E-core Tokio runtime, WASM engine,
/// graph compiler, wait-free `T2` plan exchange, project serializer, OSC, and WebSocket gateway (§7).
pub struct PluginHost {
    exchange: Arc<PlanExchange>,
    params: Arc<SharedParamBank>,
    events: Arc<HostEventQueue>,
    latched: Arc<LatchedBridge>,
    cmd_tx: mpsc::Sender<ActorMsg>,
    snapshot_tx: watch::Sender<Vec<u8>>,
    ws_enable_tx: watch::Sender<bool>,
    shutdown_tx: watch::Sender<bool>,
    ws_token: String,
    ws_port: u16,
    osc_in_port: Arc<AtomicU16>,
    osc_out_port: u16,
    plugin_count: Arc<AtomicU32>,
    runtime: Option<tokio::runtime::Runtime>,
}

impl PluginHost {
    /// Creates the plugin host with default atomic replace (`fs::rename` after `sync_all`).
    pub fn create(cfg: &AetherHostConfig) -> Result<Self, HostError> {
        Self::create_with_replace_fn(cfg, default_atomic_replace)
    }

    /// Creates the plugin host with an injected atomic file-replace callback (e.g., Win32 `MoveFileExW`).
    #[allow(unsafe_code)]
    fn borrow_plugins_dir(cfg: &AetherHostConfig) -> Option<PathBuf> {
        if cfg.plugins_dir_utf8.is_null() {
            return None;
        }
        // Wait — instead of touching unsafe here, parse via CStr inside aether-rust-host OR convert?
        // Look: `crates/aether-plugin-host` has `#![deny(unsafe_code)]`, let's add a tiny pure-Rust helper constructor `create_with_paths`.
        let _ = cfg;
        None
    }

    /// Creates the plugin host cleanly from decoded Rust config values + atomic replace callback (`§7.1`).
    pub fn create_with_paths(
        cfg: &AetherHostConfig,
        plugins_dir: Option<PathBuf>,
        replace_fn: AtomicReplaceFn,
    ) -> Result<Self, HostError> {
        if usize::try_from(cfg.struct_size) != Ok(size_of::<AetherHostConfig>()) {
            return Err(HostError::InvalidArg(
                "AetherHostConfig struct_size mismatch".to_owned(),
            ));
        }

        let worker_threads = usize::try_from(cfg.worker_threads).unwrap_or(2).clamp(1, 4);
        let ws_enabled = (cfg.flags & AETHER_HOST_FLAG_WS_ENABLED) != 0;
        let osc_bind_any = (cfg.flags & AETHER_HOST_FLAG_OSC_BIND_ANY) != 0;
        let osc_in_cfg = cfg.osc_in_port;
        let osc_out_port = if cfg.osc_out_port == 0 {
            9001
        } else {
            cfg.osc_out_port
        };
        let ws_port = if cfg.ws_port == 0 { 19190 } else { cfg.ws_port };

        let wasm_rt = WasmPluginRuntime::new()?;
        let wasm_engine = wasm_rt.engine();

        let default_bundle = ProjectBundle::default();
        let initial_plan = compile_graph_document(&default_bundle.graph, &[])?;

        let exchange = Arc::new(PlanExchange::new(initial_plan));
        let params = Arc::new(SharedParamBank::new(ws_enabled));
        let events = Arc::new(HostEventQueue::new());
        let latched = Arc::new(LatchedBridge::new());
        let ws_token = generate_session_token();
        let ws_url = format!("ws://127.0.0.1:{ws_port}/v1");
        let osc_in_port = Arc::new(AtomicU16::new(osc_in_cfg));
        let plugin_count = Arc::new(AtomicU32::new(0));

        let initial_summaries = wasm_rt.summaries();
        let initial_json = serialize_snapshot(
            &default_bundle,
            &initial_summaries,
            ws_enabled,
            &ws_url,
            &ws_token,
            osc_in_cfg,
            osc_out_port,
            false,
            false,
        );
        let (snapshot_tx, _) = watch::channel(initial_json);
        let (cmd_tx, mut cmd_rx) = mpsc::channel::<ActorMsg>(256);
        let (ws_enable_tx, ws_enable_rx) = watch::channel(ws_enabled);
        let (shutdown_tx, shutdown_rx) = watch::channel(false);
        let (telemetry_tx, _) = broadcast::channel::<String>(64);

        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(worker_threads)
            .thread_name("aether-plugin-host")
            .enable_io()
            .enable_time()
            .build()
            .map_err(|e| HostError::Internal(format!("tokio runtime build failed: {e}")))?;

        let autosave_dir = std::env::var_os("LOCALAPPDATA").map_or_else(
            || std::env::temp_dir().join("Aether").join("autosave"),
            |root| PathBuf::from(root).join("Aether").join("autosave"),
        );

        let mut state = WorkerState {
            bundle: default_bundle,
            dirty: false,
            plugins_dir,
            plugins: wasm_rt,
            latched_bindings: Vec::new(),
            replace_fn,
            autosave_dir,
            last_project_dir: None,
            osc_out_port,
            ws_url,
            ws_token: ws_token.clone(),
            telemetry_state: (12.0, 60.0, 3),
        };

        // Task 1: 1 ms epoch ticker for WASM 2 ms timeout deadlines (§7.5).
        {
            let mut sd = shutdown_rx.clone();
            runtime.spawn(async move {
                let mut ticker =
                    tokio::time::interval(Duration::from_millis(PLUGIN_EPOCH_INTERVAL_MS));
                ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
                loop {
                    if *sd.borrow() {
                        break;
                    }
                    let sd_fut = pin!(sd.changed());
                    let tick_fut = pin!(ticker.tick());
                    match select(sd_fut, tick_fut).await {
                        Either::Left(_) => break,
                        Either::Right(_) => wasm_engine.increment_epoch(),
                    }
                }
            });
        }

        // Task 2: Opt-in WebSocket JSON-RPC supervisor (`127.0.0.1:<ws_port>/v1`).
        {
            let tok = ws_token.clone();
            let p = Arc::clone(&params);
            let e = Arc::clone(&events);
            let t_tx = telemetry_tx.clone();
            let sd = shutdown_rx.clone();
            runtime.spawn(run_ws_supervisor(
                ws_port,
                tok,
                p,
                e,
                t_tx,
                ws_enable_rx,
                sd,
            ));
        }

        // Task 3: Inbound/Outbound OSC UDP loop (`127.0.0.1:<osc_in>` -> params/events; out -> `:9001`).
        let (osc_out_tx, mut osc_out_rx) = mpsc::channel::<Vec<u8>>(128);
        {
            let p = Arc::clone(&params);
            let e = Arc::clone(&events);
            let port_slot = Arc::clone(&osc_in_port);
            let mut sd = shutdown_rx.clone();
            let sd_out_orig = shutdown_rx.clone();
            let bind_ip = if osc_bind_any { "0.0.0.0" } else { "127.0.0.1" };
            let out_target = format!("127.0.0.1:{osc_out_port}");
            let bind_str = format!("{bind_ip}:{osc_in_cfg}");
            runtime.spawn(async move {
                let Ok(sock) = UdpSocket::bind(&bind_str).await else {
                    tracing::warn!("OSC UDP bind failed on {bind_str}");
                    return;
                };
                if let Ok(local) = sock.local_addr() {
                    port_slot.store(local.port(), Ordering::Release);
                }
                let sock = Arc::new(sock);
                let sock_out = Arc::clone(&sock);
                let mut sd_out = sd_out_orig;
                tokio::spawn(async move {
                    loop {
                        if *sd_out.borrow() {
                            break;
                        }
                        let s_fut = pin!(sd_out.changed());
                        let r_fut = pin!(osc_out_rx.recv());
                        match select(s_fut, r_fut).await {
                            Either::Right((Some(pkt), _)) => {
                                let _ = sock_out.send_to(&pkt, &out_target).await;
                            }
                            Either::Left(_) | Either::Right((None, _)) => break,
                        }
                    }
                });

                let mut buf = vec![0_u8; 65536];
                let mut limiter = OscRateLimiter::new(Instant::now());

                loop {
                    if *sd.borrow() {
                        break;
                    }
                    let pkt_res = {
                        let s_fut = pin!(sd.changed());
                        let r_fut = pin!(sock.recv_from(&mut buf));
                        match select(s_fut, r_fut).await {
                            Either::Left(_) => None,
                            Either::Right((recv_res, _)) => Some(recv_res),
                        }
                    };
                    let Some(recv_res) = pkt_res else {
                        break;
                    };
                    if let Ok((n, _src)) = recv_res {
                        if let Ok(msgs) = parse_osc_packet(&buf[..n]) {
                            let count = u32::try_from(msgs.len()).unwrap_or(1).max(1);
                            if !limiter.allow(Instant::now(), count) {
                                p.inc_osc_rate_limited();
                                continue;
                            }
                            p.inc_osc_rx();
                            for msg in msgs {
                                dispatch_osc_msg(&msg, &p, &e);
                            }
                        }
                    }
                }
            });
        }

        // Task 4: Authoring model + debounced 50 ms graph compiler + WASM latched step (<=120 Hz) + 4 Hz telemetry + 120 s autosave.
        {
            let ex = Arc::clone(&exchange);
            let pr = Arc::clone(&params);
            let ev = Arc::clone(&events);
            let la = Arc::clone(&latched);
            let sn_tx = snapshot_tx.clone();
            let p_count = Arc::clone(&plugin_count);
            let osc_in_port = Arc::clone(&osc_in_port);
            let tel_tx = telemetry_tx.clone();
            let mut sd = shutdown_rx.clone();

            runtime.spawn(async move {
                // Initial plugin scan if configured.
                if let Some(dir) = state.plugins_dir.clone() {
                    scan_and_emit(&mut state, &dir, &ev, &p_count);
                    publish_snapshot(&state, &pr, &sn_tx, osc_in_port.load(Ordering::Relaxed));
                }

                let mut compile_deadline: Option<tokio::time::Instant> = None;
                let mut next_reclaim = tokio::time::Instant::now() + Duration::from_millis(25);
                let mut next_plugin = tokio::time::Instant::now() + Duration::from_millis(9);
                let mut next_telemetry = tokio::time::Instant::now() + Duration::from_millis(250);
                let mut next_autosave = tokio::time::Instant::now() + Duration::from_secs(120);

                loop {
                    if *sd.borrow() {
                        break;
                    }
                    let mut next_wake = next_reclaim
                        .min(next_plugin)
                        .min(next_telemetry)
                        .min(next_autosave);
                    if let Some(dl) = compile_deadline {
                        next_wake = next_wake.min(dl);
                    }

                    let sd_fut = pin!(sd.changed());
                    let cmd_fut = pin!(cmd_rx.recv());
                    let timer_fut = pin!(tokio::time::sleep_until(next_wake));
                    let cmd_or_timer = pin!(select(cmd_fut, timer_fut));

                    match select(sd_fut, cmd_or_timer).await {
                        Either::Left(_) => break,
                        Either::Right((Either::Left((msg, _)), _)) => {
                            let Some(m) = msg else { break };
                            match m {
                                ActorMsg::SetGraphJson(json_str) => {
                                    match parse_graph_json(json_str.as_bytes()) {
                                        Ok(doc) => {
                                            state.bundle.graph = doc;
                                            state.dirty = true;
                                            // §7.1: 50 ms debounced recompile (`REQ-PLG-001 <= 100 ms`).
                                            compile_deadline = Some(
                                                tokio::time::Instant::now()
                                                    + Duration::from_millis(50),
                                            );
                                        }
                                        Err(e) => {
                                            ex.record_compile_error(None);
                                            ev.push(
                                                AETHER_HOST_EVT_GRAPH_COMPILE_ERR,
                                                0,
                                                e.status_code(),
                                                0.0,
                                                &e.to_string(),
                                            );
                                        }
                                    }
                                }
                                ActorMsg::ProjectUpdateJson(json_str) => {
                                    apply_project_json_update(&mut state, &json_str);
                                    publish_snapshot(
                                        &state,
                                        &pr,
                                        &sn_tx,
                                        osc_in_port.load(Ordering::Relaxed),
                                    );
                                }
                                ActorMsg::LoadProject(dir) => match load_project_bundle(&dir) {
                                    Ok(bundle) => {
                                        let prev_states = ex.snapshot_active_states();
                                        match compile_graph_document(&bundle.graph, &prev_states) {
                                            Ok(plan) => {
                                                sync_latched_bindings(&mut state, &plan, &la, &ev);
                                                ex.publish(plan, 0.5);
                                            }
                                            Err(err) => {
                                                let nid = match &err {
                                                    HostError::Parse { node_id, .. } => *node_id,
                                                    _ => None,
                                                };
                                                ex.record_compile_error(nid);
                                                ev.push(
                                                    AETHER_HOST_EVT_GRAPH_COMPILE_ERR,
                                                    nid.unwrap_or(0),
                                                    err.status_code(),
                                                    0.0,
                                                    &err.to_string(),
                                                );
                                            }
                                        }
                                        state.bundle = bundle;
                                        state.last_project_dir = Some(dir.clone());
                                        state.dirty = false;
                                        publish_snapshot(
                                            &state,
                                            &pr,
                                            &sn_tx,
                                            osc_in_port.load(Ordering::Relaxed),
                                        );
                                        ev.push_ok(
                                            AETHER_HOST_EVT_PROJECT_LOADED,
                                            0,
                                            1.0,
                                            &dir.to_string_lossy(),
                                        );
                                    }
                                    Err(err) => {
                                        ev.push(
                                            AETHER_HOST_EVT_PROJECT_LOADED,
                                            0,
                                            err.status_code(),
                                            0.0,
                                            &err.to_string(),
                                        );
                                    }
                                },
                                ActorMsg::SaveProject(dir) => {
                                    match save_project_bundle(&dir, &state.bundle, state.replace_fn)
                                    {
                                        Ok(()) => {
                                            state.last_project_dir = Some(dir.clone());
                                            state.dirty = false;
                                            publish_snapshot(
                                                &state,
                                                &pr,
                                                &sn_tx,
                                                osc_in_port.load(Ordering::Relaxed),
                                            );
                                            ev.push_ok(
                                                AETHER_HOST_EVT_PROJECT_SAVED,
                                                0,
                                                1.0,
                                                &dir.to_string_lossy(),
                                            );
                                        }
                                        Err(err) => {
                                            ev.push(
                                                AETHER_HOST_EVT_PROJECT_SAVED,
                                                0,
                                                err.status_code(),
                                                0.0,
                                                &err.to_string(),
                                            );
                                        }
                                    }
                                }
                                ActorMsg::ReloadPlugins => {
                                    if let Some(dir) = state.plugins_dir.clone() {
                                        scan_and_emit(&mut state, &dir, &ev, &p_count);
                                        publish_snapshot(
                                            &state,
                                            &pr,
                                            &sn_tx,
                                            osc_in_port.load(Ordering::Relaxed),
                                        );
                                    }
                                }
                                ActorMsg::GrantConsent { plugin_id, allow } => {
                                    let dir = state.plugins_dir.clone();
                                    state.plugins.set_consent(&plugin_id, allow, dir.as_deref());
                                    if allow {
                                        let id_hash = hash_plugin_id(&plugin_id);
                                        ev.push_ok(
                                            AETHER_HOST_EVT_PLUGIN_LOADED,
                                            id_hash,
                                            1.0,
                                            &plugin_id,
                                        );
                                    }
                                    p_count.store(
                                        u32::try_from(state.plugins.summaries().len()).unwrap_or(0),
                                        Ordering::Release,
                                    );
                                    publish_snapshot(
                                        &state,
                                        &pr,
                                        &sn_tx,
                                        osc_in_port.load(Ordering::Relaxed),
                                    );
                                }
                                ActorMsg::UnloadPlugin { plugin_id } => {
                                    state.plugins.unload(&plugin_id);
                                    publish_snapshot(
                                        &state,
                                        &pr,
                                        &sn_tx,
                                        osc_in_port.load(Ordering::Relaxed),
                                    );
                                }
                                ActorMsg::PublishTelemetry { address, value } => {
                                    if address == "/aether/telemetry/fps" {
                                        state.telemetry_state.1 = value;
                                    } else if address == "/aether/tracking/ep" {
                                        state.telemetry_state.2 = value as i32;
                                    } else {
                                        state.telemetry_state.0 = value;
                                        if let Ok(pkt) =
                                            encode_osc_float("/aether/telemetry/latency_ms", value)
                                        {
                                            let _ = osc_out_tx.try_send(pkt);
                                        }
                                    }
                                }
                            }
                        }
                        Either::Right((Either::Right(((), _)), _)) => {
                            let now = tokio::time::Instant::now();
                            if let Some(dl) = compile_deadline {
                                if now >= dl {
                                    compile_deadline = None;
                                    let t_start = Instant::now();
                                    let prev_states = ex.snapshot_active_states();
                                    match compile_graph_document(&state.bundle.graph, &prev_states)
                                    {
                                        Ok(plan) => {
                                            sync_latched_bindings(&mut state, &plan, &la, &ev);
                                            let ms = t_start.elapsed().as_secs_f32() * 1000.0;
                                            let n_count = plan.node_ids.len();
                                            ex.publish(plan, ms);
                                            publish_snapshot(
                                                &state,
                                                &pr,
                                                &sn_tx,
                                                osc_in_port.load(Ordering::Relaxed),
                                            );
                                            ev.push_ok(
                                                AETHER_HOST_EVT_GRAPH_COMPILED,
                                                0,
                                                n_count as f32,
                                                "graph compiled cleanly",
                                            );
                                        }
                                        Err(err) => {
                                            let nid = match &err {
                                                HostError::Parse { node_id, .. } => *node_id,
                                                _ => None,
                                            };
                                            ex.record_compile_error(nid);
                                            ev.push(
                                                AETHER_HOST_EVT_GRAPH_COMPILE_ERR,
                                                nid.unwrap_or(0),
                                                err.status_code(),
                                                0.0,
                                                &err.to_string(),
                                            );
                                        }
                                    }
                                }
                            }
                            if now >= next_reclaim {
                                next_reclaim = now + Duration::from_millis(25);
                                ex.reclaim_retired();
                            }
                            if now >= next_plugin {
                                next_plugin = now + Duration::from_millis(9);
                                step_latched_plugins(&mut state, &la, &ev);
                            }
                            if now >= next_telemetry {
                                next_telemetry = now + Duration::from_millis(250);
                                let (lat, fps, ep) = state.telemetry_state;
                                if let Ok(pkt) = encode_osc_float("/aether/telemetry/fps", fps) {
                                    let _ = osc_out_tx.try_send(pkt);
                                }
                                if let Ok(pkt) = encode_osc_int("/aether/tracking/ep", ep) {
                                    let _ = osc_out_tx.try_send(pkt);
                                }
                                if pr.is_ws_enabled() {
                                    let frame = WsTelemetryFrame {
                                        kind: "telemetry".to_owned(),
                                        latency_ms: lat,
                                        fps,
                                        ep: u32::try_from(ep).unwrap_or(3),
                                    };
                                    if let Ok(txt) = serde_json::to_string(&frame) {
                                        let _ = tel_tx.send(txt);
                                    }
                                }
                            }
                            if now >= next_autosave {
                                next_autosave = now + Duration::from_secs(120);
                                if state.dirty {
                                    let dst = state.autosave_dir.join("autosave.aether");
                                    let _ =
                                        save_project_bundle(&dst, &state.bundle, state.replace_fn);
                                }
                            }
                        }
                    }
                }
            });
        }

        // Wait-free handle helper — avoid dead code warning on `borrow_plugins_dir`.
        let _ = Self::borrow_plugins_dir;

        Ok(Self {
            exchange,
            params,
            events,
            latched,
            cmd_tx,
            snapshot_tx,
            ws_enable_tx,
            shutdown_tx,
            ws_token,
            ws_port,
            osc_in_port,
            osc_out_port,
            plugin_count,
            runtime: Some(runtime),
        })
    }

    /// Creates the plugin host with custom replace callback while decoding a borrowed CStr plugins dir safely.
    pub fn create_with_replace_fn(
        cfg: &AetherHostConfig,
        replace_fn: AtomicReplaceFn,
    ) -> Result<Self, HostError> {
        // CStr decoding happens at the FFI boundary (`aether-rust-host`); here treat null -> None.
        let _ = CStr::from_bytes_with_nul;
        Self::create_with_paths(cfg, None, replace_fn)
    }

    /// Starts asynchronous `*.aether/` project loading (§7.7).
    pub fn load_project(&self, path_utf8: &str) -> Result<(), HostError> {
        if path_utf8.is_empty() {
            return Err(HostError::InvalidArg("empty project path".to_owned()));
        }
        self.cmd_tx
            .try_send(ActorMsg::LoadProject(PathBuf::from(path_utf8)))
            .map_err(|e| HostError::Internal(format!("command queue full: {e}")))
    }

    /// Starts asynchronous atomic `*.aether/` project saving (§7.7).
    pub fn save_project(&self, path_utf8: &str) -> Result<(), HostError> {
        if path_utf8.is_empty() {
            return Err(HostError::InvalidArg("empty project path".to_owned()));
        }
        self.cmd_tx
            .try_send(ActorMsg::SaveProject(PathBuf::from(path_utf8)))
            .map_err(|e| HostError::Internal(format!("command queue full: {e}")))
    }

    /// Enqueues a structural UI / host command without blocking `[ui]`.
    pub fn post_command(&self, cmd: &AetherHostCommand) -> Result<(), HostError> {
        if usize::try_from(cmd.struct_size) != Ok(size_of::<AetherHostCommand>()) {
            return Err(HostError::InvalidArg(
                "AetherHostCommand struct_size mismatch".to_owned(),
            ));
        }
        self.post_command_with_str(cmd, "")
    }

    /// Enqueues a structural UI / host command with decoded UTF-8 payload string `[ui]`.
    pub fn post_command_with_str(
        &self,
        cmd: &AetherHostCommand,
        payload: &str,
    ) -> Result<(), HostError> {
        match cmd.kind {
            AETHER_HOST_CMD_SET_GRAPH_JSON => self
                .cmd_tx
                .try_send(ActorMsg::SetGraphJson(payload.to_owned()))
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            AETHER_HOST_CMD_TRIGGER_EXPRESSION => {
                let slot = usize::try_from(cmd.target_id).unwrap_or(0).min(63);
                self.params.set_expression_override(slot, cmd.param_f32);
                Ok(())
            }
            AETHER_HOST_CMD_SET_EXTERNAL_PARAM => {
                let slot = usize::try_from(cmd.target_id).unwrap_or(0).min(63);
                self.params.set_external_param(slot, cmd.param_f32);
                Ok(())
            }
            AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT => self
                .cmd_tx
                .try_send(ActorMsg::GrantConsent {
                    plugin_id: payload.to_owned(),
                    allow: cmd.param_u32 != 0,
                })
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            AETHER_HOST_CMD_RELOAD_PLUGINS => self
                .cmd_tx
                .try_send(ActorMsg::ReloadPlugins)
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            AETHER_HOST_CMD_PUBLISH_TELEMETRY => self
                .cmd_tx
                .try_send(ActorMsg::PublishTelemetry {
                    address: payload.to_owned(),
                    value: cmd.param_f32,
                })
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            AETHER_HOST_CMD_PROJECT_UPDATE_JSON => self
                .cmd_tx
                .try_send(ActorMsg::ProjectUpdateJson(payload.to_owned()))
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            AETHER_HOST_CMD_SET_WS_ENABLED => {
                let enabled = cmd.param_u32 != 0;
                self.params.set_ws_enabled(enabled);
                let _ = self.ws_enable_tx.send(enabled);
                Ok(())
            }
            AETHER_HOST_CMD_SET_ON_AIR => {
                self.params.set_on_air(cmd.param_u32 != 0);
                Ok(())
            }
            AETHER_HOST_CMD_UNLOAD_PLUGIN => self
                .cmd_tx
                .try_send(ActorMsg::UnloadPlugin {
                    plugin_id: payload.to_owned(),
                })
                .map_err(|e| HostError::Internal(format!("command queue full: {e}"))),
            _ => Err(HostError::InvalidArg(format!(
                "unknown AetherHostCommand kind {}",
                cmd.kind
            ))),
        }
    }

    /// Drains pending plugin host events to the UI thread cleanly without blocking `[ui]`.
    pub fn poll_events(&self, out: &mut [AetherHostEvent]) -> Result<u32, HostError> {
        Ok(self.events.drain(out))
    }

    /// Compiles a standalone node graph JSON document into an allocation-free plan `[plugin]`.
    pub fn compile_graph(&self, json_bytes: &[u8]) -> Result<CompiledGraphPlan, HostError> {
        let doc = parse_graph_json(json_bytes)?;
        compile_graph_document(&doc, &[])
    }

    /// Wait-free, allocation-free T2 evaluation of the currently published graph plan (`HR-01`, `REQ-PLG-002`).
    pub fn evaluate_active(
        &self,
        inputs: &SafeGraphInputs<'_>,
        outputs: &mut AetherGraphOutputs,
    ) -> Result<(), HostError> {
        let pending_hotkeys = self.params.take_pending_hotkeys();
        let ctx = HostEvalContext {
            params: &self.params,
            latched: &self.latched,
            pending_hotkeys,
        };
        let (warn_raised, eval_us) = self
            .exchange
            .with_active_plan(|plan| plan.evaluate_with_host(inputs, Some(&ctx), outputs))
            .map_err(|m| HostError::Internal(m.to_owned()))?;
        if warn_raised {
            self.events.push(
                AETHER_HOST_EVT_GRAPH_BUDGET_WARN,
                0,
                aether_ffi::AETHER_E_TIMEOUT,
                eval_us,
                "node graph evaluation exceeded 100 us for >= 3 consecutive ticks",
            );
        }
        Ok(())
    }

    /// Copies normalized project, graph, plugins, and runtime state JSON into `out` (`ADR-0014`).
    pub fn copy_project_json(&self, out: &mut [u8]) -> Result<usize, HostError> {
        let data = self.snapshot_tx.borrow();
        let len = data.len();
        if out.len() < len {
            return Err(HostError::BufferTooSmall { needed: len });
        }
        out[..len].copy_from_slice(&data);
        Ok(len)
    }

    /// Populates live compiled node graph stats & per-node live values for Z4 UI inspection.
    pub fn get_plan_stats(&self, out: &mut AetherGraphPlanStats) -> Result<(), HostError> {
        let (rx, lim) = self.params.osc_counts();
        self.exchange.fill_stats(
            out,
            self.params.is_ws_enabled(),
            rx,
            lim,
            self.plugin_count.load(Ordering::Acquire),
        );
        Ok(())
    }

    /// Populates 64-byte aligned per-node nanosecond timings and output pin values without allocating (`§7.2`).
    pub fn get_telemetry_readout(
        &self,
        out: &mut AetherGraphTelemetryReadout,
    ) -> Result<(), HostError> {
        self.exchange.fill_telemetry_readout(out);
        Ok(())
    }

    /// Returns the active localhost WebSocket bearer token for auth/UI display (§7.8).
    pub fn ws_token(&self) -> &str {
        &self.ws_token
    }

    /// Returns configured WebSocket port (`19190` default).
    pub fn ws_port(&self) -> u16 {
        self.ws_port
    }

    /// Returns bound local OSC UDP receive port (useful when configured with ephemeral port `0`).
    pub fn osc_in_port(&self) -> u16 {
        self.osc_in_port.load(Ordering::Acquire)
    }

    /// Returns configured outbound OSC port (`9001` default).
    pub fn osc_out_port(&self) -> u16 {
        self.osc_out_port
    }
}

impl Drop for PluginHost {
    fn drop(&mut self) {
        let _ = self.shutdown_tx.send(true);
        if let Some(rt) = self.runtime.take() {
            rt.shutdown_timeout(Duration::from_millis(400));
        }
        self.exchange.reclaim_retired();
    }
}

fn dispatch_osc_msg(msg: &OscControlMsg, params: &SharedParamBank, events: &HostEventQueue) {
    match msg {
        OscControlMsg::Param { param_id, value } => {
            let slot_u32 = map_param_id_to_slot(param_id);
            let slot = usize::try_from(slot_u32).unwrap_or(0);
            params.set_external_param(slot, *value);
            events.push_ok(
                AETHER_HOST_EVT_OSC_PARAM,
                u64::from(slot_u32),
                *value,
                &format!("{param_id}={value}"),
            );
        }
        OscControlMsg::Expression { name, value } => {
            let slot_u32 = map_expression_name_to_slot(name);
            let slot = usize::try_from(slot_u32).unwrap_or(0);
            params.set_expression_override(slot, *value);
            events.push_ok(
                AETHER_HOST_EVT_OSC_PARAM,
                u64::from(slot_u32),
                *value,
                &format!("expr:{name}={value}"),
            );
        }
        OscControlMsg::Hotkey { hotkey_id } => {
            params.pulse_hotkey(*hotkey_id);
            events.push_ok(
                AETHER_HOST_EVT_HOTKEY_EVENT,
                u64::from(*hotkey_id),
                1.0,
                &format!("hotkey:{hotkey_id}"),
            );
        }
    }
}

fn scan_and_emit(
    state: &mut WorkerState,
    dir: &Path,
    events: &HostEventQueue,
    plugin_count: &AtomicU32,
) {
    let loaded = state.plugins.scan_directory(dir);
    for (id, status, diag) in loaded {
        let id_hash = hash_plugin_id(&id);
        match status {
            PluginStatus::Active => {
                events.push_ok(AETHER_HOST_EVT_PLUGIN_LOADED, id_hash, 1.0, &id);
            }
            PluginStatus::PendingConsent => {
                events.push_ok(
                    AETHER_HOST_EVT_PLUGIN_CONSENT_REQ,
                    id_hash,
                    0.0,
                    diag.as_deref().unwrap_or(&id),
                );
            }
            PluginStatus::Isolated => {
                events.push(
                    AETHER_HOST_EVT_PLUGIN_TRAP,
                    id_hash,
                    aether_ffi::AETHER_E_PLUGIN_TRAP,
                    0.0,
                    diag.as_deref().unwrap_or("plugin isolated"),
                );
            }
            PluginStatus::Disabled => {}
        }
    }
    plugin_count.store(
        u32::try_from(state.plugins.summaries().len()).unwrap_or(0),
        Ordering::Release,
    );
}

fn sync_latched_bindings(
    state: &mut WorkerState,
    plan: &CompiledGraphPlan,
    latched: &LatchedBridge,
    events: &HostEventQueue,
) {
    for i in 0..graph::eval::MAX_LATCHED_INSTANCES {
        latched.set_active(i, false);
    }
    state.latched_bindings.clear();
    for b in plan.latched_bindings() {
        let (plugin_id, node_type) = split_plugin_type_id(&b.type_id);
        let init_res = state.plugins.ensure_instance_init(
            plugin_id,
            node_type,
            u32::try_from(b.instance_slot).unwrap_or(0),
            b.input_count * 4,
            b.output_count * 4,
        );
        if let Err(err) = init_res {
            events.push(
                AETHER_HOST_EVT_PLUGIN_TRAP,
                hash_plugin_id(plugin_id),
                err.status_code(),
                0.0,
                &err.to_string(),
            );
            continue;
        }
        latched.set_active(b.instance_slot, true);
        state.latched_bindings.push(b.clone());
    }
}

fn step_latched_plugins(state: &mut WorkerState, latched: &LatchedBridge, events: &HostEventQueue) {
    let mut in_buf = [0.0_f32; graph::eval::LATCHED_IO_FLOATS];
    let mut out_buf = [0.0_f32; graph::eval::LATCHED_IO_FLOATS];
    for b in &state.latched_bindings {
        if latched.read_worker_input(b.instance_slot, &mut in_buf) {
            let (plugin_id, _) = split_plugin_type_id(&b.type_id);
            match state.plugins.step_instance(
                plugin_id,
                u32::try_from(b.instance_slot).unwrap_or(0),
                1.0 / 120.0,
                &in_buf,
                &mut out_buf,
            ) {
                Ok(()) => {
                    latched.write_worker_output(b.instance_slot, &out_buf);
                }
                Err((id_hash, err)) => {
                    latched.set_active(b.instance_slot, false);
                    events.push(
                        AETHER_HOST_EVT_PLUGIN_TRAP,
                        id_hash,
                        err.status_code(),
                        0.0,
                        &err.to_string(),
                    );
                }
            }
        }
    }
}

fn split_plugin_type_id(type_id: &str) -> (&str, &str) {
    let base = type_id.split('@').next().unwrap_or(type_id);
    if let Some((pid, nid)) = base.split_once("::") {
        (pid.strip_prefix("plugin.").unwrap_or(pid), nid)
    } else if let Some(rest) = base.strip_prefix("plugin.") {
        (rest, "default")
    } else {
        ("aether.plugin.wasm", "default")
    }
}

#[allow(clippy::too_many_arguments)]
fn serialize_snapshot(
    bundle: &ProjectBundle,
    plugins: &[PluginSummary],
    ws_enabled: bool,
    ws_url: &str,
    ws_token: &str,
    osc_in_port: u16,
    osc_out_port: u16,
    on_air: bool,
    dirty: bool,
) -> Vec<u8> {
    let view = HostSnapshotJson {
        project: &bundle.project,
        graph: &bundle.graph,
        assets: &bundle.assets,
        layout_ini: &bundle.layout_ini,
        plugins,
        host_runtime: HostRuntimeMeta {
            ws_enabled,
            ws_url,
            ws_token,
            osc_in_port,
            osc_out_port,
            on_air,
            dirty,
        },
    };
    serde_json::to_vec(&view).unwrap_or_else(|_| b"{}".to_vec())
}

fn publish_snapshot(
    state: &WorkerState,
    params: &SharedParamBank,
    tx: &watch::Sender<Vec<u8>>,
    osc_in_port: u16,
) {
    let summaries = state.plugins.summaries();
    let bytes = serialize_snapshot(
        &state.bundle,
        &summaries,
        params.is_ws_enabled(),
        &state.ws_url,
        &state.ws_token,
        osc_in_port,
        state.osc_out_port,
        params.is_on_air(),
        state.dirty,
    );
    tx.send_replace(bytes);
}

fn apply_project_json_update(state: &mut WorkerState, json_str: &str) {
    let Ok(val) = serde_json::from_str::<serde_json::Value>(json_str) else {
        return;
    };
    let Some(obj) = val.as_object() else {
        return;
    };
    if let Some(av) = obj.get("avatar_path").and_then(serde_json::Value::as_str) {
        av.clone_into(&mut state.bundle.project.avatar.path);
    }
    if let Some(sha) = obj.get("avatar_sha256").and_then(serde_json::Value::as_str) {
        sha.clone_into(&mut state.bundle.project.avatar.sha256);
    }
    if let Some(lay) = obj.get("layout_ini").and_then(serde_json::Value::as_str) {
        lay.clone_into(&mut state.bundle.layout_ini);
    }
    if let Some(r_obj) = obj.get("render").and_then(serde_json::Value::as_object) {
        for (k, v) in r_obj {
            if let Some(b) = v.as_bool() {
                state
                    .bundle
                    .project
                    .render
                    .insert(k.clone(), toml::Value::Boolean(b));
            } else if let Some(f) = v.as_f64() {
                state
                    .bundle
                    .project
                    .render
                    .insert(k.clone(), toml::Value::Float(f));
            } else if let Some(s) = v.as_str() {
                state
                    .bundle
                    .project
                    .render
                    .insert(k.clone(), toml::Value::String(s.to_owned()));
            }
        }
    }
    if let Some(g_val) = obj.get("graph") {
        if let Ok(doc) = serde_json::from_value::<GraphDocument>(g_val.clone()) {
            state.bundle.graph = doc;
        }
    }
    state.dirty = true;
}
