//! Versioned `*.aether/` project bundle schema, v1 -> v2 migration, and unknown-field round-trip (§7.7).

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::error::HostError;
use crate::graph::model::GraphDocument;

/// Current canonical `project.toml` schema version (`2` after v1 -> v2 migration).
pub const CURRENT_PROJECT_SCHEMA_VERSION: u32 = 2;
/// Hard cap on untrusted `project.toml` / `graph.json` size (`8 MiB`, `HR-06`).
pub const MAX_PROJECT_TEXT_BYTES: usize = 8 * 1024 * 1024;

/// Avatar reference and import settings inside `project.toml` (§7.7).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct AvatarProjectConfig {
    /// Relative or absolute avatar path.
    #[serde(default)]
    pub path: String,
    /// Verified lowercase hex SHA-256 of the avatar file.
    #[serde(default)]
    pub sha256: String,
    /// Import settings map (`axis`, `unit_scale`, `z_flatten_strength`, etc.).
    #[serde(default)]
    pub import_settings: BTreeMap<String, toml::Value>,
    /// Preserved unknown fields.
    #[serde(flatten)]
    pub extra: BTreeMap<String, toml::Value>,
}

/// Scene lighting, camera, and attached props configuration inside `project.toml` (§7.7).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct SceneProjectConfig {
    /// Directional / key light azimuth/elevation/intensity parameters.
    #[serde(default)]
    pub lights: BTreeMap<String, toml::Value>,
    /// Camera focal length & orbit parameters.
    #[serde(default)]
    pub camera: BTreeMap<String, toml::Value>,
    /// Attached prop descriptors.
    #[serde(default)]
    pub props: Vec<BTreeMap<String, toml::Value>>,
    /// Preserved unknown fields.
    #[serde(flatten)]
    pub extra: BTreeMap<String, toml::Value>,
}

/// Canonical `project.toml` document (§7.7) preserving unknown keys across versions.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ProjectToml {
    /// Bundle schema version (`1` migrates sequentially to `2`).
    pub schema_version: u32,
    /// Authoring studio application version (e.g. `"1.0.0"`).
    #[serde(default = "default_app_version")]
    pub app_version: String,
    /// Bound avatar settings & SHA-256.
    #[serde(default)]
    pub avatar: AvatarProjectConfig,
    /// Scene lights, camera, props.
    #[serde(default)]
    pub scene: SceneProjectConfig,
    /// Rendering parameters (`render.*` overrides).
    #[serde(default)]
    pub render: BTreeMap<String, toml::Value>,
    /// Tracking router & filter parameters.
    #[serde(default)]
    pub tracking: BTreeMap<String, toml::Value>,
    /// Virtual Cam / Spout2 / NDI / OSC / WebSocket output configuration.
    #[serde(default)]
    pub outputs: BTreeMap<String, toml::Value>,
    /// Studio UI layout/density/inspector state.
    #[serde(default)]
    pub ui: BTreeMap<String, toml::Value>,
    /// Unknown top-level fields preserved intact across save/load round-trips (§7.7).
    #[serde(flatten)]
    pub extra: BTreeMap<String, toml::Value>,
}

fn default_app_version() -> String {
    "1.0.0".to_owned()
}

impl Default for ProjectToml {
    fn default() -> Self {
        let mut render = BTreeMap::new();
        render.insert("flatness".to_owned(), toml::Value::Float(0.45));
        render.insert("outline_width".to_owned(), toml::Value::Float(1.0));
        render.insert("rt_shadows".to_owned(), toml::Value::Boolean(true));
        Self {
            schema_version: CURRENT_PROJECT_SCHEMA_VERSION,
            app_version: default_app_version(),
            avatar: AvatarProjectConfig::default(),
            scene: SceneProjectConfig::default(),
            render,
            tracking: BTreeMap::new(),
            outputs: BTreeMap::new(),
            ui: BTreeMap::new(),
            extra: BTreeMap::new(),
        }
    }
}

