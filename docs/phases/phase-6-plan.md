# Phase 6 Plan — Studio UI & Plugin Node Host

- **Specification Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-01..12`), §1.3, §3.1/3.2/3.6/3.8, §5.2, §7 (`7.1–7.8`), §12 (`12.1–12.16`), §13 (Phase 6), §14.5–14.6, Appendices A–D.
- **Prerequisites**: Phases 1–5 complete and CI-green (`tools/ci.ps1` exit code `0` across all 4 presets + `clang-tidy`); ADRs `0012`, `0013`, `0014` approved.

## 1. Objectives & Task Breakdown

| Task ID | Area | Deliverable | Spec Ref |
|---|---|---|---|
| `P6-01` | Dependencies, Fonts & FFI Host Extensions | Pin `wasmtime =36.0.20`, `tokio =1.54.1`, `rosc =0.11.4`, `tokio-tungstenite =0.30.0`, `futures-util =0.3.34`, `toml =1.2.1+spec-1.1.0`, `sha2 =0.10.9` in `Cargo.toml`; add `implot` (`0.16`) & `imguizmo` (`2024-05-29`) to `vcpkg.json`; update `docs/dependencies_allowlist.toml`; generate `AetherIcons-Regular.ttf` (`ISC`) + `NotoSansJP-Subset.ttf` (`SIL OFL 1.1`) via `tools/gen_ui_fonts.py`; add `AETHER_HOST_*` constants & `AetherGraphPlanStats` in `crates/aether-ffi` + `tools/gen_ffi.ps1` + cross-asserts. | §3.2, §5.2, §12.3.2, §12.3.5, HR-03, HR-10, ADR-0012..0014 |
| `P6-02` | Node Graph Data Model, Compiler & Zero-Alloc Plan (`aether-plugin-host`) | Type-checked DAG (`Event`, `Bool`, `Float`, `Vec3`, `Quat`, `Transform`, `Pose`, `ExprSet`, `VisemeSet`, `Color`, `String`), implicit coercions (`Bool->Float`, `Float->Vec3`, `Transform->Vec3`), Kahn topo sort + constant folding + cycle rejection (`Delay` exempt), flat type-segregated `Values` arena + `Vec<Op>`, state migration by `(id, type, version)` on hot swap, default MTD graph (`NPU Pose -> Visibility-First IK -> Output Pose`, `DSP Audio -> Formant LipSync -> Output Expressions`), full v1 built-in Source/Solver/Expression/Math/Output nodes, per-op `us` profiling & 3-tick `> 100 us` budget warning, 50 ms debounced recompile (`<= 100 ms` `REQ-PLG-001`). | §7.1–7.4, `REQ-PLG-001`, `REQ-PLG-002`, `HR-01` |
| `P6-03` | `wasmtime` Sandbox, WIT `aether:plugin@1.0.0`, `PluginHost` I/O & Project Bundle | `crates/aether-plugin-host/wit/aether_plugin.wit` with `wasmtime` `component::bindgen!`; 2-thread E-core `tokio` runtime; `ResourceLimiter` `<= 64 MiB`, `epoch_interruption` 1 ms ticker / `2 ms` deadline trap, max 64 instances, `Op::Latched` async triple-buffer execution (`<= 120 Hz`, never on `T2`); `plugin.toml` + `sha256` check + capability consent dialog flow (`net.osc.send`, `net.ws.client`, `fs.read:plugin_dir`); atomic `*.aether/` bundle save (`MoveFileExW` replace+write-through), schema migrations (`migrate_v1_to_v2`), `#[serde(flatten)] extra` round-trip, `120 s` / `30 s` ON AIR autosave; OSC inbound `127.0.0.1:9002` (`1000 msg/s` cap) + opt-in WS `127.0.0.1:19190/v1` (token auth, 4 Hz telemetry stream); fuzz targets `fuzz_project_toml`, `fuzz_graph_json`, `fuzz_osc`. | §7.1, §7.5, §7.7, §7.8, `HR-06`, `HR-07` |
| `P6-04` | Out-of-Process Native Plugin Runner (`src/plugin_runner/`) | `native_plugin_abi.h` (`aether_native_plugin_v1`), `aether-plugin-runner.exe` isolated DLL host over SPSC shared-memory rings + named events; host launcher using Win32 Job Object (`512 MiB`, `ACTIVE_PROCESS=1`, `KILL_ON_JOB_CLOSE`) + restricted token; 500 ms heartbeat watchdog killing hung processes and restarting with exponential backoff (max 3 restarts in 5 min -> disable). | §7.6, `HR-07`, ADR-0014 |
| `P6-05` | Theme, `ParamRegistry`, Undo/Redo & Persistence (`src/ui/{theme,params,widgets}/`) | Full §12.3 tokens (incl. high-contrast variant, 9-slice popovers/modals, `ui::anim::approach` fixed table + Reduce Motion), FreeType + Inter Tnum + NotoSansJP + AetherIcons merged atlas; `REQ-UI-010` `constexpr kParamSpecs[]` generating Inspector/TOML/OSC/WS/Palette/Graph/`docs/PARAMS.md` (via `tools/gen_params_doc.py`); 200-entry coalescing Undo/Redo stack; `settings.toml` & `keymaps.toml` persistence with conflict check; `PropertyRow` widget (26 px row, 104 px label, Shift/Ctrl scrub, amber reset arrow, 450 ms tooltip, right-click menu); Toast stack / inline error / determinate progress / empty state / consent modal widgets. | §12.2–12.3, §12.7, §12.12–12.14, `REQ-UI-001`, `REQ-UI-010` |
| `P6-06` | Full 5-Zone Studio Workspace, Modes, Viewports, Node Canvas, Telemetry & Wizard | Refactor `src/ui/` into `workspace/`, `panels/{outliner,viewport,inspector,nodegraph,telemetry}`, `wizard/`, `palette/`: 32 px Menu + Mode bar + `Ctrl+K` + Spout/VCam/NDI pills + ON AIR (`Ctrl+Shift+L`); per-mode `layouts\mode_*.ini` (`Ctrl+1` Live Studio / `Ctrl+2` Rigging / `Ctrl+3` Nodes); Z1 Outliner & Assets & Skeleton Mapping & Node Library & `F1–F8` Expressions; Z2 Viewport chips (`Cam 105 mm` dolly-comp, `Flatness`, `RT`, `Shading`, `Overlays`, Output chip), `128^2` Light Orbit Orb (hemisphere flip, Ctrl 15 deg, Shift lock, wheel EV, arrow nudge, Kelvin menu, live epoch frame-sync), Confidence mini-HUD & `Alt+C` CVD heatmap (colour + solid/dashed/dotted), `ImGuizmo` (`Q/W/E/R/X`), `WM_DROPFILES` + empty state; Z3 Inspector + Rigging tabs (`Bone Mapping`, `SpringBone`, `Colliders`, `Face Class`); Z4 `AetherNodeCanvas` (§12.8 anatomy, 8-slot CVD palette, dot grid, sparklines, cached Bézier wires, reroute, comments, minimap, `ImNodesStyle` alignment) + Rigging `Expression Map` (`64 x M` clipped) + `Timeline` + `Console`; Z5 `24 px` fixed Telemetry Bar (`4 Hz`, `500 ms` hysteresis, `VK_EXT_memory_budget`, clickable `440x320` `ImPlot` Performance Popover + Table toggle); `780x526` 5-step Hardware Optimizer Wizard; `560 px` `Ctrl+K` fzf Command Palette; full integration into `Application` & `PipelineSkeleton` (`T2` graph evaluation + live param mailbox + dynamic DPI `WM_DPICHANGED`). | §12.4–12.15, §14.6, `REQ-UI-020`, `HR-11` |
| `P6-07` | Phase 6 Exit-Gate Verification (`§12.16`, `REL-001/002`, Fuzz, Soak, Full CI) | Rust tests (`REQ-PLG-001/002` `<= 100 us` 256 nodes & zero allocs via `CountingAllocator`, `REL-002` WASM trap/loop/4GB/OOM/panic chaos suite, project migration + atomic save + unknown field round-trip, OSC `1000 msg/s` + WS token auth); fuzz targets `fuzz_project_toml`, `fuzz_graph_json`, `fuzz_osc` compiled + fuzzed cleanly; C++ `native_plugin_runner_test` (`500 ms` kill + `3/5 min` disable + Job Object `512 MiB` OOM containment); C++ `studio_ui_test` verifying every §12.16 checklist item (token lint, layout box parity at 100%/150% + zero text clipping at 200%, keyboard flows, Light Orbit Orb same-frame epoch propagation, `Alt+C` heatmap values, wizard completion, UI CPU `<= 0.4 ms` p95 with 64 nodes & all panels + zero ImGui arena growth, screenshot analysis `<= 5%` accent pixels & no chrome gradients); mini-soak (`tests/soak/run_soak.ps1` + chaos test); clean `tools/ci.ps1` across all presets + `clang-format`/`clang-tidy`/`cargo deny`; `phase-6-report.md` & final `docs/STATUS.md`. | §12.16, §13 Phase 6, §14.5–14.6 |

