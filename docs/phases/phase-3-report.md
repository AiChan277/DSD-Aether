# Phase 3 Report — NPU Tracking & Visibility-First IK

- **Phase**: 3 (`AETHER-TS-001` §2, §8, §9, §13)
- **Status**: **Complete** (`tools/ci.ps1` exited `0` across all 4 presets + `clang-tidy` + Rust + Vulkan validation)
- **Date**: 2026-11-13

## 1. What Was Built

1. **Multi-Tier ONNX Runtime Execution Provider Router & Model Pipeline (`P3-01`, `P3-02`)**
   - `tools/fetch_phase3_deps.ps1`, `tools/models/tflite_to_onnx.py`, `tools/models/quantize_models.py`, `tools/models/build_models.py`, and `models/manifest.toml`: converts Google MediaPipe Apache-2.0 `.task`/`.tflite` models (`M0` face detector, `M1_body` pose lite, `M1_hands` batched `2x3x224x224`, `M2` 478-point face landmarks, `M3` 52-weight ARKit blendshapes) to static-shape opset-17 ONNX (`NCHW`), runs 500-frame calibration static per-channel INT8 quantization + FP16 conversion with automated `NME delta <= 3.0%` gate, and pins SHA-256 digests in `models/manifest.toml`.
   - `src/tracking/manifest.{hpp,cpp}` & `src/tracking/ort_router.{hpp,cpp}`: verifies model SHA-256 digests prior to loading; implements 6-tier fallback chain (`TensorRT -> OpenVINO NPU -> OpenVINO iGPU GPU.0 -> DirectML iGPU -> DirectML dGPU -> CPU`), pre-allocates all input/output buffers and `Ort::Value` bindings once at `initialize()` for zero heap allocation in `run_*()` (`HR-01`), enforces a fixed 512-slot rolling latency ring (120-frame window `> 9.0 ms` auto-demotion), and maps `enable_npu_fast_compile` via OpenVINO `load_config`.
2. **Capture, AVX2 Pre-Processing & 3D Landmark / Procrustes Solver (`P3-03`, `P3-04`)**
   - `src/tracking/camera_source.hpp`, `src/tracking/camera_mf.cpp`, `src/tracking/file_camera.cpp`: Media Foundation async NV12/MJPG/YUY2 camera capture + synthetic MP4/deterministic camera playback keeping all camera frames off disk (`HR-12`).
   - `src/tracking/roi_preprocess.{hpp,cpp}`: AVX2-accelerated BT.601 NV12->NCHW planar RGB crop/rotate/normalize pre-processing for body (`256x256`), face (`256x256`), and batched dual-hand (`2x224x224`) ROIs, plus 5% border margin and `>= 0.30` confidence gating (`compute_hand_rois_from_pose`).
   - `src/tracking/canonical_face_model.hpp`, `src/tracking/landmark_solver.{hpp,cpp}`, `src/tracking/tracking_engine.{hpp,cpp}`: BlazeFace anchor decoder, 468-point weighted Procrustes Kabsch head pose solver + IRIS gaze extraction, body/hand world-landmark solver, hip-pivot yaw estimation, and `TrackingEngine` publishing `AetherTrackingFrame` (2048 B) into `LockFreeTripleBuffer` on `T1`.
3. **240 Hz Animation Engine: Filters, Visibility-First Solver, IK & AVX2 SpringBones (`P3-05`, `P3-06`, `P3-07`)**
   - `src/animation/math_quat.hpp`, `src/animation/one_euro.{hpp,cpp}`, `src/animation/kalman.{hpp,cpp}`: 1-Euro adaptive scalar/quaternion filters and 6-DOF constant-velocity Kalman predictor for zero-overshoot `<= 1` tracking-frame lag.
   - `src/animation/visibility_first.{hpp,cpp}` & `src/animation/ik_solvers.{hpp,cpp}`: C1 weight blending (`|dw/dt| <= 4.0 / s`), procedural hands-under-desk rest attractors, hip sway (`k_sway = 0.35`), automatic foot planting (`12 +- 1 cm` trigger, `0.35 s` parabola, `4 cm` apex), analytical 2-bone limb IK with 98% soft extension clamping + Appendix C joint limits, and spine/neck FABRIK.
   - `src/animation/expression_mixer.{hpp,cpp}`: sparse ARKit->VRM expression weight table `W`, auto-blink, and `60 ms` camera-vs-DSP mouth ownership cross-fade.
   - `src/animation/spring_bone.{hpp,cpp}` & `src/animation/sim_engine.{hpp,cpp}`: AVX2 SoA 8-wide Verlet SpringBone integration against sphere/capsule colliders with VRM 1.0 center-bone inertia + `> 0.5 m` teleport reset, and full `T2` tick publishing `SkinningFrame` into `aether_studio` (`application.cpp`).
4. **Live `T1 -> T2 -> T3` Studio Integration & Preview Skinning**
   - `src/app/pipeline_skeleton.{hpp,cpp}`, `src/renderer/asset_upload/gpu_avatar.cpp`, `src/renderer/preview_pass.cpp`, `src/ui/dev_shell.cpp`: live dual-buffer `LockFreeTripleBuffer<SkinningFrame>` CPU skinning upload for `PreviewPass`, skeleton rest-anchor calibration via `SimEngine::set_rest_metrics`, and NPU/EP status + avatar bone line readout in Dev Shell.

## 2. Measured Performance & Budgets (`windows-relwithdebinfo`)

