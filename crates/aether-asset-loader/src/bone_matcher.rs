//! Appendix A 55-joint Fuzzy Bone Name Matcher + Hungarian 1:1 solver (`§6.4`).

use std::collections::HashMap;

use aether_ffi::{
    AETHER_HUMANOID_BONE_COUNT, AETHER_JOINT_REGION_HAIR, AETHER_JOINT_REGION_HEAD,
    AETHER_JOINT_REGION_HIPS_LEGS, AETHER_JOINT_REGION_NECK_UPPER, AETHER_JOINT_REGION_OTHER,
    AETHER_JOINT_REGION_TORSO_ARMS, AETHER_WARN_REVIEW_BONES, AETHER_WARN_UNMAPPED_REQUIRED,
};
use unicode_normalization::UnicodeNormalization;

use crate::ir::RawJoint;

/// Canonical Appendix A names indexed `0..54`.
pub const CANONICAL_55_NAMES: [&str; 55] = [
    "hips",                    // 0
    "spine",                   // 1
    "chest",                   // 2
    "upperChest",              // 3
    "neck",                    // 4
    "head",                    // 5
    "leftEye",                 // 6
    "rightEye",                // 7
    "jaw",                     // 8
    "leftUpperLeg",            // 9
    "leftLowerLeg",            // 10
    "leftFoot",                // 11
    "leftToes",                // 12
    "rightUpperLeg",           // 13
    "rightLowerLeg",           // 14
    "rightFoot",               // 15
    "rightToes",               // 16
    "leftShoulder",            // 17
    "leftUpperArm",            // 18
    "leftLowerArm",            // 19
    "leftHand",                // 20
    "rightShoulder",           // 21
    "rightUpperArm",           // 22
    "rightLowerArm",           // 23
    "rightHand",               // 24
    "leftThumbProximal",       // 25
    "leftThumbIntermediate",   // 26
    "leftThumbDistal",         // 27
    "leftIndexProximal",       // 28
    "leftIndexIntermediate",   // 29
    "leftIndexDistal",         // 30
    "leftMiddleProximal",      // 31
    "leftMiddleIntermediate",  // 32
    "leftMiddleDistal",        // 33
    "leftRingProximal",        // 34
    "leftRingIntermediate",    // 35
    "leftRingDistal",          // 36
    "leftLittleProximal",      // 37
    "leftLittleIntermediate",  // 38
    "leftLittleDistal",        // 39
    "rightThumbProximal",      // 40
    "rightThumbIntermediate",  // 41
    "rightThumbDistal",        // 42
    "rightIndexProximal",      // 43
    "rightIndexIntermediate",  // 44
    "rightIndexDistal",        // 45
    "rightMiddleProximal",     // 46
    "rightMiddleIntermediate", // 47
    "rightMiddleDistal",       // 48
    "rightRingProximal",       // 49
    "rightRingIntermediate",   // 50
    "rightRingDistal",         // 51
    "rightLittleProximal",     // 52
    "rightLittleIntermediate", // 53
    "rightLittleDistal",       // 54
];

/// Expected canonical parent index (`None` for hips root).
pub const CANONICAL_PARENT: [Option<u16>; 55] = [
    None,     // 0 hips
    Some(0),  // 1 spine
    Some(1),  // 2 chest
    Some(2),  // 3 upperChest
    Some(3),  // 4 neck
    Some(4),  // 5 head
    Some(5),  // 6 leftEye
    Some(5),  // 7 rightEye
    Some(5),  // 8 jaw
    Some(0),  // 9 leftUpperLeg
    Some(9),  // 10 leftLowerLeg
    Some(10), // 11 leftFoot
    Some(11), // 12 leftToes
    Some(0),  // 13 rightUpperLeg
    Some(13), // 14 rightLowerLeg
    Some(14), // 15 rightFoot
    Some(15), // 16 rightToes
    Some(3),  // 17 leftShoulder
    Some(17), // 18 leftUpperArm
    Some(18), // 19 leftLowerArm
    Some(19), // 20 leftHand
    Some(3),  // 21 rightShoulder
    Some(21), // 22 rightUpperArm
    Some(22), // 23 rightLowerArm
    Some(23), // 24 rightHand
    Some(20),
    Some(25),
    Some(26), // 25..27 left thumb
    Some(20),
    Some(28),
    Some(29), // 28..30 left index
    Some(20),
    Some(31),
    Some(32), // 31..33 left middle
    Some(20),
    Some(34),
    Some(35), // 34..36 left ring
    Some(20),
    Some(37),
    Some(38), // 37..39 left little
    Some(24),
    Some(40),
    Some(41), // 40..42 right thumb
    Some(24),
    Some(43),
    Some(44), // 43..45 right index
    Some(24),
    Some(46),
    Some(47), // 46..48 right middle
    Some(24),
    Some(49),
    Some(50), // 49..51 right ring
    Some(24),
    Some(52),
    Some(53), // 52..54 right little
];

