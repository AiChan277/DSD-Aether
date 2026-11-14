# ADR-0014: Phase 6 FFI Host Commands, Graph Plan Handoff, Native Plugin Runner ABI, and Parameter Registry

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-03`, `HR-04`, `HR-07`), §5.2, §7.1–7.8, §12.12

## Context

Phase 1 froze the core C ABI structs (`AetherVertex25D` 64 B, `AetherTrackingFrame` 2048 B, `AetherDspSyncPacket` 128 B, `AetherHostConfig` 24 B, `AetherHostCommand` 40 B, `AetherHostEvent` 280 B, `AetherGraphInputs`, `AetherGraphOutputs`). Implementing Phase 6 requires:
1. Concrete discriminator constants for `AetherHostConfig::flags`, `AetherHostCommand::kind`, and `AetherHostEvent::kind`, plus additive non-breaking Rust host FFI accessors for reading back loaded project/graph state and T2 lock-free active-plan evaluation.
2. Exact binary C ABI and Win32 IPC contract for out-of-process native plugins (`aether-plugin-runner.exe`, §7.6).
3. Authoritative single-source parameter registry (`REQ-UI-010`) driving UI rows, `project.toml`/`settings.toml`, OSC `/aether/param/<id>`, WebSocket JSON-RPC, Command Palette, and `docs/PARAMS.md`.

## Decision

1. **Frozen Host Command / Event Constants & Additive Host FFI Accessors (`crates/aether-ffi`)**:
   - All existing frozen struct sizes, alignments, and Phase 1–5 function signatures remain **unchanged**.
   - Discriminator constants exported via `crates/aether-ffi/src/status.rs`:
     - `AETHER_HOST_FLAG_WS_ENABLED` (`1 << 0`), `AETHER_HOST_FLAG_OSC_BIND_ANY` (`1 << 1`).
     - `AETHER_HOST_CMD_*` (`1..=10`): `SET_GRAPH_JSON`, `TRIGGER_EXPRESSION`, `SET_EXTERNAL_PARAM`, `GRANT_PLUGIN_CONSENT`, `RELOAD_PLUGINS`, `PUBLISH_TELEMETRY`, `PROJECT_UPDATE_JSON`, `SET_WS_ENABLED`, `SET_ON_AIR`, `UNLOAD_PLUGIN`.
     - `AETHER_HOST_EVT_*` (`1..=11`): `GRAPH_COMPILED`, `GRAPH_COMPILE_ERR`, `PROJECT_LOADED`, `PROJECT_SAVED`, `PLUGIN_LOADED`, `PLUGIN_CONSENT_REQ`, `PLUGIN_TRAP`, `OSC_PARAM`, `WS_COMMAND`, `HOTKEY_EVENT`, `GRAPH_BUDGET_WARN`.
   - Additive Phase 6 C ABI entry points (`crates/aether-ffi` + `crates/aether-rust-host`, exported in `include/aether_ffi_contract.h`):
     - `aether_host_evaluate_active(host, inputs, outputs) -> AetherStatus` (`[T2]`, wait-free & allocation-free acquisition/evaluation of the latest `CompiledGraphPlan` atomic slot inside `PluginHost`).
     - `aether_host_copy_project_json(host, out_buf, cap, out_len) -> AetherStatus` (`[ui]`, copies currently loaded/migrated `*.aether` normalized project+graph JSON snapshot into C++ buffer after `AETHER_HOST_EVT_PROJECT_LOADED`).
     - `aether_host_get_plan_stats(host, out_stats: *mut AetherGraphPlanStats) -> AetherStatus` (`[ui]`, returns latest compiled node count, last compile time `ms`, last eval `us`, error node ID, budget warning flag, and active node timings/live values snapshot for the Z4 Node Graph UI).
     - Struct `AetherGraphPlanStats` (`#[repr(C)]`) asserted on both Rust and C++ sides via `layout_table.json` and `ffi_layout_test` (`HR-03`).
2. **Native Plugin ABI & Out-of-Process IPC (`src/plugin_runner/native_plugin_abi.h`, §7.6)**:
   - Native DLLs never load inside `aether_studio.exe` (`HR-07`); they load solely inside `aether-plugin-runner.exe`, launched suspended under a Win32 Job Object (`512 MiB` memory limit, `ACTIVE_PROCESS = 1`, `KILL_ON_JOB_CLOSE`) and a restricted token (`CreateRestrictedToken`), passing shared-memory section name + Win32 named event names on the command line.
   - Shared-memory section `Local\AetherNativePlugin_<pid>_<idx>` holds atomic `heartbeat_qpc_us`, `active_flag`, and two lock-free single-producer single-consumer rings (`RequestRing` 32x `512 B`, `ResponseRing` 32x `4096 B`) matching C ABI `aether_native_plugin_v1` (`describe`, `init`, `process`, `shutdown`).
   - Host watchdog verifies heartbeat freshness every `<= 50 ms`; `> 500 ms` silence triggers `TerminateJobObject` and exponential-backoff restart (`50 ms`, `150 ms`, `450 ms`), capped at `3` restarts per `300 s` window before permanent disable + error toast.
3. **Single-Source Parameter Registry (`src/ui/params/param_registry.hpp`, `REQ-UI-010`)**:
   - Every parameter in §12.7 is declared once in `constexpr` array `kParamSpecs[]` using frozen Appendix D IDs (`avatar.transform.*`, `render.*`, `output.*`, `dsp.*`, `tracking.*`).
   - Inspector rows, `CommandPalette`, `settings.toml`/`project.toml` serialization, OSC `/aether/param/<id>`, WebSocket `param.set`, Graph exposure, and `docs/PARAMS.md` are generated/validated from this single source.

## Consequences

- `T2` graph evaluation remains strictly zero-allocation and lock-free (`<= 100 us` for `<= 256` nodes), while structural authoring, OSC/WS I/O, WASM compilation, and autosave run off T1–T4 on `PluginHost`'s 2-thread E-core Tokio runtime.
- Native plugins can crash, leak, allocate arbitrarily, or deadlock without ever crashing or stalling `aether_studio.exe`.
- Parameter IDs cannot drift between UI, serialization, OSC/WS, and documentation (`REQ-UI-010`).
