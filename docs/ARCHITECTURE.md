# AETHER-2.5D Architecture Reference

- **Specification**: `AETHER-TS-001 v1.0`
- **Current Release**: `a1.0.3B` — Phases 1–6 Complete + DX11/DX12, Photometric Studio Lighting, Lighting Workspace, Native PMX 2.0/2.1, & WiX v5 MSI Installer

## Thread & Channel Topology (§2.2, §4.1)

| Thread | Priority / Placement | Role | Inter-Thread Channels |
|---|---|---|---|
| **T0 Main** | `NORMAL`, OS default | Win32 `HWND`, message pump, `WM_DEVICECHANGE`, `WM_DPICHANGED` | `InputEventRing` (`SpscRing<InputEvent, 1024>`) -> T3 |
| **T1 Vision** | `HIGHEST` (+ MMCSS Capture in Phase 3) | Synthetic frame producer in Phase 1; MF + ORT in Phase 3 | `PoseRingBuffer` (`LockFreeTripleBuffer`) -> T2 + `PoseReady` auto-reset event |
| **T2 Sim & IK** | `HIGHEST`, pinned P-core 1 | Fixed 240 Hz + `PoseReady` event tick; filters/IK/SpringBone in Phase 3 | `SkinningBuffer` (`LockFreeTripleBuffer`) -> T3 |
| **T3 Render & UI** | `HIGHEST`, pinned P-core 2 | Phase-locked frame timer (`t_post + 0.2 ms`), late-latch acquire, Vulkan 1.3 + ImGui | `OutputRing` (`LockFreeTripleBuffer`) -> T4 + `frame_tl` timeline semaphore |
| **T4 Output** | `ABOVE_NORMAL`, any P-core | Waits on `frame_tl`, stamps `present_us`; Spout2/VCam/NDI/DSP in Phase 5 | — |
| **Workers** | `BELOW_NORMAL`, E-cores | Logger ring drain + rolling file sink, crash dump thread, async tasks | Per-thread `SpscRing<LogRecord, 1024>` |

## Module Layering (§2.4)

- `aether_core`: Win32 + C++20 stdlib (+ Tracy client when enabled). Zero dependencies on other project modules.
- `aether_rhi`: `aether_core` + Vulkan 1.3 + VMA + D3D12 device. Zero ImGui/UI dependencies.
- `aether_tracking`, `aether_animation`, `aether_renderer`, `aether_output`: `aether_core` (+ `aether_rhi` for renderer/output). Zero blocking locks (`HR-02`).
- `aether_ui`: `aether_core` + Dear ImGui + ImPlot + ImGuizmo + FreeType across Live Studio, Rigging, Lighting, and Nodes workspaces. Reads immutable view-models / snapshots only.
- `aether_app`: Orchestrates `aether_core`, `aether_rhi`, `aether_tracking`, `aether_animation`, `aether_renderer`, `aether_output`, `aether_ui`, `aether_native_plugin_manager`, and `aether_rust_host`.
- `aether_vcam_ds_{x64,x86}` & `aether-plugin-runner`: Standalone COM DirectShow virtual camera filters and sandboxed native plugin worker process.
- `crates/aether-ffi`: Frozen `#[repr(C)]` / `#[repr(C, align(64))]` definitions, status codes, `layout_table.json` generator.
- `crates/aether-asset-pmx`: Bounds-checked pure-Rust PMX 2.0 / 2.1 binary parser (SDEF, CCD-IK, Append Transform, 5 morph types, RigidBody + 6-DOF constraints).
- `crates/aether-asset-loader`, `crates/aether-plugin-host`, `crates/aether-rust-host`: Unified VRM/glTF/FBX/PMX loader, node-graph compiler/evaluator, wasmtime sandbox, and single `staticlib` facade exporting all `extern "C"` functions wrapped in `catch_unwind`.

## Hard Rules Compliance Summary

- **HR-01**: Hot paths (`T1`–`T4` ticks, FFI graph eval) use `FrameArena`, `Pool`, `FixedVector`, `LockFreeTripleBuffer`, `SpscRing`, `ParamMailbox`. Checked in Debug/CI by `AETHER_HOT_PATH_SCOPE`.
- **HR-02**: Zero `std::mutex`, `std::condition_variable`, `SRWLOCK`, `CRITICAL_SECTION`, or Rust `Mutex`/`RwLock` in `src/core`, `rhi`, `tracking`, `animation`, `renderer`, `output`.
- **HR-03**: All FFI structs declared in `crates/aether-ffi`, generated into `include/aether_ffi_contract.h`, asserted at compile time on both sides, and cross-checked with `layout_table.json`.
- **HR-04**: Every `extern "C"` Rust function executes inside `ffi_guard` (`std::panic::catch_unwind` -> `AETHER_E_PANIC`).
