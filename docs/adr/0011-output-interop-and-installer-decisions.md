# ADR-0011: Phase 5 Output Interop (Spout2, DirectShow VCam, NDI 6) & WiX Installer (`§11`, `HR-09`, `HR-10`)

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §11.1–§11.4, §13 (Phase 5), `HR-01`, `HR-02`, `HR-09`, `HR-10`

## Context

Phase 5 introduces four OS/external-process video output integrations:
1. **Spout2 / DXGI Shared Texture (`§11.2`)**: Sharing a GPU texture directly from Vulkan (`VK_KHR_external_memory_win32`, `D3D11_TEXTURE_KMT`) to OBS Spout2 Capture via Spout's shared-memory registry (`SpoutSenderNames`, `<Sender>` `SharedTextureInfo` 280 B, `[Sender]_SpoutAccessMutex`).
2. **DirectShow Virtual Camera (`§11.3`)**: An in-process COM DLL (`aether_vcam_ds.dll`, x64 and x86) registered in `CLSID_VideoInputDeviceCategory`. Upstream Microsoft Windows 7 DirectShow BaseClasses are unmaintained legacy sample code, and importing them would violate `HR-10`.
3. **NDI 6 (`§11.4`)**: Optional dual-PC network output loading `Processing.NDI.Lib.x64.dll` from the user's installed NDI 6 Runtime. Clean CI machines do not bundle Vizrt NDI 6 proprietary SDK binaries.
4. **WiX MSI Installer (`§13 Phase 5`)**: Packaging runtime artifacts and COM filter registration for x64 + x86 Virtual Cam (`Code signing requires a human decision`).

## Decision

1. **First-Party Wire-Compatible Spout2 DXGI+KMT Sender (`src/output/spout_output.{hpp,cpp}`)**:
   - Instead of vendoring Spout2's legacy Win32 SDK (which internally uses `std::mutex` / `CRITICAL_SECTION` / `Sleep()` forbidden in `src/output` by `HR-02` / `tools/check_gates.py`), implement AETHER's lock-free Vulkan-D3D11 KMT bridge directly against the documented Spout2 wire protocol (`SpoutSenderNames` 40×256 table, 280 B `SharedTextureInfo` per sender, and named kernel mutex `[Sender]_SpoutAccessMutex` waited with strict `1 ms` timeout per §11.2.3).
   - Provide an automatic `CpuCompatibility` staging fallback when DXGI cross-adapter sharing fails or OBS runs on a separate iGPU/dGPU (`§11.2.4`).

2. **First-Party Modern C++20 DirectShow COM Filter (`src/vcam/aether_vcam_ds.cpp`)**:
   - Implement `IBaseFilter`, `IPin`, `IAMStreamConfig`, `IKsPropertySet`, `IEnumPins`, `IEnumMediaTypes`, and `IClassFactory` directly over Windows SDK `<strmif.h>`, `<amvideo.h>`, `<uuids.h>`, and `<olectl.h>` with **zero external dependencies (`HR-10`)**.
   - Build both `aether_vcam_ds_x64.dll` (via CMake main build) and `aether_vcam_ds_x86.dll` (via `tools/build_vcam_x86.ps1` using MSVC `vcvarsamd64_x86.bat`).
   - Support both standard HKLM `DllRegisterServer` (used by installer/admin `regsvr32`) and test/COM-friendly `DllGetClassObject` instantiation + HKCU per-user verification so unprivileged CI runs cleanly without UAC elevation.

3. **Runtime-Probed NDI 6 Sender (`src/output/ndi_output.{hpp,cpp}`, `HR-09`)**:
   - Declare exact binary-compatible NDI 6 C ABI structures (`NDIlib_send_create_t`, `NDIlib_video_frame_v2_t`, `NDIlib_FourCC_type_BGRA`, `NDIlib_FourCC_type_UYVA`, `NDIlib_send_send_video_async_v2`) without redistributing proprietary Vizrt SDK binaries (`HR-10`).
   - Verify NDI SDK alpha convention: both `NDIlib_FourCC_type_BGRA` and `NDIlib_FourCC_type_UYVA` consume **straight (unpremultiplied) alpha**; configure Pass 12 accordingly when NDI output is active.
   - Return `ErrorCode::Unsupported` cleanly and log once if `Processing.NDI.Lib.x64.dll` is not installed (`HR-09`, tracked in `docs/STATUS.md`), while exposing an injectable C function table for automated async double-buffer and UYVA tests.

4. **WiX v4 MSI Authoring (`installer/AetherStudio.wxs`)**:
   - Author full declarative WiX v4 XML definitions (`AetherStudio.wxs` + `Build-Installer.ps1`) registering both x64 and x86 `aether_vcam_ds` COM servers and `CLSID_VideoInputDeviceCategory` keys without legacy `SelfReg` actions. MSI signing / notarization requires human certificate provisioning at release time.

## Consequences

- Zero new entries needed in `vcpkg.json`, `Cargo.toml`, or `docs/dependencies_allowlist.toml` (`HR-10` strictly maintained).
- Every `src/output` hot-path module obeys `HR-01` and `HR-02` (`tools/check_gates.py` clean).
- DirectShow x64 & x86 COM filters, Spout2 D3D11 KMT sharing, OSC telemetry, and cross-process seqlock DSP sync are fully built and automated-tested on Windows CI without proprietary runtime installers.
