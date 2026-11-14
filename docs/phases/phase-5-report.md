# Phase 5 Report — Alpha Video Output & Audio-DSP IPC

**Phase**: 5 (`AETHER-TS-001 v1.0` §11, §13 Phase 5, `HR-01`–`HR-12`, `PERF-001`, `PERF-008`)  
**Status**: Complete (`tools/ci.ps1` exit gate passed across all 4 CMake presets, Rust checks, windowed Vulkan validation smoke runs, and `clang-tidy`)

---

## 1. What Was Built

- **Shared Output Configuration, Ring & Coordinator (`T4`)**:
  - Added [`output_config.hpp`](../../src/output/output_config.hpp), [`output_ring.hpp`](../../src/output/output_ring.hpp), and [`output_coordinator.hpp`](../../src/output/output_coordinator.hpp) / [`output_coordinator.cpp`](../../src/output/output_coordinator.cpp) implementing lock-free `ParamMailbox<OutputConfig>`, `SpscRing<OutputRingSlot, 16>`, and `LockFreeTripleBuffer<OutputRuntimeStatus>`.
  - Integrated `T4` output publishing and safe resize/shutdown synchronization (`wait_output_slots_drained`, shutdown ordering prior to `RhiDevice::wait_idle`) into [`pipeline_skeleton.hpp`](../../src/app/pipeline_skeleton.hpp), [`pipeline_skeleton.cpp`](../../src/app/pipeline_skeleton.cpp), [`application.cpp`](../../src/app/application.cpp), and Precision Dark Dev Shell controls/telemetry in [`dev_shell.hpp`](../../src/ui/dev_shell.hpp) / [`dev_shell.cpp`](../../src/ui/dev_shell.cpp).
- **Spout2 Zero-Copy D3D11 KMT Sender (`§11.2`)**:
  - Implemented [`spout_output.hpp`](../../src/output/spout_output.hpp) / [`spout_output.cpp`](../../src/output/spout_output.cpp) matching Vulkan GPU LUID to D3D11, sharing `DXGI_FORMAT_B8G8R8A8_UNORM` via `VK_KHR_external_memory_win32` (`D3D11_TEXTURE_KMT`), publishing the Spout2 shared-memory registry (`SpoutSenderNames`, 280 B `SharedTextureInfo`), enforcing the strict `1 ms` `SpoutAccessMutex` timeout (`HR-02`), and providing `CpuCompatibility` fallback.
- **DirectShow Virtual Camera x64 & x86 (`§11.3`)**:
  - Implemented [`vcam_transport.hpp`](../../src/output/vcam_transport.hpp) / [`vcam_transport.cpp`](../../src/output/vcam_transport.cpp) writing triple-buffered frames into `Local\AetherVCam_v1` (with `VK_EXT_external_memory_host` zero-copy path and mapped staging fallback) using lock-free seqlock headers (`AetherVCamHeader`, 128 B).
  - Implemented dependency-free COM filter [`aether_vcam_ds.cpp`](../../src/vcam/aether_vcam_ds.cpp) (`IBaseFilter`, `IPin`, `IAMStreamConfig`, `IKsPropertySet`, `IClassFactory`) negotiating `ARGB32`, `RGB32`, and `NV12` across 1080p/720p `60`/`30` fps, built for both x64 (`aether_vcam_ds_x64.dll`) and x86 (`build/vcam-x86/Release/aether_vcam_ds_x86.dll`).
- **NDI 6 Dynamic Sender (`§11.4`)**:
  - Implemented [`ndi_output.hpp`](../../src/output/ndi_output.hpp) / [`ndi_output.cpp`](../../src/output/ndi_output.cpp) probing `NDI_RUNTIME_DIR_V6\Processing.NDI.Lib.x64.dll` dynamically (`HR-09` / `HR-10`), sending async double-buffered straight-alpha `BGRA` and `UYVA` (`clock_video = false`), with injectable ABI table support for CI verification.
- **Bidirectional Audio-DSP Shared Memory IPC & Late-Latch Lip Sync (`§11.1`, `REQ-ANM-020`, `PERF-008`)**:
  - Implemented [`dsp_sync_bridge.hpp`](../../src/output/dsp_sync_bridge.hpp) / [`dsp_sync_bridge.cpp`](../../src/output/dsp_sync_bridge.cpp) against the frozen 128-byte `AetherDspSyncPacket` over `Local\AetherDspSyncSharedMem`: EMA (0.1) + `±2 ms/s` slew limit + `[0, 250] ms` clamp + `250 ms` staleness timeout on Cache Line 0 (`T4` writer), and wait-free seqlock reads + `apply_late_latch_visemes` on `T2`/`T3` (paired with smooth 60 ms mouth ownership blending in [`sim_engine.cpp`](../../src/animation/sim_engine.cpp)).
- **OSC Telemetry & Timing Scope Fix (`§11`, `HR-08`)**:
  - Implemented allocation-free WinSock2 UDP OSC telemetry in [`osc_sender.hpp`](../../src/output/osc_sender.hpp) / [`osc_sender.cpp`](../../src/output/osc_sender.cpp) (`/aether/telemetry/latency_ms` per frame, `/aether/telemetry/fps` and `/aether/telemetry/ep` at 4 Hz to `127.0.0.1:9001`).
  - Added per-scope timestamp query reset (`GpuTimingRing::cmd_reset_scope`) in [`gpu_timing.hpp`](../../src/rhi/gpu_timing.hpp), [`gpu_timing.cpp`](../../src/rhi/gpu_timing.cpp), and [`frame_graph.cpp`](../../src/renderer/frame_graph.cpp) so UI-only frames (no avatar loaded) always reset `ViewportCompose` queries cleanly under Vulkan validation.
