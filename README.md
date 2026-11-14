# DSD-Aether — AETHER-2.5D Studio Engine

Real-time VTuber studio for Windows 10/11 x64: NPU-first motion tracking, Live2D-grade 2.5D NPR rendering with hardware ray tracing, zero-copy alpha video output (Spout2, DirectShow virtual camera, NDI 6) and audio-DSP sync.

**Version:** `a1.0.0B` — alpha 1, revision 0, build 0.

The binding specification is [`AETHER-2.5D_Technical_Specification_v1.0.pdf`](AETHER-2.5D_Technical_Specification_v1.0.pdf) (AETHER-TS-001). Implementation status lives in [`docs/STATUS.md`](docs/STATUS.md), design decisions in [`docs/adr/`](docs/adr/), and per-phase reports in [`docs/phases/`](docs/phases/).

## Stack

| Area | Technology |
|---|---|
| Core, renderer, tracking, output, UI | C++20 (MSVC v143), Vulkan 1.3 (+ `VK_KHR_ray_query`), DirectX 11 backend (ADR-0015) |
| Asset loader, plugin host, node graph | Rust 2024 (static library via Corrosion) |
| Inference | ONNX Runtime with OpenVINO (NPU), DirectML / TensorRT, CPU tiers |
| UI | Dear ImGui (docking), ImNodes, ImPlot, ImGuizmo, FreeType |
| Memory | Epoch-fenced + incremental tri-color garbage collector (`src/core/garbage_collector.*`) |

## Building

Requirements: Visual Studio 2022 Build Tools (17.10+), Vulkan SDK 1.3.290+, Rust 1.85+, vcpkg, Python 3.

```powershell
# Full gate: static checks, Rust fmt/clippy/deny/tests, all 4 CMake presets, CTest
powershell -ExecutionPolicy Bypass -File tools\ci.ps1

# Release build only
cmake --preset windows-release
cmake --build --preset windows-release
```

`tools/ci.ps1` expects the Build Tools at `D:\VS2022\BuildTools`; edit `Import-MsvcEnv` if yours are elsewhere.

### Tracking models

ONNX models are not committed. Regenerate them (Apache-2.0 MediaPipe sources, see ADR-0008) with:

```powershell
python tools\models\build_models.py
```

The release page also carries a `models` archive.

## Repository layout

```
crates/    Rust: aether-ffi, aether-asset-loader, aether-plugin-host, aether-rust-host
include/   Generated C ABI contract (aether_ffi_contract.h)
src/       C++: app, core, rhi, tracking, animation, renderer, output, vcam, plugin_runner, ui
shaders/   Slang sources (SPIR-V + DXIL)
tests/     Catch2 suites, golden images, soak harness, test corpus
tools/     CI, code generators, model conversion
installer/ WiX installer (virtual camera registration)
docs/      Status, ADRs, phase plans/reports, protocol docs
```
