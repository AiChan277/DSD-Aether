//! Frozen `#[repr(C)]` FFI types shared across the C++20 / Rust 2024 boundary (§5.2).

use core::ffi::c_char;

use crate::status::AetherStatus;

/// Configuration for `aether_runtime_init` (ADR-0003).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherRuntimeConfig {
    /// `sizeof(AetherRuntimeConfig)` for forward validation.
    pub struct_size: u32,
    /// Minimum log level (`0=trace..4=error`).
    pub log_level: u32,
    /// Dedicated loader rayon thread count (`0` = E-core heuristic).
    pub worker_threads: u32,
    /// Runtime flags (`0` = default).
    pub flags: u32,
    /// Optional UTF-8 application data directory.
    pub app_data_dir_utf8: *const c_char,
    /// Optional UTF-8 cache directory.
    pub cache_dir_utf8: *const c_char,
}

/// Asset load request passed to `aether_loader_open` (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherLoadRequest {
    /// Null-terminated UTF-8 filesystem path.
    pub path_utf8: *const c_char,
    /// Bitmask of `AETHER_LOAD_FLAG_*`.
    pub flags: u32,
    /// Maximum worker threads (`0` = default pool size).
    pub max_threads: u32,
}

/// Opaque asset loader handle target.
#[repr(C)]
pub struct AetherLoaderOpaque {
    pub _opaque: u8,
}

/// Borrowed handle to an active asset load job.
pub type AetherLoaderHandle = *mut AetherLoaderOpaque;

/// Queried byte sizes and counts for phase-1 C++ arena allocation (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherAssetRequirements {
    /// Exact required byte size per `AETHER_STREAM_*` slot.
    pub stream_bytes: [u64; 15],
    /// Vertex count across streams 0–2.
    pub vertex_count: u32,
    /// Total uint32 triangle index count.
    pub index_count: u32,
    /// Submesh count.
    pub submesh_count: u32,
    /// Material count.
    pub material_count: u32,
    /// Skeleton joint count.
    pub joint_count: u32,
    /// `SpringBone` joint count.
    pub spring_joint_count: u32,
    /// `SpringBone` collider count.
    pub collider_count: u32,
    /// Morph target count.
    pub morph_target_count: u32,
    /// Expression preset count.
    pub expression_count: u32,
    /// Texture count.
    pub texture_count: u32,
    /// Detected source format enum.
    pub source_format: u32,
    /// Non-fatal load warnings bitmask.
    pub warnings_mask: u32,
}

/// Zero-copy arena slice lent by C++ to Rust — 24 B (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherArenaSlice {
    /// C++-owned, host-visible pointer valid until `aether_loader_close()`.
    pub ptr: *mut u8,
    /// Byte capacity of the borrowed slice.
    pub size_bytes: u64,
    /// Stream kind (`AETHER_STREAM_*`).
    pub stream_kind: u32,
    /// Explicit padding.
    pub _pad: u32,
}

/// Loaded avatar summary produced by `aether_loader_fill` (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherAvatarDesc {
    /// Detected source format enum.
    pub source_format: u32,
    /// Mapping from Appendix A humanoid index (`0..54`) to skeleton joint (`0xFFFF` = unmapped).
    pub humanoid_to_joint: [u16; 55],
    /// Fuzzy Bone Matcher confidence `[0, 1]` per humanoid slot.
    pub bone_match_score: [f32; 55],
    /// Model AABB minimum `(x, y, z)` in metres.
    pub aabb_min: [f32; 3],
    /// Model AABB maximum `(x, y, z)` in metres.
    pub aabb_max: [f32; 3],
    /// Ellipsoid face proxy centre `C_face` in head-local metres.
    pub face_center_head_local: [f32; 3],
    /// Normalised avatar height in metres.
    pub avatar_height_m: f32,
    /// Mapped head joint index.
    pub head_joint: u32,
    /// Warnings bitmask (`W_UNIT_GUESS`, etc.).
    pub warnings_mask: u32,
    /// Borrowed C++ slice pointers echoed back for `REQ-FFI-010` pointer identity proof.
    pub debug_stream_ptrs: [u64; 15],
}

