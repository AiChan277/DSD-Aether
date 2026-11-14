#pragma once

#include "aether_ffi_contract.h"

#ifdef AETHER_ENABLE_TEST_HOOKS

#ifdef __cplusplus
extern "C" {
#endif

/** Resets Rust CountingAllocator cumulative and calling-thread counters. */
void aether_test_alloc_reset(void);

/** Heap allocation count on the calling thread since `aether_test_alloc_reset`. */
uint64_t aether_test_thread_alloc_calls(void);

/** Heap bytes allocated on the calling thread since `aether_test_alloc_reset`. */
uint64_t aether_test_thread_alloc_bytes(void);

/** Total heap allocations performed across all threads since `aether_test_alloc_reset`. */
uint64_t aether_test_alloc_calls(void);

/** Total bytes allocated inside Rust since `aether_test_alloc_reset`. */
uint64_t aether_test_alloc_total_bytes(void);

/** Peak live Rust heap bytes since `aether_test_alloc_reset`. */
uint64_t aether_test_alloc_peak_bytes(void);

/** Current live Rust heap bytes tracked by CountingAllocator. */
uint64_t aether_test_alloc_live_bytes(void);

/** Injects a Rust panic inside `ffi_guard`; must return `AETHER_E_PANIC` cleanly (`HR-04`). */
AetherStatus aether_test_inject_panic(void);

/** Opens a synthetic two-phase zero-copy loader handle strictly for test verification. */
AetherStatus aether_test_loader_open_synthetic(uint32_t vertex_count, uint32_t index_count, AetherLoaderHandle* out);

/** Compiles a deterministic no-op graph plan strictly for zero-alloc T2 verification. */
AetherStatus aether_test_graph_noop_plan(AetherPlanHandle* out);

#ifdef __cplusplus
}
#endif

#endif // AETHER_ENABLE_TEST_HOOKS
