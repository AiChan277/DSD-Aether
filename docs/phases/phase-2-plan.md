# Phase 2 Plan — Rust Unified Asset Loader & C++ Zero-Copy Upload

- **Spec reference**: `AETHER-TS-001 v1.0` (§3.6, §5.2, §5.6, §6.1–§6.9, §13 Phase 2, §14.2)
- **Phase goal**: Implement the production multi-format Rust asset loader (`VRM 0.x`, `VRM 1.0`, `glTF/GLB`, `FBX` binary/ASCII, `PMX 2.0/2.1`, `PMD`) with coordinate/unit/rest-pose normalisation, Hungarian Fuzzy Bone Matcher (`>= 95%`), sparse expression retarget matrix `W`, SpringBone + MMD rigid-body conversion, Rayon data-parallel geometry bake (`smooth_tangent`, `proxy_normal`, 30-bit Morton sorting, `z_flatten_weight`, `sdf_face_mask`, `outline_width_scale`), MToon/PBR/PMX -> `AetherNprMaterial` (192 B), parallel image decoding + optional BC7 texture cache, hard parser resource caps, 4 fuzz targets (`>= 10 min` clean), C++ `AssetJob` two-phase VMA ReBAR/staging GPU upload (`<= 50 ms`), and Dev Shell basic-shaded 3D mesh preview with bone overlay.

---

## 1. Task Breakdown

| ID | Task | Spec Refs | Exit Criteria Covered |
|---|---|---|---|
| P2-01 | FFI stream descriptors (`AetherMorphRangeDesc`, `AetherSubmeshDesc`, `AetherNprMaterial` 192 B, `AetherJointDesc`, `AetherSpringJointDesc`, `AetherColliderDesc`, `AetherExpressionBindingDesc`, `AetherTextureDesc`) + layout asserts + `layout_table.json` + `cbindgen` regen + `Cargo.toml` dep pins (`HR-10`, ADR-0004) | §5.2, §6.8, HR-03, HR-10 | `ffi_header_check` and `ffi_layout_test` pass; `cargo deny` clean |
| P2-02 | Parser front-ends & resource limits (`limits.rs`, `vrm_gltf.rs`, `meshopt.rs`, `fbx.rs`, `pmx.rs`, `textures.rs`, `bc7_cache.rs`) | §3.6, §6.2, §6.8, §6.9, HR-06 | Loads VRM 0.x/1.0, GLB/glTF (+`EXT_meshopt_compression`, rejects Draco with `AETHER_E_UNSUPPORTED`), binary+ASCII FBX via `ufbx`, PMX/PMD; enforces <=1GiB file, <=4M verts, <=16384^2 tex, <=1024 bones/morphs (`AETHER_E_LIMIT_EXCEEDED`); parallel `zune-png`/`zune-jpeg` + BMP/TGA/DDS + BC7 Mode 6 cache |
| P2-03 | Normalisation, Fuzzy Bone Matcher & Expression/Physics retargeting (`bone_matcher.rs`, `retarget.rs`, `materials.rs`) | §6.3–§6.6, §6.8, REQ-AST-010 | >= 95% auto-mapped across VRM/Mixamo/Unreal/Rigify/CC/MMD conventions; A-pose `rest_to_tpose`; `W_UNIT_GUESS` scale fix; 64-slot expression `W` matrix + Japanese wink centroid disambiguation; SpringBone + MMD Bullet -> chains/capsules; MToon migration + OKLab PBR shade |
| P2-04 | Rayon parallel geometry baker (`bake.rs`) on dedicated E-core pool | §3.6, §6.7, REQ-AST-020..023 | Seam-welded `smooth_tangent` (exact match on UV/normal seam duplicates, degenerate fallback); spherized face `proxy_normal` (60% weight+name, `C_face`, 65% Y-flatten, `smoothstep(0.4,0.8)`); 30-bit Morton triangle sort; `z_flatten_weight` clamped <= 0.85; `sdf_face_mask` + 1-ring border; `outline_width_scale` |
| P2-05 | Zero-copy two-phase `AssetLoader` wiring & FFI export activation (`crates/aether-asset-loader/src/lib.rs`, `crates/aether-rust-host/src/lib.rs`) | §5.6, §6.1, HR-05, REQ-FFI-010 | Production `aether_loader_open/query/fill/progress/cancel/close` operational (zero-copy into C++ slices; no persistent Rust alloc >= 1 MiB; `debug_stream_ptrs[k] == lent_ptr[k]`; `PERF-007 <= 150 ms` on 70k-tri Reference Avatar) |
| P2-06 | Test corpus generator (`tools/gen_test_corpus.py`, `tests/data/LICENSES.md`) + comprehensive Rust unit/property (`proptest`) & fuzz targets (`fuzz/`, `examples/fuzz_runner.rs`) | §6.9, §13, §14.1, §14.2, HR-06 | 20-model corpus + 70k-tri Reference Avatar pass cleanly; seam, proxy-normal, Morton, limit, Draco-reject, zero-copy & cancellation tests pass; all 4 fuzz targets run `>= 10 min` with 0 crashes |
| P2-07 | C++ two-phase `AssetJob` & GPU uploader (`src/renderer/asset_upload/*`) + Dev Shell shaded mesh preview + bone overlay (`src/ui/dev_shell.*`, `src/app/application.*`) | §5.6, §6.1, §13 Phase 2, HR-08 | Async C++ load worker (does not block T3); VMA ReBAR direct-write or staging+transfer DMA upload (`<= 50 ms`); Vulkan validation + sync validation 0 errors/0 hazards; Viewport renders basic shaded mesh + bone overlay |
| P2-08 | Phase 2 C++ Catch2 verification suite (`tests/cpp/asset_loader_test.cpp`, `asset_upload_vk_test.cpp`), full CI (`tools/ci.ps1`), `docs/STATUS.md`, `docs/phases/phase-2-report.md` | §0.4, §13 Phase 2 | All presets & `tools/ci.ps1` green; `PERF-007 <= 150 ms`, >=95% corpus mapping, 4x 10-min fuzz clean; Phase 2 report written |