/// Configuration for `aether_host_create` (ADR-0003).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherHostConfig {
    /// `sizeof(AetherHostConfig)` for forward validation.
    pub struct_size: u32,
    /// Async plugin runtime worker threads (`2` default).
    pub worker_threads: u32,
    /// OSC control listen port (`9002` default).
    pub osc_in_port: u16,
    /// OSC telemetry destination port (`9001` default).
    pub osc_out_port: u16,
    /// WebSocket server port (`19190` default).
    pub ws_port: u16,
    /// Host feature bitflags (`0` = WebSocket disabled).
    pub flags: u16,
    /// UTF-8 installed plugins directory.
    pub plugins_dir_utf8: *const c_char,
}

/// Opaque plugin host handle target.
#[repr(C)]
pub struct AetherHostOpaque {
    pub _opaque: u8,
}

/// Borrowed handle to the Rust plugin host & graph engine.
pub type AetherHostHandle = *mut AetherHostOpaque;

/// Structural UI -> plugin host command (ADR-0003).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherHostCommand {
    /// `sizeof(AetherHostCommand)`.
    pub struct_size: u32,
    /// Command discriminator.
    pub kind: u32,
    /// Target node/plugin ID.
    pub target_id: u64,
    /// Optional float payload.
    pub param_f32: f32,
    /// Optional uint32 payload.
    pub param_u32: u32,
    /// Optional UTF-8 payload pointer (borrowed for the call).
    pub payload_utf8: *const c_char,
    /// Payload length in bytes.
    pub payload_len: u64,
}

/// Polled plugin host -> UI event (ADR-0003).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct AetherHostEvent {
    /// `sizeof(AetherHostEvent)`.
    pub struct_size: u32,
    /// Event discriminator.
    pub kind: u32,
    /// Source node/plugin ID.
    pub source_id: u64,
    /// Associated status code.
    pub status: AetherStatus,
    /// Optional float value.
    pub value_f32: f32,
    /// Fixed UTF-8 diagnostic buffer (null-terminated).
    pub message_utf8: [u8; 256],
}

/// Opaque compiled graph plan handle target.
#[repr(C)]
pub struct AetherPlanOpaque {
    pub _opaque: u8,
}

/// Borrowed handle to a compiled graph evaluation plan.
pub type AetherPlanHandle = *mut AetherPlanOpaque;

/// Per-frame tracking snapshot T1 (NPU) -> T2 (IK/physics) — 2048 B (§5.2).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct AetherTrackingFrame {
    /// Camera hardware timestamp, QPC domain (us).
    pub capture_timestamp_us: u64,
    /// NPU/GPU inference completion timestamp (us).
    pub inference_complete_us: u64,
    /// Active execution provider (`AETHER_EP_*`).
    pub active_execution_provider: u32,
    /// Upper-body region confidence `[0, 1]`.
    pub upper_body_confidence: f32,
    /// Left-hand region confidence `[0, 1]`.
    pub left_hand_confidence: f32,
    /// Right-hand region confidence `[0, 1]`.
    pub right_hand_confidence: f32,
    /// Lower-body region confidence `[0, 1]` (usually `< 0.2` when seated).
    pub lower_body_confidence: f32,
    /// 52 `ARKit` blendshapes in Appendix B order.
    pub arkit_blendshapes: [f32; 52],
    /// 55 joints: `xyz` world (m), `w` = per-joint confidence.
    pub joint_positions_ws: [[f32; 4]; 55],
    /// 55 joints: local rotation `(x, y, z, w)` in Appendix A order.
    pub joint_rotations_quat: [[f32; 4]; 55],
    /// Monotonic `+1` per published frame.
    pub frame_sequence: u32,
    /// Validity flags (`b0` face, `b1` `l_hand`, `b2` `r_hand`, `b3` body, `b4` `roi_reset`).
    pub valid_flags: u32,
    /// Explicit trailing reserved bytes to reach 2048 B.
    pub _reserved: [u8; 36],
}

