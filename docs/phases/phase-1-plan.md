# Phase 1 Plan — Core Scaffold & FFI Contract

- **Spec reference**: `AETHER-TS-001 v1.0` (§3, §4, §5, §12.3, §12.14, §12.15, §13)
- **Phase goal**: Establish the C++20 / Rust 2024 monorepo build, frozen zero-copy FFI contract with compile-time and JSON layout verification, lock-free inter-thread primitives, hot-path allocation guard, clocks/waitable-timer pacing, structured lock-free logging and telemetry, CPU topology/pinning, hardware enumerator, crash handler, T0–T4 synthetic thread skeleton, minimal Vulkan 1.3 RHI with validation + synchronization validation, and the ImGui Dev Shell styled with Precision Dark tokens.

---

## 1. Task Breakdown

| ID | Task | Spec Refs | Exit Criteria Covered |
|---|---|---|---|
| P1-01 | Repository scaffold, CMake root + presets, vcpkg manifest, Cargo workspace, lint/format/deny configs, ADRs, `STATUS.md`, `UI_TOKENS.md`, `ARCHITECTURE.md` | §3.1–§3.6, HR-01/02/03/10 | All 4 presets configure & build warning-free (`/W4 /permissive- /WX`, `-D warnings`); dependency allow-list check passes |
| P1-02 | Rust FFI contract (`crates/aether-ffi`), `build.rs` `layout_table.json` emitter, `cbindgen.toml`, `tools/gen_ffi.ps1`, `ffi_header_check`, Rust layout assertions | §5.1–§5.3, HR-03, REQ-FFI-001..005 | `ffi_header_check` matches checked-in `include/aether_ffi_contract.h`; Rust + C++ compile-time size/align/offset asserts pass; `ffi_layout_test` validates every field against `layout_table.json` |
| P1-03 | Rust host facade (`crates/aether-rust-host`), `ffi_guard` panic boundary, TLS `aether_last_error`, thread-role debug checks, `aether-asset-loader` / `aether-plugin-host` Phase 1 stubs (`AETHER_E_UNSUPPORTED`), test-only hooks (`test-hooks` feature) for panic injection, counting allocator, and zero-copy two-phase borrow probe | §5.4–§5.6, HR-04, HR-05, HR-09, REQ-FFI-010 | Panic injection across FFI returns `AETHER_E_PANIC` without crashing; zero-copy borrow test proves pointer identity (`debug_stream_ptrs[k] == lent_ptr[k]`) and zero persistent Rust allocation >= 1 MiB; stubs return `AETHER_E_UNSUPPORTED` |
| P1-04 | Core lock-free primitives & memory (`expected.hpp`, `triple_buffer.hpp`, `spsc_ring.hpp`, `arena.hpp`, `hot_path_guard.*`) | §4.3–§4.5, HR-01, HR-02, REQ-CORE-020/021/030 | `triple_buffer_test` passes `10^8` publish/acquire iterations with zero torn reads and non-blocking paused-producer checks; `spsc_mailbox_test` verifies ring overflow policies & `ParamMailbox` epochs; `hot_path_guard_test` catches injected heap allocations under `AETHER_HOT_PATH_SCOPE` |
| P1-05 | Core OS runtime (`clock.*`, `paths.*`, `log.*`, `telemetry.*`, `cpu_topology.*`, `hw_enum.*`, `crash_handler.*`) | §4.2, §4.6–§4.10, HR-02, HR-12 | `topology_clock_test` validates QPC monotonic timing, high-res waitable-timer pacing (no `Sleep`), P/E-core CPU set detection and placement logging, 256 B lock-free log rings, telemetry histogram p50/p95/p99, DXGI+Vulkan LUID hardware inventory, and crash minidump setup |
| P1-06 | Minimal Vulkan 1.3 RHI (`src/rhi/*`): instance, debug messenger + sync validation counter, physical/logical device selector, VMA + ReBAR detection, timeline semaphores, Win32 swapchain (`MAILBOX` -> `IMMEDIATE` -> `FIFO`), dynamic rendering helper | §10.1–§10.3, HR-08, REQ-CORE-014 | `rhi_validation_smoke_test` verifies validation + synchronization validation layers load and record 0 errors and 0 hazards across device init, buffer/image transitions, and swapchain/render pass operations |
| P1-07 | Precision Dark UI theme (`src/ui/theme/tokens.hpp`, `precision_dark.cpp`, `tr.hpp`), ImGui UI arena allocator, and Phase 1 Dev Shell (`src/ui/dev_shell.*`) | §12.1–§12.4, §12.14–§12.15, HR-11, REQ-UI-001/020 | Dev Shell renders 5-zone dockable workspace with exact Precision Dark colors/radii/typography (`Inter-Regular-tnum`, `Inter-SemiBold-tnum`, `JetBrainsMono-Regular`), zero literal colors in panel code, zero UI growth allocations after warm-up, UI CPU <= 0.4 ms |
| P1-08 | Application T0–T4 thread skeleton (`src/app/*`: `input_events.*`, `pipeline_skeleton.*`, `application.*`, `main.cpp`) passing synthetic frames end-to-end with `FrameStamps` | §2.2, §2.3, §4.1, §4.10, REQ-CORE-010..014 | `input_t0_t3_test` verifies T0->T3 SPSC input ring (mouse-move drop vs key-event preservation); `aether_studio.exe --smoke-frames 30` cold-starts cleanly, drives T0–T4 synthetic frames with `FrameStamps`, renders Dev Shell cleanly with validation active, and shuts down cleanly |
| P1-09 | CI automation (`tools/ci.ps1`, `tools/font_tnum_freeze.py`, `tools/check_deps.py`) and Phase 1 Verification & Report (`docs/phases/phase-1-report.md`) | §0.4, §3.7, §13 | Full `tools/ci.ps1` green across format, lint, grep gates, dependency allowlist, all 4 presets, all unit/contract/smoke tests, and validation run |

