//! Format-agnostic intermediate representation shared by VRM/glTF, FBX, and PMX parsers.

use std::path::PathBuf;

/// Detected coordinate/scale metadata carried by a source asset.
#[derive(Clone, Copy, Debug)]
pub struct SourceUnitMetadata {
    /// Known metres-per-source-unit multiplier if encoded in format metadata (`None` = guess if out of range).
    pub explicit_unit_scale: Option<f32>,
    /// Linear scale factor applied to input geometry/bones to obtain metres.
    pub resolved_unit_scale: f32,
    /// True if source needed heuristic scale guessing (`W_UNIT_GUESS`).
    pub scale_guessed: bool,
    /// True if source uses left-handed Z+ forward (PMX/PMD).
    pub left_handed_flipped: bool,
}

/// Raw vertex parsed from any source format before baking.
#[derive(Clone, Copy, Debug)]
pub struct RawVertex {
    /// Model-space position (pre-normalisation or normalised in-place).
    pub position: [f32; 3],
    /// Geometric vertex normal.
    pub normal: [f32; 3],
    /// Primary UV coordinates.
    pub uv0: [f32; 2],
    /// Secondary UV coordinates.
    pub uv1: [f32; 2],
    /// Vertex tint RGBA8 linear/unorm.
    pub color: [u8; 4],
    /// Up to 8 bone indices.
    pub joints: [u16; 8],
    /// Up to 8 float skin weights (normalized before packing).
    pub weights: [f32; 8],
    /// Vertex outline multiplier (`edge_scale` in PMX, `1.0` default).
    pub edge_scale: f32,
    /// Optional explicit vertex `z_flatten_weight` override (`< 0.0` = compute from skeleton).
    pub flatten_override: f32,
}

/// One raw submesh primitive prior to Morton triangle sorting.
#[derive(Clone, Debug)]
pub struct RawSubmesh {
    /// Debug name (mesh/material/node name).
    pub name: String,
    /// Triangle indices referencing `ParsedAvatar.vertices` (`len % 3 == 0`).
    pub indices: Vec<u32>,
    /// Material slot index.
    pub material_index: u32,
    /// Explicit classification hints from material/name/extension.
    pub is_eye_or_brow_hint: bool,
}

/// Normalized material inputs extracted from MToon 0.x/1.0, PBR, FBX Phong/PBR, or PMX.
#[derive(Clone, Debug)]
pub struct RawMaterial {
    /// Material name.
    pub name: String,
    /// Linear RGBA base color.
    pub base_color: [f32; 4],
    /// Explicit shade color if authored (e.g. MToon), otherwise synthesized via OKLab (§6.8).
    pub explicit_shade_color: Option<[f32; 3]>,
    /// Specular/rim/outline parameters.
    pub rim_color: [f32; 3],
    pub rim_lighting_mix: f32,
    pub rim_fresnel_power: f32,
    pub rim_lift: f32,
    pub outline_color: [f32; 3],
    pub outline_lighting_mix: f32,
    pub outline_width: f32,
    pub outline_screen_space: bool,
    pub emissive: [f32; 3],
    pub matcap_color: [f32; 3],
    pub matcap_add_mix: f32,
    pub matcap_strength: f32,
    pub gi_equalization: f32,
    pub shading_shift: f32,
    pub shading_toony: f32,
    pub uv_scroll: [f32; 2],
    pub uv_rotation: f32,
    pub alpha_mode: u32,
    pub alpha_cutoff: f32,
    pub double_sided: bool,
    pub unlit: bool,
    pub has_vrm_mtoon: bool,
    pub render_queue_offset: i32,
    /// Optional material-level `z_flatten_weight` override (`None` = derive from skin).
    pub flatten_override: Option<f32>,
    /// Texture slot indices into `ParsedAvatar.textures` (`None` = absent).
    pub tex_base: Option<u32>,
    pub tex_shade: Option<u32>,
    pub tex_shading_shift: Option<u32>,
    pub tex_normal: Option<u32>,
    pub tex_emissive: Option<u32>,
    pub tex_rim: Option<u32>,
    pub tex_matcap: Option<u32>,
    pub tex_outline_width: Option<u32>,
    pub tex_uv_mask: Option<u32>,
    pub tex_ramp: Option<u32>,
    pub tex_sdf_face: Option<u32>,
}

/// Raw skeleton joint extracted from source format.
#[derive(Clone, Debug)]
pub struct RawJoint {
    /// UTF-8 bone name (Shift-JIS decoded if PMD, NFKC normalized later).
    pub name: String,
    /// Optional English/secondary name (PMX/PMD `name_en`).
    pub alt_name: String,
    /// Parent joint index (`None` for root).
    pub parent: Option<usize>,
    /// Local bind translation in metres (post axis/unit transform).
    pub local_translation: [f32; 3],
    /// Local bind rotation quaternion `[x, y, z, w]`.
    pub local_rotation: [f32; 4],
    /// Local bind scale `[sx, sy, sz]`.
    pub local_scale: [f32; 3],
    /// Global inverse bind matrix in row-major `3x4` (`Option::None` -> computed from FK!).
    pub inverse_bind_matrix: Option<[f32; 16]>,
    /// Explicit Appendix A humanoid index from VRM `humanoid` table (`Some(0..54)`).
    pub explicit_humanoid: Option<u16>,
    /// Ordinal appearance index for Hungarian tie-breaking.
    pub ordinal: usize,
}

