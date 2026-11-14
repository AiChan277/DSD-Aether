//! Fuzzing entrypoints shared between `cargo-fuzz` libfuzzer targets and Windows fuzz smoke runner (`HR-06`).

use crate::graph::compile_graph_document;
use crate::io::parse_osc_packet;
use crate::project::{parse_and_migrate_project_toml, parse_graph_json};

/// Exercises untrusted `project.toml` parsing & v1 -> v2 schema migration (`HR-06`).
pub fn fuzz_project_toml(data: &[u8]) {
    let _ = parse_and_migrate_project_toml(data);
}

/// Exercises untrusted `graph.json` parsing & full DAG compilation (`HR-06`).
pub fn fuzz_graph_json(data: &[u8]) {
    if let Ok(doc) = parse_graph_json(data) {
        let _ = compile_graph_document(&doc, &[]);
    }
}

/// Exercises untrusted incoming OSC UDP packet decoding (`HR-06`).
pub fn fuzz_osc(data: &[u8]) {
    let _ = parse_osc_packet(data);
}
