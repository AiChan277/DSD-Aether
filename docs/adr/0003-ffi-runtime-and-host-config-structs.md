# ADR-0003: FFI Runtime, Host Config, Command, and Event Struct Layouts

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.4 (Ambiguity Protocol), §5.1, §5.2

## Context

Section §5.2 of `AETHER-TS-001 v1.0` defines the function signatures:
- `aether_runtime_init(const AetherRuntimeConfig* cfg)`
- `aether_host_create(const AetherHostConfig*, AetherHostHandle* out)`
- `aether_host_post_command(AetherHostHandle, const AetherHostCommand*)`
- `aether_host_poll_events(AetherHostHandle, AetherHostEvent* out, uint32_t cap, uint32_t* count)`

and leaves the exact fields of `AetherRuntimeConfig`, `AetherHostConfig`, `AetherHostCommand`, `AetherHostEvent`, plus the helper thread-role setter (`REQ-FFI-003`) and layout-table accessor (`REQ-FFI-005`) to be specified in Rust `crates/aether-ffi` and exported via `cbindgen`.

## Decision

The layouts below are frozen in `crates/aether-ffi` and verified at compile time on both sides and via `layout_table.json`:

1. **`AetherRuntimeConfig` (32 bytes, `struct_size` validation)**:
   - `uint32_t struct_size` (`sizeof(AetherRuntimeConfig)`)
   - `uint32_t log_level` (`0=trace..4=error`)
   - `uint32_t worker_threads` (`0` = E-core heuristic)
   - `uint32_t flags` (`0` = default)
   - `const char* app_data_dir_utf8`
   - `const char* cache_dir_utf8`
2. **`AetherHostConfig` (24 bytes, `struct_size` validation)**:
   - `uint32_t struct_size`
   - `uint32_t worker_threads` (`2` default)
   - `uint16_t osc_in_port` (`9002` default)
   - `uint16_t osc_out_port` (`9001` default)
   - `uint16_t ws_port` (`19190` default)
   - `uint16_t flags` (`0` = WebSocket off)
   - `const char* plugins_dir_utf8`
3. **`AetherHostCommand` (40 bytes, `struct_size` validation)**:
   - `uint32_t struct_size`, `uint32_t kind`, `uint64_t target_id`, `float param_f32`, `uint32_t param_u32`, `const char* payload_utf8`, `uint64_t payload_len`.
4. **`AetherHostEvent` (280 bytes, `struct_size` validation)**:
   - `uint32_t struct_size`, `uint32_t kind`, `uint64_t source_id`, `AetherStatus status`, `float value_f32`, `uint8_t message_utf8[256]`.
5. **Debug thread-affinity & layout-table exports**:
   - `void aether_set_thread_role(uint32_t role)` sets thread-local affinity tag (`AETHER_THREAD_ROLE_ANY=0` .. `UI=5`) checked in debug builds (`REQ-FFI-003`).
   - `const uint8_t* aether_ffi_layout_table_json(uintptr_t* out_len)` returns a `'static` UTF-8 JSON buffer generated at build time by `crates/aether-ffi/build.rs` (`REQ-FFI-005`).

## Consequences

- Every config/command/event struct includes `struct_size` as its first field for deterministic ABI validation across minor revisions.
- All field offsets and sizes match `include/aether_ffi_contract.h` and are asserted on both C++ and Rust sides.
