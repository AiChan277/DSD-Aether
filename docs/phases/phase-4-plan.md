# Phase 4 Plan — Vulkan 2.5D NPR Renderer & Hardware Ray Tracer

- **Specification Reference**: `AETHER-TS-001 v1.0` §3.5, §4.8, §10 (10.1–10.11), §13 (Phase 4), §14.3, `HR-01`–`HR-10`
- **Prerequisite**: Phases 1, 2, and 3 complete and CI-green (`tools/ci.ps1` exit code `0`).

## 1. Objectives & Task Breakdown

| Task ID | Area | Deliverable | Spec Ref |
|---|---|---|---|
| `P4-01` | RHI Device, Queues, Multi-GPU, Memory & Cache | Explicit LUID/vendor/device selection & scoring (`discrete +100`, `RT +50`, `VRAM GB * 2`); full §10.1 required + RT/optional extensions; optional async compute & DMA transfer queues; `VK_EXT_memory_budget` VRAM queries; `VK_KHR_calibrated_timestamps` QPC alignment (`2 s` recalibration); persisted pipeline cache; `RebarUploadRing` (`8 MiB`/slot); `RhiGpuTimerPool`. | §10.1, §10.2, §10.3, §4.6 |
| `P4-02` | Bindless Descriptor System & Slang Shader Pipeline | Global Set 0 bindless layout (`sampled_images[<=16384]` update-after-bind/partially-bound, `samplers[64]`, `storage_images[1024]`) + optional Set 1 (`TLAS` when RT enabled); BDA `<= 128 B` push constants + `DrawData` SSBO; `shaders/manifest.toml`; `tools/shader_build.py` upgraded to `spirv_1_6` + `spvRayQueryKHR` + `-reflection-json` validation + DXIL compile-only check; Debug disk load & hot-reload watcher. | §3.5, §10.3 |
| `P4-03` | Compute Skinning & BLAS/TLAS Management | Pass 1 compute shader (`aether_skin_morph.slang`) applying weight-culled (`< 1e-4`) sparse morphs, 8-weight `UNORM16` skinning (`pos`, `prev_pos`, `true_normal`, `proxy_normal`, `smooth_tangent`); Pass 2 `AccelStructManager` with `ALLOW_UPDATE \| PREFER_FAST_BUILD` avatar BLAS refit (`<= 0.35 ms` target, full rebuild every 300 frames / topology change), static prop `PREFER_FAST_TRACE`, TLAS rebuild (`<= 64` instances, masks `0x01`/`0x02`/`0x04`). | §10.4, §10.5 |
| `P4-04` | Prepass (Z-Flatten & G-Buffers) | Pass 3 (`D32_S8` stencil `0x01` avatar, `0x02` hair-front, `0x04` hair-shadow receiver; `GB_N` RG16 oct true normal; `GB_MV` RG16F; `GB_MAT` R16UI; `GB_MISC` RGBA8 flatten/sdf/flags). Un-flattened view depth preservation (`clip.z = ndc_depth_from_view_z(pos_view.z) * clip.w`) for accurate self-occlusion. | §10.4, §10.6.1 |
| `P4-05` | Stylized Ray-Query Effects & Non-RT Fallback | Pass 4 RT (`spvRayQueryKHR` compute): (1) cone variable-penumbra shadows (`<= 4` lights, 1–2 spp spatiotemporal blue noise, excludes `0x04` hair mask for `sdf_face_mask > 0.5`), (2) occluded rim (`t_max = 0.45 m`, Fresnel > 0.2), (3) half-res cosine hemisphere colour bleed (`t_max = 0.25 m`, hit material lookup, strength `0.35`). Pass 4 Non-RT fallback: `2048^2` CSM + screen-space rim occlusion + configurable bounce tint. | §10.6.1, §10.8 |
| `P4-06` | `IDenoiserBackend` & Auto-Fallback | Abstract interface `IDenoiserBackend`; mandatory pre-allocated `SvgfLiteDenoiser` (temporal MV reprojection + 3x3 AABB history clamp + 2 à-trous iterations); `NvidiaSigmaDenoiser` & `FidelityFxComputeDenoiser` compute paths; dynamic runtime probe for OptiX & OIDN 2.x (ADR-0009); `DenoiserManager` 120-frame p95 `> 0.8 ms` and failure zero-alloc auto-fallback. | §10.9, ADR-0009 |
| `P4-07` | 2.5D NPR Forward, Outlines & Crease Lines | Pass 6 opaque/cutout + Pass 8 sorted transparent (`shaders/aether_live2d_npr.slang`): Z-flattening, 2D SDF face shadow (`0–360°` light sweep + mirror + `smoothstep`), read-only `D32_S8` stencil hair drop shadow (`DEPTH_STENCIL_READ_ONLY_OPTIMAL`, `HR-08` clean), MToon 1.0 toon ramp (`Cel`/`Soft`) + matcap + angel-ring `hairHighlight`. Pass 7 inverted-hull outlines (`w^0.65` distance invariance). Pass 9 screen-space crease compute (`>35°`, `>2%` depth, material ID edge inside stencil `0x01`). | §10.6, §10.7 |
| `P4-08` | Premultiplied Bloom, Display Transform & Output Encode | Pass 10 compute 6-level dual-Kawase premultiplied bloom/glow (`rgb + glow`, `a + (1-a)*lum(glow)`). Pass 11 composite (`Standard` clamp vs `Soft highlights` AgX shoulder) -> `OUT_RGBA16F` + 14 Debug Shading Views. Pass 12 output encode -> `OUT_BGRA8` (premul/straight, sRGB OETF, `±0.5 LSB` blue-noise dither), `OUT_NV12` (alpha composited over non-alpha BG), and `OUT_BGRA8_BOTTOM_UP`. | §10.10, §10.11 |
| `P4-09` | Dual Command Buffer Submission & Viewport Compose | `REQ-RND-001` split: `cmd_critical` (Passes 1–12) signaling `frame_tl` for T4 before `cmd_ui` (Passes 13–14 -> swapchain). Pass 13 viewport compose (`8 px` `#15171C`/`#1A1D23` checkerboard, overlays, instanced `Alt+C` CVD heatmap `#199E70`/`#C98500`/`#3987E5` solid/dashed/dotted) + `128^2` Light Orbit Orb NPR sphere renderer. Integration into `Application` & `DevShell`. | §10.2, §10.11, §12.6 |
| `P4-10` | Offline 2D SDF Face Shadow Generator | `OfflineSdfGenerator` rendering face mesh/proxy normals across 16 horizontal light angles and merging a UV-space `R8` signed-distance shadow field (`Rigging ▸ Face ▸ Generate SDF`) for avatars without authored face SDF maps. | §10.6.2 |
| `P4-11` | `VK_ERROR_DEVICE_LOST` Recovery | `RendererRecovery` recreating `RhiDevice`, swapchain, frame graph, denoisers, pipelines, and GPU avatar buffers from CPU descriptors in `<= 3.0 s`, keeping last good output latched and writing a crash breadcrumb. | §4.8, §14.6 |
| `P4-12` | Exit-Gate Tests & Golden Image Suite | Catch2 suite covering sync validation (`HR-08`), Z-flatten nose invariance, `0–360°` SDF sweep, stencil hair shadow tracking, `w^0.65` outline `±10%` invariance (`0.5–3.0 m`), premultiplied glow `ΔE <= 1` over white & black, denoiser vendor auto-select & `0.8 ms` auto-fallback, BLAS refit timing & `<= 4.2 ms` 1080p RT GPU benchmark, device-lost `<= 3.0 s` recovery, and 6-scene RT-on/off NVIDIA vs Intel golden FLIP (`<= 0.05`, ADR-0010). | §13, §14.3, ADR-0010 |

