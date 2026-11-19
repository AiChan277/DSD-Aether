#![deny(clippy::pedantic)]
#![allow(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::cast_possible_wrap,
    clippy::doc_markdown,
    clippy::missing_errors_doc,
    clippy::similar_names,
    clippy::struct_excessive_bools,
    clippy::too_many_lines
)]

//! Safe pure-Rust bounds-checked `MikuMikuDance` PMX 2.0 / 2.1 binary parser.
//!
//! Converts MMD left-handed coordinates & clockwise winding into AETHER's right-handed
//! Y-up metric coordinate space (`z = -z`, `scale = 0.08 m/unit`, triangle winding `(i0, i2, i1)`),
//! preserving `SDEF` (`C, R0, R1`), per-vertex `edge_scale`, `Append Transform`, `CCD-IK`,
//! all PMX morphs (`Group`, `Vertex`, `Bone`, `UV`, `Material`), Ray-MMD material
//! auto-classification, and `RigidBody` + `6-DOF Spring Joint` physics constraints.

use aether_ffi::{
    AETHER_MAT_PROFILE_DUAL_LOBE_HAIR, AETHER_MAT_PROFILE_EMISSIVE, AETHER_MAT_PROFILE_EYE_CAUSTIC,
    AETHER_MAT_PROFILE_GLASS, AETHER_MAT_PROFILE_METALLIC, AETHER_MAT_PROFILE_MICRO_PBR_CLOTH,
    AETHER_MAT_PROFILE_SKIN_SDF, AETHER_PMX_APPEND_LOCAL, AETHER_PMX_APPEND_MOVE,
    AETHER_PMX_APPEND_ROT,
};
use glam::{EulerRot, Quat};
use thiserror::Error;

/// Default MMD model scale factor: 1 MMD unit equals ~8 cm (`0.08 m`).
pub const MMD_TO_METRES: f32 = 0.08;

/// Errors returned while decoding a PMX 2.0/2.1 binary buffer.
#[derive(Debug, Error)]
pub enum PmxError {
    /// Binary stream is malformed or truncated.
    #[error("PMX parse failure: {0}")]
    Parse(&'static str),
    /// Hard safety resource cap exceeded.
    #[error("PMX resource limit exceeded: {0}")]
    LimitExceeded(&'static str),
}

struct Cursor<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    const fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], PmxError> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or(PmxError::Parse("pmx offset overflow"))?;
        let slice = self
            .data
            .get(self.pos..end)
            .ok_or(PmxError::Parse("unexpected EOF in PMX"))?;
        self.pos = end;
        Ok(slice)
    }

    fn u8(&mut self) -> Result<u8, PmxError> {
        Ok(self.take(1)?[0])
    }

    fn u16_le(&mut self) -> Result<u16, PmxError> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }

    fn i16_le(&mut self) -> Result<i16, PmxError> {
        let b = self.take(2)?;
        Ok(i16::from_le_bytes([b[0], b[1]]))
    }

    fn u32_le(&mut self) -> Result<u32, PmxError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn i32_le(&mut self) -> Result<i32, PmxError> {
        let b = self.take(4)?;
        Ok(i32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn f32_le(&mut self) -> Result<f32, PmxError> {
        let b = self.take(4)?;
        let v = f32::from_le_bytes([b[0], b[1], b[2], b[3]]);
        Ok(if v.is_finite() { v } else { 0.0 })
    }

    fn vec2(&mut self) -> Result<[f32; 2], PmxError> {
        Ok([self.f32_le()?, self.f32_le()?])
    }

    fn vec3(&mut self) -> Result<[f32; 3], PmxError> {
        Ok([self.f32_le()?, self.f32_le()?, self.f32_le()?])
    }

    fn vec4(&mut self) -> Result<[f32; 4], PmxError> {
        Ok([
            self.f32_le()?,
            self.f32_le()?,
            self.f32_le()?,
            self.f32_le()?,
        ])
    }

    fn pmx_text(&mut self, encoding: u8) -> Result<String, PmxError> {
        let byte_len = usize::try_from(self.i32_le()?.max(0))
            .map_err(|_| PmxError::Parse("pmx text negative len"))?;
        if byte_len > 16 * 1024 * 1024 {
            return Err(PmxError::LimitExceeded("pmx text string too large"));
        }
        let raw = self.take(byte_len)?;
        if encoding == 0 {
            let (cow, _, _) = encoding_rs::UTF_16LE.decode(raw);
            Ok(cow.into_owned())
        } else {
            Ok(String::from_utf8_lossy(raw).into_owned())
        }
    }

    fn pmx_signed_idx(&mut self, size: u8) -> Result<i32, PmxError> {
        match size {
            1 => Ok(i32::from(self.u8()? as i8)),
            2 => Ok(i32::from(self.i16_le()?)),
            4 => self.i32_le(),
            _ => Err(PmxError::Parse("invalid PMX index byte size")),
        }
    }

    fn pmx_unsigned_idx(&mut self, size: u8) -> Result<u32, PmxError> {
        match size {
            1 => Ok(u32::from(self.u8()?)),
            2 => Ok(u32::from(self.u16_le()?)),
            4 => self.u32_le(),
            _ => Err(PmxError::Parse("invalid PMX vertex index byte size")),
        }
    }
}

/// Flips left-handed Z+ forward -> right-handed Z+ facing camera (`z = -z`).
#[inline]
#[must_use]
pub const fn flip_z(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[1], -v[2]]
}

/// Converts a left-handed PMX XYZ Euler rotation vector (radians) into right-handed `[rx, ry, rz]`.
#[inline]
#[must_use]
pub const fn pmx_lh_euler_to_rh(lh_euler_rad: [f32; 3]) -> [f32; 3] {
    // Reflection diag(1, 1, -1) flips pseudovector sign on X and Y while preserving Z.
    [-lh_euler_rad[0], -lh_euler_rad[1], lh_euler_rad[2]]
}