| Metric / Gate | Target Budget | Measured | Source |
|---|---:|---:|---|
| `T1` OpenVINO NPU `M2_face + M3_bs` p95 | `<= 6.0 ms` | **`3.3455 ms`** (`M2=2.0949, M3=1.2778`) | `tracking_router_test.exe [perf]` |
| `T1` OpenVINO NPU `M1_body + M2 + M3` p95 (13 TOPS) | `<= 12.0 ms` | **`7.4369 ms`** (`M1_body=3.8029`) | `tracking_router_test.exe [perf]` |
| Tier 2A OpenVINO iGPU fallback (`GPU.0` LATENCY) | `p50<=15 / p95<=35 ms` | **`p50=6.5039 ms`, `p95=8.5805 ms`** | `tracking_router_test.exe [perf]` |
| `NV12->RGB` AVX2 ROI preprocess (`256x256 + 224x224`) | `<= 0.60 ms` | **`0.2371 ms`** (`max_diff=6.08e-5`) | `tracking_preprocess_test.exe` |
| 1-Euro static jitter (`pos` / `rot`) | `<= 1.0 mm` / `<= 0.15 deg` | **`0.9765 mm`** / **`0.0086 deg`** | `animation_filters_ik_test.exe` |
| 1-Euro + Kalman 2 Hz dynamic lag | `<= 20.0 ms` (`<= 1` frame) | **`16.67 ms`** | `animation_filters_ik_test.exe` |
| Visibility-First occluded wrist RMS / step trigger | `<= 5.0 mm/s` / `12 +- 1 cm` | **`1.6145 mm/s`** / **`11.75 cm`** | `animation_filters_ik_test.exe` |
| `SpringBone` 512 joints x 32 colliders AVX2 parity | `<= 1e-5 m` delta | **`0.0 m`** delta | `spring_bone_simd_test.exe` |
| Full `SimEngine::tick` (512 spring joints, 0 allocs) | `<= 0.80 ms` | **`0.0522 ms`** | `spring_bone_simd_test.exe` |
| GPU avatar upload (`reference_avatar.vrm`) | `<= 50.0 ms` | **`24.06 ms`** (`48.68 MiB` staging) | `asset_upload_vk_test.exe` |
| Windowed Vulkan validation smoke (60 frames + VRM) | `0` errs, UI `<= 0.40 ms` | **`0` vk/alloc errs, `0.165 ms` mean** | `aether_studio.exe --smoke-frames 60` |

## 3. CI Verification (`tools/ci.ps1`)

- Static HR & UI token gates (`73` C++ files, Rust crates, dependency allowlist, `font_tnum_freeze.py`): **Passed**.
- `clang-format --dry-run --Werror` & `clang-tidy` (all `35` C++ TUs under `src/`): **Passed (0 errors)**.
- Rust (`cargo fmt --check`, `cargo clippy` default & `--all-features`, `cargo deny check`, `cargo test` `8/8` + `3/3` + `4/4`) & `cbindgen` parity: **Passed**.
- CMake presets: `windows-debug` (`14/14` CTest + `2x 30`-frame Vulkan validation smoke), `windows-relwithdebinfo` (`14/14` CTest), `windows-release` (LTO build), `ci-validation` (ASan + GPU-assisted Vulkan validation + `HotPathGuard`, `14/14` CTest). **All passed, exit code `0`.**

## 4. ADRs & Technical Findings / Deviations

- **`ADR-0007`**: Pinned prebuilt ONNX Runtime `1.24.1` + OpenVINO EP; mapped `enable_npu_fast_compile` through OpenVINO JSON `load_config` because ORT 1.24.1 rejects the deprecated boolean option; configured `GpuOpenVino` (`GPU.0`) with `load_config={"GPU":{"PERFORMANCE_HINT":"LATENCY"}}`.
- **`ADR-0008`**: Selected Apache-2.0 MediaPipe `.task`/`.tflite` models, opset-17 `NCHW` static conversion, 500-frame INT8 calibration with `<= 3%` NME delta gate, and runtime SHA-256 verification.
- **DirectML Odd-Byte FP16 Tensor ASan Fix (`manifest.cpp`)**: ORT `1.24.1` DML EP performs 4-byte-aligned constant upload copies, causing a 2-byte OOB read (`390 -> 392`) under MSVC ASan on odd-half-count FP16 tensors (`[1, 195]`, `[2, 63]`). Updated `verify_and_resolve_model_path()` to select 4-byte-aligned `fp32` model variants for `DirectMlIgpu`/`DirectMlDgpu` while keeping `fp16` for OpenVINO iGPU and TensorRT.
- **Hot-Path Cleanup**: Replaced `std::deque<double>` in `ort_router.cpp` latency health tracking with a fixed `std::array<double, 512>` ring (`HR-01`); added `HOST_ACCESS_RANDOM_BIT` + zero-copy rest buffer init fixes in Phase 2 GPU upload path (`asset_job.cpp`, `gpu_avatar.cpp`).

## 5. Open Stubs Carried Forward

- **Phase 4 (`Renderer & RT`)**: Production Vulkan 2.5D NPR compute skinning, MToon/outline/rim-light passes, hardware RT shadows/AO/reflections, and FSR3/XeSS upscaling (current renderer draws loaded avatar meshes via Phase 2 `PreviewPass`).
- **Phase 5 (`Output & DSP`)**: Spout2, Virtual Camera, OSC/VMC IPC, and audio-DSP lip-sync input beyond test stubs.
- **Phase 6 (`Studio UI & Plugins`)**: Full 5-zone docked Studio UI (`Live Studio` / `Rigging` / `Nodes`) and `crates/aether-plugin-host` WASM/node execution (`aether_host_*` and `aether_graph_*` remain `AETHER_E_UNSUPPORTED` in production builds; tracked in `docs/STATUS.md`).