/// Required core humanoid slots: failing `< 0.55` sets `AETHER_WARN_UNMAPPED_REQUIRED`.
const REQUIRED_CORE_SLOTS: [u16; 15] = [
    0, 1, 5, // hips, spine, head
    9, 10, 11, 13, 14, 15, // legs + feet
    18, 19, 20, 22, 23, 24, // arms + hands
];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Chirality {
    Center,
    Left,
    Right,
}

fn canonical_chirality(h: usize) -> Chirality {
    if matches!(h, 6 | 9 | 10 | 11 | 12 | 17 | 18 | 19 | 20 | 25..=39) {
        Chirality::Left
    } else if matches!(h, 7 | 13 | 14 | 15 | 16 | 21 | 22 | 23 | 24 | 40..=54) {
        Chirality::Right
    } else {
        Chirality::Center
    }
}

/// Maps explicit VRM / canonical camelCase names to `0..54`.
#[must_use]
pub fn canonical_humanoid_index(name: &str) -> Option<u16> {
    let norm: String = name.nfkc().collect();
    let lower = norm.trim().to_ascii_lowercase();
    for (idx, canon) in CANONICAL_55_NAMES.iter().enumerate() {
        if lower == canon.to_ascii_lowercase() {
            return Some(idx as u16);
        }
    }
    None
}

fn translate_jp_to_tokens(nfkc_lower: &str) -> Option<&'static str> {
    const JP_MAP: &[(&str, &str)] = &[
        ("下半身", "hips"),
        ("センター", "mmd_center"),
        ("グルーブ", "mmd_groove"),
        ("全ての親", "mmd_root"),
        ("上半身3", "upper chest"),
        ("上半身2", "chest"),
        ("上半身", "spine"),
        ("首", "neck"),
        ("頭", "head"),
        ("左目", "left eye"),
        ("右目", "right eye"),
        ("顎", "jaw"),
        ("左足", "left upper leg"),
        ("左ひざ", "left lower leg"),
        ("左膝", "left lower leg"),
        ("左足首", "left foot"),
        ("左つま先", "left toes"),
        ("右足", "right upper leg"),
        ("右ひざ", "right lower leg"),
        ("右膝", "right lower leg"),
        ("右足首", "right foot"),
        ("右つま先", "right toes"),
        ("左肩", "left shoulder"),
        ("左腕", "left upper arm"),
        ("左ひじ", "left lower arm"),
        ("左肘", "left lower arm"),
        ("左手首", "left hand"),
        ("右肩", "right shoulder"),
        ("右腕", "right upper arm"),
        ("右ひじ", "right lower arm"),
        ("右肘", "right lower arm"),
        ("右手首", "right hand"),
        ("左親指0", "left thumb 1"),
        ("左親指1", "left thumb 2"),
        ("左親指2", "left thumb 3"),
        ("右親指0", "right thumb 1"),
        ("右親指1", "right thumb 2"),
        ("右親指2", "right thumb 3"),
        ("左人指1", "left index 1"),
        ("左人指2", "left index 2"),
        ("左人指3", "left index 3"),
        ("左人差し指1", "left index 1"),
        ("左人差し指2", "left index 2"),
        ("左人差し指3", "left index 3"),
        ("右人指1", "right index 1"),
        ("右人指2", "right index 2"),
        ("右人指3", "right index 3"),
        ("右人差し指1", "right index 1"),
        ("右人差し指2", "right index 2"),
        ("右人差し指3", "right index 3"),
        ("左中指1", "left middle 1"),
        ("左中指2", "left middle 2"),
        ("左中指3", "left middle 3"),
        ("右中指1", "right middle 1"),
        ("右中指2", "right middle 2"),
        ("右中指3", "right middle 3"),
        ("左薬指1", "left ring 1"),
        ("左薬指2", "left ring 2"),
        ("左薬指3", "left ring 3"),
        ("右薬指1", "right ring 1"),
        ("右薬指2", "right ring 2"),
        ("右薬指3", "right ring 3"),
        ("左小指1", "left little 1"),
        ("左小指2", "left little 2"),
        ("左小指3", "left little 3"),
        ("右小指1", "right little 1"),
        ("右小指2", "right little 2"),
        ("右小指3", "right little 3"),
    ];
    for &(jp, en) in JP_MAP {
        if nfkc_lower == jp {
            return Some(en);
        }
    }
    None
}

