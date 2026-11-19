//! Expression binding checks against the user's real VRoid avatar (skipped when it is not present).

use std::collections::HashSet;
use std::path::PathBuf;

use aether_asset_loader::vrm_gltf::parse_vrm_or_gltf;

fn user_vrm() -> Option<PathBuf> {
    let profile = std::env::var_os("USERPROFILE")?;
    let p = PathBuf::from(profile)
        .join("Downloads")
        .join("8590256991748008892.vrm");
    p.is_file().then_some(p)
}

#[test]
fn vroid_eye_and_mouth_presets_reach_every_face_primitive() -> Result<(), Box<dyn std::error::Error>>
{
    let Some(path) = user_vrm() else {
        eprintln!("8590256991748008892.vrm not found in Downloads; skipping");
        return Ok(());
    };
    let data = std::fs::read(&path)?;
    let base_dir = path.parent().ok_or("vrm path has no parent")?;
    let avatar = parse_vrm_or_gltf(&data, base_dir)?;

    // Map each vertex to the submesh (primitive) that references it.
    let mut vertex_submesh = vec![usize::MAX; avatar.vertices.len()];
    for (si, sm) in avatar.submeshes.iter().enumerate() {
        for &i in &sm.indices {
            if let Some(slot) = vertex_submesh.get_mut(i as usize) {
                *slot = si;
            }
        }
    }

    let mut checked = 0;
    for preset in &avatar.expr_presets {
        let lower = preset.name.to_ascii_lowercase();
        if !matches!(
            lower.as_str(),
            "blink" | "blink_l" | "blink_r" | "a" | "i" | "u" | "e" | "o"
        ) {
            continue;
        }
        let mut prims = HashSet::new();
        let mut deltas = 0usize;
        for &(m, _) in &preset.morph_binds {
            let morph = avatar.morphs.get(m).ok_or("bound morph index must exist")?;
            deltas += morph.deltas.len();
            for d in &morph.deltas {
                if let Some(&s) = vertex_submesh.get(d.vertex_index as usize) {
                    prims.insert(s);
                }
            }
        }
        eprintln!(
            "[VRM-EXPR] {:8} binds={} deltas={} primitives={}",
            preset.name,
            preset.morph_binds.len(),
            deltas,
            prims.len()
        );
        assert!(deltas > 0, "{} binds no geometry", preset.name);
        // Regression: morphs used to be split per primitive and only the first primitive's copy was bound, so a
        // blink moved 564 of 2256 vertices (skin only; lashes / eye whites / irises stayed open).
        let min_prims = if lower.starts_with("blink") { 4 } else { 3 };
        assert!(
            prims.len() >= min_prims,
            "{} only moves {} primitive(s)",
            preset.name,
            prims.len()
        );
        checked += 1;
    }
    assert!(
        checked >= 8,
        "expected Blink/Blink_L/Blink_R/A/I/U/E/O presets, found {checked}"
    );
    eprintln!(
        "[VRM-EXPR] morph targets after per-mesh merge: {}",
        avatar.morphs.len()
    );
    assert!(
        avatar.morphs.len() <= 64,
        "per-primitive morph duplicates were not merged"
    );
    Ok(())
}
