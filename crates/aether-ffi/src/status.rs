//! Status codes and ABI constants for the AETHER-2.5D FFI boundary.

/// FFI status code (`0` = success, `< 0` = typed error).
pub type AetherStatus = i32;

/// Operation succeeded.
pub const AETHER_OK: AetherStatus = 0;
/// Null, misaligned, or out-of-range argument.
pub const AETHER_E_INVALID_ARG: AetherStatus = -1;
/// File or operating-system I/O error.
pub const AETHER_E_IO: AetherStatus = -2;
/// Malformed external input (offset reported in last error).
pub const AETHER_E_PARSE: AetherStatus = -3;
/// Feature not implemented or not supported on this host (`HR-09`).
pub const AETHER_E_UNSUPPORTED: AetherStatus = -4;
/// Caller-provided slice is smaller than the queried requirement.
pub const AETHER_E_BUFFER_TOO_SMALL: AetherStatus = -5;
/// Hard resource limit (§3.6) exceeded.
pub const AETHER_E_LIMIT_EXCEEDED: AetherStatus = -6;
/// Rust panic caught at the FFI boundary (`HR-04`).
pub const AETHER_E_PANIC: AetherStatus = -7;
/// Asynchronous load cancelled via `aether_loader_cancel`.
pub const AETHER_E_CANCELLED: AetherStatus = -8;
/// WASM plugin trapped, exhausted fuel, or crashed.
pub const AETHER_E_PLUGIN_TRAP: AetherStatus = -9;
/// Execution budget or timeout exceeded.
pub const AETHER_E_TIMEOUT: AetherStatus = -10;
/// FFI ABI version mismatch.
pub const AETHER_E_VERSION_MISMATCH: AetherStatus = -11;
/// Internal invariant violated.
pub const AETHER_E_INTERNAL: AetherStatus = -12;

/// ABI major version (`AETHER_FFI_VERSION_MAJOR`). Major mismatch is fatal at startup.
pub const AETHER_FFI_VERSION_MAJOR: u32 = 1;
/// ABI minor version (`AETHER_FFI_VERSION_MINOR`).
pub const AETHER_FFI_VERSION_MINOR: u32 = 0;

/// Intel AI Boost NPU via `OpenVINO` Execution Provider (Tier 1).
pub const AETHER_EP_INTEL_NPU: u32 = 0;
/// NVIDIA `TensorRT` Execution Provider (Tier 2B).
pub const AETHER_EP_NVIDIA_TRT: u32 = 1;
/// `DirectML` GPU Execution Provider (Tier 2A/2B).
pub const AETHER_EP_DIRECTML_GPU: u32 = 2;
/// CPU fallback Execution Provider (`OpenVINO` CPU / `XNNPACK`, Tier 3).
pub const AETHER_EP_CPU: u32 = 3;
/// Intel `OpenVINO` GPU Execution Provider (Tier 2A integrated GPU).
pub const AETHER_EP_OPENVINO_GPU: u32 = 4;

/// Magic header for `Local\AetherDspSyncSharedMem`: `'AETH'` (`0x41455448`).
pub const AETHER_DSP_MAGIC: u32 = 0x4145_5448;

/// Stream kind: `AetherVertex25D` (64 B per vertex).
pub const AETHER_STREAM_VERTEX25D: u32 = 0;
/// Stream kind: `AetherVertexSurface` (32 B per vertex).
pub const AETHER_STREAM_SURFACE: u32 = 1;
/// Stream kind: `AetherVertexSkin` (32 B per vertex).
pub const AETHER_STREAM_SKIN: u32 = 2;
/// Stream kind: `uint32_t` triangle indices.
pub const AETHER_STREAM_INDEX_U32: u32 = 3;
/// Stream kind: `AetherMorphDelta` (32 B sparse deltas).
pub const AETHER_STREAM_MORPH_DELTAS: u32 = 4;
/// Stream kind: morph target range descriptors.
pub const AETHER_STREAM_MORPH_RANGES: u32 = 5;
/// Stream kind: submesh descriptors.
pub const AETHER_STREAM_SUBMESHES: u32 = 6;
/// Stream kind: material descriptors.
pub const AETHER_STREAM_MATERIALS: u32 = 7;
/// Stream kind: normalised skeleton joints.
pub const AETHER_STREAM_JOINTS: u32 = 8;
/// Stream kind: `SpringBone` chains/joints.
pub const AETHER_STREAM_SPRINGS: u32 = 9;
/// Stream kind: `SpringBone` colliders.
pub const AETHER_STREAM_COLLIDERS: u32 = 10;
/// Stream kind: expression bindings / retarget matrix.
pub const AETHER_STREAM_EXPRESSIONS: u32 = 11;
/// Stream kind: decoded texture descriptors.
pub const AETHER_STREAM_TEXTURE_DESCS: u32 = 12;
/// Stream kind: decoded texture pixel staging buffer.
pub const AETHER_STREAM_TEXTURE_PIXELS: u32 = 13;
/// Stream kind: UTF-8 string table.
pub const AETHER_STREAM_STRINGS: u32 = 14;
/// Total number of zero-copy asset arena stream slots.
pub const AETHER_STREAM_COUNT: u32 = 15;

