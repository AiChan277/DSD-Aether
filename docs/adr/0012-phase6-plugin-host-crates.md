# ADR-0012: Phase 6 Rust Plugin Host, WASM Sandbox, OSC, WebSocket, and Project Format Crates

- **Status**: Accepted
- **Date**: 2026-11-14
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (`HR-06`, `HR-07`, `HR-10`), §3.2, §7.1–7.8

## Context

Phase 6 replaces the Phase 1 `aether-plugin-host` stubs (`aether_host_*`, `aether_graph_*`) with the full production plugin host, deterministic node-graph compiler/evaluator, sandboxed WASM execution (`aether:plugin@1.0.0`), project bundle serialization (`*.aether/`), OSC inbound listener (`127.0.0.1:9002`), and opt-in WebSocket JSON-RPC server (`ws://127.0.0.1:19190/v1`). Under `HR-10`, every newly introduced Rust crate requires an explicit license, maintenance, binary-size, and security review before pinning in `Cargo.toml` and `docs/dependencies_allowlist.toml`.

## Decision

The following crates are pinned to exact versions in `Cargo.toml` (`[workspace.dependencies]`) and added to `docs/dependencies_allowlist.toml`:

| Crate | Pinned Version | License | Role | Maintenance, Size & Security Notes |
|---|---|---|---|---|
| `wasmtime` | `=36.0.20` (`default-features = false`, features: `cranelift`, `component-model`, `wat`, `runtime`, `std`) | `Apache-2.0 WITH LLVM-exception` | `aether:plugin@1.0.0` WASM/Component sandbox engine (`OptLevel::Speed`, epoch interruption, `ResourceLimiter`) | Bytecode Alliance active LTS release (`>= 26` per §3.2). Disabling `pooling-allocator` avoids multi-gigabyte VirtualAlloc reservations that collide with MSVC AddressSanitizer (`ci-validation`), while explicit `ResourceLimiter` + `memory_reservation(64 MiB)` strictly enforces the §7.5 `64 MiB` memory cap and `2 ms` epoch trap deadline. |
| `tokio` | `=1.54.1` (`rt`, `rt-multi-thread`, `net`, `sync`, `time`, `io-util`) | `MIT` | Async I/O & 2-thread E-core runtime for OSC/WS/autosave/latched WASM (`§7.1`) | Industry-standard async runtime (`tokio-rs`); minimal feature slice avoids unused `fs`/`process`/`signal` bloat. Isolated to `aether-plugin-host`; never invoked on C++ T1–T4 hot paths. |
| `rosc` | `=0.11.4` (`default-features = false`) | `MIT OR Apache-2.0` | Untrusted OSC UDP packet decoder/encoder (`127.0.0.1:9002` in, `OSC Out` node) | Focused zero-`unsafe` Rust OSC parser (`klingtnet/rosc`); minimal binary footprint (~35 KB); covered by `fuzz_osc` (`HR-06`). Disabling `default-features` (`std` -> `time`) drops the unused `time` dependency and eliminates advisory `RUSTSEC-2026-0009` completely (`cargo deny check` = 0 errors). |
| `tokio-tungstenite` | `=0.30.0` (`handshake`) | `MIT` | Opt-in RFC 6455 WebSocket gateway on `ws://127.0.0.1:19190/v1` | Maintained Tokio WebSocket wrapper (`snapview`); bound strictly to `127.0.0.1` by default, disabled until explicit opt-in, and gated by per-session random bearer token auth (`§7.8`). |
| `futures-util` | `=0.3.34` (`std`, `async-await`, `sink`) | `MIT OR Apache-2.0` | Stream/Sink adaptors for `tokio-tungstenite` | `rust-lang/futures-rs` companion crate required for split WebSocket reader/writer tasks cleanly without custom polling boilerplate. |
| `toml` | `=1.2.1+spec-1.1.0` | `MIT OR Apache-2.0` | `project.toml`, `assets.toml`, `plugin.toml`, `settings.toml` parser/serializer | `toml-rs/toml`; validates strict schema types and round-trips unknown fields (`#[serde(flatten)]`) per §7.7; covered by `fuzz_project_toml` (`HR-06`). |
| `sha2` | `=0.10.9` (`std`) | `MIT OR Apache-2.0` | SHA-256 verification of `plugin.wasm` against `plugin.toml` (`§7.5`) and project avatar digests | Pure-Rust RustCrypto `SHA-256`; small, zero unsafe code on default fallback/intrinsics, prevents executing tampered or mismatched WASM plugins. |

## Consequences

- `cargo deny check` remains clean (`MIT`, `Apache-2.0`, `Apache-2.0 WITH LLVM-exception`, `BSD-*`, `ISC`, `Unicode-3.0`, `Zlib` only; crates.io registry only).
- `wasmtime` executes untrusted WASM binaries with zero filesystem/network capabilities unless explicitly consented via `plugin.toml`, preventing crashes or escapes from touching `T2` or `T3`.
- All three Phase 6 network/file parsers (`project.toml`, `graph.json`, `osc`) have dedicated `cargo-fuzz` + Windows fuzz targets (`HR-06`).
