//! Safe pure-Rust bounds-checked PMX 2.0/2.1 and PMD 1.0 (Shift-JIS) parser (`§6.2`).
//!
//! Converts MMD left-handed coordinates & clockwise winding into AETHER right-handed Y-up metres
//! (`z = -z`, triangle winding reversed, `pos * 0.08`) and extracts Bullet rigid bodies & constraints.

use std::path::Path;

use aether_ffi::{
    AETHER_ALPHA_MODE_OPAQUE, AETHER_COLLIDER_SHAPE_CAPSULE, AETHER_COLLIDER_SHAPE_SPHERE,
    AETHER_SOURCE_FORMAT_PMD, AETHER_SOURCE_FORMAT_PMX,
};
use encoding_rs::SHIFT_JIS;

use crate::LoaderError;
use crate::ir::{
    ParsedAvatar, RawCollider, RawExprPreset, RawJoint, RawMaterial, RawMorphDeltaItem,
    RawMorphTarget, RawSpringChain, RawSpringNode, RawSubmesh, RawTextureDesc, RawTextureSource,
    RawVertex, SourceUnitMetadata,
};
use crate::limits::{
    MAX_COLLIDERS, MAX_MATERIALS, MAX_MORPH_DELTAS, check_bone_count, check_index_count,
    check_morph_count, check_vertex_count,
};