/// Loader request flag: bake spherized face proxy normals.
pub const AETHER_LOAD_FLAG_BAKE_PROXY_NORMALS: u32 = 1 << 0;
/// Loader request flag: bake seam-welded outline smooth tangents.
pub const AETHER_LOAD_FLAG_BAKE_SMOOTH_TANGENTS: u32 = 1 << 1;
/// Loader request flag: sort triangles by 30-bit Morton code.
pub const AETHER_LOAD_FLAG_MORTON_SORT: u32 = 1 << 2;
/// Loader request flag: decode embedded/referenced textures.
pub const AETHER_LOAD_FLAG_DECODE_TEXTURES: u32 = 1 << 3;
/// Loader request flag: read/write BC7 texture cache.
pub const AETHER_LOAD_FLAG_TEXTURE_CACHE: u32 = 1 << 4;

/// Thread affinity tag (`REQ-FFI-003`): callable from any thread.
pub const AETHER_THREAD_ROLE_ANY: u32 = 0;
/// Thread affinity tag (`REQ-FFI-003`): T0 main thread.
pub const AETHER_THREAD_ROLE_MAIN: u32 = 1;
/// Thread affinity tag (`REQ-FFI-003`): async asset loader worker thread.
pub const AETHER_THREAD_ROLE_LOADER: u32 = 2;
/// Thread affinity tag (`REQ-FFI-003`): plugin runtime thread.
pub const AETHER_THREAD_ROLE_PLUGIN: u32 = 3;
/// Thread affinity tag (`REQ-FFI-003`): T2 simulation & IK thread.
pub const AETHER_THREAD_ROLE_T2: u32 = 4;
/// Thread affinity tag (`REQ-FFI-003`): T3 UI thread.
pub const AETHER_THREAD_ROLE_UI: u32 = 5;

/// Total Appendix A humanoid skeleton joints (`0..54`).
pub const AETHER_HUMANOID_BONE_COUNT: u32 = 55;
/// Total Appendix B expression input slots (`0..63`).
pub const AETHER_EXPRESSION_SLOT_COUNT: u32 = 64;
/// Sentinel invalid uint32 index for optional textures/slots.
pub const AETHER_INVALID_INDEX: u32 = 0xFFFF_FFFF;

/// Avatar source format: unknown or unparsed.
pub const AETHER_SOURCE_FORMAT_UNKNOWN: u32 = 0;
/// Avatar source format: VRM 0.x (`VRM` extension in GLB).
pub const AETHER_SOURCE_FORMAT_VRM0: u32 = 1;
/// Avatar source format: VRM 1.0 (`VRMC_vrm` extension in GLB/glTF).
pub const AETHER_SOURCE_FORMAT_VRM1: u32 = 2;
/// Avatar source format: plain glTF 2.0 / GLB.
pub const AETHER_SOURCE_FORMAT_GLTF: u32 = 3;
/// Avatar source format: Autodesk FBX (binary or ASCII).
pub const AETHER_SOURCE_FORMAT_FBX: u32 = 4;
/// Avatar source format: `MikuMikuDance` PMX 2.0/2.1.
pub const AETHER_SOURCE_FORMAT_PMX: u32 = 5;
/// Avatar source format: `MikuMikuDance` PMD 1.0 (migrated to PMX representation).
pub const AETHER_SOURCE_FORMAT_PMD: u32 = 6;

