//! Unified glTF 2.0 / GLB / VRM 0.x / VRM 1.0 parser (`§6.2`, `§6.8`, `HR-06`).
//!
//! Uses `gltf` (`default-features = false`, pure Rust) + `serde_json` for `VRM` / `VRMC_vrm` /
//! `VRMC_materials_mtoon` / `VRMC_springBone` / `EXT_meshopt_compression`. Rejects
//! `KHR_draco_mesh_compression` deterministically with `LoaderError::Unsupported`.

use std::collections::HashMap;
use std::path::Path;

use aether_ffi::{
    AETHER_ALPHA_MODE_BLEND, AETHER_ALPHA_MODE_MASK, AETHER_ALPHA_MODE_OPAQUE,
    AETHER_COLLIDER_SHAPE_CAPSULE, AETHER_COLLIDER_SHAPE_SPHERE, AETHER_SOURCE_FORMAT_GLTF,
    AETHER_SOURCE_FORMAT_VRM0, AETHER_SOURCE_FORMAT_VRM1, AETHER_TEX_FORMAT_RGBA8_SRGB,
    AETHER_TEX_USAGE_COLOR, AETHER_WARN_NODE_CONSTRAINT_APPROX,
};
use serde_json::Value;

use crate::LoaderError;
use crate::ir::{
    ParsedAvatar, RawCollider, RawExprPreset, RawJoint, RawMaterial, RawMorphDeltaItem,
    RawMorphTarget, RawSpringChain, RawSpringNode, RawSubmesh, RawTextureDesc, RawTextureSource,
    RawVertex, SourceUnitMetadata,
};
use crate::limits::{
    MAX_COLLIDERS, MAX_MATERIALS, MAX_MORPH_DELTAS, MAX_TEXTURES, check_bone_count,
    check_index_count, check_morph_count, check_vertex_count,
};
use crate::meshopt::{MeshoptFilter, MeshoptMode, decode_meshopt};

enum ResolvedView<'a> {
    Borrowed(&'a [u8]),
    Owned(Vec<u8>),
}

impl ResolvedView<'_> {
    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Borrowed(s) => s,
            Self::Owned(v) => v.as_slice(),
        }
    }
}

fn check_draco_extension(ext_list: Option<&[String]>) -> Result<(), LoaderError> {
    if let Some(list) = ext_list
        && list.iter().any(|s| s == "KHR_draco_mesh_compression")
    {
        return Err(LoaderError::Unsupported(
            "KHR_draco_mesh_compression is unsupported in runtime loader (§6.2)",
        ));
    }
    Ok(())
}

fn check_draco_map(map: Option<&serde_json::Map<String, Value>>) -> Result<(), LoaderError> {
    if let Some(m) = map
        && m.contains_key("KHR_draco_mesh_compression")
    {
        return Err(LoaderError::Unsupported(
            "KHR_draco_mesh_compression primitive extension is unsupported (§6.2)",
        ));
    }
    Ok(())
}

fn decode_base64_simple(input: &str) -> Result<Vec<u8>, LoaderError> {
    let mut out = Vec::with_capacity((input.len() / 4) * 3);
    let mut buf = [0u8; 4];
    let mut b_len = 0usize;
    let mut pad = 0usize;
    for b in input.bytes() {
        if b.is_ascii_whitespace() {
            continue;
        }
        let v = match b {
            b'A'..=b'Z' => b - b'A',
            b'a'..=b'z' => b - b'a' + 26,
            b'0'..=b'9' => b - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => {
                pad += 1;
                0
            }
            _ => return Err(LoaderError::Parse("invalid base64 character in data URI")),
        };
        buf[b_len] = v;
        b_len += 1;
        if b_len == 4 {
            out.push((buf[0] << 2) | (buf[1] >> 4));
            if pad < 2 {
                out.push((buf[1] << 4) | (buf[2] >> 2));
            }
            if pad < 1 {
                out.push((buf[2] << 6) | buf[3]);
            }
            b_len = 0;
            pad = 0;
        }
    }
    Ok(out)
}

fn safe_external_path(base_dir: &Path, uri: &str) -> Option<std::path::PathBuf> {
    if uri.contains("..") || uri.starts_with('/') || uri.contains("://") {
        return None;
    }
    let clean = uri.replace('\\', "/");
    let p = base_dir.join(clean);
    if p.is_file() { Some(p) } else { None }
}

fn resolve_buffer_data<'a>(
    buf: &gltf::Buffer<'_>,
    bin_chunk: Option<&'a [u8]>,
    base_dir: &Path,
) -> Result<ResolvedView<'a>, LoaderError> {
    match buf.source() {
        gltf::buffer::Source::Bin => {
            let bin = bin_chunk.ok_or(LoaderError::Parse("GLB missing BIN chunk"))?;
            Ok(ResolvedView::Borrowed(bin))
        }
        gltf::buffer::Source::Uri(uri) => {
            if let Some(rest) = uri.strip_prefix("data:")
                && let Some((_, b64)) = rest.split_once(";base64,")
            {
                return Ok(ResolvedView::Owned(decode_base64_simple(b64)?));
            }
            if let Some(path) = safe_external_path(base_dir, uri) {
                let bytes = std::fs::read(path)
                    .map_err(|_| LoaderError::Parse("failed to read glTF .bin"))?;
                if bytes.len() as u64 > crate::limits::MAX_FILE_BYTES {
                    return Err(LoaderError::LimitExceeded("external .bin exceeds 1 GiB"));
                }
                return Ok(ResolvedView::Owned(bytes));
            }
            Err(LoaderError::Parse("unresolved glTF buffer URI"))
        }
    }
}

fn resolve_all_buffer_views<'a>(
    doc: &gltf::Document,
    bin_chunk: Option<&'a [u8]>,
    base_dir: &Path,
) -> Result<Vec<ResolvedView<'a>>, LoaderError> {
    let mut buffers = Vec::new();
    for b in doc.buffers() {
        buffers.push(resolve_buffer_data(&b, bin_chunk, base_dir)?);
    }

    let mut views = Vec::new();
    for bv in doc.views() {
        if let Some(ext_map) = bv.extensions()
            && let Some(mo) = ext_map.get("EXT_meshopt_compression")
        {
            let src_buf_idx = mo.get("buffer").and_then(Value::as_u64).unwrap_or(0) as usize;
            let byte_offset = mo.get("byteOffset").and_then(Value::as_u64).unwrap_or(0) as usize;
            let byte_length = mo
                .get("byteLength")
                .and_then(Value::as_u64)
                .ok_or(LoaderError::Parse("meshopt missing byteLength"))?
                as usize;
            let byte_stride = mo
                .get("byteStride")
                .and_then(Value::as_u64)
                .ok_or(LoaderError::Parse("meshopt missing byteStride"))?
                as usize;
            let count =
                mo.get("count")
                    .and_then(Value::as_u64)
                    .ok_or(LoaderError::Parse("meshopt missing count"))? as usize;
            let mode = match mo
                .get("mode")
                .and_then(Value::as_str)
                .unwrap_or("ATTRIBUTES")
            {
                "ATTRIBUTES" => MeshoptMode::Attributes,
                "TRIANGLES" => MeshoptMode::Triangles,
                "INDICES" => MeshoptMode::Indices,
                _ => return Err(LoaderError::Parse("meshopt unknown mode")),
            };
            let filter = match mo.get("filter").and_then(Value::as_str).unwrap_or("NONE") {
                "OCTAHEDRAL" => MeshoptFilter::Octahedral,
                "QUATERNION" => MeshoptFilter::Quaternion,
                "EXPONENTIAL" => MeshoptFilter::Exponential,
                _ => MeshoptFilter::None,
            };
            let raw_buf = buffers
                .get(src_buf_idx)
                .ok_or(LoaderError::Parse("meshopt buffer index out of bounds"))?
                .as_slice();
            let end = byte_offset
                .checked_add(byte_length)
                .ok_or(LoaderError::Parse("meshopt offset overflow"))?;
            let encoded = raw_buf
                .get(byte_offset..end)
                .ok_or(LoaderError::Parse("meshopt slice out of bounds"))?;
            let decoded = decode_meshopt(encoded, count, byte_stride, mode, filter)?;
            views.push(ResolvedView::Owned(decoded));
            continue;
        }

        let buf_idx = bv.buffer().index();
        let off = bv.offset();
        let len = bv.length();
        let end = off
            .checked_add(len)
            .ok_or(LoaderError::Parse("bufferView offset overflow"))?;
        let raw_buf = buffers
            .get(buf_idx)
            .ok_or(LoaderError::Parse("bufferView invalid buffer index"))?;
        let view_slice = raw_buf
            .as_slice()
            .get(off..end)
            .ok_or(LoaderError::Parse("bufferView range out of bounds"))?;
        // Store owned slice if parent buffer was owned, else point directly into mmap!
        match raw_buf {
            ResolvedView::Borrowed(_) => {
                // Convert back with lifetime `'a` safely from `bin_chunk`!
                if let Some(bin) = bin_chunk
                    && let Some(sub) = bin.get(off..end)
                {
                    views.push(ResolvedView::Borrowed(sub));
                    continue;
                }
                views.push(ResolvedView::Owned(view_slice.to_vec()));
            }
            ResolvedView::Owned(_) => views.push(ResolvedView::Owned(view_slice.to_vec())),
        }
    }
    Ok(views)
}

