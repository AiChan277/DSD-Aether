//! Safe pure-Rust bounds-checked PMX 2.0/2.1 (`aether-asset-pmx`) and PMD 1.0 (Shift-JIS) adapter (`§6.2`).
//!
//! Converts MMD left-handed coordinates & clockwise winding into AETHER right-handed Y-up metres
//! (`z = -z`, triangle winding reversed, `pos * 0.08`) and extracts `SDEF`, `Append`, `CCD-IK`,
//! all 5 morph types (`Group`, `Vertex`, `Bone`, `UV`, `Material`), Ray-MMD material classification,
//! and Bullet `RigidBody` + `6-DOF Spring Joint` constraints.

use std::path::Path;

use aether_asset_pmx::{
    MMD_TO_METRES, PmxError, PmxMorphData, PmxWeightType, parse_pmx_bytes, pmx_fnv1a_lower,
};
use aether_ffi::{
    AETHER_ALPHA_MODE_OPAQUE, AETHER_COLLIDER_SHAPE_CAPSULE, AETHER_COLLIDER_SHAPE_SPHERE,
    AETHER_INVALID_INDEX, AETHER_PMX_MORPH_BONE, AETHER_PMX_MORPH_GROUP, AETHER_PMX_MORPH_MATERIAL,
    AETHER_PMX_MORPH_UV, AETHER_PMX_MORPH_VERTEX, AETHER_SOURCE_FORMAT_PMD,
    AETHER_SOURCE_FORMAT_PMX, AetherPmxBoneExt, AetherPmxBoneMorphOffset,
    AetherPmxConstraint6DofDesc, AetherPmxGroupMorphChild, AetherPmxIkLink,
    AetherPmxMaterialMorphDelta, AetherPmxMorphChannelDesc, AetherPmxRigidBodyDesc,
    AetherPmxSdefVertex, AetherPmxUvMorphDelta,
};
use encoding_rs::SHIFT_JIS;

