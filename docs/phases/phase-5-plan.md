# Phase 5 Plan — Transparent Video Output & Audio-DSP IPC

- **Specification Reference**: `AETHER-TS-001 v1.0` §1.3 (`PERF-001`, `PERF-005`, `PERF-008`), §2.2 (`T4`), §5.2 (`AetherDspSyncPacket`), §7.8 (`OSC :9001`), §9.7 (`REQ-ANM-020`), §11 (`11.1–11.5`), §13 (Phase 5), §14.4, Appendix D.
- **Prerequisite**: Phase 4 complete and CI-green (`tools/ci.ps1` exit code `0` across all 4 presets + `clang-tidy`).

## 1. Objectives & Task Breakdown

| Task ID | Area | Deliverable | Spec Ref |
|---|---|---|---|
| `P5-01` | Output Resolution/Rate & `OutputRing` | Decouple `OUT_*` render resolution (`1280x720`, `1920x1080`, `2560x1440` <=120 Hz, `3840x2160` <=60 Hz; default `1920x1080@60`) from viewport panel size; add 3-slot `OutputRing` handoff (`T3` encodes & signals `frame_timeline_`, `T4` consumes slot `N` concurrently while `T3` renders `N+1`); upgrade Pass 12 for `#00B140`/black/custom `OUT_NV12` background composite, straight-alpha BGRA / `OUT_UYVA` (`UYVY + A8`), and conditional bottom-up BGRA copy when a DirectShow RGB pin is active. | §11.1, §10.2, §10.10 |
| `P5-02` | Spout2 / DXGI Shared Texture (`T4`) | First-party zero-lock `SpoutOutput` (`src/output/spout_output.{hpp,cpp}`): LUID-matched `ID3D11Device` (`IDXGIFactory4::EnumAdapterByLuid`), `DXGI_FORMAT_B8G8R8A8_UNORM` shared KMT resource imported into Vulkan (`VK_KHR_external_memory_win32`, `D3D11_TEXTURE_KMT`), `VK_QUEUE_FAMILY_EXTERNAL` release/acquire barriers + `output_timeline_`, `[Sender]_SpoutAccessMutex` (`1 ms` timeout, skip & count on contention), Spout2 `SpoutSenderNames` + 280 B `SharedTextureInfo` publication, CPU fallback mode, and hybrid iGPU/dGPU display adapter mismatch detection. Target `<= 0.2 ms`. | §11.2, `PERF-005`, ADR-0011 |
| `P5-03` | DirectShow Virtual Camera DLL (`x64` & `x86`) + DMA Transport | Frozen CLSID (`src/vcam/guids.h`), `"Aether 2.5D Virtual Cam (Alpha)"` COM in-process source filter (`aether_vcam_ds_x64.dll` + `aether_vcam_ds_x86.dll`) offering `MEDIASUBTYPE_ARGB32`, `MEDIASUBTYPE_RGB32`, `MEDIASUBTYPE_NV12` at `1920x1080`/`1280x720` (`60`/`30` fps), embedded `"Aether is not running"` idle frame; `Local\AetherVCam_v1` 3-slot seqlock transport (`VcamTransport`) using Vulkan DMA transfer queue with `VK_EXT_external_memory_host` import (readback fallback) within `<= 1.1 ms`. | §11.3, `PERF-005`, App D, ADR-0011 |
| `P5-04` | NDI 6 Network Stream | `NdiOutput` dynamically loading `Processing.NDI.Lib.x64.dll` via `NDI_RUNTIME_DIR_V6`, sender `"Aether 2.5D (<hostname>)"`, `clock_video = false`, straight-alpha `NDIlib_FourCC_type_BGRA` and `NDIlib_FourCC_type_UYVA` via `NDIlib_send_send_video_async_v2` with double-buffered host frames (clean `Unsupported` fallback + injectable dispatch table for CI tests). | §11.4, App D, `HR-09`, ADR-0011 |
| `P5-05` | `DspSyncBridge`, `REQ-ANM-020` Late-Latch & OSC `:9001` | `Local\AetherDspSyncSharedMem` (`4096 B`, `AetherDspSyncPacket` 128 B at offset 0), Cache Line 0 seqlock writer (`EMA_0.1` smoothed delay `clamp(..., 0, 250)` slew `+-2 ms/s`, 120f `p95-p50` jitter), 4-try Cache Line 1 reader (`250 ms` stale cutoff); zero-alloc WinSock2 UDP OSC sender (`127.0.0.1:9001`) emitting `/aether/telemetry/latency_ms` per frame + `fps`/`ep` at `4 Hz`; `T2` 60 ms `mouth_owner_blend` + `T3` late-latch `PERF-008 <= 0.5 ms` (`<= 10 us` target) morph application; `docs/DSP_PROTOCOL.md` + C11 reference reader/writer. | §11.5, §9.7, §7.8, `PERF-008`, App D |
| `P5-06` | WiX Installer, Live T4 Integration & Exit-Gate Tests | `installer/AetherStudio.wxs` + build script packaging binaries and registering x64/x86 Virtual Cam COM filters; wire `T4` into `PipelineSkeleton` / `Application` + `DevShell` Phase 5 Output/DSP telemetry panel; pass all Phase 5 automated tests (`Spout2` D3D11 pixel probe & `<= 0.2 ms`, `VCam` COM negotiation + x64/x86 builds + `<= 1.1 ms` DMA, `NDI` straight/UYVA async send, `10^7` two-process seqlock torture test, OSC `:9001`, `PERF-008`, and `T1->T2->T3->T4` M2P harness `p95 <= 20.0 ms`). | §13 Phase 5, §14.4 |

