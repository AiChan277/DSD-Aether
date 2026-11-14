# ADR-0002: Test-Only FFI Hooks and Hot-Path Allocation Guard Mechanism

- **Status**: Accepted
- **Date**: 2026-11-13
- **Spec Reference**: `AETHER-TS-001 v1.0` §0.3 (HR-01, HR-04, HR-05, HR-09), §4.5, §5.6

## Context

Phase 1 exit gates require automated proof of:
1. Catching heap allocations on C++ per-frame hot paths (`REQ-CORE-030`, `AETHER_HOT_PATH_SCOPE`).
2. Catching panics across the FFI boundary (`HR-04`) without crashing the host process.
3. Proving zero-copy pointer identity (`REQ-FFI-010`) and counting Rust heap allocations during FFI calls (`HR-01`, `HR-05`) before real 3D model parsers arrive in Phase 2.
4. Supporting all 4 CMake presets including `ci-validation` with MSVC AddressSanitizer (`/fsanitize=address`).

At the same time, `HR-09` forbids mock data in production code paths and requires frozen contracts in `include/aether_ffi_contract.h` to remain untouched.

## Decision

1. **Test-only FFI hooks isolated behind `AETHER_ENABLE_TEST_HOOKS` / Rust `test-hooks` feature**:
   - `windows-release` sets `AETHER_ENABLE_TEST_HOOKS=OFF`, so test hooks do not exist in shipping builds.
   - `tests/cpp/aether_test_hooks.h` declares test-only `extern "C"` hooks implemented in `crates/aether-rust-host` under `#[cfg(feature = "test-hooks")]`:
     - Counting global allocator (`#[global_allocator]` active only when `test-hooks` is enabled) exposing live bytes, peak bytes, total alloc calls, and reset.
     - `aether_test_inject_panic()` to trigger an unwind caught by `ffi_guard` (`AETHER_E_PANIC`).
     - `aether_test_loader_open_synthetic(...)` to exercise the real `aether_loader_query` -> C++ arena allocation -> `aether_loader_fill` (`ffi_slice.rs` + `bytemuck::cast_slice_mut`) -> `aether_loader_close` zero-copy path without introducing any mock path into `aether_loader_open`.
     - `aether_test_graph_noop_plan()` to exercise `aether_graph_evaluate` under the Rust counting allocator.
   - `include/aether_ffi_contract.h` remains 100% pure and free of test-only symbols.

2. **C++ Hot-Path Allocation Guard (`AETHER_ENABLE_HOT_PATH_GUARD`)**:
   - In non-ASan Debug builds (`windows-debug`), `src/core/hot_path_guard.cpp` overrides global `operator new` / `operator new[]` / `operator delete` / `operator delete[]` (aligned and unaligned) plus installs a CRT debug alloc hook (`_CrtSetAllocHook`) to catch both C++ `new` and `malloc`/`calloc`/`realloc`.
   - Under MSVC ASan (`ci-validation`), MSVC's ASan runtime owns global `operator new` / `malloc` replacement; overriding global `operator new` conflicts with `clang_rt.asan_dynamic`. Therefore, when `__SANITIZE_ADDRESS__` is defined, `hot_path_guard.cpp` installs `__sanitizer_install_malloc_and_free_hooks` (`<sanitizer/allocator_interface.h>`) alongside `_CrtSetAllocHook` and uses Win32 TEB-slot TLS (`TlsAlloc` / `TlsGetValue` / `TlsSetValue` backed by a fixed array) rather than C++ `thread_local`, keeping `AETHER_HOT_PATH_SCOPE` safe for OS/pre-CRT threads in both `windows-debug` and `ci-validation`.

## Consequences

- `include/aether_ffi_contract.h` stays strictly frozen.
- Production `aether_loader_open` / `aether_host_create` / `aether_graph_compile` return `AETHER_E_UNSUPPORTED` in Phase 1 (tracked in `docs/STATUS.md`).
- Both `windows-debug` and `ci-validation` detect injected hot-path allocations cleanly.
