# Phase 6 Report — Studio UI & Plugin Node Host

- **Specification Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-01..12`), §1.3, §3.1–3.2, §5.2, §7 (`7.1–7.8`), §12 (`12.1–12.16`), §13 (Phase 6), §14.5–14.6, Appendices A–D.
- **Status**: Complete (`tools/ci.ps1` green across static gates, Rust checks, `cbindgen`, all 4 CMake presets, 23/23 CTest targets, windowed Vulkan validation smoke runs, and `clang-tidy`; `tests/soak/run_soak.ps1` green).

---

## 1. Subsystems Built & Integrated

### 1.1 Plugin Host, Node Graph Compiler/Evaluator & I/O (`crates/aether-plugin-host`, `crates/aether-ffi`, `crates/aether-rust-host`)
- **FFI Host Contract Extensions** (`crates/aether-ffi` -> [include/aether_ffi_contract.h](file:///d:/Project/DSD-Aether/include/aether_ffi_contract.h)):
  - Added `AETHER_HOST_FLAG_*`, `AETHER_HOST_CMD_*` (`1..=10`), `AETHER_HOST_EVENT_*`, `AetherGraphPlanStats`, `aether_host_evaluate_active`, `aether_host_copy_project_json`, and `aether_host_get_plan_stats` with zero changes to frozen Phase 1 structs.
- **Node Graph Compiler & Zero-Alloc Evaluator** (`crates/aether-plugin-host/src/graph/{model.rs,compiler.rs,eval.rs,builtins.rs,plan_slot.rs}`):
  - Strongly typed DAG (`Event`, `Bool`, `Float`, `Vec3`, `Quat`, `Transform`, `Pose`, `ExprSet`, `VisemeSet`, `Color`, `String`) with explicit coercions, Kahn topological sort, constant folding, cycle detection (`Delay` exempt), built-in MTD nodes, per-op profiling with 3-tick `> 100 us` budget warning, and triple-buffer/plan-slot hot-swap preserving state across `(id, type, version)`.
  - `T2` calls `aether_host_evaluate_active` with zero heap allocations across hot-swaps (`ffi_host_alloc_tests` verified).
- **WASM Sandbox (`wasmtime =36.0.20`, `wit/aether_plugin.wit`)** (`crates/aether-plugin-host/src/wasm/engine.rs`):
  - Component + core module support via `aether:plugin@1.0.0`; `ResourceLimiter` (`<= 64 MiB` linear memory), `1 ms` epoch ticker enforcing `2 ms` trap deadlines, max 64 instances, latched execution off `T2`, SHA-256 verification, and capability consent enforcement.
- **Project Bundle, OSC & Opt-In WebSocket** (`crates/aether-plugin-host/src/{project/{schema.rs,atomic.rs},io/{osc.rs,ws.rs},fuzz_entry.rs}`):
  - Atomic `*.aether/` persistence (`MoveFileExW`), `v1 -> v2` schema migration preserving unknown `#[serde(flatten)]` fields, OSC inbound (`127.0.0.1:9002`, `rosc =0.11.4`, `1000 msg/s` limit), and opt-in authenticated WebSocket control (`ws://127.0.0.1:19190/v1`, `tokio-tungstenite =0.30.0`, bearer token auth).
- **Fuzz Targets** (`fuzz/fuzz_targets/{fuzz_project_toml.rs,fuzz_graph_json.rs,fuzz_osc.rs}`):
  - Verified clean compilation via `cargo check --manifest-path fuzz/Cargo.toml` alongside property-based parser tests (`proptest_untrusted_osc_and_toml_never_panic`).

### 1.2 Out-of-Process Native Plugin Runner (`src/plugin_runner/`)
- **`native_plugin_abi.h`, `runner_main.cpp`, `native_plugin_manager.{hpp,cpp}`**:
  - Hosts third-party native DLLs (`aether_native_plugin_v1`) out-of-process inside `aether-plugin-runner.exe` over SPSC shared-memory rings and named Win32 events.
  - Enforces Win32 Job Object limits (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `512 MiB` commit limit, `ActiveProcessLimit = 1`, `DIE_ON_UNHANDLED_EXCEPTION`, restricted/Safer token isolation).
  - Implements a `500 ms` heartbeat watchdog with exponential backoff (`50 / 150 / 450 ms`) capped at `3` restarts in `300 s` before transitioning to `PluginState::Disabled`.
  - Fixed a multithreaded slot-initialization race where `SlotRuntime::occupied` could be observed by `tick_slot` before IPC handles and `start_requested` were published (`0=Free`, `1=Initializing`, `2=Active` release-acquire state machine).