## 2. Planned File Layout

- `docs/adr/0012-phase6-plugin-host-crates.md`, `0013-phase6-ui-widgets-and-fonts.md`, `0014-phase6-ffi-host-and-native-plugin-abi.md`
- `docs/PARAMS.md` (generated from `src/ui/params/param_registry.hpp` by `tools/gen_params_doc.py`)
- `tools/gen_ui_fonts.py`, `tools/gen_params_doc.py`, `assets/fonts/{AetherIcons-Regular.ttf,AetherIcons-ISC.txt,NotoSansJP-Subset.ttf,NotoSansJP-OFL.txt}`
- `crates/aether-ffi/` (extended host constants + `AetherGraphPlanStats` + 3 additive host functions, zero changes to frozen structs) -> `include/aether_ffi_contract.h`
- `crates/aether-plugin-host/`:
  - `wit/aether_plugin.wit`
  - `src/{lib.rs, error.rs, graph/{mod.rs, model.rs, compiler.rs, eval.rs, builtins.rs}, wasm/{mod.rs, engine.rs}, io/{mod.rs, osc.rs, ws.rs}, project/{mod.rs, schema.rs, atomic.rs}, fuzz_entry.rs}`
  - `examples/plugin_fuzz_runner.rs`
- `fuzz/fuzz_targets/{fuzz_project_toml.rs, fuzz_graph_json.rs, fuzz_osc.rs}`
- `src/plugin_runner/`:
  - `CMakeLists.txt`, `native_plugin_abi.h`, `runner_main.cpp`, `native_plugin_manager.{hpp,cpp}`
