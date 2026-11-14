//! Sandboxed `wasmtime` Component & Core WASM plugin runtime (`§7.5`, `REL-002`).
//!
//! Enforces strict SHA-256 verification against `plugin.toml`, per-plugin capability
//! consent, `64 MiB` `ResourceLimiter` memory caps, `2 ms` epoch interruption deadlines,
//! and fault isolation (`PluginTrap` never crashes `T2` or `T3`).

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use wasmtime::component::{Component, Linker, ResourceTable};
use wasmtime::{Config, Engine, Instance, Module, OptLevel, ResourceLimiter, Store, TypedFunc};

use crate::error::HostError;
use crate::graph::eval::LATCHED_IO_FLOATS;

/// Hard linear memory limit per sandboxed WASM plugin instance (`64 MiB`, §7.5).
pub const PLUGIN_MAX_MEMORY_BYTES: usize = 64 * 1024 * 1024;
/// Epoch tick interval in milliseconds (`1 ms`, deadline = `2` ticks -> `2 ms`, §7.5).
pub const PLUGIN_EPOCH_INTERVAL_MS: u64 = 1;
/// Epoch deadline in ticks per `process()` call (`2 ticks` = `2 ms`, §7.5).
pub const PLUGIN_EPOCH_DEADLINE_TICKS: u64 = 2;
/// Maximum registered WASM node instances (`64`, §7.5).
pub const PLUGIN_MAX_INSTANCES: usize = 64;

#[allow(clippy::all, clippy::pedantic)]
mod wit_bindings {
    wasmtime::component::bindgen!({
        path: "wit/aether_plugin.wit",
        world: "aether-plugin",
    });
}

pub use wit_bindings::aether::plugin::types::{
    Direction as WitDirection, NodeDesc as WitNodeDesc, PortDesc as WitPortDesc,
    ValueType as WitValueType,
};

/// Parsed `plugin.toml` manifest descriptor (§7.5).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PluginManifest {
    /// Unique reverse-DNS plugin identifier.
    pub id: String,
    /// Human-readable display name.
    pub name: String,
    /// Semantic version string.
    pub version: String,
    /// Required WIT plugin API version (`"1.0"`).
    pub api: String,
    /// Author/publisher string.
    pub author: String,
    /// SPDX license identifier.
    pub license: String,
    /// Relative entry path (e.g. `"plugin.wasm"` or `"plugin.wat"`).
    pub entry: String,
    /// Declared host capabilities (`net.osc.send`, `net.ws.client`, `fs.read:plugin_dir`).
    #[serde(default)]
    pub capabilities: Vec<String>,
    /// Lowercase hex SHA-256 digest of the entry binary bytes.
    pub sha256: String,
    /// Forward-compatible preserved fields.
    #[serde(flatten)]
    pub extra: std::collections::BTreeMap<String, toml::Value>,
}

/// Lifecycle state of a discovered plugin directory.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PluginStatus {
    /// Loaded cleanly and verified.
    Active,
    /// Waiting for user capability consent via consent dialog.
    PendingConsent,
    /// User declined consent or explicitly unloaded.
    Disabled,
    /// Trapped, exceeded memory, tampered SHA-256, or exceeded 2 ms epoch deadline.
    Isolated,
}

/// Serializable summary of a registered plugin returned in project/runtime UI JSON snapshots.
#[derive(Clone, Debug, Serialize)]
pub struct PluginSummary {
    /// Manifest identifier.
    pub id: String,
    /// Deterministic FNV-1a 64-bit source hash reported in `AetherHostEvent::source_id`.
    pub id_hash: u64,
    /// Display name.
    pub name: String,
    /// Version.
    pub version: String,
    /// Declared capabilities.
    pub capabilities: Vec<String>,
    /// Current lifecycle status.
    pub status: PluginStatus,
    /// Last trap/diagnostic message, if any.
    pub last_error: Option<String>,
    /// Exported node descriptors returned by `describe()`.
    pub nodes: Vec<String>,
}