/// Inputs passed into `aether_graph_evaluate` on T2 (§5.2).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct AetherGraphInputs {
    /// Borrowed pointer to current extrapolated tracking frame.
    pub tracking: *const AetherTrackingFrame,
    /// Current QPC timestamp (us).
    pub now_us: u64,
    /// Tick delta time (seconds).
    pub dt_s: f32,
    /// Monotonic simulation tick index.
    pub tick_index: u32,
    /// Latest DSP viseme weights `A, I, U, E, O`.
    pub dsp_visemes: [f32; 5],
    /// Latest DSP input level (dBFS).
    pub dsp_rms_db: f32,
    /// Held key bitset (128 bits).
    pub keys_down: [u64; 2],
    /// Newly pressed key bitset (128 bits).
    pub keys_pressed: [u64; 2],
    /// External parameter slots.
    pub external_params: [f32; 64],
}

/// Outputs produced by `aether_graph_evaluate` on T2 (§5.2).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct AetherGraphOutputs {
    /// Evaluated 64-slot expression weights.
    pub expression_weights: [f32; 64],
    /// Override weights (`0` = keep base, `1` = override).
    pub expression_override: [f32; 64],
    /// 8 IK targets: `pos xyz`, `quat xyzw`, `weight`.
    pub ik_targets: [[f32; 8]; 8],
    /// Per-joint rotation offsets `(x, y, z, w)` in Appendix A order.
    pub bone_rotation_offsets: [[f32; 4]; 55],
    /// World-space look-at target `(x, y, z, weight)`.
    pub look_at_ws: [f32; 4],
    /// Triggered event IDs for this tick.
    pub events: [u32; 16],
    /// Valid entry count in `events`.
    pub event_count: u32,
}

/// Stream 0: 2.5D NPR geometry vertex — 64 B, frozen by MTD (§5.2).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherVertex25D {
    /// 12 B local-space position (metres).
    pub position: [f32; 3],
    /// 4 B per-vertex outline thickness factor (from vertex colour / paint).
    pub outline_width_scale: f32,
    /// 12 B original geometric normal (specular, rim light).
    pub true_normal: [f32; 3],
    /// 4 B perspective compression weight; authored `[0, 1]`, clamped to `0.85` at bake.
    pub z_flatten_weight: f32,
    /// 12 B spherized / cylindrical proxy normal (smooth face shading).
    pub proxy_normal: [f32; 3],
    /// 4 B face region mask that uses the 2D SDF shadow texture (`> 0.5`).
    pub sdf_face_mask: f32,
    /// 16 B `xyz` = seam-welded averaged normal for inverted hull, `w = 1`.
    pub smooth_tangent: [f32; 4],
}

/// Stream 1: surface attributes — 32 B (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherVertexSurface {
    /// Base colour, SDF and ramp UVs.
    pub uv0: [f32; 2],
    /// Secondary UV (copy of `uv0` when absent).
    pub uv1: [f32; 2],
    /// Vertex colour, sRGB, R in the low byte.
    pub color_rgba8: u32,
    /// Octahedral-encoded tangent, `2 x snorm16`.
    pub tangent_oct: u32,
    /// Bit 0: bitangent sign negative; bits 8..15: material slot.
    pub flags: u32,
    /// Explicit alignment padding.
    pub _reserved: u32,
}

/// Stream 2: skinning — 32 B, max 8 influences (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherVertexSkin {
    /// Joint indices, sorted by weight descending.
    pub joints: [u16; 8],
    /// Normalized weights; sum == `65535` exactly, unused = `0`.
    pub weights_unorm16: [u16; 8],
}

/// Sparse morph-target delta — 32 B (§5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherMorphDelta {
    /// Target vertex index within the stream.
    pub vertex_index: u32,
    /// Position delta (metres).
    pub d_position: [f32; 3],
    /// Normal delta.
    pub d_normal: [f32; 3],
    /// Blend toward recomputed proxy normal for strong face morphs.
    pub d_proxy_weight: f32,
}

