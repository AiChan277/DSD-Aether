#pragma once

#include <cstdint>

namespace aether {

/** Returns monotonic timestamp in microseconds derived from Win32 QPC (`§4.6`). */
[[nodiscard]] uint64_t now_us() noexcept;

/** Returns monotonic QPC frequency in Hz (cached after first call). */
[[nodiscard]] uint64_t qpc_frequency_hz() noexcept;

/**
 * High-resolution thread-pacing timer backed by Win32 `CreateWaitableTimerExW`
 * (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) — never calls `Sleep()` (`§4.6`).
 */
class HighResTimer {
  public:
    HighResTimer() noexcept;
    ~HighResTimer() noexcept;

    HighResTimer(const HighResTimer&) = delete;
    HighResTimer& operator=(const HighResTimer&) = delete;
    HighResTimer(HighResTimer&& other) noexcept;
    HighResTimer& operator=(HighResTimer&& other) noexcept;

    /** Sleeps until `target_us` (QPC microseconds) with optional final spin threshold. */
    void wait_until_us(uint64_t target_us, uint64_t spin_finish_us = 150) noexcept;

    /** Sleeps for `duration_us` microseconds relative to `now_us()`. */
    void sleep_us(uint64_t duration_us, uint64_t spin_finish_us = 150) noexcept;

    /** Waits for either `event_handle` signaled or `target_us` deadline (`PoseReady` wake). */
    bool wait_event_or_deadline_us(void* event_handle, uint64_t target_us) noexcept;

    [[nodiscard]] bool is_high_res() const noexcept { return high_res_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  private:
    void* handle_{nullptr};
    bool high_res_{false};
};

/**
 * Waits for a Win32 mutex/event handle with sub-millisecond high-res timer backoff up to `timeout_us`
 * (avoiding the 15.6 ms Win32 scheduler quantum on short waits, without `Sleep()`).
 * Returns Win32 `WAIT_OBJECT_0` (0), `WAIT_ABANDONED` (0x80), or `WAIT_TIMEOUT` (0x102).
 */
[[nodiscard]] uint32_t wait_handle_high_res_us(void* win32_handle, HighResTimer& backoff_timer,
                                               uint64_t timeout_us) noexcept;

} // namespace aether