struct StoreLimits {
    max_memory: usize,
    max_tables: usize,
}

impl ResourceLimiter for StoreLimits {
    fn memory_growing(
        &mut self,
        _current: usize,
        desired: usize,
        maximum: Option<usize>,
    ) -> wasmtime::Result<bool> {
        let cap = maximum.map_or(self.max_memory, |m| m.min(self.max_memory));
        Ok(desired <= cap)
    }

    fn table_growing(
        &mut self,
        _current: usize,
        desired: usize,
        maximum: Option<usize>,
    ) -> wasmtime::Result<bool> {
        let cap = maximum.map_or(self.max_tables, |m| m.min(self.max_tables));
        Ok(desired <= cap)
    }
}

struct HostStoreData {
    plugin_id: String,
    logs: Vec<(u8, String)>,
    start_instant: std::time::Instant,
    limits: StoreLimits,
    _table: ResourceTable,
}

impl HostStoreData {
    fn new(plugin_id: String, start_instant: std::time::Instant) -> Self {
        Self {
            plugin_id,
            logs: Vec::new(),
            start_instant,
            limits: StoreLimits {
                max_memory: PLUGIN_MAX_MEMORY_BYTES,
                max_tables: 1024,
            },
            _table: ResourceTable::new(),
        }
    }
}

impl wit_bindings::aether::plugin::types::Host for HostStoreData {}

impl wit_bindings::AetherPluginImports for HostStoreData {
    fn host_log(&mut self, level: u8, msg: String) {
        if self.logs.len() < 32 {
            self.logs.push((level, msg.clone()));
        }
        tracing::info!(plugin = %self.plugin_id, level, "[wasm] {msg}");
    }

    fn host_time_us(&mut self) -> u64 {
        u64::try_from(self.start_instant.elapsed().as_micros()).unwrap_or(u64::MAX)
    }
}

enum LoadedBackend {
    Component {
        store: Store<HostStoreData>,
        bindings: wit_bindings::AetherPlugin,
    },
    Core {
        store: Store<HostStoreData>,
        instance: Instance,
        init_fn: TypedFunc<(u32, u32, u32), i32>,
        process_fn: TypedFunc<(u32, f32), i32>,
    },
}

/// Instantiated sandboxed WASM plugin isolated inside its own `wasmtime::Store`.
pub struct SandboxedPlugin {
    /// Parsed manifest.
    pub manifest: PluginManifest,
    /// Stable 64-bit hash of `manifest.id`.
    pub id_hash: u64,
    /// Current runtime status.
    pub status: PluginStatus,
    /// Diagnostic error if isolated/rejected.
    pub last_error: Option<String>,
    /// Exported node type IDs.
    pub exported_node_types: Vec<String>,
    backend: Option<LoadedBackend>,
    instance_io_offsets: HashMap<u32, u32>,
}

/// Computes deterministic 64-bit FNV-1a hash of a plugin ID for FFI event correlation.
#[must_use]
pub fn hash_plugin_id(id: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in id.as_bytes() {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x0100_0000_01b3);
    }
    h
}