/// Raw sparse morph target collected prior to zero-copy packing.
#[derive(Clone, Debug)]
pub struct RawMorphDeltaItem {
    /// Index into `ParsedAvatar.vertices`.
    pub vertex_index: u32,
    /// Position delta in metres.
    pub d_pos: [f32; 3],
    /// Normal delta.
    pub d_norm: [f32; 3],
}

/// Named morph target (blend shape).
#[derive(Clone, Debug)]
pub struct RawMorphTarget {
    /// Morph target name.
    pub name: String,
    /// Sparse non-zero deltas.
    pub deltas: Vec<RawMorphDeltaItem>,
    /// Average X of displaced vertices for left/right wink disambiguation (§6.5).
    pub centroid_x: f32,
}

/// Raw expression binding authored in VRM/FBX/PMX before Appendix B remapping.
#[derive(Clone, Debug)]
pub struct RawExprPreset {
    /// Author-specified preset or morph name (`blinkLeft`, `aa`, `ウィンク`, etc.).
    pub name: String,
    /// Optional explicit morph indices + weights (for VRM `morphTargetBinds`); empty for direct name->morph binding.
    pub morph_binds: Vec<(usize, f32)>,
}

/// Raw spring joint definition.
#[derive(Clone, Debug)]
pub struct RawSpringChain {
    /// Skeleton joint indices belonging to this chain (root -> tip order).
    pub joints: Vec<RawSpringNode>,
    /// Center reference joint (`None` = model space).
    pub center_joint: Option<usize>,
    /// Indices into collider groups (or raw collider masks).
    pub collider_group_mask: u64,
}

/// Single node inside a `RawSpringChain`.
#[derive(Clone, Debug)]
pub struct RawSpringNode {
    pub joint_index: usize,
    pub hit_radius: f32,
    pub stiffness: f32,
    pub drag_force: f32,
    pub gravity_power: f32,
    pub gravity_dir: [f32; 3],
    pub from_mmd: bool,
}

/// Raw collider attached to a joint.
#[derive(Clone, Debug)]
pub struct RawCollider {
    pub joint_index: usize,
    pub group_bit: u32,
    pub shape_kind: u32,
    pub radius: f32,
    pub offset: [f32; 3],
    pub tail: [f32; 3],
}

/// Raw texture reference: either byte range inside source mmap, owned/decompressed bytes, or relative file path.
#[derive(Clone, Debug)]
pub enum RawTextureSource {
    /// `[start, end)` range inside the mapped source file (`VRM/GLB`).
    MappedSlice { offset: usize, len: usize },
    /// Companion image file next to PMX/PMD/FBX/glTF.
    ExternalFile(PathBuf),
    /// Embedded inline bytes (e.g. data URI / FBX binary Video content).
    EmbeddedBytes(Vec<u8>),
    /// Missing/unreadable sentinel -> emits 1x1 white/flat fallback + `W_MISSING_TEXTURES`.
    Missing,
}

/// Texture metadata discovered during `open()` before bulk pixel decode.
#[derive(Clone, Debug)]
pub struct RawTextureDesc {
    pub name: String,
    pub source: RawTextureSource,
    pub width: u32,
    pub height: u32,
    pub srgb: bool,
    pub usage_flags: u32,
    pub content_hash_lo: u64,
    /// Byte size when decoded to RGBA8.
    pub rgba_byte_len: u64,
    /// Pre-planned cache hit BC7 file path & byte size when `TEXTURE_CACHE` is active and warm.
    pub cached_bc7_path: Option<PathBuf>,
    pub cached_bc7_byte_len: u64,
    pub pixel_offset: u64,
    pub pixel_size: u64,
    pub final_format: u32,
}

/// Complete parsed avatar collected from format parser & normalized before `fill()`.
#[derive(Clone, Debug)]
pub struct ParsedAvatar {
    pub source_format: u32,
    pub base_dir: PathBuf,
    pub unit_meta: SourceUnitMetadata,
    pub warnings_mask: u32,
    pub vertices: Vec<RawVertex>,
    pub submeshes: Vec<RawSubmesh>,
    pub materials: Vec<RawMaterial>,
    pub joints: Vec<RawJoint>,
    pub morphs: Vec<RawMorphTarget>,
    pub expr_presets: Vec<RawExprPreset>,
    pub spring_chains: Vec<RawSpringChain>,
    pub colliders: Vec<RawCollider>,
    pub textures: Vec<RawTextureDesc>,
}