fn read_accessor_bytes<'v>(
    acc: &gltf::Accessor<'_>,
    views: &'v [ResolvedView<'_>],
) -> Result<(&'v [u8], usize, usize), LoaderError> {
    let view = acc
        .view()
        .ok_or(LoaderError::Parse("accessor missing bufferView"))?;
    let v_idx = view.index();
    let v_slice = views
        .get(v_idx)
        .ok_or(LoaderError::Parse("accessor bufferView out of range"))?
        .as_slice();
    let start = acc.offset();
    let elem_size = acc.size();
    if elem_size == 0 {
        return Err(LoaderError::Parse("accessor zero element size"));
    }
    let stride = view.stride().unwrap_or(elem_size);
    if stride < elem_size {
        return Err(LoaderError::Parse("bufferView stride smaller than element"));
    }
    let count = acc.count();
    if count == 0 {
        return Ok((&[], stride, elem_size));
    }
    let last_end = start
        .checked_add((count - 1).saturating_mul(stride))
        .and_then(|x| x.checked_add(elem_size))
        .ok_or(LoaderError::Parse("accessor span overflow"))?;
    let sub = v_slice
        .get(start..last_end)
        .ok_or(LoaderError::Parse("accessor data out of bounds"))?;
    Ok((sub, stride, elem_size))
}

#[inline]
fn read_f32_le(s: &[u8]) -> f32 {
    let v = f32::from_le_bytes([s[0], s[1], s[2], s[3]]);
    if v.is_finite() { v } else { 0.0 }
}

fn read_vec3_iter(
    acc: &gltf::Accessor<'_>,
    views: &[ResolvedView<'_>],
) -> Result<Vec<[f32; 3]>, LoaderError> {
    if acc.dimensions() != gltf::accessor::Dimensions::Vec3
        || acc.data_type() != gltf::accessor::DataType::F32
    {
        return Err(LoaderError::Parse("expected F32 VEC3 accessor"));
    }
    check_vertex_count(acc.count())?;
    let (data, stride, elem) = read_accessor_bytes(acc, views)?;
    if elem < 12 {
        return Err(LoaderError::Parse("expected F32 VEC3 accessor"));
    }
    let count = acc.count();
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let base = i * stride;
        let b = &data[base..base + 12];
        out.push([
            read_f32_le(&b[0..4]),
            read_f32_le(&b[4..8]),
            read_f32_le(&b[8..12]),
        ]);
    }
    Ok(out)
}

fn read_vec2_f32(
    acc: &gltf::Accessor<'_>,
    views: &[ResolvedView<'_>],
) -> Result<Vec<[f32; 2]>, LoaderError> {
    if acc.dimensions() != gltf::accessor::Dimensions::Vec2 {
        return Err(LoaderError::Parse("expected VEC2 accessor"));
    }
    check_vertex_count(acc.count())?;
    let (data, stride, elem) = read_accessor_bytes(acc, views)?;
    let count = acc.count();
    let mut out = Vec::with_capacity(count);
    match acc.data_type() {
        gltf::accessor::DataType::F32 if elem >= 8 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 8];
                out.push([read_f32_le(&b[0..4]), read_f32_le(&b[4..8])]);
            }
        }
        gltf::accessor::DataType::U16 if elem >= 4 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 4];
                out.push([
                    f32::from(u16::from_le_bytes([b[0], b[1]])) / 65535.0,
                    f32::from(u16::from_le_bytes([b[2], b[3]])) / 65535.0,
                ]);
            }
        }
        gltf::accessor::DataType::U8 if elem >= 2 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 2];
                out.push([f32::from(b[0]) / 255.0, f32::from(b[1]) / 255.0]);
            }
        }
        _ => return Err(LoaderError::Parse("unsupported UV data type")),
    }
    Ok(out)
}

fn read_joints_u16(
    acc: &gltf::Accessor<'_>,
    views: &[ResolvedView<'_>],
) -> Result<Vec<[u16; 4]>, LoaderError> {
    if acc.dimensions() != gltf::accessor::Dimensions::Vec4 {
        return Err(LoaderError::Parse("expected VEC4 JOINTS_0 accessor"));
    }
    check_vertex_count(acc.count())?;
    let (data, stride, elem) = read_accessor_bytes(acc, views)?;
    let count = acc.count();
    let mut out = Vec::with_capacity(count);
    match acc.data_type() {
        gltf::accessor::DataType::U8 if elem >= 4 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 4];
                out.push([
                    u16::from(b[0]),
                    u16::from(b[1]),
                    u16::from(b[2]),
                    u16::from(b[3]),
                ]);
            }
        }
        gltf::accessor::DataType::U16 if elem >= 8 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 8];
                out.push([
                    u16::from_le_bytes([b[0], b[1]]),
                    u16::from_le_bytes([b[2], b[3]]),
                    u16::from_le_bytes([b[4], b[5]]),
                    u16::from_le_bytes([b[6], b[7]]),
                ]);
            }
        }
        _ => return Err(LoaderError::Parse("unsupported JOINTS_0 data type")),
    }
    Ok(out)
}

fn read_weights_f32(
    acc: &gltf::Accessor<'_>,
    views: &[ResolvedView<'_>],
) -> Result<Vec<[f32; 4]>, LoaderError> {
    if acc.dimensions() != gltf::accessor::Dimensions::Vec4 {
        return Err(LoaderError::Parse("expected VEC4 WEIGHTS_0 accessor"));
    }
    check_vertex_count(acc.count())?;
    let (data, stride, elem) = read_accessor_bytes(acc, views)?;
    let count = acc.count();
    let mut out = Vec::with_capacity(count);
    match acc.data_type() {
        gltf::accessor::DataType::F32 if elem >= 16 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 16];
                out.push([
                    read_f32_le(&b[0..4]),
                    read_f32_le(&b[4..8]),
                    read_f32_le(&b[8..12]),
                    read_f32_le(&b[12..16]),
                ]);
            }
        }
        gltf::accessor::DataType::U16 if elem >= 8 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 8];
                out.push([
                    f32::from(u16::from_le_bytes([b[0], b[1]])) / 65535.0,
                    f32::from(u16::from_le_bytes([b[2], b[3]])) / 65535.0,
                    f32::from(u16::from_le_bytes([b[4], b[5]])) / 65535.0,
                    f32::from(u16::from_le_bytes([b[6], b[7]])) / 65535.0,
                ]);
            }
        }
        gltf::accessor::DataType::U8 if elem >= 4 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 4];
                out.push([
                    f32::from(b[0]) / 255.0,
                    f32::from(b[1]) / 255.0,
                    f32::from(b[2]) / 255.0,
                    f32::from(b[3]) / 255.0,
                ]);
            }
        }
        _ => return Err(LoaderError::Parse("unsupported WEIGHTS_0 data type")),
    }
    Ok(out)
}

