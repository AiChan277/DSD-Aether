# Phase 2 Exit Report — Rust Unified Asset Loader & GPU Preview

**Status:** Complete (`tools/ci.ps1` exit gate passed across all 4 presets, 40 CPU-minutes of fuzzing clean)
**Specification:** `AETHER-TS-001 v1.0` (§5, §6, §13 Phase 2, `HR-01`–`HR-11`)

---

## 1. Summary of Built Systems

1. **FFI Contract Extensions (`ADR-0004`, `HR-03`, `HR-05`)**:
   - Exported Phase 2 arena stream descriptors (`AetherMorphRangeDesc`, `AetherSubmeshDesc`, `AetherNprMaterial`, `AetherJointDesc`, `AetherSpringJointDesc`, `AetherColliderDesc`, `AetherExpressionBindingDesc`, `AetherTextureDesc`) and `AETHER_MAT_FLAG_*` / stream constants `0..12` in [`include/aether_ffi_contract.h`](../../include/aether_ffi_contract.h) verified by `cbindgen --check` (`tools/gen_ffi.ps1 -CheckOnly`) and C++/Rust size/alignment/offset asserts.
   - Implemented two-phase zero-copy C++ arena borrow (`aether_loader_open` -> `aether_loader_query_requirements` -> `aether_loader_fill_arena` -> `aether_loader_close`) inside [`crates/aether-asset-loader`](../../crates/aether-asset-loader) and [`crates/aether-rust-host`](../../crates/aether-rust-host), with `catch_unwind` across all FFI boundaries (`HR-04`) and cooperative atomic cancellation (`aether_loader_cancel`).

2. **Unified Multi-Format Parsers (`HR-06`, §6.2–§6.8)**:
   - **glTF 2.0 / GLB / VRM 0.x / VRM 1.0** (`src/vrm_gltf.rs`): pure-Rust parsing + schema/index/bounds validator (`validate_gltf_root`) hardened against invalid `Checked<T>` enums, OOB indices, stride/dimension mismatches, and oversized resource tables; parses `VRM`, `VRMC_vrm`, `VRMC_materials_mtoon`, `VRMC_springBone`, pure-Rust `EXT_meshopt_compression` (`src/meshopt.rs`), and deterministically rejects `KHR_draco_mesh_compression` with `LoaderError::Unsupported`.
   - **Autodesk FBX 6.x/7.x** (`src/fbx.rs`): `ufbx` binary/ASCII parsing with coordinate/unit normalization, material & texture path resolution, skinning, and blendshapes.
   - **MikuMikuDance PMX 2.0/2.1 & Shift-JIS PMD 1.0** (`src/pmx.rs`): Japanese Shift-JIS / UTF-16LE text decoding via `encoding_rs`, scale conversion (`0.08 m/unit` -> meters + RH conversion), rigid-body / constraint -> SpringBone chains, and vertex/bone/tex resource caps.

