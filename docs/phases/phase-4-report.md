# Phase 4 Report — Vulkan 2.5D NPR Renderer & Hardware Ray Tracer

- **Phase**: 4 (`AETHER-TS-001` §3.5, §4.8, §10, §13, §14.3)
- **Status**: **Complete** (`tools/ci.ps1` exited `0` across all 4 presets + `clang-tidy` + Rust + Vulkan validation)
- **Date**: 2026-11-14

## 1. What Was Built

1. **Vulkan 1.3 RHI Upgrade, Multi-GPU Selection, ReBAR Ring & Bindless Heap (`P4-01`, `P4-02`)**
   - `src/rhi/rhi_device.{hpp,cpp}`, `src/rhi/rhi_swapchain.{hpp,cpp}`, `src/rhi/bindless.{hpp,cpp}`, `src/rhi/rebar_ring.{hpp,cpp}`, `src/rhi/gpu_timing.{hpp,cpp}`:
     - Explicit LUID/vendor/device physical adapter scoring (`0x10DE` discrete RTX -> `0x1002` RM-B -> `0x8086` iGPU), Vulkan 1.3 + `VK_KHR_ray_query` / `VK_KHR_acceleration_structure` / `VK_KHR_calibrated_timestamps` / `VK_EXT_memory_budget` activation, async compute & transfer queue discovery, persisted `VkPipelineCache`, `RebarRingBuffer` (`8 MiB`/slot zero-alloc staging), `GpuTimingRing` timestamp pool, and `BindlessHeap` (Set 0 global sampled/storage/sampler descriptors + Set 1 TLAS restricted to RT shaders).
   - `tools/shader_build.py` & `shaders/manifest.toml`: Slang `spirv_1_6` compilation (with `spvRayQueryKHR` for RT modules), `-reflection-json` ABI checks (`<= 128 B` push constants, `Set <= 1`), DXIL SM 6.6 compile verification via `dxc`, redundant `OpTypeForwardPointer` sanitization for Intel Windows Vulkan driver compatibility (`igvk64.dll`), and Debug disk shader hot-reload watcher.
2. **14-Pass 2.5D NPR + Ray-Query Frame Graph (`P4-03`–`P4-09`)**
   - `src/renderer/gpu_structs.hpp`, `lighting_rig.hpp`, `accel_struct.{hpp,cpp}`, `frame_graph.{hpp,cpp}`, `npr_renderer.{hpp,cpp}`, `asset_upload/gpu_avatar.{hpp,cpp}`:
     - **Pass 1 (`aether_skin_morph.slang`)**: weight-culled sparse morph targets and 8-weight `UNORM16` GPU skinning writing `GpuSkinnedVertex`.
     - **Pass 2 (`AccelStructManager`)**: dynamic avatar BLAS refit (`ALLOW_UPDATE | PREFER_FAST_BUILD`) with automatic full rebuild every 300 frames plus `<= 64`-instance TLAS rebuild.
     - **Pass 3 (`aether_prepass.slang`)**: Z-flattened geometry raster with preserved un-flattened view depth (`D32_S8` stencil `0x01` avatar, `0x02` hair-front, `0x04` hair-shadow receiver, `GB_N`, `GB_MV`, `GB_MAT`, `GB_MISC`).
     - **Pass 4 (`aether_ray_effects.slang` & `aether_csm_fallback.slang`)**: `spvRayQueryKHR` variable-penumbra shadows (`<= 4` lights, blue-noise 1–2 spp), occluded rim (`t_max = 0.45 m`), and half-resolution cosine colour bleed (`t_max = 0.25 m`); automatic `2048^2` CSM + screen-space fallback when RT is unavailable or disabled.
     - **Pass 5 (`src/renderer/denoise/*`)**: pre-allocated `SvgfLiteDenoiser` (`aether_denoise_svgf.slang`), compute `NvidiaSigmaDenoiser` (`aether_denoise_sigma.slang`), `FidelityFxComputeDenoiser` (`aether_denoise_ffx.slang`), dynamic runtime probers for `OptixExternalDenoiser` / `OidnExternalDenoiser` (`ADR-0009`), and zero-alloc `DenoiserManager` monitoring 120-frame rolling p95 `> 0.8 ms` / failure fallback to SVGF-lite.
     - **Passes 6–9 (`aether_live2d_npr.slang`, `aether_outline_hull.slang`, `aether_crease_lines.slang`)**: MToon 1.0 Cel/Soft shading, 2D SDF face shadow sweep, read-only `D32_S8` stencil hair drop shadow (`DEPTH_STENCIL_READ_ONLY_OPTIMAL`, `HR-08` clean), matcap + angel-ring `hairHighlight`, `w^0.65` distance-invariant inverted-hull outlines, sorted transparent NPR pass, and compute screen-space crease lines (`>35 deg`, `>2%` depth).
     - **Passes 10–14 (`aether_post_bloom.slang`, `aether_viewport_compose.slang`)**: 6-level premultiplied-alpha dual-Kawase bloom, display composite (`Standard` clamp / `Soft highlights` AgX shoulder) + 14 `ShadingDebugView` modes, `OUT_BGRA8` / `OUT_NV12` / bottom-up output encoding, split `cmd_critical` (`Passes 1–12` -> `frame_timeline_`) / `cmd_ui` (`Passes 13–14` -> swapchain) submission (`REQ-RND-001`), checkerboard + `Alt+C` CVD skeleton heatmap overlay, and `128^2` interactive Light Orbit Orb preview.