/// Shared memory with external Audio DSP: `Local\AetherDspSyncSharedMem` — 128 B (§5.2).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherDspSyncPacket {
    /// Cache line 0 (written ONLY by Aether): `'AETH'` (`0x41455448`).
    pub magic_header: u32,
    /// Seqlock: odd while writing, `+2` per published frame.
    pub sequence_number: u32,
    /// Actual camera capture -> Spout2/VCam present latency (ms).
    pub total_video_latency_ms: f32,
    /// Delay-line value the DSP should apply (ms).
    pub recommended_dsp_delay_ms: f32,
    /// QPC us of the present this packet describes.
    pub video_present_qpc_us: u64,
    /// Protocol version (`1`).
    pub protocol_version: u32,
    /// Video rate in milli-FPS (`60000` = 60.000 fps).
    pub video_fps_milli: u32,
    /// Latency jitter `p95 - p50` over last 120 frames (ms).
    pub latency_jitter_ms: f32,
    /// Video output flags (`b0` spout, `b1` vcam, `b2` ndi, `b3` `tracking_lost`).
    pub video_flags: u32,
    /// Cache line 0 reserved padding.
    pub _reserved0: [u8; 24],
    /// Cache line 1 (written ONLY by DSP): seqlock counter.
    pub dsp_sequence_number: u32,
    /// Current audio input level (dBFS).
    pub dsp_input_rms_db: f32,
    /// Viseme weights `A, I, U, E, O` in `[0, 1]`.
    pub dsp_viseme_weights: [f32; 5],
    /// DSP's own processing latency (ms).
    pub dsp_reported_latency_ms: f32,
    /// QPC us at DSP write.
    pub dsp_write_qpc_us: u64,
    /// DSP flags (`b0` `voice_active`, `b1` `delay_applied`, `b2` `visemes_valid`).
    pub dsp_flags: u32,
    /// Cache line 1 reserved padding.
    pub _reserved1: [u8; 20],
}

/// Morph target range descriptor — 32 B (`AETHER_STREAM_MORPH_RANGES`, §5.2, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherMorphRangeDesc {
    /// Starting index inside `AETHER_STREAM_MORPH_DELTAS`.
    pub delta_offset: u32,
    /// Number of sparse deltas belonging to this morph target.
    pub delta_count: u32,
    /// Byte offset of UTF-8 morph name in `AETHER_STREAM_STRINGS`.
    pub name_offset: u32,
    /// Byte length of UTF-8 morph name.
    pub name_len: u32,
    /// Average X coordinate of displaced vertices (for wink left/right disambiguation per §6.5).
    pub centroid_x: f32,
    /// Non-zero if morph belongs to face/eyes/mouth region (`AETHER_SUBMESH_FLAG_FACE` / head).
    pub is_face_morph: u32,
    /// Reserved alignment padding.
    pub _reserved: [u32; 2],
}

/// Sorted draw submesh descriptor — 64 B (`AETHER_STREAM_SUBMESHES`, §5.2, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherSubmeshDesc {
    /// Starting index in `AETHER_STREAM_INDEX_U32`.
    pub index_offset: u32,
    /// Triangle index count (multiple of 3).
    pub index_count: u32,
    /// Starting vertex index in vertex streams.
    pub vertex_offset: u32,
    /// Number of vertices referenced by this submesh.
    pub vertex_count: u32,
    /// Material index in `AETHER_STREAM_MATERIALS`.
    pub material_index: u32,
    /// Submesh classification and pass flags (`AETHER_SUBMESH_FLAG_*`).
    pub flags: u32,
    /// Byte offset of UTF-8 submesh name in `AETHER_STREAM_STRINGS`.
    pub name_offset: u32,
    /// Byte length of UTF-8 submesh name.
    pub name_len: u32,
    /// VRM / `MToon` render queue offset relative to pass baseline.
    pub render_queue_offset: i32,
    /// Reserved alignment padding.
    pub _pad0: u32,
    /// Local-space AABB minimum `(x, y, z)`.
    pub aabb_min: [f32; 3],
    /// Local-space AABB maximum `(x, y, z)`.
    pub aabb_max: [f32; 3],
}

