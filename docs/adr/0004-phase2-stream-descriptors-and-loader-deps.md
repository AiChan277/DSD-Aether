# ADR-0004: Phase 2 Stream Descriptors, Loader Dependencies, and Texture Cache

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-03`, `HR-05`, `HR-06`, `HR-10`), §3.2, §3.6, §5.2, §6.1–§6.9

## Context

Phase 1 froze the core FFI types (`AetherVertex25D`, `AetherVertexSurface`, `AetherVertexSkin`, `AetherMorphDelta`, `AetherTrackingFrame`, `AetherDspSyncPacket`, `AetherArenaSlice`, `AetherAssetRequirements`, `AetherAvatarDesc`) and enumerated stream slots `AETHER_STREAM_VERTEX25D (0)` through `AETHER_STREAM_STRINGS (14)`. Streams `5..12` require concrete `#[repr(C)]` layouts exported in `include/aether_ffi_contract.h` and compiled with cross-language layout verification (`HR-03`) without modifying any frozen Phase 1 struct or breaking ABI major/minor `1.0`.

Phase 2 also requires adding the Rust parsing, math, threading, image-decoding, and property-testing crates specified in §3.2 and §6 (`HR-10`).

## Decisions

### 1. Additive Stream Payload Structs (`crates/aether-ffi`, ABI `1.0` compatible)

Every stream record type for streams `5..12` is `#[repr(C, align(8))]` (matching `ffi_slice::required_alignment == 8` for streams `5..12`) and `bytemuck::Pod + bytemuck::Zeroable`:

- **Stream 5 (`AETHER_STREAM_MORPH_RANGES`)**: `AetherMorphRangeDesc` (32 B, align 8) — sparse morph slice `[delta_offset, delta_offset + delta_count)`, UTF-8 name span in `STRINGS`, vertex X centroid (for load-time wink side verification per §6.5), and `is_face_morph`.
- **Stream 6 (`AETHER_STREAM_SUBMESHES`)**: `AetherSubmeshDesc` (64 B, align 8) — sorted triangle index span, vertex span, material index, classification flags (`FACE`, `HAIR_FRONT`, `HAIR_BACK`, `EYE_OR_BROW`, alpha flags), name span, `render_queue_offset`, and submesh AABB (`aabb_min`, `aabb_max`).
- **Stream 7 (`AETHER_STREAM_MATERIALS`)**: `AetherNprMaterial` (192 B exact per §6.8 scalar block spec, align 8) — packed linear `base_color`, `shade_color_gi`, `rim_color_mix`, `outline_color_mix`, `emissive_and_pad`, `matcap_color`, `toon_params`, `uv_outline_alpha`, scalar `outline_width`, `alpha_mode`, `render_queue_offset`, `flags` (`DOUBLE_SIDED`, `SDF_FACE`, `HAIR_FRONT` stencil `0x02`, `RECEIVES_HAIR_SHADOW`, `OUTLINE_SCREEN`), 11 material texture slot indices (`tex_base`, `tex_shade`, `tex_shading_shift`, `tex_normal`, `tex_emissive`, `tex_rim`, `tex_matcap`, `tex_outline_width`, `tex_uv_mask`, `tex_ramp`, `tex_sdf_face`), and `name_offset`.
- **Stream 8 (`AETHER_STREAM_JOINTS`)**: `AetherJointDesc` (128 B, align 8) — hierarchy `parent_index`, Appendix A `humanoid_bone` (`0..54` or `0xFFFF`), `region_tag`, name span, bind TRS, `rest_to_tpose_quat` (`§6.3`), and GPU `3x4` row-major inverse bind matrix.
- **Stream 9 (`AETHER_STREAM_SPRINGS`)**: `AetherSpringJointDesc` (80 B, align 8) — sorted depth-first per chain so Phase 3 §9.6 SoA builders need no runtime sort; stores `collider_group_mask` (`u64`), `gravity_dir`, `hit_radius`, `rest_dir`, `bone_length`, `joint_index`, `parent_spring_index`, `chain_id`, `depth_level`, `center_joint_index`, `stiffness`, `drag_force`, `gravity_power`, and `flags`.
- **Stream 10 (`AETHER_STREAM_COLLIDERS`)**: `AetherColliderDesc` (48 B, align 8) — sphere or capsule collider attached to `joint_index` with `group_bit` (`0..63`), `radius`, local `offset`, and `tail`.
- **Stream 11 (`AETHER_STREAM_EXPRESSIONS`)**: `AetherExpressionBindingDesc` (24 B, align 8) — sparse entry of the retarget matrix `W` (`input_slot` in `0..63`, `morph_index`, `weight`), preset index, and UTF-8 preset name span.
- **Stream 12 (`AETHER_STREAM_TEXTURE_DESCS`)**: `AetherTextureDesc` (48 B, align 8) — 16-byte-aligned slice `[pixel_offset, pixel_size)` in `TEXTURE_PIXELS`, `width`, `height`, `format` (`RGBA8_UNORM`, `RGBA8_SRGB`, `BC7_UNORM`, `BC7_SRGB`), `usage_flags`, UTF-8 name span, and 64-bit content hash for cache lookup.

