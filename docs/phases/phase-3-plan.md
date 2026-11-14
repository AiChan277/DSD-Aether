# Phase 3 Implementation Plan: NPU Tracking & Visibility-First IK

- **Phase**: 3 of 6 (`AETHER-TS-001 v1.0` §8, §9, §13 Phase 3)
- **Prerequisites**: Phase 1 & Phase 2 complete (`tools/ci.ps1` green, all FFI & asset-loader gates closed)
- **ADRs**: `docs/adr/0007-onnxruntime-and-ep-distribution.md`, `docs/adr/0008-tracking-models-and-licences.md`

---

## 1. Scope & Target Architecture

Phase 3 delivers the live & file-replay **T1 AI Tracking Engine** (`src/tracking/`) and **T2 Simulation, Visibility-First IK, Filters, SpringBone & Expression Mixer** (`src/animation/`), plus Dev Shell camera preview & confidence HUD (`src/ui/`, `src/app/`, `src/renderer/` CPU preview skinning bridge until Phase 4 compute skinning).

### Module Layering (`§2.4` Strict)
- `src/tracking/` -> depends ONLY on `core` + ONNX Runtime + Media Foundation (+ D3D12/DirectML for EP binding). Must NOT depend on `renderer`, `ui`, or `output`.
- `src/animation/` -> depends ONLY on `core` + `aether_rust_host` (`aether_graph_evaluate`) + internal math/AVX2. Must NOT depend on `renderer`, `ui`, or `tracking` internals (consumes `AetherTrackingFrame` only).
- `T1 -> T2` channel -> `LockFreeTripleBuffer<AetherTrackingFrame>` + auto-reset Win32 `PoseReady` event (`HR-02`).
- `T2 -> T3` channel -> `LockFreeTripleBuffer<SkinningFrame>` (updated Phase 1/2 `SkinningSnapshot` to match `§9.8` full `SkinningFrame` layout with 1024 bone matrices + 1024 morph weights + `mouth_owner_blend` + NPR head pivot/dirs).

---

## 2. Work Breakdown & Files

### P3-01 · Toolchain, Model Manifest & Hardware NPU Probe
- `tools/fetch_phase3_deps.ps1`: Downloads/verifies `ort-1.24.1` (`DirectML` + `OpenVINO 2025.4.1`) into `%LOCALAPPDATA%\Aether\toolchains\ort-1.24.1` and ensures MediaPipe ONNX models under `models/` match `models/manifest.toml`.
- `tools/models/tflite_to_onnx.py`, `tools/models/quantize_models.py`, `tools/models/build_models.py`: Converts Apache-2.0 MediaPipe `.tflite` models to static NCHW ONNX opset 17 (`M0 1x3x128x128`, `M1_body 1x3x256x256`, `M1_hands 2x3x224x224`, `M2 1x3x256x256`, `M3 1x146x2`), calibrates static per-channel INT8 on 500 synthetic calibration frames, enforces `<= 3%` NME delta vs FP32, writes `models/manifest.toml` (with SHA-256, shapes, opset, normalisation, tier variants).
- `src/core/hw_enum.cpp` + `src/core/hw_enum.hpp`: Implements real NPU detection via DXCore (`IDXCoreAdapterFactory`) / SetupDi PnP (`ComputeAccelerator`, `PCI\VEN_8086&DEV_AD1D`) so `hw.npu.probed = true`, `ffi_status = AETHER_OK`; removes `HardwareInventory::npu` stub from `docs/STATUS.md`.