## 2. Planned File Layout

- `src/rhi/`:
  - `rhi_device.{hpp,cpp}` (upgraded multi-GPU, RT/optional exts, queues, budget, pipeline cache, device-lost rebuild)
  - `rhi_swapchain.{hpp,cpp}` (device-lost detection + recreate + split command helpers)
  - `bindless.{hpp,cpp}` (Global Set 0 bindless + Set 1 TLAS descriptor manager)
  - `rebar_ring.{hpp,cpp}` (8 MiB/frame ReBAR upload ring + fallback copy)
  - `gpu_timing.{hpp,cpp}` (timestamp query pool + `VK_KHR_calibrated_timestamps` QPC calibration + pass stats)
- `src/renderer/`:
  - `frame_graph.{hpp,cpp}` (14-pass FrameGraph, render targets, dual command buffer execution, debug view routing)
  - `skinning_pass.{hpp,cpp}` (Pass 1 compute skin + morph dispatch + `DrawData` buffer)
  - `accel_struct.{hpp,cpp}` (Pass 2 BLAS refit / 300-frame rebuild + TLAS build)
  - `npr_renderer.{hpp,cpp}` (high-level 14-pass orchestrator replacing Phase 2 `PreviewPass` for viewport + headless rendering, Light Orbit Orb 128x128 preview, `OfflineSdfGenerator`)
  - `denoise/` (`denoiser_backend.hpp`, `svgf_lite_denoiser.{hpp,cpp}`, `vendor_denoisers.{hpp,cpp}`, `denoiser_manager.{hpp,cpp}`)
  - `lighting_rig.hpp` (up to 8 scene lights, 4 RT shadow casters, Key/Rim/Hair Orbit roles, presets)
  - `sdf_generator.{hpp,cpp}` (offline 16-angle 2D SDF face shadow generator)
