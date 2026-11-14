//! Coordinate/scale/rest-pose normalisation (`§6.3`), expression retarget `W` (`§6.5`),
//! and SpringBone / MMD Bullet chain conversion (`§6.6`).

use aether_ffi::{AETHER_EXPRESSION_SLOT_COUNT, AETHER_INVALID_INDEX, AETHER_WARN_UNIT_GUESS};
use glam::{Mat4, Quat, Vec3};
use unicode_normalization::UnicodeNormalization;

use crate::bone_matcher::HumanoidMatchResult;
use crate::ir::ParsedAvatar;

/// Output expression binding prior to UTF-8 string offset patching.
#[derive(Clone, Debug)]
pub struct ResolvedExprBinding {
    pub preset_index: u32,
    pub input_slot: u32,
    pub morph_index: u32,
    pub weight: f32,
    pub preset_name: String,
}

/// Output normalised joint metrics computed in §6.3.
#[derive(Clone, Copy, Debug)]
pub struct NormalisedJointPose {
    pub bind_translation: [f32; 3],
    pub bind_rotation_quat: [f32; 4],
    pub bind_scale: [f32; 3],
    pub tail_length: f32,
    pub rest_to_tpose_quat: [f32; 4],
    pub inverse_bind_matrix_row_major: [[f32; 4]; 3],
    pub world_pos: [f32; 3],
}

/// Computes world positions of joints in the skeleton.
pub fn compute_joint_world_positions(joints: &[crate::ir::RawJoint]) -> Vec<Vec3> {
    fn eval_node(
        idx: usize,
        joints: &[crate::ir::RawJoint],
        world_mats: &mut [Mat4],
        computed: &mut [bool],
        depth: usize,
    ) -> Mat4 {
        if computed[idx] || depth > 128 {
            return world_mats[idx];
        }
        let j = &joints[idx];
        let t = Vec3::from_array(j.local_translation);
        let q = Quat::from_xyzw(
            j.local_rotation[0],
            j.local_rotation[1],
            j.local_rotation[2],
            j.local_rotation[3],
        )
        .normalize();
        let s = Vec3::from_array(j.local_scale);
        let local = Mat4::from_scale_rotation_translation(s, q, t);
        let m = match j.parent {
            Some(p) if p < joints.len() && p != idx => {
                eval_node(p, joints, world_mats, computed, depth + 1) * local
            }
            _ => local,
        };
        world_mats[idx] = m;
        computed[idx] = true;
        m
    }

    let n = joints.len();
    let mut world_mats = vec![Mat4::IDENTITY; n];
    let mut computed = vec![false; n];

    for i in 0..n {
        eval_node(i, joints, &mut world_mats, &mut computed, 0);
    }
    world_mats
        .into_iter()
        .map(|m| m.w_axis.truncate())
        .collect()
}