/// Converts a left-handed PMX rotation quaternion `[x, y, z, w]` to right-handed.
#[inline]
#[must_use]
pub fn pmx_lh_quat_to_rh(lh_q: [f32; 4]) -> [f32; 4] {
    let q = Quat::from_xyzw(-lh_q[0], -lh_q[1], lh_q[2], lh_q[3]);
    if q.length_squared() > 1e-8 {
        q.normalize().to_array()
    } else {
        [0.0, 0.0, 0.0, 1.0]
    }
}

/// Computes deterministic lowercase ASCII/UTF-8 FNV-1a 32-bit hash for fast material/morph slot lookup.
#[must_use]
pub fn pmx_fnv1a_lower(text: &str) -> u32 {
    let mut h: u32 = 0x811C_9DC5;
    for b in text.as_bytes() {
        let lb = b.to_ascii_lowercase();
        h ^= u32::from(lb);
        h = h.wrapping_mul(0x0100_0193);
    }
    h
}

/// Vertex skinning deformation type in PMX 2.0/2.1.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PmxWeightType {
    /// Single bone influence (`wt = 0`).
    Bdef1,
    /// Linear blend of 2 bones (`wt = 1`).
    Bdef2,
    /// Linear blend of up to 4 bones (`wt = 2`).
    Bdef4,
    /// Spherical deform (`wt = 3`) carrying `C`, `R0`, `R1` anchors preventing shoulder/elbow collapse.
    Sdef,
    /// Dual quaternion deform (`wt = 4`, PMX 2.1).
    Qdef,
}

/// Parsed vertex in right-handed AETHER metres (`scale = 0.08`, `z = -z`).
#[derive(Clone, Copy, Debug)]
pub struct PmxVertex {
    pub position_m: [f32; 3],
    pub normal: [f32; 3],
    pub uv0: [f32; 2],
    pub uv1: [f32; 2],
    pub weight_type: PmxWeightType,
    pub joints: [u16; 4],
    pub weights: [f32; 4],
    pub sdef_c_m: [f32; 3],
    pub sdef_r0_m: [f32; 3],
    pub sdef_r1_m: [f32; 3],
    pub edge_scale: f32,
}

/// Ray-MMD / 2.5D NPR auto-classification result for one PMX material.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RayMmdClassification {
    /// Resolved `AETHER_MAT_PROFILE_*` discriminant (`0..=6`).
    pub shader_profile: u32,
    pub is_skin: bool,
    pub is_face_skin: bool,
    pub is_hair: bool,
    pub is_eye_or_brow: bool,
    pub is_metallic: bool,
    pub is_glass: bool,
    pub is_emissive: bool,
}

/// Auto-classifies a PMX material from its JP/EN names, `.spa`/`.sph` sphere map mode, and specular properties.
#[must_use]
pub fn classify_pmx_ray_mmd(
    name_jp: &str,
    name_en: &str,
    diffuse: [f32; 4],
    specular: [f32; 3],
    spec_pow: f32,
    sphere_mode: u8,
) -> RayMmdClassification {
    let en_l = name_en.to_ascii_lowercase();
    let jp_l = name_jp.to_ascii_lowercase();
    let has_any = |needles: &[&str]| -> bool {
        needles
            .iter()
            .any(|n| en_l.contains(n) || jp_l.contains(n) || name_jp.contains(n))
    };

    let is_eye_or_brow = has_any(&[
        "eye",
        "iris",
        "pupil",
        "lash",
        "brow",
        "highlight",
        "目",
        "瞳",
        "白目",
        "まつ毛",
        "まつげ",
        "まぶた",
        "眉",
        "ハイライト",
    ]);
    let is_face_hint = has_any(&["face", "cheek", "mouth", "tongue", "teeth", "顔", "ほほ", "頬", "口", "舌", "歯"]);
    let is_skin_hint = is_face_hint || has_any(&["skin", "body", "neck", "hada", "肌", "首", "腕", "脚", "太もも"]);
    let is_hair = has_any(&[
        "hair",
        "bang",
        "ponytail",
        "twin",
        "ahoge",
        "kami",
        "髪",
        "前髪",
        "後髪",
        "アホ毛",
        "ツインテ",
        "もみあげ",
    ]);
    let is_glass = has_any(&["glass", "lens", "crystal", "gem", "water", "ガラス", "眼鏡", "メガネ", "レンズ", "涙"])
        || (diffuse[3] < 0.85 && spec_pow >= 25.0);
    let is_emissive = has_any(&["emit", "emissive", "glow", "neon", "led", "発光", "蛍光", "ランプ"]);
    let spec_luma = 0.2126 * specular[0] + 0.7152 * specular[1] + 0.0722 * specular[2];
    let is_metallic = has_any(&[
        "metal", "gold", "silver", "chrome", "steel", "armor", "buckle", "button", "zipper", "金属", "金", "銀", "メッキ",
        "鉄",
    ]) || (sphere_mode == 2 && spec_pow >= 28.0 && spec_luma > 0.35);

    let shader_profile = if is_emissive {
        AETHER_MAT_PROFILE_EMISSIVE
    } else if is_glass {
        AETHER_MAT_PROFILE_GLASS
    } else if is_eye_or_brow {
        AETHER_MAT_PROFILE_EYE_CAUSTIC
    } else if is_skin_hint {
        AETHER_MAT_PROFILE_SKIN_SDF
    } else if is_hair {
        AETHER_MAT_PROFILE_DUAL_LOBE_HAIR
    } else if is_metallic {
        AETHER_MAT_PROFILE_METALLIC
    } else if sphere_mode == 2 {
        // Additive `.spa` sphere maps on non-skin default to sheen/dual-lobe accent highlights.
        AETHER_MAT_PROFILE_DUAL_LOBE_HAIR
    } else {
        // `.sph` multiply maps & diffuse cloth map cleanly to Micro-PBR Cloth.
        AETHER_MAT_PROFILE_MICRO_PBR_CLOTH
    };

    RayMmdClassification {
        shader_profile,
        is_skin: is_skin_hint,
        is_face_skin: is_face_hint || has_any(&["face", "顔"]),
        is_hair,
        is_eye_or_brow,
        is_metallic,
        is_glass,
        is_emissive,
    }
}