/// Computes lowercase hex SHA-256 digest of a byte slice.
#[must_use]
pub fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut out = String::with_capacity(64);
    for b in digest {
        use core::fmt::Write as _;
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// Manages the shared `wasmtime::Engine` (component model + Cranelift Speed + epoch interruption)
/// and per-plugin isolated `Store`s (§7.5).
pub struct WasmPluginRuntime {
    engine: Engine,
    start_instant: std::time::Instant,
    plugins: HashMap<String, SandboxedPlugin>,
    consented: HashMap<String, bool>,
}

impl WasmPluginRuntime {
    /// Initializes the shared `wasmtime::Engine` per §7.5 specs.
    pub fn new() -> Result<Self, HostError> {
        let mut cfg = Config::new();
        cfg.wasm_component_model(true);
        cfg.cranelift_opt_level(OptLevel::Speed);
        cfg.epoch_interruption(true);
        cfg.memory_reservation(PLUGIN_MAX_MEMORY_BYTES as u64);

        let engine = Engine::new(&cfg)
            .map_err(|e| HostError::Internal(format!("wasmtime Engine init failed: {e}")))?;

        Ok(Self {
            engine,
            start_instant: std::time::Instant::now(),
            plugins: HashMap::new(),
            consented: HashMap::new(),
        })
    }

    /// Returns a thread-safe clone of the underlying `wasmtime::Engine` for the 1 ms epoch ticker.
    #[must_use]
    pub fn engine(&self) -> Engine {
        self.engine.clone()
    }

    /// Scans `plugins_dir/<id>/plugin.toml`, returning newly registered plugins and consent events.
    pub fn scan_directory(
        &mut self,
        plugins_dir: &Path,
    ) -> Vec<(String, PluginStatus, Option<String>)> {
        let mut results = Vec::new();
        let Ok(entries) = std::fs::read_dir(plugins_dir) else {
            return results;
        };
        for entry in entries.flatten() {
            let dir = entry.path();
            if !dir.is_dir() {
                continue;
            }
            let manifest_path = dir.join("plugin.toml");
            if !manifest_path.exists() {
                continue;
            }
            let res = self.load_plugin_from_dir(&dir);
            results.push(res);
        }
        results
    }

    /// Parses `plugin.toml`, checks SHA-256 & consent, and loads the sandboxed component/module.
    pub fn load_plugin_from_dir(&mut self, dir: &Path) -> (String, PluginStatus, Option<String>) {
        let manifest_path = dir.join("plugin.toml");
        let toml_text = match std::fs::read_to_string(&manifest_path) {
            Ok(s) => s,
            Err(e) => {
                let key = dir.to_string_lossy().into_owned();
                return (
                    key,
                    PluginStatus::Isolated,
                    Some(format!("read manifest failed: {e}")),
                );
            }
        };
        let manifest: PluginManifest = match toml::from_str(&toml_text) {
            Ok(m) => m,
            Err(e) => {
                let key = dir.to_string_lossy().into_owned();
                return (
                    key,
                    PluginStatus::Isolated,
                    Some(format!("invalid plugin.toml: {e}")),
                );
            }
        };

        let id = manifest.id.clone();
        let id_hash = hash_plugin_id(&id);
        let entry_path = resolve_safe_child(dir, &manifest.entry);
        let Some(entry_path) = entry_path else {
            let msg = "plugin entry escapes plugin directory".to_owned();
            self.record_failed_plugin(manifest, id_hash, PluginStatus::Isolated, msg.clone());
            return (id, PluginStatus::Isolated, Some(msg));
        };

        let bytes = match std::fs::read(&entry_path) {
            Ok(b) => b,
            Err(e) => {
                let msg = format!("read plugin binary failed: {e}");
                self.record_failed_plugin(manifest, id_hash, PluginStatus::Isolated, msg.clone());
                return (id, PluginStatus::Isolated, Some(msg));
            }
        };

        if manifest.api != "1.0" {
            let msg = format!("unsupported plugin api version '{}'", manifest.api);
            self.record_failed_plugin(manifest, id_hash, PluginStatus::Isolated, msg.clone());
            return (id, PluginStatus::Isolated, Some(msg));
        }

        // §7.5 SHA-256 verification before compiling or executing any untrusted WASM code.
        let actual_sha = sha256_hex(&bytes);
        if !actual_sha.eq_ignore_ascii_case(manifest.sha256.trim()) {
            let msg = format!(
                "SHA-256 mismatch for '{}': expected {}, got {actual_sha}",
                manifest.entry, manifest.sha256
            );
            self.record_failed_plugin(manifest, id_hash, PluginStatus::Isolated, msg.clone());
            return (id, PluginStatus::Isolated, Some(msg));
        }

        // Enforce explicit user consent for any declared capabilities (§7.5).
        if !manifest.capabilities.is_empty() {
            match self.consented.get(&id).copied() {
                None => {
                    let msg = format!(
                        "{}|{}|{}",
                        manifest.id,
                        manifest.capabilities.join(","),
                        &manifest.sha256[..manifest.sha256.len().min(16)]
                    );
                    self.record_failed_plugin(
                        manifest,
                        id_hash,
                        PluginStatus::PendingConsent,
                        msg.clone(),
                    );
                    return (id, PluginStatus::PendingConsent, Some(msg));
                }
                Some(false) => {
                    let msg = "capability consent denied by user".to_owned();
                    self.record_failed_plugin(
                        manifest,
                        id_hash,
                        PluginStatus::Disabled,
                        msg.clone(),
                    );
                    return (id, PluginStatus::Disabled, Some(msg));
                }
                Some(true) => {}
            }
        }

        match self.instantiate_bytes(&manifest, &bytes) {
            Ok(mut plugin) => {
                // Immediately verify describe() within epoch deadline.
                let desc_res = plugin.call_describe();
                if let Err(e) = desc_res {
                    let msg = format!("describe() failed: {e}");
                    plugin.status = PluginStatus::Isolated;
                    plugin.last_error = Some(msg.clone());
                    plugin.backend = None;
                    self.plugins.insert(id.clone(), plugin);
                    return (id, PluginStatus::Isolated, Some(msg));
                }
                plugin.status = PluginStatus::Active;
                self.plugins.insert(id.clone(), plugin);
                (id, PluginStatus::Active, None)
            }
            Err(e) => {
                let msg = e.to_string();
                self.record_failed_plugin(manifest, id_hash, PluginStatus::Isolated, msg.clone());
                (id, PluginStatus::Isolated, Some(msg))
            }
        }
    }

    fn record_failed_plugin(
        &mut self,
        manifest: PluginManifest,
        id_hash: u64,
        status: PluginStatus,
        msg: String,
    ) {
        let id = manifest.id.clone();
        self.plugins.insert(
            id,
            SandboxedPlugin {
                manifest,
                id_hash,
                status,
                last_error: Some(msg),
                exported_node_types: Vec::new(),
                backend: None,
                instance_io_offsets: HashMap::new(),
            },
        );
    }

    /// Loads in-memory WASM/WAT component or core module directly (helper for tests & verified memory bundles).
    pub fn load_in_memory(
        &mut self,
        manifest: &PluginManifest,
        bytes: &[u8],
    ) -> Result<(), HostError> {
        let actual_sha = sha256_hex(bytes);
        if !actual_sha.eq_ignore_ascii_case(manifest.sha256.trim()) {
            return Err(HostError::PluginTrap(format!(
                "SHA-256 mismatch: expected {}, got {actual_sha}",
                manifest.sha256
            )));
        }
        let mut plugin = self.instantiate_bytes(manifest, bytes)?;
        plugin.call_describe()?;
        plugin.status = PluginStatus::Active;
        self.plugins.insert(manifest.id.clone(), plugin);
        Ok(())
    }

    /// Records explicit user capability consent (`allow = true/false`) and retries loading if needed.
    pub fn set_consent(&mut self, plugin_id: &str, allow: bool, plugins_dir: Option<&Path>) {
        self.consented.insert(plugin_id.to_owned(), allow);
        if !allow {
            if let Some(p) = self.plugins.get_mut(plugin_id) {
                p.status = PluginStatus::Disabled;
                p.backend = None;
            }
            return;
        }
        if let Some(dir) = plugins_dir {
            let candidate = dir.join(plugin_id);
            if candidate.join("plugin.toml").exists() {
                let _ = self.load_plugin_from_dir(&candidate);
            }
        }
    }

    /// Disables and drops a plugin instance cleanly (`UNLOAD_PLUGIN`).
    pub fn unload(&mut self, plugin_id: &str) {
        if let Some(p) = self.plugins.get_mut(plugin_id) {
            p.status = PluginStatus::Disabled;
            p.backend = None;
        }
    }

    /// Runs `init` for a latched instance slot if the matching plugin is active.
    pub fn ensure_instance_init(
        &mut self,
        plugin_id: &str,
        type_id: &str,
        instance_slot: u32,
        input_bytes: u32,
        output_bytes: u32,
    ) -> Result<(), HostError> {
        let Some(plugin) = self.plugins.get_mut(plugin_id) else {
            return Err(HostError::PluginTrap(format!(
                "plugin '{plugin_id}' not loaded"
            )));
        };
        if plugin.status != PluginStatus::Active {
            return Err(HostError::PluginTrap(format!(
                "plugin '{plugin_id}' is not active"
            )));
        }
        if plugin.instance_io_offsets.contains_key(&instance_slot) {
            return Ok(());
        }
        if plugin.instance_io_offsets.len() >= PLUGIN_MAX_INSTANCES {
            return Err(HostError::LimitExceeded(
                "plugin exceeded maximum of 64 instances (§7.5)".to_owned(),
            ));
        }
        plugin.call_init(type_id, instance_slot, input_bytes, output_bytes)
    }

    /// Steps one latched plugin instance within the strict `2 ms` epoch deadline, isolating any trap/OOM.
    pub fn step_instance(
        &mut self,
        plugin_id: &str,
        instance_slot: u32,
        dt_s: f32,
        in_floats: &[f32; LATCHED_IO_FLOATS],
        out_floats: &mut [f32; LATCHED_IO_FLOATS],
    ) -> Result<(), (u64, HostError)> {
        let Some(plugin) = self.plugins.get_mut(plugin_id) else {
            return Ok(());
        };
        if plugin.status != PluginStatus::Active {
            return Ok(());
        }
        let id_hash = plugin.id_hash;
        match plugin.call_process(instance_slot, dt_s, in_floats, out_floats) {
            Ok(()) => Ok(()),
            Err(err) => {
                // Fault isolation: mark Isolated and drop backend so next frames keep running cleanly (`REL-002`).
                plugin.status = PluginStatus::Isolated;
                plugin.last_error = Some(err.to_string());
                plugin.backend = None;
                Err((id_hash, err))
            }
        }
    }

    /// Returns serializable summaries of all known plugins.
    #[must_use]
    pub fn summaries(&self) -> Vec<PluginSummary> {
        let mut out: Vec<PluginSummary> = self
            .plugins
            .values()
            .map(|p| PluginSummary {
                id: p.manifest.id.clone(),
                id_hash: p.id_hash,
                name: p.manifest.name.clone(),
                version: p.manifest.version.clone(),
                capabilities: p.manifest.capabilities.clone(),
                status: p.status,
                last_error: p.last_error.clone(),
                nodes: p.exported_node_types.clone(),
            })
            .collect();
        out.sort_by(|a, b| a.id.cmp(&b.id));
        out
    }

    /// Looks up plugin ID & status for diagnostics.
    #[must_use]
    pub fn plugin_status(&self, plugin_id: &str) -> Option<PluginStatus> {
        self.plugins.get(plugin_id).map(|p| p.status)
    }

    fn instantiate_bytes(
        &self,
        manifest: &PluginManifest,
        bytes: &[u8],
    ) -> Result<SandboxedPlugin, HostError> {
        let mut store = Store::new(
            &self.engine,
            HostStoreData::new(manifest.id.clone(), self.start_instant),
        );
        store.limiter(|data| &mut data.limits);
        store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);

        // Try Component Model first (standard `.wasm` or `(component ...)` WAT); fallback to Core Module if it's a core module.
        let is_core_wasm = bytes.starts_with(b"\x00asm\x01\x00\x00\x00");
        let is_core_wat = !is_core_wasm
            && std::str::from_utf8(bytes).is_ok_and(|s| s.trim_start().starts_with("(module"));

        let backend = if is_core_wasm || is_core_wat {
            let module = Module::new(&self.engine, bytes)
                .map_err(|e| HostError::PluginTrap(format!("WASM core module load failed: {e}")))?;
            let mut linker: wasmtime::Linker<HostStoreData> = wasmtime::Linker::new(&self.engine);
            let _ = linker.func_wrap(
                "host",
                "log",
                |mut caller: wasmtime::Caller<'_, HostStoreData>,
                 level: u32,
                 _ptr: u32,
                 _len: u32| {
                    caller
                        .data_mut()
                        .logs
                        .push((level as u8, "core".to_owned()));
                },
            );
            let _ = linker.func_wrap(
                "host",
                "time_us",
                |caller: wasmtime::Caller<'_, HostStoreData>| -> u64 {
                    u64::try_from(caller.data().start_instant.elapsed().as_micros())
                        .unwrap_or(u64::MAX)
                },
            );
            let instance = linker.instantiate(&mut store, &module).map_err(|e| {
                HostError::PluginTrap(format!("core module instantiate failed: {e}"))
            })?;
            let init_fn = instance
                .get_typed_func::<(u32, u32, u32), i32>(&mut store, "init")
                .map_err(|e| HostError::PluginTrap(format!("missing init export: {e}")))?;
            let process_fn = instance
                .get_typed_func::<(u32, f32), i32>(&mut store, "process")
                .map_err(|e| HostError::PluginTrap(format!("missing process export: {e}")))?;
            LoadedBackend::Core {
                store,
                instance,
                init_fn,
                process_fn,
            }
        } else {
            let component = Component::new(&self.engine, bytes)
                .map_err(|e| HostError::PluginTrap(format!("WASM component load failed: {e:#}")))?;
            let mut linker: Linker<HostStoreData> = Linker::new(&self.engine);
            wit_bindings::AetherPlugin::add_to_linker::<
                HostStoreData,
                wasmtime::component::HasSelf<HostStoreData>,
            >(&mut linker, |state| state)
            .map_err(|e| HostError::Internal(format!("WIT linker error: {e}")))?;
            let bindings = wit_bindings::AetherPlugin::instantiate(&mut store, &component, &linker)
                .map_err(|e| HostError::PluginTrap(format!("component instantiate failed: {e}")))?;
            LoadedBackend::Component { store, bindings }
        };

        Ok(SandboxedPlugin {
            manifest: manifest.clone(),
            id_hash: hash_plugin_id(&manifest.id),
            status: PluginStatus::Active,
            last_error: None,
            exported_node_types: Vec::new(),
            backend: Some(backend),
            instance_io_offsets: HashMap::new(),
        })
    }
}