fn read_indices_u32(
    acc: &gltf::Accessor<'_>,
    views: &[ResolvedView<'_>],
) -> Result<Vec<u32>, LoaderError> {
    if acc.dimensions() != gltf::accessor::Dimensions::Scalar {
        return Err(LoaderError::Parse("expected SCALAR index accessor"));
    }
    check_index_count(acc.count())?;
    let (data, stride, elem) = read_accessor_bytes(acc, views)?;
    let count = acc.count();
    let mut out = Vec::with_capacity(count);
    match acc.data_type() {
        gltf::accessor::DataType::U8 => {
            for i in 0..count {
                out.push(u32::from(data[i * stride]));
            }
        }
        gltf::accessor::DataType::U16 if elem >= 2 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 2];
                out.push(u32::from(u16::from_le_bytes([b[0], b[1]])));
            }
        }
        gltf::accessor::DataType::U32 if elem >= 4 => {
            for i in 0..count {
                let b = &data[i * stride..i * stride + 4];
                out.push(u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
            }
        }
        _ => return Err(LoaderError::Parse("unsupported index component type")),
    }
    Ok(out)
}

fn val_vec3(v: Option<&Value>, def: [f32; 3]) -> [f32; 3] {
    let Some(arr) = v.and_then(Value::as_array) else {
        return def;
    };
    if arr.len() < 3 {
        return def;
    }
    [
        arr[0].as_f64().unwrap_or(f64::from(def[0])) as f32,
        arr[1].as_f64().unwrap_or(f64::from(def[1])) as f32,
        arr[2].as_f64().unwrap_or(f64::from(def[2])) as f32,
    ]
}

fn val_vec4(v: Option<&Value>, def: [f32; 4]) -> [f32; 4] {
    let Some(arr) = v.and_then(Value::as_array) else {
        return def;
    };
    if arr.len() < 4 {
        return def;
    }
    [
        arr[0].as_f64().unwrap_or(f64::from(def[0])) as f32,
        arr[1].as_f64().unwrap_or(f64::from(def[1])) as f32,
        arr[2].as_f64().unwrap_or(f64::from(def[2])) as f32,
        arr[3].as_f64().unwrap_or(f64::from(def[3])) as f32,
    ]
}

fn obj_xyz(v: Option<&Value>, def: [f32; 3]) -> [f32; 3] {
    let Some(o) = v.and_then(Value::as_object) else {
        return def;
    };
    [
        o.get("x")
            .and_then(Value::as_f64)
            .unwrap_or(f64::from(def[0])) as f32,
        o.get("y")
            .and_then(Value::as_f64)
            .unwrap_or(f64::from(def[1])) as f32,
        o.get("z")
            .and_then(Value::as_f64)
            .unwrap_or(f64::from(def[2])) as f32,
    ]
}

fn parse_raw_json_value(val: Option<&gltf::json::extras::RawValue>) -> Option<Value> {
    val.and_then(|rv| serde_json::from_str::<Value>(rv.get()).ok())
}

fn validate_gltf_root(root: gltf::json::Root) -> Result<gltf::Document, LoaderError> {
    use gltf::json::validation::Checked;

    check_draco_extension(Some(&root.extensions_required))?;
    check_draco_extension(Some(&root.extensions_used))?;

    if root.images.len() > MAX_TEXTURES
        || root.materials.len() > MAX_MATERIALS
        || root.nodes.len() > 65_536
        || root.meshes.len() > 65_536
        || root.accessors.len() > 262_144
        || root.buffer_views.len() > 262_144
        || root.buffers.len() > 4_096
        || root.skins.len() > 1_024
        || root.textures.len() > MAX_TEXTURES * 2
    {
        return Err(LoaderError::LimitExceeded(
            "glTF resource table cap exceeded",
        ));
    }

    let check_usize64 = |v: u64| -> Result<(), LoaderError> {
        usize::try_from(v).map_err(|_| LoaderError::Parse("glTF 64-bit size overflow"))?;
        Ok(())
    };
    let check_opt_idx = |idx: usize, len: usize| -> Result<(), LoaderError> {
        if idx < len {
            Ok(())
        } else {
            Err(LoaderError::Parse("glTF index out of bounds"))
        }
    };

    if let Some(s) = root.scene {
        check_opt_idx(s.value(), root.scenes.len())?;
    }
    for sc in &root.scenes {
        for n in &sc.nodes {
            check_opt_idx(n.value(), root.nodes.len())?;
        }
    }
    for buf in &root.buffers {
        check_usize64(buf.byte_length.0)?;
    }
    for bv in &root.buffer_views {
        check_opt_idx(bv.buffer.value(), root.buffers.len())?;
        check_usize64(bv.byte_length.0)?;
        if let Some(off) = bv.byte_offset {
            check_usize64(off.0)?;
        }
        if let Some(Checked::Invalid) = bv.target {
            return Err(LoaderError::Parse("invalid glTF bufferView target"));
        }
    }
    for acc in &root.accessors {
        if let Some(bv) = acc.buffer_view {
            check_opt_idx(bv.value(), root.buffer_views.len())?;
        }
        if matches!(acc.component_type, Checked::Invalid) || matches!(acc.type_, Checked::Invalid) {
            return Err(LoaderError::Parse(
                "invalid glTF accessor type/componentType",
            ));
        }
        check_usize64(acc.count.0)?;
        if let Some(off) = acc.byte_offset {
            check_usize64(off.0)?;
        }
        if let Some(sp) = &acc.sparse {
            check_usize64(sp.count.0)?;
            check_opt_idx(sp.indices.buffer_view.value(), root.buffer_views.len())?;
            if matches!(sp.indices.component_type, Checked::Invalid) {
                return Err(LoaderError::Parse(
                    "invalid glTF sparse index componentType",
                ));
            }
            check_usize64(sp.indices.byte_offset.0)?;
            check_opt_idx(sp.values.buffer_view.value(), root.buffer_views.len())?;
            check_usize64(sp.values.byte_offset.0)?;
        }
    }
    for img in &root.images {
        if let Some(bv) = img.buffer_view {
            check_opt_idx(bv.value(), root.buffer_views.len())?;
            if img.mime_type.is_none() {
                return Err(LoaderError::Parse("glTF bufferView image missing mimeType"));
            }
        } else if img.uri.is_none() {
            return Err(LoaderError::Parse(
                "glTF image missing both uri and bufferView",
            ));
        }
    }
    for smp in &root.samplers {
        if matches!(smp.mag_filter, Some(Checked::Invalid))
            || matches!(smp.min_filter, Some(Checked::Invalid))
            || matches!(smp.wrap_s, Checked::Invalid)
            || matches!(smp.wrap_t, Checked::Invalid)
        {
            return Err(LoaderError::Parse("invalid glTF sampler filter/wrap"));
        }
    }
    for tex in &root.textures {
        check_opt_idx(tex.source.value(), root.images.len())?;
        if let Some(smp) = tex.sampler {
            check_opt_idx(smp.value(), root.samplers.len())?;
        }
    }
    for mat in &root.materials {
        if matches!(mat.alpha_mode, Checked::Invalid) {
            return Err(LoaderError::Parse("invalid glTF material alphaMode"));
        }
        if let Some(info) = &mat.pbr_metallic_roughness.base_color_texture {
            check_opt_idx(info.index.value(), root.textures.len())?;
        }
        if let Some(info) = &mat.pbr_metallic_roughness.metallic_roughness_texture {
            check_opt_idx(info.index.value(), root.textures.len())?;
        }
        if let Some(info) = &mat.normal_texture {
            check_opt_idx(info.index.value(), root.textures.len())?;
        }
        if let Some(info) = &mat.occlusion_texture {
            check_opt_idx(info.index.value(), root.textures.len())?;
        }
        if let Some(info) = &mat.emissive_texture {
            check_opt_idx(info.index.value(), root.textures.len())?;
        }
    }
    for cam in &root.cameras {
        match cam.type_ {
            Checked::Valid(gltf::json::camera::Type::Orthographic)
                if cam.orthographic.is_some() => {}
            Checked::Valid(gltf::json::camera::Type::Perspective) if cam.perspective.is_some() => {}
            _ => return Err(LoaderError::Parse("invalid glTF camera definition")),
        }
    }
    for node in &root.nodes {
        if let Some(children) = &node.children {
            for ch in children {
                check_opt_idx(ch.value(), root.nodes.len())?;
            }
        }
        if let Some(m) = node.mesh {
            check_opt_idx(m.value(), root.meshes.len())?;
        }
        if let Some(s) = node.skin {
            check_opt_idx(s.value(), root.skins.len())?;
        }
        if let Some(c) = node.camera {
            check_opt_idx(c.value(), root.cameras.len())?;
        }
    }
    for mesh in &root.meshes {
        for prim in &mesh.primitives {
            if matches!(prim.mode, Checked::Invalid) {
                return Err(LoaderError::Parse("invalid glTF primitive mode"));
            }
            if let Some(ind) = prim.indices {
                check_opt_idx(ind.value(), root.accessors.len())?;
            }
            if let Some(m) = prim.material {
                check_opt_idx(m.value(), root.materials.len())?;
            }
            for (sem, acc_idx) in &prim.attributes {
                if matches!(sem, Checked::Invalid) {
                    return Err(LoaderError::Parse("invalid glTF attribute semantic"));
                }
                check_opt_idx(acc_idx.value(), root.accessors.len())?;
            }
            if let Some(tgts) = &prim.targets {
                for tgt in tgts {
                    if let Some(a) = tgt.positions {
                        check_opt_idx(a.value(), root.accessors.len())?;
                    }
                    if let Some(a) = tgt.normals {
                        check_opt_idx(a.value(), root.accessors.len())?;
                    }
                    if let Some(a) = tgt.tangents {
                        check_opt_idx(a.value(), root.accessors.len())?;
                    }
                }
            }
        }
    }
    for skin in &root.skins {
        if let Some(ibm) = skin.inverse_bind_matrices {
            check_opt_idx(ibm.value(), root.accessors.len())?;
        }
        if let Some(sk) = skin.skeleton {
            check_opt_idx(sk.value(), root.nodes.len())?;
        }
        for j in &skin.joints {
            check_opt_idx(j.value(), root.nodes.len())?;
        }
    }
    for anim in &root.animations {
        for smp in &anim.samplers {
            check_opt_idx(smp.input.value(), root.accessors.len())?;
            check_opt_idx(smp.output.value(), root.accessors.len())?;
            if matches!(smp.interpolation, Checked::Invalid) {
                return Err(LoaderError::Parse("invalid glTF animation interpolation"));
            }
        }
        for ch in &anim.channels {
            check_opt_idx(ch.sampler.value(), anim.samplers.len())?;
            check_opt_idx(ch.target.node.value(), root.nodes.len())?;
            if matches!(ch.target.path, Checked::Invalid) {
                return Err(LoaderError::Parse("invalid glTF animation path"));
            }
        }
    }

    Ok(gltf::Document::from_json_without_validation(root))
}