- `src/ui/`:
  - `CMakeLists.txt`, `dev_shell.{hpp,cpp}` (facade over modular panels so `Application` integrates cleanly)
  - `theme/{tokens.hpp, icons.hpp, anim.hpp, precision_dark.{hpp,cpp}, tr.hpp}`
  - `params/{param_registry.{hpp,cpp}, undo_stack.{hpp,cpp}, project_settings.{hpp,cpp}}`
  - `widgets/{property_row.{hpp,cpp}, feedback.{hpp,cpp}}`
  - `workspace/{studio_workspace.{hpp,cpp}}`
  - `panels/outliner/{outliner_panel.{hpp,cpp}}`
  - `panels/viewport/{viewport_panel.{hpp,cpp}}`
  - `panels/inspector/{inspector_panel.{hpp,cpp}}`
  - `panels/nodegraph/{node_canvas.{hpp,cpp}, timeline_console.{hpp,cpp}}`
  - `panels/telemetry/{telemetry_bar.{hpp,cpp}}`
  - `wizard/{hardware_optimizer_wizard.{hpp,cpp}}`
  - `palette/{command_palette.{hpp,cpp}}`
- `tests/`:
  - `cpp/{native_plugin_runner_test.cpp, studio_ui_test.cpp, test_native_plugin.cpp}`
  - `soak/{run_soak.ps1}`

