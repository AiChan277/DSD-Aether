//! Versioned project bundle format (`*.aether/`), migrations, and atomic saves (§7.7).

pub mod atomic;
pub mod schema;

pub use atomic::{
    AtomicReplaceFn, atomic_write_bytes, default_atomic_replace, load_project_bundle,
    save_project_bundle,
};
pub use schema::{
    AssetsToml, AvatarProjectConfig, CURRENT_PROJECT_SCHEMA_VERSION, MAX_PROJECT_TEXT_BYTES,
    ProjectAssetRef, ProjectBundle, ProjectToml, SceneProjectConfig, migrate_v1_to_v2,
    parse_and_migrate_project_toml, parse_assets_toml, parse_graph_json,
};
