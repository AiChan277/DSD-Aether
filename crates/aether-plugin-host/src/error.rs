//! Typed errors for `aether-plugin-host` mapped 1:1 onto [`AetherStatus`] (`HR-04`, §3.6).

use aether_ffi::{
    AETHER_E_BUFFER_TOO_SMALL, AETHER_E_INTERNAL, AETHER_E_INVALID_ARG, AETHER_E_IO,
    AETHER_E_LIMIT_EXCEEDED, AETHER_E_PARSE, AETHER_E_PLUGIN_TRAP, AETHER_E_TIMEOUT, AetherStatus,
};
use thiserror::Error;

/// Typed errors returned by the plugin host, graph compiler, project loader, and I/O tasks.
#[derive(Debug, Error)]
pub enum HostError {
    /// Null, misaligned, or invalid argument.
    #[error("invalid plugin host argument: {0}")]
    InvalidArg(String),
    /// Filesystem or socket I/O error.
    #[error("plugin host I/O error: {0}")]
    Io(String),
    /// Malformed TOML, JSON, OSC, or graph topology error (such as cycle or type mismatch).
    #[error("parse or compile error (node {node_id:?}): {message}")]
    Parse {
        /// Offending node ID when compiling a node graph, if applicable.
        node_id: Option<u64>,
        /// Human-readable diagnostic message.
        message: String,
    },
    /// Caller-supplied buffer is smaller than required (`out_len` filled with required length).
    #[error("caller buffer too small: need {needed} bytes")]
    BufferTooSmall {
        /// Exact required byte length.
        needed: usize,
    },
    /// Hard resource cap (§3.6 / §7.5 / §7.8) exceeded.
    #[error("resource limit exceeded: {0}")]
    LimitExceeded(String),
    /// Sandboxed WASM plugin trapped, ran out of memory, or violated SHA-256 verification.
    #[error("plugin trapped or rejected: {0}")]
    PluginTrap(String),
    /// WASM plugin or operation exceeded its execution deadline.
    #[error("plugin execution timed out: {0}")]
    Timeout(String),
    /// Internal runtime or concurrency invariant violated.
    #[error("internal plugin host error: {0}")]
    Internal(String),
}

impl HostError {
    /// Convenience constructor for non-graph parse errors.
    #[must_use]
    pub fn parse(msg: impl Into<String>) -> Self {
        Self::Parse {
            node_id: None,
            message: msg.into(),
        }
    }

    /// Convenience constructor for graph compile errors associated with a specific node ID.
    #[must_use]
    pub fn compile(node_id: u64, msg: impl Into<String>) -> Self {
        Self::Parse {
            node_id: Some(node_id),
            message: msg.into(),
        }
    }

    /// Maps the error to its frozen [`AetherStatus`] FFI return code.
    #[must_use]
    pub const fn status_code(&self) -> AetherStatus {
        match self {
            Self::InvalidArg(_) => AETHER_E_INVALID_ARG,
            Self::Io(_) => AETHER_E_IO,
            Self::Parse { .. } => AETHER_E_PARSE,
            Self::BufferTooSmall { .. } => AETHER_E_BUFFER_TOO_SMALL,
            Self::LimitExceeded(_) => AETHER_E_LIMIT_EXCEEDED,
            Self::PluginTrap(_) => AETHER_E_PLUGIN_TRAP,
            Self::Timeout(_) => AETHER_E_TIMEOUT,
            Self::Internal(_) => AETHER_E_INTERNAL,
        }
    }
}