fn jp_en_to_normalized(en: &str) -> (Vec<String>, Chirality) {
    let mut side = Chirality::Center;
    let mut toks = Vec::new();
    for w in en.split_whitespace() {
        if w == "left" {
            side = Chirality::Left;
        } else if w == "right" {
            side = Chirality::Right;
        } else {
            toks.push(w.to_owned());
        }
    }
    (toks, side)
}

fn strip_prefixes_ci(mut s: &str) -> (&str, Chirality) {
    let mut side = Chirality::Center;
    for _ in 0..4 {
        let lower = s.to_ascii_lowercase();
        let mut stripped = false;
        if let Some(pos) = s.find("::") {
            s = &s[pos + 2..];
            continue;
        }
        for p in [
            "j_bip_l_",
            "j_bip_r_",
            "j_bip_c_",
            "mixamorig:",
            "mixamorig_",
            "mixamorig",
            "bip01_",
            "bip01 ",
            "bip001_",
            "bip001 ",
            "def-",
            "def_",
            "org-",
            "mch-",
            "valvebiped_",
            "cc_base_",
            "rig_",
        ] {
            if lower.starts_with(p) {
                if p == "j_bip_l_" {
                    side = Chirality::Left;
                } else if p == "j_bip_r_" {
                    side = Chirality::Right;
                }
                s = &s[p.len()..];
                stripped = true;
                break;
            }
        }
        if stripped {
            continue;
        }
        // Generic `namespace:BoneName` (e.g. `mixamorig1:Hips` or `Character:Hips`)
        if let Some(pos) = s.find(':')
            && !s[..pos].contains(' ')
        {
            s = &s[pos + 1..];
            continue;
        }
        break;
    }
    (s, side)
}

