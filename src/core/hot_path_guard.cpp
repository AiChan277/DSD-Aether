#include "core/hot_path_guard.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
// clang-format off
#include <Windows.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <malloc.h>
// clang-format on
#endif

#if defined(__SANITIZE_ADDRESS__) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
#include <sanitizer/allocator_interface.h>
#endif

namespace aether {

namespace {

constexpr size_t kMaxGuardedThreads = 256;

struct alignas(64) ThreadHotPathState {
    std::atomic<bool> in_use{false};
    uint32_t scope_depth{0};
    uint32_t bypass_depth{0};
    uint64_t violations{0};
    std::array<uint8_t, 40> pad_{};
};
static_assert(sizeof(ThreadHotPathState) == 64);

std::atomic<uint64_t> g_total_violations{0};
std::atomic<bool> g_hook_installed{false};

#if defined(_WIN32)
std::array<ThreadHotPathState, kMaxGuardedThreads> g_thread_states{};
std::atomic<DWORD> g_tls_slot{TLS_OUT_OF_INDEXES};

DWORD ensure_tls_slot() noexcept {
    DWORD slot = g_tls_slot.load(std::memory_order_acquire);
    if (slot != TLS_OUT_OF_INDEXES) {
        return slot;
    }
    const DWORD fresh = ::TlsAlloc();
    if (fresh == TLS_OUT_OF_INDEXES) {
        return TLS_OUT_OF_INDEXES;
    }
    DWORD expected = TLS_OUT_OF_INDEXES;
    if (!g_tls_slot.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) {
        ::TlsFree(fresh);
        return expected;
    }
    return fresh;
}

/** Reads caller's existing hot-path slot from the Win32 TEB without touching CRT TLS or heap. */
ThreadHotPathState* peek_thread_state() noexcept {
    const DWORD slot = g_tls_slot.load(std::memory_order_acquire);
    if (slot == TLS_OUT_OF_INDEXES) {
        return nullptr;
    }
    const DWORD last_err = ::GetLastError();
    auto* st = static_cast<ThreadHotPathState*>(::TlsGetValue(slot));
    ::SetLastError(last_err);
    return st;
}

ThreadHotPathState* acquire_thread_state() noexcept {
    const DWORD slot = ensure_tls_slot();
    if (slot == TLS_OUT_OF_INDEXES) {
        return nullptr;
    }
    const DWORD last_err = ::GetLastError();
    auto* existing = static_cast<ThreadHotPathState*>(::TlsGetValue(slot));
    if (existing != nullptr) {
        ::SetLastError(last_err);
        return existing;
    }
    for (auto& candidate : g_thread_states) {
        bool expected = false;
        if (candidate.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            candidate.scope_depth = 0;
            candidate.bypass_depth = 0;
            candidate.violations = 0;
            ::TlsSetValue(slot, &candidate);
            ::SetLastError(last_err);
            return &candidate;
        }
    }
    ::SetLastError(last_err);
    return nullptr;
}
#else
thread_local ThreadHotPathState g_posix_state{};
ThreadHotPathState* peek_thread_state() noexcept {
    return &g_posix_state;
}
ThreadHotPathState* acquire_thread_state() noexcept {
    return &g_posix_state;
}
#endif

struct BypassScope {
    ThreadHotPathState* state{nullptr};
    explicit BypassScope(ThreadHotPathState* st) noexcept : state(st) {
        if (state != nullptr) {
            ++state->bypass_depth;
        }
    }
    ~BypassScope() noexcept {
        if (state != nullptr && state->bypass_depth > 0) {
            --state->bypass_depth;
        }
    }
};

#if defined(__SANITIZE_ADDRESS__) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
void SANITIZER_CDECL aether_asan_malloc_hook(const volatile void* /*ptr*/, size_t /*size*/) noexcept {
    ThreadHotPathState* st = peek_thread_state();
    if (st != nullptr && st->bypass_depth == 0 && st->scope_depth > 0) {
        HotPathGuard::record_violation();
    }
}

void SANITIZER_CDECL aether_asan_free_hook(const volatile void* /*ptr*/) noexcept {}
#elif defined(_WIN32) && defined(_DEBUG) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
int __cdecl aether_crt_alloc_hook(int alloc_type, void* /*user_data*/, size_t /*size*/, int block_use,
                                  long /*request_number*/, const unsigned char* /*filename*/,
                                  int /*line_number*/) noexcept {
    ThreadHotPathState* st = peek_thread_state();
    if (st != nullptr && st->bypass_depth == 0 && st->scope_depth > 0 && block_use != _CRT_BLOCK &&
        (alloc_type == _HOOK_ALLOC || alloc_type == _HOOK_REALLOC)) {
        HotPathGuard::record_violation();
    }
    return 1; // Always allow allocation so live process cannot crash inside hook (HR-01 telemetry catches it).
}
#endif

void ensure_installed() noexcept {
#if defined(_WIN32)
    (void)ensure_tls_slot();
#endif
#if defined(__SANITIZE_ADDRESS__) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
    if (!g_hook_installed.exchange(true, std::memory_order_acq_rel)) {
        BypassScope bypass(acquire_thread_state());
        (void)__sanitizer_install_malloc_and_free_hooks(&aether_asan_malloc_hook, &aether_asan_free_hook);
    }
#elif defined(_WIN32) && defined(_DEBUG) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
    if (!g_hook_installed.exchange(true, std::memory_order_acq_rel)) {
        BypassScope bypass(acquire_thread_state());
        (void)_CrtSetAllocHook(&aether_crt_alloc_hook);
    }
#else
    g_hook_installed.store(true, std::memory_order_release);
#endif
}

#if !defined(__SANITIZE_ADDRESS__) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
void* guarded_raw_alloc(size_t size, size_t alignment) noexcept {
    ThreadHotPathState* st = peek_thread_state();
    if (st != nullptr && st->bypass_depth == 0 && st->scope_depth > 0) {
        HotPathGuard::record_violation();
    }
    BypassScope bypass(st);
    const size_t safe_size = size == 0 ? 1U : size;
#if defined(_WIN32)
    return _aligned_malloc(safe_size, alignment);
#else
    return std::aligned_alloc(alignment, safe_size);
#endif
}

void guarded_raw_free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    BypassScope bypass(peek_thread_state());
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
#endif

} // namespace

void HotPathGuard::install() noexcept {
    ensure_installed();
}

uint64_t HotPathGuard::thread_violations() noexcept {
    const ThreadHotPathState* st = peek_thread_state();
    return st != nullptr ? st->violations : 0;
}

void HotPathGuard::reset_thread_violations() noexcept {
    if (ThreadHotPathState* st = acquire_thread_state(); st != nullptr) {
        st->violations = 0;
    }
}

uint64_t HotPathGuard::total_violations() noexcept {
    return g_total_violations.load(std::memory_order_acquire);
}

void HotPathGuard::reset_total_violations() noexcept {
    g_total_violations.store(0, std::memory_order_release);
}

bool HotPathGuard::is_active() noexcept {
    const ThreadHotPathState* st = peek_thread_state();
    return st != nullptr && st->scope_depth > 0;
}

void HotPathGuard::enter_scope() noexcept {
    ensure_installed();
    if (ThreadHotPathState* st = acquire_thread_state(); st != nullptr) {
        ++st->scope_depth;
    }
}

void HotPathGuard::exit_scope() noexcept {
    if (ThreadHotPathState* st = peek_thread_state(); st != nullptr && st->scope_depth > 0) {
        --st->scope_depth;
    }
}

void HotPathGuard::record_violation() noexcept {
    if (ThreadHotPathState* st = peek_thread_state(); st != nullptr) {
        ++st->violations;
    }
    g_total_violations.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aether

#if !defined(__SANITIZE_ADDRESS__) && defined(AETHER_ENABLE_HOT_PATH_GUARD) && AETHER_ENABLE_HOT_PATH_GUARD
void* operator new(std::size_t size) {
    void* p = aether::guarded_raw_alloc(size, alignof(std::max_align_t));
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* operator new[](std::size_t size) {
    void* p = aether::guarded_raw_alloc(size, alignof(std::max_align_t));
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return aether::guarded_raw_alloc(size, alignof(std::max_align_t));
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return aether::guarded_raw_alloc(size, alignof(std::max_align_t));
}

void* operator new(std::size_t size, std::align_val_t align) {
    const size_t a = static_cast<size_t>(align);
    void* p = aether::guarded_raw_alloc(size, a < sizeof(void*) ? sizeof(void*) : a);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* operator new[](std::size_t size, std::align_val_t align) {
    const size_t a = static_cast<size_t>(align);
    void* p = aether::guarded_raw_alloc(size, a < sizeof(void*) ? sizeof(void*) : a);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    const size_t a = static_cast<size_t>(align);
    return aether::guarded_raw_alloc(size, a < sizeof(void*) ? sizeof(void*) : a);
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    const size_t a = static_cast<size_t>(align);
    return aether::guarded_raw_alloc(size, a < sizeof(void*) ? sizeof(void*) : a);
}

void operator delete(void* ptr) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete[](void* ptr) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    aether::guarded_raw_free(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    aether::guarded_raw_free(ptr);
}
#endif