### P3-02 · Media Foundation Async Camera & Deterministic File Camera (`§8.1`, `HR-12`)
- `src/tracking/camera_source.hpp` / `camera_mf.cpp` / `file_camera.cpp`
  - `LatestFrameSlot` (lock-free 2-entry triple/atomic exchange holding latest `IMFSample*` reference + QPC capture timestamp; `IMF2DBuffer2::Lock2DSize` zero-copy pixel view, NV12/YUY2/BGRA -> NV12/RGB uniform descriptor).
  - Live `MfCameraSource`: `IMFSourceReader` in `MF_SOURCE_READER_ASYNC_CALLBACK` with `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`, format negotiation order `1280x720@60 NV12/YUY2` -> `1920x1080@60` -> `MJPEG`. Exposes `IAMCameraControl` / `IAMVideoProcAmp` (exposure lock, anti-flicker 50/60 Hz, WB lock) + per-device symbolic link settings store (no frames ever serialized or leaves machine, `HR-12`).
  - Deterministic `FileCameraSource`: reads MP4/H.264/WMV via MF `IMFSourceReader` (or deterministic synthetic MP4 generated during tests) replaying frames paced to original presentation timestamps, plus in-memory synthetic frame feed for unit/property tests.

### P3-03 · Static ROI & AVX2 Pre-Processor (`§8.2`)
- `src/tracking/roi_preprocess.hpp` / `roi_preprocess.cpp`
  - Face ROI: square `1.6x` expansion from prev frame face landmarks, EMA smoothed, roll-aligned to eye line; M0 reset path sets `valid_flags |= AETHER_TRACK_FLAG_ROI_RESET` (bit 4).
  - Body ROI: derived from previous shoulders + hips (`M0` fallback reset when body lost).
  - Hand ROI pair: left/right wrist crop boxes with `5%` frame margin & `arm_conf >= 0.30` gate (`§8.5`, Appendix C).
  - AVX2 kernel (`_mm256_*` with scalar fallback): affine bilinear warp from NV12 (Y + UV) / YUY2 / BGRA directly into pre-bound NCHW planar float32 buffer `[N, 3, H, W]` with normalisation folded in (`<= 0.6 ms` budget, zero heap allocations).

### P3-04 · Dynamic ORT Router, Zero-Alloc `Ort::IoBinding` & Health Monitor (`§8.4`, `REQ-TRK-001/002/003`)
- `src/tracking/manifest.hpp` / `manifest.cpp`: Parses `models/manifest.toml` cleanly, verifies SHA-256 on startup before loading ORT sessions.
- `src/tracking/ort_router.hpp` / `ort_router.cpp`:
  - `AetherTrackingRouter` owns one `Ort::Env`, global thread pool, pre-allocated input/output CPU staging buffers, `Ort::IoBinding`, and reused `Ort::RunOptions` (`REQ-TRK-001`: 0 allocations per inference).
  - Tier selection:
    - Tier 1 (`AETHER_EP_INTEL_NPU`): OpenVINO EP `device_type="NPU"`, `cache_dir=%LOCALAPPDATA%\Aether\cache\ov_cache`, mapped fast-compile `load_config` (logged per `§8.4`), INT8 models (with FP16 fallback if needed). Warm-up 10 dummy runs. Budget `p95 <= 6.0 ms`.
    - Tier 2A (`AETHER_EP_OPENVINO_GPU` / `AETHER_EP_DIRECTML_GPU`): iGPU with DXGI LUID != render dGPU LUID; FP16 models; wizard benchmark check `p95 <= 6.0 ms`.
    - Tier 2B (`AETHER_EP_NVIDIA_TRT` / `AETHER_EP_DIRECTML_GPU`): TensorRT EP if runtime DLLs present (high-priority CUDA stream, 256 MB workspace, TRT engine cache) else DirectML EP on dGPU with `D3D12_COMMAND_QUEUE_PRIORITY_HIGH`, `DisableMemPattern`, sequential execution, `IDXGIAdapter3::QueryVideoMemoryInfo` `<= 256 MB` cap, FP16 models.
    - Tier 3 (`AETHER_EP_CPU`): OpenVINO `CPU` / ORT `CPU` EP, worker threads pinned to E-cores / secondary SMT via `SetThreadAffinityMask`, INT8 models, relaxed budget `p95 <= 9.0 ms` (body every 2nd frame, hands every 3rd).
  - Health monitor (`REQ-TRK-002`, `REQ-TRK-003`): 120-frame ring EMA + p95 per frame total; 120 consecutive frames over tier budget or session error demotes one tier, emits amber/red telemetry status + toast event. User forced-tier override still demotes cleanly on error.

