//! Rayon data-parallel geometry baker (`§6.7`):
//! - Skin weight top-8 normalization (`sum == 65535` exact)
//! - Seam-welded smooth outline tangents (`eps = 1e-5 * height` hashed cell)
//! - Spherized face proxy normals (`C_face`, 65% Y flatten, `smoothstep(0.4, 0.8, head_weight)`)
//! - `z_flatten_weight` per vertex (`<= 0.85`), `sdf_face_mask` with 1-ring boundary
//! - 30-bit Morton triangle sort (`par_sort_unstable_by_key`)

use aether_ffi::{
    AETHER_JOINT_REGION_HEAD, AETHER_JOINT_REGION_HIPS_LEGS, AETHER_JOINT_REGION_NECK_UPPER,
    AetherMorphDelta, AetherMorphRangeDesc, AetherSubmeshDesc, AetherVertex25D, AetherVertexSkin,
    AetherVertexSurface,
};
use ahash::AHashMap;
use glam::Vec3;
use rayon::prelude::*;

use crate::bone_matcher::HumanoidMatchResult;
use crate::ir::ParsedAvatar;
use crate::materials::classify_submesh_flags;
use crate::retarget::NormalisedJointPose;

/// Normalizes 8 bone influences descending into `[u16; 8]` summing to **exactly `65535`**.
#[must_use]
pub fn pack_skin_weights(joints: [u16; 8], weights: [f32; 8]) -> AetherVertexSkin {
    let mut pairs = [(0u16, 0.0f32); 8];
    for k in 0..8 {
        let w = if weights[k].is_finite() && weights[k] > 0.0 {
            weights[k]
        } else {
            0.0
        };
        pairs[k] = (joints[k], w);
    }
    pairs.sort_by(|a, b| b.1.total_cmp(&a.1));

    let total: f32 = pairs.iter().map(|p| p.1).sum();
    let mut out_j = [0u16; 8];
    let mut out_w = [0u16; 8];
    if total <= 1e-8 {
        out_j[0] = pairs[0].0;
        out_w[0] = 65_535;
        return AetherVertexSkin {
            joints: out_j,
            weights_unorm16: out_w,
        };
    }

    let mut sum_u16 = 0u32;
    for k in 0..8 {
        out_j[k] = pairs[k].0;
        let q = ((pairs[k].1 / total) * 65_535.0)
            .round()
            .clamp(0.0, 65_535.0) as u16;
        out_w[k] = q;
        sum_u16 += u32::from(q);
    }
    if sum_u16 != 65_535 {
        let diff = 65_535i32 - (sum_u16 as i32);
        let adjusted = (i32::from(out_w[0]) + diff).clamp(0, 65_535) as u16;
        out_w[0] = adjusted;
    }

    AetherVertexSkin {
        joints: out_j,
        weights_unorm16: out_w,
    }
}

/// Encodes a unit vector into 2x `i8` octahedral coordinates + parity bit `[i8; 2]`.
#[inline]
#[must_use]
pub fn oct_encode(v: [f32; 3]) -> [i8; 2] {
    let n = Vec3::from_array(v).normalize_or(Vec3::Z);
    let denom = n.x.abs() + n.y.abs() + n.z.abs();
    let (mut ox, mut oy) = (n.x / denom.max(1e-6), n.y / denom.max(1e-6));
    if n.z < 0.0 {
        let sx = ox;
        let sy = oy;
        ox = (1.0 - sy.abs()) * if sx >= 0.0 { 1.0 } else { -1.0 };
        oy = (1.0 - sx.abs()) * if sy >= 0.0 { 1.0 } else { -1.0 };
    }
    [
        (ox * 127.0).round().clamp(-127.0, 127.0) as i8,
        (oy * 127.0).round().clamp(-127.0, 127.0) as i8,
    ]
}