/// Normalises units (height -> ~[0.3, 3.0] m, ground feet `y = 0`), A-pose -> T-pose delta quaternions,
/// and synthesises `3x4` inverse bind matrices (`§6.3`).
pub fn normalise_avatar_pose(
    avatar: &mut ParsedAvatar,
    matched: &HumanoidMatchResult,
) -> (Vec<NormalisedJointPose>, [f32; 3], [f32; 3], f32) {
    // 1. Measure raw AABB height across both mesh vertices and skeleton joints so head/bust meshes on a full rig don't trigger a false 10x scale guess.
    let (mut min_p, mut max_p) = compute_aabb(&avatar.vertices);
    let mut joint_min_y = f32::INFINITY;
    let mut joint_max_y = f32::NEG_INFINITY;
    for wp in compute_joint_world_positions(&avatar.joints) {
        if wp.y.is_finite() {
            joint_min_y = joint_min_y.min(wp.y);
            joint_max_y = joint_max_y.max(wp.y);
        }
    }
    let raw_min_y = if joint_min_y.is_finite() {
        min_p[1].min(joint_min_y)
    } else {
        min_p[1]
    };
    let raw_max_y = if joint_max_y.is_finite() {
        max_p[1].max(joint_max_y)
    } else {
        max_p[1]
    };
    let raw_height = (raw_max_y - raw_min_y).max(1e-5);

    // Infer power-of-10 unit multiplier if model falls far outside `[0.3, 3.0]` m (§6.3)
    if !(0.30..=3.0).contains(&raw_height) {
        let mut best_scale = 1.0f32;
        let mut best_dist = f32::INFINITY;
        for pow in -3i32..=3 {
            let cand = 10.0f32.powi(pow);
            let h = raw_height * cand;
            if (0.35..=2.8).contains(&h) {
                let d = (h - 1.60).abs();
                if d < best_dist {
                    best_dist = d;
                    best_scale = cand;
                }
            }
        }
        let scale = if best_dist.is_finite() {
            best_scale
        } else {
            1.60 / raw_height
        };
        avatar.warnings_mask |= AETHER_WARN_UNIT_GUESS;
        avatar.unit_meta.scale_guessed = true;
        avatar.unit_meta.resolved_unit_scale *= scale;

        for v in &mut avatar.vertices {
            v.position[0] *= scale;
            v.position[1] *= scale;
            v.position[2] *= scale;
        }
        for m in &mut avatar.morphs {
            m.centroid_x *= scale;
            for d in &mut m.deltas {
                d.d_pos[0] *= scale;
                d.d_pos[1] *= scale;
                d.d_pos[2] *= scale;
            }
        }
        for j in &mut avatar.joints {
            j.local_translation[0] *= scale;
            j.local_translation[1] *= scale;
            j.local_translation[2] *= scale;
            if let Some(ref mut ibm) = j.inverse_bind_matrix {
                ibm[3] *= scale;
                ibm[7] *= scale;
                ibm[11] *= scale;
            }
        }
        for c in &mut avatar.colliders {
            c.radius *= scale;
            c.offset = [
                c.offset[0] * scale,
                c.offset[1] * scale,
                c.offset[2] * scale,
            ];
            c.tail = [c.tail[0] * scale, c.tail[1] * scale, c.tail[2] * scale];
        }
        for sp in &mut avatar.spring_chains {
            for node in &mut sp.joints {
                node.hit_radius *= scale;
            }
        }
        (min_p, max_p) = compute_aabb(&avatar.vertices);
    }

    // 2. Ground feet so lowest point of skeleton + mesh sits at `y = 0` without detaching bust meshes from head bones
    let mut grounded_joint_min_y = f32::INFINITY;
    let mut grounded_joint_max_y = f32::NEG_INFINITY;
    for wp in compute_joint_world_positions(&avatar.joints) {
        if wp.y.is_finite() {
            grounded_joint_min_y = grounded_joint_min_y.min(wp.y);
            grounded_joint_max_y = grounded_joint_max_y.max(wp.y);
        }
    }
    let combined_min_y = if grounded_joint_min_y.is_finite() {
        min_p[1].min(grounded_joint_min_y)
    } else {
        min_p[1]
    };
    let combined_max_y = if grounded_joint_max_y.is_finite() {
        max_p[1].max(grounded_joint_max_y)
    } else {
        max_p[1]
    };
    let floor_y = combined_min_y;
    if floor_y.abs() > 1e-4 {
        for v in &mut avatar.vertices {
            v.position[1] -= floor_y;
        }
        for j in &mut avatar.joints {
            if j.parent.is_none() {
                j.local_translation[1] -= floor_y;
            }
            if let Some(ref mut ibm) = j.inverse_bind_matrix {
                ibm[7] += floor_y;
            }
        }
        min_p[1] -= floor_y;
        max_p[1] -= floor_y;
    }

    let height_m = (combined_max_y - combined_min_y).max(0.05);

    // 3. Build world transforms, tail lengths, inverse bind matrices & A-pose -> T-pose delta quaternions
    let world_pos = compute_joint_world_positions(&avatar.joints);
    let n = avatar.joints.len();
    let mut world_mats = vec![Mat4::IDENTITY; n];
    let mut child_first = vec![None::<usize>; n];
    for (i, j) in avatar.joints.iter().enumerate() {
        if let Some(p) = j.parent
            && p < n
            && child_first[p].is_none()
        {
            child_first[p] = Some(i);
        }
    }

    // Compute world matrices properly via iterative FK (handle unordered parent indices safely)
    let mut done = vec![false; n];
    for pass in 0..n {
        let mut progressed = false;
        for i in 0..n {
            if done[i] {
                continue;
            }
            let p_mat = match avatar.joints[i].parent {
                Some(p) if p < n && p != i => {
                    if !done[p] && pass + 1 < n {
                        continue;
                    }
                    world_mats[p]
                }
                _ => Mat4::IDENTITY,
            };
            let j = &avatar.joints[i];
            let q = Quat::from_xyzw(
                j.local_rotation[0],
                j.local_rotation[1],
                j.local_rotation[2],
                j.local_rotation[3],
            )
            .normalize();
            let local = Mat4::from_scale_rotation_translation(
                Vec3::from_array(j.local_scale),
                q,
                Vec3::from_array(j.local_translation),
            );
            world_mats[i] = p_mat * local;
            done[i] = true;
            progressed = true;
        }
        if !progressed {
            break;
        }
    }

    // A-pose -> T-pose delta quaternion for upper/lower arms (§6.3)
    let mut rest_to_tpose = vec![Quat::IDENTITY; n];
    let pairs = [
        (18usize, 19usize, Vec3::X),  // leftUpperArm -> leftLowerArm -> +X
        (19usize, 20usize, Vec3::X),  // leftLowerArm -> leftHand -> +X
        (22usize, 23usize, -Vec3::X), // rightUpperArm -> rightLowerArm -> -X
        (23usize, 24usize, -Vec3::X), // rightLowerArm -> rightHand -> -X
    ];
    for (h_from, h_to, target_dir) in pairs {
        let jf = matched.humanoid_to_joint[h_from];
        let jt = matched.humanoid_to_joint[h_to];
        if jf != 0xFFFF && jt != 0xFFFF {
            let a = usize::from(jf);
            let b = usize::from(jt);
            let delta = world_pos[b] - world_pos[a];
            if delta.length() > 1e-4 {
                let dir = delta.normalize();
                if dir.dot(target_dir) < 0.99 {
                    rest_to_tpose[a] = Quat::from_rotation_arc(dir, target_dir).normalize();
                }
            }
        }
    }

    let mut poses = Vec::with_capacity(n);
    for (i, j) in avatar.joints.iter().enumerate() {
        let tail_length = match child_first[i] {
            Some(c) => (world_pos[c] - world_pos[i]).length().max(0.05),
            None => 0.05,
        };
        let ibm_4x4 = if let Some(m) = j.inverse_bind_matrix {
            m
        } else {
            let inv = world_mats[i].inverse();
            let r0 = inv.row(0);
            let r1 = inv.row(1);
            let r2 = inv.row(2);
            [
                r0.x, r0.y, r0.z, r0.w, r1.x, r1.y, r1.z, r1.w, r2.x, r2.y, r2.z, r2.w, 0.0, 0.0,
                0.0, 1.0,
            ]
        };
        let rq = rest_to_tpose[i];
        poses.push(NormalisedJointPose {
            bind_translation: j.local_translation,
            bind_rotation_quat: j.local_rotation,
            bind_scale: j.local_scale,
            tail_length,
            rest_to_tpose_quat: [rq.x, rq.y, rq.z, rq.w],
            inverse_bind_matrix_row_major: [
                [ibm_4x4[0], ibm_4x4[1], ibm_4x4[2], ibm_4x4[3]],
                [ibm_4x4[4], ibm_4x4[5], ibm_4x4[6], ibm_4x4[7]],
                [ibm_4x4[8], ibm_4x4[9], ibm_4x4[10], ibm_4x4[11]],
            ],
            world_pos: world_pos[i].to_array(),
        });
    }

    (poses, min_p, max_p, height_m)
}