---

## 2. Files to Create / Modify

### Modified Docs & Contracts
- `docs/adr/0004-phase2-stream-descriptors-and-loader-deps.md`
- `docs/adr/0005-windows-fuzz-harness-and-test-corpus.md`
- `docs/dependencies_allowlist.toml`
- `docs/STATUS.md`
- `Cargo.toml`, `Cargo.lock`
- `crates/aether-ffi/src/{status.rs, types.rs, layout_asserts.rs}`, `crates/aether-ffi/build.rs`, `include/aether_ffi_contract.h`

### Rust Asset Loader & FFI Host
- `crates/aether-asset-loader/Cargo.toml`
- `crates/aether-asset-loader/src/{lib.rs, ffi_slice.rs, limits.rs, vrm_gltf.rs, meshopt.rs, fbx.rs, pmx.rs, bone_matcher.rs, retarget.rs, bake.rs, materials.rs, textures.rs, bc7_cache.rs, pool.rs, fuzz_entry.rs}`
- `crates/aether-asset-loader/tests/loader_tests.rs`
- `crates/aether-asset-loader/examples/fuzz_runner.rs`
- `crates/aether-rust-host/src/lib.rs`, `crates/aether-rust-host/tests/ffi_tests.rs`
- `fuzz/{Cargo.toml, fuzz_targets/{fuzz_glb.rs, fuzz_vrm_json.rs, fuzz_fbx.rs, fuzz_pmx.rs}}`