use crate::LoaderError;
use crate::ir::{
    ParsedAvatar, RawCollider, RawExprPreset, RawJoint, RawMaterial, RawMorphDeltaItem,
    RawMorphTarget, RawPmxAux, RawSpringChain, RawSpringNode, RawSubmesh, RawTextureDesc,
    RawTextureSource, RawVertex, SourceUnitMetadata,
};
use crate::limits::{
    MAX_COLLIDERS, MAX_MATERIALS, MAX_MORPH_DELTAS, check_bone_count, check_index_count,
    check_morph_count, check_vertex_count,
};
use crate::retarget::map_preset_to_slots;

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

    fn u32_le(&mut self) -> Result<u32, LoaderError> {
        let b = self.take(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
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

fn parse_pmx(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    let doc = parse_pmx_bytes(data).map_err(|e| match e {
        PmxError::Parse(msg) => LoaderError::Parse(msg),
        PmxError::LimitExceeded(msg) => LoaderError::LimitExceeded(msg),
    })?;

    check_vertex_count(doc.vertices.len())?;
    check_index_count(doc.indices.len())?;
    check_bone_count(doc.bones.len())?;
    check_morph_count(doc.morphs.len())?;
    if doc.materials.len() > MAX_MATERIALS || doc.texture_paths.len() > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded(
            "PMX material/texture count > 1024",
        ));
    }

    let mut vertices = Vec::with_capacity(doc.vertices.len());
    let mut sdef_vertices = Vec::with_capacity(doc.vertices.len());
    let mut any_sdef = false;
    for pv in &doc.vertices {
        let mut joints = [0u16; 8];
        let mut weights = [0.0f32; 8];
        joints[..4].copy_from_slice(&pv.joints);
        weights[..4].copy_from_slice(&pv.weights);
        let is_sdef = pv.weight_type == PmxWeightType::Sdef;
        if is_sdef {
            any_sdef = true;
        }
        sdef_vertices.push(AetherPmxSdefVertex {
            c: pv.sdef_c_m,
            flags: u32::from(is_sdef),
            r0: pv.sdef_r0_m,
            _pad0: 0,
            r1: pv.sdef_r1_m,
            _pad1: 0,
        });
        vertices.push(RawVertex {
            position: pv.position_m,
            normal: pv.normal,
            uv0: pv.uv0,
            uv1: pv.uv1,
            color: [255, 255, 255, 255],
            joints,
            weights,
            edge_scale: pv.edge_scale,
            flatten_override: -1.0,
        });
    }
    if !any_sdef {
        sdef_vertices.clear();
    }

    let tex_count = doc.texture_paths.len();
    let mut textures = Vec::with_capacity(tex_count);
    for (idx, cleaned) in doc.texture_paths.iter().enumerate() {
        let resolved = resolve_pmx_texture_path(base_dir, cleaned);
        textures.push(RawTextureDesc {
            name: if cleaned.is_empty() {
                format!("pmx_tex_{idx}")
            } else {
                cleaned.clone()
            },
            source: match resolved {
                Some(path) => RawTextureSource::ExternalFile(path),
                None => RawTextureSource::Missing,
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

    let mut materials = Vec::with_capacity(doc.materials.len());
    let mut submeshes = Vec::with_capacity(doc.materials.len());
    let mut idx_cursor = 0usize;
    let to_u32 = |v: i32| -> Option<u32> {
        if v >= 0 && (v as usize) < tex_count {
            Some(v as u32)
        } else {
            None
        }
    };

    for (m_idx, pm) in doc.materials.iter().enumerate() {
        let name = if pm.name_en.is_empty() {
            pm.name_jp.clone()
        } else {
            pm.name_en.clone()
        };
        let end_idx = idx_cursor
            .saturating_add(pm.surface_count)
            .min(doc.indices.len());
        let sub_indices = doc.indices[idx_cursor..end_idx].to_vec();
        idx_cursor = end_idx;

        let cls = pm.classification;
        let emissive = if cls.is_emissive {
            [
                pm.ambient[0].max(pm.diffuse[0]) * 1.25,
                pm.ambient[1].max(pm.diffuse[1]) * 1.25,
                pm.ambient[2].max(pm.diffuse[2]) * 1.25,
            ]
        } else {
            [0.0, 0.0, 0.0]
        };

        materials.push(RawMaterial {
            name: name.clone(),
            base_color: pm.diffuse,
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: if cls.is_glass { 2.5 } else { 4.0 },
            rim_lift: 0.0,
            outline_color: [pm.edge_color[0], pm.edge_color[1], pm.edge_color[2]],
            outline_lighting_mix: 0.25,
            outline_width: if (pm.draw_flags & 0x10) != 0 {
                (pm.edge_size * 0.002).clamp(0.0, 0.02)
            } else {
                0.0
            },
            outline_screen_space: false,
            emissive,
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: if pm.sphere_mode == 2 { 1.0 } else { 0.0 },
            matcap_strength: if pm.sphere_mode == 0 { 0.0 } else { 0.6 },
            gi_equalization: 0.15,
            shading_shift: -0.05,
            shading_toony: 0.85,
            uv_scroll: [0.0, 0.0],
            uv_rotation: 0.0,
            alpha_mode: if pm.diffuse[3] < 0.99 {
                aether_ffi::AETHER_ALPHA_MODE_BLEND
            } else {
                AETHER_ALPHA_MODE_OPAQUE
            },
            alpha_cutoff: 0.5,
            double_sided: (pm.draw_flags & 0x01) != 0,
            unlit: false,
            has_vrm_mtoon: false,
            cast_shadow: (pm.draw_flags & 0x04) != 0,
            edge_enabled: (pm.draw_flags & 0x10) != 0,
            is_pmx: true,
            render_queue_offset: m_idx as i32,
            flatten_override: None,
            tex_base: to_u32(pm.tex_idx),
            tex_shade: if pm.toon_flag == 0 {
                to_u32(pm.toon_tex_or_builtin)
            } else {
                None
            },
            tex_shading_shift: None,
            tex_normal: None,
            tex_emissive: None,
            tex_rim: None,
            tex_matcap: if pm.sphere_mode > 0 {
                to_u32(pm.sphere_tex)
            } else {
                None
            },
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
            shader_profile: Some(cls.shader_profile),
        });

        submeshes.push(RawSubmesh {
            name,
            indices: sub_indices,
            material_index: m_idx as u32,
            is_eye_or_brow_hint: cls.is_eye_or_brow,
        });
    }

    let bone_count = doc.bones.len();
    let mut world_pos = Vec::with_capacity(bone_count);
    let mut joints = Vec::with_capacity(bone_count);
    let mut bone_exts = Vec::with_capacity(bone_count);
    let mut ik_links = Vec::new();

    for (ord, pb) in doc.bones.iter().enumerate() {
        let scaled_w = pb.world_pos_m;
        world_pos.push(scaled_w);
        let name = if pb.name_jp.is_empty() {
            pb.name_en.clone()
        } else {
            pb.name_jp.clone()
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
            alt_name: pb.name_en.clone(),
            parent: pb.parent,
            local_translation: scaled_w,
            local_rotation: [0.0, 0.0, 0.0, 1.0],
            local_scale: [1.0, 1.0, 1.0],
            inverse_bind_matrix: Some(inv_bind),
            explicit_humanoid: None,
            ordinal: ord,
        });

        let ik_off = ik_links.len() as u32;
        for lnk in &pb.ik_links {
            ik_links.push(AetherPmxIkLink {
                bone_index: lnk.bone_index,
                has_limit: u32::from(lnk.has_limit),
                limit_min_rad: lnk.limit_min_rad,
                limit_max_rad: lnk.limit_max_rad,
            });
        }
        bone_exts.push(AetherPmxBoneExt {
            append_parent: pb.append_parent.map_or(-1, |idx| idx as i32),
            append_ratio: pb.append_ratio,
            append_flags: pb.append_flags,
            ik_target: pb.ik_target.map_or(-1, |idx| idx as i32),
            ik_loops: pb.ik_loops,
            ik_limit_rad: pb.ik_limit_rad,
            ik_link_offset: ik_off,
            ik_link_count: pb.ik_links.len() as u32,
            transform_level: pb.transform_level,
            _pad: [0; 3],
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

    let mut morphs = Vec::new();
    let mut expr_presets = Vec::new();
    let mut pmx_to_vertex_morph = vec![None::<usize>; doc.morphs.len()];
    let mut morph_channels = Vec::with_capacity(doc.morphs.len());
    let mut group_children = Vec::new();
    let mut uv_morphs = Vec::new();
    let mut mat_morphs = Vec::new();
    let mut bone_morphs = Vec::new();
    let mut total_deltas = 0usize;

    for (pm_idx, pm) in doc.morphs.iter().enumerate() {
        let mname = if pm.name_jp.is_empty() {
            pm.name_en.clone()
        } else {
            pm.name_jp.clone()
        };
        let mut centroid_x = 0.0f32;
        let mut v_morph_idx = AETHER_INVALID_INDEX;
        let child_off = group_children.len() as u32;
        let mut child_cnt = 0u32;
        let bone_off = bone_morphs.len() as u32;
        let mut bone_cnt = 0u32;
        let uv_off = uv_morphs.len() as u32;
        let mut uv_cnt = 0u32;
        let mat_off = mat_morphs.len() as u32;
        let mut mat_cnt = 0u32;

        let morph_kind = match &pm.data {
            PmxMorphData::Group(children) => {
                for &(c_idx, w) in children {
                    group_children.push(AetherPmxGroupMorphChild {
                        child_morph_index: c_idx as u32,
                        weight: w,
                        _pad: [0; 2],
                    });
                    child_cnt += 1;
                }
                AETHER_PMX_MORPH_GROUP
            }
            PmxMorphData::Vertex(v_deltas) => {
                total_deltas = total_deltas.saturating_add(v_deltas.len());
                if total_deltas > MAX_MORPH_DELTAS {
                    return Err(LoaderError::LimitExceeded("PMX morph deltas exceed 16M"));
                }
                if !v_deltas.is_empty() {
                    let mut deltas = Vec::with_capacity(v_deltas.len());
                    let mut sum_x = 0.0f32;
                    for &(vi, d_pos) in v_deltas {
                        if (vi as usize) < vertices.len() {
                            sum_x += vertices[vi as usize].position[0];
                            deltas.push(RawMorphDeltaItem {
                                vertex_index: vi,
                                d_pos,
                                d_norm: [0.0, 0.0, 0.0],
                            });
                        }
                    }
                    if !deltas.is_empty() {
                        centroid_x = sum_x / (deltas.len() as f32);
                        let m_idx = morphs.len();
                        pmx_to_vertex_morph[pm_idx] = Some(m_idx);
                        v_morph_idx = m_idx as u32;
                        morphs.push(RawMorphTarget {
                            name: mname.clone(),
                            deltas,
                            centroid_x,
                        });
                        expr_presets.push(RawExprPreset {
                            name: mname.clone(),
                            morph_binds: vec![(m_idx, 1.0)],
                        });
                    }
                }
                AETHER_PMX_MORPH_VERTEX
            }
            PmxMorphData::Bone(b_offs) => {
                for bo in b_offs {
                    bone_morphs.push(AetherPmxBoneMorphOffset {
                        bone_index: bo.bone_index,
                        _pad0: 0,
                        translation: bo.translation_m,
                        rotation_quat: bo.rotation_quat,
                        _pad1: 0,
                    });
                    bone_cnt += 1;
                }
                AETHER_PMX_MORPH_BONE
            }
            PmxMorphData::Uv(uv_offs) => {
                for &(vi, duv) in uv_offs {
                    uv_morphs.push(AetherPmxUvMorphDelta {
                        vertex_index: vi,
                        _pad0: 0,
                        duv,
                    });
                    uv_cnt += 1;
                }
                AETHER_PMX_MORPH_UV
            }
            PmxMorphData::Material(m_offs) => {
                for mo in m_offs {
                    mat_morphs.push(AetherPmxMaterialMorphDelta {
                        material_index: mo.material_index,
                        op_mode: u32::from(mo.op_mode),
                        spec_pow: mo.spec_pow,
                        edge_size: mo.edge_size,
                        diffuse: mo.diffuse,
                        specular: [mo.specular[0], mo.specular[1], mo.specular[2], 1.0],
                        ambient: [mo.ambient[0], mo.ambient[1], mo.ambient[2], 1.0],
                        edge_color: mo.edge_color,
                        tex_tint: mo.tex_tint,
                        sphere_tint: mo.sphere_tint,
                        toon_tint: mo.toon_tint,
                    });
                    mat_cnt += 1;
                }
                AETHER_PMX_MORPH_MATERIAL
            }
            PmxMorphData::Other => u32::from(pm.morph_type),
        };

        let mapped_slots = map_preset_to_slots(&mname, centroid_x, false);
        let appendix_b_slot = mapped_slots
            .first()
            .map_or(AETHER_INVALID_INDEX, |&(slot, _)| slot);

        morph_channels.push(AetherPmxMorphChannelDesc {
            morph_kind,
            appendix_b_slot,
            vertex_morph_index: v_morph_idx,
            name_hash: pmx_fnv1a_lower(&mname),
            child_offset: child_off,
            child_count: child_cnt,
            bone_offset: bone_off,
            bone_count: bone_cnt,
            uv_offset: uv_off,
            uv_count: uv_cnt,
            mat_offset: mat_off,
            mat_count: mat_cnt,
        });
    }

    for pm in &doc.morphs {
        if let PmxMorphData::Group(children) = &pm.data {
            let mut binds = Vec::new();
            for &(c_idx, w) in children {
                if let Some(Some(vm_idx)) = pmx_to_vertex_morph.get(c_idx) {
                    binds.push((*vm_idx, w));
                }
            }
            if !binds.is_empty() {
                let mname = if pm.name_jp.is_empty() {
                    pm.name_en.clone()
                } else {
                    pm.name_jp.clone()
                };
                expr_presets.push(RawExprPreset {
                    name: mname,
                    morph_binds: binds,
                });
            }
        }
    }

    let mut colliders = Vec::new();
    let mut rigid_bodies = Vec::with_capacity(doc.rigid_bodies.len());
    let mut warnings_mask = 0u32;

    for rb in &doc.rigid_bodies {
        if rb.phys_mode == 0
            && rb.bone_index >= 0
            && (rb.bone_index as usize) < joints.len()
            && colliders.len() < MAX_COLLIDERS
        {
            let b_idx = rb.bone_index as usize;
            let w = world_pos[b_idx];
            let off = [rb.pos_m[0] - w[0], rb.pos_m[1] - w[1], rb.pos_m[2] - w[2]];
            let radius = rb.size_m[0].max(0.01);
            colliders.push(RawCollider {
                joint_index: b_idx,
                group_bit: u32::from(rb.group_index & 31),
                shape_kind: if rb.shape_kind == 2 {
                    AETHER_COLLIDER_SHAPE_CAPSULE
                } else {
                    AETHER_COLLIDER_SHAPE_SPHERE
                },
                radius,
                offset: off,
                tail: [off[0], off[1] - rb.size_m[1] * 0.5, off[2]],
            });
        }
        rigid_bodies.push(AetherPmxRigidBodyDesc {
            bone_index: rb.bone_index,
            group_index: u32::from(rb.group_index),
            collision_mask: u32::from(rb.collision_mask),
            shape_kind: u32::from(rb.shape_kind),
            phys_mode: u32::from(rb.phys_mode),
            _pad0: 0,
            size: rb.size_m,
            mass: rb.mass,
            pos_ms: rb.pos_m,
            linear_damping: rb.linear_damping,
            rot_euler_rh: rb.rot_euler_rh,
            angular_damping: rb.angular_damping,
            restitution: rb.restitution,
            friction: rb.friction,
        });
    }

    let mut constraints_6dof = Vec::with_capacity(doc.constraints.len());
    let mut children_adj: Vec<Vec<(usize, f32)>> = vec![Vec::new(); doc.rigid_bodies.len()];
    let mut parent_count = vec![0usize; doc.rigid_bodies.len()];
    for jc in &doc.constraints {
        constraints_6dof.push(AetherPmxConstraint6DofDesc {
            rb_a: jc.rb_a,
            rb_b: jc.rb_b,
            spring_pos: jc.spring_pos,
            spring_rot: jc.spring_rot,
            pos_ms: jc.pos_m,
            rot_euler_rh: jc.rot_euler_rh,
            pos_min: jc.pos_min_m,
            pos_max: jc.pos_max_m,
            rot_min: jc.rot_min_rad,
            rot_max: jc.rot_max_rad,
            _pad: [0; 2],
        });

        if jc.rb_a >= 0
            && jc.rb_b >= 0
            && (jc.rb_a as usize) < doc.rigid_bodies.len()
            && (jc.rb_b as usize) < doc.rigid_bodies.len()
        {
            let a = jc.rb_a as usize;
            let b = jc.rb_b as usize;
            let k_rot =
                (jc.spring_rot[0].abs() + jc.spring_rot[1].abs() + jc.spring_rot[2].abs()) / 3.0;
            if doc.rigid_bodies[b].phys_mode > 0 {
                if parent_count[b] > 0 {
                    warnings_mask |= aether_ffi::AETHER_WARN_MMD_CROSS_CHAIN;
                } else {
                    children_adj[a].push((b, k_rot));
                    parent_count[b] += 1;
                }
            } else if doc.rigid_bodies[a].phys_mode > 0 {
                warnings_mask |= aether_ffi::AETHER_WARN_MMD_CROSS_CHAIN;
            }
        }
    }

    let mut spring_chains = Vec::new();
    for root_rb in 0..doc.rigid_bodies.len() {
        if doc.rigid_bodies[root_rb].phys_mode == 0 && !children_adj[root_rb].is_empty() {
            for &(first_dyn, first_k) in &children_adj[root_rb] {
                let mut chain_nodes = Vec::new();
                let mut stack = vec![(first_dyn, first_k)];
                while let Some((rb_i, k_rot)) = stack.pop() {
                    let rb = &doc.rigid_bodies[rb_i];
                    if rb.bone_index >= 0 && (rb.bone_index as usize) < joints.len() {
                        chain_nodes.push(RawSpringNode {
                            joint_index: rb.bone_index as usize,
                            hit_radius: rb.size_m[0].clamp(0.01, 0.25),
                            stiffness: (k_rot / 100.0).clamp(0.05, 4.0),
                            drag_force: rb.angular_damping.clamp(0.0, 1.0),
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

    finalize_pmx_material_alpha_tiers(
        &mut materials,
        &submeshes,
        &vertices,
        &textures,
        &doc.morphs,
    );

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
        pmx_aux: Some(RawPmxAux {
            sdef_vertices,
            bone_exts,
            ik_links,
            morph_channels,
            group_children,
            uv_morphs,
            mat_morphs,
            bone_morphs,
            rigid_bodies,
            constraints_6dof,
        }),
    })
}

/// Resolves a PMX/PMD relative texture path against `base_dir`, normalizing UTF-16 BOMs,
/// slashes, relative prefixes, and case-insensitive directory entries.
fn resolve_pmx_texture_path(base_dir: &Path, raw_rel: &str) -> Option<std::path::PathBuf> {
    let cleaned = raw_rel
        .trim()
        .trim_start_matches('\u{FEFF}')
        .trim_end_matches('\0')
        .replace('\\', "/");
    if cleaned.is_empty() {
        return None;
    }
    let rel = cleaned.trim_start_matches("./");
    let direct = base_dir.join(rel);
    if direct.is_file() {
        return Some(direct);
    }

    // Component-by-component case-insensitive filesystem walk inside base_dir.
    let mut cur = base_dir.to_path_buf();
    let mut walked_ok = true;
    for comp in rel.split('/').filter(|c| !c.is_empty() && *c != ".") {
        if comp == ".." {
            return None;
        }
        let exact = cur.join(comp);
        if exact.exists() {
            cur = exact;
            continue;
        }
        let target_low = comp.to_ascii_lowercase();
        let mut matched = None;
        if let Ok(entries) = std::fs::read_dir(&cur) {
            for ent in entries.flatten() {
                if let Some(fname) = ent.file_name().to_str()
                    && fname.to_ascii_lowercase() == target_low
                {
                    matched = Some(ent.path());
                    break;
                }
            }
        }
        if let Some(m) = matched {
            cur = m;
        } else {
            walked_ok = false;
            break;
        }
    }
    if walked_ok && cur.is_file() {
        return Some(cur);
    }

    // Basename fallback in base_dir and common MMD texture subfolders.
    let file_name = rel.rsplit('/').next()?.trim();
    if file_name.is_empty() {
        return None;
    }
    let fname_low = file_name.to_ascii_lowercase();
    for sub in [
        "", "tex", "Tex", "textures", "Textures", "texture", "Texture",
    ] {
        let dir = if sub.is_empty() {
            base_dir.to_path_buf()
        } else {
            base_dir.join(sub)
        };
        if let Ok(entries) = std::fs::read_dir(&dir) {
            for ent in entries.flatten() {
                let p = ent.path();
                if p.is_file()
                    && p.file_name()
                        .and_then(|s| s.to_str())
                        .is_some_and(|s| s.to_ascii_lowercase() == fname_low)
                {
                    return Some(p);
                }
            }
        }
    }
    None
}

fn finalize_pmx_material_alpha_tiers(
    materials: &mut [RawMaterial],
    submeshes: &[RawSubmesh],
    vertices: &[RawVertex],
    textures: &[RawTextureDesc],
    morphs: &[aether_asset_pmx::PmxMorph],
) {
    let mut morph_affects_alpha = vec![false; materials.len()];
    for pm in morphs {
        if let PmxMorphData::Material(offs) = &pm.data {
            for mo in offs {
                if mo.material_index >= 0 && (mo.material_index as usize) < materials.len() {
                    let changes_alpha = (mo.op_mode == 0 && mo.diffuse[3] < 0.99)
                        || (mo.op_mode != 0 && mo.diffuse[3].abs() > 0.01);
                    if changes_alpha {
                        morph_affects_alpha[mo.material_index as usize] = true;
                    }
                }
            }
        }
    }

    let mut decoded_cache: std::collections::HashMap<u32, Option<(u32, u32, Vec<u8>)>> =
        std::collections::HashMap::new();

    for (m_idx, raw) in materials.iter_mut().enumerate() {
        let Some(tex_idx) = raw.tex_base else {
            if morph_affects_alpha[m_idx] || raw.base_color[3] < 0.99 {
                raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_BLEND;
            }
            continue;
        };
        let Some(td) = textures.get(tex_idx as usize) else {
            if morph_affects_alpha[m_idx] || raw.base_color[3] < 0.99 {
                raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_BLEND;
            }
            continue;
        };
        let mname_low = raw.name.to_ascii_lowercase();

        if Path::new(&td.name)
            .extension()
            .is_some_and(|ext| ext.eq_ignore_ascii_case("spa"))
        {
            raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_ADDITIVE;
            raw.unlit = true;
            raw.cast_shadow = false;
            continue;
        }

        let decoded = decoded_cache.entry(tex_idx).or_insert_with(|| {
            if let RawTextureSource::ExternalFile(path) = &td.source {
                crate::textures::decode_image_file_rgba(path, true)
                    .ok()
                    .map(|(w, h, _fmt, px)| (w, h, px))
            } else {
                None
            }
        });

        let Some((w, h, px)) = decoded.as_ref() else {
            if morph_affects_alpha[m_idx] || raw.base_color[3] < 0.99 {
                raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_BLEND;
            }
            continue;
        };
        if *w == 0 || *h == 0 || px.len() < (*w as usize) * (*h as usize) * 4 {
            continue;
        }

        // Collect UV samples specifically covered by this material's submeshes so shared
        // atlases classify per-material UV islands accurately rather than leaking alpha across parts.
        let mut sample_coords: Vec<(u32, u32)> = Vec::new();
        let mut u_min = 1.0f32;
        let mut v_min = 1.0f32;
        let mut u_max = 0.0f32;
        let mut v_max = 0.0f32;
        let mut any_uv = false;
        let wi = *w as usize;
        let hi = *h as usize;

        for sm in submeshes {
            if sm.material_index != m_idx as u32 {
                continue;
            }
            let tri_step = (sm.indices.len() / 3).div_ceil(1024).max(1);
            for tri in sm.indices.chunks_exact(3).step_by(tri_step) {
                let uvs = [
                    vertices[tri[0] as usize].uv0,
                    vertices[tri[1] as usize].uv0,
                    vertices[tri[2] as usize].uv0,
                ];
                for uv in uvs {
                    let u = uv[0].rem_euclid(1.0);
                    let v = uv[1].rem_euclid(1.0);
                    u_min = u_min.min(u);
                    v_min = v_min.min(v);
                    u_max = u_max.max(u);
                    v_max = v_max.max(v);
                    any_uv = true;
                }
                let bary = [
                    [1.0f32, 0.0, 0.0],
                    [0.0, 1.0, 0.0],
                    [0.0, 0.0, 1.0],
                    [0.5, 0.5, 0.0],
                    [0.0, 0.5, 0.5],
                    [0.5, 0.0, 0.5],
                    [1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0],
                ];
                for b in bary {
                    let u =
                        (uvs[0][0] * b[0] + uvs[1][0] * b[1] + uvs[2][0] * b[2]).rem_euclid(1.0);
                    let v =
                        (uvs[0][1] * b[0] + uvs[1][1] * b[1] + uvs[2][1] * b[2]).rem_euclid(1.0);
                    let tx = ((u * (wi as f32 - 1.0)).round() as usize).min(wi - 1);
                    let ty = ((v * (hi as f32 - 1.0)).round() as usize).min(hi - 1);
                    sample_coords.push((tx as u32, ty as u32));
                }
            }
        }

        if any_uv && u_max > u_min && v_max > v_min {
            let x0 = ((u_min * (wi as f32)).floor() as usize).min(wi - 1);
            let y0 = ((v_min * (hi as f32)).floor() as usize).min(hi - 1);
            let x1 = ((u_max * (wi as f32)).ceil() as usize).clamp(x0 + 1, wi);
            let y1 = ((v_max * (hi as f32)).ceil() as usize).clamp(y0 + 1, hi);
            let bw = x1 - x0;
            let bh = y1 - y0;
            let area = bw.saturating_mul(bh);
            let step = ((area as f32 / 8192.0).sqrt().ceil() as usize).max(1);
            for y in (y0..y1).step_by(step) {
                for x in (x0..x1).step_by(step) {
                    sample_coords.push((x as u32, y as u32));
                }
            }
        } else if sample_coords.is_empty() {
            let step = (((wi * hi) as f32 / 8192.0).sqrt().ceil() as usize).max(1);
            for y in (0..hi).step_by(step) {
                for x in (0..wi).step_by(step) {
                    sample_coords.push((x as u32, y as u32));
                }
            }
        }

        let mut total = 0u32;
        let mut zero_alpha = 0u32;
        let mut semi_alpha = 0u32;
        let mut black_bg_opaque = 0u32;
        let mut bright_glow = 0u32;
        for (tx, ty) in sample_coords {
            let o = ((ty as usize) * wi + (tx as usize)) * 4;
            let r = px[o];
            let g = px[o + 1];
            let b = px[o + 2];
            let a = px[o + 3];
            total += 1;
            if a <= 10 {
                zero_alpha += 1;
            } else if a < 245 {
                semi_alpha += 1;
            } else {
                let max_c = r.max(g).max(b);
                if max_c <= 18 {
                    black_bg_opaque += 1;
                } else if max_c >= 85 {
                    bright_glow += 1;
                }
            }
        }
        if total == 0 {
            continue;
        }

        let inv = 1.0f32 / (total as f32);
        let zero_frac = (zero_alpha as f32) * inv;
        let semi_frac = (semi_alpha as f32) * inv;
        let black_frac = (black_bg_opaque as f32) * inv;
        let glow_frac = (bright_glow as f32) * inv;

        let additive_name = mname_low.contains("highlight")
            || mname_low.contains("hikari")
            || mname_low.contains("kira")
            || mname_low.contains("sparkle")
            || mname_low.contains("additive")
            || mname_low.contains("glow")
            || mname_low.contains("flair")
            || mname_low.contains("flare")
            || raw.name.contains("光");
        let opaque_garment_name = mname_low.contains("hair")
            || mname_low.contains("suit")
            || mname_low.contains("coat")
            || mname_low.contains("shoe")
            || mname_low.contains("boot")
            || mname_low.contains("pants")
            || mname_low.contains("skin")
            || mname_low.contains("iris")
            || mname_low.contains("sclera");
        let blend_name = mname_low.contains("blush")
            || mname_low.contains("cheek")
            || mname_low.contains("tear")
            || mname_low.contains("glass")
            || mname_low.contains("lens")
            || mname_low.contains("veil")
            || mname_low.contains("translucent")
            || raw.name.contains("照")
            || raw.name.contains("頬");

        if (additive_name && (black_frac >= 0.35 || semi_frac > 0.05) && zero_frac < 0.25)
            || (zero_frac < 0.02
                && black_frac >= 0.45
                && glow_frac >= 0.005
                && semi_frac < 0.08
                && (!raw.cast_shadow || additive_name || !opaque_garment_name))
        {
            raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_ADDITIVE;
            raw.unlit = true;
            raw.cast_shadow = false;
        } else if morph_affects_alpha[m_idx]
            || raw.base_color[3] < 0.99
            || semi_frac >= 0.08
            || (blend_name && (zero_frac + semi_frac) >= 0.005)
        {
            raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_BLEND;
        } else if (zero_frac + semi_frac) >= 0.005 {
            raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_MASK;
            raw.alpha_cutoff = if mname_low.contains("lash") || mname_low.contains("hair") {
                0.35
            } else {
                0.5
            };
        } else {
            raw.alpha_mode = aether_ffi::AETHER_ALPHA_MODE_OPAQUE;
        }
    }
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
            let resolved = resolve_pmx_texture_path(base_dir, &trimmed);
            textures.push(RawTextureDesc {
                name: trimmed,
                source: match resolved {
                    Some(path) => RawTextureSource::ExternalFile(path),
                    None => RawTextureSource::Missing,
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
            alpha_mode: if diffuse[3] < 0.99 {
                aether_ffi::AETHER_ALPHA_MODE_BLEND
            } else {
                AETHER_ALPHA_MODE_OPAQUE
            },
            alpha_cutoff: 0.5,
            double_sided: false,
            unlit: false,
            has_vrm_mtoon: false,
            cast_shadow: true,
            edge_enabled: edge_flag != 0,
            is_pmx: true,
            render_queue_offset: m_idx as i32,
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
            shader_profile: None,
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

    finalize_pmx_material_alpha_tiers(&mut materials, &submeshes, &vertices, &textures, &[]);

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
        pmx_aux: None,
    })
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod rev31_pmx_tests {
    use super::*;

    fn write_pmx_utf16_str(buf: &mut Vec<u8>, s: &str) {
        let utf16: Vec<u16> = s.encode_utf16().collect();
        buf.extend_from_slice(&((utf16.len() * 2) as i32).to_le_bytes());
        for u in utf16 {
            buf.extend_from_slice(&u.to_le_bytes());
        }
    }

    fn write_tga_rgba(path: &Path, w: u16, h: u16, px_fn: impl Fn(u16, u16) -> [u8; 4]) {
        let mut tga = Vec::with_capacity(18 + usize::from(w) * usize::from(h) * 4);
        tga.extend_from_slice(&[0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        tga.extend_from_slice(&w.to_le_bytes());
        tga.extend_from_slice(&h.to_le_bytes());
        tga.push(32); // 32-bit BGRA
        tga.push(0x28); // top-left origin + 8 alpha bits
        for y in 0..h {
            for x in 0..w {
                let [r, g, b, a] = px_fn(x, y);
                tga.extend_from_slice(&[b, g, r, a]);
            }
        }
        std::fs::write(path, tga).unwrap();
    }

    #[test]
    fn pmx_4tier_transparency_case_insensitive_and_sphere_mix() {
        let dir = std::env::temp_dir().join("aether_rev31_pmx_4tier_test");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("TexSub")).unwrap();

        // 0: opaque.tga (all a=255)
        write_tga_rgba(&dir.join("TexSub/Opaque.tga"), 4, 4, |_, _| {
            [220, 180, 160, 255]
        });
        // 1: lash_cutout.tga (binary alpha: half a=0, half a=255)
        write_tga_rgba(&dir.join("TexSub/Lash_Cutout.tga"), 4, 4, |x, _| {
            if x < 2 {
                [30, 25, 30, 0]
            } else {
                [30, 25, 30, 255]
            }
        });
        // 2: veil_blend.tga (smooth semi-transparent gradient a=128)
        write_tga_rgba(&dir.join("TexSub/Veil_Blend.tga"), 4, 4, |_, _| {
            [200, 220, 255, 128]
        });
        // 3: eye_kira.spa (black background + bright glow center)
        write_tga_rgba(&dir.join("TexSub/Eye_Kira.spa"), 4, 4, |x, y| {
            if x == 1 && y == 1 {
                [255, 250, 240, 255]
            } else {
                [0, 0, 0, 255]
            }
        });
        // 4: hair_sph.sph (sphere multiply map)
        write_tga_rgba(&dir.join("TexSub/Hair_Sph.sph"), 4, 4, |_, _| {
            [200, 180, 160, 255]
        });

        let mut pmx = Vec::new();
        pmx.extend_from_slice(b"PMX ");
        pmx.extend_from_slice(&2.0f32.to_le_bytes());
        // globals: count=8, [utf16=0, uv_cnt=0, vtx=1, tex=1, mat=1, bone=1, morph=1, rb=1]
        pmx.extend_from_slice(&[8, 0, 0, 1, 1, 1, 1, 1, 1]);
        write_pmx_utf16_str(&mut pmx, "TestModel");
        write_pmx_utf16_str(&mut pmx, "TestModelEn");
        write_pmx_utf16_str(&mut pmx, "");
        write_pmx_utf16_str(&mut pmx, "");

        // 12 vertices (4 quads/tris, 3 verts each)
        pmx.extend_from_slice(&12i32.to_le_bytes());
        for i in 0..12 {
            let tri = (i / 3) as f32;
            let k = (i % 3) as f32;
            let uv = match i % 3 {
                0 => [0.0f32, 0.0],
                1 => [1.0, 0.0],
                _ => [0.5, 1.0],
            };
            pmx.extend_from_slice(&[tri + k * 0.2, 0.0, 0.0].map(f32::to_le_bytes).concat());
            pmx.extend_from_slice(&[0.0f32, 0.0, 1.0].map(f32::to_le_bytes).concat());
            pmx.extend_from_slice(&uv.map(f32::to_le_bytes).concat());
            pmx.push(0); // BDEF1
            pmx.push(0); // bone 0
            pmx.extend_from_slice(&1.0f32.to_le_bytes()); // edge_scale
        }
        // 12 indices (4 tris)
        pmx.extend_from_slice(&12i32.to_le_bytes());
        for i in 0u8..12 {
            pmx.push(i);
        }
        // 5 textures with wrong case & backslash to test UTF-16 + case-insensitive resolver
        pmx.extend_from_slice(&5i32.to_le_bytes());
        write_pmx_utf16_str(&mut pmx, "\\texsub\\opaque.TGA");
        write_pmx_utf16_str(&mut pmx, ".\\TEXSUB\\LASH_CUTOUT.tga");
        write_pmx_utf16_str(&mut pmx, "texsub\\veil_blend.tga");
        write_pmx_utf16_str(&mut pmx, "TexSub\\eye_kira.spa");
        write_pmx_utf16_str(&mut pmx, "texsub/hair_sph.sph");

        // 4 materials:
        // 0: opaque body (cast_shadow=1, edge=1, sphere=.sph mode 1)
        // 1: cutout lash (cast_shadow=0, edge=0)
        // 2: blend veil (cast_shadow=0, edge=0)
        // 3: additive .spa card (cast_shadow=0, edge=0, sphere=.spa mode 2)
        pmx.extend_from_slice(&4i32.to_le_bytes());
        let specs = [
            ("body_opaque", 0u8, 4u8, 0x10u8 | 0x04u8, 1u8),
            ("lash_cutout", 1u8, 4u8, 0x01u8, 0u8),
            ("veil_blend", 2u8, 4u8, 0x01u8, 0u8),
            ("highlight_card", 3u8, 3u8, 0x01u8, 2u8),
        ];
        for (name, tex_i, sph_i, flags, sph_mode) in specs {
            write_pmx_utf16_str(&mut pmx, name);
            write_pmx_utf16_str(&mut pmx, name);
            pmx.extend_from_slice(&[1.0f32, 1.0, 1.0, 1.0].map(f32::to_le_bytes).concat());
            pmx.extend_from_slice(&[0.2f32, 0.2, 0.2].map(f32::to_le_bytes).concat());
            pmx.extend_from_slice(&8.0f32.to_le_bytes());
            pmx.extend_from_slice(&[0.5f32, 0.5, 0.5].map(f32::to_le_bytes).concat());
            pmx.push(flags);
            pmx.extend_from_slice(&[0.0f32, 0.0, 0.0, 1.0].map(f32::to_le_bytes).concat());
            pmx.extend_from_slice(&1.0f32.to_le_bytes());
            pmx.push(tex_i);
            pmx.push(sph_i);
            pmx.push(sph_mode);
            pmx.push(1); // builtin toon
            pmx.push(0);
            write_pmx_utf16_str(&mut pmx, "");
            pmx.extend_from_slice(&3i32.to_le_bytes());
        }
        // 1 root bone
        pmx.extend_from_slice(&1i32.to_le_bytes());
        write_pmx_utf16_str(&mut pmx, "センター");
        write_pmx_utf16_str(&mut pmx, "center");
        pmx.extend_from_slice(&[0.0f32; 3].map(f32::to_le_bytes).concat());
        pmx.push(0xFF); // parent -1
        pmx.extend_from_slice(&0i32.to_le_bytes());
        pmx.extend_from_slice(&0u16.to_le_bytes());
        pmx.extend_from_slice(&[0.0f32; 3].map(f32::to_le_bytes).concat());
        // 0 morphs, 0 frames, 0 RBs, 0 joints
        for _ in 0..4 {
            pmx.extend_from_slice(&0i32.to_le_bytes());
        }

        let parsed = parse_pmx(&pmx, &dir).unwrap();
        assert_eq!(parsed.materials.len(), 4);
        assert_eq!(parsed.materials[0].alpha_mode, AETHER_ALPHA_MODE_OPAQUE);
        assert!(parsed.materials[0].cast_shadow);
        assert!(parsed.materials[0].edge_enabled);
        assert!((parsed.materials[0].matcap_add_mix - 0.0).abs() < 1e-4);

        assert_eq!(
            parsed.materials[1].alpha_mode,
            aether_ffi::AETHER_ALPHA_MODE_MASK
        );
        assert!(!parsed.materials[1].cast_shadow);
        assert!(!parsed.materials[1].edge_enabled);

        assert_eq!(
            parsed.materials[2].alpha_mode,
            aether_ffi::AETHER_ALPHA_MODE_BLEND
        );
        assert_eq!(
            parsed.materials[3].alpha_mode,
            aether_ffi::AETHER_ALPHA_MODE_ADDITIVE
        );
        assert!((parsed.materials[3].matcap_add_mix - 1.0).abs() < 1e-4);

        for (i, t) in parsed.textures.iter().enumerate() {
            assert!(
                matches!(t.source, RawTextureSource::ExternalFile(_)),
                "texture {i} ({}) failed case-insensitive resolution",
                t.name
            );
        }

        let _ = std::fs::remove_dir_all(&dir);
    }
}