3. **Rig Retargeting, Surface Baking & BC7 Texture Pipeline (§6.3–§6.7)**:
   - 55-bone Appendix A Hungarian/bipartite matcher (`src/bone_matcher.rs`) + Japanese/English facial expression retargeting with eye-centroid X-sign disambiguation (`src/retarget.rs`).
   - Seam-welded smooth tangents (`tangent.w` sign preservation), face submesh sphere-proxy normal blending (`face_sdf_mask` attenuation), 8-bone `unorm16` skin packing summing strictly to `65535`, and 30-bit Morton triangle sorting (`src/bake.rs`).
   - Parallel PNG/JPEG/BMP/TGA/DDS image decoding (`zune-png`, `zune-jpeg`) + persistent BC7 Mode 6 disk cache (`%LOCALAPPDATA%\Aether\cache\textures\`, overrideable via `AETHER_TEXTURE_CACHE_DIR`) (`src/textures.rs`, `src/bc7_cache.rs`).

4. **Async C++ `AssetJob`, VMA GPU Upload & Dev Shell 3D Preview (`src/renderer/`, `src/rhi/`, `src/ui/`, `src/app/`)**:
   - Mutex-free `AssetJob` (`src/renderer/asset_upload/asset_job.cpp`) running on `AETHER_THREAD_ROLE_LOADER` with cooperative `cancel()`.
   - 256-byte-aligned single-batch `vkQueueSubmit2` + Synchronization2 VMA upload (`src/renderer/asset_upload/gpu_avatar.cpp`).
   - Shader-Slang v2026.22 standalone compiler (`ADR-0006`, `tools/shader_build.py`) building MToon preview & bone overlay SPIR-V (`shaders/preview_{mesh,bones}.{vert,frag}.hlsl`).
   - Swapchain split (`acquire_and_begin_commands` / `begin_swapchain_pass`) + 2-slot offscreen `PreviewPass` (`src/renderer/preview_pass.cpp`) embedded into Precision Dark `DevShell` (`src/ui/dev_shell.cpp`) with orbit/pan/zoom, bone overlay, wireframe toggle, and `--avatar` CLI flag (`src/app/main.cpp`).

---

## 2. Measured Budgets & Exit-Gate Verification

| Check / Metric | Budget / Target | Measured Result | Conditions |
|---|---:|---:|---|
| **Static HR / UI Token / Allowlist Gates (`tools/check_gates.py`)** | `0` violations | Passed (`43` C++ files, Rust crates, UI tokens, allowlist) | `tools/ci.ps1` |
| **FFI Header Parity (`tools/gen_ffi.ps1 -CheckOnly`)** | Exact match | Passed | `tools/ci.ps1` |
| **Rust `cargo fmt` / `clippy` (default & `--all-features`) / `cargo deny`** | `0` warnings / `0` denies | Passed | `tools/ci.ps1` |
| **Rust Unit & Loader Exit-Gate Tests (`loader_tests` + `ffi_tests`)** | 100% pass | `8/8` `loader_tests` + `3/3` `ffi_tests` (default); `8/8` + `4/4` (`test-hooks`) | `tools/ci.ps1` |
| **C++ Test Suite — `windows-debug`** | `10/10` pass | `10/10` passed (`23.47 s`) | `tools/ci.ps1` |
| **C++ Test Suite — `windows-relwithdebinfo`** | `10/10` pass | `10/10` passed (`11.13 s`) | `tools/ci.ps1` |
| **C++ Test Suite — `ci-validation` (ASan + GPU-AV + SyncVal + HotPathGuard)** | `10/10` pass | `10/10` passed (`33.38 s`) | `tools/ci.ps1` |
| **Build Only — `windows-release`** | Clean link | Passed | `tools/ci.ps1` |
| **`clang-tidy` (`src/**/*.cpp`)** | `0` errors | Passed (`19/19` translation units clean) | `tools/ci.ps1` |
| **Windowed Vulkan Validation Smoke (`--smoke-frames 30`, no avatar, Debug)** | `0` errors/warnings | `vk_errors=0 vk_warnings=0 hot_path_violations=0 ui_cpu_p95_ms=0.265` | `tools/ci.ps1` (`windows-debug`) |
| **Windowed Vulkan Validation Smoke (`--smoke-frames 30 --avatar reference_avatar.vrm`, Debug)** | `0` errors/warnings | `vk_errors=0 vk_warnings=0 hot_path_violations=0 ui_cpu_p95_ms=0.384` | `tools/ci.ps1` (`windows-debug`) |
| **Windowed Smoke (`--smoke-frames 60 --avatar reference_avatar.vrm`, RelWithDebInfo)** | UI CPU `<= 0.40 ms` | `ui_cpu_mean_ms=0.117`, `ui_cpu_p95_ms=0.221`, `0` steady growth, `0` fallbacks | `build/windows-relwithdebinfo/src/app/aether_studio.exe` |
| **`PERF-007` Warm/Cold Cache Load (`reference_avatar.vrm`, Release)** | Warm `<= 150.0 ms` | Cold `128.34 ms`, Warm `47.86 ms` | Clean temp cache dir (`AETHER_TEXTURE_CACHE_DIR`) |
| **Single-Batch VMA GPU Upload (`reference_avatar.vrm`, RelWithDebInfo)** | `<= 50.0 ms` | `open+query=10.18 ms`, `fill=33.78 ms`, `gpu_upload=20.66 ms` (`48.68 MiB` staging) | `asset_upload_vk_test.exe` |
| **20-Model Corpus Auto-Mapping & Retargeting (`tests/data/corpus/*`)** | `20/20` load, `>= 53/55` (`95%`) mapped | Passed in both Rust (`loader_tests`) and C++ (`asset_loader_test`) across all presets | `tools/ci.ps1` |
| **Continuous Fuzzing — `fuzz_glb` (`>= 600 s`)** | `0` crashes | `600 s`, `4,392,698` iterations (`7,321.2 exec/s`), `crashes=0` | `fuzz_runner.exe --release` |
| **Continuous Fuzzing — `fuzz_vrm_json` (`>= 600 s`)** | `0` crashes | `600 s`, `32,245,957` iterations (`53,743.3 exec/s`), `crashes=0` | `fuzz_runner.exe --release` |
| **Continuous Fuzzing — `fuzz_fbx` (`>= 600 s`)** | `0` crashes | `600 s`, `2,015,519` iterations (`3,359.2 exec/s`), `crashes=0` | `fuzz_runner.exe --release` |
| **Continuous Fuzzing — `fuzz_pmx` (`>= 600 s`)** | `0` crashes | `600 s`, `1,489,930` iterations (`2,483.2 exec/s`), `crashes=0` | `fuzz_runner.exe --release` |

---

## 3. Fuzz Hardening & CI Discoveries Fixed Before Closure

1. **`gltf` Validation & Slice Bounds (`crates/aether-asset-loader/src/vrm_gltf.rs`)**:
   - Initial `fuzz_glb` (`iter=15`) and `fuzz_vrm_json` (`iter=13161`) exposed internal `unwrap()` panics in `gltf-1.4.1` caused by calling `Document::from_json_without_validation`, and a subsequent `fuzz_glb` (`iter=43076`) exposed mismatched vertex-attribute accessor counts (`index out of bounds`).
   - Replaced validation bypass with an explicit O(n) panic-free `validate_gltf_root`, enforced accessor element-dimension and stride bounds checks in `read_accessor_bytes` / typed readers, checked primitive vertex index ranges prior to baking, and re-ran `fuzz_glb` and `fuzz_vrm_json` for 600 s cleanly (`0` crashes across >40.1M total iterations).
2. **`HotPathGuard` Scope Boundary Around Vulkan Validation Layer Hooks (`tests/cpp/asset_upload_vk_test.cpp`)**:
   - `ci-validation` (GPU-AV + `HotPathGuard`) caught `425` internal CRT heap allocations originating from `VK_LAYER_KHRONOS_validation` hooks inside `vkCmd*` when `PreviewPass::record()` was wrapped inside `AETHER_HOT_PATH_SCOPE`. Aligned the test boundary with `src/app/application.cpp` (profiling `fit_camera_to_avatar` CPU work inside `AETHER_HOT_PATH_SCOPE`, keeping `vkCmd*` recording outside CRT `malloc` trapping while keeping `PreviewPass::record` allocation-free in source).
3. **CMake Python3 Lookup & Ninja Console Pool Build Wrapper (`src/renderer/CMakeLists.txt`, `tools/ci.ps1`, `src/app/main.cpp`)**:
   - Added `find_package(Python3 REQUIRED COMPONENTS Interpreter)` in `src/renderer/CMakeLists.txt`, wrapped `cmake --build` with `DEVNULL` stdin in `tools/ci.ps1` for headless Ninja Compatibility with Corrosion's `USES_TERMINAL`, and guarded `main()` against `bugprone-exception-escape`.

---

## 4. Architecture Decision Records & Open Stubs

- **ADRs written during Phase 2**:
  - `docs/adr/0004-phase2-asset-loader.md`
  - `docs/adr/0005-phase2-fuzz-and-synthetic-corpus.md`
  - `docs/adr/0006-slang-shader-compiler-toolchain.md`
- **Remaining Stubs (`docs/STATUS.md`)**:
  - Plugin host / production graph (`aether_host_*`, `aether_graph_*`): Phase 6.
  - Full OpenVINO `NPU` execution provider probe in `HardwareInventory::npu`: Phase 3.
  - Translation lookup table `tr()`: Post-v1.
