#pragma once

#include <cstdint>

namespace aether {

/**
 * Per-thread heap-allocation guard (`HR-01`, `REQ-CORE-030`, ADR-0002).
 *
 * When `AETHER_ENABLE_HOT_PATH_GUARD == 1`, any heap allocation (`operator new`,
 * `malloc`, `calloc`, `realloc`) executed inside an active `AETHER_HOT_PATH_SCOPE`
 * on that thread records a violation immediately without blocking or allocating.
 */
class HotPathGuard {
  public:
    /** Installs CRT debug allocation hooks (`_CrtSetAllocHook`) idempotently. */
    static void install() noexcept;

    /** Returns total hot-path allocation violations triggered on the calling thread. */
    [[nodiscard]] static uint64_t thread_violations() noexcept;

    /** Resets the calling thread's violation counter (intended for unit tests). */
    static void reset_thread_violations() noexcept;

    /** Returns cumulative violations observed across all threads since process start. */
    [[nodiscard]] static uint64_t total_violations() noexcept;

    /** Resets cumulative process-wide violation counter (intended for unit tests). */
    static void reset_total_violations() noexcept;

    /** Returns true if the calling thread is currently inside at least one `HotPathScope`. */
    [[nodiscard]] static bool is_active() noexcept;

    /** Enters a guarded hot-path section on the calling thread. Prefer `AETHER_HOT_PATH_SCOPE`. */
    static void enter_scope() noexcept;

    /** Leaves a guarded hot-path section on the calling thread. */
    static void exit_scope() noexcept;

    /** Records one violation from external allocation hooks (internal/test use). */
    static void record_violation() noexcept;
};

class HotPathScope {
  public:
    HotPathScope() noexcept {
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        HotPathGuard::enter_scope();
#endif
    }
    ~HotPathScope() noexcept {
#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
        HotPathGuard::exit_scope();
#endif
    }

    HotPathScope(const HotPathScope&) = delete;
    HotPathScope& operator=(const HotPathScope&) = delete;
    HotPathScope(HotPathScope&&) = delete;
    HotPathScope& operator=(HotPathScope&&) = delete;
};

} // namespace aether

#if defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
#define AETHER_HP_CONCAT_INNER(a, b) a##b
#define AETHER_HP_CONCAT(a, b) AETHER_HP_CONCAT_INNER(a, b)
#define AETHER_HOT_PATH_SCOPE ::aether::HotPathScope AETHER_HP_CONCAT(aether_hp_scope_, __LINE__)
#else
#define AETHER_HOT_PATH_SCOPE ((void)0)
#endif