---

## 2. Files to Create

### Root & Configuration
- `.gitignore`
- `CMakeLists.txt`
- `CMakePresets.json`
- `vcpkg.json`
- `Cargo.toml`
- `rust-toolchain.toml`
- `cbindgen.toml`
- `deny.toml`
- `.clang-format`
- `.clang-tidy`

### Docs & Governance
- `docs/adr/0001-toolchain.md`
- `docs/adr/0002-ffi-test-hooks-and-hot-path-guard.md`
- `docs/adr/0003-ffi-runtime-and-host-config-structs.md`
- `docs/STATUS.md`
- `docs/ARCHITECTURE.md`
- `docs/UI_TOKENS.md`
- `docs/dependencies_allowlist.toml`
- `docs/phases/phase-1-report.md` (at phase completion)

### Rust Workspace (`crates/`)
- `crates/aether-ffi/Cargo.toml`, `build.rs`, `src/lib.rs`, `src/types.rs`, `src/status.rs`, `src/layout_asserts.rs`
- `crates/aether-asset-loader/Cargo.toml`, `src/lib.rs`, `src/ffi_slice.rs`
- `crates/aether-plugin-host/Cargo.toml`, `src/lib.rs`
- `crates/aether-rust-host/Cargo.toml`, `src/lib.rs`, `tests/ffi_tests.rs`

