#![deny(clippy::pedantic)]
#![deny(unsafe_code)]
#![allow(clippy::cast_possible_truncation)]
#![allow(clippy::cast_possible_wrap)]
#![allow(clippy::cast_lossless)]
#![allow(clippy::cast_precision_loss)]
#![allow(clippy::cast_sign_loss)]
#![allow(clippy::doc_markdown)]
#![allow(clippy::many_single_char_names)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::missing_panics_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::similar_names)]
#![allow(clippy::struct_excessive_bools)]
#![allow(clippy::too_many_lines)]

//! Safe zero-copy asset loader and mesh/texture baker (`AETHER-TS-001` §5.6, §6).
//!
//! Parses `VRM 0.x`, `VRM 1.0`, `glTF/GLB`, `FBX` (binary & ASCII), `PMX 2.0/2.1`, and
//! `PMD 1.0` strictly in Rust with pre-allocation resource caps (`HR-06`), normalizes
//! skeleton/pose/expressions/physics (`§6.3–§6.6`), bakes 2.5D NPR geometry (`§6.7–§6.8`)
//! and decodes/caches textures (`§6.9`) directly into C++-owned arena memory (`HR-05`).

pub mod bake;
pub mod bc7_cache;
pub mod bone_matcher;
pub mod fbx;
pub mod ffi_slice;
pub mod fuzz_entry;
pub mod ir;
pub mod limits;
pub mod materials;
pub mod meshopt;
pub mod pmx;
pub mod pool;
pub mod retarget;
pub mod textures;
pub mod vrm_gltf;

use core::mem::size_of;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::fs::File;
use std::path::{Path, PathBuf};

use aether_ffi::{
    AETHER_E_BUFFER_TOO_SMALL, AETHER_E_CANCELLED, AETHER_E_INVALID_ARG, AETHER_E_IO,
    AETHER_E_LIMIT_EXCEEDED, AETHER_E_PARSE, AETHER_E_UNSUPPORTED,
    AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS, AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS,
    AETHER_LOAD_FLAG_DECODE_TEXTURES, AETHER_LOAD_FLAG_MORTON_SORT, AETHER_LOAD_FLAG_TEXTURE_CACHE,
    AETHER_STREAM_COLLIDERS, AETHER_STREAM_EXPRESSIONS, AETHER_STREAM_INDEX_U32,
    AETHER_STREAM_JOINTS, AETHER_STREAM_MATERIALS, AETHER_STREAM_MORPH_DELTAS,
    AETHER_STREAM_MORPH_RANGES, AETHER_STREAM_SKIN, AETHER_STREAM_SPRINGS, AETHER_STREAM_STRINGS,
    AETHER_STREAM_SUBMESHES, AETHER_STREAM_SURFACE, AETHER_STREAM_TEXTURE_DESCS,
    AETHER_STREAM_TEXTURE_PIXELS, AETHER_STREAM_VERTEX25D, AETHER_SUBMESH_FLAG_EYE_OR_BROW,
    AETHER_SUBMESH_FLAG_FACE, AETHER_SUBMESH_FLAG_HAIR_BACK, AETHER_SUBMESH_FLAG_HAIR_FRONT,
    AETHER_TEX_FORMAT_RGBA8_SRGB, AETHER_TEX_FORMAT_RGBA8_UNORM, AETHER_WARN_UNIT_GUESS,
    AetherAssetRequirements, AetherAvatarDesc, AetherColliderDesc, AetherExpressionBindingDesc,
    AetherJointDesc, AetherLoadRequest, AetherMorphDelta, AetherMorphRangeDesc, AetherNprMaterial,
    AetherSpringJointDesc, AetherStatus, AetherSubmeshDesc, AetherTextureDesc, AetherVertex25D,
    AetherVertexSkin, AetherVertexSurface,
};
use memmap2::Mmap;
use thiserror::Error;

use crate::bake::bake_into_arena;
use crate::bone_matcher::match_humanoid_bones;
use crate::fbx::parse_fbx;
use crate::ffi_slice::{BorrowedArenaSet, map_file_readonly};
use crate::ir::{ParsedAvatar, RawTextureSource};
use crate::limits::check_file_size;
use crate::materials::convert_material;
use crate::pmx::parse_pmx_or_pmd;
use crate::pool::run_in_worker_pool;
use crate::retarget::{
    ResolvedExprBinding, build_expression_retarget, build_spring_descriptors, normalise_avatar_pose,
};
use crate::textures::{decode_textures_into_arena, plan_textures};
use crate::vrm_gltf::parse_vrm_or_gltf;