All existing Phase 1 structs remain unchanged; `AETHER_FFI_VERSION` stays `1.0`.

### 2. Phase 2 Rust Crate Additions (`HR-10`)

Every crate below is pinned in `Cargo.lock`, uses an approved commercial-friendly license verified by `cargo deny`, and is restricted to `crates/aether-asset-loader` (never imported into C++ directly or `aether-ffi`):

| Crate | Exact Pin | Licence | Maintenance & Security Surface | Role |
|---|---|---|---|---|
| `gltf` | `1.4.1` | `MIT OR Apache-2.0` | Widely deployed pure-Rust glTF 2.0 parser (`default-features = false`); bounds-checked chunks; avoids HTTP/base64 bloat | VRM 0.x, VRM 1.0, glTF/GLB parsing |
| `ufbx` | `0.11.5` | `MIT OR Unlicense` | Official Rust bindings to actively maintained single-file `ufbx` C library; compiled cleanly in staticlib | Binary & ASCII FBX 6.x/7.x parsing (§3.2, §6.2) |
| `rayon` | `1.12.0` | `MIT OR Apache-2.0` | Standard data-parallelism pool; configured once on `workers` E-cores (`BELOW_NORMAL` priority per §3.6) | Parallel bake & texture decode |
| `glam` | `0.34.3` | `MIT OR Apache-2.0` | Pure-Rust SIMD math, column vectors (`§6.3`) | Transforms, normal baking, slerp, Kabsch/rest-pose |
| `serde` | `1.0.229` | `MIT OR Apache-2.0` | Zero-copy/borrowed serde derive | VRM extensions, shader/model manifests |
| `serde_json` | `1.0.154` | `MIT OR Apache-2.0` | Safe JSON parser | VRM 0.x / 1.0 / `EXT_meshopt_compression` JSON |
| `zune-png` | `0.5.2` | `MIT OR Apache-2.0 OR Zlib` | High-speed pure-Rust SIMD PNG decoder | Parallel PNG decoding (`§6.9`) |
| `zune-jpeg` | `0.5.15` | `MIT OR Apache-2.0 OR Zlib` | High-speed pure-Rust SIMD JPEG decoder | Parallel JPEG decoding (`§6.9`) |
| `memmap2` | `0.9.12` | `MIT OR Apache-2.0` | Cross-platform read-only mmap (`open` bounds checks before reading) | Two-phase open (`§5.6`) |
| `unicode-normalization` | `0.1.26` | `(MIT OR Apache-2.0) AND Unicode-3.0` | Official Unicode TR15 NFKC normalization | Fuzzy Bone Name Matcher (§6.4) |
| `encoding_rs` | `0.8.44` | `(Apache-2.0 OR MIT) AND BSD-3-Clause` | WHATWG Shift-JIS decoder (`PMD` Japanese names) | PMD -> PMX migration (`§6.2`) |
| `ahash` | `0.8.12` | `MIT OR Apache-2.0` | Fast non-cryptographic hasher (`RandomState::with_seeds` for deterministic content/weld hashing) | Seam hash grid & texture content cache keys |
| `proptest` (dev-only) | `1.12.0` | `MIT OR Apache-2.0` | Property-based test generator (`dev-dependencies` only; stripped from shipping binary) | Bake & normal property tests (`§13`, `§14.1`) |

In-house pure-Rust modules inside `crates/aether-asset-loader` (avoiding extra third-party crates):
- **PMX 2.0/2.1 & PMD 1.0 parser** (`src/pmx.rs`), enforcing resource caps before vector reservations (`HR-06`, §3.6).
- **BMP / uncompressed+RLE TGA / DDS & pure-Rust `EXT_meshopt_compression` decoder** (`src/textures.rs`, `src/meshopt.rs`).
- **BC7 Mode 6 fast parallel compressor & content-hash disk cache** (`%LOCALAPPDATA%\Aether\cache\textures\*.bc7`, keyed by FNV/aHash content digest + dimension + sRGB flag) so subsequent loads hit BC7 without adding heavyweight C++/wgpu dependencies (`§6.9`).

## Consequences

- `crates/aether-asset-loader` keeps `#![deny(unsafe_code)]` globally with `ffi_slice.rs` as the sole audited `unsafe` boundary module, plus safe Win32 thread-priority call or Rayon `start_handler` helper isolated if needed. Wait: to preserve `#![deny(unsafe_code)]` cleanly in `aether-asset-loader`, worker-thread priority setup (`BELOW_NORMAL` / E-cores) runs either via `aether-rust-host` or via safe Rust / C++ `Application` runtime init helper. Keeping zero `unsafe` in `aether-asset-loader` outside `ffi_slice.rs` (only Win32 thread-priority if placed in `aether-rust-host`) preserves §3.4 literally!
- All existing and new crates remain 100% offline-compilable once `Cargo.lock` and Cargo registry cache are populated and pass `cargo deny check licenses bans sources`.