- **WiX v4 MSI Authoring**:
  - Authored [`AetherStudio.wxs`](../../installer/AetherStudio.wxs) and [`Build-Installer.ps1`](../../installer/Build-Installer.ps1) declaratively registering both x64 and x86 Virtual Camera COM components.

---

## 2. Measured Budgets & Exit-Gate Results

Measured in `windows-relwithdebinfo` (after final code edit) on NVIDIA GeForce RTX 5060 Laptop GPU (`0x10DE`) with Vulkan validation active where enabled by the tests:

| Metric / Gate | Target / Budget | Measured | Status |
|---|---|---|---|
| `tools/ci.ps1` (`windows-debug`) | `21/21` CTest + dual 30f VK validation smoke | `21/21` (`51.94 s`), `vk_errors=0`, `vk_warnings=0`, `hot_path_violations=0` | Pass |
| `tools/ci.ps1` (`windows-relwithdebinfo`) | `21/21` CTest | `21/21` (`29.24 s`) | Pass |
| `tools/ci.ps1` (`windows-release`) | LTO release build clean | Clean exit | Pass |
| `tools/ci.ps1` (`ci-validation`: ASan + GPU-AV + HPG) | `21/21` CTest | `21/21` (`178.35 s`) | Pass |
| Rust gates (`clippy`, `cargo-deny`, `cargo test`) & FFI header | Clean + parity | `8` loader + `3` ffi tests passed; FFI parity passed | Pass |
| Static HR/UI token gates + tabular numerals + `clang-format` + `clang-tidy` | 0 violations / 0 errors | `114` C++ files gated, `53` TUs clean in `clang-tidy` | Pass |
| Spout2 1080p60 publish (`output_spout_vcam_test`) | `p50 <= 0.25 ms` (target `<= 0.2 ms`) | `p50 = 0.185 ms`; `317` assertions / `4` cases passed | Pass |
| DirectShow VCam 1080p60 DMA (`output_spout_vcam_test`) | `p95 <= 1.1 ms` | `p95 = 0.483 ms` | Pass |
| T3 Late-Latch DSP sample-to-GPU write (`PERF-008`, `dsp_sync_osc_test`) | `p95 <= 0.5 ms` | `p95 = 0.151 ms`; `503` assertions / `5` cases (incl. `10^7` cross-process seqlock torture) passed | Pass |
| End-to-End 1080p60 `RT=ON` M2P (`phase5_pipeline_m2p_test`) | `p95 <= 20.0 ms` (`PERF-001`), 0 VK errors | `p50 = 7.396 ms`, `p95 = 8.332 ms`, `max = 8.409 ms` (`spout=0.196 ms`, `vcam=0.896 ms`, `ll=1240 us`); `236` assertions passed | Pass |
| Windowed smoke (`windows-relwithdebinfo`, 30f, no avatar) | `vk_errors=0`, `ui_cpu_p95 <= 0.4 ms` | `vk_errors=0`, `hot_path_violations=0`, `ui_cpu_mean=0.178 ms`, `ui_cpu_p95=0.226 ms` | Pass |
| Windowed smoke (`windows-relwithdebinfo`, 30f, `reference_avatar.vrm`) | `vk_errors=0`, `ui_cpu_p95 <= 0.4 ms` | `vk_errors=0`, `hot_path_violations=0`, `ui_cpu_mean=0.195 ms`, `ui_cpu_p95=0.280 ms` | Pass |

---

## 3. ADRs, Deviations & Open Boundaries (`HR-09`)

- **ADR-0011** ([`0011-output-interop-and-installer-decisions.md`](../adr/0011-output-interop-and-installer-decisions.md)): First-party Spout2 KMT protocol implementation (avoiding legacy `CRITICAL_SECTION`/`Sleep()` in external Spout SDKs), first-party COM `aether_vcam_ds` x64+x86 filters without BaseClasses, runtime-probed NDI 6 ABI declarations, and WiX v4 MSI definition with zero new third-party dependencies (`HR-10`).
- **ASan Hook Note (`ci-validation`)**: In `phase5_pipeline_m2p_test` (matching `rhi_phase4_test` and `output_spout_vcam_test`), `__sanitizer_install_malloc_and_free_hooks` intercepts internal `malloc()` inside `VkLayer_khronos_validation.dll` on `T4` Vulkan submits under ASan; `HotPathGuard::thread_violations() == 0` stays enforced unconditionally around CPU late-latch/update paths under ASan, while process-wide `total_violations() == 0` is enforced on non-ASan builds (`windows-debug` Debug CRT reports `0` violations across `T1`–`T4`).
- **Environment / Human Boundaries (unexecuted on headless CI)**:
  - `Processing.NDI.Lib.x64.dll`: Not installed on host (`NDI_RUNTIME_DIR_V6` unset); runtime cleanly returns `ErrorCode::Unsupported` and logs once (`HR-09`), while async `BGRA` & `UYVA` paths are covered via mock ABI dispatch table tests.
  - WiX v4 CLI (`wix`) is not installed on this host, and EV/OV Authenticode MSI signing requires a human certificate provisioning step.
  - Elevated HKLM COM `regsvr32` registration and manual interactive OBS Studio / Discord desktop capture checks were not executed in unprivileged CI (in-process `DllGetClassObject` + `IAMStreamConfig` + shared-memory frame delivery verified automatically).
  - Phase 6 (`Studio UI & Plugin Node Host`) remains unstarted (`aether_host_*` and production `aether_graph_*` still return `AETHER_E_UNSUPPORTED`, tracked in [`STATUS.md`](../STATUS.md)).
