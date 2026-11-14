# ADR-0001: Toolchain and Dependency Pins

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §3.2, HR-10

## Context

`AETHER-TS-001` requires exact pinning of all C++ and Rust toolchains and third-party dependencies in Phase 1, with an explicit license, maintenance, binary-size, and security-surface review for every dependency (`HR-10`).

## Decision

### Build Toolchains (Exact Versions Verified on Host)

| Component | Version | License | Notes |
|---|---|---|---|
| MSVC (VS 2022 BuildTools) | `14.44.35207` (v143) | Proprietary SDK | Primary C++20 compiler (`/std:c++20 /permissive- /W4 /WX /Zc:__cplusplus /utf-8`). |
| LLVM `clang-format` / `clang-tidy` | `19.1.5` | Apache-2.0 WITH LLVM-exception | Formatting and static analysis (`bugprone-*`, `performance-*`, `concurrency-*`). Note: `clang-cl` is not installed in the current BuildTools layout; CI falls back gracefully with a logged notice when absent. |
| CMake / Ninja | `3.31.6-msvc6` / `1.13.2` | BSD-3 / Apache-2.0 | Build orchestration (`CMakePresets.json`). |
| vcpkg | `2025-11-19` (`3af1d1e60af2b2abf55760538cd607829029b07a`) | MIT | Manifest-mode C++ package manager. |
| Rust toolchain | `1.100.0` (`x86_64-pc-windows-msvc`, edition 2024) | MIT / Apache-2.0 | `rustfmt` and `clippy` components pinned via `rust-toolchain.toml`. |
| Corrosion | `v0.5.1` | MIT | CMake-Cargo bridge (`FetchContent`). |
| `cbindgen` | `0.29.4` | MPL-2.0 (build tool) | Build-time C header generator from `crates/aether-ffi`. Not linked into binaries. |
| Python | `3.14.2` (`fonttools 4.67.0`) | PSF / MIT | Build/CI helper scripts (`font_tnum_freeze.py`, `check_gates.py`). |

### C++ Dependencies (`vcpkg.json`, baseline `3af1d1e60af2b2abf55760538cd607829029b07a`)

| Package | Version | License | Role | Maintenance & Security Surface |
|---|---|---|---|---|
| `vulkan-headers` | `1.4.309.0` | Apache-2.0 | Vulkan 1.3 API headers | Khronos Group official; compile-time headers only. |
| `vulkan-loader` | `1.4.309.0#2` | Apache-2.0 | ICD loader (`vulkan-1.dll`) | Khronos Group official; ~1.1 MB DLL, standard GPU driver dispatch. |
| `vulkan-memory-allocator` | `3.3.0` | MIT | GPU & ReBAR memory allocation | GPUOpen active; header-only (`vk_mem_alloc.h`), zero network/parser surface. |
| `vulkan-validationlayers` | `1.4.309.0` | Apache-2.0 | Debug/CI validation + sync validation | Khronos Group official; shipped/enabled only in Debug & CI, not production release runtime. |
| `freetype` | `2.13.3` | FTL (chosen over GPL) | UI font glyph rasterisation | Widely maintained C library; bundled fonts only (no untrusted web fonts). Attribution included in About box. |
| `imgui` | `1.91.9` | MIT | Studio UI (`docking-experimental`, `freetype`, `vulkan-binding`, `win32-binding`) | Active (`ocornut/imgui`); ~0.8 MB compiled code, UI only. |
| `tracy` | `0.11.1#2` | BSD-3-Clause | Profiling zones & Vulkan GPU timestamps | Active (`wolfpld/tracy`); compiled out of `windows-release`. |
| `catch2` | `3.11.0` | BSL-1.0 | C++ unit/property/smoke test harness | Active (`catchorg/Catch2`); test binaries only. |
| `nlohmann-json` | `3.12.0#1` | MIT | Test-only JSON parser for `layout_table.json` (`REQ-FFI-005`) | Active (`nlohmann/json`); header-only, test binaries only (never linked into `aether_studio.exe`). |

Transitive vcpkg libraries (`zlib 1.3.1`, `libpng 1.6.50`, `bzip2 1.0.8#6`, `brotli 1.2.0`) are pulled solely by `freetype` and carry permissive licenses (`Zlib`, `libpng-2.0`, `bzip2-1.0.6`, `MIT`).

### Rust Dependencies (`Cargo.lock`, Phase 1)

| Crate | Version | License | Role | Maintenance & Security Surface |
|---|---|---|---|---|
| `bytemuck` (+ `bytemuck_derive`) | `1.25.2` | Zlib / Apache-2.0 / MIT | Zero-copy Pod/Zeroable slice casts (`§5.6`) | Widely used; pure safe abstraction over layout-checked `#[repr(C)]` pods. |
| `thiserror` | `2.0.21` | MIT / Apache-2.0 | Typed error enum mapping to `AetherStatus` | `dtolnay`, minimal proc-macro. |
| `tracing` | `0.1.44` | MIT | Diagnostics inside Rust crates | Tokio project, standard structured logging. |

### Fonts (`assets/fonts/`)

- `Inter` (`Inter-Regular-tnum.ttf`, `Inter-SemiBold-tnum.ttf`): SIL Open Font License 1.1 (`Inter-OFL.txt`), tabular numerals (`tnum` + `cv11`) frozen.
- `JetBrains Mono` (`JetBrainsMono-Regular.ttf`): SIL Open Font License 1.1 (`JetBrainsMono-OFL.txt`).

## Consequences

- Every dependency is permissively licensed for commercial distribution (no GPL/AGPL; `FTL` chosen for FreeType; `cbindgen` MPL-2.0 is a build tool only).
- `docs/dependencies_allowlist.toml` mirrors this list and is enforced in `tools/ci.ps1`.
- Adding any new dependency in Phases 2–6 requires a new ADR before adding to `vcpkg.json` or `Cargo.toml`.