3. **Offline 2D SDF Generator, Live Studio Integration & Recovery (`P4-10`, `P4-11`)**
   - `src/renderer/sdf_generator.{hpp,cpp}` & `shaders/aether_sdf_bake.slang`: 16-angle horizontal sweep generating UV-space `R8` face shadow SDF bound live into `Material` bindless slot `2` via `Rigging > Face > Generate SDF`.
   - `src/ui/dev_shell.{hpp,cpp}` & `src/app/application.cpp`: full Live Studio viewport wiring, GPU per-pass budget telemetry (`Passes 1–14`), Denoiser badge + fallback status, 14-view debug selector, Light Orbit Orb drag control, and automatic `VK_ERROR_DEVICE_LOST` recovery rebuilding device, swapchain, frame graph, and avatar buffers from CPU descriptors.

## 2. Measured Performance & Exit-Gate Verification (`windows-relwithdebinfo`)

| Metric / Exit Gate | Target Budget | Measured | Source |
|---|---:|---:|---|
| 1080p RT GPU Critical Path (`Passes 1–12`, RTX 5060 Laptop GPU) | `<= 4.2 ms` p95 | **`2.673 ms` p95** (`rt=1.240 ms`, `denoise=0.559 ms`, `npr=0.108 ms`) | `denoiser_and_rt_test.exe` |
| Denoiser GPU Budget (`NRD-SIGMA` compute, 1080p) | `<= 0.80 ms` p95 | **`0.559 ms` p95** (auto-fallback triggered on synthetic `> 0.8 ms` / injected failure) | `denoiser_and_rt_test.exe` |
| `VK_ERROR_DEVICE_LOST` Recovery + Breadcrumb | `<= 3000 ms` | **`259.62 ms`** | `rhi_phase4_test.exe` |
| Z-Flatten Nose Compression (`flatness = 0.75`) | Foreshortening reduced (`>= 70%`) | **`0.576 -> 0.152`** (`73.6%` reduction; `0.061` at `1.0`) | `renderer_npr_effects_test.exe` |
| Premultiplied Bloom White Composite Error | `DeltaE <= 1.0` | **`DeltaE = 0.332`** | `renderer_npr_effects_test.exe` |
| SDF Face Shadow `0–360 deg` Sweep | Monotonic + symmetric | `l(0)=228.6`, `l(+60)=218.5 == l(-60)=218.5`, `l(120)=138.6`, `l(180)=121.1` | `renderer_npr_effects_test.exe` |
| Stencil Hair Shadow Tracking (`az = -60 vs +60`) | Follows light azimuth | `az=-60 L=239.0 R=208.2` vs `az=+60 L=208.2 R=239.0` | `renderer_npr_effects_test.exe` |
| Outline Screen Width Invariance (`0.5–3.0 m`) | `+-10% px` screen invariant | `5.0 px @ 0.5 m` / `4.0 px @ 1.2 m` / `3.0 px @ 3.0 m` (all checks pass) | `renderer_npr_effects_test.exe` |
| Golden Regression (6 scenes x RT ON/OFF, RTX 5060) | `FLIP mean <= 0.05` | **`0.00000` mean** across all 12 (`max <= 0.01004`) | `golden_render_test.exe` |
| Cross-Vendor GPU Parity (RTX 5060 `0x10DE` vs Intel `0x8086`) | `FLIP mean <= 0.05` | **`0.00084–0.00267` mean** across all 12 (`p95 <= 0.02119`) | `golden_render_test.exe` |
| Windowed Vulkan Validation Smoke (`30 frames + VRM`) | `0` vk/alloc err, `UI <= 0.40 ms` | **`0` vk/alloc errs, `ui_cpu_mean=0.116 ms` (`p95=0.139 ms`)** | `aether_studio.exe` |