fn normalize_tokens(raw: &str) -> (Vec<String>, Chirality) {
    let nfkc: String = raw.nfkc().collect();
    let trimmed = nfkc.trim();
    if let Some(en) = translate_jp_to_tokens(trimmed) {
        return jp_en_to_normalized(en);
    }

    // Split camelCase + separators after iteratively stripping rig/FBX/namespace prefixes
    let (orig_after_prefix, prefix_side) = strip_prefixes_ci(trimmed);
    if let Some(en) = translate_jp_to_tokens(orig_after_prefix) {
        return jp_en_to_normalized(en);
    }
    let mut raw_tokens = Vec::new();
    let mut cur = String::new();
    let chars: Vec<char> = orig_after_prefix.chars().collect();
    for (i, &c) in chars.iter().enumerate() {
        if !c.is_alphanumeric() {
            if !cur.is_empty() {
                raw_tokens.push(core::mem::take(&mut cur));
            }
            continue;
        }
        let split_camel = c.is_ascii_uppercase()
            && chars
                .get(i.saturating_sub(1))
                .is_some_and(char::is_ascii_lowercase);
        let split_digit = c.is_ascii_digit()
            && chars
                .get(i.saturating_sub(1))
                .is_some_and(char::is_ascii_alphabetic);
        if (split_camel || split_digit) && !cur.is_empty() {
            raw_tokens.push(core::mem::take(&mut cur));
        }
        cur.push(c.to_ascii_lowercase());
    }
    if !cur.is_empty() {
        raw_tokens.push(cur);
    }

    // Determine chirality & normalize synonyms
    let mut side = prefix_side;
    let mut norm = Vec::with_capacity(raw_tokens.len());
    for tok in raw_tokens {
        match tok.as_str() {
            "l" | "left" | "左" => {
                side = Chirality::Left;
            }
            "r" | "right" | "右" => {
                side = Chirality::Right;
            }
            "pelvis" | "hip" => norm.push("hips".to_owned()),
            "clavicle" => norm.push("shoulder".to_owned()),
            "upperarm" => {
                norm.push("upper".to_owned());
                norm.push("arm".to_owned());
            }
            "lowerarm" | "forearm" => {
                norm.push("lower".to_owned());
                norm.push("arm".to_owned());
            }
            "thigh" | "upleg" | "upperleg" => {
                norm.push("upper".to_owned());
                norm.push("leg".to_owned());
            }
            "calf" | "shin" | "lowerleg" => {
                norm.push("lower".to_owned());
                norm.push("leg".to_owned());
            }
            "ball" | "toebase" | "toe" => norm.push("toes".to_owned()),
            "pinky" => norm.push("little".to_owned()),
            "01" | "1" | "prox" | "proximal" => norm.push("1".to_owned()),
            "02" | "2" | "mid" | "middle_phalanx" | "intermediate" => norm.push("2".to_owned()),
            "03" | "3" | "end" | "tip" | "distal" => norm.push("3".to_owned()),
            _ => norm.push(tok),
        }
    }
    // Drop redundant `hand` token when qualified by a specific finger noun (Mixamo `LeftHandThumb1`).
    let has_finger = norm
        .iter()
        .any(|t| matches!(t.as_str(), "thumb" | "index" | "middle" | "ring" | "little"));
    if has_finger {
        norm.retain(|t| t != "hand");
    }
    (norm, side)
}

#[derive(Clone, Copy)]
struct SkeletonConvention {
    has_explicit_hips: bool,
    has_bare_spine: bool,
}

fn detect_convention(joints: &[RawJoint]) -> SkeletonConvention {
    let mut has_explicit_hips = false;
    let mut has_bare_spine = false;
    for j in joints {
        let (toks, _) = normalize_tokens(&j.name);
        if toks.as_slice() == ["hips"] {
            has_explicit_hips = true;
        } else if toks.as_slice() == ["spine"] {
            has_bare_spine = true;
        }
    }
    SkeletonConvention {
        has_explicit_hips,
        has_bare_spine,
    }
}

fn is_mmd_cog_ancestor(name: &str) -> bool {
    let n: String = name.nfkc().collect();
    let t = n.trim();
    matches!(t, "センター" | "グルーブ" | "全ての親" | "center" | "groove" | "motherbone")
}