/// Parsed PMX material descriptor with Ray-MMD classification.
#[derive(Clone, Debug)]
pub struct PmxMaterial {
    pub name_jp: String,
    pub name_en: String,
    pub diffuse: [f32; 4],
    pub specular: [f32; 3],
    pub spec_pow: f32,
    pub ambient: [f32; 3],
    pub draw_flags: u8,
    pub edge_color: [f32; 4],
    pub edge_size: f32,
    pub tex_idx: i32,
    pub sphere_tex: i32,
    pub sphere_mode: u8,
    pub toon_flag: u8,
    pub toon_tex_or_builtin: i32,
    pub surface_count: usize,
    pub classification: RayMmdClassification,
}

/// One link inside a PMX `CCD-IK` chain.
#[derive(Clone, Copy, Debug)]
pub struct PmxIkLink {
    pub bone_index: u32,
    pub has_limit: bool,
    pub limit_min_rad: [f32; 3],
    pub limit_max_rad: [f32; 3],
}

/// Parsed PMX bone preserving `Append Transform` and `CCD-IK` metadata.
#[derive(Clone, Debug)]
pub struct PmxBone {
    pub name_jp: String,
    pub name_en: String,
    pub world_pos_m: [f32; 3],
    pub parent: Option<usize>,
    pub transform_level: i32,
    pub flags: u16,
    pub append_parent: Option<usize>,
    pub append_ratio: f32,
    pub append_flags: u32,
    pub ik_target: Option<usize>,
    pub ik_loops: i32,
    pub ik_limit_rad: f32,
    pub ik_links: Vec<PmxIkLink>,
}

/// Material morph payload (multiplicative or additive).
#[derive(Clone, Copy, Debug)]
pub struct PmxMaterialMorphOffset {
    pub material_index: i32,
    pub op_mode: u8,
    pub diffuse: [f32; 4],
    pub specular: [f32; 3],
    pub spec_pow: f32,
    pub ambient: [f32; 3],
    pub edge_color: [f32; 4],
    pub edge_size: f32,
    pub tex_tint: [f32; 4],
    pub sphere_tint: [f32; 4],
    pub toon_tint: [f32; 4],
}

/// Bone morph offset in RH metres + quaternion.
#[derive(Clone, Copy, Debug)]
pub struct PmxBoneMorphOffset {
    pub bone_index: u32,
    pub translation_m: [f32; 3],
    pub rotation_quat: [f32; 4],
}

/// Decoded payload of a PMX morph across Group, Vertex, Bone, UV, and Material types.
#[derive(Clone, Debug)]
pub enum PmxMorphData {
    Group(Vec<(usize, f32)>),
    Vertex(Vec<(u32, [f32; 3])>),
    Bone(Vec<PmxBoneMorphOffset>),
    Uv(Vec<(u32, [f32; 2])>),
    Material(Vec<PmxMaterialMorphOffset>),
    Other,
}

/// Parsed PMX morph target.
#[derive(Clone, Debug)]
pub struct PmxMorph {
    pub name_jp: String,
    pub name_en: String,
    pub panel: u8,
    pub morph_type: u8,
    pub data: PmxMorphData,
}

/// Parsed PMX Bullet RigidBody in right-handed metres.
#[derive(Clone, Debug)]
pub struct PmxRigidBody {
    pub name_jp: String,
    pub name_en: String,
    pub bone_index: i32,
    pub group_index: u8,
    pub collision_mask: u16,
    pub shape_kind: u8,
    pub size_m: [f32; 3],
    pub pos_m: [f32; 3],
    pub rot_euler_rh: [f32; 3],
    pub mass: f32,
    pub linear_damping: f32,
    pub angular_damping: f32,
    pub restitution: f32,
    pub friction: f32,
    pub phys_mode: u8,
}

/// Parsed PMX Bullet 6-DOF Spring Constraint in right-handed metres.
#[derive(Clone, Debug)]
pub struct PmxConstraint6Dof {
    pub name_jp: String,
    pub name_en: String,
    pub rb_a: i32,
    pub rb_b: i32,
    pub pos_m: [f32; 3],
    pub rot_euler_rh: [f32; 3],
    pub pos_min_m: [f32; 3],
    pub pos_max_m: [f32; 3],
    pub rot_min_rad: [f32; 3],
    pub rot_max_rad: [f32; 3],
    pub spring_pos: [f32; 3],
    pub spring_rot: [f32; 3],
}

/// Complete parsed PMX 2.0/2.1 document converted into right-handed metres.
#[derive(Clone, Debug)]
pub struct PmxDocument {
    pub version: f32,
    pub model_name_jp: String,
    pub model_name_en: String,
    pub vertices: Vec<PmxVertex>,
    pub indices: Vec<u32>,
    pub texture_paths: Vec<String>,
    pub materials: Vec<PmxMaterial>,
    pub bones: Vec<PmxBone>,
    pub morphs: Vec<PmxMorph>,
    pub rigid_bodies: Vec<PmxRigidBody>,
    pub constraints: Vec<PmxConstraint6Dof>,
}

