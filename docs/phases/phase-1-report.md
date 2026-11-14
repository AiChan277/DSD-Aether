# Phase 1 Report — Core Scaffold & FFI Contract

- **Spec Reference**: `AETHER-TS-001 v1.0` (§0.3, §2.2–§2.4, §3, §4, §5, §10.1–§10.3, §12.1–§12.4, §12.14–§12.15, §13)
- **Plan**: `docs/phases/phase-1-plan.md`
- **Phase Gate Verdict**: **PASS** (`tools/ci.ps1` exit code `0` across all gates, 4 CMake presets, all C++/Rust unit & contract tests, and windowed Vulkan 1.3 validation smoke runs)

---

## 1. What Was Built (P1-01 .. P1-09)

### Build, Tooling & Governance (`P1-01`, `P1-09`)
- Monorepo build configuration (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `Cargo.toml`, `rust-toolchain.toml`, `cbindgen.toml`, `deny.toml`, `.clang-format`, `.clang-tidy`).
- CI and static enforcement scripts:
  - `tools/ci.ps1`: orchestrates static HR/UI token gates, tabular numeral checks, `clang-format --dry-run --Werror`, Rust `fmt`/`clippy` (default & `--all-features`) / `cargo deny --offline check licenses bans sources` / `cargo test` (default & `test-hooks`), `cbindgen` FFI header parity (`tools/gen_ffi.ps1 -CheckOnly`), configure/build/CTest across all 4 presets (`windows-debug`, `windows-relwithdebinfo`, `windows-release`, `ci-validation`), windowed Vulkan validation smoke test, and `clang-tidy` across `src/`.
  - `tools/check_gates.py`: enforces `HR-02` (zero blocking locks in C++ hot modules and Rust crates), `§4.6` (no `Sleep`/`sleep_for`/`sleep_until`), priority-1 crash-free no-abort check, `REQ-UI-001` (zero literal colors outside `src/ui/theme/tokens.hpp`), and `HR-10` (`docs/dependencies_allowlist.toml` parity).
  - `tools/font_tnum_freeze.py`: verifies uniform tabular digit advance widths on bundled `assets/fonts/Inter-Regular-tnum.ttf` (`1328`) and `assets/fonts/Inter-SemiBold-tnum.ttf` (`1325`).

### Rust FFI & Host Boundary (`P1-02`, `P1-03`)
- Frozen C ABI contract generated into `include/aether_ffi_contract.h` from `crates/aether-ffi` (`types.rs`, `status.rs`, `layout_asserts.rs`, `build.rs` emitting compile-time `layout_table.json` verified via `aether_ffi_layout_table_json`).
- Host facade (`crates/aether-rust-host/src/lib.rs`) enforcing `catch_unwind` at every `extern "C"` entry point (`AETHER_E_PANIC`, `HR-04`), TLS `aether_last_error`, debug thread-role verification (`aether_set_thread_role`, `REQ-FFI-003`), Phase 1 production stubs returning `AETHER_E_UNSUPPORTED` (`crates/aether-asset-loader`, `crates/aether-plugin-host`, `HR-09`), two-phase zero-copy slice helpers (`crates/aether-asset-loader/src/ffi_slice.rs`, `bytemuck::cast_slice_mut`), and isolated `test-hooks` verification (`tests/cpp/aether_test_hooks.h`, `crates/aether-rust-host/tests/ffi_tests.rs`).

### C++ Core Runtime & Primitives (`P1-04`, `P1-05`)
- `src/core/expected.hpp`: `Expected<T, Error>` + stack `Error` (`c_str()` / `what()`).
- `src/core/triple_buffer.hpp`: 64-byte cache-line-aligned `LockFreeTripleBuffer<T>` (`CacheLineAligned<T>`).
- `src/core/spsc_ring.hpp`: wait-free `SpscRing<T, N>` (overflow drop-oldest vs drop-newest policies) and atomic epoch `ParamMailbox<T>`.
- `src/core/arena.hpp`: `FrameArena` and `Pool<T, N>` zero-heap-allocation hot-path storage.
- `src/core/hot_path_guard.*`: `AETHER_HOT_PATH_SCOPE` catching heap allocations in Debug (`operator new` + `_CrtSetAllocHook`) and ASan (`__sanitizer_install_malloc_and_free_hooks` + `_CrtSetAllocHook`), using Win32 TEB-slot TLS (`TlsAlloc`) so OS/pre-CRT threads never fault.
- `src/core/clock.*`, `paths.*`, `log.*`, `telemetry.*`, `cpu_topology.*`, `hw_enum.*`, `crash_handler.*`: QPC monotonic clock, high-res waitable timer pacing (zero `Sleep`), KnownFolder paths, per-thread 256-byte lock-free log rings, rolling telemetry percentiles (`RollingLatencyStats`), Intel hybrid P/E-core `CpuSet` detection & placement, DXGI + Vulkan LUID hardware inventory (with Phase 1 `NPU` `AETHER_E_UNSUPPORTED` stub), and out-of-band `MiniDumpWriteDump` handler.