### C++ Core, RHI, UI, App (`src/`)
- `src/CMakeLists.txt`
- `src/core/CMakeLists.txt`
- `src/core/expected.hpp`
- `src/core/triple_buffer.hpp`
- `src/core/spsc_ring.hpp`
- `src/core/arena.hpp`
- `src/core/hot_path_guard.hpp`, `src/core/hot_path_guard.cpp`
- `src/core/clock.hpp`, `src/core/clock.cpp`
- `src/core/paths.hpp`, `src/core/paths.cpp`
- `src/core/log.hpp`, `src/core/log.cpp`
- `src/core/telemetry.hpp`, `src/core/telemetry.cpp`
- `src/core/cpu_topology.hpp`, `src/core/cpu_topology.cpp`
- `src/core/hw_enum.hpp`, `src/core/hw_enum.cpp`
- `src/core/crash_handler.hpp`, `src/core/crash_handler.cpp`
- `src/rhi/CMakeLists.txt`
- `src/rhi/rhi_device.hpp`, `src/rhi/rhi_device.cpp`
- `src/rhi/rhi_swapchain.hpp`, `src/rhi/rhi_swapchain.cpp`
- `src/ui/CMakeLists.txt`
- `src/ui/theme/tr.hpp`
- `src/ui/theme/tokens.hpp`
- `src/ui/theme/precision_dark.hpp`, `src/ui/theme/precision_dark.cpp`
- `src/ui/dev_shell.hpp`, `src/ui/dev_shell.cpp`
- `src/app/CMakeLists.txt`
- `src/app/input_events.hpp`, `src/app/input_events.cpp`
- `src/app/pipeline_skeleton.hpp`, `src/app/pipeline_skeleton.cpp`
- `src/app/application.hpp`, `src/app/application.cpp`
- `src/app/main.cpp`

### Tests & Tools (`tests/`, `tools/`)
- `tests/CMakeLists.txt`
- `tests/cpp/aether_test_hooks.h`
- `tests/cpp/ffi_layout_test.cpp`
- `tests/cpp/zero_copy_borrow_test.cpp`
- `tests/cpp/triple_buffer_test.cpp`
- `tests/cpp/spsc_mailbox_test.cpp`
- `tests/cpp/hot_path_guard_test.cpp`
- `tests/cpp/topology_clock_test.cpp`
- `tests/cpp/rhi_validation_smoke_test.cpp`
- `tests/cpp/input_t0_t3_test.cpp`
- `tools/ci.ps1`
- `tools/gen_ffi.ps1`
- `tools/font_tnum_freeze.py`
- `tools/check_gates.py`

---

## 3. Interfaces Touched

- **Frozen C ABI (`include/aether_ffi_contract.h`)**: All §5.2 structs, constants, opaque handles, and `extern "C"` functions preserved intact and verified by `cbindgen` + `ffi_header_check` + `layout_table.json`.
- **Test-only FFI (`tests/cpp/aether_test_hooks.h`)**: Isolated behind `AETHER_ENABLE_TEST_HOOKS` / Rust feature `test-hooks` (never compiled into `windows-release`).
- **Module layering (§2.4)**:
  - `aether_core` depends ONLY on Win32 & C++20 stdlib (+ optional Tracy header/client).
  - `aether_rhi` depends on `aether_core`, Vulkan loader/headers, VMA.
  - `aether_ui` depends on `aether_core`, Dear ImGui, FreeType (zero Vulkan headers in UI panel/theme logic except minimal Vulkan init handle struct passed from app/rhi).
  - `aether_app` orchestrates `aether_core`, `aether_rhi`, `aether_ui`, `aether_rust_host`.

---

