//! Atomic bundle load/save (`write *.tmp` + `sync_all` + write-through replace, §7.7).

use std::fs::{self, File};
use std::io::Write;
use std::path::Path;

use crate::error::HostError;
use crate::project::schema::{
    AssetsToml, ProjectBundle, parse_and_migrate_project_toml, parse_assets_toml, parse_graph_json,
};

/// Platform callback performing atomic durable replacement `tmp_path -> dst_path`.
/// In `aether-rust-host` on Windows this delegates to `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` (§7.7).
pub type AtomicReplaceFn = fn(&Path, &Path) -> std::io::Result<()>;

/// Default safe cross-platform atomic replacement fallback (after `File::sync_all()`).
pub fn default_atomic_replace(tmp_path: &Path, dst_path: &Path) -> std::io::Result<()> {
    fs::rename(tmp_path, dst_path)
}

/// Atomically writes `bytes` to `dst_path` using a sibling `.tmp` file, `sync_all()`, and `replace_fn`.
pub fn atomic_write_bytes(
    dst_path: &Path,
    bytes: &[u8],
    replace_fn: AtomicReplaceFn,
) -> Result<(), HostError> {
    if let Some(parent) = dst_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| HostError::Io(format!("create_dir_all failed: {e}")))?;
    }
    let file_name = dst_path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("file");
    let tmp_name = format!(".{file_name}.{}.tmp", std::process::id());
    let tmp_path = dst_path.with_file_name(tmp_name);

    let write_res = (|| -> std::io::Result<()> {
        let mut f = File::create(&tmp_path)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        drop(f);
        replace_fn(&tmp_path, dst_path)
    })();

    if let Err(err) = write_res {
        let _ = fs::remove_file(&tmp_path);
        return Err(HostError::Io(format!(
            "atomic save failed for '{}': {err}",
            dst_path.display()
        )));
    }
    Ok(())
}

/// Loads and validates a `MyShow.aether/` project bundle directory from disk (§7.7).
pub fn load_project_bundle(dir: &Path) -> Result<ProjectBundle, HostError> {
    if !dir.exists() {
        return Err(HostError::Io(format!(
            "project bundle directory does not exist: '{}'",
            dir.display()
        )));
    }
    let project_bytes = fs::read(dir.join("project.toml"))
        .map_err(|e| HostError::Io(format!("failed reading project.toml: {e}")))?;
    let project = parse_and_migrate_project_toml(&project_bytes)?;

    let graph_path = dir.join("graph.json");
    let graph = if graph_path.exists() {
        let bytes = fs::read(&graph_path)
            .map_err(|e| HostError::Io(format!("failed reading graph.json: {e}")))?;
        parse_graph_json(&bytes)?
    } else {
        crate::graph::model::GraphDocument::default_project_graph()
    };

    let assets_path = dir.join("assets.toml");
    let assets = if assets_path.exists() {
        let bytes = fs::read(&assets_path)
            .map_err(|e| HostError::Io(format!("failed reading assets.toml: {e}")))?;
        parse_assets_toml(&bytes)?
    } else {
        AssetsToml::default()
    };

    let layout_path = dir.join("layout.ini");
    let layout_ini = if layout_path.exists() {
        fs::read_to_string(&layout_path)
            .map_err(|e| HostError::Io(format!("failed reading layout.ini: {e}")))?
    } else {
        String::new()
    };

    Ok(ProjectBundle {
        project,
        graph,
        assets,
        layout_ini,
    })
}

/// Atomically writes all four files inside a `*.aether/` project bundle (§7.7).
pub fn save_project_bundle(
    dir: &Path,
    bundle: &ProjectBundle,
    replace_fn: AtomicReplaceFn,
) -> Result<(), HostError> {
    fs::create_dir_all(dir)
        .map_err(|e| HostError::Io(format!("create project dir failed: {e}")))?;

    let project_toml = toml::to_string_pretty(&bundle.project)
        .map_err(|e| HostError::Internal(format!("serialize project.toml failed: {e}")))?;
    let graph_json = serde_json::to_string_pretty(&bundle.graph)
        .map_err(|e| HostError::Internal(format!("serialize graph.json failed: {e}")))?;
    let assets_toml = toml::to_string_pretty(&bundle.assets)
        .map_err(|e| HostError::Internal(format!("serialize assets.toml failed: {e}")))?;

    atomic_write_bytes(
        &dir.join("project.toml"),
        project_toml.as_bytes(),
        replace_fn,
    )?;
    atomic_write_bytes(&dir.join("graph.json"), graph_json.as_bytes(), replace_fn)?;
    atomic_write_bytes(&dir.join("assets.toml"), assets_toml.as_bytes(), replace_fn)?;
    atomic_write_bytes(
        &dir.join("layout.ini"),
        bundle.layout_ini.as_bytes(),
        replace_fn,
    )?;
    Ok(())
}