### P3-05 · Landmark -> Joint Solving, Kabsch Head Fit, Calibration & T1 Publish (`§8.6`, `§8.7`)
- `src/tracking/canonical_face_model.hpp`: 468 metric vertices + Procrustes weights + 146 M3 landmark subset indices + BlazeFace/Pose anchors.
- `src/tracking/landmark_solver.hpp` / `landmark_solver.cpp`:
  - M0 BlazeFace decoder (NMS + anchors -> face bounding box + eye centers).
  - M2 478 face + iris landmarks -> Kabsch/Procrustes `3x3` head R & `t` (SVG/eigen/Jacobi 3x3 SVD, zero alloc!); low body conf splits rotation 60% head / 30% neck / 10% upperChest.
  - Iris gaze yaw/pitch -> `leftEye` / `rightEye` quaternions synced with `eyeLook*`.
  - M1 body (33 world landmarks) -> swing-twist local rotations in canonical Appendix A T-pose + forearm twist.
  - M1 hands (`2x21` keypoints) -> finger flexion & palm-plane abduction, clamped to Appendix C limits (`0..100 deg`).
  - Per-joint confidence = `model_vis * roi_blur_quality * bone_length_plausibility`; region confidences (`upper_body`, `left_hand`, `right_hand`, `lower_body`).
  - Calibration state: 3 s capture averaging neutral head pose, shoulder width, camera pitch/height estimate, per-blendshape neutral offsets & ranges; ARKit-52 remapping & curves.
- `src/tracking/tracking_engine.hpp` / `tracking_engine.cpp`: Runs on `T1`, owns `LatestFrameSlot` consumer, ROI+AVX2 preprocess, `AetherTrackingRouter`, `LandmarkSolver`, publishes into `LockFreeTripleBuffer<AetherTrackingFrame>`, signals `PoseReady` (`HR-01`, `HR-02`).

### P3-06 · T2 Filters, Kalman, Visibility-First IK, Analytical/FABRIK IK, SIMD SpringBone & Expression Mixer (`§9`)
- `src/animation/math_quat.hpp`: Fast inline vec3/quat/mat3/mat4 helpers, tangent log/exp, hemisphere dot guard, swing-twist, Appendix A & C constants.
- `src/animation/one_euro.hpp` / `one_euro.cpp`: Scalar + tangent-space quaternion 1-Euro filter (`fc_min = 1.2 Hz`, `beta = 0.45`, `dcutoff = 1.0 Hz`; eyes `beta = 0.8`, mouth `fc_min = 2.0 Hz`, fingers `fc_min = 1.0 Hz`; `Te = dt_capture`).
- `src/animation/kalman.hpp` / `kalman.cpp`: Constant-velocity 2-state scalar Kalman + quaternion angular-velocity extrapolator (`horizon = clamp(t_render - t_capture, 0, 14 ms)`, `sigma_a = 8 m/s^2`, `sigma_m = 2 mm / 0.02`, `R = sigma_m^2 / max(c, 0.05)^2`, `c < 0.30` damps `v *= 0.85`, disabled on `roi_reset`).
- `src/animation/visibility_first.hpp` / `visibility_first.cpp`: Hermite `w_i = t^2 (3 - 2t)` over `[0.30, 0.75]`, slew rate limit `4.0 / s` (`>= 250 ms` transition), procedural idle attractors (thigh / virtual desk per side) with `0.15 s` clavicle lag, `0.25 Hz` breathing (`+-1.2 deg` chest, `4 mm` shoulder lift, `6 mm` attractor lift), critically damped spring (`wn = 2*pi*1.5, zeta=1`), hip sway (`k_sway = 0.35`), auto foot planting (`step threshold 0.12 m`, `0.35 s` parabola, `0.04 m` apex).
- `src/animation/ik_solvers.hpp` / `ik_solvers.cpp`: Law-of-cosines 2-bone IK (98% soft limit, preserved end-effector rotation, elbow back-down / knee forward pole hints), spine-neck FABRIK (`<= 4` iters, `1 mm` tol, cone limits), Appendix C joint clamping (elbow `0..150 deg`, knee `0..155 deg`, neck yaw `+-80`, pitch `-60/+70`, fingers `0..100 deg`).
- `src/animation/spring_bone.hpp` / `spring_bone.cpp`: SoA layout grouped by depth level, scalar reference + **AVX2 8-wide** (plus AVX-512 16-wide runtime-dispatched path when supported by CPU) matching scalar within `<= 1e-5 m`; `u64` collider group bitmask; sphere + capsule colliders; VRM `spring center` node; `> 0.5 m` teleport reset; `<= 0.4 ms` for 512 joints x 32 colliders (`REQ-ANM-010`).
- `src/animation/expression_mixer.hpp` / `expression_mixer.cpp`: Sparse `W * input` matrix multiply + preset fades + 60 ms `mouth_owner_blend` hysteresis tracking (`§9.7`, `REQ-ANM-020`).
- `src/animation/skinning_frame.hpp`: Full `alignas(64) SkinningFrame` (`§9.8`: `kMaxBones = 1024`, `kMaxMorphs = 1024`, bone matrices, morph weights, `mouth_owner_blend`, head pivot/dirs, light overrides).
- `src/animation/sim_engine.hpp` / `sim_engine.cpp`: Real T2 simulation engine integrating 1-Euro -> Kalman -> `aether_graph_evaluate` -> `VisibilityFirstSolver` -> IK -> `SpringBone` -> `ExpressionMixer` -> `SkinningFrame` publish.

