# ADR-0008: Tracking Models, Commercial Licences, and Static-Shape ONNX Pipeline

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-10`, `HR-12`), §8.2, §8.3, §13 Phase 3

## Context

`AETHER-TS-001` §8.3 specifies four tracking model families with fixed static input shapes (required for Intel AI Boost NPU AOT compilation), INT8/FP16 tier variants, a SHA-256-pinned `models/manifest.toml`, conversion/quantisation scripts under `tools/models/`, an accuracy gate (`NME delta <= 3%` vs FP32), and explicit commercial-redistribution licence approval.

## Decision

### 1. Upstream Model Selection & Commercial Licences (`HR-10`)
We adopt **Google MediaPipe official Solution Task / TFLite models**, published by Google LLC under the **Apache License 2.0** (commercial use, modification, static conversion, and redistribution permitted cleanly with Apache-2.0 attribution):

| Slot | Model ID | Source Asset (`storage.googleapis.com/mediapipe-models/...`) | Static Input Shape | Output Tensors | Tier Precision |
|---|---|---|---|---|---|
| `M0` | `face_detector` | `face_detector/blaze_face_short_range/float16/latest/blaze_face_short_range.tflite` | `[1, 3, 128, 128]` (`float32` NCHW) | `regressors [1, 896, 16]`, `classificators [1, 896, 1]` | FP16 / INT8 (ROI reset only) |
| `M1_body` | `pose_landmarks` | `pose_landmarker/pose_landmarker_lite/float16/latest/pose_landmarker_lite.task` (`pose_landmarks_detector.tflite`) | `[1, 3, 256, 256]` (`float32` NCHW) | `landmarks [1, 195]`, `poseflag [1, 1]`, `world_landmarks [1, 117]` (plus seg/heatmap kept optional) | INT8 (`NPU`/`CPU`) · FP16 (`GPU`) |
| `M1_hands` | `hand_landmarks` | `hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task` (`hand_landmarks_detector.tflite`) | `[2, 3, 224, 224]` (`float32` NCHW batched L/R) | `landmarks [2, 63]`, `handflag [2, 1]`, `handedness [2, 1]`, `world_landmarks [2, 63]` | INT8 (`NPU`/`CPU`) · FP16 (`GPU`) |
| `M2` | `face_landmarks` | `face_landmarker/face_landmarker/float16/latest/face_landmarker.task` (`face_landmarks_detector.tflite` + `geometry_pipeline_metadata_landmarks.binarypb`) | `[1, 3, 256, 256]` (`float32` NCHW) | `landmarks [1, 1434]` (`478*3`), `faceflag [1, 1]` | INT8 (`NPU`/`CPU`) · FP16 (`GPU`) |
| `M3` | `face_blendshapes` | `face_landmarker/face_landmarker/float16/latest/face_landmarker.task` (`face_blendshapes.tflite`) | `[1, 146, 2]` (`float32` 146 normalized 2D face landmarks) | `blendshapes [1, 52]` (52 ARKit weights) | FP16 / FP32 (sub-1 ms MLP) |

Canonical Kabsch face geometry (`468` canonical 3D vertices and Procrustes landmark basis weights) is extracted verbatim from MediaPipe's Apache-2.0 `geometry_pipeline_metadata_landmarks.binarypb` into a static C++ header `src/tracking/canonical_face_model.hpp`.

### 2. Static ONNX Export, Calibration & Accuracy Gate (`tools/models/`)
- Because TFLite `.task` assets use MediaPipe's `NHWC` layout and FP16-stored initializers (`DEQUANTIZE`), `tools/models/tflite_to_onnx.py` deterministically converts each `.tflite` flatbuffer into clean **ONNX opset-17** with:
  - Constant-folded `DEQUANTIZE` FP32 initializers.
  - Static **NCHW** image inputs (`[1, 3, H, W]` / `[2, 3, 224, 224]`) matching the AVX2 NV12->planar-RGB pre-processor (`§8.2`) with zero runtime transpose outside the NPU/GPU graph.
  - Exact static batch dimension `2` baked into `hand_landmarks` (`2x3x224x224`) so L+R hands evaluate in one batched NPU dispatch when active.
  - Cross-validation against OpenVINO's native TFLite frontend (`ov.Core().compile_model("*.tflite", "CPU")`) requiring `max_abs_diff < 1e-3`.
- `tools/models/quantize_models.py` runs static per-channel INT8 quantisation (`onnxruntime.quantization.quantize_static`, QDQ opset 17, `per_channel=True`) over a **500-frame calibration corpus** (deterministic rendered/warped textured faces, upper bodies, and hands — never shipping real webcam images, `HR-12`), plus FP16 graph conversion, and fails the build if:
  `NME(INT8) - NME(FP32) > 3.0%` (`§8.3`).
- `models/manifest.toml` records exact filenames, SHA-256 digests, opset, I/O tensor names/shapes, normalisation scale/bias, and per-tier precision filenames (`fp16` / `int8` / `fp32`). Generated `.onnx` weights under `models/` are gitignored and reproducibly fetched/generated via `tools/fetch_phase3_deps.ps1` / `tools/models/build_models.py` so git history stays clean while runtime SHA-256 verification remains strict.

## Consequences
- Every tracking model obeys §8.3 static tensor contracts and Apache-2.0 commercial licensing (`HR-10`).
- Build-time conversion dependencies (`onnx`, `onnxruntime`, `flatbuffers`, `tflite`, `onnxconverter-common`, `openvino` Python) live exclusively in `%LOCALAPPDATA%\Aether\toolchains\tools-venv` and are never linked into or shipped inside `aether_studio.exe`.
- Runtime loads models exclusively by verifying SHA-256 against `models/manifest.toml` before passing paths to ONNX Runtime (`HR-12`, offline safety).