struct Cursor<'a> {
    data: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    const fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0 }
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], LoaderError> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or(LoaderError::Parse("pmx offset overflow"))?;
        let slice = self
            .data
            .get(self.pos..end)
            .ok_or(LoaderError::Parse("unexpected EOF in PMX/PMD"))?;
        self.pos = end;
        Ok(slice)
    }

    fn u8(&mut self) -> Result<u8, LoaderError> {
        Ok(self.take(1)?[0])
    }

    fn u16_le(&mut self) -> Result<u16, LoaderError> {
        let b = self.take(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }

    fn i16_le(&mut self) -> Result<i16, LoaderError> {
        let b = self.take(2)?;
        Ok(i16::from_le_bytes([b[0], b[1]]))
    }

    fn u32_le(&mut self) -> Result<u32, LoaderError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn i32_le(&mut self) -> Result<i32, LoaderError> {
        let b = self.take(4)?;
        Ok(i32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn f32_le(&mut self) -> Result<f32, LoaderError> {
        let v = f32::from_le_bytes(
            self.take(4)?
                .try_into()
                .map_err(|_| LoaderError::Parse("float slice fail"))?,
        );
        Ok(if v.is_finite() { v } else { 0.0 })
    }

    fn vec2(&mut self) -> Result<[f32; 2], LoaderError> {
        Ok([self.f32_le()?, self.f32_le()?])
    }

    fn vec3(&mut self) -> Result<[f32; 3], LoaderError> {
        Ok([self.f32_le()?, self.f32_le()?, self.f32_le()?])
    }

    fn vec4(&mut self) -> Result<[f32; 4], LoaderError> {
        Ok([
            self.f32_le()?,
            self.f32_le()?,
            self.f32_le()?,
            self.f32_le()?,
        ])
    }

    fn pmx_text(&mut self, encoding: u8) -> Result<String, LoaderError> {
        let byte_len = usize::try_from(self.i32_le()?.max(0))
            .map_err(|_| LoaderError::Parse("pmx text negative len"))?;
        if byte_len > 1024 * 1024 {
            return Err(LoaderError::LimitExceeded("pmx string > 1 MiB"));
        }
        let bytes = self.take(byte_len)?;
        if encoding == 1 {
            Ok(String::from_utf8_lossy(bytes).into_owned())
        } else {
            let mut u16s = Vec::with_capacity(bytes.len() / 2);
            for chunk in bytes.chunks_exact(2) {
                u16s.push(u16::from_le_bytes([chunk[0], chunk[1]]));
            }
            Ok(String::from_utf16_lossy(&u16s))
        }
    }

    fn pmx_signed_idx(&mut self, size: u8) -> Result<i32, LoaderError> {
        match size {
            1 => Ok(i32::from(self.u8()? as i8)),
            2 => Ok(i32::from(self.i16_le()?)),
            4 => self.i32_le(),
            _ => Err(LoaderError::Parse("invalid pmx index byte size")),
        }
    }

    fn pmx_unsigned_idx(&mut self, size: u8) -> Result<u32, LoaderError> {
        match size {
            1 => Ok(u32::from(self.u8()?)),
            2 => Ok(u32::from(self.u16_le()?)),
            4 => self.u32_le(),
            _ => Err(LoaderError::Parse("invalid pmx vertex index byte size")),
        }
    }
}

fn decode_sjis_cstr(raw: &[u8]) -> String {
    let nul = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
    let (cow, _, _) = SHIFT_JIS.decode(&raw[..nul]);
    cow.trim().to_owned()
}

/// Parses either a PMX 2.0/2.1 binary (`"PMX "`) or PMD 1.0 binary (`"Pmd"`).
pub fn parse_pmx_or_pmd(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    if data.starts_with(b"PMX ") {
        parse_pmx(data, base_dir)
    } else if data.starts_with(b"Pmd") {
        parse_pmd(data, base_dir)
    } else {
        Err(LoaderError::Parse("unrecognized PMX/PMD signature"))
    }
}

fn flip_z(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[1], -v[2]]
}

const MMD_TO_METRES: f32 = 0.08;

struct RawRb {
    bone: i32,
    group: u8,
    shape: u8,
    size: [f32; 3],
    pos: [f32; 3],
    ang_damp: f32,
    phys_mode: u8,
}

fn parse_pmx(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    let mut cur = Cursor::new(data);
    let sig = cur.take(4)?;
    if sig != b"PMX " {
        return Err(LoaderError::Parse("bad PMX magic"));
    }
    let version = cur.f32_le()?;
    if !(1.9..=2.2).contains(&version) {
        return Err(LoaderError::Parse("unsupported PMX version"));
    }
    let globals_len = usize::from(cur.u8()?);
    if !(8..=32).contains(&globals_len) {
        return Err(LoaderError::Parse("invalid PMX globals length"));
    }
    let globals = cur.take(globals_len)?;
    let enc = globals[0];
    let add_uv4 = usize::from(globals[1].min(4));
    let v_idx_sz = globals[2];
    let tex_idx_sz = globals[3];
    let mat_idx_sz = globals[4];
    let bone_idx_sz = globals[5];
    let morph_idx_sz = globals[6];
    let rb_idx_sz = globals[7];

    // Model info
    let _name_jp = cur.pmx_text(enc)?;
    let _name_en = cur.pmx_text(enc)?;
    let _comment_jp = cur.pmx_text(enc)?;
    let _comment_en = cur.pmx_text(enc)?;

    // Vertices
    let vert_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative vertex count"))?;
    check_vertex_count(vert_count)?;
    let mut vertices = Vec::with_capacity(vert_count);

    for _ in 0..vert_count {
        let p = flip_z(cur.vec3()?);
        let n = flip_z(cur.vec3()?);
        let uv0 = cur.vec2()?;
        let mut uv1 = [0.0, 0.0];
        for k in 0..add_uv4 {
            let v4 = cur.vec4()?;
            if k == 0 {
                uv1 = [v4[0], v4[1]];
            }
        }
        let wt_type = cur.u8()?;
        let mut joints = [0u16; 8];
        let mut weights = [0.0f32; 8];
        match wt_type {
            0 => {
                // BDEF1
                let b0 = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                joints[0] = b0;
                weights[0] = 1.0;
            }
            1 | 3 => {
                // BDEF2 / SDEF
                let b0 = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                let b1 = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                let w0 = cur.f32_le()?;
                if wt_type == 3 {
                    let _c = cur.vec3()?;
                    let _r0 = cur.vec3()?;
                    let _r1 = cur.vec3()?;
                }
                joints[0] = b0;
                joints[1] = b1;
                weights[0] = w0;
                weights[1] = 1.0 - w0;
            }
            2 | 4 => {
                // BDEF4 / QDEF
                for slot in &mut joints[..4] {
                    *slot = cur.pmx_signed_idx(bone_idx_sz)?.max(0) as u16;
                }
                for slot in &mut weights[..4] {
                    *slot = cur.f32_le()?;
                }
            }
            _ => return Err(LoaderError::Parse("unsupported PMX weight deform type")),
        }
        let edge_scale = cur.f32_le()?;
        vertices.push(RawVertex {
            position: [
                p[0] * MMD_TO_METRES,
                p[1] * MMD_TO_METRES,
                p[2] * MMD_TO_METRES,
            ],
            normal: n,
            uv0,
            uv1,
            color: [255, 255, 255, 255],
            joints,
            weights,
            edge_scale: edge_scale.clamp(0.0, 4.0),
            flatten_override: -1.0,
        });
    }

    // Surfaces (reverse triangle winding i0, i2, i1 for LH -> RH flip)
    let index_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative surface count"))?;
    check_index_count(index_count)?;
    if index_count % 3 != 0 {
        return Err(LoaderError::Parse("PMX index count not multiple of 3"));
    }
    let mut raw_indices = Vec::with_capacity(index_count);
    for tri in 0..(index_count / 3) {
        let i0 = cur.pmx_unsigned_idx(v_idx_sz)?;
        let i1 = cur.pmx_unsigned_idx(v_idx_sz)?;
        let i2 = cur.pmx_unsigned_idx(v_idx_sz)?;
        if (i0 as usize) >= vert_count || (i1 as usize) >= vert_count || (i2 as usize) >= vert_count
        {
            return Err(LoaderError::Parse("PMX vertex index out of bounds"));
        }
        let _ = tri;
        raw_indices.push(i0);
        raw_indices.push(i2);
        raw_indices.push(i1);
    }

    // Textures
    let tex_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative texture count"))?;
    if tex_count > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded("PMX texture count > cap"));
    }
    let mut textures = Vec::with_capacity(tex_count);
    for idx in 0..tex_count {
        let rel = cur.pmx_text(enc)?;
        let cleaned = rel.replace('\\', "/");
        let path = base_dir.join(&cleaned);
        textures.push(RawTextureDesc {
            name: if cleaned.is_empty() {
                format!("pmx_tex_{idx}")
            } else {
                cleaned
            },
            source: if path.is_file() {
                RawTextureSource::ExternalFile(path)
            } else {
                RawTextureSource::Missing
            },
            width: 1,
            height: 1,
            srgb: true,
            usage_flags: aether_ffi::AETHER_TEX_USAGE_COLOR,
            content_hash_lo: 0,
            rgba_byte_len: 4,
            cached_bc7_path: None,
            cached_bc7_byte_len: 0,
            pixel_offset: 0,
            pixel_size: 4,
            final_format: aether_ffi::AETHER_TEX_FORMAT_RGBA8_SRGB,
        });
    }

    // Materials -> build 1 submesh per material consuming `surface_count` indices
    let mat_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative material count"))?;
    if mat_count > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded("PMX material count > 1024"));
    }
    let mut materials = Vec::with_capacity(mat_count);
    let mut submeshes = Vec::with_capacity(mat_count);
    let mut idx_cursor = 0usize;

    for m_idx in 0..mat_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let name = if name_en.is_empty() { name_jp } else { name_en };
        let diffuse = cur.vec4()?;
        let _spec = cur.vec3()?;
        let _spec_pow = cur.f32_le()?;
        let _ambient = cur.vec3()?;
        let draw_flags = cur.u8()?;
        let edge_color = cur.vec4()?;
        let edge_size = cur.f32_le()?;
        let tex_idx = cur.pmx_signed_idx(tex_idx_sz)?;
        let sphere_tex = cur.pmx_signed_idx(tex_idx_sz)?;
        let sphere_mode = cur.u8()?;
        let toon_flag = cur.u8()?;
        if toon_flag == 0 {
            let _ = cur.pmx_signed_idx(tex_idx_sz)?;
        } else {
            let _ = cur.u8()?;
        }
        let _memo = cur.pmx_text(enc)?;
        let surf_cnt = usize::try_from(cur.i32_le()?.max(0))
            .map_err(|_| LoaderError::Parse("negative material surface count"))?;
        let end_idx = idx_cursor.saturating_add(surf_cnt).min(raw_indices.len());
        let sub_indices = raw_indices[idx_cursor..end_idx].to_vec();
        idx_cursor = end_idx;

        let to_u32 = |v: i32| -> Option<u32> {
            if v >= 0 && (v as usize) < tex_count {
                Some(v as u32)
            } else {
                None
            }
        };

        materials.push(RawMaterial {
            name: name.clone(),
            base_color: diffuse,
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: 4.0,
            rim_lift: 0.0,
            outline_color: [edge_color[0], edge_color[1], edge_color[2]],
            outline_lighting_mix: 0.25,
            outline_width: if (draw_flags & 0x10) != 0 {
                (edge_size * 0.002).clamp(0.0, 0.02)
            } else {
                0.0
            },
            outline_screen_space: false,
            emissive: [0.0, 0.0, 0.0],
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: if sphere_mode == 2 { 0.0 } else { 1.0 },
            matcap_strength: if sphere_mode == 0 { 0.0 } else { 0.6 },
            gi_equalization: 0.15,
            shading_shift: -0.05,
            shading_toony: 0.85,
            uv_scroll: [0.0, 0.0],
            uv_rotation: 0.0,
            alpha_mode: if diffuse[3] < 0.99 {
                aether_ffi::AETHER_ALPHA_MODE_BLEND
            } else {
                AETHER_ALPHA_MODE_OPAQUE
            },
            alpha_cutoff: 0.5,
            double_sided: (draw_flags & 0x01) != 0,
            unlit: false,
            has_vrm_mtoon: false,
            render_queue_offset: 0,
            flatten_override: None,
            tex_base: to_u32(tex_idx),
            tex_shade: None,
            tex_shading_shift: None,
            tex_normal: None,
            tex_emissive: None,
            tex_rim: None,
            tex_matcap: if sphere_mode > 0 {
                to_u32(sphere_tex)
            } else {
                None
            },
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
        });

        let lower = name.to_ascii_lowercase();
        submeshes.push(RawSubmesh {
            name,
            indices: sub_indices,
            material_index: m_idx as u32,
            is_eye_or_brow_hint: lower.contains("eye")
                || lower.contains("brow")
                || lower.contains("lash")
                || lower.contains("目")
                || lower.contains("眉"),
        });
    }

    // Bones (world position -> converted to local offset relative to parent after collecting all)
    let bone_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative bone count"))?;
    check_bone_count(bone_count)?;
    let mut world_pos = Vec::with_capacity(bone_count);
    let mut joints = Vec::with_capacity(bone_count);

    for ord in 0..bone_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let wpos = flip_z(cur.vec3()?);
        let scaled_w = [
            wpos[0] * MMD_TO_METRES,
            wpos[1] * MMD_TO_METRES,
            wpos[2] * MMD_TO_METRES,
        ];
        let parent_i32 = cur.pmx_signed_idx(bone_idx_sz)?;
        let parent = if parent_i32 >= 0 && (parent_i32 as usize) < bone_count {
            Some(parent_i32 as usize)
        } else {
            None
        };
        let _layer = cur.i32_le()?;
        let flags = cur.u16_le()?;
        if (flags & 0x0001) != 0 {
            let _ = cur.pmx_signed_idx(bone_idx_sz)?;
        } else {
            let _ = cur.vec3()?;
        }
        if (flags & 0x0100) != 0 || (flags & 0x0200) != 0 {
            let _ = cur.pmx_signed_idx(bone_idx_sz)?;
            let _ = cur.f32_le()?;
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
        if (flags & 0x0020) != 0 {
            let _target = cur.pmx_signed_idx(bone_idx_sz)?;
            let _loops = cur.i32_le()?;
            let _lim = cur.f32_le()?;
            let link_cnt = usize::try_from(cur.i32_le()?.max(0)).unwrap_or(0).min(128);
            for _ in 0..link_cnt {
                let _link_bone = cur.pmx_signed_idx(bone_idx_sz)?;
                let has_lim = cur.u8()?;
                if has_lim == 1 {
                    let _lo = cur.vec3()?;
                    let _hi = cur.vec3()?;
                }
            }
        }

        world_pos.push(scaled_w);
        let name = if name_jp.is_empty() {
            name_en.clone()
        } else {
            name_jp
        };
        let inv_bind = [
            1.0,
            0.0,
            0.0,
            -scaled_w[0],
            0.0,
            1.0,
            0.0,
            -scaled_w[1],
            0.0,
            0.0,
            1.0,
            -scaled_w[2],
            0.0,
            0.0,
            0.0,
            1.0,
        ];
        joints.push(RawJoint {
            name,
            alt_name: name_en,
            parent,
            local_translation: scaled_w,
            local_rotation: [0.0, 0.0, 0.0, 1.0],
            local_scale: [1.0, 1.0, 1.0],
            inverse_bind_matrix: Some(inv_bind),
            explicit_humanoid: None,
            ordinal: ord,
        });
    }
    for i in 0..joints.len() {
        if let Some(p) = joints[i].parent {
            joints[i].local_translation = [
                world_pos[i][0] - world_pos[p][0],
                world_pos[i][1] - world_pos[p][1],
                world_pos[i][2] - world_pos[p][2],
            ];
        }
    }

    // Morphs
    let morph_count = usize::try_from(cur.i32_le()?.max(0))
        .map_err(|_| LoaderError::Parse("negative morph count"))?;
    check_morph_count(morph_count)?;
    let mut morphs = Vec::with_capacity(morph_count);
    let mut expr_presets = Vec::with_capacity(morph_count);
    let mut total_deltas = 0usize;

    for _ in 0..morph_count {
        let name_jp = cur.pmx_text(enc)?;
        let name_en = cur.pmx_text(enc)?;
        let _panel = cur.u8()?;
        let morph_type = cur.u8()?;
        let off_cnt = usize::try_from(cur.i32_le()?.max(0))
            .map_err(|_| LoaderError::Parse("negative morph offset count"))?;
        let mut deltas = Vec::new();
        let mut sum_x = 0.0f32;

        match morph_type {
            1 => {
                // Vertex morph
                total_deltas = total_deltas.saturating_add(off_cnt);
                if total_deltas > MAX_MORPH_DELTAS {
                    return Err(LoaderError::LimitExceeded("PMX morph deltas exceed 16M"));
                }
                deltas.reserve(off_cnt.min(vert_count));
                for _ in 0..off_cnt {
                    let v_idx = cur.pmx_unsigned_idx(v_idx_sz)?;
                    let dp = flip_z(cur.vec3()?);
                    if (v_idx as usize) < vert_count {
                        sum_x += vertices[v_idx as usize].position[0];
                        deltas.push(RawMorphDeltaItem {
                            vertex_index: v_idx,
                            d_pos: [
                                dp[0] * MMD_TO_METRES,
                                dp[1] * MMD_TO_METRES,
                                dp[2] * MMD_TO_METRES,
                            ],
                            d_norm: [0.0, 0.0, 0.0],
                        });
                    }
                }
            }
            0 => {
                for _ in 0..off_cnt {
                    let _ = cur.pmx_signed_idx(morph_idx_sz)?;
                    let _ = cur.f32_le()?;
                }
            }
            2 => {
                for _ in 0..off_cnt {
                    let _ = cur.pmx_signed_idx(bone_idx_sz)?;
                    let _ = cur.vec3()?;
                    let _ = cur.vec4()?;
                }
            }
            3..=7 => {
                for _ in 0..off_cnt {
                    let _ = cur.pmx_unsigned_idx(v_idx_sz)?;
                    let _ = cur.vec4()?;
                }
            }
            8 => {
                for _ in 0..off_cnt {
                    let _ = cur.pmx_signed_idx(mat_idx_sz)?;
                    let _ = cur.u8()?;
                    cur.take(108)?;
                }
            }
            _ => return Err(LoaderError::Parse("unknown PMX morph type")),
        }

        if morph_type == 1 && !deltas.is_empty() {
            let centroid_x = sum_x / (deltas.len() as f32);
            let mname = if name_jp.is_empty() { name_en } else { name_jp };
            let m_idx = morphs.len();
            morphs.push(RawMorphTarget {
                name: mname.clone(),
                deltas,
                centroid_x,
            });
            expr_presets.push(RawExprPreset {
                name: mname,
                morph_binds: vec![(m_idx, 1.0)],
            });
        }
    }

    // Display frames (skip cleanly)
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

    // Rigid bodies & Bullet joints (§6.6)
    let mut rbs = Vec::new();
    let mut colliders = Vec::new();
    let mut warnings_mask = 0u32;

    if let Ok(rb_cnt_i32) = cur.i32_le() {
        let rb_cnt = usize::try_from(rb_cnt_i32.max(0))
            .unwrap_or(0)
            .min(MAX_COLLIDERS * 4);
        for _ in 0..rb_cnt {
            let _njp = cur.pmx_text(enc)?;
            let _nen = cur.pmx_text(enc)?;
            let bone = cur.pmx_signed_idx(bone_idx_sz)?;
            let group = cur.u8()?;
            let _mask = cur.u16_le()?;
            let shape = cur.u8()?;
            let size = cur.vec3()?;
            let pos = flip_z(cur.vec3()?);
            let _rot = cur.vec3()?;
            let _mass = cur.f32_le()?;
            let _lin_damp = cur.f32_le()?;
            let ang_damp = cur.f32_le()?;
            let _rest = cur.f32_le()?;
            let _fric = cur.f32_le()?;
            let phys_mode = cur.u8()?;
            if phys_mode == 0
                && bone >= 0
                && (bone as usize) < joints.len()
                && colliders.len() < MAX_COLLIDERS
            {
                let b_idx = bone as usize;
                let w = world_pos[b_idx];
                let off = [
                    pos[0] * MMD_TO_METRES - w[0],
                    pos[1] * MMD_TO_METRES - w[1],
                    pos[2] * MMD_TO_METRES - w[2],
                ];
                let radius = (size[0] * MMD_TO_METRES).max(0.01);
                colliders.push(RawCollider {
                    joint_index: b_idx,
                    group_bit: u32::from(group & 31),
                    shape_kind: if shape == 2 {
                        AETHER_COLLIDER_SHAPE_CAPSULE
                    } else {
                        AETHER_COLLIDER_SHAPE_SPHERE
                    },
                    radius,
                    offset: off,
                    tail: [off[0], off[1] - size[1] * 0.5 * MMD_TO_METRES, off[2]],
                });
            }
            rbs.push(RawRb {
                bone,
                group,
                shape,
                size,
                pos,
                ang_damp,
                phys_mode,
            });
        }
    }

    let mut spring_chains = Vec::new();
    if let Ok(jnt_cnt_i32) = cur.i32_le() {
        let jnt_cnt = usize::try_from(jnt_cnt_i32.max(0)).unwrap_or(0).min(4096);
        // Partition MMD dynamic constraints into tree chains (§6.6)
        let mut children_adj: Vec<Vec<(usize, f32)>> = vec![Vec::new(); rbs.len()];
        let mut parent_count = vec![0usize; rbs.len()];

        for _ in 0..jnt_cnt {
            let _njp = cur.pmx_text(enc)?;
            let _nen = cur.pmx_text(enc)?;
            let _jtype = cur.u8()?;
            let rb_a = cur.pmx_signed_idx(rb_idx_sz)?;
            let rb_b = cur.pmx_signed_idx(rb_idx_sz)?;
            let _pos = cur.vec3()?;
            let _rot = cur.vec3()?;
            let _pmin = cur.vec3()?;
            let _pmax = cur.vec3()?;
            let _rmin = cur.vec3()?;
            let _rmax = cur.vec3()?;
            let _spos = cur.vec3()?;
            let srot = cur.vec3()?;

            if rb_a >= 0 && rb_b >= 0 && (rb_a as usize) < rbs.len() && (rb_b as usize) < rbs.len()
            {
                let a = rb_a as usize;
                let b = rb_b as usize;
                let k_rot = (srot[0].abs() + srot[1].abs() + srot[2].abs()) / 3.0;
                if rbs[b].phys_mode > 0 {
                    if parent_count[b] > 0 {
                        warnings_mask |= aether_ffi::AETHER_WARN_MMD_CROSS_CHAIN;
                    } else {
                        children_adj[a].push((b, k_rot));
                        parent_count[b] += 1;
                    }
                } else if rbs[a].phys_mode > 0 {
                    warnings_mask |= aether_ffi::AETHER_WARN_MMD_CROSS_CHAIN;
                }
            }
        }

        // Start a spring chain from every kinematic RB that feeds dynamic children (or dynamic root)
        for root_rb in 0..rbs.len() {
            if rbs[root_rb].phys_mode == 0 && !children_adj[root_rb].is_empty() {
                for &(first_dyn, first_k) in &children_adj[root_rb] {
                    let mut chain_nodes = Vec::new();
                    let mut stack = vec![(first_dyn, first_k)];
                    while let Some((rb_i, k_rot)) = stack.pop() {
                        let rb = &rbs[rb_i];
                        if rb.bone >= 0 && (rb.bone as usize) < joints.len() {
                            let _ = (rb.group, rb.shape, rb.pos);
                            chain_nodes.push(RawSpringNode {
                                joint_index: rb.bone as usize,
                                hit_radius: (rb.size[0] * MMD_TO_METRES).clamp(0.01, 0.25),
                                stiffness: (k_rot / 100.0).clamp(0.05, 4.0),
                                drag_force: rb.ang_damp.clamp(0.0, 1.0),
                                gravity_power: 1.0,
                                gravity_dir: [0.0, -1.0, 0.0],
                                from_mmd: true,
                            });
                        }
                        for &(nxt, nk) in &children_adj[rb_i] {
                            stack.push((nxt, nk));
                        }
                    }
                    if !chain_nodes.is_empty() {
                        spring_chains.push(RawSpringChain {
                            joints: chain_nodes,
                            center_joint: None,
                            collider_group_mask: 0xFFFF,
                        });
                    }
                }
            }
        }
    }

    Ok(ParsedAvatar {
        source_format: AETHER_SOURCE_FORMAT_PMX,
        base_dir: base_dir.to_path_buf(),
        unit_meta: SourceUnitMetadata {
            explicit_unit_scale: Some(MMD_TO_METRES),
            resolved_unit_scale: MMD_TO_METRES,
            scale_guessed: false,
            left_handed_flipped: true,
        },
        warnings_mask,
        vertices,
        submeshes,
        materials,
        joints,
        morphs,
        expr_presets,
        spring_chains,
        colliders,
        textures,
    })
}

