# AETHER-2.5D Architecture Reference

- **Specification**: `AETHER-TS-001 v1.0`
- **Current Phase**: Phase 1 — Core Scaffold & FFI Contract

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
- `aether_rhi`: `aether_core` + Vulkan 1.3 + VMA. Zero ImGui/UI dependencies.
- `aether_ui`: `aether_core` + Dear ImGui + FreeType. Reads immutable view-models / snapshots only.
- `aether_app`: Orchestrates `aether_core`, `aether_rhi`, `aether_ui`, and `aether_rust_host`.
- `crates/aether-ffi`: Frozen `#[repr(C)]` / `#[repr(C, align(64))]` definitions, status codes, `layout_table.json` generator.
- `crates/aether-rust-host`: Single `staticlib` facade exporting all `extern "C"` functions wrapped in `catch_unwind`.

## Hard Rules Compliance Summary

- **HR-01**: Hot paths (`T1`–`T4` ticks, FFI graph eval) use `FrameArena`, `Pool`, `FixedVector`, `LockFreeTripleBuffer`, `SpscRing`, `ParamMailbox`. Checked in Debug/CI by `AETHER_HOT_PATH_SCOPE`.
- **HR-02**: Zero `std::mutex`, `std::condition_variable`, `SRWLOCK`, `CRITICAL_SECTION`, or Rust `Mutex`/`RwLock` in `src/core`, `rhi`, `tracking`, `animation`, `renderer`, `output`.
- **HR-03**: All FFI structs declared in `crates/aether-ffi`, generated into `include/aether_ffi_contract.h`, asserted at compile time on both sides, and cross-checked with `layout_table.json`.
- **HR-04**: Every `extern "C"` Rust function executes inside `ffi_guard` (`std::panic::catch_unwind` -> `AETHER_E_PANIC`).