### 1.3 Precision Dark 5-Zone Studio UI & Engine Wiring (`src/ui/`, `src/app/`)
- **Theme, Fonts & Parameters** (`src/ui/{theme/,params/,widgets/}`):
  - Precision Dark tokens (`tokens.hpp`), animated approach helpers (`anim.hpp`), FreeType merged font atlas (`Inter-Regular-tnum.ttf`, `Inter-SemiBold-tnum.ttf`, `NotoSansJP-Subset.ttf`, `AetherIcons-Regular.ttf`) routed through `ImGuiArenaAllocator` (`0` steady-state growth, `0` OS fallbacks).
  - Single-source `constexpr` parameter table (`param_registry.{hpp,cpp}`) synced with `docs/PARAMS.md` (`55` lines, checked in `tools/ci.ps1`), 200-entry drag-coalescing `UndoStack`, `KeymapRegistry` (collision fix changing `ActionId::OutlinerRename` from `F2` to `Shift+F2`, stack-allocated key parsing with `0` hot-path allocs), and `PropertyRow` / `Feedback` widgets.
- **5-Zone Workspace & Tools** (`src/ui/{workspace/,panels/,palette/,wizard/}`):
  - `StudioWorkspace` across `Live Studio` (`Ctrl+1`), `Rigging` (`Ctrl+2`), and `Nodes` (`Ctrl+3`) with `Shift+Space` tracked via `GImGui->HoveredWindow`.
  - Panels: `OutlinerPanel` (Z1), `ViewportPanel` (Z2 with `128^2` Light Orbit Orb, `Alt+C` CVD skeleton heatmap, `ImGuizmo`, `WM_DROPFILES`), `InspectorPanel` (Z3), `AetherNodeCanvas` + `TimelineConsole` (Z4 with 64-node stress layout, cached Bézier wires, minimap, console), `TelemetryBar` (Z5 24 px bar + `ImPlot` performance popover), `CommandPalette` (`Ctrl+K`), `PreferencesModal`, `ShortcutOverlayModal`, and `HardwareOptimizerWizard`.
  - Integrated into [src/app/application.cpp](file:///d:/Project/DSD-Aether/src/app/application.cpp) and [src/app/pipeline_skeleton.cpp](file:///d:/Project/DSD-Aether/src/app/pipeline_skeleton.cpp) (live `ImPlot` lifecycle, between-frame `WM_DPICHANGED`/theme font rebuild, `AsyncFileDialogJob`, `aether_host_*` event/snapshot polling, `T2` graph evaluation).

---

## 2. Measured Numbers vs Budgets (Observed RelWithDebInfo & CI Runs)

| Metric | Target / Budget | Measured Result | Verification Source |
|---|---|---|---|
| Studio UI CPU (`studio_ui_test`, 64-node stress) | `<= 0.40 ms` p95 (`REQ-UI-020`, `HR-11`) | `p95_ms=0.1427`, `mean_ms=0.0949` (`0.0778`–`0.0882` p95 in soak) | `build/windows-relwithdebinfo/tests/studio_ui_test.exe`, `tests/soak/run_soak.ps1` |
| Windowed App UI CPU (`aether_studio`, empty / VRM, 30f) | `<= 0.40 ms` p95 (`REQ-UI-020`, `HR-11`) | Empty: `mean=0.221 ms`, `p95=0.304 ms`; VRM: `mean=0.131 ms`, `p95=0.174 ms` | `build/windows-relwithdebinfo/src/app/aether_studio.exe` |
| Windowed App UI CPU (`aether_studio`, 300f x2 soak) | `<= 0.40 ms` p95, `0` leaks/growth | Iter 1: `mean=0.137`, `p95=0.199`; Iter 2: `mean=0.188`, `p95=0.239` | `tests/soak/run_soak.ps1` |
| ImGui Arena Steady-State Growth & OS Fallbacks | `0 B` growth, `0` fallbacks (`HR-01`) | `steady_growth=0`, `os_fallbacks=0`, `hot_path_violations=0` | `studio_ui_test`, `aether_studio`, `run_soak.ps1` |
| Precision Dark Accent Pixel Ratio | `<= 5%` (`§12.2`, `§12.16`) | `accent_area_pct=0.0510%` | `studio_ui_test.exe` |
| Font Tabular Numeral Advance Freeze | Uniform 0–9 advance width | `Inter-Regular-tnum.ttf = 1328`, `Inter-SemiBold-tnum.ttf = 1325` | `tools/font_tnum_freeze.py` (`tools/ci.ps1`) |
| Native Plugin Crash/Hang Contained Recovery | `<= 3` restarts in `300 s`, `512 MiB` cap | `43/43` assertions in 2 test cases; `15/15` stress iterations passed | `native_plugin_runner_test.exe` |
| Rust Plugin Host & Zero-Alloc FFI Eval (`REQ-PLG-001/002`, `REL-002`) | `<= 100 ms` recompile, `<= 100 us` 256-node eval, `0` `T2` allocs | `6/6` `phase6_host_tests` + `1/1` `ffi_host_alloc_tests` + `4/4` `ffi_tests` passed | `cargo test --workspace` + `--features test-hooks` |

---

## 3. Verification Matrix (`tools/ci.ps1` & Soak)

- `tools/ci.ps1` (`ALL AETHER-2.5D CI GATES PASSED.` — exit code `0`):
  - `tools/check_gates.py`: passed (`149 C++ files`, Rust crates, UI token isolation, dependency allowlist).
  - `tools/gen_params_doc.py --check`: `docs/PARAMS.md parity OK (55 lines)`.
  - `tools/font_tnum_freeze.py`: passed (`1328` / `1325`).
  - `clang-format --dry-run --Werror`: clean across `src/` & `tests/`.
  - Rust: `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings` (default & `--all-features`), `cargo deny check`, and `cargo test --workspace` (+ `--features test-hooks`): all passed (`aether-asset-loader` 8/8, `aether-plugin-host` 6/6, `aether-ffi` 3/3, `aether-rust-host` `ffi_tests` 4/4 + `ffi_host_alloc_tests` 1/1).
  - `tools/gen_ffi.ps1 -CheckOnly`: passed (`include/aether_ffi_contract.h`).
  - `windows-debug`: CTest `23/23` passed + dual 30-frame Vulkan validation smoke (`vk_errors=0 vk_warnings=0 hot_path_violations=0`).
  - `windows-relwithdebinfo`: CTest `23/23` passed.
  - `windows-release`: LTO build clean.
  - `ci-validation` (ASan + GPU-AV + HotPathGuard): CTest `23/23` passed.
  - `clang-tidy`: 69 C++ translation units under `src/` clean (exit code `0`).
- `cargo check --manifest-path fuzz/Cargo.toml`: passed cleanly (all 7 fuzz targets compile, including `fuzz_project_toml`, `fuzz_graph_json`, `fuzz_osc`).
- `tests/soak/run_soak.ps1 -Preset windows-relwithdebinfo -SmokeFrames 300 -Iterations 2`: `SOAK TEST PASSED` in `13.2 s`.

---

## 4. ADRs & Deviations

- **ADRs**:
  - [docs/adr/0012-phase6-plugin-host-crates.md](file:///d:/Project/DSD-Aether/docs/adr/0012-phase6-plugin-host-crates.md) — Pinned Rust plugin host, WASM, OSC, WS, and project bundle crates (`wasmtime =36.0.20` without `pooling-allocator` for ASan compatibility).
  - [docs/adr/0013-phase6-ui-widgets-and-fonts.md](file:///d:/Project/DSD-Aether/docs/adr/0013-phase6-ui-widgets-and-fonts.md) — `implot`, `imguizmo`, custom `AetherNodeCanvas`, and offline ISC/OFL font generation.
  - [docs/adr/0014-phase6-ffi-host-and-native-plugin-abi.md](file:///d:/Project/DSD-Aether/docs/adr/0014-phase6-ffi-host-and-native-plugin-abi.md) — Additive `AETHER_HOST_*` FFI constants/accessors, native plugin SPSC/Job Object ABI, and authoritative `PARAM_SPECS`.
  - [docs/adr/0015-epoch-gc-and-dx11-renderer.md](file:///d:/Project/DSD-Aether/docs/adr/0015-epoch-gc-and-dx11-renderer.md) — Deterministic C++20 `aether::GarbageCollector` (wait-free SPSC epoch-fenced resource reclamation + incremental tri-color mark-and-sweep with Dijkstra write barrier) and `aether::renderer::Dx11NprRenderer` (DirectX 11 SM 5.0 2.5D NPR cel/rim renderer + DXGI shared-handle interop backend, verified by `gc_and_dx11_renderer_test`, `51/51` assertions across 24/24 CTest targets).
- **CI / Build Reliability Hardening**:
  - Disabled MSVC `/INCREMENTAL` (`/INCREMENTAL:NO`) for Debug and RelWithDebInfo in [CMakeLists.txt](file:///d:/Project/DSD-Aether/CMakeLists.txt#L28-L41) and added transient `LNK1104`/`LNK1168` retry handling (up to 3 attempts, 2 s delay only on transient MSVC binary locks) in [tools/ci.ps1](file:///d:/Project/DSD-Aether/tools/ci.ps1#L92-L115).
  - Note: Phase 6 fuzz targets compile cleanly (`cargo check --manifest-path fuzz/Cargo.toml`) and parser resilience is exercised by `proptest_untrusted_osc_and_toml_never_panic`; extended multi-minute libFuzzer fuzz campaigns on the 3 new Phase 6 fuzz targets were not executed separately during this verification run.

---

## 5. Open Stubs (`HR-09`)

All Phase 6 `aether_host_*` and `aether_graph_*` production stubs have been removed. Only pre-existing optional/post-v1 items remain in [docs/STATUS.md](file:///d:/Project/DSD-Aether/docs/STATUS.md):
- `NdiOutput::init` (without NDI 6 Runtime SDK installed -> `Optional Runtime`).
- `tr()` string table (`src/ui/theme/tr.hpp` English passthrough -> `Post-v1`).