### P3-07 · Pipeline Integration, Preview Rig Drive & Dev Shell Confidence HUD
- Update `src/app/pipeline_skeleton.hpp/cpp`: Integrate real `TrackingEngine` on T1 (selectable webcam / file-camera / synthetic test feed without altering smoke test speed or leaking `HR-01`) and real `SimEngine` on T2.
- Update `src/renderer/preview_pass.*`: Add lightweight CPU/mapped-VB preview skinning + morph update on dirty/updated `SkinningFrame` so the Phase 2 Vulkan preview viewport animates live with webcam/file-camera tracking cleanly with 0 VK sync validation warnings (`HR-08`, full GPU compute skinning arrives in Phase 4).
- Update `src/ui/dev_shell.*`: Add Precision Dark **Tracking & Confidence HUD** in Scene/Inspector (camera device picker + Start/Stop + Calibrate 3 s countdown + Execution Provider override/status pill `[NPU | iGPU | dGPU | CPU]` + health dot + per-region confidence bars `[Face | Upper | L-Hand | R-Hand | Lower]` + attractor mode `[Thigh | Desk]` + tracking latency readout, zero literal colors `REQ-UI-001`, zero `HR-01`/`HR-11` violations).

---

## 3. Risks & Mitigations
1. **OpenVINO NPU first-run compile & model ops support**: Some dynamic/obscure TFLite ops fail NPU compilation -> mitigated by `tflite_to_onnx.py` emitting clean static-shape standard opset-17 ONNX primitives tested directly on host Intel AI Boost NPU, caching blobs in `%LOCALAPPDATA%\Aether\cache\ov_cache`, and testing all fallback tiers explicitly.
2. **DirectML + Vulkan WDDM/GPU-AV interaction & VRAM budget**: DirectML uses D3D12 compute queue -> allocate `ID3D12Device` explicitly on target LUID adapter, check `QueryVideoMemoryInfo` `<= 256 MB`, enable `DisableMemPattern` + sequential execution (`REQ-TRK-001`), never block T3 render thread.
3. **MSVC ASan (`ci-validation`) + ORT / AVX2 Intrinsics**: Test AVX2 & SIMD kernels under `ci-validation` (`/fsanitize=address`) carefully for 32-byte alignment and unaligned `_mm256_loadu_ps` where appropriate, and ensure `AETHER_HOT_PATH_SCOPE` wraps T1/T2 infer & solve loops cleanly once session init/warm-up finishes (`ort::IoBinding` pre-bound!).
4. **`clang-tidy` & `check_gates.py`**: No mutexes, condition variables, `Sleep()`, or `abort()` anywhere in `src/tracking/` or `src/animation/`; initialize all member fields in structs (`cppcoreguidelines-pro-type-member-init`).