fn exact_alias_match(
    h_idx: usize,
    tokens: &[String],
    side: Chirality,
    conv: SkeletonConvention,
) -> Option<f32> {
    let want_side = canonical_chirality(h_idx);
    if want_side != Chirality::Center && side != Chirality::Center && want_side != side {
        return Some(0.0);
    }
    if want_side == Chirality::Center && side != Chirality::Center {
        return Some(0.0);
    }

    // Spine chains: Rigify (`DEF-spine` -> `0`, `.001..006`), Mixamo (`Spine`/`Spine1`/`Spine2`), UE (`spine_01..03`)
    if tokens.first().map(String::as_str) == Some("spine") {
        let seg = tokens.get(1).map(String::as_str);
        if !conv.has_explicit_hips {
            match (h_idx, seg) {
                (0, None) | (4, Some("004" | "4" | "005")) => return Some(0.95),
                (1, Some("001" | "1")) | (2, Some("002" | "2")) | (3, Some("003" | "3")) => {
                    return Some(0.98);
                }
                (5, Some("006" | "6")) => return Some(0.96),
                _ => {}
            }
        } else if conv.has_bare_spine {
            match (h_idx, seg) {
                (1, None) => return Some(0.99),
                (2, Some("1" | "01")) | (3, Some("2" | "02")) => return Some(0.98),
                (3, Some("3" | "03")) => return Some(0.96),
                _ => {}
            }
        } else {
            match (h_idx, seg) {
                (1, Some("1" | "01")) | (2, Some("2" | "02")) | (3, Some("3" | "03")) => {
                    return Some(0.98);
                }
                _ => {}
            }
        }
    }

    // Mixamo `LeftLeg`/`RightLeg` (`["leg"]` -> lower leg) & `LeftArm`/`RightArm` (`["arm"]` -> upper arm), UE `neck_01`
    if tokens == ["leg"] && matches!(h_idx, 10 | 14) && side == want_side {
        return Some(0.96);
    }
    if tokens == ["arm"] && matches!(h_idx, 18 | 22) && side == want_side {
        return Some(0.96);
    }
    if tokens == ["neck", "1"] && h_idx == 4 && side == Chirality::Center {
        return Some(0.96);
    }
    if matches!(
        tokens.first().map(String::as_str),
        Some("mmd_center" | "mmd_groove" | "mmd_root")
    ) {
        return if h_idx == 0 && !conv.has_explicit_hips {
            Some(0.92)
        } else {
            Some(0.0)
        };
    }

    None
}

fn token_similarity(h_idx: usize, joint: &RawJoint, conv: SkeletonConvention) -> f32 {
    if let Some(exp) = joint.explicit_humanoid {
        return if usize::from(exp) == h_idx { 1.0 } else { 0.0 };
    }
    let canon_name = CANONICAL_55_NAMES[h_idx];
    let (c_toks, c_side) = normalize_tokens(canon_name);

    let mut best = 0.0f32;
    for candidate_str in [&joint.name, &joint.alt_name] {
        if candidate_str.is_empty() {
            continue;
        }
        let (j_toks, j_side) = normalize_tokens(candidate_str);
        if let Some(exact) = exact_alias_match(h_idx, &j_toks, j_side, conv) {
            best = best.max(exact);
            continue;
        }
        if c_side != j_side && (c_side != Chirality::Center || j_side != Chirality::Center) {
            continue;
        }
        // Check finger `f index 1` etc. (ignore stray `f` prefix in Rigify `DEF-f_index.01.L`)
        let filtered_j: Vec<&str> = j_toks
            .iter()
            .map(String::as_str)
            .filter(|t| *t != "f")
            .collect();
        if filtered_j.is_empty() || c_toks.is_empty() {
            continue;
        }
        let mut hits = 0usize;
        for ct in &c_toks {
            if filtered_j.contains(&ct.as_str()) {
                hits += 1;
            }
        }
        // Special spine / upper chest distinctions: avoid "upper"+"leg" matching "upper"+"arm"
        let dice = (2.0 * hits as f32) / ((c_toks.len() + filtered_j.len()) as f32);
        // Require primary body noun overlap for limbs/fingers
        if dice > 0.0 && (hits == c_toks.len() && hits == filtered_j.len()) {
            best = best.max(1.0);
        } else if hits >= c_toks.len().min(filtered_j.len()) && dice >= 0.65 {
            best = best.max(dice * 0.96);
        } else {
            best = best.max(dice * 0.75);
        }
    }
    best
}