/// Encodes a unit vector into packed `2 x snorm16` octahedral coordinates (`AetherVertexSurface::tangent_oct`).
#[inline]
#[must_use]
pub fn oct_encode_snorm16_u32(v: [f32; 3]) -> u32 {
    let n = Vec3::from_array(v).normalize_or(Vec3::Z);
    let denom = n.x.abs() + n.y.abs() + n.z.abs();
    let (mut ox, mut oy) = (n.x / denom.max(1e-6), n.y / denom.max(1e-6));
    if n.z < 0.0 {
        let sx = ox;
        let sy = oy;
        ox = (1.0 - sy.abs()) * if sx >= 0.0 { 1.0 } else { -1.0 };
        oy = (1.0 - sx.abs()) * if sy >= 0.0 { 1.0 } else { -1.0 };
    }
    let ix = (ox * 32_767.0).round().clamp(-32_767.0, 32_767.0) as i16;
    let iy = (oy * 32_767.0).round().clamp(-32_767.0, 32_767.0) as i16;
    u32::from(ix as u16) | (u32::from(iy as u16) << 16)
}

/// Encodes a unit normal into biased `2 x 15-bit` octahedral coordinates with bit 31 set as validity flag.
#[inline]
#[must_use]
pub fn oct_encode_healed_valid_u32(v: [f32; 3]) -> u32 {
    let n = Vec3::from_array(v).normalize_or(Vec3::Z);
    let denom = n.x.abs() + n.y.abs() + n.z.abs();
    let (mut ox, mut oy) = (n.x / denom.max(1e-6), n.y / denom.max(1e-6));
    if n.z < 0.0 {
        let sx = ox;
        let sy = oy;
        ox = (1.0 - sy.abs()) * if sx >= 0.0 { 1.0 } else { -1.0 };
        oy = (1.0 - sx.abs()) * if sy >= 0.0 { 1.0 } else { -1.0 };
    }
    let ix = ((ox * 16_383.0).round().clamp(-16_383.0, 16_383.0) as i32 + 16_383) as u32;
    let iy = ((oy * 16_383.0).round().clamp(-16_383.0, 16_383.0) as i32 + 16_383) as u32;
    (1u32 << 31) | ((iy & 0x7FFFu32) << 15) | (ix & 0x7FFFu32)
}