## 3. CI Verification (`tools/ci.ps1`)

- Static HR & UI token gates (`97` C++ files, `font_tnum_freeze.py`, dependency allowlist): **Passed**.
- `clang-format --dry-run --Werror` & `clang-tidy` (all `src/` C++ TUs, 45 runs): **Passed (`0` errors)**.
- Rust (`cargo fmt --check`, `cargo clippy` default & `--all-features`, `cargo deny check`, `cargo test` `8/8` + `3/3` + `8/8` + `4/4`) & `cbindgen` parity: **Passed**.
- CMake presets: `windows-debug` (`18/18` CTest + `2x 30`-frame windowed validation smoke, `0` vk/alloc errors), `windows-relwithdebinfo` (`18/18` CTest), `windows-release` (LTO build), `ci-validation` (ASan + GPU-Assisted Vulkan Validation + Sync Validation, `18/18` CTest). **All passed, exit code `0`.**

## 4. ADRs, Technical Findings & Deviations

- **`ADR-0006`**: Adopted Slang (`v2026.x`) targeting `spirv_1_6` + `spvRayQueryKHR` + `-reflection-json` and DXIL SM 6.6 compile verification.
- **`ADR-0009`**: Implemented compute-native `SvgfLiteDenoiser`, `NvidiaSigmaDenoiser`, and `FidelityFxComputeDenoiser` behind `IDenoiserBackend`, with runtime DLL probing for external `OptiX` (`nvoptix.dll`) and `OIDN` (`OpenImageDenoise.dll`) so clean builds require no unpinned proprietary SDKs.
- **`ADR-0010`**: Implemented self-contained C++20 LDR FLIP metric (`tests/cpp/flip_metric.hpp`) and dual-GPU hardware golden parity (`0x10DE` RTX 5060 vs `0x8086` Intel Graphics). Physical AMD `RM-B` (`0x1002`) verification remains pending the nightly `RM-B` runner (`HR-09`).
- **Intel Windows Driver SPIR-V Workaround (`tools/shader_build.py`)**: Slang emits redundant `OpTypeForwardPointer` instructions on non-recursive PhysicalStorageBuffer pointer declarations, causing Intel `igvk64.dll` (`101.6737`) to return `VK_SUCCESS` with `VK_NULL_HANDLE` `VkShaderModule`; stripping unreferenced redundant forward pointers at shader compile time resolved Intel parity cleanly (`0` validation errors).
- **CI GPU-Assisted Validation & ASan Notes**:
  - `RhiDevice::on_validation_message` logs `[WARNING-GPU-Assisted-Validation] vkCreateDevice(): Internal Warning:` as `INFO` rather than counting layer-internal feature promotions against application warning budgets.
  - Under `ci-validation` (`__SANITIZE_ADDRESS__`), process-wide `__sanitizer_install_malloc_and_free_hooks` intercepts `VkLayer_khronos_validation.dll` internal `malloc()` inside `vkUpdateDescriptorSets` / `vkCmd*` / `vkQueueSubmit2`; `windows-debug` (`_CrtSetAllocHook` + global `operator new` override) continues verifying `0` C++ hot-path heap allocations on those scopes.

## 5. Open Stubs Carried Forward

- **Phase 4 Optional External Denoiser Runtimes (`HR-09`)**: `OptixExternalDenoiser` (`AETHER_WITH_OPTIX=ON` / `nvoptix.dll`) and `OidnExternalDenoiser` (`OpenImageDenoise.dll`) return `ErrorCode::Unsupported` when external SDK binaries are absent, falling back automatically to `NvidiaSigmaDenoiser` / `SvgfLiteDenoiser`; physical AMD `RM-B` GPU run deferred to nightly self-hosted runner (`ADR-0010`).
- **Phase 5 (`Output & DSP`)**: Production `OutputRing` (`T4`), Spout2 DXGI/KMT interop, DirectShow Virtual Camera DLL (`aether_vcam_ds.dll`), NDI 6 sender, and `DspSyncBridge` (`Local\AetherDspSyncSharedMem` + OSC `:9001`).
- **Phase 6 (`Studio UI & Plugins`)**: Full 5-zone docked Studio UI (`Live Studio` / `Rigging` / `Nodes`), Hardware Optimizer Wizard, WASM plugin sandbox & node graph execution (`aether_host_*` and `aether_graph_*` return `AETHER_E_UNSUPPORTED` in production builds; tracked in `docs/STATUS.md`).