### Vulkan 1.3 RHI, Precision Dark Dev Shell & T0–T4 Skeleton (`P1-06`..`P1-08`)
- `src/rhi/rhi_device.*` & `src/rhi/rhi_swapchain.*`: Vulkan 1.3 instance/device creation, `VK_LAYER_KHRONOS_validation` + `VK_EXT_validation_features` synchronization validation (and optional GPU-AV in `ci-validation`), VMA allocator + ReBAR capability check, timeline semaphore support, and Win32 swapchain (`MAILBOX` -> `IMMEDIATE` -> `FIFO`) with dynamic rendering.
- `src/ui/theme/*` & `src/ui/dev_shell.*`: Precision Dark token system (`tokens.hpp`, `precision_dark.cpp`, `tr.hpp`), zero-growth `ImGuiArenaAllocator`, frozen tabular `Inter` + `JetBrains Mono` typography, and 5-zone dockable Phase 1 Dev Shell (`Live Studio` / `Rigging` / `Nodes` chrome, hardware/FFI/telemetry/log/input panels).
- `src/app/*`: `InputEventRing` (T0 Win32 pump -> T3, dropping oldest mouse moves while preserving key events), `PipelineSkeleton` (T1 60 Hz synthetic capture -> T2 240 Hz sim -> T3 Render/UI -> T4 output latency stats), and `Application` + `main.cpp` supporting `--smoke-frames N` and `--require-validation`.

---

## 2. CI & Test Results (Observed After Final Code Edits)

- **Static Gates (`tools/check_gates.py`, `tools/font_tnum_freeze.py`, `clang-format --dry-run --Werror`)**: Pass (`37` C++ files checked, zero blocking locks / sleeps / literal UI colors, dependency allowlist clean).
- **Rust Workspace**:
  - `cargo fmt --check`: Pass.
  - `cargo clippy --workspace --all-targets --offline -- -D warnings`: Pass.
  - `cargo clippy --workspace --all-features --all-targets --offline -- -D warnings`: Pass.
  - `cargo deny --offline check licenses bans sources`: Pass (`bans ok, licenses ok, sources ok`; duplicate `syn` v2.0/v3.0 warning only).
  - `cargo test --workspace --offline`: `3 passed, 0 failed` (`ffi_tests`).
  - `cargo test --workspace --features test-hooks --offline`: `4 passed, 0 failed` (`ffi_tests`, including panic boundary, two-phase zero-copy borrow, and 0-alloc `aether_graph_evaluate` test plan).
- **`cbindgen` Parity (`tools/gen_ffi.ps1 -CheckOnly`)**: Pass (`include/aether_ffi_contract.h`).
- **`clang-tidy` (`src/*.cpp` against `build/windows-relwithdebinfo/compile_commands.json`)**: Pass (`16` source files / `18` target invocations, `0` errors).
- **CMake Presets & CTest Suites (8/8 C++ tests per tested preset)**:

| Preset | Configure & Build | CTest (8 Suites) | Windowed 30-Frame Smoke |
|---|---|---|---|
| `windows-debug` (`/Od /W4 /WX`) | Pass | `8/8 Passed` (`21.93 s`) | Pass (`--require-validation`) |
| `windows-relwithdebinfo` (`/O2 /Zi`) | Pass | `8/8 Passed` (`9.47 s`) | Pass (`--require-validation`) |
| `windows-release` (`/GL /LTCG`) | Pass (`test-hooks` OFF) | N/A (tests excluded) | Pass (`--smoke-frames 30`) |
| `ci-validation` (ASan + GPU-AV) | Pass | `8/8 Passed` (`15.75 s`) | Pass (`--require-validation`) |

CTest suites executed cleanly in `windows-debug`, `windows-relwithdebinfo`, and `ci-validation`:
`ffi_layout_test`, `zero_copy_borrow_test`, `triple_buffer_test`, `spsc_mailbox_test`, `hot_path_guard_test`, `topology_clock_test`, `rhi_validation_smoke_test`, `input_t0_t3_test`.

---

## 3. Measured Numbers vs. Phase 1 Budgets

Host hardware observed during runs: Windows 11, Intel hybrid CPU, NVIDIA GeForce RTX 5060 Laptop GPU.

