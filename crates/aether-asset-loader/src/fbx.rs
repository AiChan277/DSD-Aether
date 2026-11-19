//! Autodesk FBX (Binary & ASCII 6.x/7.x) loader powered by `ufbx` (`§6.2`, `ADR-0004`).

use std::collections::HashMap;
use std::path::Path;

use aether_ffi::{
    AETHER_ALPHA_MODE_OPAQUE, AETHER_SOURCE_FORMAT_FBX, AETHER_TEX_FORMAT_RGBA8_SRGB,
    AETHER_TEX_USAGE_COLOR,
};

use crate::LoaderError;
use crate::ir::{
    ParsedAvatar, RawExprPreset, RawJoint, RawMaterial, RawMorphDeltaItem, RawMorphTarget,
    RawSubmesh, RawTextureDesc, RawTextureSource, RawVertex, SourceUnitMetadata,
};
use crate::limits::{
    MAX_MATERIALS, MAX_MORPH_DELTAS, MAX_TEXTURES, check_bone_count, check_index_count,
    check_morph_count, check_vertex_count,
};

/// Parses a binary or ASCII FBX document using `ufbx`, normalizing axes to RH Y-up metres.
pub fn parse_fbx(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    let opts = ufbx::LoadOpts {
        target_axes: ufbx::CoordinateAxes::right_handed_y_up(),
        target_unit_meters: 1.0,
        space_conversion: ufbx::SpaceConversion::ModifyGeometry,
        generate_missing_normals: true,
        clean_skin_weights: true,
        load_external_files: false,
        file_format: ufbx::FileFormat::Fbx,
        ..Default::default()
    };

    let scene = ufbx::load_memory(data, opts)
        .map_err(|_| LoaderError::Parse("failed to parse FBX scene via ufbx"))?;

    // Collect skeleton joints from scene nodes tagged as bones (plus skin cluster targets)
    let mut node_to_joint: HashMap<u32, usize> = HashMap::new();
    let mut bone_nodes: Vec<&ufbx::Node> = Vec::new();
    for node_ref in &scene.nodes {
        let node: &ufbx::Node = node_ref;
        if node.bone.is_some() {
            node_to_joint.insert(node.element.element_id, bone_nodes.len());
            bone_nodes.push(node);
        }
    }
    check_bone_count(bone_nodes.len())?;

    let mut joints = Vec::with_capacity(bone_nodes.len());
    for (ord, node) in bone_nodes.iter().enumerate() {
        let parent = node
            .parent
            .as_ref()
            .and_then(|p| node_to_joint.get(&p.element.element_id).copied());
        let tr = node.local_transform;
        joints.push(RawJoint {
            name: node.element.name.to_string(),
            alt_name: String::new(),
            parent,
            local_translation: [
                tr.translation.x as f32,
                tr.translation.y as f32,
                tr.translation.z as f32,
            ],
            local_rotation: [
                tr.rotation.x as f32,
                tr.rotation.y as f32,
                tr.rotation.z as f32,
                tr.rotation.w as f32,
            ],
            local_scale: [tr.scale.x as f32, tr.scale.y as f32, tr.scale.z as f32],
            inverse_bind_matrix: None,
            explicit_humanoid: None,
            ordinal: ord,
        });
    }

    // Textures
    if scene.textures.len() > MAX_TEXTURES {
        return Err(LoaderError::LimitExceeded("FBX texture count > 512"));
    }
    let mut tex_id_map: HashMap<u32, u32> = HashMap::new();
    let mut textures = Vec::with_capacity(scene.textures.len());
    for tex_ref in &scene.textures {
        let tex: &ufbx::Texture = tex_ref;
        let idx = textures.len() as u32;
        tex_id_map.insert(tex.element.element_id, idx);
        let rel = tex.relative_filename.as_ref();
        let file = tex.filename.as_ref();
        let content_bytes = if !tex.content.is_empty() {
            Some(tex.content.as_ref().to_vec())
        } else if let Some(vid) = tex.video.as_ref()
            && !vid.content.is_empty()
        {
            Some(vid.content.as_ref().to_vec())
        } else {
            None
        };
        let source = if let Some(bytes) = content_bytes {
            RawTextureSource::EmbeddedBytes(bytes)
        } else {
            let clean_rel = rel.replace('\\', "/");
            let cand1 = base_dir.join(&clean_rel);
            let fname_only = Path::new(file)
                .file_name()
                .map(|n| base_dir.join(n))
                .unwrap_or_default();
            if cand1.is_file() {
                RawTextureSource::ExternalFile(cand1)
            } else if fname_only.is_file() {
                RawTextureSource::ExternalFile(fname_only)
            } else {
                RawTextureSource::Missing
            }
        };
        textures.push(RawTextureDesc {
            name: if rel.is_empty() {
                format!("fbx_tex_{idx}")
            } else {
                rel.to_owned()
            },
            source,
            width: 1,
            height: 1,
            srgb: true,
            usage_flags: AETHER_TEX_USAGE_COLOR,
            content_hash_lo: 0,
            rgba_byte_len: 4,
            cached_bc7_path: None,
            cached_bc7_byte_len: 0,
            pixel_offset: 0,
            pixel_size: 4,
            final_format: AETHER_TEX_FORMAT_RGBA8_SRGB,
        });
    }

    // Materials
    if scene.materials.len() > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded("FBX materials > 1024"));
    }
    let mut mat_id_map: HashMap<u32, u32> = HashMap::new();
    let mut materials = Vec::with_capacity(scene.materials.len().max(1));
    for mat_ref in &scene.materials {
        let mat: &ufbx::Material = mat_ref;
        let m_idx = materials.len() as u32;
        mat_id_map.insert(mat.element.element_id, m_idx);
        let diff = if mat.pbr.base_color.has_value {
            mat.pbr.base_color.value_vec4
        } else if mat.fbx.diffuse_color.has_value {
            mat.fbx.diffuse_color.value_vec4
        } else {
            ufbx::Vec4 {
                x: 1.0,
                y: 1.0,
                z: 1.0,
                w: 1.0,
            }
        };
        let tex_base = mat
            .pbr
            .base_color
            .texture
            .as_ref()
            .or(mat.fbx.diffuse_color.texture.as_ref())
            .and_then(|t| tex_id_map.get(&t.element.element_id).copied());

        materials.push(RawMaterial {
            name: mat.element.name.to_string(),
            base_color: [diff.x as f32, diff.y as f32, diff.z as f32, 1.0],
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: 4.0,
            rim_lift: 0.0,
            outline_color: [0.1, 0.08, 0.08],
            outline_lighting_mix: 0.5,
            outline_width: 0.0,
            outline_screen_space: false,
            emissive: [0.0, 0.0, 0.0],
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: 0.0,
            matcap_strength: 0.0,
            gi_equalization: 0.15,
            shading_shift: 0.0,
            shading_toony: 0.9,
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
            tex_matcap: None,
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
            shader_profile: None,
        });
    }
    if materials.is_empty() {
        materials.push(RawMaterial {
            name: "fbx_default".to_owned(),
            base_color: [1.0, 1.0, 1.0, 1.0],
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: 4.0,
            rim_lift: 0.0,
            outline_color: [0.1, 0.08, 0.08],
            outline_lighting_mix: 0.5,
            outline_width: 0.0,
            outline_screen_space: false,
            emissive: [0.0, 0.0, 0.0],
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: 0.0,
            matcap_strength: 0.0,
            gi_equalization: 0.15,
            shading_shift: 0.0,
            shading_toony: 0.9,
            uv_scroll: [0.0, 0.0],
            uv_rotation: 0.0,
            alpha_mode: AETHER_ALPHA_MODE_OPAQUE,
            alpha_cutoff: 0.5,
            double_sided: false,
            unlit: false,
            has_vrm_mtoon: false,
            render_queue_offset: 0,
            flatten_override: None,
            tex_base: None,
            tex_shade: None,
            tex_shading_shift: None,
            tex_normal: None,
            tex_emissive: None,
            tex_rim: None,
            tex_matcap: None,
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
            shader_profile: None,
        });
    }

    // Meshes -> triangulated corner vertices mapped into RawVertex + RawSubmesh
    let mut vertices = Vec::new();
    let mut submeshes = Vec::new();
    let mut morphs = Vec::new();
    let mut expr_presets = Vec::new();
    let mut tri_scratch = Vec::new();
    let mut total_morph_deltas = 0usize;

    for mesh_ref in &scene.meshes {
        let mesh: &ufbx::Mesh = mesh_ref;
        let skin = mesh.skin_deformers.first();
        if let Some(sk) = skin {
            for (c_i, cl_ref) in sk.clusters.iter().enumerate() {
                let _ = c_i;
                if let Some(bn) = cl_ref.bone_node.as_ref()
                    && let Some(&j_idx) = node_to_joint.get(&bn.element.element_id)
                {
                    let m = cl_ref.geometry_to_bone;
                    joints[j_idx].inverse_bind_matrix = Some([
                        m.m00 as f32,
                        m.m01 as f32,
                        m.m02 as f32,
                        m.m03 as f32,
                        m.m10 as f32,
                        m.m11 as f32,
                        m.m12 as f32,
                        m.m13 as f32,
                        m.m20 as f32,
                        m.m21 as f32,
                        m.m22 as f32,
                        m.m23 as f32,
                        0.0,
                        0.0,
                        0.0,
                        1.0,
                    ]);
                }
            }
        }

        // Map original FBX vertex index -> emitted `vertices` indices (for blendshape expansion)
        let mut orig_to_emitted: Vec<Vec<u32>> = vec![Vec::new(); mesh.num_vertices];
        let mut part_indices: Vec<(u32, Vec<u32>)> = Vec::new();
        let mat_count = mesh.materials.len().max(1);
        for pm_i in 0..mat_count {
            let m_global = mesh
                .materials
                .get(pm_i)
                .and_then(|mr| mat_id_map.get(&mr.element.element_id).copied())
                .unwrap_or(0);
            part_indices.push((m_global, Vec::new()));
        }

        let pos_src = &mesh.vertex_position;
        for (f_idx, &face) in mesh.faces.iter().enumerate() {
            let num_tris = ufbx::triangulate_face_vec(&mut tri_scratch, mesh, face);
            let mat_slot = mesh.face_material.get(f_idx).copied().unwrap_or(0) as usize;
            let dst_idx = mat_slot.min(part_indices.len() - 1);
            let dst_list = &mut part_indices[dst_idx].1;
            check_index_count(dst_list.len() + (num_tris as usize) * 3)?;

            for &corner in tri_scratch.iter().take((num_tris as usize) * 3) {
                let c_u = corner as usize;
                let v_orig = mesh.vertex_indices.get(c_u).copied().unwrap_or(0) as usize;
                let p = if pos_src.exists {
                    pos_src
                        .indices
                        .get(c_u)
                        .and_then(|&vi| pos_src.values.get(vi as usize).copied())
                        .unwrap_or_default()
                } else {
                    ufbx::Vec3::default()
                };
                let n = if mesh.vertex_normal.exists {
                    mesh.vertex_normal
                        .indices
                        .get(c_u)
                        .and_then(|&vi| mesh.vertex_normal.values.get(vi as usize).copied())
                        .unwrap_or(ufbx::Vec3 {
                            x: 0.0,
                            y: 1.0,
                            z: 0.0,
                        })
                } else {
                    ufbx::Vec3 {
                        x: 0.0,
                        y: 1.0,
                        z: 0.0,
                    }
                };
                let uv = if mesh.vertex_uv.exists {
                    mesh.vertex_uv
                        .indices
                        .get(c_u)
                        .and_then(|&vi| mesh.vertex_uv.values.get(vi as usize).copied())
                        .unwrap_or_default()
                } else {
                    ufbx::Vec2::default()
                };

                let mut jts = [0u16; 8];
                let mut wts = [0.0f32; 8];
                if let Some(sk) = skin
                    && let Some(sv) = sk.vertices.get(v_orig)
                {
                    let beg = sv.weight_begin as usize;
                    let cnt = (sv.num_weights as usize).min(8);
                    for k in 0..cnt {
                        if let Some(sw) = sk.weights.get(beg + k)
                            && let Some(cl) = sk.clusters.get(sw.cluster_index as usize)
                            && let Some(bn) = cl.bone_node.as_ref()
                            && let Some(&j_idx) = node_to_joint.get(&bn.element.element_id)
                        {
                            jts[k] = j_idx as u16;
                            wts[k] = sw.weight as f32;
                        }
                    }
                } else {
                    wts[0] = 1.0;
                }

                check_vertex_count(vertices.len() + 1)?;
                let emitted_idx = vertices.len() as u32;
                vertices.push(RawVertex {
                    position: [p.x as f32, p.y as f32, p.z as f32],
                    normal: [n.x as f32, n.y as f32, n.z as f32],
                    uv0: [uv.x as f32, 1.0 - uv.y as f32],
                    uv1: [0.0, 0.0],
                    color: [255, 255, 255, 255],
                    joints: jts,
                    weights: wts,
                    edge_scale: 1.0,
                    flatten_override: -1.0,
                });
                if let Some(slot) = orig_to_emitted.get_mut(v_orig) {
                    slot.push(emitted_idx);
                }
                dst_list.push(emitted_idx);
            }
        }

        let base_mesh_name = mesh.element.name.to_string();
        for (m_global, idxs) in part_indices {
            if idxs.is_empty() {
                continue;
            }
            let sname = if base_mesh_name.is_empty() {
                format!("fbx_submesh_{}", submeshes.len())
            } else {
                base_mesh_name.clone()
            };
            let lower = sname.to_ascii_lowercase();
            submeshes.push(RawSubmesh {
                name: sname,
                indices: idxs,
                material_index: m_global,
                is_eye_or_brow_hint: lower.contains("eye") || lower.contains("brow"),
            });
        }

        // BlendShapes
        for bd_ref in &mesh.blend_deformers {
            let bd: &ufbx::BlendDeformer = bd_ref;
            for ch_ref in &bd.channels {
                let ch: &ufbx::BlendChannel = ch_ref;
                let Some(shape) = ch.target_shape.as_ref() else {
                    continue;
                };
                check_morph_count(morphs.len() + 1)?;
                let mut deltas = Vec::new();
                let mut sum_x = 0.0f32;
                let n_off = shape.num_offsets.min(shape.offset_vertices.len());
                for i in 0..n_off {
                    let v_orig = shape.offset_vertices[i] as usize;
                    let dp = shape.position_offsets.get(i).copied().unwrap_or_default();
                    let dn = shape.normal_offsets.get(i).copied().unwrap_or_default();
                    if let Some(emitted_list) = orig_to_emitted.get(v_orig) {
                        for &ev in emitted_list {
                            total_morph_deltas = total_morph_deltas.saturating_add(1);
                            if total_morph_deltas > MAX_MORPH_DELTAS {
                                return Err(LoaderError::LimitExceeded("FBX morph deltas > 16M"));
                            }
                            sum_x += vertices[ev as usize].position[0];
                            deltas.push(RawMorphDeltaItem {
                                vertex_index: ev,
                                d_pos: [dp.x as f32, dp.y as f32, dp.z as f32],
                                d_norm: [dn.x as f32, dn.y as f32, dn.z as f32],
                            });
                        }
                    }
                }
                let cx = if deltas.is_empty() {
                    0.0
                } else {
                    sum_x / (deltas.len() as f32)
                };
                let cname = ch.element.name.to_string();
                let m_idx = morphs.len();
                morphs.push(RawMorphTarget {
                    name: cname.clone(),
                    deltas,
                    centroid_x: cx,
                });
                expr_presets.push(RawExprPreset {
                    name: cname,
                    morph_binds: vec![(m_idx, 1.0)],
                });
            }
        }
    }

    Ok(ParsedAvatar {
        source_format: AETHER_SOURCE_FORMAT_FBX,
        base_dir: base_dir.to_path_buf(),
        unit_meta: SourceUnitMetadata {
            explicit_unit_scale: Some(scene.settings.unit_meters as f32),
            resolved_unit_scale: 1.0,
            scale_guessed: false,
            left_handed_flipped: false,
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