/// Loader warning: unit scale had to be inferred heuristically (§6.3).
pub const AETHER_WARN_UNIT_GUESS: u32 = 1 << 0;
/// Loader warning: one or more humanoid bones fell in `[0.55, 0.80)` review band (§6.4).
pub const AETHER_WARN_REVIEW_BONES: u32 = 1 << 1;
/// Loader warning: required humanoid bone could not be matched cleanly (`< 0.55`).
pub const AETHER_WARN_UNMAPPED_REQUIRED: u32 = 1 << 2;
/// Loader warning: MMD Bullet constraint linked across distinct chains (§6.6).
pub const AETHER_WARN_MMD_CROSS_CHAIN: u32 = 1 << 3;
/// Loader warning: glTF `KHR_node_constraint` approximated.
pub const AETHER_WARN_NODE_CONSTRAINT_APPROX: u32 = 1 << 4;
/// Loader warning: referenced texture missing/unreadable; 1x1 neutral fallback substituted.
pub const AETHER_WARN_MISSING_TEXTURES: u32 = 1 << 5;
/// Loader warning: BC7 cache miss forced RGBA8 upload or fast encode.
pub const AETHER_WARN_TEXTURE_CACHE_MISS: u32 = 1 << 6;

/// Submesh flag: identified as face/head surface.
pub const AETHER_SUBMESH_FLAG_FACE: u32 = 1 << 0;
/// Submesh flag: front hair / bangs (writes stencil `0x02`).
pub const AETHER_SUBMESH_FLAG_HAIR_FRONT: u32 = 1 << 1;
/// Submesh flag: back hair.
pub const AETHER_SUBMESH_FLAG_HAIR_BACK: u32 = 1 << 2;
/// Submesh flag: eyes, eyelashes, or eyebrows (receives alpha cutout / cutout punch).
pub const AETHER_SUBMESH_FLAG_EYE_OR_BROW: u32 = 1 << 3;
/// Submesh flag: opaque pass.
pub const AETHER_SUBMESH_FLAG_OPAQUE: u32 = 1 << 4;
/// Submesh flag: alpha cutout pass.
pub const AETHER_SUBMESH_FLAG_ALPHA_CUTOUT: u32 = 1 << 5;
/// Submesh flag: transparent forward pass.
pub const AETHER_SUBMESH_FLAG_TRANSPARENT: u32 = 1 << 6;

/// Material alpha mode: opaque.
pub const AETHER_ALPHA_MODE_OPAQUE: u32 = 0;
/// Material alpha mode: alpha test / cutout.
pub const AETHER_ALPHA_MODE_MASK: u32 = 1;
/// Material alpha mode: alpha blend.
pub const AETHER_ALPHA_MODE_BLEND: u32 = 2;

/// Material flag: double-sided rendering (`cullMode = NONE`).
pub const AETHER_MAT_FLAG_DOUBLE_SIDED: u32 = 1 << 0;
/// Material flag: uses dedicated face SDF shadow texture.
pub const AETHER_MAT_FLAG_SDF_FACE: u32 = 1 << 1;
/// Material flag: writes stencil `0x02` (`HAIR_FRONT`).
pub const AETHER_MAT_FLAG_HAIR_FRONT_STENCIL: u32 = 1 << 2;
/// Material flag: receives hair cast shadow on skin/face (`stencil == 0x02`).
pub const AETHER_MAT_FLAG_RECEIVES_HAIR_SHADOW: u32 = 1 << 3;
/// Material flag: screen-space outline mode (vs world-space).
pub const AETHER_MAT_FLAG_OUTLINE_SCREEN: u32 = 1 << 4;
/// Material flag: unlit (`KHR_materials_unlit` / constant shadeless).
pub const AETHER_MAT_FLAG_UNLIT: u32 = 1 << 5;
/// Material flag: migrated from VRM `MToon` (`0.x` or `1.0`).
pub const AETHER_MAT_FLAG_HAS_EXT_VRM: u32 = 1 << 6;

/// Joint region tag for §6.7 2.5D depth-flattening weights: generic/other.
pub const AETHER_JOINT_REGION_OTHER: u16 = 0;
/// Joint region tag: head/eyes/jaw (`0.85`).
pub const AETHER_JOINT_REGION_HEAD: u16 = 1;
/// Joint region tag: neck/upper chest (`0.70`).
pub const AETHER_JOINT_REGION_NECK_UPPER: u16 = 2;
/// Joint region tag: chest/shoulders/arms (`0.45`).
pub const AETHER_JOINT_REGION_TORSO_ARMS: u16 = 3;
/// Joint region tag: hips/legs (`0.20`).
pub const AETHER_JOINT_REGION_HIPS_LEGS: u16 = 4;
/// Joint region tag: hair chain (inherits attachment bone weight).
pub const AETHER_JOINT_REGION_HAIR: u16 = 5;