| Metric | Budget / Criterion | Measured (`windows-relwithdebinfo` unless noted) | Status |
|---|---|---|---|
| UI CPU time/frame (`HR-11`) | `<= 0.400 ms` | `0.126 ms` mean / `0.169 ms` p95 (`windows-relwithdebinfo` + validation)<br>`0.145 ms` mean / `0.171 ms` p95 (`windows-release`)<br>`0.335 ms` mean / `0.371 ms` p95 (`windows-debug` + validation)<br>`0.331 ms` mean / `0.379 ms` p95 (`ci-validation` ASan + GPU-AV) | **Within budget** across all 4 presets |
| Steady-state ImGui growth (`§12.15`) | `0 B` after warm-up (`5` frames), `0` OS fallbacks | `imgui_steady_growth=0`, `imgui_os_fallbacks=0` (all 4 presets) | **Pass** |
| Hot-path allocations (`HR-01`) | `0` violations during T1–T3 steady loop | `hot_path_violations=0` (all 4 presets) + caught injected new/malloc in `hot_path_guard_test` | **Pass** |
| Vulkan validation + sync (`HR-08`) | `0` errors, `0` warnings/hazards | `vk_errors=0`, `vk_warnings=0` in `rhi_validation_smoke_test` & 30-frame smoke (`windows-debug`, `relwithdebinfo`, `ci-validation`) | **Pass** |
| `LockFreeTripleBuffer` (`REQ-CORE-020`) | `10^8` publish/acquire iterations, `0` torn reads | `iterations=100000000`, `consumer_reads=25628744`, `elapsed_ms=4761.96` (`20007` assertions, `0` failures) | **Pass** |
| Tabular digit width (`REQ-UI`) | Uniform glyph advance for `0`–`9` | `Inter-Regular-tnum.ttf = 1328`, `Inter-SemiBold-tnum.ttf = 1325` | **Pass** |
| Zero-copy borrow & Rust alloc (`HR-05`) | Pointer identity & `0` persistent Rust alloc | Verified in `zero_copy_borrow_test` & `ffi_tests` (`aether_test_thread_alloc_calls() == 0`) | **Pass** |

---

## 4. ADRs, Deviations & Environment Notes

- **ADRs Written**:
  - `docs/adr/0001-toolchain.md` — Pinned C++/Rust/vcpkg/font versions and licenses (`HR-10`).
  - `docs/adr/0002-ffi-test-hooks-and-hot-path-guard.md` — Isolated `test-hooks` FFI probes (`HR-09`) and Debug vs. ASan `HotPathGuard` hook design.
  - `docs/adr/0003-ffi-runtime-and-host-config-structs.md` — Froze `AetherRuntimeConfig` (32 B), `AetherHostConfig` (24 B), `AetherHostCommand` (40 B), `AetherHostEvent` (280 B), `aether_set_thread_role`, and `aether_ffi_layout_table_json`.
- **Deviations / Host Environment Notes**:
  - **`clang-cl` unavailable locally**: VS 2022 BuildTools includes `clang-format` / `clang-tidy` `19.1.5` without `clang-cl`. MSVC `cl.exe` `14.44.35207` (`/std:c++20 /permissive- /W4 /WX`) serves as primary C++ compiler while `clang-tidy` validates static checks over `compile_commands.json` (with `--extra-arg=-Wno-unused-command-line-argument --extra-arg=-Wno-missing-field-initializers` for MSVC-specific flags `/MP` and `/external:anglebrackets` and `/W4` Vulkan aggregate init mapping).
  - **Offline `cargo deny`**: `tools/ci.ps1` runs `cargo deny --offline check licenses bans sources` (skipping online advisory DB fetch cleanly without internet access).
  - **Validation layer log filtering (`src/rhi/rhi_device.cpp`)**: Portable environments running via `VK_ADD_LAYER_PATH` without system-installed LunarG registry entries emit loader portable-registry / GPU-AV initialization notices; these specific informational messages are classified as `AETHER_LOG_INFO` while all validation errors, warnings, and synchronization hazards remain counted as fatal failures in smoke/CI checks.
  - **`ci-validation` runtime DLL path**: Launching `build/ci-validation/src/app/aether_studio.exe` outside `tools/ci.ps1` requires VS MSVC `bin/Hostx64/x64` on `PATH` for `clang_rt.asan_dynamic-x86_64.dll` (otherwise exits `0xC0000135`).

---

## 5. Open Phase 1 Stubs (`HR-09`, `docs/STATUS.md`)

All deferred Phase 2–6 / Post-v1 surfaces remain explicitly stubbed (`AETHER_E_UNSUPPORTED`) and tracked in `docs/STATUS.md`:
- `aether_loader_open` / `query` / `fill` / `progress` / `cancel` / `close` -> **Phase 2** (Asset Loader).
- `HardwareInventory::npu` OpenVINO `NPU` EP probe -> **Phase 3** (Tracking & IK).
- `aether_host_create` / `load_project` / `save_project` / `post_command` / `poll_events` / `destroy` and `aether_graph_compile` / `evaluate` / `release` -> **Phase 6** (Studio UI & Plugin Host).
- `tr()` localization lookup table (`src/ui/theme/tr.hpp`) -> **Post-v1**.