fn compute_aabb(vertices: &[crate::ir::RawVertex]) -> ([f32; 3], [f32; 3]) {
    if vertices.is_empty() {
        return ([0.0, 0.0, 0.0], [0.0, 1.6, 0.0]);
    }
    let mut min_p = [f32::INFINITY; 3];
    let mut max_p = [f32::NEG_INFINITY; 3];
    for v in vertices {
        for k in 0..3 {
            min_p[k] = min_p[k].min(v.position[k]);
            max_p[k] = max_p[k].max(v.position[k]);
        }
    }
    (min_p, max_p)
}

fn map_preset_to_slots(name: &str, centroid_x: f32) -> Vec<(u32, f32)> {
    const ARKIT_LOWER: [&str; 52] = [
        "eyeblinkleft",
        "eyelookdownleft",
        "eyelookinleft",
        "eyelookoutleft",
        "eyelookupleft",
        "eyesquintleft",
        "eyewideleft",
        "eyeblinkright",
        "eyelookdownright",
        "eyelookinright",
        "eyelookoutright",
        "eyelookupright",
        "eyesquintright",
        "eyewideright",
        "jawforward",
        "jawleft",
        "jawright",
        "jawopen",
        "mouthclose",
        "mouthfunnel",
        "mouthpucker",
        "mouthleft",
        "mouthright",
        "mouthsmileleft",
        "mouthsmileright",
        "mouthfrownleft",
        "mouthfrownright",
        "mouthdimpleleft",
        "mouthdimpleright",
        "mouthstretchleft",
        "mouthstretchright",
        "mouthrolllower",
        "mouthrollupper",
        "mouthshruglower",
        "mouthshrugupper",
        "mouthpressleft",
        "mouthpressright",
        "mouthlowerdownleft",
        "mouthlowerdownright",
        "mouthupperupleft",
        "mouthupperupright",
        "browdownleft",
        "browdownright",
        "browinnerup",
        "browouterupleft",
        "browouterupright",
        "cheekpuff",
        "cheeksquintleft",
        "cheeksquintright",
        "nosesneerleft",
        "nosesneerright",
        "tongueout",
    ];
    let nfkc: String = name.nfkc().collect();
    let trimmed = nfkc.trim();
    let lower = trimmed.to_ascii_lowercase();

    // Direct ARKit 52 names (exact slot order matching Appendix B)
    let stripped: String = lower.chars().filter(char::is_ascii_alphanumeric).collect();
    for (idx, &ark) in ARKIT_LOWER.iter().enumerate() {
        if stripped == ark {
            return vec![(idx as u32, 1.0)];
        }
    }
    // Check `_l` / `_r` / `left` / `right` ARKit permutations
    let has_r = lower.ends_with("_r")
        || std::path::Path::new(&lower)
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("r"))
        || lower.contains("right")
        || trimmed.contains('右');
    let has_l = lower.ends_with("_l")
        || std::path::Path::new(&lower)
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("l"))
        || lower.contains("left")
        || trimmed.contains('左');

    // Visemes 52..56: A, I, U, E, O
    match trimmed {
        "あ" | "a" | "aa" | "vis_aa" | "viseme_a" | "vrc.v_aa" => return vec![(52, 1.0)],
        "い" | "i" | "ih" | "vis_ih" | "viseme_i" | "vrc.v_ih" => return vec![(53, 1.0)],
        "う" | "u" | "ou" | "vis_ou" | "viseme_u" | "vrc.v_ou" => return vec![(54, 1.0)],
        "え" | "e" | "ee" | "vis_ee" | "viseme_e" | "vrc.v_ee" => return vec![(55, 1.0)],
        "お" | "o" | "oh" | "vis_oh" | "viseme_o" | "vrc.v_oh" => return vec![(56, 1.0)],
        _ => {}
    }

    // Japanese & VRM 0/1 expressions + §6.5 ambiguous wink centroid disambiguation!
    if trimmed == "まばたき" || lower == "blink" || lower == "eyesclosed" {
        return vec![(0, 1.0), (7, 1.0)];
    }
    if lower == "blinkleft" || lower == "blink_l" {
        return vec![(0, 1.0)];
    }
    if lower == "blinkright" || lower == "blink_r" {
        return vec![(7, 1.0)];
    }
    if trimmed.contains("ウィンク") || trimmed.contains("ウインク") || lower.contains("wink")
    {
        if has_r && !has_l {
            return vec![(7, 1.0)];
        }
        if has_l && !has_r {
            return vec![(0, 1.0)];
        }
        // Ambiguous wink -> use morph vertex centroid X (§6.5: +X is anatomical left on RH front-facing model)
        return if centroid_x >= 0.0 {
            vec![(0, 1.0)]
        } else {
            vec![(7, 1.0)]
        };
    }
    if lower.contains("mouthopen") || lower.contains("jawopen") {
        return vec![(17, 1.0)];
    }
    if lower.contains("smile") || lower == "joy" || lower == "happy" || trimmed == "笑い" {
        return vec![(23, 1.0), (24, 1.0)];
    }
    if lower == "angry" || trimmed.contains("怒") {
        return vec![(41, 0.8), (42, 0.8), (57, 1.0)];
    }
    if lower == "sorrow" || lower == "sad" || trimmed.contains("哀") || trimmed.contains("困") {
        return vec![(43, 0.9), (25, 0.6), (26, 0.6), (58, 1.0)];
    }

    // Fallback: user custom expression slot in 57..63 (<= AETHER_EXPRESSION_SLOT_COUNT)
    vec![(57, 1.0)]
}

