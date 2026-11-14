# ADR-0009: Multi-Vendor Denoiser Backends & External SDK Probing (`§10.9`, `HR-09`, `HR-10`)

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.7 (#8, #9), §3.2, §10.9, `HR-01`, `HR-09`, `HR-10`

## Context

`AETHER-TS-001` §10.9 specifies a common `IDenoiserBackend` interface supporting four conceptual paths:
1. **Built-in "SVGF-lite"** (mandatory, vendor-agnostic, temporal accumulation with motion-vector reprojection + 2 à-trous edge-stopped iterations).
2. **NVIDIA**: OptiX AI Denoiser (Vulkan–CUDA external memory interop, optional `AETHER_WITH_OPTIX=ON`) or NRD SIGMA (pure compute default when OptiX is not built).
3. **Intel**: OIDN 2.x (SYCL/XMX with Vulkan external memory interop).
4. **AMD**: FidelityFX Shadow Denoiser (pure compute; WMMA FP16 optional & capability-checked per §0.7 #9).

Clean build machines and standard CI hosts do not bundle proprietary CUDA/OptiX 8, Intel oneAPI/OIDN 2.x SYCL runtime, or external NRD/FidelityFX SDK trees, and `HR-10` prohibits importing third-party source trees or redistributable binaries without licence review and version pinning. Meanwhile, `HR-01` requires zero heap allocations on per-frame hot paths—even when a denoiser backend fails or exceeds the `0.8 ms` p95 over 120 frames budget and must auto-switch to Built-in SVGF-lite.

## Decision

1. **Mandatory Built-in SVGF-lite (`SvgfLiteDenoiser`)**:
   - Implemented in first-party Slang compute shaders (`shaders/aether_denoise_svgf.slang`), compiled to SPIR-V 1.6 cleanly by `slangc` (and verified against DXIL).
   - Performs motion-vector reprojection (`GB_MV`), depth/normal disocclusion rejection, `3×3` neighborhood AABB variance history clamping, and 2 edge-stopped à-trous spatial iterations using `D32`, `GB_N`, and `GB_MISC`.
   - Always pre-initialized during `DenoiserManager::init(...)` alongside any selected vendor backend so runtime fallback (injected failure or `p95 > 0.8 ms` for 120 frames) swaps cleanly with **zero heap allocation (`HR-01`)** and **zero Vulkan sync hazards (`HR-08`)**.

2. **Vendor Backends without External Binary Dependencies (Compute-Native)**:
   - **NRD-SIGMA Compute (`NvidiaSigmaDenoiser`)**: Implemented as a genuine Vulkan 1.3 Slang compute pipeline (`shaders/aether_denoise_sigma.slang`) modeling hit-distance-guided penumbra radius stabilization (`t_hit` stored in `RT_SHADOW.w` / `RT_SHADOW.g`), temporal variance tracking, and subgroup-friendly edge-stopped spatial filtering. Auto-selected on NVIDIA GPUs (`vendor_id == 0x10DE`) when external OptiX is not installed/enabled.
   - **AMD FidelityFX Compute (`FidelityFxComputeDenoiser`)**: Implemented as a genuine Vulkan 1.3 Slang compute pipeline (`shaders/aether_denoise_ffx.slang`) following tile-classified spatio-temporal shadow filtering (`§0.7 #8/#9`), using pure compute (no WMMA requirement). Auto-selected on AMD GPUs (`vendor_id == 0x1002`).

3. **External Interop Probing for OptiX & OIDN 2.x (`HR-09`, `HR-10`)**:
   - **OptiX (`OptixExternalDenoiser`)**: Requires `AETHER_WITH_OPTIX=ON` plus dynamic load of `nvoptix.dll` / `nvcuda.dll` and Vulkan extensions `VK_KHR_external_memory_win32` + `VK_KHR_external_semaphore_win32`. When absent, `init()` returns `ErrorCode::Unsupported` and `DenoiserManager` logs a one-time notice and selects `NvidiaSigmaDenoiser` -> fallback `SvgfLiteDenoiser`.
   - **Intel OIDN 2.x (`OidnExternalDenoiser`)**: Dynamically probes `OpenImageDenoise.dll` (Apache-2.0) + `VK_KHR_external_memory_win32`. When absent on the host (as tracked in `docs/STATUS.md`), `init()` returns `ErrorCode::Unsupported` cleanly and `DenoiserManager` falls back immediately to `SvgfLiteDenoiser` per §10.9 ("This is the fallback whenever a vendor backend is missing, fails, or exceeds 0.8 ms p95 over 120 frames").

## Consequences

- Zero unpinned external C++/binary dependencies added to `vcpkg.json` or repo (`HR-10` satisfied).
- Per-vendor auto-selection (`0x10DE`, `0x8086`, `0x1002`, software) and runtime budget/failure auto-fallback work deterministically out of the box on NVIDIA RTX 5060, Intel Graphics, and SwiftShader.
- External `OptiX` and `OIDN` interop slots remain explicitly documented as runtime-optional in `docs/STATUS.md` (`HR-09`), never misrepresented as linked when the external runtime is missing.