fn world_positions(joints: &[RawJoint]) -> Vec<[f32; 3]> {
    let mut world = vec![[0.0f32; 3]; joints.len()];
    for i in 0..joints.len() {
        let mut pos = joints[i].local_translation;
        let mut p = joints[i].parent;
        let mut depth = 0usize;
        while let Some(pi) = p
            && depth < 128
        {
            pos[0] += world[pi][0];
            pos[1] += world[pi][1];
            pos[2] += world[pi][2];
            p = if pi < i { None } else { joints[pi].parent };
            if pi < i {
                break;
            }
            depth += 1;
        }
        if let Some(pi) = joints[i].parent
            && pi >= i
        {
            // Recompute cleanly if parent appeared later than child
            let mut cur = Some(i);
            let mut sum = [0.0f32; 3];
            let mut hop = 0usize;
            while let Some(c) = cur
                && hop < 128
            {
                sum[0] += joints[c].local_translation[0];
                sum[1] += joints[c].local_translation[1];
                sum[2] += joints[c].local_translation[2];
                cur = joints[c].parent;
                hop += 1;
            }
            pos = sum;
        }
        world[i] = pos;
    }
    world
}

fn geometric_prior(h_idx: usize, wpos: [f32; 3], hips_pos: [f32; 3], head_y: f32) -> f32 {
    let dx = wpos[0] - hips_pos[0];
    let side = canonical_chirality(h_idx);
    let side_ok = match side {
        Chirality::Left => dx >= -0.02,
        Chirality::Right => dx <= 0.02,
        Chirality::Center => dx.abs() <= 0.12,
    };
    if !side_ok {
        return 0.0;
    }
    let y_norm = if head_y > hips_pos[1] + 0.1 {
        ((wpos[1] - hips_pos[1]) / (head_y - hips_pos[1])).clamp(-1.5, 1.5)
    } else {
        0.5
    };
    match h_idx {
        0 => 1.0 - y_norm.abs().min(1.0),
        1..=3 => {
            if (0.05..=0.85).contains(&y_norm) {
                0.9
            } else {
                0.4
            }
        }
        4..=8 => {
            if y_norm >= 0.75 {
                0.95
            } else {
                0.3
            }
        }
        9..=16 => {
            if y_norm <= 0.05 {
                0.95
            } else {
                0.2
            }
        }
        17..=54 => {
            if (0.35..=1.05).contains(&y_norm) {
                0.9
            } else {
                0.4
            }
        }
        _ => 0.5,
    }
}