/// Builds the sparse Appendix B retarget matrix `W` (`§6.5`), sorted by `(input_slot, morph_index)`.
pub fn build_expression_retarget(avatar: &ParsedAvatar) -> Vec<ResolvedExprBinding> {
    let mut out = Vec::new();
    for (p_idx, preset) in avatar.expr_presets.iter().enumerate() {
        for &(m_idx, w) in &preset.morph_binds {
            let cx = avatar.morphs.get(m_idx).map_or(0.0, |m| m.centroid_x);
            for (slot, slot_w) in map_preset_to_slots(&preset.name, cx) {
                if slot < AETHER_EXPRESSION_SLOT_COUNT {
                    out.push(ResolvedExprBinding {
                        preset_index: p_idx as u32,
                        input_slot: slot,
                        morph_index: m_idx as u32,
                        weight: (w * slot_w).clamp(-2.0, 2.0),
                        preset_name: preset.name.clone(),
                    });
                }
            }
        }
    }
    out.sort_by_key(|b| (b.input_slot, b.morph_index, b.preset_index));
    out
}

/// Packs SpringBone / migrated MMD chains depth-first into FFI descriptors (`§6.6`).
pub fn build_spring_descriptors(
    avatar: &ParsedAvatar,
    poses: &[NormalisedJointPose],
) -> (
    Vec<aether_ffi::AetherSpringJointDesc>,
    Vec<aether_ffi::AetherColliderDesc>,
) {
    let mut springs = Vec::new();
    let mut colliders = Vec::new();

    for c in avatar.colliders.iter().take(crate::limits::MAX_COLLIDERS) {
        colliders.push(aether_ffi::AetherColliderDesc {
            joint_index: if c.joint_index < poses.len() {
                c.joint_index as u32
            } else {
                AETHER_INVALID_INDEX
            },
            group_bit: c.group_bit,
            shape_kind: c.shape_kind,
            radius: c.radius.max(0.005),
            offset: c.offset,
            _pad0: 0.0,
            tail: c.tail,
            _pad1: 0.0,
        });
    }

    for (chain_id, chain) in avatar.spring_chains.iter().enumerate() {
        let chain_start = springs.len();
        for (depth, node) in chain.joints.iter().enumerate() {
            if springs.len() >= crate::limits::MAX_SPRING_JOINTS {
                break;
            }
            if node.joint_index >= poses.len() {
                continue;
            }
            let p_cur = Vec3::from_array(poses[node.joint_index].world_pos);
            let (rest_dir, bone_len) = match chain.joints.get(depth + 1) {
                Some(nxt) if nxt.joint_index < poses.len() => {
                    let delta = Vec3::from_array(poses[nxt.joint_index].world_pos) - p_cur;
                    let len = delta.length();
                    if len > 1e-4 {
                        ((delta / len).to_array(), len)
                    } else {
                        (
                            [0.0, -1.0, 0.0],
                            poses[node.joint_index].tail_length.max(0.05),
                        )
                    }
                }
                _ => (
                    [0.0, -1.0, 0.0],
                    poses[node.joint_index].tail_length.max(0.05),
                ),
            };
            let parent_spring_index = if depth == 0 {
                -1
            } else {
                (chain_start + depth - 1) as i32
            };
            springs.push(aether_ffi::AetherSpringJointDesc {
                collider_group_mask: chain.collider_group_mask,
                gravity_dir: node.gravity_dir,
                hit_radius: node.hit_radius.max(0.005),
                rest_dir,
                bone_length: bone_len,
                joint_index: node.joint_index as u32,
                parent_spring_index,
                chain_id: chain_id as u32,
                depth_level: depth as u32,
                center_joint_index: chain.center_joint.map_or(-1, |c| c as i32),
                stiffness: node.stiffness.clamp(0.01, 10.0),
                drag_force: node.drag_force.clamp(0.0, 1.0),
                gravity_power: node.gravity_power.clamp(0.0, 5.0),
                flags: u32::from(node.from_mmd),
                _pad: 0,
            });
        }
    }

    (springs, colliders)
}