/// Parses a binary PMX 2.0/2.1 buffer safely with bounds checking and LH->RH metre conversion.
pub fn parse_pmx_bytes(data: &[u8]) -> Result<PmxDocument, PmxError> {
    const MAX_VERTS: usize = 2_000_000;
    const MAX_INDS: usize = 6_000_000;
    const MAX_BONES: usize = 4_096;
    const MAX_MATS: usize = 1_024;
    const MAX_MORPHS: usize = 1_024;
    const MAX_MORPH_DELTAS: usize = 16_000_000;

    let mut cur = Cursor::new(data);
    let magic = cur.take(4)?;
    if magic != b"PMX " {
        return Err(PmxError::Parse("invalid PMX magic"));
    }
    let version = cur.f32_le()?;
    if !(1.99..=2.15).contains(&version) {
        return Err(PmxError::Parse("unsupported PMX version (expected 2.0 or 2.1)"));
    }
    let g_len = usize::from(cur.u8()?);
    if g_len < 8 {
        return Err(PmxError::Parse("truncated PMX globals"));
    }
    let globals = cur.take(g_len)?;
    let enc = globals[0];
    let add_vec4 = usize::from(globals[1].min(4));
    let v_idx_sz = globals[2];
    let tex_idx_sz = globals[3];
    let mat_idx_sz = globals[4];
    let bone_idx_sz = globals[5];
    let morph_idx_sz = globals[6];
    let rb_idx_sz = globals[7];

    let model_name_jp = cur.pmx_text(enc)?;
    let model_name_en = cur.pmx_text(enc)?;
    let _comment_jp = cur.pmx_text(enc)?;
    let _comment_en = cur.pmx_text(enc)?;

    // Vertices
    let vert_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX vertex count"))?;
    if vert_count > MAX_VERTS {
        return Err(PmxError::LimitExceeded("PMX vertex count exceeds cap"));
    }
    let mut vertices = Vec::with_capacity(vert_count);
    for _ in 0..vert_count {
        let p_lh = cur.vec3()?;
        let n_lh = cur.vec3()?;
        let uv0 = cur.vec2()?;
        let mut uv1 = uv0;
        for k in 0..add_vec4 {
            let v4 = cur.vec4()?;
            if k == 0 {
                uv1 = [v4[0], v4[1]];
            }
        }
        let wt_type = cur.u8()?;
        let weight_type: PmxWeightType;
        let mut joints = [0u16; 4];
        let mut weights = [0.0f32; 4];
        let mut sdef_c_m = [0.0f32; 3];
        let mut sdef_r0_m = [0.0f32; 3];
        let mut sdef_r1_m = [0.0f32; 3];

        match wt_type {
            0 => {
                weight_type = PmxWeightType::Bdef1;
                joints[0] = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                weights[0] = 1.0;
            }
            1 => {
                weight_type = PmxWeightType::Bdef2;
                joints[0] = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                joints[1] = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                let w0 = cur.f32_le()?;
                weights[0] = w0;
                weights[1] = 1.0 - w0;
            }
            2 | 4 => {
                weight_type = if wt_type == 4 {
                    PmxWeightType::Qdef
                } else {
                    PmxWeightType::Bdef4
                };
                for slot in &mut joints {
                    *slot = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                }
                for slot in &mut weights {
                    *slot = cur.f32_le()?;
                }
            }
            3 => {
                weight_type = PmxWeightType::Sdef;
                joints[0] = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                joints[1] = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                let w0 = cur.f32_le()?;
                weights[0] = w0;
                weights[1] = 1.0 - w0;
                let c_lh = cur.vec3()?;
                let r0_lh = cur.vec3()?;
                let r1_lh = cur.vec3()?;
                let c_rh = flip_z(c_lh);
                let r0_rh = flip_z(r0_lh);
                let r1_rh = flip_z(r1_lh);
                sdef_c_m = [
                    c_rh[0] * MMD_TO_METRES,
                    c_rh[1] * MMD_TO_METRES,
                    c_rh[2] * MMD_TO_METRES,
                ];
                sdef_r0_m = [
                    r0_rh[0] * MMD_TO_METRES,
                    r0_rh[1] * MMD_TO_METRES,
                    r0_rh[2] * MMD_TO_METRES,
                ];
                sdef_r1_m = [
                    r1_rh[0] * MMD_TO_METRES,
                    r1_rh[1] * MMD_TO_METRES,
                    r1_rh[2] * MMD_TO_METRES,
                ];
            }
            _ => return Err(PmxError::Parse("unsupported PMX weight deform type")),
        }

        let edge_scale = cur.f32_le()?.clamp(0.0, 4.0);
        let p_rh = flip_z(p_lh);
        let n_rh = flip_z(n_lh);
        vertices.push(PmxVertex {
            position_m: [
                p_rh[0] * MMD_TO_METRES,
                p_rh[1] * MMD_TO_METRES,
                p_rh[2] * MMD_TO_METRES,
            ],
            normal: n_rh,
            uv0,
            uv1,
            weight_type,
            joints,
            weights,
            sdef_c_m,
            sdef_r0_m,
            sdef_r1_m,
            edge_scale,
        });
    }

    // Indices (LH CW -> RH CCW swap i1 <-> i2)
    let index_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX index count"))?;
    if index_count > MAX_INDS {
        return Err(PmxError::LimitExceeded("PMX index count exceeds cap"));
    }
    if index_count % 3 != 0 {
        return Err(PmxError::Parse("PMX index count not multiple of 3"));
    }
    let mut indices = Vec::with_capacity(index_count);
    for _ in 0..(index_count / 3) {
        let i0 = cur.pmx_unsigned_idx(v_idx_sz)?;
        let i1 = cur.pmx_unsigned_idx(v_idx_sz)?;
        let i2 = cur.pmx_unsigned_idx(v_idx_sz)?;
        if (i0 as usize) >= vert_count || (i1 as usize) >= vert_count || (i2 as usize) >= vert_count
        {
            return Err(PmxError::Parse("PMX vertex index out of bounds"));
        }
        indices.push(i0);
        indices.push(i2);
        indices.push(i1);
    }

    // Texture table
    let tex_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX texture count"))?;
    if tex_count > MAX_MATS {
        return Err(PmxError::LimitExceeded("PMX texture count exceeds cap"));
    }
    let mut texture_paths = Vec::with_capacity(tex_count);
    for _ in 0..tex_count {
        texture_paths.push(cur.pmx_text(enc)?.replace('\\', "/"));
    }

    // Materials
    let mat_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX material count"))?;
    if mat_count > MAX_MATS {
        return Err(PmxError::LimitExceeded("PMX material count exceeds cap"));
    }
    let mut materials = Vec::with_capacity(mat_count);
    for _ in 0..mat_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let diffuse = cur.vec4()?;
        let specular = cur.vec3()?;
        let spec_pow = cur.f32_le()?;
        let ambient = cur.vec3()?;
        let draw_flags = cur.u8()?;
        let edge_color = cur.vec4()?;
        let edge_size = cur.f32_le()?;
        let tex_idx = cur.pmx_signed_idx(tex_idx_sz)?;
        let sphere_tex = cur.pmx_signed_idx(tex_idx_sz)?;
        let sphere_mode = cur.u8()?;
        let toon_flag = cur.u8()?;
        let toon_tex_or_builtin = if toon_flag == 0 {
            cur.pmx_signed_idx(tex_idx_sz)?
        } else {
            i32::from(cur.u8()?)
        };
        let _memo = cur.pmx_text(enc)?;
        let surface_count = usize::try_from(cur.i32_le()?.max(0))
            .map_err(|_| PmxError::Parse("negative PMX material surface count"))?;
        let classification =
            classify_pmx_ray_mmd(&name_jp, &name_en, diffuse, specular, spec_pow, sphere_mode);
        materials.push(PmxMaterial {
            name_jp,
            name_en,
            diffuse,
            specular,
            spec_pow,
            ambient,
            draw_flags,
            edge_color,
            edge_size,
            tex_idx,
            sphere_tex,
            sphere_mode,
            toon_flag,
            toon_tex_or_builtin,
            surface_count,
            classification,
        });
    }

    // Bones
    let bone_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX bone count"))?;
    if bone_count > MAX_BONES {
        return Err(PmxError::LimitExceeded("PMX bone count exceeds cap"));
    }
    let mut bones = Vec::with_capacity(bone_count);
    for _ in 0..bone_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let pos_rh = flip_z(cur.vec3()?);
        let world_pos_m = [
            pos_rh[0] * MMD_TO_METRES,
            pos_rh[1] * MMD_TO_METRES,
            pos_rh[2] * MMD_TO_METRES,
        ];
        let parent_i32 = cur.pmx_signed_idx(bone_idx_sz)?;
        let parent = if parent_i32 >= 0 && (parent_i32 as usize) < bone_count {
            Some(parent_i32 as usize)
        } else {
            None
        };
        let transform_level = cur.i32_le()?;
        let flags = cur.u16_le()?;
        if (flags & 0x0001) != 0 {
            let _ = cur.pmx_signed_idx(bone_idx_sz)?;
        } else {
            let _ = cur.vec3()?;
        }

        let mut append_parent = None;
        let mut append_ratio = 0.0f32;
        let mut append_flags = 0u32;
        if (flags & 0x0100) != 0 || (flags & 0x0200) != 0 {
            let ap = cur.pmx_signed_idx(bone_idx_sz)?;
            append_ratio = cur.f32_le()?;
            if ap >= 0 && (ap as usize) < bone_count {
                append_parent = Some(ap as usize);
                if (flags & 0x0100) != 0 {
                    append_flags |= AETHER_PMX_APPEND_ROT;
                }
                if (flags & 0x0200) != 0 {
                    append_flags |= AETHER_PMX_APPEND_MOVE;
                }
                if (flags & 0x0080) != 0 {
                    append_flags |= AETHER_PMX_APPEND_LOCAL;
                }
            }
        }
        if (flags & 0x0400) != 0 {
            let _ = cur.vec3()?;
        }
        if (flags & 0x0800) != 0 {
            let _ = cur.vec3()?;
            let _ = cur.vec3()?;
        }
        if (flags & 0x2000) != 0 {
            let _ = cur.i32_le()?;
        }

        let mut ik_target = None;
        let mut ik_loops = 0i32;
        let mut ik_limit_rad = 0.0f32;
        let mut ik_links = Vec::new();
        if (flags & 0x0020) != 0 {
            let t = cur.pmx_signed_idx(bone_idx_sz)?;
            ik_loops = cur.i32_le()?;
            ik_limit_rad = cur.f32_le()?;
            if t >= 0 && (t as usize) < bone_count {
                ik_target = Some(t as usize);
            }
            let link_cnt = usize::try_from(cur.i32_le()?.max(0))
                .unwrap_or(0)
                .min(128);
            ik_links.reserve(link_cnt);
            for _ in 0..link_cnt {
                let lb = cur.pmx_signed_idx(bone_idx_sz)?;
                let has_lim = cur.u8()? == 1;
                let (limit_min_rad, limit_max_rad) = if has_lim {
                    let lo = cur.vec3()?;
                    let hi = cur.vec3()?;
                    // LH->RH: rx' = -rx, ry' = -ry, rz' = +rz -> swap min/max on X and Y!
                    ([-hi[0], -hi[1], lo[2]], [-lo[0], -lo[1], hi[2]])
                } else {
                    ([0.0; 3], [0.0; 3])
                };
                if lb >= 0 && (lb as usize) < bone_count {
                    ik_links.push(PmxIkLink {
                        bone_index: lb as u32,
                        has_limit: has_lim,
                        limit_min_rad,
                        limit_max_rad,
                    });
                }
            }
        }

        bones.push(PmxBone {
            name_jp,
            name_en,
            world_pos_m,
            parent,
            transform_level,
            flags,
            append_parent,
            append_ratio,
            append_flags,
            ik_target,
            ik_loops,
            ik_limit_rad,
            ik_links,
        });
    }

    // Morphs
    let morph_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| PmxError::Parse("negative PMX morph count"))?;
    if morph_count > MAX_MORPHS {
        return Err(PmxError::LimitExceeded("PMX morph count exceeds cap"));
    }
    let mut morphs = Vec::with_capacity(morph_count);
    let mut total_deltas = 0usize;

    for _ in 0..morph_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let panel = cur.u8()?;
        let morph_type = cur.u8()?;
        let off_cnt = usize::try_from(cur.i32_le()?.max(0))
            .map_err(|_| PmxError::Parse("negative PMX morph offset count"))?;
        let data = match morph_type {
            0 | 9 => {
                // Group morph (0) / Flip morph (9, PMX 2.1)
                let mut children = Vec::with_capacity(off_cnt.min(256));
                for _ in 0..off_cnt {
                    let m = cur.pmx_signed_idx(morph_idx_sz)?;
                    let w = cur.f32_le()?;
                    if m >= 0 && (m as usize) < morph_count {
                        children.push((m as usize, w));
                    }
                }
                PmxMorphData::Group(children)
            }
            1 => {
                // Vertex morph
                total_deltas = total_deltas.saturating_add(off_cnt);
                if total_deltas > MAX_MORPH_DELTAS {
                    return Err(PmxError::LimitExceeded("PMX morph deltas exceed cap"));
                }
                let mut deltas = Vec::with_capacity(off_cnt.min(vert_count));
                for _ in 0..off_cnt {
                    let v_idx = cur.pmx_unsigned_idx(v_idx_sz)?;
                    let dp = flip_z(cur.vec3()?);
                    if (v_idx as usize) < vert_count {
                        deltas.push((
                            v_idx,
                            [
                                dp[0] * MMD_TO_METRES,
                                dp[1] * MMD_TO_METRES,
                                dp[2] * MMD_TO_METRES,
                            ],
                        ));
                    }
                }
                PmxMorphData::Vertex(deltas)
            }
            2 => {
                // Bone morph
                let mut entries = Vec::with_capacity(off_cnt.min(bone_count));
                for _ in 0..off_cnt {
                    let b = cur.pmx_signed_idx(bone_idx_sz)?;
                    let t_rh = flip_z(cur.vec3()?);
                    let q_rh = pmx_lh_quat_to_rh(cur.vec4()?);
                    if b >= 0 && (b as usize) < bone_count {
                        entries.push(PmxBoneMorphOffset {
                            bone_index: b as u32,
                            translation_m: [
                                t_rh[0] * MMD_TO_METRES,
                                t_rh[1] * MMD_TO_METRES,
                                t_rh[2] * MMD_TO_METRES,
                            ],
                            rotation_quat: q_rh,
                        });
                    }
                }
                PmxMorphData::Bone(entries)
            }
            3..=7 => {
                // UV & Additional UV1..4 morphs
                total_deltas = total_deltas.saturating_add(off_cnt);
                if total_deltas > MAX_MORPH_DELTAS {
                    return Err(PmxError::LimitExceeded("PMX UV morph deltas exceed cap"));
                }
                let mut uvs = Vec::with_capacity(off_cnt.min(vert_count));
                for _ in 0..off_cnt {
                    let v_idx = cur.pmx_unsigned_idx(v_idx_sz)?;
                    let d = cur.vec4()?;
                    if (v_idx as usize) < vert_count && (morph_type == 3 || morph_type == 4) {
                        uvs.push((v_idx, [d[0], d[1]]));
                    }
                }
                PmxMorphData::Uv(uvs)
            }
            8 => {
                // Material morph (108 bytes scalars + mat index + op)
                let mut mats = Vec::with_capacity(off_cnt.min(MAX_MATS));
                for _ in 0..off_cnt {
                    let material_index = cur.pmx_signed_idx(mat_idx_sz)?;
                    let op_mode = cur.u8()?;
                    let diffuse = cur.vec4()?;
                    let specular = cur.vec3()?;
                    let spec_pow = cur.f32_le()?;
                    let ambient = cur.vec3()?;
                    let edge_color = cur.vec4()?;
                    let edge_size = cur.f32_le()?;
                    let tex_tint = cur.vec4()?;
                    let sphere_tint = cur.vec4()?;
                    let toon_tint = cur.vec4()?;
                    mats.push(PmxMaterialMorphOffset {
                        material_index,
                        op_mode,
                        diffuse,
                        specular,
                        spec_pow,
                        ambient,
                        edge_color,
                        edge_size,
                        tex_tint,
                        sphere_tint,
                        toon_tint,
                    });
                }
                PmxMorphData::Material(mats)
            }
            10 => {
                // PMX 2.1 Impulse morph
                for _ in 0..off_cnt {
                    let _ = cur.pmx_signed_idx(rb_idx_sz)?;
                    let _ = cur.u8()?;
                    let _ = cur.vec3()?;
                    let _ = cur.vec3()?;
                }
                PmxMorphData::Other
            }
            _ => return Err(PmxError::Parse("unknown PMX morph type")),
        };

        morphs.push(PmxMorph {
            name_jp,
            name_en,
            panel,
            morph_type,
            data,
        });
    }

    // Display frames (skip safely)
    if let Ok(frame_cnt) = cur.i32_le() {
        let fc = usize::try_from(frame_cnt.max(0)).unwrap_or(0).min(1024);
        for _ in 0..fc {
            let _ = cur.pmx_text(enc)?;
            let _ = cur.pmx_text(enc)?;
            let _ = cur.u8()?;
            let ec = usize::try_from(cur.i32_le().unwrap_or(0).max(0))
                .unwrap_or(0)
                .min(4096);
            for _ in 0..ec {
                let kind = cur.u8()?;
                if kind == 0 {
                    let _ = cur.pmx_signed_idx(bone_idx_sz)?;
                } else {
                    let _ = cur.pmx_signed_idx(morph_idx_sz)?;
                }
            }
        }
    }

    // RigidBodies
    let mut rigid_bodies = Vec::new();
    if let Ok(rb_cnt_i32) = cur.i32_le() {
        let rb_cnt = usize::try_from(rb_cnt_i32.max(0)).unwrap_or(0).min(2048);
        rigid_bodies.reserve(rb_cnt);
        for _ in 0..rb_cnt {
            let name_jp = cur.pmx_text(enc)?;
            let name_en = cur.pmx_text(enc)?;
            let bone_index = cur.pmx_signed_idx(bone_idx_sz)?;
            let group_index = cur.u8()?;
            let collision_mask = cur.u16_le()?;
            let shape_kind = cur.u8()?;
            let size_raw = cur.vec3()?;
            let pos_rh = flip_z(cur.vec3()?);
            let rot_euler_rh = pmx_lh_euler_to_rh(cur.vec3()?);
            let mass = cur.f32_le()?;
            let linear_damping = cur.f32_le()?;
            let angular_damping = cur.f32_le()?;
            let restitution = cur.f32_le()?;
            let friction = cur.f32_le()?;
            let phys_mode = cur.u8()?;
            rigid_bodies.push(PmxRigidBody {
                name_jp,
                name_en,
                bone_index,
                group_index,
                collision_mask,
                shape_kind,
                size_m: [
                    size_raw[0] * MMD_TO_METRES,
                    size_raw[1] * MMD_TO_METRES,
                    size_raw[2] * MMD_TO_METRES,
                ],
                pos_m: [
                    pos_rh[0] * MMD_TO_METRES,
                    pos_rh[1] * MMD_TO_METRES,
                    pos_rh[2] * MMD_TO_METRES,
                ],
                rot_euler_rh,
                mass,
                linear_damping,
                angular_damping,
                restitution,
                friction,
                phys_mode,
            });
        }
    }

    // 6-DOF Spring Constraints
    let mut constraints = Vec::new();
    if let Ok(jnt_cnt_i32) = cur.i32_le() {
        let jnt_cnt = usize::try_from(jnt_cnt_i32.max(0)).unwrap_or(0).min(4096);
        constraints.reserve(jnt_cnt);
        for _ in 0..jnt_cnt {
            let name_jp = cur.pmx_text(enc)?;
            let name_en = cur.pmx_text(enc)?;
            let _jtype = cur.u8()?;
            let rb_a = cur.pmx_signed_idx(rb_idx_sz)?;
            let rb_b = cur.pmx_signed_idx(rb_idx_sz)?;
            let pos_rh = flip_z(cur.vec3()?);
            let rot_euler_rh = pmx_lh_euler_to_rh(cur.vec3()?);
            let pmin_lh = cur.vec3()?;
            let pmax_lh = cur.vec3()?;
            let rmin_lh = cur.vec3()?;
            let rmax_lh = cur.vec3()?;
            let spring_pos = cur.vec3()?;
            let spring_rot = cur.vec3()?;
            // LH->RH swap min/max for negated Z translation and negated X/Y Euler rotations
            let pos_min_m = [
                pmin_lh[0] * MMD_TO_METRES,
                pmin_lh[1] * MMD_TO_METRES,
                -pmax_lh[2] * MMD_TO_METRES,
            ];
            let pos_max_m = [
                pmax_lh[0] * MMD_TO_METRES,
                pmax_lh[1] * MMD_TO_METRES,
                -pmin_lh[2] * MMD_TO_METRES,
            ];
            let rot_min_rad = [-rmax_lh[0], -rmax_lh[1], rmin_lh[2]];
            let rot_max_rad = [-rmin_lh[0], -rmin_lh[1], rmax_lh[2]];

            let _ = Quat::from_euler(
                EulerRot::YXZ,
                rot_euler_rh[1],
                rot_euler_rh[0],
                rot_euler_rh[2],
            );

            constraints.push(PmxConstraint6Dof {
                name_jp,
                name_en,
                rb_a,
                rb_b,
                pos_m: [
                    pos_rh[0] * MMD_TO_METRES,
                    pos_rh[1] * MMD_TO_METRES,
                    pos_rh[2] * MMD_TO_METRES,
                ],
                rot_euler_rh,
                pos_min_m,
                pos_max_m,
                rot_min_rad,
                rot_max_rad,
                spring_pos,
                spring_rot,
            });
        }
    }

    Ok(PmxDocument {
        version,
        model_name_jp,
        model_name_en,
        vertices,
        indices,
        texture_paths,
        materials,
        bones,
        morphs,
        rigid_bodies,
        constraints,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn push_pmx_str(dst: &mut Vec<u8>, s: &str, utf8: bool) {
        let bytes = if utf8 {
            s.as_bytes().to_vec()
        } else {
            s.encode_utf16().flat_map(u16::to_le_bytes).collect()
        };
        dst.extend_from_slice(&(bytes.len() as i32).to_le_bytes());
        dst.extend_from_slice(&bytes);
    }

    #[test]
    fn parses_pmx_21_sdef_append_ik_morphs_and_physics() {
        let mut buf = Vec::new();
        buf.extend_from_slice(b"PMX ");
        buf.extend_from_slice(&2.1f32.to_le_bytes());
        buf.push(8);
        // globals: utf8=1, add_uv4=0, idx sizes=1
        buf.extend_from_slice(&[1, 0, 1, 1, 1, 1, 1, 1]);
        push_pmx_str(&mut buf, "テストモデル", true);
        push_pmx_str(&mut buf, "TestModel", true);
        push_pmx_str(&mut buf, "", true);
        push_pmx_str(&mut buf, "", true);

        // 3 vertices (SDEF with C, R0, R1, edge_scale = 0.0 on v0, 1.0 on v1, 2.0 on v2)
        buf.extend_from_slice(&3i32.to_le_bytes());
        for (i, es) in [0.0f32, 1.0, 2.0].iter().enumerate() {
            for f in [i as f32, 10.0, 2.0] {
                buf.extend_from_slice(&f.to_le_bytes());
            }
            for f in [0.0f32, 0.0, 1.0] {
                buf.extend_from_slice(&f.to_le_bytes());
            }
            buf.extend_from_slice(&0.5f32.to_le_bytes());
            buf.extend_from_slice(&0.25f32.to_le_bytes());
            buf.push(3); // SDEF
            buf.push(0); // b0
            buf.push(1); // b1
            buf.extend_from_slice(&0.5f32.to_le_bytes()); // w0
            // C, R0, R1
            for f in [0.0f32, 10.0, 2.0, 0.0, 9.0, 2.0, 0.0, 11.0, 2.0] {
                buf.extend_from_slice(&f.to_le_bytes());
            }
            buf.extend_from_slice(&es.to_le_bytes());
        }

        // 3 indices (LH CW 0,1,2 -> RH CCW 0,2,1)
        buf.extend_from_slice(&3i32.to_le_bytes());
        buf.extend_from_slice(&[0, 1, 2]);

        // 2 textures (.png + .spa)
        buf.extend_from_slice(&2i32.to_le_bytes());
        push_pmx_str(&mut buf, "tex\\hair.png", true);
        push_pmx_str(&mut buf, "tex\\gold.spa", true);

        // 2 materials (Skin + .spa Metal)
        buf.extend_from_slice(&2i32.to_le_bytes());
        for (jp, en, sp, mode, scnt) in [
            ("顔肌", "FaceSkin", 4.0f32, 0u8, 3i32),
            ("金属パーツ", "ArmorMetal", 48.0f32, 2u8, 0i32),
        ] {
            push_pmx_str(&mut buf, jp, true);
            push_pmx_str(&mut buf, en, true);
            for _ in 0..4 {
                buf.extend_from_slice(&1.0f32.to_le_bytes());
            }
            for _ in 0..3 {
                buf.extend_from_slice(&0.8f32.to_le_bytes());
            }
            buf.extend_from_slice(&sp.to_le_bytes());
            for _ in 0..3 {
                buf.extend_from_slice(&0.2f32.to_le_bytes());
            }
            buf.push(0x11);
            for _ in 0..4 {
                buf.extend_from_slice(&0.0f32.to_le_bytes());
            }
            buf.extend_from_slice(&1.0f32.to_le_bytes());
            buf.push(0); // tex 0
            buf.push(if mode > 0 { 1 } else { 255 }); // sphere tex
            buf.push(mode);
            buf.push(1);
            buf.push(0);
            push_pmx_str(&mut buf, "", true);
            buf.extend_from_slice(&scnt.to_le_bytes());
        }

        // 2 bones (0=頭, 1=アホ毛 with Append rot + IK)
        buf.extend_from_slice(&2i32.to_le_bytes());
        push_pmx_str(&mut buf, "頭", true);
        push_pmx_str(&mut buf, "head", true);
        for f in [0.0f32, 15.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }
        buf.push(255); // no parent
        buf.extend_from_slice(&0i32.to_le_bytes());
        buf.extend_from_slice(&0x0000u16.to_le_bytes());
        for f in [0.0f32, 1.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }

        push_pmx_str(&mut buf, "左足ＩＫ", true);
        push_pmx_str(&mut buf, "leftFootIK", true);
        for f in [1.0f32, 0.0, 1.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }
        buf.push(0); // parent 0
        buf.extend_from_slice(&1i32.to_le_bytes());
        buf.extend_from_slice(&(0x0100u16 | 0x0020u16).to_le_bytes());
        for f in [0.0f32, 1.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }
        buf.push(0); // append parent 0
        buf.extend_from_slice(&0.5f32.to_le_bytes());
        buf.push(0); // ik target 0
        buf.extend_from_slice(&20i32.to_le_bytes());
        buf.extend_from_slice(&1.5f32.to_le_bytes());
        buf.extend_from_slice(&1i32.to_le_bytes()); // 1 link
        buf.push(0); // link bone 0
        buf.push(1); // has limit
        for f in [-0.5f32, 0.0, 0.0, 0.2, 0.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }

        // 2 morphs: UV morph + Material morph
        buf.extend_from_slice(&2i32.to_le_bytes());
        push_pmx_str(&mut buf, "瞳小", true);
        push_pmx_str(&mut buf, "PupilSmall", true);
        buf.push(1);
        buf.push(3); // UV
        buf.extend_from_slice(&1i32.to_le_bytes());
        buf.push(1); // v_idx 1
        for f in [0.125f32, -0.05, 0.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }

        push_pmx_str(&mut buf, "照れ", true);
        push_pmx_str(&mut buf, "BlushMat", true);
        buf.push(3);
        buf.push(8); // Material
        buf.extend_from_slice(&1i32.to_le_bytes());
        buf.push(0); // mat 0
        buf.push(1); // Add
        for f in [0.2f32, 0.0, 0.0, 0.0] {
            buf.extend_from_slice(&f.to_le_bytes());
        }
        for _ in 0..(112 / 4 - 4) {
            buf.extend_from_slice(&0.0f32.to_le_bytes());
        }

        // 0 display frames
        buf.extend_from_slice(&0i32.to_le_bytes());
        // 2 rigid bodies
        buf.extend_from_slice(&2i32.to_le_bytes());
        for mode in [0u8, 1u8] {
            push_pmx_str(&mut buf, "rb", true);
            push_pmx_str(&mut buf, "rb", true);
            buf.push(mode);
            buf.push(0);
            buf.extend_from_slice(&0xFFFFu16.to_le_bytes());
            buf.push(0);
            for f in [1.0f32, 1.0, 1.0, 0.0, 10.0, 2.0, 0.0, 0.0, 0.0, 1.0, 0.9, 0.9, 0.0, 0.5] {
                buf.extend_from_slice(&f.to_le_bytes());
            }
            buf.push(mode);
        }
        // 1 6-DOF constraint
        buf.extend_from_slice(&1i32.to_le_bytes());
        push_pmx_str(&mut buf, "c0", true);
        push_pmx_str(&mut buf, "c0", true);
        buf.push(0);
        buf.push(0);
        buf.push(1);
        for _ in 0..24 {
            buf.extend_from_slice(&0.0f32.to_le_bytes());
        }

        let res = parse_pmx_bytes(&buf);
        assert!(res.is_ok());
        let Ok(doc) = res else {
            return;
        };
        assert!((doc.version - 2.1).abs() < 0.01);
        assert_eq!(doc.vertices.len(), 3);
        assert_eq!(doc.vertices[0].weight_type, PmxWeightType::Sdef);
        assert!((doc.vertices[0].sdef_c_m[2] - (-2.0 * MMD_TO_METRES)).abs() < 1e-5);
        assert_eq!(doc.vertices[0].edge_scale, 0.0);
        assert_eq!(doc.vertices[2].edge_scale, 2.0);
        assert_eq!(doc.indices, vec![0, 2, 1]);
        assert_eq!(
            doc.materials[0].classification.shader_profile,
            AETHER_MAT_PROFILE_SKIN_SDF
        );
        assert_eq!(
            doc.materials[1].classification.shader_profile,
            AETHER_MAT_PROFILE_METALLIC
        );
        assert_eq!(doc.bones[1].append_parent, Some(0));
        assert_eq!(doc.bones[1].ik_target, Some(0));
        assert!((doc.bones[1].ik_links[0].limit_min_rad[0] - (-0.2)).abs() < 1e-5);
        assert_eq!(doc.rigid_bodies.len(), 2);
        assert_eq!(doc.constraints.len(), 1);
    }
}