/// Asset manifest entry recorded inside `*.aether/assets.toml` (§7.7).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProjectAssetRef {
    /// Relative asset path inside or alongside the project bundle.
    pub relative_path: String,
    /// Lowercase hex SHA-256 content hash.
    pub sha256: String,
    /// Logical kind (`"avatar"`, `"texture"`, `"plugin"`, etc.).
    #[serde(default)]
    pub kind: String,
}

/// Bundle asset inventory (`assets.toml`) with preserved unknown fields.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct AssetsToml {
    /// Relative asset entries & hashes.
    #[serde(default)]
    pub assets: Vec<ProjectAssetRef>,
    /// Unknown keys preserved intact.
    #[serde(flatten)]
    pub extra: BTreeMap<String, toml::Value>,
}

/// Complete in-memory loaded project bundle (`*.aether/`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProjectBundle {
    /// Normalized & migrated `project.toml`.
    pub project: ProjectToml,
    /// Node graph document (`graph.json`).
    pub graph: GraphDocument,
    /// Asset reference table (`assets.toml`).
    pub assets: AssetsToml,
    /// Raw Dear ImGui `layout.ini` string.
    pub layout_ini: String,
}

/// Migrates a parsed `v1` project document sequentially to `v2`, preserving unknown fields (§7.7).
#[must_use]
pub fn migrate_v1_to_v2(mut v1: ProjectToml) -> ProjectToml {
    if v1.schema_version >= 2 {
        return v1;
    }
    // Legacy v1 fields allowed `avatar_path` in `extra` or `render.z_flatten` instead of `render.flatness`.
    if v1.avatar.path.is_empty() {
        if let Some(toml::Value::String(p)) = v1.extra.remove("avatar_path") {
            v1.avatar.path = p;
        }
    }
    if !v1.render.contains_key("flatness") {
        if let Some(val) = v1.render.remove("z_flatten") {
            v1.render.insert("flatness".to_owned(), val);
        }
    }
    if let Some(toml::Value::Table(legacy_rt)) = v1.extra.remove("renderer") {
        for (k, val) in legacy_rt {
            v1.render.entry(k).or_insert(val);
        }
    }
    v1.schema_version = 2;
    v1
}

/// Parses untrusted `project.toml` bytes safely and executes versioned migrations (`v1 -> v2`).
pub fn parse_and_migrate_project_toml(bytes: &[u8]) -> Result<ProjectToml, HostError> {
    if bytes.len() > MAX_PROJECT_TEXT_BYTES {
        return Err(HostError::LimitExceeded(
            "project.toml exceeds 8 MiB resource limit".to_owned(),
        ));
    }
    let text = core::str::from_utf8(bytes)
        .map_err(|e| HostError::parse(format!("project.toml invalid UTF-8: {e}")))?;
    let mut parsed: ProjectToml = toml::from_str(text)
        .map_err(|e| HostError::parse(format!("project.toml parse error: {e}")))?;
    if parsed.schema_version == 0 {
        return Err(HostError::parse("project.toml schema_version 0 is invalid"));
    }
    if parsed.schema_version == 1 {
        parsed = migrate_v1_to_v2(parsed);
    }
    Ok(parsed)
}

/// Parses untrusted `graph.json` bytes with size caps (`HR-06`).
pub fn parse_graph_json(bytes: &[u8]) -> Result<GraphDocument, HostError> {
    if bytes.len() > MAX_PROJECT_TEXT_BYTES {
        return Err(HostError::LimitExceeded(
            "graph.json exceeds 8 MiB resource limit".to_owned(),
        ));
    }
    serde_json::from_slice(bytes)
        .map_err(|e| HostError::parse(format!("graph.json parse error: {e}")))
}

/// Parses `assets.toml` bytes with size caps (`HR-06`).
pub fn parse_assets_toml(bytes: &[u8]) -> Result<AssetsToml, HostError> {
    if bytes.len() > MAX_PROJECT_TEXT_BYTES {
        return Err(HostError::LimitExceeded(
            "assets.toml exceeds 8 MiB resource limit".to_owned(),
        ));
    }
    let text = core::str::from_utf8(bytes)
        .map_err(|e| HostError::parse(format!("assets.toml invalid UTF-8: {e}")))?;
    toml::from_str(text).map_err(|e| HostError::parse(format!("assets.toml parse error: {e}")))
}