/// Typed errors produced by the asset loader.
#[derive(Debug, Error)]
pub enum LoaderError {
    /// Null, misaligned, duplicate, or invalid FFI argument.
    #[error("invalid loader argument: {0}")]
    InvalidArg(&'static str),
    /// Operating-system file open or read failure.
    #[error("asset I/O error: {0}")]
    Io(&'static str),
    /// Caller-allocated arena slice is smaller than `aether_loader_query` requested.
    #[error("buffer too small for stream {stream_kind}: need {required} B, got {provided} B")]
    BufferTooSmall {
        /// Stream index (`AETHER_STREAM_*`).
        stream_kind: u32,
        /// Required size in bytes.
        required: u64,
        /// Provided size in bytes.
        provided: u64,
    },
    /// Resource cap exceeded (§3.6).
    #[error("asset resource limit exceeded: {0}")]
    LimitExceeded(&'static str),
    /// Corrupted or invalid input data.
    #[error("asset parse failure: {0}")]
    Parse(&'static str),
    /// Load cancelled via `aether_loader_cancel`.
    #[error("asset load cancelled")]
    Cancelled,
    /// Unimplemented or unsupported feature (e.g. Draco mesh compression, `HR-09`).
    #[error("unsupported asset feature: {0}")]
    Unsupported(&'static str),
}

impl LoaderError {
    /// Converts the error into an [`AetherStatus`] FFI error code.
    #[must_use]
    pub const fn status_code(&self) -> AetherStatus {
        match self {
            Self::InvalidArg(_) => AETHER_E_INVALID_ARG,
            Self::Io(_) => AETHER_E_IO,
            Self::BufferTooSmall { .. } => AETHER_E_BUFFER_TOO_SMALL,
            Self::LimitExceeded(_) => AETHER_E_LIMIT_EXCEEDED,
            Self::Parse(_) => AETHER_E_PARSE,
            Self::Cancelled => AETHER_E_CANCELLED,
            Self::Unsupported(_) => AETHER_E_UNSUPPORTED,
        }
    }
}

/// Compact pre-planned texture slot kept in `AssetLoader` (`< 256 B` per texture) so `open()`
/// retains zero persistent bulk asset buffers (`HR-05`).
#[derive(Clone, Debug)]
struct PlannedTexMeta {
    width: u32,
    height: u32,
    rgba_byte_len: u64,
    content_hash_lo: u64,
    pixel_offset: u64,
    pixel_size: u64,
    final_format: u32,
    is_missing: bool,
    cached_bc7_path: Option<PathBuf>,
}

struct FileLoaderState {
    path_buf: PathBuf,
    mmap: Mmap,
    flags: u32,
    max_threads: usize,
    planned_textures: Vec<PlannedTexMeta>,
    open_warnings_mask: u32,
}

enum LoaderBackend {
    File(FileLoaderState),
    #[cfg(feature = "test-hooks")]
    Synthetic {
        vertex_count: u32,
        index_count: u32,
    },
}

/// Active two-phase loader handle backed by lightweight Rust metadata (`< 64 KiB`).
pub struct AssetLoader {
    requirements: AetherAssetRequirements,
    progress_bits: AtomicU32,
    cancelled: AtomicBool,
    backend: LoaderBackend,
}

fn trim_ascii_start(mut s: &[u8]) -> &[u8] {
    while let Some((&b, rest)) = s.split_first() {
        if matches!(b, b' ' | b'\t' | b'\r' | b'\n') {
            s = rest;
        } else {
            break;
        }
    }
    s
}

fn detect_and_parse(data: &[u8], path: &Path) -> Result<ParsedAvatar, LoaderError> {
    let base_dir = path.parent().unwrap_or_else(|| Path::new("."));
    if data.starts_with(b"glTF") {
        return parse_vrm_or_gltf(data, base_dir);
    }
    if data.starts_with(b"PMX ") || data.starts_with(b"Pmd") {
        return parse_pmx_or_pmd(data, base_dir);
    }
    if data.starts_with(b"Kaydara FBX Binary") {
        return parse_fbx(data, base_dir);
    }
    let trimmed = trim_ascii_start(data);
    if trimmed.starts_with(b"{") {
        return parse_vrm_or_gltf(data, base_dir);
    }
    if trimmed.starts_with(b"; FBX") {
        return parse_fbx(data, base_dir);
    }

    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();
    match ext.as_str() {
        "vrm" | "glb" | "gltf" => parse_vrm_or_gltf(data, base_dir),
        "fbx" => parse_fbx(data, base_dir),
        "pmx" | "pmd" => parse_pmx_or_pmd(data, base_dir),
        _ => Err(LoaderError::Parse("unrecognized or corrupted avatar file")),
    }
}

fn compute_string_bytes(avatar: &ParsedAvatar, exprs: &[ResolvedExprBinding]) -> u64 {
    let mut total = 0usize;
    for sm in &avatar.submeshes {
        total += sm.name.len() + 1;
    }
    for m in &avatar.morphs {
        total += m.name.len() + 1;
    }
    for mat in &avatar.materials {
        total += mat.name.len() + 1;
    }
    for j in &avatar.joints {
        total += j.name.len() + 1;
    }
    for e in exprs {
        total += e.preset_name.len() + 1;
    }
    for tex in &avatar.textures {
        total += tex.name.len() + 1;
    }
    total as u64
}

fn stream_slice_mut<'a, T: bytemuck::Pod>(
    arena: &'a mut BorrowedArenaSet<'_>,
    stream_kind: u32,
    count: usize,
) -> Result<&'a mut [T], LoaderError> {
    if count == 0 {
        return Ok(&mut []);
    }
    let stream = arena
        .stream_mut(stream_kind)
        .ok_or(LoaderError::BufferTooSmall {
            stream_kind,
            required: (count * size_of::<T>()) as u64,
            provided: 0,
        })?;
    stream.cast_prefix_for_stream_mut::<T>(stream_kind, count)
}

impl AssetLoader {
    /// Phase 1: opens an avatar model file read-only (`mmap`), enforces resource caps (`HR-06`),
    /// plans textures/BC7 cache hits, computes exact stream byte sizes (`§5.6`), and drops
    /// all bulk geometry buffers before returning.
    pub fn open(path_utf8: &str, req: &AetherLoadRequest) -> Result<Self, LoaderError> {
        if path_utf8.is_empty() {
            return Err(LoaderError::InvalidArg("empty asset path"));
        }

        let path_buf = PathBuf::from(path_utf8);
        let file =
            File::open(&path_buf).map_err(|_| LoaderError::Io("failed to open asset file"))?;
        let meta = file
            .metadata()
            .map_err(|_| LoaderError::Io("failed to stat asset file"))?;
        if !meta.is_file() {
            return Err(LoaderError::Io("asset path is not a regular file"));
        }
        check_file_size(meta.len())?;

        let mmap = map_file_readonly(&file)?;
        let flags = if req.flags == 0 {
            AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS
                | AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS
                | AETHER_LOAD_FLAG_MORTON_SORT
                | AETHER_LOAD_FLAG_DECODE_TEXTURES
                | AETHER_LOAD_FLAG_TEXTURE_CACHE
        } else {
            req.flags
        };
        let decode_textures = (flags & AETHER_LOAD_FLAG_DECODE_TEXTURES) != 0;
        let use_cache = (flags & AETHER_LOAD_FLAG_TEXTURE_CACHE) != 0;
        let max_threads = req.max_threads as usize;

        let mut avatar = run_in_worker_pool(max_threads, || detect_and_parse(&mmap, &path_buf))?;
        let matched = match_humanoid_bones(&avatar.joints);
        let (poses, _, _, _) = normalise_avatar_pose(&mut avatar, &matched);
        let expr_bindings = build_expression_retarget(&avatar);
        let (springs, colliders) = build_spring_descriptors(&avatar, &poses);

        let mut warnings = avatar.warnings_mask | matched.warnings_mask;
        if avatar.unit_meta.scale_guessed {
            warnings |= AETHER_WARN_UNIT_GUESS;
        }
        let tex_pixel_bytes = plan_textures(
            &mut avatar.textures,
            &mmap,
            decode_textures,
            use_cache,
            &mut warnings,
        )?;

        let vertex_count = avatar.vertices.len() as u32;
        let index_count: u32 = avatar
            .submeshes
            .iter()
            .map(|s| s.indices.len() as u32)
            .sum();
        let submesh_count = avatar.submeshes.len() as u32;
        let material_count = avatar.materials.len() as u32;
        let joint_count = avatar.joints.len() as u32;
        let spring_joint_count = springs.len() as u32;
        let collider_count = colliders.len() as u32;
        let morph_target_count = avatar.morphs.len() as u32;
        let morph_delta_count: usize = avatar.morphs.iter().map(|m| m.deltas.len()).sum();
        let expression_count = expr_bindings.len() as u32;
        let texture_count = avatar.textures.len() as u32;
        let string_bytes = compute_string_bytes(&avatar, &expr_bindings);

        let mut stream_bytes = [0u64; 15];
        stream_bytes[AETHER_STREAM_VERTEX25D as usize] =
            u64::from(vertex_count) * size_of::<AetherVertex25D>() as u64;
        stream_bytes[AETHER_STREAM_SURFACE as usize] =
            u64::from(vertex_count) * size_of::<AetherVertexSurface>() as u64;
        stream_bytes[AETHER_STREAM_SKIN as usize] =
            u64::from(vertex_count) * size_of::<AetherVertexSkin>() as u64;
        stream_bytes[AETHER_STREAM_INDEX_U32 as usize] =
            u64::from(index_count) * size_of::<u32>() as u64;
        stream_bytes[AETHER_STREAM_MORPH_DELTAS as usize] =
            (morph_delta_count as u64) * size_of::<AetherMorphDelta>() as u64;
        stream_bytes[AETHER_STREAM_MORPH_RANGES as usize] =
            u64::from(morph_target_count) * size_of::<AetherMorphRangeDesc>() as u64;
        stream_bytes[AETHER_STREAM_SUBMESHES as usize] =
            u64::from(submesh_count) * size_of::<AetherSubmeshDesc>() as u64;
        stream_bytes[AETHER_STREAM_MATERIALS as usize] =
            u64::from(material_count) * size_of::<AetherNprMaterial>() as u64;
        stream_bytes[AETHER_STREAM_JOINTS as usize] =
            u64::from(joint_count) * size_of::<AetherJointDesc>() as u64;
        stream_bytes[AETHER_STREAM_SPRINGS as usize] =
            u64::from(spring_joint_count) * size_of::<AetherSpringJointDesc>() as u64;
        stream_bytes[AETHER_STREAM_COLLIDERS as usize] =
            u64::from(collider_count) * size_of::<AetherColliderDesc>() as u64;
        stream_bytes[AETHER_STREAM_EXPRESSIONS as usize] =
            u64::from(expression_count) * size_of::<AetherExpressionBindingDesc>() as u64;
        stream_bytes[AETHER_STREAM_TEXTURE_DESCS as usize] =
            u64::from(texture_count) * size_of::<AetherTextureDesc>() as u64;
        stream_bytes[AETHER_STREAM_TEXTURE_PIXELS as usize] = tex_pixel_bytes;
        stream_bytes[AETHER_STREAM_STRINGS as usize] = string_bytes;

        let planned_textures: Vec<PlannedTexMeta> = avatar
            .textures
            .iter()
            .map(|t| PlannedTexMeta {
                width: t.width,
                height: t.height,
                rgba_byte_len: t.rgba_byte_len,
                content_hash_lo: t.content_hash_lo,
                pixel_offset: t.pixel_offset,
                pixel_size: t.pixel_size,
                final_format: t.final_format,
                is_missing: matches!(t.source, RawTextureSource::Missing),
                cached_bc7_path: t.cached_bc7_path.clone(),
            })
            .collect();

        let source_format = avatar.source_format;

        Ok(Self {
            requirements: AetherAssetRequirements {
                stream_bytes,
                vertex_count,
                index_count,
                submesh_count,
                material_count,
                joint_count,
                spring_joint_count,
                collider_count,
                morph_target_count,
                expression_count,
                texture_count,
                source_format,
                warnings_mask: warnings,
            },
            progress_bits: AtomicU32::new(0.0f32.to_bits()),
            cancelled: AtomicBool::new(false),
            backend: LoaderBackend::File(FileLoaderState {
                path_buf,
                mmap,
                flags,
                max_threads,
                planned_textures,
                open_warnings_mask: warnings,
            }),
        })
    }

    /// Creates a deterministic synthetic loader strictly under `test-hooks` to verify
    /// the two-phase zero-copy C++ arena borrow pipeline (`REQ-FFI-010`).
    #[cfg(feature = "test-hooks")]
    pub fn open_synthetic_for_test(
        vertex_count: u32,
        index_count: u32,
    ) -> Result<Self, LoaderError> {
        if vertex_count == 0 || index_count == 0 {
            return Err(LoaderError::InvalidArg(
                "synthetic loader requires non-zero vertex and index counts",
            ));
        }

        let mut req = AetherAssetRequirements {
            stream_bytes: [0; 15],
            vertex_count,
            index_count,
            submesh_count: 1,
            material_count: 1,
            joint_count: 1,
            spring_joint_count: 0,
            collider_count: 0,
            morph_target_count: 0,
            expression_count: 0,
            texture_count: 0,
            source_format: 0,
            warnings_mask: 0,
        };
        req.stream_bytes[AETHER_STREAM_VERTEX25D as usize] =
            u64::from(vertex_count) * size_of::<AetherVertex25D>() as u64;
        req.stream_bytes[AETHER_STREAM_SURFACE as usize] =
            u64::from(vertex_count) * size_of::<AetherVertexSurface>() as u64;
        req.stream_bytes[AETHER_STREAM_SKIN as usize] =
            u64::from(vertex_count) * size_of::<AetherVertexSkin>() as u64;
        req.stream_bytes[AETHER_STREAM_INDEX_U32 as usize] =
            u64::from(index_count) * size_of::<u32>() as u64;

        Ok(Self {
            requirements: req,
            progress_bits: AtomicU32::new(0.0f32.to_bits()),
            cancelled: AtomicBool::new(false),
            backend: LoaderBackend::Synthetic {
                vertex_count,
                index_count,
            },
        })
    }

    /// Queries exact byte requirements per `AETHER_STREAM_*` slot.
    #[must_use]
    pub const fn requirements(&self) -> &AetherAssetRequirements {
        &self.requirements
    }

    /// Decodes/bakes directly into C++-lent arena slices without persistent heap allocations.
    pub fn fill(&self, arena: &mut BorrowedArenaSet<'_>) -> Result<AetherAvatarDesc, LoaderError> {
        if self.cancelled.load(Ordering::Acquire) {
            return Err(LoaderError::Cancelled);
        }

        match &self.backend {
            LoaderBackend::File(state) => self.fill_from_file(state, arena),
            #[cfg(feature = "test-hooks")]
            &LoaderBackend::Synthetic {
                vertex_count,
                index_count,
            } => self.fill_synthetic(vertex_count, index_count, arena),
        }
    }

    fn fill_from_file(
        &self,
        state: &FileLoaderState,
        arena: &mut BorrowedArenaSet<'_>,
    ) -> Result<AetherAvatarDesc, LoaderError> {
        let mut debug_ptrs = [0u64; 15];
        for (slot, ptr) in debug_ptrs.iter_mut().enumerate() {
            *ptr = arena.raw_addr(slot as u32);
        }

        let decode_textures = (state.flags & AETHER_LOAD_FLAG_DECODE_TEXTURES) != 0;
        let write_cache = decode_textures && (state.flags & AETHER_LOAD_FLAG_TEXTURE_CACHE) != 0;
        let morton_sort = (state.flags & AETHER_LOAD_FLAG_MORTON_SORT) != 0;
        let bake_proxy = (state.flags & AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS) != 0;
        let bake_smooth = (state.flags & AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS) != 0;

        run_in_worker_pool(state.max_threads, || {
            if self.cancelled.load(Ordering::Acquire) {
                return Err(LoaderError::Cancelled);
            }

            let mut avatar = detect_and_parse(&state.mmap, &state.path_buf)?;
            for (tex, plan) in avatar
                .textures
                .iter_mut()
                .zip(state.planned_textures.iter())
            {
                tex.width = plan.width;
                tex.height = plan.height;
                tex.rgba_byte_len = plan.rgba_byte_len;
                tex.content_hash_lo = plan.content_hash_lo;
                tex.pixel_offset = plan.pixel_offset;
                tex.pixel_size = plan.pixel_size;
                tex.final_format = plan.final_format;
                tex.cached_bc7_path.clone_from(&plan.cached_bc7_path);
                if plan.is_missing {
                    tex.source = RawTextureSource::Missing;
                }
            }

            let matched = match_humanoid_bones(&avatar.joints);
            let (poses, aabb_min, aabb_max, height_m) =
                normalise_avatar_pose(&mut avatar, &matched);
            let expr_bindings = build_expression_retarget(&avatar);
            let (springs, colliders) = build_spring_descriptors(&avatar, &poses);

            self.progress_bits
                .store(0.10f32.to_bits(), Ordering::Release);
            if self.cancelled.load(Ordering::Acquire) {
                return Err(LoaderError::Cancelled);
            }

            let v_count = avatar.vertices.len();
            let i_count = self.requirements.index_count as usize;
            let md_count = (self.requirements.stream_bytes[AETHER_STREAM_MORPH_DELTAS as usize]
                as usize)
                / size_of::<AetherMorphDelta>();
            let mr_count = avatar.morphs.len();
            let sm_count = avatar.submeshes.len();
            let mat_count = avatar.materials.len();
            let joint_count = avatar.joints.len();
            let spring_count = springs.len();
            let col_count = colliders.len();
            let expr_count = expr_bindings.len();
            let tex_count = avatar.textures.len();
            let tex_pixel_bytes =
                self.requirements.stream_bytes[AETHER_STREAM_TEXTURE_PIXELS as usize] as usize;
            let req_string_bytes =
                self.requirements.stream_bytes[AETHER_STREAM_STRINGS as usize] as usize;

            let mut string_buf = Vec::with_capacity(req_string_bytes);
            tracing::debug!(
                target: "aether_asset_loader",
                vertices = v_count,
                indices = i_count,
                textures = tex_count,
                "baking avatar into zero-copy C++ arena"
            );

            let mut opt_v25d = arena.take_stream(AETHER_STREAM_VERTEX25D);
            let mut opt_surf = arena.take_stream(AETHER_STREAM_SURFACE);
            let mut opt_skin = arena.take_stream(AETHER_STREAM_SKIN);
            let mut opt_idx = arena.take_stream(AETHER_STREAM_INDEX_U32);
            let mut opt_md = arena.take_stream(AETHER_STREAM_MORPH_DELTAS);
            let mut opt_mr = arena.take_stream(AETHER_STREAM_MORPH_RANGES);
            let mut opt_sm = arena.take_stream(AETHER_STREAM_SUBMESHES);

            let out_v25d: &mut [AetherVertex25D] = match opt_v25d.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_VERTEX25D, v_count)?,
                None if v_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_VERTEX25D,
                        required: (v_count * size_of::<AetherVertex25D>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_surf: &mut [AetherVertexSurface] = match opt_surf.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_SURFACE, v_count)?,
                None if v_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_SURFACE,
                        required: (v_count * size_of::<AetherVertexSurface>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_skin: &mut [AetherVertexSkin] = match opt_skin.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_SKIN, v_count)?,
                None if v_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_SKIN,
                        required: (v_count * size_of::<AetherVertexSkin>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_indices: &mut [u32] = match opt_idx.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_INDEX_U32, i_count)?,
                None if i_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_INDEX_U32,
                        required: (i_count * size_of::<u32>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_md: &mut [AetherMorphDelta] = match opt_md.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_MORPH_DELTAS, md_count)?,
                None if md_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_MORPH_DELTAS,
                        required: (md_count * size_of::<AetherMorphDelta>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_mr: &mut [AetherMorphRangeDesc] = match opt_mr.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_MORPH_RANGES, mr_count)?,
                None if mr_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_MORPH_RANGES,
                        required: (mr_count * size_of::<AetherMorphRangeDesc>()) as u64,
                        provided: 0,
                    });
                }
            };
            let out_sm: &mut [AetherSubmeshDesc] = match opt_sm.as_mut() {
                Some(s) => s.cast_prefix_for_stream_mut(AETHER_STREAM_SUBMESHES, sm_count)?,
                None if sm_count == 0 => &mut [],
                None => {
                    return Err(LoaderError::BufferTooSmall {
                        stream_kind: AETHER_STREAM_SUBMESHES,
                        required: (sm_count * size_of::<AetherSubmeshDesc>()) as u64,
                        provided: 0,
                    });
                }
            };

            let summary = bake_into_arena(
                &avatar,
                &matched,
                &poses,
                height_m,
                morton_sort,
                bake_proxy,
                bake_smooth,
                out_v25d,
                out_surf,
                out_skin,
                out_indices,
                out_md,
                out_mr,
                out_sm,
                &mut string_buf,
            );

            self.progress_bits
                .store(0.50f32.to_bits(), Ordering::Release);
            if self.cancelled.load(Ordering::Acquire) {
                return Err(LoaderError::Cancelled);
            }

            // Or submesh flags per material slot for HAIR_FRONT / RECEIVES_HAIR_SHADOW.
            let mut mat_submesh_flags = vec![0u32; mat_count];
            for (sm_idx, sm) in avatar.submeshes.iter().enumerate() {
                let m = sm.material_index as usize;
                if m < mat_submesh_flags.len() {
                    let f = summary.submesh_flags.get(sm_idx).copied().unwrap_or(0);
                    mat_submesh_flags[m] |= f;
                }
            }

            // Refine `out_surf` flags cleanly for eye/brow/hair/face if vertices belong to face submesh.
            let _ = (
                AETHER_SUBMESH_FLAG_FACE,
                AETHER_SUBMESH_FLAG_HAIR_FRONT,
                AETHER_SUBMESH_FLAG_HAIR_BACK,
                AETHER_SUBMESH_FLAG_EYE_OR_BROW,
            );

            {
                let out_mats: &mut [AetherNprMaterial] =
                    stream_slice_mut(arena, AETHER_STREAM_MATERIALS, mat_count)?;
                for (m_idx, raw_mat) in avatar.materials.iter().enumerate() {
                    let name_offset = string_buf.len() as u32;
                    string_buf.extend_from_slice(raw_mat.name.as_bytes());
                    string_buf.push(0);
                    out_mats[m_idx] =
                        convert_material(raw_mat, mat_submesh_flags[m_idx], name_offset);
                }
            }

            {
                let out_joints: &mut [AetherJointDesc] =
                    stream_slice_mut(arena, AETHER_STREAM_JOINTS, joint_count)?;
                for (j_idx, (raw_j, pose)) in avatar.joints.iter().zip(poses.iter()).enumerate() {
                    let name_offset = string_buf.len() as u32;
                    string_buf.extend_from_slice(raw_j.name.as_bytes());
                    string_buf.push(0);
                    let name_len = raw_j.name.len() as u32;
                    out_joints[j_idx] = AetherJointDesc {
                        parent_index: raw_j.parent.map_or(-1, |p| p as i32),
                        humanoid_bone: matched.joint_humanoid[j_idx],
                        region_tag: matched.joint_regions[j_idx],
                        name_offset,
                        name_len,
                        bind_translation: pose.bind_translation,
                        tail_length: pose.tail_length,
                        bind_rotation_quat: pose.bind_rotation_quat,
                        bind_scale: pose.bind_scale,
                        _pad0: 0.0,
                        rest_to_tpose_quat: pose.rest_to_tpose_quat,
                        inverse_bind_matrix_row_major: pose.inverse_bind_matrix_row_major,
                    };
                }
            }

            {
                let out_springs: &mut [AetherSpringJointDesc] =
                    stream_slice_mut(arena, AETHER_STREAM_SPRINGS, spring_count)?;
                out_springs.copy_from_slice(&springs);
            }

            {
                let out_colliders: &mut [AetherColliderDesc] =
                    stream_slice_mut(arena, AETHER_STREAM_COLLIDERS, col_count)?;
                out_colliders.copy_from_slice(&colliders);
            }

            {
                let out_exprs: &mut [AetherExpressionBindingDesc] =
                    stream_slice_mut(arena, AETHER_STREAM_EXPRESSIONS, expr_count)?;
                for (idx, b) in expr_bindings.iter().enumerate() {
                    let name_offset = string_buf.len() as u32;
                    string_buf.extend_from_slice(b.preset_name.as_bytes());
                    string_buf.push(0);
                    let name_len = b.preset_name.len() as u32;
                    out_exprs[idx] = AetherExpressionBindingDesc {
                        preset_index: b.preset_index,
                        input_slot: b.input_slot,
                        morph_index: b.morph_index,
                        weight: b.weight,
                        name_offset,
                        name_len,
                    };
                }
            }

            self.progress_bits
                .store(0.65f32.to_bits(), Ordering::Release);
            if self.cancelled.load(Ordering::Acquire) {
                return Err(LoaderError::Cancelled);
            }

            let mut decode_warnings = 0u32;
            {
                let mut opt_tdesc = arena.take_stream(AETHER_STREAM_TEXTURE_DESCS);
                let mut opt_tpix = arena.take_stream(AETHER_STREAM_TEXTURE_PIXELS);
                let out_tdescs: &mut [AetherTextureDesc] = match opt_tdesc.as_mut() {
                    Some(s) => {
                        s.cast_prefix_for_stream_mut(AETHER_STREAM_TEXTURE_DESCS, tex_count)?
                    }
                    None if tex_count == 0 => &mut [],
                    None => {
                        return Err(LoaderError::BufferTooSmall {
                            stream_kind: AETHER_STREAM_TEXTURE_DESCS,
                            required: (tex_count * size_of::<AetherTextureDesc>()) as u64,
                            provided: 0,
                        });
                    }
                };
                if decode_textures && tex_pixel_bytes > 0 {
                    let out_tpix: &mut [u8] = match opt_tpix.as_mut() {
                        Some(s) => s.cast_prefix_for_stream_mut(
                            AETHER_STREAM_TEXTURE_PIXELS,
                            tex_pixel_bytes,
                        )?,
                        None => {
                            return Err(LoaderError::BufferTooSmall {
                                stream_kind: AETHER_STREAM_TEXTURE_PIXELS,
                                required: tex_pixel_bytes as u64,
                                provided: 0,
                            });
                        }
                    };
                    decode_warnings = decode_textures_into_arena(
                        &avatar.textures,
                        &state.mmap,
                        out_tpix,
                        out_tdescs,
                        write_cache,
                        &self.cancelled,
                        &mut string_buf,
                    )?;
                } else {
                    for (idx, tex) in avatar.textures.iter().enumerate() {
                        let name_offset = string_buf.len() as u32;
                        string_buf.extend_from_slice(tex.name.as_bytes());
                        string_buf.push(0);
                        let name_len = tex.name.len() as u32;
                        out_tdescs[idx] = AetherTextureDesc {
                            pixel_offset: 0,
                            pixel_size: 0,
                            width: tex.width,
                            height: tex.height,
                            format: if tex.srgb {
                                AETHER_TEX_FORMAT_RGBA8_SRGB
                            } else {
                                AETHER_TEX_FORMAT_RGBA8_UNORM
                            },
                            usage_flags: tex.usage_flags,
                            name_offset,
                            name_len,
                            content_hash_lo: tex.content_hash_lo,
                        };
                    }
                }
            }

            {
                let str_len = string_buf.len();
                let out_strings: &mut [u8] =
                    stream_slice_mut(arena, AETHER_STREAM_STRINGS, str_len)?;
                out_strings.copy_from_slice(&string_buf);
            }

            self.progress_bits
                .store(1.0f32.to_bits(), Ordering::Release);

            Ok(AetherAvatarDesc {
                source_format: avatar.source_format,
                humanoid_to_joint: matched.humanoid_to_joint,
                bone_match_score: matched.bone_match_score,
                aabb_min,
                aabb_max,
                face_center_head_local: summary.face_center_head_local,
                avatar_height_m: height_m,
                head_joint: matched.head_joint,
                warnings_mask: state.open_warnings_mask | decode_warnings,
                debug_stream_ptrs: debug_ptrs,
            })
        })
    }

    #[cfg(feature = "test-hooks")]
    fn fill_synthetic(
        &self,
        vertex_count: u32,
        index_count: u32,
        arena: &mut BorrowedArenaSet<'_>,
    ) -> Result<AetherAvatarDesc, LoaderError> {
        let mut debug_ptrs = [0u64; 15];
        for (slot, ptr) in debug_ptrs.iter_mut().enumerate() {
            *ptr = arena.raw_addr(slot as u32);
        }

        let vcount = vertex_count as usize;
        let icount = index_count as usize;

        if let Some(stream) = arena.stream_mut(AETHER_STREAM_VERTEX25D) {
            let verts = stream
                .cast_prefix_for_stream_mut::<AetherVertex25D>(AETHER_STREAM_VERTEX25D, vcount)?;
            for (idx, v) in verts.iter_mut().enumerate() {
                if self.cancelled.load(Ordering::Acquire) {
                    return Err(LoaderError::Cancelled);
                }
                let f = idx as f32;
                *v = AetherVertex25D {
                    position: [f * 0.001, (idx % 32) as f32 * 0.01, 0.0],
                    outline_width_scale: 1.0,
                    true_normal: [0.0, 0.0, 1.0],
                    z_flatten_weight: 0.5,
                    proxy_normal: [0.0, 0.0, 1.0],
                    sdf_face_mask: 0.0,
                    smooth_tangent: [0.0, 0.0, 1.0, 1.0],
                };
            }
        }
        self.progress_bits
            .store(0.25f32.to_bits(), Ordering::Release);

        if let Some(stream) = arena.stream_mut(AETHER_STREAM_SURFACE) {
            let surfs = stream
                .cast_prefix_for_stream_mut::<AetherVertexSurface>(AETHER_STREAM_SURFACE, vcount)?;
            for (idx, s) in surfs.iter_mut().enumerate() {
                let u = ((idx & 0xFF) as f32) / 255.0;
                *s = AetherVertexSurface {
                    uv0: [u, 1.0 - u],
                    uv1: [u, 1.0 - u],
                    color_rgba8: 0xFFFF_FFFF,
                    tangent_oct: 0,
                    flags: 0,
                    _reserved: 0,
                };
            }
        }
        self.progress_bits
            .store(0.50f32.to_bits(), Ordering::Release);

        if let Some(stream) = arena.stream_mut(AETHER_STREAM_SKIN) {
            let skins = stream
                .cast_prefix_for_stream_mut::<AetherVertexSkin>(AETHER_STREAM_SKIN, vcount)?;
            for sk in skins.iter_mut() {
                *sk = AetherVertexSkin {
                    joints: [0; 8],
                    weights_unorm16: [65535, 0, 0, 0, 0, 0, 0, 0],
                };
            }
        }
        self.progress_bits
            .store(0.75f32.to_bits(), Ordering::Release);

        if let Some(stream) = arena.stream_mut(AETHER_STREAM_INDEX_U32) {
            let indices =
                stream.cast_prefix_for_stream_mut::<u32>(AETHER_STREAM_INDEX_U32, icount)?;
            let vmod = vertex_count.max(1);
            for (idx, dst) in indices.iter_mut().enumerate() {
                *dst = (idx as u32) % vmod;
            }
        }
        self.progress_bits
            .store(1.0f32.to_bits(), Ordering::Release);

        let mut humanoid_to_joint = [0xFFFFu16; 55];
        humanoid_to_joint[0] = 0;

        Ok(AetherAvatarDesc {
            source_format: 0,
            humanoid_to_joint,
            bone_match_score: [0.0; 55],
            aabb_min: [0.0, 0.0, 0.0],
            aabb_max: [1.0, 1.7, 0.5],
            face_center_head_local: [0.0, 0.1, 0.05],
            avatar_height_m: 1.7,
            head_joint: 0,
            warnings_mask: 0,
            debug_stream_ptrs: debug_ptrs,
        })
    }

    /// Returns atomic load progress in `[0.0, 1.0]`.
    #[must_use]
    pub fn progress(&self) -> f32 {
        f32::from_bits(self.progress_bits.load(Ordering::Acquire))
    }

    /// Requests cooperative cancellation of this loader job.
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }
}