### C++ Upload, Basic Viewport Mesh Pass & Dev Shell
- `src/renderer/CMakeLists.txt`
- `src/renderer/asset_upload/{asset_job.hpp, asset_job.cpp, gpu_avatar.hpp, gpu_avatar.cpp}`
- `src/renderer/preview_pass.{hpp, cpp}` + `shaders/preview_mesh.{vert, frag}` (compiled via `tools/shader_build.py` or `glslc`/`slangc` — wait: check shader compiler in Step 2!)
- `src/ui/dev_shell.{hpp, cpp}`, `src/app/application.cpp`, `src/CMakeLists.txt`

### Tools & Tests
- `tools/gen_test_corpus.py`, `tools/check_gates.py`, `tools/ci.ps1`
- `tests/data/LICENSES.md`
- `tests/CMakeLists.txt`, `tests/cpp/{ffi_layout_test.cpp, asset_loader_test.cpp, asset_upload_vk_test.cpp}`

---

## 3. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `slangc` / `glslc` not yet installed on this host for C++ Dev Shell preview shaders | Check `VULKAN_SDK` / `slangc` presence first; download standalone official `slang` binary or embed pre-compiled verified SPIR-V / C++ Slang step if compiler missing, or check exact SDK availability before P2-07 |
| `ufbx` C compilation CRT flags vs Debug `/MDd` ASan | Check `ufbx` build across all 4 CMake presets (`windows-debug`, `windows-relwithdebinfo`, `windows-release`, `ci-validation`) early after P2-01 |
| `PERF-007 <= 150 ms` on 70k tris + 10x 2048^2 textures | Use Rayon parallel `zune-png` decoding directly into borrowed C++ `TEXTURE_PIXELS` slice (zero intermediate whole-image copies), pre-sized `ahash::HashMap` with capacity = `vertex_count` for seam hashing, and release-mode optimization (`opt-level = 2` for dev deps, `3` for `windows-relwithdebinfo`/`release`) |
| Windows missing `cargo-fuzz` / `libFuzzer` runtime | Ship both standard `fuzz/fuzz_targets/` definitions and native `fuzz_runner` executing `>= 600s` per target cleanly (`ADR-0005`) |

---

## 4. Tests to Write First (Exit Gate Verification)

1. `ffi_layout_test.cpp` + `crates/aether-ffi` compile-time checks for all stream descriptors (`5..12`) + `AetherNprMaterial == 192 B`.
2. `crates/aether-asset-loader/tests/loader_tests.rs`:
   - Synthetic & generated **Reference Avatar (`PERF-007`)** benchmark timing (`<= 150 ms` warm cache).
   - **20-model corpus** load test: zero crashes, `>= 95%` humanoid auto-mapped per rig (`VRM0`, `VRM1`, `Mixamo`, `UE`, `Rigify`, `PMX`, `PMD`).
   - **Seam test**: cube/cylinder with split UVs/normals proves identical `smooth_tangent` across split coincident vertices.
   - **Proxy-normal `proptest`**: unit length `|n - 1| < 1e-4`, 65% Y-flatten invariant, smooth continuity `smoothstep(0.4, 0.8)` across head weight boundary.
   - **Morton ordering test**: strictly non-decreasing 30-bit Morton codes across each submesh's triangles.
   - **Resource limits + Draco rejection**: `> 1 GiB` / `> 4 M verts` / `> 16384^2 tex` / `> 1024 bones` return `AETHER_E_LIMIT_EXCEEDED`; Draco GLB returns `AETHER_E_UNSUPPORTED`.
3. `tests/cpp/asset_loader_test.cpp` & `asset_upload_vk_test.cpp`:
   - Two-phase zero-copy C++ `AssetJob` pointer identity (`debug_stream_ptrs[k] == lent_ptr[k]`) + ReBAR/staging DMA upload to Vulkan GPU buffers with `vk_errors == 0 && sync_hazards == 0`.
4. 4 fuzz targets (`fuzz_glb`, `fuzz_vrm_json`, `fuzz_fbx`, `fuzz_pmx`) >= 10 minutes each with zero crashes.