#[inline]
fn smoothstep(edge0: f32, edge1: f32, x: f32) -> f32 {
    let t = ((x - edge0) / (edge1 - edge0)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

#[inline]
fn slerp_unit(a: Vec3, b: Vec3, t: f32) -> Vec3 {
    let na = a.normalize_or(Vec3::Z);
    let nb = b.normalize_or(na);
    let dot = na.dot(nb).clamp(-1.0, 1.0);
    if dot > 0.9995 || t <= 0.0 {
        return na.lerp(nb, t).normalize_or(na);
    }
    if dot < -0.9995 {
        return na.lerp(nb, t).normalize_or(Vec3::Y);
    }
    let theta = dot.acos() * t;
    let rel = (nb - na * dot).normalize_or(na);
    (na * theta.cos() + rel * theta.sin()).normalize_or(na)
}

/// Quantizes `p` into a 21-bit-per-axis spatial weld key (`u64`) using `eps = 1e-5 * height` (§6.7).
#[inline]
#[must_use]
pub fn quantize_weld_key(p: [f32; 3], eps: f32) -> u64 {
    let inv = 1.0 / eps.max(1e-7);
    let bias = 1_048_576i64; // 2^20
    let mask = 0x1F_FFFFu64;
    let qx = ((p[0] * inv).round() as i64 + bias).clamp(0, 0x1F_FFFF) as u64 & mask;
    let qy = ((p[1] * inv).round() as i64 + bias).clamp(0, 0x1F_FFFF) as u64 & mask;
    let qz = ((p[2] * inv).round() as i64 + bias).clamp(0, 0x1F_FFFF) as u64 & mask;
    qx | (qy << 21) | (qz << 42)
}

/// Computes 30-bit Morton code by bit-interleaving `[0, 1023]^3`.
#[inline]
#[must_use]
pub fn morton_30bit(cx: f32, cy: f32, cz: f32, aabb_min: [f32; 3], aabb_max: [f32; 3]) -> u32 {
    #[inline]
    fn expand_bits(mut v: u32) -> u32 {
        v = (v | (v << 16)) & 0x0300_00FF;
        v = (v | (v << 8)) & 0x0300_F00F;
        v = (v | (v << 4)) & 0x030C_30C3;
        v = (v | (v << 2)) & 0x0924_9249;
        v
    }
    let norm = |val: f32, lo: f32, hi: f32| -> u32 {
        let span = (hi - lo).max(1e-5);
        (((val - lo) / span).clamp(0.0, 1.0) * 1023.0) as u32
    };
    let x = norm(cx, aabb_min[0], aabb_max[0]);
    let y = norm(cy, aabb_min[1], aabb_max[1]);
    let z = norm(cz, aabb_min[2], aabb_max[2]);
    (expand_bits(x) << 2) | (expand_bits(y) << 1) | expand_bits(z)
}

/// Result summary for `bake_into_arena` returned alongside arena-filled data.
pub struct BakedSummary {
    pub face_center_head_local: [f32; 3],
    pub submesh_flags: Vec<u32>,
}

/// Performs parallel §6.7 mesh/skin/morph baking DIRECTLY into C++-lent arena slices!
#[allow(clippy::too_many_arguments)]
pub fn bake_into_arena(
    avatar: &ParsedAvatar,
    matched: &HumanoidMatchResult,
    poses: &[NormalisedJointPose],
    height_m: f32,
    morton_sort: bool,
    bake_proxy: bool,
    bake_smooth_tangent: bool,
    out_v25d: &mut [AetherVertex25D],
    out_surf: &mut [AetherVertexSurface],
    out_skin: &mut [AetherVertexSkin],
    out_indices: &mut [u32],
    out_morph_deltas: &mut [AetherMorphDelta],
    out_morph_ranges: &mut [AetherMorphRangeDesc],
    out_submeshes: &mut [AetherSubmeshDesc],
    string_buf: &mut Vec<u8>,
) -> BakedSummary {
    let v_count = avatar.vertices.len();
    let head_joint = matched.head_joint as usize;
    let head_world_pos = poses
        .get(head_joint)
        .map_or([0.0, height_m * 0.9, 0.0], |p| p.world_pos);
    let eye_level_y = head_world_pos[1] + 0.04 * (height_m / 1.6);

    // 1. Compute per-vertex head chain weight & `z_flatten_weight` in parallel
    let mut head_weights = vec![0.0f32; v_count];
    out_skin
        .par_iter_mut()
        .zip(head_weights.par_iter_mut())
        .zip(avatar.vertices.par_iter())
        .for_each(|((dst_skin, hw), rv)| {
            *dst_skin = pack_skin_weights(rv.joints, rv.weights);
            let mut h_w = 0.0f32;
            for k in 0..8 {
                let w = f32::from(dst_skin.weights_unorm16[k]) / 65_535.0;
                if w > 0.0 {
                    let j = usize::from(dst_skin.joints[k]);
                    let reg = matched.joint_regions.get(j).copied().unwrap_or(0);
                    if j == head_joint || reg == AETHER_JOINT_REGION_HEAD {
                        h_w += w;
                    }
                }
            }
            *hw = h_w.clamp(0.0, 1.0);
        });

    // 2. Detect face submeshes (`>= 60%` head weight >= 0.5 OR name hint) & accumulate face AABB
    let mut submesh_is_face = vec![false; avatar.submeshes.len()];
    let mut is_face_vert = vec![false; v_count];
    let mut face_min = [f32::INFINITY; 3];
    let mut face_max = [f32::NEG_INFINITY; 3];

    for (s_idx, sm) in avatar.submeshes.iter().enumerate() {
        let lower = sm.name.to_ascii_lowercase();
        let name_face = lower.contains("face")
            || lower.contains("head_skin")
            || sm.name.contains("顔")
            || avatar
                .materials
                .get(sm.material_index as usize)
                .is_some_and(|m| {
                    m.tex_sdf_face.is_some() || m.name.to_ascii_lowercase().contains("face")
                });
        let mut weighted_cnt = 0usize;
        let mut total_sampled = 0usize;
        for &vi in &sm.indices {
            if let Some(&hw) = head_weights.get(vi as usize) {
                total_sampled += 1;
                if hw >= 0.5 {
                    weighted_cnt += 1;
                }
            }
        }
        let ratio_face = total_sampled > 0 && (weighted_cnt * 100 >= total_sampled * 60);
        let is_face = (name_face
            || (ratio_face && !lower.contains("hair") && !lower.contains("body")))
            && !sm.is_eye_or_brow_hint;
        submesh_is_face[s_idx] = is_face;
        if is_face {
            for &vi in &sm.indices {
                if let Some(flag) = is_face_vert.get_mut(vi as usize) {
                    *flag = true;
                    let p = avatar.vertices[vi as usize].position;
                    for k in 0..3 {
                        face_min[k] = face_min[k].min(p[k]);
                        face_max[k] = face_max[k].max(p[k]);
                    }
                }
            }
        }
    }

    // Compute `C_face` in model space and head-local metres (§6.7)
    let c_face_ws = if face_min[0].is_finite() {
        let cx = f32::midpoint(face_min[0], face_max[0]);
        let depth = (face_max[2] - face_min[2]).max(0.06);
        let cy = eye_level_y.clamp(face_min[1], face_max[1]);
        let cz = face_max[2] - 0.55 * depth;
        Vec3::new(cx, cy, cz)
    } else {
        Vec3::new(head_world_pos[0], eye_level_y, head_world_pos[2] - 0.04)
    };
    let face_center_head_local = [
        c_face_ws.x - head_world_pos[0],
        c_face_ws.y - head_world_pos[1],
        c_face_ws.z - head_world_pos[2],
    ];

    // 3. `sdf_face_mask` 1-ring boundary fade across face vs non-face edges (§6.7)
    let mut sdf_mask = vec![0.0f32; v_count];
    for (i, &f) in is_face_vert.iter().enumerate() {
        sdf_mask[i] = if f { 1.0 } else { 0.0 };
    }
    let eps = 1e-5 * height_m;
    if is_face_vert.iter().any(|&b| b) {
        let mut cell_has_face: AHashMap<u64, u8> = AHashMap::with_capacity(v_count);
        for (i, rv) in avatar.vertices.iter().enumerate() {
            let key = quantize_weld_key(rv.position, eps);
            let bit = if is_face_vert[i] { 1 } else { 2 };
            *cell_has_face.entry(key).or_insert(0) |= bit;
        }
        // Any face triangle touching a vertex near a non-face vertex transitions mask -> 0
        let mut border_verts = vec![false; v_count];
        for (s_idx, sm) in avatar.submeshes.iter().enumerate() {
            for tri in sm.indices.chunks_exact(3) {
                let any_border = tri.iter().any(|&vi| {
                    let k = quantize_weld_key(avatar.vertices[vi as usize].position, eps);
                    cell_has_face.get(&k).copied().unwrap_or(0) == 3
                });
                if !submesh_is_face[s_idx] || any_border {
                    for &vi in tri {
                        let k = quantize_weld_key(avatar.vertices[vi as usize].position, eps);
                        if cell_has_face.get(&k).copied().unwrap_or(0) == 3 {
                            border_verts[vi as usize] = true;
                        }
                    }
                }
            }
        }
        for (i, &is_b) in border_verts.iter().enumerate() {
            if is_b {
                sdf_mask[i] = 0.0;
            }
        }
    }

    // 4. Seam-welded smooth outline tangents (`smooth_tangent`) + Rule-B safe coincident-vertex healed normals
    let mut smooth_accum: AHashMap<u64, Vec3> = AHashMap::with_capacity(v_count);
    if bake_smooth_tangent {
        for sm in &avatar.submeshes {
            for tri in sm.indices.chunks_exact(3) {
                let i0 = tri[0] as usize;
                let i1 = tri[1] as usize;
                let i2 = tri[2] as usize;
                let p0 = Vec3::from_array(avatar.vertices[i0].position);
                let p1 = Vec3::from_array(avatar.vertices[i1].position);
                let p2 = Vec3::from_array(avatar.vertices[i2].position);
                let face_n = (p1 - p0).cross(p2 - p0); // length = 2 * triangle area
                for &idx in &[i0, i1, i2] {
                    let k = quantize_weld_key(avatar.vertices[idx].position, eps);
                    smooth_accum
                        .entry(k)
                        .and_modify(|acc| *acc += face_n)
                        .or_insert(face_n);
                }
            }
        }
    }

    // Rule B coincident-vertex normal healing:
    // ONLY average normals within the SAME submesh & material, distance < 1e-4 m, dihedral < 50 deg (dot > 0.6427876).
    let mut vert_submesh = vec![usize::MAX; v_count];
    for (s_idx, sm) in avatar.submeshes.iter().enumerate() {
        for &vi in &sm.indices {
            let idx = vi as usize;
            if idx < v_count && vert_submesh[idx] == usize::MAX {
                vert_submesh[idx] = s_idx;
            }
        }
    }
    let true_normals: Vec<Vec3> = avatar
        .vertices
        .iter()
        .map(|rv| Vec3::from_array(rv.normal).normalize_or(Vec3::Y))
        .collect();
    let mut grid: AHashMap<(usize, i32, i32, i32), Vec<usize>> = AHashMap::with_capacity(v_count);
    for (i, rv) in avatar.vertices.iter().enumerate() {
        let s_idx = vert_submesh[i];
        if s_idx == usize::MAX {
            continue;
        }
        let gx = (rv.position[0] * 10_000.0).floor() as i32;
        let gy = (rv.position[1] * 10_000.0).floor() as i32;
        let gz = (rv.position[2] * 10_000.0).floor() as i32;
        grid.entry((s_idx, gx, gy, gz)).or_default().push(i);
    }
    let healed_normals: Vec<Vec3> = (0..v_count)
        .into_par_iter()
        .map(|i| {
            let n_i = true_normals[i];
            let s_idx = vert_submesh[i];
            if s_idx == usize::MAX {
                return n_i;
            }
            let mat_i = avatar.submeshes[s_idx].material_index;
            let p_i = Vec3::from_array(avatar.vertices[i].position);
            let gx = (p_i.x * 10_000.0).floor() as i32;
            let gy = (p_i.y * 10_000.0).floor() as i32;
            let gz = (p_i.z * 10_000.0).floor() as i32;
            let mut acc = n_i;
            for dz in -1..=1 {
                for dy in -1..=1 {
                    for dx in -1..=1 {
                        if let Some(bucket) = grid.get(&(s_idx, gx + dx, gy + dy, gz + dz)) {
                            for &j in bucket {
                                if j == i || avatar.submeshes[vert_submesh[j]].material_index != mat_i {
                                    continue;
                                }
                                let p_j = Vec3::from_array(avatar.vertices[j].position);
                                if (p_i - p_j).length_squared() < 1e-8 {
                                    let n_j = true_normals[j];
                                    if n_i.dot(n_j) > 0.642_787_6 {
                                        acc += n_j;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            acc.normalize_or(n_i)
        })
        .collect();

    // 5. Bake VERTEX25D + SURFACE in parallel via Rayon
    out_v25d
        .par_iter_mut()
        .zip(out_surf.par_iter_mut())
        .zip(avatar.vertices.par_iter().enumerate())
        .for_each(|((v25, vsurf), (i, rv))| {
            let tn = true_normals[i];

            // Region-weighted z_flatten_weight <= 0.85 (§6.7)
            let z_flat = if rv.flatten_override >= 0.0 {
                rv.flatten_override.clamp(0.0, 0.85)
            } else {
                let mut acc = 0.0f32;
                let sk = &out_skin[i];
                for k in 0..8 {
                    let w = f32::from(sk.weights_unorm16[k]) / 65_535.0;
                    if w > 0.0 {
                        let reg = matched
                            .joint_regions
                            .get(usize::from(sk.joints[k]))
                            .copied()
                            .unwrap_or(0);
                        let rw = match reg {
                            AETHER_JOINT_REGION_HEAD => 0.85,
                            AETHER_JOINT_REGION_NECK_UPPER => 0.70,
                            AETHER_JOINT_REGION_HIPS_LEGS => 0.20,
                            _ => 0.45,
                        };
                        acc += w * rw;
                    }
                }
                acc.clamp(0.0, 0.85)
            };

            // Spherized proxy normal ONLY on face vertices for analytic anime face shadow;
            // keep all body/clothing/hair normals 100% unmodified when Disguise Low-Poly Creases is OFF.
            let proxy = if bake_proxy && is_face_vert[i] {
                let mut d = Vec3::from_array(rv.position) - c_face_ws;
                d.y *= 0.35;
                let ellip = d.normalize_or(tn);
                let alpha = smoothstep(0.4, 0.8, head_weights[i]);
                slerp_unit(tn, ellip, alpha)
            } else {
                tn
            };

            // Seam-welded smooth tangent with degenerate fallback to true_normal (§6.7)
            let smooth = if bake_smooth_tangent {
                let key = quantize_weld_key(rv.position, eps);
                if let Some(&sum_n) = smooth_accum.get(&key) {
                    if sum_n.length() >= 1e-4 {
                        sum_n.normalize()
                    } else {
                        tn
                    }
                } else {
                    tn
                }
            } else {
                tn
            };

            *v25 = AetherVertex25D {
                position: rv.position,
                outline_width_scale: rv.edge_scale,
                true_normal: tn.to_array(),
                z_flatten_weight: z_flat,
                proxy_normal: proxy.to_array(),
                sdf_face_mask: sdf_mask[i],
                smooth_tangent: [smooth.x, smooth.y, smooth.z, 1.0],
            };

            let any_tangent = tn.any_orthonormal_vector().to_array();
            *vsurf = AetherVertexSurface {
                uv0: rv.uv0,
                uv1: rv.uv1,
                color_rgba8: u32::from_le_bytes(rv.color),
                tangent_oct: oct_encode_snorm16_u32(any_tangent),
                flags: u32::from(is_face_vert[i]),
                _reserved: oct_encode_healed_valid_u32(healed_normals[i].to_array()),
            };
        });

    // 6. Parallel 30-bit Morton triangle sort & pack submesh descriptors
    let mut submesh_flags = Vec::with_capacity(avatar.submeshes.len());
    let mut index_offset = 0u32;

    for (s_idx, sm) in avatar.submeshes.iter().enumerate() {
        let mut smin = [f32::INFINITY; 3];
        let mut smax = [f32::NEG_INFINITY; 3];
        let mut csum = Vec3::ZERO;
        let mut v_cnt = 0usize;
        let mut v_min = u32::MAX;
        let mut v_max = 0u32;

        for &vi in &sm.indices {
            v_min = v_min.min(vi);
            v_max = v_max.max(vi);
            let p = avatar.vertices[vi as usize].position;
            csum += Vec3::from_array(p);
            v_cnt += 1;
            for k in 0..3 {
                smin[k] = smin[k].min(p[k]);
                smax[k] = smax[k].max(p[k]);
            }
        }
        if !smin[0].is_finite() {
            smin = [0.0; 3];
            smax = [0.0; 3];
        }
        let centroid = if v_cnt > 0 {
            (csum / (v_cnt as f32)).to_array()
        } else {
            [0.0; 3]
        };

        let i_cnt = sm.indices.len() as u32;
        let dst_slice = &mut out_indices[index_offset as usize..(index_offset + i_cnt) as usize];

        if morton_sort && sm.indices.len() >= 3 {
            let tri_count = sm.indices.len() / 3;
            let mut morton_tris: Vec<(u32, [u32; 3])> = (0..tri_count)
                .into_par_iter()
                .map(|t| {
                    let i0 = sm.indices[t * 3];
                    let i1 = sm.indices[t * 3 + 1];
                    let i2 = sm.indices[t * 3 + 2];
                    let p0 = avatar.vertices[i0 as usize].position;
                    let p1 = avatar.vertices[i1 as usize].position;
                    let p2 = avatar.vertices[i2 as usize].position;
                    let cx = (p0[0] + p1[0] + p2[0]) / 3.0;
                    let cy = (p0[1] + p1[1] + p2[1]) / 3.0;
                    let cz = (p0[2] + p1[2] + p2[2]) / 3.0;
                    let code = morton_30bit(cx, cy, cz, smin, smax);
                    (code, [i0, i1, i2])
                })
                .collect();
            morton_tris.par_sort_unstable_by_key(|item| item.0);
            for (t, (_code, tri)) in morton_tris.into_iter().enumerate() {
                dst_slice[t * 3] = tri[0];
                dst_slice[t * 3 + 1] = tri[1];
                dst_slice[t * 3 + 2] = tri[2];
            }
        } else {
            dst_slice.copy_from_slice(&sm.indices);
        }

        let mat = avatar.materials.get(sm.material_index as usize);
        let alpha_mode = mat.map_or(0, |m| m.alpha_mode);
        let rq_off = mat.map_or(0, |m| m.render_queue_offset);
        let flags = classify_submesh_flags(
            &sm.name,
            alpha_mode,
            submesh_is_face[s_idx],
            sm.is_eye_or_brow_hint,
            centroid,
            eye_level_y,
            head_world_pos[2] + 0.04,
        );
        submesh_flags.push(flags);

        let name_offset = string_buf.len() as u32;
        string_buf.extend_from_slice(sm.name.as_bytes());
        string_buf.push(0);
        let name_len = sm.name.len() as u32;

        if s_idx < out_submeshes.len() {
            out_submeshes[s_idx] = AetherSubmeshDesc {
                index_offset,
                index_count: i_cnt,
                vertex_offset: if v_cnt > 0 { v_min } else { 0 },
                vertex_count: if v_cnt > 0 { v_max - v_min + 1 } else { 0 },
                material_index: sm.material_index,
                flags,
                name_offset,
                name_len,
                render_queue_offset: rq_off,
                _pad0: 0,
                aabb_min: smin,
                aabb_max: smax,
            };
        }
        index_offset += i_cnt;
    }

    // 7. Pack sparse morph deltas & ranges directly into arena
    let mut delta_cursor = 0usize;
    for (m_idx, m) in avatar.morphs.iter().enumerate() {
        let start = delta_cursor;
        let mut face_votes = 0usize;
        for item in &m.deltas {
            if delta_cursor >= out_morph_deltas.len() {
                break;
            }
            if is_face_vert
                .get(item.vertex_index as usize)
                .copied()
                .unwrap_or(false)
            {
                face_votes += 1;
            }
            out_morph_deltas[delta_cursor] = AetherMorphDelta {
                vertex_index: item.vertex_index,
                d_position: item.d_pos,
                d_normal: item.d_norm,
                d_proxy_weight: if is_face_vert
                    .get(item.vertex_index as usize)
                    .copied()
                    .unwrap_or(false)
                {
                    0.5
                } else {
                    0.0
                },
            };
            delta_cursor += 1;
        }
        let name_offset = string_buf.len() as u32;
        string_buf.extend_from_slice(m.name.as_bytes());
        string_buf.push(0);
        let name_len = m.name.len() as u32;
        if m_idx < out_morph_ranges.len() {
            out_morph_ranges[m_idx] = AetherMorphRangeDesc {
                delta_offset: start as u32,
                delta_count: (delta_cursor - start) as u32,
                name_offset,
                name_len,
                centroid_x: m.centroid_x,
                is_face_morph: u32::from(face_votes > 0 || m.centroid_x != 0.0),
                _reserved: [0; 2],
            };
        }
    }

    BakedSummary {
        face_center_head_local,
        submesh_flags,
    }
}