## 2. Planned File Layout

- `docs/phases/phase-5-plan.md`, `docs/adr/0011-output-interop-and-installer-decisions.md`, `docs/DSP_PROTOCOL.md`
- `src/output/`:
  - `CMakeLists.txt`
  - `output_config.hpp`, `output_ring.hpp`
  - `spout_output.{hpp,cpp}`
  - `vcam_transport.{hpp,cpp}`
  - `ndi_output.{hpp,cpp}`
  - `osc_sender.{hpp,cpp}`
  - `dsp_sync_bridge.{hpp,cpp}`
  - `output_coordinator.{hpp,cpp}`
- `src/vcam/`:
  - `CMakeLists.txt`, `guids.h`, `vcam_shm.h`, `aether_vcam_ds.def`, `aether_vcam_ds.cpp`
  - `tools/build_vcam_x86.ps1` (compiles x86 `aether_vcam_ds_x86.dll` via `vcvarsamd64_x86.bat`)
- `src/renderer/` & `shaders/`:
  - Extended `frame_graph.{hpp,cpp}`, `npr_renderer.{hpp,cpp}`, `aether_post_bloom.slang`, `aether_viewport_compose.slang` (decoupled output size, 3-slot `OutputRing`, `NV12` non-alpha BG, `UYVA`/straight alpha, bottom-up copy, zero-alloc late-latch DSP viseme blend in `application.cpp` / `pipeline_skeleton.*`)
- `installer/`:
  - `AetherStudio.wxs`, `Build-Installer.ps1`
- `tests/cpp/` & `tests/`:
  - `dsp_torture_worker.c` (C11 reference reader/writer & separate-process `10^7` torture helper)
  - `output_spout_vcam_test.cpp`
  - `dsp_sync_osc_test.cpp`

## 3. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `check_gates.py` forbids `std::mutex`, `CRITICAL_SECTION`, `SRWLOCK`, `Sleep` across `src/output/`. | Use lock-free atomics/seqlocks everywhere in `src/output/` and Spout's kernel `CreateMutexA`/`WaitForSingleObject(..., 1)`/`ReleaseMutex` with explicit `1 ms` skip-on-timeout semantics (`§11.2.3`). |
| Vulkan validation `sync2` / external memory hazards on Spout DXGI KMT shared texture. | Use `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT` with dedicated memory allocation, import with `VK_IMAGE_TILING_OPTIMAL`, transition `UNDEFINED/GENERAL <-> TRANSFER_DST_OPTIMAL -> GENERAL` with `VK_QUEUE_FAMILY_EXTERNAL` ownership release before `output_timeline_` wait. |
| Hybrid laptop adapter mismatch when OBS opens Spout on iGPU vs Aether rendering on dGPU. | Query `DXGI_ADAPTER_DESC1` + `IDXGIOutput` display attachment matching Vulkan LUID and expose `SpoutSharingMode::GpuDefault` / `CpuCompatibility` plus one-time UI hint (`§11.2.4`). |
| `T4` reading `OUT_*` while `T3` overwrites `OUT_*` at `120+ fps`. | Expand `FrameGraph` output buffers (`OUT_BGRA8`, `OUT_NV12`, optional `OUT_BOTTOM_UP`) to 3 ring sets synchronized by `frame_timeline_` monotonically (`T4` waits `frame_tl >= N` while `T3` renders into slot `(N+1)%3`). |

## 4. Exit-Gate Tests to Write First

1. `output_spout_vcam_test.cpp`:
   - `FrameGraph` decoupled output (`1920x1080`) vs viewport (`1280x720`) + `OUT_NV12` `#00B140` chroma background composite + bottom-up orientation & UYVA straight-alpha verification.
   - `SpoutOutput` real D3D11/DXGI KMT receiver probe (zero `vk_errors`/`vk_warnings`, premultiplied BGRA pixel probe delta == `0`, publish `<= 0.2 ms` mean/p95 in Release/RelWithDebInfo, bounded `1 ms` mutex timeout skip test).
   - `VcamTransport` + `aether_vcam_ds_x64.dll` COM `DllGetClassObject` instantiation, `IAMStreamConfig` negotiation (`ARGB32`, `RGB32`, `NV12` at `1080p`/`720p` `60`/`30` fps), embedded offline fallback bitmap check, live frame sample delivery via `Local\AetherVCam_v1`, and DMA `<= 1.1 ms` budget.
   - `NdiOutput` mock/loopback async double-buffered `BGRA` (straight alpha) and `UYVA` send verification (`clock_video = false`).
2. `dsp_sync_osc_test.cpp`:
   - C11 `dsp_torture_worker.exe` spawned cross-process executing `10,000,000` concurrent seqlock reads/writes on `Local\AetherDspSyncSharedMem` with zero torn reads.
   - `DspSyncBridge` EMA_0.1, `+-2 ms/s` slew limit, `[0, 250] ms` clamp, `250 ms` staleness timeout, and `REQ-ANM-020` / `PERF-008` late-latch upload `< 0.5 ms` (`<= 10 us` morph step).
   - WinSock2 UDP loopback capture validating `/aether/telemetry/latency_ms` per frame (`f32`) + `fps`/`ep` at 4 Hz (`127.0.0.1:9001`, 0 heap allocs).
   - Integrated `T1 (synthetic file camera) -> T2 -> T3 -> T4 (Spout)` latency measurement verifying `PERF-001` (`M2P <= 20.0 ms`).