- `shaders/`:
  - `manifest.toml`
  - `aether_common.slang` (structs, octahedral encoding, bindless heap accessors, Z-flatten helpers, blue-noise hash)
  - `aether_skin_morph.slang` (Pass 1 compute)
  - `aether_prepass.slang` (Pass 3 vertex/pixel)
  - `aether_ray_effects.slang` (Pass 4 RT compute with `spvRayQueryKHR`)
  - `aether_csm_fallback.slang` (Pass 4 non-RT `2048^2` shadow map + SS rim/bounce compute)
  - `aether_denoise_svgf.slang`, `aether_denoise_sigma.slang`, `aether_denoise_ffx.slang` (Pass 5)
  - `aether_live2d_npr.slang` (Pass 6 + Pass 8 + 128² Orbit Orb)
  - `aether_outline_hull.slang` (Pass 7)
  - `aether_crease_lines.slang` (Pass 9 compute)
  - `aether_post_bloom.slang` (Pass 10 dual-Kawase + Pass 11 composite/AgX/debug + Pass 12 encode/NV12/BGRA)
  - `aether_viewport_compose.slang` (Pass 13 checker/overlays + CVD skeleton heatmap)
  - `aether_sdf_bake.slang` (Offline 16-angle SDF compute)
- `tools/`:
  - `shader_build.py` (manifest-driven `spirv_1_6` + `spvRayQueryKHR` + `-reflection-json` + DXIL check)

## 3. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `VK_LAYER_KHRONOS_validation` sync2 hazards on read-only stencil view (`D32_S8` used concurrently as read-only depth attachment and stencil `Texture2D<uint>`). | Create explicit `VK_IMAGE_VIEW_TYPE_2D` with `aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT`, transition entire `D32_S8` image (`DEPTH \| STENCIL` aspects) via `sync2` into `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` before Pass 6, and use `DEPTH_READ_ONLY_BIT \| STENCIL_READ_ONLY_BIT` flags in `VkRenderingInfo`. |
| Hardware RT vs Non-RT / SwiftShader validation parity. | Guard all `TLAS` / `VK_KHR_ray_query` pipeline & descriptor set creation behind verified `rt_supported` capability; Set 1 holds TLAS so Set 0 remains identical across all devices (`HR-08`). |
| Exceeding `4.2 ms` GPU budget @ 1080p under heavy hair/overdraw. | Morton-ordered index buffers (Phase 2), BLAS refit-only (`ALLOW_UPDATE \| PREFER_FAST_BUILD`), 1 spp blue-noise cone ray + SVGF-lite / NRD-SIGMA compute, half-res bounce, full rebuild amortized every 300 frames. |
| Hot-path allocations (`HR-01`) in frame graph or denoiser auto-fallback. | Pre-allocate all frame graph images, descriptor updates, ReBAR upload buffers, and both active + fallback denoiser backends during `init()` / `resize()`. |

## 4. Tests to Write First (`tests/cpp/`)

1. `rhi_phase4_test.cpp`: Multi-GPU enumeration & scoring, ReBAR ring wraparound & zero-alloc update, bindless descriptor capacity & reflection check, device-lost simulated recovery (`<= 3.0 s` end-to-end).
2. `renderer_optix_fx_test.cpp` (`renderer_npr_effects_test.cpp`):
   - Prepass + NPR optical invariants: Z-flattening nose-depth compression ratio (`>= 70%` foreshortening reduction at `d=0.5 m`), `0–360°` SDF face shadow sweep monotonicity & left/right symmetry, stencil hair shadow displacement tracking light azimuth, inverted-hull screen width within `±10% px` across `0.5–3.0 m`, premultiplied bloom over `#000000` & `#FFFFFF` matching analytical composite (`ΔE <= 1.0`).
   - `OfflineSdfGenerator` bake produces valid monotonic SDF on synthetic/reference face mesh.
   - Zero hot-path heap allocations (`AETHER_HOT_PATH_SCOPE`) & zero Vulkan validation/sync errors (`HR-08`).
3. `denoiser_and_rt_test.cpp`:
   - Vendor auto-selection (`0x10DE`, `0x8086`, `0x1002`, SwiftShader).
   - Injected failure + synthetic `> 0.8 ms` p95 over 120 frames triggers zero-alloc fallback to Built-in SVGF-lite + notification flag.
   - BLAS refit & 14-pass 1080p RT performance benchmark (`BLAS refit <= 0.6 ms / ~0.35 ms`, total output-critical `Passes 1–12 <= 4.2 ms p95` on RTX 5060).
4. `golden_render_test.cpp`:
   - `tests/cpp/flip_metric.hpp` self-tests.
   - 6 canonical scenes × (RT ON + RT OFF) rendered on active discrete GPU + Intel iGPU cross-adapter check + comparison vs locked `tests/golden/references/*.png` (`FLIP mean <= 0.05`, zero `vk_errors`).