---

## 4. Automated Exit-Gate Tests (Written First)
1. `tracking_preprocess_test` (`tests/cpp/tracking_preprocess_test.cpp`):
   - Model manifest SHA-256 verification + static tensor contracts.
   - AVX2 NV12 -> planar RGB affine warp accuracy vs scalar reference (`max abs diff < 1e-4` and bias normalization) + latency benchmark `<= 0.6 ms` across M2+M1+hands crops under `AETHER_HOT_PATH_SCOPE` (0 heap allocations).
   - Landmark -> Kabsch/Procrustes head fit recovery test (known 3D `R, t` applied to canonical 468 face -> exact `R, t` recovered within `< 0.1 deg`, `< 0.5 mm`).
2. `tracking_router_test` (`tests/cpp/tracking_router_test.cpp`):
   - Forced-fallback tests across all 4 tiers (`Intel NPU` / `OpenVINO GPU` / `DirectML LUID` / `CPU`) using `Ort::IoBinding` under `AETHER_HOT_PATH_SCOPE` (0 heap allocs during `infer()`).
   - 120-frame health monitor budget-overrun demotion test (`Tier 1 -> Tier 2A -> Tier 2B -> Tier 3` verified deterministically).
   - Real static-shape ONNX media inference timing on host Intel AI Boost NPU (`PERF-002` `p95 <= 6.0 ms`) and deterministic MP4/FileCamera clip replay (`HR-12`, original timestamps, hands gating `5%` margin & `>= 0.30` conf).
3. `animation_filters_ik_test` (`tests/cpp/animation_filters_ik_test.cpp`):
   - `SkinningFrame` size/alignment + full `T2` pipeline allocation-free guard.
   - 1-Euro + Kalman static-subject jitter test (noisy static signal -> `pos jitter <= 1.0 mm`, `rot jitter <= 0.15 deg`).
   - 2 Hz sinusoid phase lag test with 1-Euro + Kalman horizon (`<= 1 frame` = `<= 16.67 ms` at 60 Hz tracking / `<= 4.17 ms` sim).
   - `VisibilityFirstSolver` property test: `w_i` C1-continuity & slew-rate `|dw/dt| <= 4.0 / s`; hands-under-desk procedural attractor RMS wrist velocity `<= 5 mm/s`; hip sway `k_sway = 0.35`; automatic foot planting step trigger at `12 +- 1 cm` (`0.35 s` parabola, `4 cm` apex).
   - Analytical 2-bone IK reach/pole accuracy + 98% soft limit + Appendix C joint limit clamp; spine-neck FABRIK convergence `<= 4` iters (`< 1 mm` error).
   - Expression mixer sparse `W` + 60 ms mouth ownership blend.
4. `spring_bone_simd_test` (`tests/cpp/spring_bone_simd_test.cpp`):
   - AVX2 SIMD vs scalar reference parity across 512 joints & 32 sphere/capsule colliders (`max pos delta <= 1e-5 m`, `REQ-ANM-010`).
   - VRM 1.0 `spring center` node & teleport reset `> 0.5 m`.
   - SpringBone perf benchmark (`512 joints x 32 colliders <= 0.4 ms` on P-core).
   - Full `T2::tick` perf benchmark (`<= 0.8 ms` total for 55+160 bone reference rig under `AETHER_HOT_PATH_SCOPE` = 0 allocs).
5. `aether_studio.exe --smoke-frames 60 --avatar tests/data/reference_avatar.vrm` + full `tools/ci.ps1`:
   - `UI CPU <= 0.4 ms`, `0` Vulkan/sync validation errors, `0` hot-path violations across all 4 presets.
