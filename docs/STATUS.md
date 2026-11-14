# AETHER-2.5D Implementation Status (`HR-09`)

Updated per task and phase. Every unimplemented FFI entry point or subsystem returns `AETHER_E_UNSUPPORTED`, logs once, and is tracked here until its owning phase completes.

## Phase Progress

- **Phase 1 — Core Scaffold & FFI Contract**: Complete (`tools/ci.ps1` exit gate green across all 4 presets; see `docs/phases/phase-1-report.md`)
- **Phase 2 — Rust Unified Asset Loader**: Complete (`tools/ci.ps1` exit gate green across all 4 presets, 4×600 s fuzz clean, `PERF-007` warm `47.86 ms <= 150 ms`; see `docs/phases/phase-2-report.md`)
- **Phase 3 — NPU Tracking & Visibility-First IK**: Complete (`tools/ci.ps1` exit gate green across all 4 presets, `M2+M3 p95=3.35 ms`, `SimEngine::tick=0.052 ms`; see `docs/phases/phase-3-report.md`)
- **Phase 4 — Vulkan 2.5D Renderer & Ray Tracer**: Complete (`tools/ci.ps1` exit gate green across all 4 presets, 1080p RT `2.673 ms <= 4.2 ms`, RTX 5060 vs Intel Xe `FLIP <= 0.00267`; see `docs/phases/phase-4-report.md`)
- **Phase 5 — Alpha Video Output & Audio-DSP IPC**: Complete (`tools/ci.ps1` exit gate green across all 4 presets; `phase5_pipeline_m2p_test` 1080p60 `RT=ON` `p95 = 8.332 ms`, Spout `0.185 ms`, VCam DMA `p95 = 0.483 ms`, late-latch `p95 = 0.151 ms`; see `docs/phases/phase-5-report.md`)
- **Phase 6 — Studio UI & Plugin Node Host**: Complete (`tools/ci.ps1` exit gate green across all 4 presets + `clang-tidy`; `studio_ui_test` 64-node `p95 = 0.1427 ms <= 0.40 ms`, 300-frame soak x2 `p95 <= 0.239 ms`, `0` steady-state arena growth/fallbacks/HotPath violations, `native_plugin_runner_test` 15/15 stress green; see `docs/phases/phase-6-report.md`)
- **Release `a1.0.0B` Extensions (ADR-0015)**: Complete (C++20 `aether::GarbageCollector` epoch + incremental tri-color mark-sweep collector in `src/core/garbage_collector.{hpp,cpp}` and `aether::renderer::Dx11NprRenderer` DirectX 11 SM 5.0 2.5D NPR renderer & DXGI shared-texture backend in `src/renderer/dx11_renderer.{hpp,cpp}`; verified by `gc_and_dx11_renderer_test`, 24/24 CTest suites green)
- **GC & DirectX 11 Backend Integration (ADR-0016)**: Complete (`render.backend` Vulkan | DirectX 11 selectable in Inspector and via `--render-backend`; `Dx11Bridge` hands the D3D11 frame to the Vulkan outputs by shared texture or CPU copy; avatar swaps and bridge resources are retired through the GC against the frame timeline; verified by `dx11_backend_test` (silhouette IoU 0.972 on both paths) and the DX11/GC smoke gate in `tools/ci.ps1`)

## Open Stubs (`AETHER_E_UNSUPPORTED`)

| Symbol / Subsystem | Current Behaviour | Owning Phase |
|---|---|---|
| `NdiOutput::init` (without NDI 6 Runtime) | Returns `ErrorCode::Unsupported` and logs once when `NDI_RUNTIME_DIR_V6\Processing.NDI.Lib.x64.dll` is absent (`src/output/ndi_output.cpp`). | Optional Runtime |
| `tr()` string table (`src/ui/theme/tr.hpp`) | Identity passthrough returning English string literal; translation lookup table deferred post-v1 (§1.1). | Post-v1 |