/// Parses glTF 2.0 / GLB / VRM 0.x / VRM 1.0 directly from memory (`data` is the mapped source file).
pub fn parse_vrm_or_gltf(data: &[u8], base_dir: &Path) -> Result<ParsedAvatar, LoaderError> {
    let (doc, bin_chunk, raw_bin_offset_in_file): (gltf::Document, Option<&[u8]>, Option<usize>) =
        if data.starts_with(b"glTF") {
            let glb = gltf::Glb::from_slice(data)
                .map_err(|_| LoaderError::Parse("invalid GLB binary header/chunks"))?;
            let bin_off = glb
                .bin
                .as_ref()
                .map(|b| b.as_ptr() as usize - data.as_ptr() as usize);
            let root = gltf::json::Root::from_slice(&glb.json)
                .map_err(|_| LoaderError::Parse("invalid glTF JSON chunk"))?;
            let doc = validate_gltf_root(root)?;
            // SAFETY-free: re-slice `data` at exact offset of BIN chunk!
            let bin_slice = match (bin_off, glb.bin.as_ref()) {
                (Some(off), Some(b)) => data.get(off..off + b.len()),
                _ => None,
            };
            (doc, bin_slice, bin_off)
        } else {
            let root = gltf::json::Root::from_slice(data)
                .map_err(|_| LoaderError::Parse("invalid glTF JSON document"))?;
            let doc = validate_gltf_root(root)?;
            (doc, None, None)
        };

    check_draco_extension(Some(
        &doc.extensions_required()
            .map(str::to_owned)
            .collect::<Vec<_>>(),
    ))?;
    check_draco_extension(Some(
        &doc.extensions_used().map(str::to_owned).collect::<Vec<_>>(),
    ))?;

    let mut warnings_mask = 0u32;
    if doc.extensions_used().any(|e| e == "KHR_node_constraint") {
        warnings_mask |= AETHER_WARN_NODE_CONSTRAINT_APPROX;
    }

    let root_ext = doc.extensions();
    let is_vrm1 = root_ext.is_some_and(|e| e.contains_key("VRMC_vrm"));
    let is_vrm0 = !is_vrm1 && root_ext.is_some_and(|e| e.contains_key("VRM"));
    let source_format = if is_vrm1 {
        AETHER_SOURCE_FORMAT_VRM1
    } else if is_vrm0 {
        AETHER_SOURCE_FORMAT_VRM0
    } else {
        AETHER_SOURCE_FORMAT_GLTF
    };

    let views = resolve_all_buffer_views(&doc, bin_chunk, base_dir)?;

    // Build texture descriptors (image data stays un-decoded in `open`, header dimensions read lazily later)
    let img_list: Vec<gltf::Image<'_>> = doc.images().collect();
    if img_list.len() > MAX_TEXTURES {
        return Err(LoaderError::LimitExceeded("texture count exceeds 512"));
    }
    let mut textures = Vec::with_capacity(doc.textures().len().max(img_list.len()));
    let json_doc = doc.as_json();
    let json_view_map: Vec<gltf::buffer::View<'_>> = doc.views().collect();

    for tex in doc.textures() {
        let img = tex.source();
        let name = tex
            .name()
            .or_else(|| img.name())
            .map_or_else(|| format!("tex_{}", tex.index()), str::to_owned);
        let source = match img.source() {
            gltf::image::Source::View { view, .. } => {
                // Zero-copy mmap slice if raw GLB bufferView without meshopt/base64!
                if let (Some(bin_off), Some(bv)) =
                    (raw_bin_offset_in_file, json_view_map.get(view.index()))
                    && bv
                        .extensions()
                        .is_none_or(|m| !m.contains_key("EXT_meshopt_compression"))
                {
                    let start = bin_off + bv.offset();
                    let end = start + bv.length();
                    if end <= data.len() {
                        RawTextureSource::MappedSlice {
                            offset: start,
                            len: bv.length(),
                        }
                    } else {
                        RawTextureSource::Missing
                    }
                } else {
                    RawTextureSource::EmbeddedBytes(
                        views
                            .get(view.index())
                            .map_or_else(Vec::new, |v| v.as_slice().to_vec()),
                    )
                }
            }
            gltf::image::Source::Uri { uri, .. } => {
                if let Some(rest) = uri.strip_prefix("data:")
                    && let Some((_, b64)) = rest.split_once(";base64,")
                {
                    RawTextureSource::EmbeddedBytes(decode_base64_simple(b64).unwrap_or_default())
                } else if let Some(path) = safe_external_path(base_dir, uri) {
                    RawTextureSource::ExternalFile(path)
                } else {
                    warnings_mask |= aether_ffi::AETHER_WARN_MISSING_TEXTURES;
                    RawTextureSource::Missing
                }
            }
        };
        textures.push(RawTextureDesc {
            name,
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

    // Build skeleton joints mapping all nodes (or skin+vrm+spring nodes)
    let all_nodes: Vec<gltf::Node<'_>> = doc.nodes().collect();
    if all_nodes.len() > crate::limits::MAX_BONES {
        // Only reject if skeleton joints exceed limit; check after selecting bone set, unless > 4096 total nodes
        if all_nodes.len() > 8192 {
            return Err(LoaderError::LimitExceeded("glTF node graph exceeds 8192"));
        }
    }

    let mut parent_node_of: Vec<Option<usize>> = vec![None; all_nodes.len()];
    for node in &all_nodes {
        for ch in node.children() {
            parent_node_of[ch.index()] = Some(node.index());
        }
        if let Some(ext) = node.extensions()
            && ext.contains_key("KHR_node_constraint")
        {
            warnings_mask |= AETHER_WARN_NODE_CONSTRAINT_APPROX;
        }
    }

    // Collect inverse bind matrices & joint order from first skin
    let primary_skin = doc.skins().next();
    let mut node_ibm: HashMap<usize, [f32; 16]> = HashMap::new();
    let mut node_to_joint: HashMap<usize, usize> = HashMap::new();
    let mut joint_node_list: Vec<usize> = Vec::new();

    if let Some(skin) = &primary_skin {
        let skin_joints: Vec<gltf::Node<'_>> = skin.joints().collect();
        check_bone_count(skin_joints.len())?;
        let ibms: Option<Vec<[f32; 16]>> = skin.inverse_bind_matrices().and_then(|acc| {
            let (bytes, stride, elem) = read_accessor_bytes(&acc, &views).ok()?;
            if elem < 64 {
                return None;
            }
            let mut list = Vec::with_capacity(acc.count());
            for i in 0..acc.count() {
                let b = &bytes[i * stride..];
                // glTF column-major -> store row-major 4x4
                let m = |col: usize, row: usize| read_f32_le(&b[(col * 4 + row) * 4..]);
                list.push([
                    m(0, 0),
                    m(1, 0),
                    m(2, 0),
                    m(3, 0),
                    m(0, 1),
                    m(1, 1),
                    m(2, 1),
                    m(3, 1),
                    m(0, 2),
                    m(1, 2),
                    m(2, 2),
                    m(3, 2),
                    m(0, 3),
                    m(1, 3),
                    m(2, 3),
                    m(3, 3),
                ]);
            }
            Some(list)
        });
        for (idx, jn) in skin_joints.iter().enumerate() {
            node_to_joint.insert(jn.index(), joint_node_list.len());
            joint_node_list.push(jn.index());
            if let Some(ref mats) = ibms
                && let Some(&mat) = mats.get(idx)
            {
                node_ibm.insert(jn.index(), mat);
            }
        }
    } else {
        check_bone_count(all_nodes.len())?;
        for n in &all_nodes {
            node_to_joint.insert(n.index(), joint_node_list.len());
            joint_node_list.push(n.index());
        }
    }

    // Also collect explicit VRM humanoid mappings
    let mut explicit_humanoid_by_node: HashMap<usize, u16> = HashMap::new();
    if is_vrm1
        && let Some(vrm1) = root_ext.and_then(|e| e.get("VRMC_vrm"))
        && let Some(hb) = vrm1
            .get("humanoid")
            .and_then(|h| h.get("humanBones"))
            .and_then(Value::as_object)
    {
        for (bname, bobj) in hb {
            if let Some(h_idx) = crate::bone_matcher::canonical_humanoid_index(bname)
                && let Some(n_idx) = bobj.get("node").and_then(Value::as_u64)
            {
                explicit_humanoid_by_node.insert(n_idx as usize, h_idx);
            }
        }
    } else if is_vrm0
        && let Some(vrm0) = root_ext.and_then(|e| e.get("VRM"))
        && let Some(arr) = vrm0
            .get("humanoid")
            .and_then(|h| h.get("humanBones"))
            .and_then(Value::as_array)
    {
        for entry in arr {
            let Some(bname) = entry.get("bone").and_then(Value::as_str) else {
                continue;
            };
            let Some(n_idx) = entry.get("node").and_then(Value::as_u64) else {
                continue;
            };
            if let Some(h_idx) = crate::bone_matcher::canonical_humanoid_index(bname) {
                explicit_humanoid_by_node.insert(n_idx as usize, h_idx);
            }
        }
    }

    // Build `joints`
    check_bone_count(joint_node_list.len())?;
    let mut joints = Vec::with_capacity(joint_node_list.len());
    for (ord, &node_idx) in joint_node_list.iter().enumerate() {
        let node = &all_nodes[node_idx];
        let (t, r, s) = node.transform().decomposed();
        let parent_j = parent_node_of[node_idx].and_then(|p| node_to_joint.get(&p).copied());
        joints.push(RawJoint {
            name: node
                .name()
                .map_or_else(|| format!("joint_{ord}"), str::to_owned),
            alt_name: String::new(),
            parent: parent_j,
            local_translation: t,
            local_rotation: r,
            local_scale: s,
            inverse_bind_matrix: node_ibm.get(&node_idx).copied(),
            explicit_humanoid: explicit_humanoid_by_node.get(&node_idx).copied(),
            ordinal: ord,
        });
    }

    // Parse Materials (+ VRM0 materialProperties OR VRM1 VRMC_materials_mtoon)
    let gltf_mats: Vec<gltf::Material<'_>> = doc.materials().collect();
    if gltf_mats.len() > MAX_MATERIALS {
        return Err(LoaderError::LimitExceeded("material count exceeds 1024"));
    }
    let vrm0_mat_props = if is_vrm0 {
        root_ext
            .and_then(|e| e.get("VRM"))
            .and_then(|v| v.get("materialProperties"))
            .and_then(Value::as_array)
    } else {
        None
    };

    let mut materials = Vec::with_capacity(gltf_mats.len().max(1));
    for mat in &gltf_mats {
        let pbr = mat.pbr_metallic_roughness();
        let base_color = pbr.base_color_factor();
        let mut raw = RawMaterial {
            name: mat.name().map_or_else(
                || format!("mat_{}", mat.index().unwrap_or(0)),
                str::to_owned,
            ),
            base_color,
            explicit_shade_color: None,
            rim_color: [0.0, 0.0, 0.0],
            rim_lighting_mix: 0.0,
            rim_fresnel_power: 4.0,
            rim_lift: 0.0,
            outline_color: [0.12, 0.08, 0.08],
            outline_lighting_mix: 0.5,
            outline_width: 0.0,
            outline_screen_space: false,
            emissive: mat.emissive_factor(),
            matcap_color: [1.0, 1.0, 1.0],
            matcap_add_mix: 0.0,
            matcap_strength: 0.0,
            gi_equalization: 0.15,
            shading_shift: 0.0,
            shading_toony: 0.9,
            uv_scroll: [0.0, 0.0],
            uv_rotation: 0.0,
            alpha_mode: match mat.alpha_mode() {
                gltf::material::AlphaMode::Opaque => AETHER_ALPHA_MODE_OPAQUE,
                gltf::material::AlphaMode::Mask => AETHER_ALPHA_MODE_MASK,
                gltf::material::AlphaMode::Blend => AETHER_ALPHA_MODE_BLEND,
            },
            alpha_cutoff: mat.alpha_cutoff().unwrap_or(0.5),
            double_sided: mat.double_sided(),
            unlit: mat.unlit(),
            has_vrm_mtoon: false,
            render_queue_offset: 0,
            flatten_override: None,
            tex_base: pbr.base_color_texture().map(|t| t.texture().index() as u32),
            tex_shade: None,
            tex_shading_shift: None,
            tex_normal: mat.normal_texture().map(|t| t.texture().index() as u32),
            tex_emissive: mat.emissive_texture().map(|t| t.texture().index() as u32),
            tex_rim: None,
            tex_matcap: None,
            tex_outline_width: None,
            tex_uv_mask: None,
            tex_ramp: None,
            tex_sdf_face: None,
        };

        // Mark non-color normal/mask texture usage flags
        if let Some(ti) = raw.tex_normal
            && let Some(td) = textures.get_mut(ti as usize)
        {
            td.srgb = false;
            td.usage_flags = aether_ffi::AETHER_TEX_USAGE_NORMAL;
        }

        // VRM 1.0 MToon
        if let Some(mtoon) = mat.extensions().and_then(|e| e.get("VRMC_materials_mtoon")) {
            raw.has_vrm_mtoon = true;
            if let Some(sc) = mtoon.get("shadeColorFactor") {
                raw.explicit_shade_color = Some(val_vec3(Some(sc), [0.85, 0.72, 0.72]));
            }
            raw.tex_shade = mtoon
                .get("shadeMultiplyTexture")
                .and_then(|t| t.get("index"))
                .and_then(Value::as_u64)
                .map(|i| i as u32);
            raw.shading_shift = mtoon
                .get("shadingShiftFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.0) as f32;
            raw.shading_toony = mtoon
                .get("shadingToonyFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.9) as f32;
            raw.gi_equalization = mtoon
                .get("giEqualizationFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.15) as f32;
            raw.rim_color = val_vec3(mtoon.get("parametricRimColorFactor"), [0.0, 0.0, 0.0]);
            raw.rim_fresnel_power = mtoon
                .get("parametricRimFresnelPowerFactor")
                .and_then(Value::as_f64)
                .unwrap_or(4.0) as f32;
            raw.rim_lift = mtoon
                .get("parametricRimLiftFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.0) as f32;
            raw.rim_lighting_mix = mtoon
                .get("rimLightingMixFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.0) as f32;
            let omode = mtoon
                .get("outlineWidthMode")
                .and_then(Value::as_str)
                .unwrap_or("none");
            raw.outline_screen_space = omode == "screenCoordinates";
            raw.outline_width = if omode == "none" {
                0.0
            } else {
                mtoon
                    .get("outlineWidthFactor")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.002) as f32
            };
            raw.outline_color = val_vec3(mtoon.get("outlineColorFactor"), [0.1, 0.08, 0.08]);
            raw.outline_lighting_mix = mtoon
                .get("outlineLightingMixFactor")
                .and_then(Value::as_f64)
                .unwrap_or(0.5) as f32;
            raw.render_queue_offset = mtoon
                .get("renderQueueOffsetNumber")
                .and_then(Value::as_i64)
                .unwrap_or(0) as i32;
        } else if let (Some(idx), Some(props_arr)) = (mat.index(), vrm0_mat_props)
            && let Some(mp) = props_arr.get(idx)
        {
            raw.has_vrm_mtoon = true;
            let vp = mp.get("vectorProperties");
            let fp = mp.get("floatProperties");
            if let Some(vp) = vp {
                let c = val_vec4(vp.get("_Color"), raw.base_color);
                raw.base_color = c;
                if let Some(sc) = vp.get("_ShadeColor") {
                    let s4 = val_vec4(Some(sc), [0.85, 0.72, 0.72, 1.0]);
                    raw.explicit_shade_color = Some([s4[0], s4[1], s4[2]]);
                }
                let rc = val_vec4(vp.get("_RimColor"), [0.0, 0.0, 0.0, 1.0]);
                raw.rim_color = [rc[0], rc[1], rc[2]];
                let oc = val_vec4(vp.get("_OutlineColor"), [0.1, 0.08, 0.08, 1.0]);
                raw.outline_color = [oc[0], oc[1], oc[2]];
            }
            if let Some(fp) = fp {
                raw.shading_shift =
                    fp.get("_ShadeShift").and_then(Value::as_f64).unwrap_or(0.0) as f32;
                raw.shading_toony =
                    fp.get("_ShadeToony").and_then(Value::as_f64).unwrap_or(0.9) as f32;
                raw.rim_fresnel_power = fp
                    .get("_RimFresnelPower")
                    .and_then(Value::as_f64)
                    .unwrap_or(4.0) as f32;
                raw.rim_lift = fp.get("_RimLift").and_then(Value::as_f64).unwrap_or(0.0) as f32;
                raw.rim_lighting_mix = fp
                    .get("_RimLightingMix")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.5) as f32;
                let ow_mode = fp
                    .get("_OutlineWidthMode")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.0) as i32;
                let ow = fp
                    .get("_OutlineWidth")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.0) as f32;
                raw.outline_screen_space = ow_mode == 2;
                // VRM 0.x world outline is authored in cm (0.01 m) (§6.8)
                raw.outline_width = match ow_mode {
                    1 => ow * 0.01,
                    2 => ow,
                    _ => 0.0,
                };
                if fp.get("_CullMode").and_then(Value::as_f64).unwrap_or(2.0) == 0.0 {
                    raw.double_sided = true;
                }
            }
            raw.render_queue_offset = mp
                .get("renderQueue")
                .and_then(Value::as_i64)
                .map_or(0, |rq| (rq as i32) - 2000);
        }

        if let Some(extras) = parse_raw_json_value(mat.extras().as_deref()) {
            if let Some(sdf_t) = extras.get("aetherFaceSdfTexture").and_then(Value::as_u64) {
                raw.tex_sdf_face = Some(sdf_t as u32);
                if let Some(td) = textures.get_mut(sdf_t as usize) {
                    td.srgb = false;
                    td.usage_flags = aether_ffi::AETHER_TEX_USAGE_SDF_FACE;
                }
            }
            if let Some(fw) = extras.get("aetherFlattenWeight").and_then(Value::as_f64) {
                raw.flatten_override = Some((fw as f32).clamp(0.0, 0.85));
            }
        }
        materials.push(raw);
    }
    if materials.is_empty() {
        materials.push(RawMaterial {
            name: "default".to_owned(),
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
        });
    }

    // Extract vertices, submeshes & morph targets
    let mut vertices = Vec::new();
    let mut submeshes = Vec::new();
    let mut morphs = Vec::new();
    let mut mesh_node_morph_base: HashMap<usize, usize> = HashMap::new();
    let mut total_morph_deltas = 0usize;

    let mut mesh_nodes: Vec<(usize, gltf::Mesh<'_>)> = Vec::new();
    for node in &all_nodes {
        if let Some(m) = node.mesh() {
            mesh_nodes.push((node.index(), m));
        }
    }
    if mesh_nodes.is_empty() {
        for m in doc.meshes() {
            mesh_nodes.push((usize::MAX, m));
        }
    }

    for (node_idx, mesh) in mesh_nodes {
        let mesh_first_morph = morphs.len();
        if node_idx != usize::MAX {
            mesh_node_morph_base.insert(node_idx, mesh_first_morph);
        }
        let mesh_target_names: Option<Vec<String>> = parse_raw_json_value(mesh.extras().as_deref())
            .and_then(|v| {
                v.get("targetNames").and_then(Value::as_array).map(|a| {
                    a.iter()
                        .filter_map(Value::as_str)
                        .map(str::to_owned)
                        .collect()
                })
            });

        for prim in mesh.primitives() {
            check_draco_map(prim.extensions())?;
            let pos_acc = prim
                .get(&gltf::Semantic::Positions)
                .ok_or(LoaderError::Parse("glTF primitive missing POSITION"))?;
            let v_count = pos_acc.count();
            let next_v_len = vertices
                .len()
                .checked_add(v_count)
                .ok_or(LoaderError::LimitExceeded("vertex count overflow"))?;
            check_vertex_count(next_v_len)?;

            let positions = read_vec3_iter(&pos_acc, &views)?;
            let v_count = positions.len();
            let normals = match prim.get(&gltf::Semantic::Normals) {
                Some(acc) => read_vec3_iter(&acc, &views)?,
                None => vec![[0.0, 1.0, 0.0]; v_count],
            };
            let uv0s = match prim.get(&gltf::Semantic::TexCoords(0)) {
                Some(acc) => read_vec2_f32(&acc, &views)?,
                None => vec![[0.0, 0.0]; v_count],
            };
            let uv1s = match prim.get(&gltf::Semantic::TexCoords(1)) {
                Some(acc) => read_vec2_f32(&acc, &views)?,
                None => vec![[0.0, 0.0]; v_count],
            };
            let j0s = match prim.get(&gltf::Semantic::Joints(0)) {
                Some(acc) => read_joints_u16(&acc, &views)?,
                None => vec![[0; 4]; v_count],
            };
            let w0s = match prim.get(&gltf::Semantic::Weights(0)) {
                Some(acc) => read_weights_f32(&acc, &views)?,
                None => vec![[1.0, 0.0, 0.0, 0.0]; v_count],
            };
            let j1s = prim
                .get(&gltf::Semantic::Joints(1))
                .and_then(|acc| read_joints_u16(&acc, &views).ok());
            let w1s = prim
                .get(&gltf::Semantic::Weights(1))
                .and_then(|acc| read_weights_f32(&acc, &views).ok());

            let mat_idx = prim.material().index().unwrap_or(0) as u32;
            let mat_flatten = materials
                .get(mat_idx as usize)
                .and_then(|m| m.flatten_override)
                .unwrap_or(-1.0);

            let v_base = vertices.len() as u32;
            for (i, &position) in positions.iter().enumerate() {
                let nrm = normals.get(i).copied().unwrap_or([0.0, 1.0, 0.0]);
                let u0 = uv0s.get(i).copied().unwrap_or([0.0, 0.0]);
                let u1 = uv1s.get(i).copied().unwrap_or([0.0, 0.0]);
                let j0 = j0s.get(i).copied().unwrap_or([0; 4]);
                let w0 = w0s.get(i).copied().unwrap_or([1.0, 0.0, 0.0, 0.0]);
                let j1 = j1s
                    .as_ref()
                    .and_then(|v| v.get(i).copied())
                    .unwrap_or([0; 4]);
                let w1 = w1s
                    .as_ref()
                    .and_then(|v| v.get(i).copied())
                    .unwrap_or([0.0; 4]);
                vertices.push(RawVertex {
                    position,
                    normal: nrm,
                    uv0: u0,
                    uv1: u1,
                    color: [255, 255, 255, 255],
                    joints: [j0[0], j0[1], j0[2], j0[3], j1[0], j1[1], j1[2], j1[3]],
                    weights: [w0[0], w0[1], w0[2], w0[3], w1[0], w1[1], w1[2], w1[3]],
                    edge_scale: 1.0,
                    flatten_override: mat_flatten,
                });
            }

            let raw_inds = match prim.indices() {
                Some(acc) => read_indices_u32(&acc, &views)?,
                None => (0..v_count as u32).collect(),
            };
            check_index_count(raw_inds.len())?;
            let mut indices = Vec::with_capacity(raw_inds.len());
            for idx in raw_inds {
                if (idx as usize) >= v_count {
                    return Err(LoaderError::Parse("primitive index out of vertex range"));
                }
                indices.push(v_base + idx);
            }
            let sub_name = prim
                .material()
                .name()
                .or_else(|| mesh.name())
                .map_or_else(|| format!("submesh_{}", submeshes.len()), str::to_owned);
            let lower = sub_name.to_ascii_lowercase();
            submeshes.push(RawSubmesh {
                name: sub_name,
                indices,
                material_index: mat_idx,
                is_eye_or_brow_hint: lower.contains("eye")
                    || lower.contains("brow")
                    || lower.contains("lash")
                    || lower.contains("iris"),
            });

            // Morph targets on primitive
            let json_prim = json_doc
                .meshes
                .get(mesh.index())
                .and_then(|jm| jm.primitives.get(prim.index()));
            let prim_target_names: Option<Vec<String>> = json_prim
                .and_then(|jp| parse_raw_json_value(jp.extras.as_deref()))
                .and_then(|v| {
                    v.get("targetNames").and_then(Value::as_array).map(|a| {
                        a.iter()
                            .filter_map(Value::as_str)
                            .map(str::to_owned)
                            .collect()
                    })
                });

            let target_count = prim.morph_targets().len();
            check_morph_count(morphs.len() + target_count)?;

            for (t_i, target) in prim.morph_targets().enumerate() {
                let tname = prim_target_names
                    .as_ref()
                    .and_then(|n| n.get(t_i))
                    .or_else(|| mesh_target_names.as_ref().and_then(|n| n.get(t_i)))
                    .cloned()
                    .unwrap_or_else(|| format!("morph_{}", morphs.len()));

                let mut deltas = Vec::new();
                let mut sum_x = 0.0f32;
                if let Some(dp_acc) = target.positions() {
                    let dpos = read_vec3_iter(&dp_acc, &views)?;
                    let dnorm = target
                        .normals()
                        .and_then(|na| read_vec3_iter(&na, &views).ok());
                    for (vi, dp) in dpos.iter().enumerate() {
                        if vi >= v_count {
                            break;
                        }
                        if dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2] > 1e-12 {
                            let abs_v = v_base + vi as u32;
                            sum_x += positions[vi][0];
                            deltas.push(RawMorphDeltaItem {
                                vertex_index: abs_v,
                                d_pos: *dp,
                                d_norm: dnorm
                                    .as_ref()
                                    .and_then(|n| n.get(vi).copied())
                                    .unwrap_or([0.0; 3]),
                            });
                        }
                    }
                }
                total_morph_deltas = total_morph_deltas.saturating_add(deltas.len());
                if total_morph_deltas > MAX_MORPH_DELTAS {
                    return Err(LoaderError::LimitExceeded("morph deltas exceed 16M"));
                }
                let centroid_x = if deltas.is_empty() {
                    0.0
                } else {
                    sum_x / (deltas.len() as f32)
                };
                morphs.push(RawMorphTarget {
                    name: tname,
                    deltas,
                    centroid_x,
                });
            }
        }
    }

    // Expressions
    let mut expr_presets = Vec::new();
    if is_vrm1
        && let Some(exprs) = root_ext
            .and_then(|e| e.get("VRMC_vrm"))
            .and_then(|v| v.get("expressions"))
    {
        for group_key in ["preset", "custom"] {
            if let Some(obj) = exprs.get(group_key).and_then(Value::as_object) {
                for (ename, eobj) in obj {
                    let mut binds = Vec::new();
                    if let Some(arr) = eobj.get("morphTargetBinds").and_then(Value::as_array) {
                        for b in arr {
                            let node_i =
                                b.get("node").and_then(Value::as_u64).unwrap_or(0) as usize;
                            let m_rel =
                                b.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                            let w = b.get("weight").and_then(Value::as_f64).unwrap_or(1.0) as f32;
                            let base = mesh_node_morph_base.get(&node_i).copied().unwrap_or(0);
                            let m_idx = base + m_rel;
                            if m_idx < morphs.len() {
                                binds.push((m_idx, w));
                            }
                        }
                    }
                    expr_presets.push(RawExprPreset {
                        name: ename.clone(),
                        morph_binds: binds,
                    });
                }
            }
        }
    } else if is_vrm0
        && let Some(groups) = root_ext
            .and_then(|e| e.get("VRM"))
            .and_then(|v| v.get("blendShapeMaster"))
            .and_then(|b| b.get("blendShapeGroups"))
            .and_then(Value::as_array)
    {
        for g in groups {
            let pname = g
                .get("presetName")
                .and_then(Value::as_str)
                .filter(|s| *s != "unknown" && !s.is_empty())
                .or_else(|| g.get("name").and_then(Value::as_str))
                .unwrap_or("custom")
                .to_owned();
            let mut binds = Vec::new();
            if let Some(arr) = g.get("binds").and_then(Value::as_array) {
                for b in arr {
                    let m_idx = b.get("index").and_then(Value::as_u64).unwrap_or(0) as usize;
                    let w =
                        (b.get("weight").and_then(Value::as_f64).unwrap_or(100.0) as f32) / 100.0;
                    if m_idx < morphs.len() {
                        binds.push((m_idx, w));
                    }
                }
            }
            expr_presets.push(RawExprPreset {
                name: pname,
                morph_binds: binds,
            });
        }
    } else {
        for (m_idx, m) in morphs.iter().enumerate() {
            expr_presets.push(RawExprPreset {
                name: m.name.clone(),
                morph_binds: vec![(m_idx, 1.0)],
            });
        }
    }

    // SpringBones & Colliders
    let mut colliders = Vec::new();
    let mut spring_chains = Vec::new();
    if is_vrm1 && let Some(sb) = root_ext.and_then(|e| e.get("VRMC_springBone")) {
        let mut group_masks = Vec::new();
        if let Some(col_arr) = sb.get("colliders").and_then(Value::as_array) {
            for c in col_arr.iter().take(MAX_COLLIDERS) {
                let n_idx = c.get("node").and_then(Value::as_u64).unwrap_or(0) as usize;
                let j_idx = node_to_joint.get(&n_idx).copied().unwrap_or(0);
                let shape = c.get("shape");
                if let Some(sph) = shape.and_then(|s| s.get("sphere")) {
                    colliders.push(RawCollider {
                        joint_index: j_idx,
                        group_bit: 0,
                        shape_kind: AETHER_COLLIDER_SHAPE_SPHERE,
                        radius: sph.get("radius").and_then(Value::as_f64).unwrap_or(0.05) as f32,
                        offset: val_vec3(sph.get("offset"), [0.0; 3]),
                        tail: [0.0; 3],
                    });
                } else if let Some(cap) = shape.and_then(|s| s.get("capsule")) {
                    colliders.push(RawCollider {
                        joint_index: j_idx,
                        group_bit: 0,
                        shape_kind: AETHER_COLLIDER_SHAPE_CAPSULE,
                        radius: cap.get("radius").and_then(Value::as_f64).unwrap_or(0.05) as f32,
                        offset: val_vec3(cap.get("offset"), [0.0; 3]),
                        tail: val_vec3(cap.get("tail"), [0.0, 0.1, 0.0]),
                    });
                }
            }
        }
        if let Some(cg_arr) = sb.get("colliderGroups").and_then(Value::as_array) {
            for (g_bit, cg) in cg_arr.iter().enumerate() {
                if let Some(idxs) = cg.get("colliders").and_then(Value::as_array) {
                    for ci in idxs {
                        if let Some(c) = ci.as_u64().and_then(|idx| colliders.get_mut(idx as usize))
                        {
                            c.group_bit = (g_bit as u32) & 63;
                        }
                    }
                }
                group_masks.push(1u64 << (g_bit.min(63)));
            }
        }
        if let Some(sp_arr) = sb.get("springs").and_then(Value::as_array) {
            for sp in sp_arr {
                let center_joint = sp
                    .get("center")
                    .and_then(Value::as_u64)
                    .and_then(|n| node_to_joint.get(&(n as usize)).copied());
                let mut mask = 0u64;
                if let Some(cgs) = sp.get("colliderGroups").and_then(Value::as_array) {
                    for g in cgs {
                        if let Some(&bit) = g.as_u64().and_then(|idx| group_masks.get(idx as usize))
                        {
                            mask |= bit;
                        }
                    }
                }
                let mut s_nodes = Vec::new();
                if let Some(j_arr) = sp.get("joints").and_then(Value::as_array) {
                    for sj in j_arr {
                        let Some(n_i) = sj.get("node").and_then(Value::as_u64) else {
                            continue;
                        };
                        let Some(&j_idx) = node_to_joint.get(&(n_i as usize)) else {
                            continue;
                        };
                        s_nodes.push(RawSpringNode {
                            joint_index: j_idx,
                            hit_radius: sj.get("hitRadius").and_then(Value::as_f64).unwrap_or(0.02)
                                as f32,
                            stiffness: sj.get("stiffness").and_then(Value::as_f64).unwrap_or(1.0)
                                as f32,
                            drag_force: sj.get("dragForce").and_then(Value::as_f64).unwrap_or(0.4)
                                as f32,
                            gravity_power: sj
                                .get("gravityPower")
                                .and_then(Value::as_f64)
                                .unwrap_or(0.0) as f32,
                            gravity_dir: val_vec3(sj.get("gravityDir"), [0.0, -1.0, 0.0]),
                            from_mmd: false,
                        });
                    }
                }
                if !s_nodes.is_empty() {
                    spring_chains.push(RawSpringChain {
                        joints: s_nodes,
                        center_joint,
                        collider_group_mask: mask,
                    });
                }
            }
        }
    } else if is_vrm0
        && let Some(sa) = root_ext
            .and_then(|e| e.get("VRM"))
            .and_then(|v| v.get("secondaryAnimation"))
    {
        let mut group_masks = Vec::new();
        if let Some(cg_arr) = sa.get("colliderGroups").and_then(Value::as_array) {
            for (g_bit, cg) in cg_arr.iter().enumerate() {
                let n_idx = cg.get("node").and_then(Value::as_u64).unwrap_or(0) as usize;
                let j_idx = node_to_joint.get(&n_idx).copied().unwrap_or(0);
                if let Some(cols) = cg.get("colliders").and_then(Value::as_array) {
                    for c in cols {
                        if colliders.len() < MAX_COLLIDERS {
                            colliders.push(RawCollider {
                                joint_index: j_idx,
                                group_bit: (g_bit as u32) & 63,
                                shape_kind: AETHER_COLLIDER_SHAPE_SPHERE,
                                radius: c.get("radius").and_then(Value::as_f64).unwrap_or(0.05)
                                    as f32,
                                offset: obj_xyz(c.get("offset"), [0.0; 3]),
                                tail: [0.0; 3],
                            });
                        }
                    }
                }
                group_masks.push(1u64 << g_bit.min(63));
            }
        }
        if let Some(bg_arr) = sa.get("boneGroups").and_then(Value::as_array) {
            for bg in bg_arr {
                let center_joint = bg.get("center").and_then(Value::as_i64).and_then(|n| {
                    if n >= 0 {
                        node_to_joint.get(&(n as usize)).copied()
                    } else {
                        None
                    }
                });
                let stiff = bg
                    .get("stiffiness")
                    .or_else(|| bg.get("stiffness"))
                    .and_then(Value::as_f64)
                    .unwrap_or(1.0) as f32;
                let drag = bg.get("dragForce").and_then(Value::as_f64).unwrap_or(0.4) as f32;
                let gpow = bg
                    .get("gravityPower")
                    .and_then(Value::as_f64)
                    .unwrap_or(0.0) as f32;
                let gdir = obj_xyz(bg.get("gravityDir"), [0.0, -1.0, 0.0]);
                let hrad = bg.get("hitRadius").and_then(Value::as_f64).unwrap_or(0.02) as f32;
                let mut mask = 0u64;
                if let Some(cgs) = bg.get("colliderGroups").and_then(Value::as_array) {
                    for g in cgs {
                        if let Some(&bit) = g.as_u64().and_then(|idx| group_masks.get(idx as usize))
                        {
                            mask |= bit;
                        }
                    }
                }
                let mut s_nodes = Vec::new();
                if let Some(bones) = bg.get("bones").and_then(Value::as_array) {
                    for b in bones {
                        if let Some(n_i) = b.as_u64()
                            && let Some(&j_idx) = node_to_joint.get(&(n_i as usize))
                        {
                            s_nodes.push(RawSpringNode {
                                joint_index: j_idx,
                                hit_radius: hrad,
                                stiffness: stiff,
                                drag_force: drag,
                                gravity_power: gpow,
                                gravity_dir: gdir,
                                from_mmd: false,
                            });
                        }
                    }
                }
                if !s_nodes.is_empty() {
                    spring_chains.push(RawSpringChain {
                        joints: s_nodes,
                        center_joint,
                        collider_group_mask: mask,
                    });
                }
            }
        }
    }

    Ok(ParsedAvatar {
        source_format,
        base_dir: base_dir.to_path_buf(),
        unit_meta: SourceUnitMetadata {
            // Standard glTF / VRM specifies metres, though non-standard assets exist and get verified in §6.3
            explicit_unit_scale: Some(1.0),
            resolved_unit_scale: 1.0,
            scale_guessed: false,
            left_handed_flipped: false,
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
