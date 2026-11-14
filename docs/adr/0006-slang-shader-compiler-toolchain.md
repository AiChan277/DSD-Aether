# ADR-0006: Slang Shader Compiler Build Toolchain (`slangc`)

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §3.2, §3.5, `HR-10`

## Context

`AETHER-TS-001` §3.2 and §3.5 specify `slangc` (from Khronos/Shader-Slang) compiling project shaders under `shaders/` into SPIR-V (`spirv_1_5` / `spirv_1_6`) via `tools/shader_build.py` invoked from CMake. The local Windows build host does not have a global Vulkan SDK installation (`$env:VULKAN_SDK` unset), and Windows Kits `10.0.26100.0` `dxc.exe` was built without `-DENABLE_SPIRV_CODEGEN=ON`.

## Decision

1. Pin the official **Shader-Slang** standalone build tool **v2026.22** (`slang-2026.22-windows-x86_64.zip`, Apache-2.0 WITH LLVM-Exception, governed by Khronos Group) in user-local build-tool cache `%LOCALAPPDATA%\Aether\toolchains\slang-2026.22-windows-x86_64\bin\slangc.exe` (SHA-256 of zip archive: `DD0C4A86BD44CDB7E9CF712AF0C8EBF2FFF1BBA19FA5E3C128FF87384E8FF17B`).
2. `slangc.exe` is strictly a **build-time compiler** (like `cbindgen.exe` in `ADR-0001`). Neither Slang runtime libraries nor compiler DLLs are linked into `aether_studio.exe` or distributed at runtime; compiled SPIR-V binaries are embedded into C++ via `tools/embed_spv.py` for Release builds cleanly (and loaded/validated in Debug).
3. `tools/shader_build.py` resolves `slangc.exe` in deterministic order:
   - `AETHER_SLANGC` environment override
   - `%LOCALAPPDATA%\Aether\toolchains\slang-2026.22-windows-x86_64\bin\slangc.exe`
   - `%VULKAN_SDK%\Bin\slangc.exe`
   - `PATH` lookup (`shutil.which("slangc")`)

## Consequences

- `slangc` compiles Phase 2 preview HLSL/Slang shaders (`shaders/preview_mesh.*`, `shaders/preview_bones.*`) now and scales seamlessly to Phase 4 (`shaders/aether_live2d_npr.slang`, outlines, ray-query shadows, compute passes) without changing the build pipeline.
- Offline builds (`cmake --build`) work with zero network access once `%LOCALAPPDATA%\Aether\toolchains\slang-2026.22-windows-x86_64` is present.