/// Spring collider shape: sphere.
pub const AETHER_COLLIDER_SHAPE_SPHERE: u32 = 0;
/// Spring collider shape: capsule (`offset` to `tail`).
pub const AETHER_COLLIDER_SHAPE_CAPSULE: u32 = 1;

/// Decoded texture format: linear `R8G8B8A8_UNORM`.
pub const AETHER_TEX_FORMAT_RGBA8_UNORM: u32 = 0;
/// Decoded texture format: sRGB `R8G8B8A8_SRGB`.
pub const AETHER_TEX_FORMAT_RGBA8_SRGB: u32 = 1;
/// Compressed texture format: linear `BC7_UNORM_BLOCK`.
pub const AETHER_TEX_FORMAT_BC7_UNORM: u32 = 2;
/// Compressed texture format: sRGB `BC7_SRGB_BLOCK`.
pub const AETHER_TEX_FORMAT_BC7_SRGB: u32 = 3;

/// Texture semantic usage: albedo / shade / emissive color.
pub const AETHER_TEX_USAGE_COLOR: u32 = 1 << 0;
/// Texture semantic usage: tangent-space normal map.
pub const AETHER_TEX_USAGE_NORMAL: u32 = 1 << 1;
/// Texture semantic usage: scalar mask (rim, matcap mask, outline width, uv anim, ramp).
pub const AETHER_TEX_USAGE_MASK: u32 = 1 << 2;
/// Texture semantic usage: face SDF shadow threshold map.
pub const AETHER_TEX_USAGE_SDF_FACE: u32 = 1 << 3;
/// Texture semantic usage: matcap sphere texture.
pub const AETHER_TEX_USAGE_MATCAP: u32 = 1 << 4;

/// Host config flag: enable localhost WebSocket JSON-RPC server (`ws://127.0.0.1:19190/v1`, opt-in §7.8).
pub const AETHER_HOST_FLAG_WS_ENABLED: u16 = 1 << 0;
/// Host config flag: bind OSC listener on `0.0.0.0` (only when explicitly enabled by user).
pub const AETHER_HOST_FLAG_OSC_BIND_ANY: u16 = 1 << 1;

/// Host command kind: compile and live-swap node graph JSON (`payload_utf8`).
pub const AETHER_HOST_CMD_SET_GRAPH_JSON: u32 = 1;
/// Host command kind: trigger or clear an expression override (`target_id` = slot, `param_f32` = weight).
pub const AETHER_HOST_CMD_TRIGGER_EXPRESSION: u32 = 2;
/// Host command kind: set external graph parameter (`target_id` = slot `0..63`, `param_f32` = value).
pub const AETHER_HOST_CMD_SET_EXTERNAL_PARAM: u32 = 3;
/// Host command kind: record user capability consent for plugin (`target_id` = plugin hash, `param_u32` = allow 1/0, `payload_utf8` = plugin id).
pub const AETHER_HOST_CMD_GRANT_PLUGIN_CONSENT: u32 = 4;
/// Host command kind: rescan plugins directory (`RELOAD_PLUGINS`).
pub const AETHER_HOST_CMD_RELOAD_PLUGINS: u32 = 5;
/// Host command kind: queue OSC telemetry packet or frame summary (`payload_utf8` = address, `param_f32` = value).
pub const AETHER_HOST_CMD_PUBLISH_TELEMETRY: u32 = 6;
/// Host command kind: push normalized project state JSON from C++ UI (`payload_utf8`).
pub const AETHER_HOST_CMD_PROJECT_UPDATE_JSON: u32 = 7;
/// Host command kind: toggle WebSocket JSON-RPC server (`param_u32` = enable 1/0).
pub const AETHER_HOST_CMD_SET_WS_ENABLED: u32 = 8;
/// Host command kind: set ON AIR lock state (`param_u32` = `on_air` 1/0).
pub const AETHER_HOST_CMD_SET_ON_AIR: u32 = 9;
/// Host command kind: disable/unload plugin (`target_id` = plugin hash, `payload_utf8` = plugin id).
pub const AETHER_HOST_CMD_UNLOAD_PLUGIN: u32 = 10;