impl SandboxedPlugin {
    fn call_describe(&mut self) -> Result<(), HostError> {
        match self.backend.as_mut() {
            Some(LoadedBackend::Component { store, bindings }) => {
                store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);
                let descs = bindings
                    .aether_plugin_node()
                    .call_describe(store)
                    .map_err(|e| map_wasm_err(&e))?;
                self.exported_node_types = descs.into_iter().map(|d| d.type_id).collect();
                Ok(())
            }
            Some(LoadedBackend::Core { .. }) => {
                self.exported_node_types = vec![self.manifest.id.clone()];
                Ok(())
            }
            None => Err(HostError::PluginTrap(
                "plugin backend unavailable".to_owned(),
            )),
        }
    }

    fn call_init(
        &mut self,
        type_id: &str,
        instance_slot: u32,
        input_bytes: u32,
        output_bytes: u32,
    ) -> Result<(), HostError> {
        match self.backend.as_mut() {
            Some(LoadedBackend::Component { store, bindings }) => {
                store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);
                let offset = bindings
                    .aether_plugin_node()
                    .call_init(store, type_id, instance_slot, input_bytes, output_bytes)
                    .map_err(|e| map_wasm_err(&e))?
                    .map_err(HostError::PluginTrap)?;
                self.instance_io_offsets.insert(instance_slot, offset);
                Ok(())
            }
            Some(LoadedBackend::Core { store, init_fn, .. }) => {
                store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);
                let off = init_fn
                    .call(store, (instance_slot, input_bytes, output_bytes))
                    .map_err(|e| map_wasm_err(&e))?;
                if off < 0 {
                    return Err(HostError::PluginTrap(format!(
                        "core plugin init failed ({off})"
                    )));
                }
                self.instance_io_offsets
                    .insert(instance_slot, u32::try_from(off).unwrap_or(0));
                Ok(())
            }
            None => Err(HostError::PluginTrap(
                "plugin backend unavailable".to_owned(),
            )),
        }
    }

    fn call_process(
        &mut self,
        instance_slot: u32,
        dt_s: f32,
        in_floats: &[f32; LATCHED_IO_FLOATS],
        out_floats: &mut [f32; LATCHED_IO_FLOATS],
    ) -> Result<(), HostError> {
        match self.backend.as_mut() {
            Some(LoadedBackend::Component { store, bindings }) => {
                store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);
                bindings
                    .aether_plugin_node()
                    .call_process(store, instance_slot, dt_s)
                    .map_err(|e| map_wasm_err(&e))?
                    .map_err(HostError::PluginTrap)?;
                out_floats[0] = in_floats[0];
                Ok(())
            }
            Some(LoadedBackend::Core {
                store,
                instance,
                process_fn,
                ..
            }) => {
                let offset = self
                    .instance_io_offsets
                    .get(&instance_slot)
                    .copied()
                    .unwrap_or(0) as usize;
                if let Some(mem) = instance.get_memory(&mut *store, "memory") {
                    let in_byte_len = LATCHED_IO_FLOATS * 4;
                    let data = mem.data_mut(&mut *store);
                    if offset.saturating_add(in_byte_len * 2) <= data.len() {
                        for (i, &val) in in_floats.iter().enumerate() {
                            let bytes = val.to_le_bytes();
                            let start = offset + i * 4;
                            data[start..start + 4].copy_from_slice(&bytes);
                        }
                    }
                }
                store.set_epoch_deadline(PLUGIN_EPOCH_DEADLINE_TICKS);
                let rc = process_fn
                    .call(&mut *store, (instance_slot, dt_s))
                    .map_err(|e| map_wasm_err(&e))?;
                if rc < 0 {
                    return Err(HostError::PluginTrap(format!(
                        "core plugin process returned {rc}"
                    )));
                }
                if let Some(mem) = instance.get_memory(&mut *store, "memory") {
                    let in_byte_len = LATCHED_IO_FLOATS * 4;
                    let out_start = offset.saturating_add(in_byte_len);
                    let data = mem.data(&*store);
                    if out_start.saturating_add(in_byte_len) <= data.len() {
                        for (i, dst) in out_floats.iter_mut().enumerate() {
                            let s = out_start + i * 4;
                            let val = f32::from_le_bytes([
                                data[s],
                                data[s + 1],
                                data[s + 2],
                                data[s + 3],
                            ]);
                            *dst = if val.is_finite() { val } else { 0.0 };
                        }
                    }
                }
                Ok(())
            }
            None => Err(HostError::PluginTrap(
                "plugin backend unavailable".to_owned(),
            )),
        }
    }
}

fn map_wasm_err(err: &wasmtime::Error) -> HostError {
    if let Some(trap) = err.downcast_ref::<wasmtime::Trap>() {
        if *trap == wasmtime::Trap::Interrupt {
            return HostError::Timeout(
                "WASM plugin exceeded 2 ms epoch deadline (§7.5)".to_owned(),
            );
        }
        return HostError::PluginTrap(format!("WASM trap: {trap}"));
    }
    HostError::PluginTrap(format!("WASM error: {err}"))
}

fn resolve_safe_child(root: &Path, rel: &str) -> Option<PathBuf> {
    let p = Path::new(rel);
    if p.is_absolute() {
        return None;
    }
    for comp in p.components() {
        if matches!(
            comp,
            std::path::Component::ParentDir
                | std::path::Component::RootDir
                | std::path::Component::Prefix(_)
        ) {
            return None;
        }
    }
    Some(root.join(p))
}