fn parse_pmd(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    let mut cur = Cursor::new(data);
    if cur.take(3)? != b"Pmd" {
        return Err(LoaderError::Parse("bad PMD signature"));
    }
    let _ver = cur.f32_le()?;
    let _model_name = decode_sjis_cstr(cur.take(20)?);
    let _comment = decode_sjis_cstr(cur.take(256)?);

    let vert_count =
        usize::try_from(cur.u32_le()?).map_err(|_| LoaderError::Parse("PMD vertex overflow"))?;
    check_vertex_count(vert_count)?;
    let mut vertices = Vec::with_capacity(vert_count);
    for _ in 0..vert_count {
        let p = flip_z(cur.vec3()?);
        let n = flip_z(cur.vec3()?);
        let uv0 = cur.vec2()?;
        let b0 = cur.u16_le()?;
        let b1 = cur.u16_le()?;
        let w0 = f32::from(cur.u8()?) / 100.0;
        let edge_flag = cur.u8()?;
        vertices.push(RawVertex {
            position: [
                p[0] * MMD_TO_METRES,
                p[1] * MMD_TO_METRES,
                p[2] * MMD_TO_METRES,
            ],
            normal: n,
            uv0,
            uv1: [0.0, 0.0],
            color: [255, 255, 255, 255],
            joints: [b0, b1, 0, 0, 0, 0, 0, 0],
            weights: [w0, 1.0 - w0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            edge_scale: if edge_flag == 0 { 1.0 } else { 0.0 },
            flatten_override: -1.0,
        });
    }

    let index_count =
        usize::try_from(cur.u32_le()?).map_err(|_| LoaderError::Parse("PMD index overflow"))?;
    check_index_count(index_count)?;
    if index_count % 3 != 0 {
        return Err(LoaderError::Parse("PMD index count not multiple of 3"));
    }
    let mut raw_indices = Vec::with_capacity(index_count);
    for _ in 0..(index_count / 3) {
        let i0 = u32::from(cur.u16_le()?);
        let i1 = u32::from(cur.u16_le()?);
        let i2 = u32::from(cur.u16_le()?);
        if (i0 as usize) >= vert_count || (i1 as usize) >= vert_count || (i2 as usize) >= vert_count
        {
            return Err(LoaderError::Parse("PMD index out of bounds"));
        }
        raw_indices.push(i0);
        raw_indices.push(i2);
        raw_indices.push(i1);
    }

    let mat_count =
        usize::try_from(cur.u32_le()?).map_err(|_| LoaderError::Parse("PMD material overflow"))?;
    if mat_count > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded("PMD materials > 1024"));
    }
    let mut materials = Vec::with_capacity(mat_count);
    let mut submeshes = Vec::with_capacity(mat_count);
    let mut textures = Vec::new();
    let mut idx_cursor = 0usize;

    for m_idx in 0..mat_count {
        let diffuse = cur.vec4()?;
        let _spec_pow = cur.f32_le()?;
        let _spec = cur.vec3()?;
        let _ambient = cur.vec3()?;
        let _toon = cur.u8()?;
        let edge_flag = cur.u8()?;
        let face_cnt = usize::try_from(cur.u32_le()?).unwrap_or(0);
        let tex_str = decode_sjis_cstr(cur.take(20)?);

        let end_idx = idx_cursor.saturating_add(face_cnt).min(raw_indices.len());
        let sub_indices = raw_indices[idx_cursor..end_idx].to_vec();
        idx_cursor = end_idx;

        let mut tex_base = None;
        let mut tex_matcap = None;
        for (part_i, part) in tex_str.split('*').enumerate() {
            let trimmed = part.trim().replace('\\', "/");
            if trimmed.is_empty() {
                continue;
            }
            let t_idx = textures.len() as u32;
            let path = base_dir.join(&trimmed);
            textures.push(RawTextureDesc {
                name: trimmed,
                source: if path.is_file() {
                    RawTextureSource::ExternalFile(path)
                } else {
                    RawTextureSource::Missing
                },
                width: 1,
                height: 1,
                srgb: true,
                usage_flags: aether_ffi::AETHER_TEX_USAGE_COLOR,
                content_hash_lo: 0,
                rgba_byte_len: 4,
                cached_bc7_path: None,
                cached_bc7_byte_len: 0,
                pixel_offset: 0,
                pixel_size: 4,
                final_format: aether_ffi::AETHER_TEX_FORMAT_RGBA8_SRGB,
            });
            if part_i == 0 {
                tex_base = Some(t_idx);
            } else {
                tex_matcap = Some(t_idx);
            }
        }

        let name = format!("pmd_mat_{m_idx}");
        materials.push(RawMaterial {
            name: name.clone(),
            base_color: diffuse,
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: 4.0,
            rim_lift: 0.0,
            outline_color: [0.1, 0.05, 0.05],
            outline_lighting_mix: 0.25,
            outline_width: if edge_flag != 0 { 0.002 } else { 0.0 },
            outline_screen_space: false,
            emissive: [0.0, 0.0, 0.0],
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: 1.0,
            matcap_strength: if tex_matcap.is_some() { 0.6 } else { 0.0 },
            gi_equalization: 0.15,
            shading_shift: -0.05,
            shading_toony: 0.85,
            uv_scroll: [0.0, 0.0],
            uv_rotation: 0.0,
            alpha_mode: AETHER_ALPHA_MODE_OPAQUE,
            alpha_cutoff: 0.5,
            double_sided: false,
            unlit: false,
            has_vrm_mtoon: false,
            render_queue_offset: 0,
            flatten_override: None,
            tex_base,
            tex_shade: None,
            tex_shading_shift: None,
            tex_normal: None,
            tex_emissive: None,
            tex_rim: None,
            tex_matcap,
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
        });
        submeshes.push(RawSubmesh {
            name,
            indices: sub_indices,
            material_index: m_idx as u32,
            is_eye_or_brow_hint: false,
        });
    }

    let bone_count = usize::from(cur.u16_le()?);
    check_bone_count(bone_count)?;
    let mut world_pos = Vec::with_capacity(bone_count);
    let mut joints = Vec::with_capacity(bone_count);
    for ord in 0..bone_count {
        let name = decode_sjis_cstr(cur.take(20)?);
        let parent_u16 = cur.u16_le()?;
        let _tail = cur.u16_le()?;
        let _kind = cur.u8()?;
        let _ik_parent = cur.u16_le()?;
        let pos = flip_z(cur.vec3()?);
        let scaled = [
            pos[0] * MMD_TO_METRES,
            pos[1] * MMD_TO_METRES,
            pos[2] * MMD_TO_METRES,
        ];
        let parent = if parent_u16 != 0xFFFF && usize::from(parent_u16) < bone_count {
            Some(usize::from(parent_u16))
        } else {
            None
        };
        world_pos.push(scaled);
        joints.push(RawJoint {
            name: name.clone(),
            alt_name: String::new(),
            parent,
            local_translation: scaled,
            local_rotation: [0.0, 0.0, 0.0, 1.0],
            local_scale: [1.0, 1.0, 1.0],
            inverse_bind_matrix: Some([
                1.0, 0.0, 0.0, -scaled[0], 0.0, 1.0, 0.0, -scaled[1], 0.0, 0.0, 1.0, -scaled[2],
                0.0, 0.0, 0.0, 1.0,
            ]),
            explicit_humanoid: None,
            ordinal: ord,
        });
    }
    for i in 0..joints.len() {
        if let Some(p) = joints[i].parent {
            joints[i].local_translation = [
                world_pos[i][0] - world_pos[p][0],
                world_pos[i][1] - world_pos[p][1],
                world_pos[i][2] - world_pos[p][2],
            ];
        }
    }

    // IK skip
    if let Ok(ik_cnt) = cur.u16_le() {
        for _ in 0..usize::from(ik_cnt) {
            cur.take(4)?;
            let chain_len = usize::from(cur.u8()?);
            cur.take(6)?;
            cur.take(chain_len.saturating_mul(2))?;
        }
    }

    // Morphs (skin[0] is base index table; skin[1..] index into base)
    let mut morphs = Vec::new();
    let mut expr_presets = Vec::new();
    if let Ok(skin_cnt) = cur.u16_le() {
        check_morph_count(usize::from(skin_cnt))?;
        let mut base_indices: Vec<u32> = Vec::new();
        for s_i in 0..usize::from(skin_cnt) {
            let mname = decode_sjis_cstr(cur.take(20)?);
            let vc = usize::try_from(cur.u32_le()?).unwrap_or(0).min(vert_count);
            let _skin_type = cur.u8()?;
            let mut deltas = Vec::new();
            let mut sum_x = 0.0f32;
            for _ in 0..vc {
                let raw_i = cur.u32_le()?;
                let dpos = flip_z(cur.vec3()?);
                if s_i == 0 {
                    base_indices.push(raw_i);
                } else if let Some(&v_idx) = base_indices.get(raw_i as usize)
                    && (v_idx as usize) < vert_count
                {
                    sum_x += vertices[v_idx as usize].position[0];
                    deltas.push(RawMorphDeltaItem {
                        vertex_index: v_idx,
                        d_pos: [
                            dpos[0] * MMD_TO_METRES,
                            dpos[1] * MMD_TO_METRES,
                            dpos[2] * MMD_TO_METRES,
                        ],
                        d_norm: [0.0, 0.0, 0.0],
                    });
                }
            }
            if s_i > 0 && !deltas.is_empty() {
                let cx = sum_x / (deltas.len() as f32);
                let m_idx = morphs.len();
                morphs.push(RawMorphTarget {
                    name: mname.clone(),
                    deltas,
                    centroid_x: cx,
                });
                expr_presets.push(RawExprPreset {
                    name: mname,
                    morph_binds: vec![(m_idx, 1.0)],
                });
            }
        }
    }

    Ok(ParsedAvatar {
        source_format: AETHER_SOURCE_FORMAT_PMD,
        base_dir: base_dir.to_path_buf(),
        unit_meta: SourceUnitMetadata {
            explicit_unit_scale: Some(MMD_TO_METRES),
            resolved_unit_scale: MMD_TO_METRES,
            scale_guessed: false,
            left_handed_flipped: true,
        },
        warnings_mask: 0,
        vertices,
        submeshes,
        materials,
        joints,
        morphs,
        expr_presets,
        spring_chains: Vec::new(),
        colliders: Vec::new(),
        textures,
    })
}
