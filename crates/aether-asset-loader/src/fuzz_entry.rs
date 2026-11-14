//! Bounds-checked fuzz target entrypoints (`HR-06`, `ADR-0005`).
//!
//! Exposed cleanly so both `cargo-fuzz` (`fuzz/fuzz_targets/*.rs`) and the native Windows
//! CI/10-minute runner (`examples/fuzz_runner.rs`) drive identical safe parser code paths.

use std::path::Path;

use crate::bone_matcher::match_humanoid_bones;
use crate::fbx::parse_fbx;
use crate::limits::check_file_size;
use crate::pmx::parse_pmx_or_pmd;
use crate::retarget::{build_expression_retarget, build_spring_descriptors, normalise_avatar_pose};
use crate::textures::plan_textures;
use crate::vrm_gltf::parse_vrm_or_gltf;

fn exercise_post_parse(mut avatar: crate::ir::ParsedAvatar, raw_bytes: &[u8]) {
    let matched = match_humanoid_bones(&avatar.joints);
    let (poses, _, _, _) = normalise_avatar_pose(&mut avatar, &matched);
    let _ = build_expression_retarget(&avatar);
    let _ = build_spring_descriptors(&avatar, &poses);
    let mut warnings = avatar.warnings_mask;
    let _ = plan_textures(&mut avatar.textures, raw_bytes, true, false, &mut warnings);
}

/// Fuzz target 1: untrusted GLB / glTF 2.0 / VRM 0.x / VRM 1.0 binary container (`fuzz_glb`).
pub fn fuzz_glb(data: &[u8]) {
    if check_file_size(data.len() as u64).is_err() {
        return;
    }
    if let Ok(avatar) = parse_vrm_or_gltf(data, Path::new(".")) {
        exercise_post_parse(avatar, data);
    }
}

/// Fuzz target 2: untrusted VRM/glTF JSON metadata & `EXT_meshopt_compression` (`fuzz_vrm_json`).
pub fn fuzz_vrm_json(data: &[u8]) {
    if data.is_empty() || data.len() > 8 * 1024 * 1024 {
        return;
    }
    // Feed both raw JSON wrapped in a valid 12-byte GLB header + JSON chunk AND raw bytes.
    if data.starts_with(b"glTF") {
        fuzz_glb(data);
        return;
    }
    let padded_len = (data.len() + 3) & !3;
    let total_len = 12 + 8 + padded_len;
    let mut glb = Vec::with_capacity(total_len);
    glb.extend_from_slice(b"glTF");
    glb.extend_from_slice(&2u32.to_le_bytes());
    glb.extend_from_slice(&(total_len as u32).to_le_bytes());
    glb.extend_from_slice(&(padded_len as u32).to_le_bytes());
    glb.extend_from_slice(&0x4E4F_534Au32.to_le_bytes()); // "JSON"
    glb.extend_from_slice(data);
    glb.resize(total_len, b' ');

    if let Ok(avatar) = parse_vrm_or_gltf(&glb, Path::new(".")) {
        exercise_post_parse(avatar, &glb);
    }
}

/// Fuzz target 3: untrusted Autodesk FBX Binary & ASCII 6.x/7.x via `ufbx` (`fuzz_fbx`).
pub fn fuzz_fbx(data: &[u8]) {
    if check_file_size(data.len() as u64).is_err() {
        return;
    }
    if let Ok(avatar) = parse_fbx(data, Path::new(".")) {
        exercise_post_parse(avatar, data);
    }
}

/// Fuzz target 4: untrusted MMD PMX 2.0/2.1 & Shift-JIS PMD 1.0 (`fuzz_pmx`).
pub fn fuzz_pmx(data: &[u8]) {
    if check_file_size(data.len() as u64).is_err() {
        return;
    }
    if let Ok(avatar) = parse_pmx_or_pmd(data, Path::new(".")) {
        exercise_post_parse(avatar, data);
    }
}
