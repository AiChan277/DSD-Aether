# ADR-0007: ONNX Runtime and Execution Provider Distribution (`v1.24.1`)

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-01`, `HR-10`, `HR-12`), §3.2, §8.4

## Context

`AETHER-TS-001` §8.4 requires a unified ONNX Runtime C++20 inference pipeline supporting three hardware tiers without heap allocations during inference (`Ort::IoBinding`, `REQ-TRK-001`):
1. **Tier 1 (Primary)**: Intel AI Boost NPU via OpenVINO Execution Provider (`device_type = "NPU"`, disk blob cache `%LOCALAPPDATA%\Aether\cache\ov_cache`).
2. **Tier 2A (iGPU)**: Intel iGPU via OpenVINO EP (`device_type = "GPU"`) or DirectML EP on an integrated adapter whose DXGI LUID differs from the Vulkan render dGPU.
3. **Tier 2B (dGPU)**: NVIDIA TensorRT EP (when TensorRT runtime DLLs are installed) or DirectML EP (`D3D12_COMMAND_QUEUE_PRIORITY_HIGH` async compute queue, VRAM capped `<= 256 MB` via `IDXGIAdapter3::QueryVideoMemoryInfo`).
4. **Tier 3 (CPU)**: OpenVINO CPU (`VNNI`/`AVX2` INT8) / ORT CPU EP pinned to E-cores / secondary SMT threads via `SetThreadAffinityMask`.

## Decision

### 1. Pinned Runtime Packages (`HR-10`)
Instead of redistributing unpinned system DLLs or building Chromium-sized monorepos on every clean machine, Phase 3 pins official Microsoft & Intel release packages for **ONNX Runtime 1.24.1**:

| Package | Version | Licence | SHA-256 (`nupkg`) | Role |
|---|---|---|---|---|
| `Microsoft.ML.OnnxRuntime.DirectML` | `1.24.1` | MIT | `D5521507B32C638F1F29DE42CB18F21182D40A7541398786D44E38F4A1D869F1` | Single `onnxruntime.dll` (built with DirectML EP + dynamic shared EP loader), `onnxruntime.lib`, `onnxruntime_providers_shared.dll`, C/C++/DML headers. |
| `Intel.ML.OnnxRuntime.OpenVINO` | `1.24.1` (OpenVINO `2025.4.1.20426`) | MIT (`EP`) / Apache-2.0 (`OpenVINO`, `oneTBB`) | `F53AD5F90E3D616970A5C65E4880EBBE92C9774E9727020661DB591CEA74A110` | `onnxruntime_providers_openvino.dll`, `openvino.dll`, `openvino_intel_npu_plugin.dll`, `openvino_intel_gpu_plugin.dll`, `openvino_intel_cpu_plugin.dll`, `openvino_onnx_frontend.dll`, `openvino_ir_frontend.dll`, `tbb12.dll`, `tbbbind_2_5.dll`. |
| Windows Inbox `DirectML.dll` | Windows 10/11 >= `1.13` (`1.15.6` on dev host) | MIT / OS Inbox | OS System32 | Direct3D 12 `IDMLDevice` execution (`Tier 2A/2B`). |

Verified on host (`Intel Core Ultra 7 255HX` + `Intel AI Boost` + `Intel Graphics` + `RTX 5060`): Microsoft's `onnxruntime.dll 1.24.1` loads Intel's `onnxruntime_providers_openvino.dll 1.24.1` directly in the same process, allowing **one `onnxruntime.dll`** to execute OpenVINO NPU, OpenVINO GPU, OpenVINO CPU, and DirectML high-priority D3D12 queues simultaneously.

### 2. Local Toolchain Cache & Staged Deployment
`tools/fetch_phase3_deps.ps1` downloads, SHA-256 verifies, and installs the merged SDK into `%LOCALAPPDATA%\Aether\toolchains\ort-1.24.1\` (overrideable via `AETHER_ORT_DIR`), matching `ADR-0006` (`slangc`). CMake copies the required runtime `.dll` files beside binaries targeting `aether_tracking` (`POST_BUILD` `copy_if_different`) so `aether_studio.exe` and CTest binaries run self-contained.

### 3. OpenVINO 2025.4 EP Option Mapping (`§8.4`)
ORT `1.24.1`'s `onnxruntime_providers_openvino.dll` removed the legacy `enable_npu_fast_compile` string option (`Invalid provider_option key: enable_npu_fast_compile`). Per `§8.4`:
> "If the installed EP version renames an option, use its documented equivalent and log the mapping."

`AetherTrackingRouter` maps fast latency-first NPU compilation to OpenVINO `load_config` (`{"NPU": {"NPU_COMPILER_TYPE": "DRIVER", "PERFORMANCE_HINT": "LATENCY"}}` with clean fallback to `{device_type="NPU", cache_dir=%LOCALAPPDATA%\Aether\cache\ov_cache}` if driver rejects `load_config`), and logs the option mapping explicitly.

### 4. Optional NVIDIA TensorRT EP (`AETHER_WITH_TENSORRT`)
Under proprietary NVIDIA TensorRT SLA, CUDA/TensorRT libraries (`nvinfer_10.dll`, `cudart64_12.dll`) are not redistributed by default. When `AETHER_WITH_TENSORRT=ON`, Tier 2B dynamically probes for `onnxruntime_providers_tensorrt.dll` + TensorRT runtime via `LoadLibraryExW`; when present, it configures a high-priority CUDA stream (`cudaStreamCreateWithPriority`), `trt_max_workspace_size = 268435456` (256 MB), FP16 enabled, and `%LOCALAPPDATA%\Aether\cache\trt_cache`. Otherwise Tier 2B selects DirectML EP on the dGPU with `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` cleanly.

### 5. Security, Maintenance & Licence Summary (`HR-10`)
- **Licences**: Strictly permissive (`MIT`, `Apache-2.0`); commercial closed-source/open-source distribution allowed with attribution in `ThirdPartyNotices.txt`.
- **Binary Size**: ~110 MiB total native runtime DLLs (exclude debug `*_debug.dll` plugins).
- **Security Surface**: Offline local inference only; every loaded ONNX file is SHA-256 verified against `models/manifest.toml` before creating an `Ort::Session`. Zero network I/O (`HR-12`).

## Consequences
- Every machine running Windows 10/11 x64 reproduces the exact ORT + OpenVINO + DirectML stack via `tools/fetch_phase3_deps.ps1` with verified checksums.
- All four tracking execution tiers (NPU, iGPU, dGPU, CPU) operate cleanly under a single `onnxruntime.dll`.