## 3. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| `wasmtime` pooling allocator reserving multi-GB VirtualAlloc regions colliding with Windows ASan (`ci-validation`). | Disable `pooling-allocator` feature in `wasmtime` (On-Demand allocator + `memory_reservation(64 MiB)` + `ResourceLimiter`); verify `ci-validation` preset directly on WASM tests. |
| `implot` / `imguizmo` allocating heap memory per frame or hitting warnings under `/W4 /WX` + `clang-tidy`. | Compile external ports via vcpkg (`SYSTEM` includes), verify all allocations route through `ImGuiArenaAllocator`, keep steady-state growth `== 0` after warm-up, and disable `clang-tidy` only on vcpkg headers (`--header-filter` already scopes to `src/`). |
| Custom `std::string` allocations inside `src/ui/` breaking `REQ-UI-020` (`<= 0.4 ms` UI CPU) or `ImGuiArenaAllocator` zero growth. | Use fixed `std::array<char, N>` formatting buffers + `snprintf`, `ImGuiListClipper` for `> 50`-row lists, cached Bézier wire tessellation in `AetherNodeCanvas`, and fixed `ui::anim` slot table. |
| Blocking `T3` on WASM plugin loads, IFileDialog, saves, or hardware benchmarks. | File dialogs run on STA worker thread; saves/loads run in `PluginHost` tokio pool; Wizard 5s benchmark executes asynchronously over pipeline telemetry counters while rendering continues smoothly at frame rate behind the modal. |

## 4. Exit-Gate Tests to Write First

1. **Rust `aether-plugin-host` & `aether-rust-host` Exit-Gate Suite**:
   - DAG compiler + Kahn topo sort + cycle error + `Delay` feedback loop validity + hot-swap state migration by `(id, type, version)`.
   - `REQ-PLG-001`: debounced edit-to-live adoption `<= 100 ms`; `REQ-PLG-002`: 256-node graph `aether_graph_evaluate` executes in `<= 100 us` with **0 allocations** (`CountingAllocator` `aether_test_thread_alloc_calls() == 0`).
   - `REL-002` WASM chaos suite (`aether:plugin@1.0.0` component/core modules): arithmetic transform correctness, deliberate trap, `loop {}` interrupted within `2 ms` deadline (`AETHER_E_PLUGIN_TRAP`), 4 GiB / `> 64 MiB` linear memory OOM denied, SHA-256 mismatch rejection, consent enforcement for `net.*` / `fs.read:*`.
   - `*.aether/` bundle migration (`v1 -> v2`), unknown-field `#[serde(flatten)]` preservation, atomic `MoveFileExW` save, OSC `:9002` parsing + `1000 msg/s` throttle, WebSocket `:19190/v1` token auth rejection & `param.set`/`expression.trigger`/`subscribe` flow.
2. **Native Plugin Runner (`tests/cpp/native_plugin_runner_test.cpp`)**:
   - Loads `test_native_plugin.dll` out-of-process in `aether-plugin-runner.exe`; verifies `describe/init/process` over SPSC SHM; injects crash (`AV`), memory bomb (`> 512 MiB` blocked by Job Object), and `> 500 ms` hang -> killed by watchdog, restarted `<= 3` times, then disabled on 4th failure within 5 minutes with clean host recovery (`aether_studio` completely unaffected).
3. **Studio UI Acceptance & Perf Suite (`tests/cpp/studio_ui_test.cpp`, §12.16)**:
   - Token & font checks (no literal colors outside `tokens.hpp`, `PARAM_SPECS` matches `docs/PARAMS.md` and round-trips OSC/TOML/Inspector).
   - ImGui Test/Input-driven checks: 5-zone geometry parity (`±2 px`) at 100% & 150%, zero text overlap/clipping at minimum panel width and 200% scale, screenshot pixel analysis proving accent coverage `<= 5%` and flat chrome (zero gradients).
   - Interactive checks: Light Orbit Orb drag updates lighting epoch on the exact same frame; `Alt+C` CVD skeleton heatmap values & line styles match solver weights; keyboard-only workflow (`Ctrl+K` palette, `Ctrl+1/2/3` modes, `F1–F8` expressions, `Ctrl+Z/Y` undo coalescing, Output toggles, Hardware Optimizer Wizard 5-step completion).
   - Perf & memory gate (`REQ-UI-020`): all panels open + 64-node graph active achieves UI CPU `<= 0.4 ms` p95 with **zero** `ImGuiArenaAllocator` steady-state growth and zero `os_fallback_allocs` across 240+ frames.
4. **Chaos + Mini-Soak Gate (`tests/soak/run_soak.ps1`)**:
   - Drives live `aether_studio.exe` + OSC controller + graph edits + simulated device recovery + WASM trap injection, asserting zero crashes, handle/memory stability, and bounded frame times.
