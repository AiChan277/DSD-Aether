# ADR-0016: Wiring the Garbage Collector and the DirectX 11 Backend into the Studio App

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-01`, `HR-02`, `HR-08`), §0.7 #7, §10.4, §11, §12.12
- **Builds on**: ADR-0015 (collector and DirectX 11 renderer as standalone components)

## Context

ADR-0015 added `aether::GarbageCollector` and `Dx11NprRenderer`, but the running app used neither: avatars were destroyed synchronously after a full GPU stall on swap, and the avatar could only be drawn by the Vulkan frame graph.

## Decision

### Render backend selection (`render.backend`)
- New app-scoped parameter `render.backend` (`Vulkan | DirectX 11`) in the parameter registry, shown as the first row of Inspector ▸ Lighting & NPR, plus the `--render-backend vulkan|dx11` launch flag (wins over the persisted value at startup).
- **Only the avatar draw changes.** The UI, viewport compose, output encode, Spout2, the virtual camera, NDI and DSP sync always run on Vulkan, so every output keeps working in either mode. Ray tracing is unavailable on DirectX 11 (the Inspector row is disabled with the reason).
- `renderer::Dx11Bridge` renders the CPU-skinned avatar (`GpuAvatar::skinned_vertices_scratch`, the same pose the Vulkan path receives) with one draw per sub-mesh using each material's lit/shade colours, in linear light to match the `OUT_RGBA16F` contract. The D3D11 device is created on the adapter whose LUID matches the Vulkan device.
- Hand-off to Vulkan, chosen automatically:
  1. **Shared texture** (default): the D3D11 `R8G8B8A8_UNORM` target is imported as a `VkImage` through `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT`, acquired from / released to `VK_QUEUE_FAMILY_EXTERNAL` around the blit.
  2. **CPU copy** (WARP device, no external-memory support, or `set_force_cpu_copy`): D3D11 readback into a persistently mapped staging buffer, copied to a Vulkan image in the frame's command buffer.
- New `FrameRenderRequest::external_*` fields: when set, the frame graph skips Passes 1–11 and blits the external image into `OUT_RGBA16F`; Pass 12 (encode) and the UI pass run unchanged.
- Synchronisation: D3D11 → Vulkan by an event-query wait after drawing; Vulkan → D3D11 by waiting on the frame timeline value of the last submit that read the image before D3D11 overwrites it. D3D11 KMT handles are not reference counted, so the bridge also waits for that value before releasing D3D11 resources.
- If DirectX 11 cannot be initialised, the app falls back to Vulkan and shows a toast.

### Garbage collector
- The app owns one `GarbageCollector` on T3 and calls `collect_epoch(completed frame timeline, 16)` every frame.
- **Avatar swap**: the previous `GpuAvatar` is retired against the last submitted timeline value instead of `rhi->wait_idle()` + immediate destruction. (`FrameGraph::bind_avatar` still fences its own frame-graph buffers.)
- **DX11 bridge**: images, imported memory and staging buffers replaced on resize, capacity growth or backend switch are retired to the collector against the last frame that read them.
- Device-lost recovery and shutdown release the bridge and `flush_all_epochs()` after the GPU is idle, before the device is rebuilt or destroyed.
- GC counters (pending, reclaimed, bytes) are shown in Inspector ▸ Hardware & Contracts and printed in the smoke report.

## Verification
- `dx11_backend_test`: reference avatar rendered through Vulkan and through the bridge on both hand-off paths; silhouette IoU vs. Vulkan 0.972 on both; reconfigure retires objects that the collector reclaims; Vulkan validation clean.
- `tools/ci.ps1` smoke gate (windows-debug): `--render-backend dx11 --smoke-reload-avatar-at 30` must render 90/90 frames on DirectX 11, reclaim ≥ 1 retired object, with 0 validation errors and 0 hot-path violations.

## Consequences / limits
- The DirectX 11 path is a compatibility renderer: cel shading with material colours and rim light, no textures, outlines, SDF face shadows or ray tracing.
- The tri-color mark-and-sweep half of the collector has no runtime client yet; only the epoch-fenced retirement half is used by the app.
- The shared-texture path adds a CPU wait for the previous frame's GPU work, so the DirectX 11 path is not latency-equivalent to the Vulkan path.