/// Unified GPU 2.5D NPR material scalar block — 192 B exact (`AETHER_STREAM_MATERIALS`, §6.8, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherNprMaterial {
    /// Linear base color RGBA (`baseColorFactor`).
    pub base_color: [f32; 4],
    /// Linear shade color RGB + `w = gi_equalization`.
    pub shade_color_gi: [f32; 4],
    /// Linear parametric rim color RGB + `w = rim_lighting_mix`.
    pub rim_color_mix: [f32; 4],
    /// Linear outline color RGB + `w = outline_lighting_mix`.
    pub outline_color_mix: [f32; 4],
    /// Linear emissive HDR RGB + `w = matcap_strength`.
    pub emissive_and_pad: [f32; 4],
    /// Linear matcap tint RGB + `w = matcap_add_mix` (`0` = additive, `1` = multiply/roughness).
    pub matcap_color: [f32; 4],
    /// `(shading_shift, shading_toony, rim_fresnel_power, rim_lift)`.
    pub toon_params: [f32; 4],
    /// `(uv_scroll_x, uv_scroll_y, uv_rot_rad, alpha_cutoff)`.
    pub uv_outline_alpha: [f32; 4],
    /// World/screen outline width in metres or pixels depending on `AETHER_MAT_FLAG_OUTLINE_SCREEN`.
    pub outline_width: f32,
    /// Alpha blend mode (`AETHER_ALPHA_MODE_*`).
    pub alpha_mode: u32,
    /// Render queue tie-breaker offset.
    pub render_queue_offset: i32,
    /// Material feature flags (`AETHER_MAT_FLAG_*`).
    pub flags: u32,
    /// Base color texture index in `AETHER_STREAM_TEXTURE_DESCS` (`AETHER_INVALID_INDEX` if none).
    pub tex_base: u32,
    /// Shade color multiply texture index.
    pub tex_shade: u32,
    /// Shading shift texture index.
    pub tex_shading_shift: u32,
    /// Tangent-space normal map index.
    pub tex_normal: u32,
    /// Emissive texture index.
    pub tex_emissive: u32,
    /// Rim mask texture index.
    pub tex_rim: u32,
    /// Matcap sphere texture index.
    pub tex_matcap: u32,
    /// Outline width mask texture index.
    pub tex_outline_width: u32,
    /// UV animation mask texture index.
    pub tex_uv_mask: u32,
    /// Custom 1D cel ramp texture index.
    pub tex_ramp: u32,
    /// Face SDF shadow map texture index (`AETHER_MAT_FLAG_SDF_FACE`).
    pub tex_sdf_face: u32,
    /// Byte offset into `AETHER_STREAM_STRINGS` (`len` determined up to NUL or material index).
    pub name_offset: u32,
}

/// Normalised right-handed Y-up metres skeleton joint descriptor — 128 B (`AETHER_STREAM_JOINTS`, §5.2, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherJointDesc {
    /// Parent joint index (`-1` for root joint).
    pub parent_index: i32,
    /// Mapped Appendix A humanoid index (`0..54`) or `0xFFFF` if non-humanoid (e.g. hair/skirt).
    pub humanoid_bone: u16,
    /// Body-part depth-flatten region tag (`AETHER_JOINT_REGION_*`).
    pub region_tag: u16,
    /// Byte offset of UTF-8 joint name in `AETHER_STREAM_STRINGS`.
    pub name_offset: u32,
    /// Byte length of UTF-8 joint name.
    pub name_len: u32,
    /// Local bind-pose translation in metres relative to parent.
    pub bind_translation: [f32; 3],
    /// Distance to primary child or synthetic tip in metres (`>= 0.05` fallback).
    pub tail_length: f32,
    /// Local bind-pose rotation quaternion `[x, y, z, w]`.
    pub bind_rotation_quat: [f32; 4],
    /// Local bind-pose scale `[sx, sy, sz]` (normalised toward `1.0` where non-uniformity removed).
    pub bind_scale: [f32; 3],
    /// Alignment padding.
    pub _pad0: f32,
    /// Rest-pose to canonical T-pose delta quaternion `[x, y, z, w]` computed per §6.3.
    pub rest_to_tpose_quat: [f32; 4],
    /// Row-major `3x4` inverse bind matrix mapping model space -> joint local space.
    pub inverse_bind_matrix_row_major: [[f32; 4]; 3],
}

