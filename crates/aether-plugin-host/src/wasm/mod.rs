//! Sandboxed WASM plugin runtime (`wasmtime` Component Model + Core I/O modules, §7.5).

pub mod engine;

pub use engine::{
    PLUGIN_EPOCH_DEADLINE_TICKS, PLUGIN_EPOCH_INTERVAL_MS, PLUGIN_MAX_INSTANCES,
    PLUGIN_MAX_MEMORY_BYTES, PluginManifest, PluginStatus, PluginSummary, WasmPluginRuntime,
    hash_plugin_id, sha256_hex,
};