## 4. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `ci-validation` ASan (`/fsanitize=address`) interacting with Rust `staticlib` CRT and global `operator new` override | Test ASan preset explicitly; verify `/fsanitize=address` + dynamic CRT `/MDd` or document any MSVC ASan symbol/alloc-hook interaction via `_CrtSetAllocHook` / fallback in `hot_path_guard.cpp` and record in ADR-0002. |
| `clang-cl` not installed in local VS BuildTools (`clang-format` and `clang-tidy` 19.1.5 ARE present) | Document in ADR-0001 and `phase-1-report.md`; enforce `clang-format` + `clang-tidy` + MSVC `/W4 /permissive- /WX` across all 4 presets. |
| Vulkan validation layers not found if `VK_ADD_LAYER_PATH` is unset in test environment | In `tools/ci.ps1` and `rhi_validation_smoke_test`, auto-locate `VkLayer_khronos_validation.json` next to the binary / `vcpkg_installed/bin` and assert in the smoke test that the validation layer actually loaded. |
| Stale `build/` objects from pre-session state | Clean old CMake caches/targets in `build/` (preserving `vcpkg_installed` where manifest matches) before running fresh preset configures and `tools/ci.ps1`. |
| `cargo deny check advisories` failing offline without local advisory DB | Keep strict `cargo deny check licenses bans sources` offline and attempt `advisories` when advisory DB exists or network permits; enforce `docs/dependencies_allowlist.toml` in `tools/ci.ps1`. |

---

## 5. Tests Written First (Encoding Exit-Gate Criteria)

1. `tests/cpp/ffi_layout_test.cpp` (`REQ-FFI-001..005`, `HR-03`): Compile-time `static_assert` on sizes, alignments, and every field offset of all FFI structs + runtime verification against Rust-embedded `layout_table.json` + ABI version check.
2. `tests/cpp/zero_copy_borrow_test.cpp` (`REQ-FFI-010`, `HR-04`, `HR-05`, `HR-09`): Verifies FFI panic injection (`AETHER_E_PANIC`, process survives), `AETHER_E_UNSUPPORTED` on unimplemented Phase 1 production paths, two-phase zero-copy borrow pointer identity (`debug_stream_ptrs[k] == lent_ptr[k]`), and Rust counting allocator showing 0 persistent allocation >= 1 MiB.
3. `tests/cpp/triple_buffer_test.cpp` (`REQ-CORE-020`, `HR-02`): `10^8` publish/acquire iterations across two pinned threads verifying monotonic sequence numbers, zero torn reads (FNV/XOR checksum), and non-blocking consumer when producer is paused.
4. `tests/cpp/spsc_mailbox_test.cpp` (`REQ-CORE-021`, `HR-01`, `HR-02`): Concurrent SPSC ring throughput + overflow policy tests + `ParamMailbox<float>` / `ParamMailbox<int32_t>` atomic epoch change detection.
5. `tests/cpp/hot_path_guard_test.cpp` (`REQ-CORE-030`, `HR-01`): Verifies `AETHER_HOT_PATH_SCOPE` catches an injected heap allocation (`new` / `malloc`), logs call stack context, allows clean `FrameArena` & `Pool<T, N>` allocations inside the hot scope, and checks Rust allocation guard on `aether_graph_evaluate` test plan.
6. `tests/cpp/topology_clock_test.cpp` (`§4.2`, `§4.6`, `§4.7`, `§4.9`): Verifies QPC monotonicity, waitable-timer pacing precision, CPU topology P/E-core enumeration, thread pinning API (`SetThreadSelectedCpuSets` / affinity), 256-byte lock-free logging rings, telemetry histogram percentiles, and `HardwareInventory` snapshot.
7. `tests/cpp/rhi_validation_smoke_test.cpp` (`HR-08`, `§10.1–§10.3`): Verifies Vulkan 1.3 device creation, VMA initialization, ReBAR detection, timeline semaphores, dynamic rendering submission, and explicit assertion that Khronos validation + synchronization validation loaded and reported `0` errors/hazards.
8. `tests/cpp/input_t0_t3_test.cpp` (`§2.2`, `§4.4`, `§12.3`, `§12.15`, `REQ-UI-001/020`): Verifies T0->T3 `InputEventRing` overflow behavior (oldest mouse-move dropped, key events never dropped), ImGui UI arena zero-growth after warm-up, and Precision Dark token integrity.
9. `crates/aether-rust-host/tests/ffi_tests.rs`: Rust-side contract, panic-guard, zero-copy borrow, and counting-allocator property/unit tests.