/// Normalised `SpringBone` / migrated MMD chain joint — 80 B (`AETHER_STREAM_SPRINGS`, §6.6, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherSpringJointDesc {
    /// 64-bit mask selecting colliders in `AETHER_STREAM_COLLIDERS` by `group_bit`.
    pub collider_group_mask: u64,
    /// Gravity direction in world space (typically `[0.0, -1.0, 0.0]`).
    pub gravity_dir: [f32; 3],
    /// Collision sphere radius in metres.
    pub hit_radius: f32,
    /// Local rest bone direction (normalized unit vector toward child/tip).
    pub rest_dir: [f32; 3],
    /// Bone segment length in metres.
    pub bone_length: f32,
    /// Skeleton joint index in `AETHER_STREAM_JOINTS`.
    pub joint_index: u32,
    /// Parent spring index in `AETHER_STREAM_SPRINGS` (`-1` if chain root).
    pub parent_spring_index: i32,
    /// Contiguous chain identifier (`0..num_chains`).
    pub chain_id: u32,
    /// Depth within the chain (`0` = root).
    pub depth_level: u32,
    /// Reference center joint index (`-1` if model-space).
    pub center_joint_index: i32,
    /// Stiffness scalar (normalised).
    pub stiffness: f32,
    /// Velocity drag factor in `[0.0, 1.0]`.
    pub drag_force: f32,
    /// Gravity strength multiplier.
    pub gravity_power: f32,
    /// Spring flags (e.g. bit 0 = migrated from MMD).
    pub flags: u32,
    /// Explicit struct alignment tail.
    pub _pad: u32,
}

/// `SpringBone` collider descriptor — 48 B (`AETHER_STREAM_COLLIDERS`, §6.6, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherColliderDesc {
    /// Joint index this collider moves with (`AETHER_INVALID_INDEX` = root-attached).
    pub joint_index: u32,
    /// Collider group bit index (`0..63`).
    pub group_bit: u32,
    /// Shape discriminator (`AETHER_COLLIDER_SHAPE_*`).
    pub shape_kind: u32,
    /// Sphere or capsule radius in metres.
    pub radius: f32,
    /// Local-space center/start offset in metres.
    pub offset: [f32; 3],
    /// Explicit padding.
    pub _pad0: f32,
    /// Local-space capsule tail offset in metres (unused for sphere).
    pub tail: [f32; 3],
    /// Explicit padding.
    pub _pad1: f32,
}

/// Sparse expression retarget matrix `W` entry — 24 B (`AETHER_STREAM_EXPRESSIONS`, §6.5, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherExpressionBindingDesc {
    /// Logical expression preset index (ordinal in model).
    pub preset_index: u32,
    /// Appendix B unified expression slot (`0..63`).
    pub input_slot: u32,
    /// Target morph range index in `AETHER_STREAM_MORPH_RANGES`.
    pub morph_index: u32,
    /// Sparse matrix weight `W[input_slot, morph_index]`.
    pub weight: f32,
    /// Byte offset of UTF-8 preset name in `AETHER_STREAM_STRINGS`.
    pub name_offset: u32,
    /// Byte length of UTF-8 preset name.
    pub name_len: u32,
}

