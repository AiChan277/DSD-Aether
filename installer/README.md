# AETHER-2.5D Studio Windows Installer

This directory builds the Windows release packages from the `windows-release` CMake build and the 32-bit Virtual Camera build:

- `AetherStudio-<tag>-x64.msi` — standard per-machine Windows Installer (`.msi`) built with WiX Toolset v5.
- `AetherStudio-<tag>-win64-portable.zip` — portable folder archive with `portable.flag` (keeps settings inside `.\UserData`).
- `SHA256SUMS.txt` — SHA-256 checksums for verifying package integrity after download.

---

## Why a Declarative WiX v5 `.msi` (Antivirus & SmartScreen Hygiene)

Custom `.exe` installers (self-extracting stubs, UPX/packed wrappers, embedded PowerShell/batch scripts, or downloaders) frequently trigger false positives in heuristic antivirus scanners. To keep AETHER-2.5D Studio clean and predictable:

1. **Native Windows Installer (`msiexec.exe`)**: `AetherStudio.wxs` compiles directly to a standard `.msi` with an embedded CAB archive—no bootstrapper `.exe`, no runtime packer, no obfuscation, and no script-based custom actions.
2. **PE Metadata & Least Privilege**: Every first-party executable (`aether_studio.exe`, `aether-plugin-runner.exe`) and DirectShow COM DLL (`aether_vcam_ds_x64.dll`, `aether_vcam_ds_x86.dll`) embeds full Win32 `VS_VERSION_INFO` metadata and an explicit `requestedExecutionLevel level="asInvoker"` application manifest.
3. **App-Local Microsoft Visual C++ Runtime**: Signed Microsoft `VC143.CRT` runtime DLLs are staged app-local next to the binaries (`Program Files\AETHER-2.5D Studio` and `vcam/`), avoiding nested third-party redistributable installers.
4. **Standard COM Virtual Camera Registration**: DirectShow filter registration (`regsvr32 /s` on install, `/u /s` on uninstall) runs solely as deferred elevated actions against the installed DLL paths and removes all entries cleanly on uninstall.
5. **User-Writable Data Separation**: Installed runs write session state (`session_state.json`), project archives (`.apr`), custom `.alfx` packages, logs, caches, and custom camera presets to `%APPDATA%\AETHER-2.5D-Studio` / `%LOCALAPPDATA%\AetherStudio` (never to `Program Files`).

### About Windows SmartScreen on Unsigned Community Builds

If the release is built without `-CertThumbprint` (no paid EV/OV Authenticode certificate or Azure Trusted Signing profile), Windows SmartScreen may show **"Windows protected your PC / Unknown publisher"** when first run because the file lacks a commercial code-signing reputation.

- Always verify the download against `SHA256SUMS.txt` from the GitHub Release page:
  ```powershell
  Get-FileHash .\AetherStudio-a1.0.4B-x64.msi -Algorithm SHA256
  ```
- **Never** distribute binaries signed with a self-signed certificate for public releases—untrusted root signatures still warn in SmartScreen and trigger reputation issues.

---

## Prerequisites

1. Build the release application and 32-bit virtual camera:
   ```powershell
   cmake --preset windows-release
   cmake --build --preset windows-release
   powershell -ExecutionPolicy Bypass -File tools\build_vcam_x86.ps1 -Config Release
   ```
2. Install WiX Toolset v5 and extensions (one-time):
   ```powershell
   dotnet tool install --global wix --version 5.0.2
   wix extension add -g WixToolset.UI.wixext/5.0.2
   wix extension add -g WixToolset.Util.wixext/5.0.2
   ```

## Building the Installer

Unsigned local / release build:

```powershell
powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1
```

Authenticode-signed build (when a certificate from `Cert:\CurrentUser\My` or hardware token is available):

```powershell
powershell -ExecutionPolicy Bypass -File installer\Build-Installer.ps1 -CertThumbprint "<SHA1_THUMBPRINT>"
```

Outputs are written to `dist\`:
- `dist\AetherStudio-a1.0.4B-x64.msi`
- `dist\AetherStudio-a1.0.4B-win64-portable.zip`
- `dist\SHA256SUMS.txt`

## What the Installer Installs

- **Application**: `C:\Program Files\AETHER-2.5D Studio\` (`aether_studio.exe`, `aether-plugin-runner.exe`, runtime DLLs, `models\`, `assets\fonts\`, `config\`, `presets\alfx\`, `vcam\x64\aether_vcam_ds_x64.dll`, `LICENSE.txt`, `README.md`)
- **32-bit Virtual Camera**: `C:\Program Files (x86)\AETHER-2.5D Studio\vcam\aether_vcam_ds_x86.dll`
- **Shortcuts, File Associations & Registry**: Start Menu folder, Desktop shortcut, `.apr` & `.alfx` Explorer file associations, Apps & Features entry (`AiChan277 / AETHER-2.5D Studio`), `App Paths\aether_studio.exe`, and DirectShow `CLSID_VideoInputDeviceCategory` registration.
