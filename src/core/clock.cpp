#include "core/clock.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <intrin.h>
#endif

#include <atomic>

namespace aether {

namespace {

std::atomic<uint64_t> g_qpc_freq_hz{0};

uint64_t load_freq() noexcept {
    uint64_t cached = g_qpc_freq_hz.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached;
    }
    LARGE_INTEGER li{};
    ::QueryPerformanceFrequency(&li);
    const uint64_t freq = li.QuadPart > 0 ? static_cast<uint64_t>(li.QuadPart) : 10'000'000ULL;
    g_qpc_freq_hz.store(freq, std::memory_order_release);
    return freq;
}

} // namespace

uint64_t qpc_frequency_hz() noexcept {
    return load_freq();
}

uint64_t now_us() noexcept {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t freq = load_freq();
    const uint64_t whole_secs = ticks / freq;
    const uint64_t rem_ticks = ticks % freq;
    return whole_secs * 1'000'000ULL + (rem_ticks * 1'000'000ULL) / freq;
}

HighResTimer::HighResTimer() noexcept {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE h = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (h != nullptr) {
        handle_ = h;
        high_res_ = true;
        return;
    }
    HANDLE fallback = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    handle_ = fallback;
    high_res_ = false;
}

HighResTimer::~HighResTimer() noexcept {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

HighResTimer::HighResTimer(HighResTimer&& other) noexcept : handle_(other.handle_), high_res_(other.high_res_) {
    other.handle_ = nullptr;
    other.high_res_ = false;
}

HighResTimer& HighResTimer::operator=(HighResTimer&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = other.handle_;
        high_res_ = other.high_res_;
        other.handle_ = nullptr;
        other.high_res_ = false;
    }
    return *this;
}

void HighResTimer::wait_until_us(uint64_t target_us, uint64_t spin_finish_us) noexcept {
    uint64_t current = now_us();
    if (current >= target_us) {
        return;
    }

    const uint64_t remaining = target_us - current;
    if (handle_ != nullptr && remaining > spin_finish_us) {
        const uint64_t wait_us = remaining - spin_finish_us;
        LARGE_INTEGER due_time{};
        due_time.QuadPart = -static_cast<LONGLONG>(wait_us * 10ULL);
        if (::SetWaitableTimerEx(static_cast<HANDLE>(handle_), &due_time, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
        }
    }

    while (now_us() < target_us) {
        _mm_pause();
    }
}

void HighResTimer::sleep_us(uint64_t duration_us, uint64_t spin_finish_us) noexcept {
    if (duration_us == 0) {
        return;
    }
    wait_until_us(now_us() + duration_us, spin_finish_us);
}

bool HighResTimer::wait_event_or_deadline_us(void* event_handle, uint64_t target_us) noexcept {
    const uint64_t current = now_us();
    if (current >= target_us) {
        if (event_handle != nullptr) {
            return ::WaitForSingleObject(static_cast<HANDLE>(event_handle), 0) == WAIT_OBJECT_0;
        }
        return false;
    }

    if (event_handle == nullptr) {
        wait_until_us(target_us);
        return false;
    }

    const uint64_t remaining = target_us - current;
    constexpr uint64_t kSpinMargin = 150;
    if (handle_ != nullptr && remaining > kSpinMargin) {
        const uint64_t wait_us = remaining - kSpinMargin;
        LARGE_INTEGER due_time{};
        due_time.QuadPart = -static_cast<LONGLONG>(wait_us * 10ULL);
        if (::SetWaitableTimerEx(static_cast<HANDLE>(handle_), &due_time, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            HANDLE handles[2] = {static_cast<HANDLE>(event_handle), static_cast<HANDLE>(handle_)};
            const DWORD res = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (res == WAIT_OBJECT_0) {
                return true;
            }
        }
    }

    while (now_us() < target_us) {
        if (::WaitForSingleObject(static_cast<HANDLE>(event_handle), 0) == WAIT_OBJECT_0) {
            return true;
        }
        _mm_pause();
    }
    return ::WaitForSingleObject(static_cast<HANDLE>(event_handle), 0) == WAIT_OBJECT_0;
}

} // namespace aether