/// Decoded texture descriptor pointing into `AETHER_STREAM_TEXTURE_PIXELS` — 48 B (`AETHER_STREAM_TEXTURE_DESCS`, §6.9, ADR-0004).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, bytemuck::Pod, bytemuck::Zeroable)]
pub struct AetherTextureDesc {
    /// 16-byte aligned byte offset in `AETHER_STREAM_TEXTURE_PIXELS`.
    pub pixel_offset: u64,
    /// Payload size in bytes.
    pub pixel_size: u64,
    /// Width in texels (`<= 16384`).
    pub width: u32,
    /// Height in texels (`<= 16384`).
    pub height: u32,
    /// Pixel format (`AETHER_TEX_FORMAT_*`).
    pub format: u32,
    /// Usage classification flags (`AETHER_TEX_USAGE_*`).
    pub usage_flags: u32,
    /// Byte offset of UTF-8 texture name in `AETHER_STREAM_STRINGS`.
    pub name_offset: u32,
    /// Byte length of UTF-8 texture name.
    pub name_len: u32,
    /// 64-bit content hash for BC7 texture disk cache (`§6.9`).
    pub content_hash_lo: u64,
}

/// Read-only live statistics and node inspection snapshot for Z4 Node Graph UI (ADR-0014).
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug)]
pub struct AetherGraphPlanStats {
    /// `sizeof(AetherGraphPlanStats)` for forward validation.
    pub struct_size: u32,
    /// Number of active nodes in the compiled topological plan.
    pub active_node_count: u32,
    /// First failing node ID on compile error (`AETHER_INVALID_INDEX` when valid).
    pub error_node_id: u32,
    /// Monotonic successful compile count.
    pub compile_count: u32,
    /// Most recent graph compile duration (milliseconds).
    pub last_compile_ms: f32,
    /// Most recent active graph evaluation duration (microseconds).
    pub last_eval_us: f32,
    /// Sliding-window p95 active graph evaluation duration (microseconds).
    pub p95_eval_us: f32,
    /// Non-zero if graph evaluation exceeded the `100 us` hot-path budget (`REQ-PLG-002`).
    pub budget_warn: u32,
    /// Non-zero if the localhost WebSocket server (`127.0.0.1:19190`) is enabled.
    pub ws_enabled: u32,
    /// Total valid OSC packets accepted since startup.
    pub osc_packets_rx: u32,
    /// Total graph evaluations executed on `T2`.
    pub eval_count: u64,
    /// Total OSC packets dropped due to `1000 msg/s` rate limit.
    pub osc_rate_limited: u32,
    /// Total registered plugins (loaded or pending consent).
    pub plugin_count: u32,
    /// Per-node last execution time (microseconds) indexed by active slot (`0..active_node_count`).
    pub node_eval_us: [f32; 256],
    /// Per-node primary live scalar value for pin/badge inspection (`0..active_node_count`).
    pub node_live_values: [f32; 256],
}

/// 64-byte aligned live per-node timing and per-output-pin value snapshot for Studio Z4 inspection (`§7.2`).
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct AetherGraphTelemetryReadout {
    /// Expected `sizeof(AetherGraphTelemetryReadout)` filled by caller.
    pub struct_size: u32,
    /// Valid slot count in `node_ids` and `node_eval_ns` (`<= 256`).
    pub active_node_count: u32,
    /// Valid slot count in `pin_*` arrays (`<= 384`).
    pub pin_readout_count: u32,
    /// Non-zero once at least one graph evaluation tick has populated this snapshot.
    pub valid: u32,
    /// Monotonic evaluation tick counter.
    pub eval_sequence: u64,
    /// Total evaluation duration of the latest graph tick (nanoseconds).
    pub total_eval_ns: u64,
    /// Graph node IDs corresponding slot-for-slot to `node_eval_ns`.
    pub node_ids: [u64; 256],
    /// Per-node execution duration in nanoseconds.
    pub node_eval_ns: [u32; 256],
    /// Owning node ID for each output pin readout slot.
    pub pin_node_ids: [u64; 384],
    /// Four-lane summary payload (`[scalar, ...]`, `[x, y, z, w]`, `[rms_db, voiced, f1, f2]`, etc.).
    pub pin_values: [[f32; 4]; 384],
    /// Zero-based output pin index on `pin_node_ids[i]`.
    pub pin_indices: [u8; 384],
    /// Value classification tag (`AETHER_PIN_VALUE_*`).
    pub pin_kinds: [u8; 384],
    /// Explicit padding to maintain a 64-byte multiple (`13120` bytes total).
    pub _reserved_tail: [u64; 4],
}
