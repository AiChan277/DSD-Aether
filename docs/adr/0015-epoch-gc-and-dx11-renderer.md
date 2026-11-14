# ADR-0015: Epoch-Fenced / Tri-Color Garbage Collector and DirectX 11 2.5D NPR Renderer Backend

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-01`, `HR-02`, `HR-03`, `HR-08`, `HR-10`), §3.1, §8, §11.1

## Context

To support deterministic reclamation of frame-retiring GPU/engine resources and cyclic runtime graph objects without violating `HR-01` (zero heap allocations on hot paths) or `HR-02` (zero blocking locks between `T1–T4`), and to provide native DirectX 11 2.5D NPR rendering alongside the primary Vulkan 1.3 renderer and DXGI/Spout interop path:

1. **Garbage Collector (`src/core/garbage_collector.{hpp,cpp}`)**:
   - Implemented in **C++20** (the industry-standard systems language used for production garbage collectors such as V8 Oilpan/CppGC, JVM HotSpot G1/ZGC, and Unreal Engine's UObject GC).
   - Combines:
     - **Lock-Free Epoch-Fenced Resource Collector**: Uses `SpscRing` + fixed pre-allocated retirement slots so hot-path threads (`T1–T4`) can retire handles in `O(1)` with `0` heap allocations (`HotPathGuard`-safe), reclaiming them only after `completed_epoch >= retire_epoch` within a bounded per-tick item budget.
     - **Fixed-Slab Incremental Tri-Color (`White`/`Gray`/`Black`) Mark-and-Sweep Collector**: Manages up to 256 cyclic/graph objects in pre-allocated slab storage with root pinning, incremental mark steps, and sweep reclamation without dynamic allocation during collection.
2. **DirectX 11 2.5D NPR Renderer Backend (`src/renderer/dx11_renderer.{hpp,cpp}`)**:
   - Uses standard Windows 10/11 SDK system libraries (`d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`), requiring zero external third-party packages (`HR-10`).
   - Consumes the frozen 64-byte `AetherVertex25D` layout (`include/aether_ffi_contract.h`, `HR-03`), compiles Shader Model 5.0 (`vs_5_0` / `ps_5_0`) 2.5D NPR cel/toon + rim-lighting shaders at `init()`, pre-allocates dynamic vertex/index/constant buffers and state objects, and renders into premultiplied-alpha `DXGI_FORMAT_R8G8B8A8_UNORM` targets (with `OpenSharedResource` DXGI shared-texture interop support) with `0` per-frame heap allocations (`HR-01`).

## Consequences

- `aether_core` gains `garbage_collector.{hpp,cpp}` and `aether_renderer` gains `dx11_renderer.{hpp,cpp}` linked against Windows SDK `d3d11`, `dxgi`, and `d3dcompiler`.
- Verified by `gc_and_dx11_renderer_test` under CTest across `windows-debug`, `windows-relwithdebinfo`, and `ci-validation` (`HotPathGuard` + ASan).