/// Kuhn-Munkres (Hungarian) maximum weight bipartite matching (`R` rows x `C` cols, `R <= C` or general).
/// Returns assigned column `Option<usize>` per row maximizing sum of scores in `[0.0, 1.0]`.
fn hungarian_maximize(scores: &[Vec<f32>]) -> Vec<Option<usize>> {
    let rows = scores.len();
    if rows == 0 {
        return Vec::new();
    }
    let cols = scores[0].len();
    if cols == 0 {
        return vec![None; rows];
    }

    // Pad square matrix of size `n = max(rows, cols)` with 0 cost slack columns if needed
    let n = rows.max(cols);
    let mut u = vec![0.0f64; n + 1];
    let mut v = vec![0.0f64; n + 1];
    let mut p = vec![0usize; n + 1];
    let mut way = vec![0usize; n + 1];

    for i in 1..=rows {
        p[0] = i;
        let mut j0 = 0usize;
        let mut minv = vec![f64::INFINITY; n + 1];
        let mut used = vec![false; n + 1];

        loop {
            used[j0] = true;
            let i0 = p[j0];
            let mut delta = f64::INFINITY;
            let mut j1 = 0usize;

            for j in 1..=n {
                if !used[j] {
                    let s = if i0 <= rows && j <= cols {
                        f64::from(scores[i0 - 1][j - 1])
                    } else {
                        0.0
                    };
                    let cur = -s - u[i0] - v[j];
                    if cur < minv[j] {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if minv[j] < delta {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }
            for j in 0..=n {
                if used[j] {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
            if p[j0] == 0 {
                break;
            }
        }

        loop {
            let j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
            if j0 == 0 {
                break;
            }
        }
    }

    let mut result = vec![None; rows];
    for (col_1based, &row_1based) in p.iter().enumerate().skip(1) {
        if row_1based >= 1 && row_1based <= rows && col_1based <= cols {
            result[row_1based - 1] = Some(col_1based - 1);
        }
    }
    result
}

/// Output of humanoid skeleton mapping (`§6.4`).
pub struct HumanoidMatchResult {
    pub humanoid_to_joint: [u16; 55],
    pub bone_match_score: [f32; 55],
    pub joint_humanoid: Vec<u16>,
    pub joint_regions: Vec<u16>,
    pub head_joint: u32,
    pub warnings_mask: u32,
}

/// Solves 1:1 Appendix A humanoid mapping + assigns §6.7 per-joint depth-flatten regions.
pub fn match_humanoid_bones(joints: &[RawJoint]) -> HumanoidMatchResult {
    let mut humanoid_to_joint = [0xFFFFu16; 55];
    let mut bone_match_score = [0.0f32; 55];
    let mut joint_humanoid = vec![0xFFFFu16; joints.len()];

    if joints.is_empty() {
        return HumanoidMatchResult {
            humanoid_to_joint,
            bone_match_score,
            joint_humanoid,
            joint_regions: Vec::new(),
            head_joint: 0,
            warnings_mask: AETHER_WARN_UNMAPPED_REQUIRED,
        };
    }

    // Step 1: Honor explicit VRM bindings first (1.0 confidence)
    let mut used_joint = vec![false; joints.len()];
    let mut unmatched_hum = Vec::new();
    for h in 0..AETHER_HUMANOID_BONE_COUNT as usize {
        if let Some(j_idx) = joints
            .iter()
            .position(|j| j.explicit_humanoid == Some(h as u16) && !used_joint[j.ordinal])
        {
            humanoid_to_joint[h] = j_idx as u16;
            bone_match_score[h] = 1.0;
            joint_humanoid[j_idx] = h as u16;
            used_joint[j_idx] = true;
        } else {
            unmatched_hum.push(h);
        }
    }

    // Step 2: Hungarian fuzzy matching for any remaining unmapped Appendix A slots
    if !unmatched_hum.is_empty() {
        let free_joints: Vec<usize> = (0..joints.len()).filter(|&j| !used_joint[j]).collect();
        if !free_joints.is_empty() {
            let wpos = world_positions(joints);
            let hips_pos = joints
                .iter()
                .find(|j| {
                    let l = j.name.to_ascii_lowercase();
                    l.contains("hip") || l.contains("pelvis") || j.name.contains("下半身")
                })
                .map_or(wpos[0], |j| wpos[j.ordinal]);
            let max_y = wpos.iter().map(|p| p[1]).fold(f32::NEG_INFINITY, f32::max);
            let max_ord = (joints.len().max(1)) as f32;

            let conv = detect_convention(joints);
            let mut matrix = vec![vec![0.0f32; free_joints.len()]; unmatched_hum.len()];
            for (r, &h_idx) in unmatched_hum.iter().enumerate() {
                let canon_parent_name =
                    CANONICAL_PARENT[h_idx].map(|p| CANONICAL_55_NAMES[p as usize]);
                for (c, &j_idx) in free_joints.iter().enumerate() {
                    let t_score = token_similarity(h_idx, &joints[j_idx], conv);
                    if t_score <= 0.05 {
                        continue;
                    }
                    let h_score = match (joints[j_idx].parent, canon_parent_name) {
                        (None, None) => 1.0,
                        (Some(jp), None) if h_idx == 0 && is_mmd_cog_ancestor(&joints[jp].name) => {
                            1.0
                        }
                        (Some(jp), Some(_))
                            if h_idx == 1 && is_mmd_cog_ancestor(&joints[jp].name) =>
                        {
                            1.0
                        }
                        (Some(jp), Some(cp_name)) => {
                            let c_p_idx = CANONICAL_PARENT[h_idx].unwrap_or(0) as usize;
                            if humanoid_to_joint[c_p_idx] == jp as u16 {
                                1.0
                            } else {
                                token_similarity(c_p_idx, &joints[jp], conv).max(
                                    if joints[jp]
                                        .name
                                        .to_ascii_lowercase()
                                        .contains(&cp_name.to_ascii_lowercase())
                                    {
                                        0.8
                                    } else {
                                        0.3
                                    },
                                )
                            }
                        }
                        _ => 0.4,
                    };
                    let g_score = geometric_prior(h_idx, wpos[j_idx], hips_pos, max_y);
                    let ord_score = 1.0 - (joints[j_idx].ordinal as f32 / max_ord).min(1.0);
                    let combined =
                        0.50 * t_score + 0.20 * h_score + 0.20 * g_score + 0.10 * ord_score;
                    matrix[r][c] = combined.clamp(0.0, 1.0);
                }
            }

            let assignment = hungarian_maximize(&matrix);
            for (r, opt_c) in assignment.into_iter().enumerate() {
                if let Some(c) = opt_c {
                    let score = matrix[r][c];
                    let h_idx = unmatched_hum[r];
                    let j_idx = free_joints[c];
                    bone_match_score[h_idx] = score;
                    if score >= 0.55 {
                        humanoid_to_joint[h_idx] = j_idx as u16;
                        joint_humanoid[j_idx] = h_idx as u16;
                    }
                }
            }
        }
    }

    // Step 3: Classify review/required warnings
    let mut warnings_mask = 0u32;
    for (h, &score) in bone_match_score.iter().enumerate() {
        if humanoid_to_joint[h] != 0xFFFF && (0.55..0.80).contains(&score) {
            warnings_mask |= AETHER_WARN_REVIEW_BONES;
        }
    }
    for &req_slot in &REQUIRED_CORE_SLOTS {
        if humanoid_to_joint[req_slot as usize] == 0xFFFF {
            warnings_mask |= AETHER_WARN_UNMAPPED_REQUIRED;
            break;
        }
    }

    // Determine best head joint (fallback to highest Y joint if head unmapped)
    let head_joint = if humanoid_to_joint[5] == 0xFFFF {
        let wpos = world_positions(joints);
        wpos.iter()
            .enumerate()
            .max_by(|a, b| a.1[1].total_cmp(&b.1[1]))
            .map_or(0, |(idx, _)| idx as u32)
    } else {
        u32::from(humanoid_to_joint[5])
    };

    // Step 4: Assign §6.7 depth-flatten region tags via humanoid mapping + ancestor walk + hair heuristic
    let mut joint_regions = vec![AETHER_JOINT_REGION_OTHER; joints.len()];
    let mut hum_lookup: HashMap<usize, u16> = HashMap::new();
    for (h, &j) in humanoid_to_joint.iter().enumerate() {
        if j != 0xFFFF {
            hum_lookup.insert(usize::from(j), h as u16);
        }
    }

    for j_idx in 0..joints.len() {
        let lname = joints[j_idx].name.to_ascii_lowercase();
        if lname.contains("hair") || joints[j_idx].name.contains("髪") {
            joint_regions[j_idx] = AETHER_JOINT_REGION_HAIR;
            continue;
        }
        let mut cur = Some(j_idx);
        let mut hops = 0usize;
        let mut assigned = AETHER_JOINT_REGION_TORSO_ARMS;
        while let Some(c) = cur
            && hops < 64
        {
            if let Some(&h) = hum_lookup.get(&c) {
                assigned = match h {
                    5..=8 => AETHER_JOINT_REGION_HEAD,
                    3 | 4 => AETHER_JOINT_REGION_NECK_UPPER,
                    0 | 9..=16 => AETHER_JOINT_REGION_HIPS_LEGS,
                    _ => AETHER_JOINT_REGION_TORSO_ARMS,
                };
                break;
            }
            cur = joints[c].parent;
            hops += 1;
        }
        joint_regions[j_idx] = assigned;
    }

    HumanoidMatchResult {
        humanoid_to_joint,
        bone_match_score,
        joint_humanoid,
        joint_regions,
        head_joint,
        warnings_mask,
    }
}