/// Host event kind: debounced node graph compile succeeded and swapped cleanly.
pub const AETHER_HOST_EVT_GRAPH_COMPILED: u32 = 1;
/// Host event kind: node graph compile failed (`source_id` = offending node id, `message_utf8` = diagnostic).
pub const AETHER_HOST_EVT_GRAPH_COMPILE_ERR: u32 = 2;
/// Host event kind: `*.aether` project bundle finished loading (`aether_host_copy_project_json` ready).
pub const AETHER_HOST_EVT_PROJECT_LOADED: u32 = 3;
/// Host event kind: `*.aether` project bundle saved atomically (`message_utf8` = path).
pub const AETHER_HOST_EVT_PROJECT_SAVED: u32 = 4;
/// Host event kind: plugin discovered/loaded (`source_id` = plugin hash, `message_utf8` = metadata).
pub const AETHER_HOST_EVT_PLUGIN_LOADED: u32 = 5;
/// Host event kind: plugin requires user capability consent before running (`source_id` = plugin hash, `message_utf8` = summary).
pub const AETHER_HOST_EVT_PLUGIN_CONSENT_REQ: u32 = 6;
/// Host event kind: plugin trapped, exceeded timeout/memory, and was isolated (`source_id` = plugin hash).
pub const AETHER_HOST_EVT_PLUGIN_TRAP: u32 = 7;
/// Host event kind: inbound OSC parameter update (`source_id` = hash/slot, `value_f32` = value, `message_utf8` = address).
pub const AETHER_HOST_EVT_OSC_PARAM: u32 = 8;
/// Host event kind: external WebSocket JSON-RPC control command received (`source_id` = slot, `value_f32` = value, `message_utf8` = summary).
pub const AETHER_HOST_EVT_WS_COMMAND: u32 = 9;
/// Host event kind: hotkey or event node fired (`source_id` = event id).
pub const AETHER_HOST_EVT_HOTKEY_EVENT: u32 = 10;
/// Host event kind: compiled graph evaluation exceeded `100 us` warning threshold (`value_f32` = eval us).
pub const AETHER_HOST_EVT_GRAPH_BUDGET_WARN: u32 = 11;

/// Maximum per-node live stats slots exposed by `AetherGraphPlanStats`.
pub const AETHER_GRAPH_MAX_DEBUG_NODES: usize = 256;
/// Maximum per-output-pin live readout slots exposed by `AetherGraphTelemetryReadout`.
pub const AETHER_GRAPH_MAX_DEBUG_PINS: usize = 384;

/// Compact live pin summary value kind: empty / unavailable.
pub const AETHER_PIN_VALUE_INVALID: u8 = 0;
/// Compact live pin summary value kind: scalar float (`[val, 0, 0, 0]`).
pub const AETHER_PIN_VALUE_FLOAT: u8 = 1;
/// Compact live pin summary value kind: boolean (`[0.0 | 1.0, 0, 0, 0]`).
pub const AETHER_PIN_VALUE_BOOL: u8 = 2;
/// Compact live pin summary value kind: vec3 (`[x, y, z, 0]`).
pub const AETHER_PIN_VALUE_VEC3: u8 = 3;
/// Compact live pin summary value kind: quat / color (`[x, y, z, w]`).
pub const AETHER_PIN_VALUE_QUAT: u8 = 4;
/// Compact live pin summary value kind: transform (`[px, py, pz, scale]`).
pub const AETHER_PIN_VALUE_TRANSFORM: u8 = 5;
/// Compact live pin summary value kind: pose stream (`[head_x, head_y, head_z, root_conf]`).
pub const AETHER_PIN_VALUE_POSE: u8 = 6;
/// Compact live pin summary value kind: expression set (`[max_w, active_count, jaw_open, blink_l]`).
pub const AETHER_PIN_VALUE_EXPR: u8 = 7;
/// Compact live pin summary value kind: 5-viseme set (`[max_w, dominant_idx, sum, a_w]`).
pub const AETHER_PIN_VALUE_VISEME: u8 = 8;
/// Compact live pin summary value kind: DSP audio frame (`[rms_db, voiced, f1_hz, f2_hz]`).
pub const AETHER_PIN_VALUE_AUDIO: u8 = 9;
/// Compact live pin summary value kind: trigger event counter (`[fired, 0, 0, 0]`).
pub const AETHER_PIN_VALUE_EVENT: u8 = 10;
