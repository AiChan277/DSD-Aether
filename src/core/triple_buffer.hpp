#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace aether {

/**
 * Wraps `T` to an exact multiple of `Line` bytes with `alignas(Line)` and explicit tail padding
 * so lock-free structures avoid false sharing without implicit compiler alignment padding.
 */
template <typename T, size_t Line = 64, bool Exact = ((sizeof(T) % Line) == 0)> struct alignas(Line) CacheLineAligned;

template <typename T, size_t Line> struct alignas(Line) CacheLineAligned<T, Line, true> {
    T value{};
};

template <typename T, size_t Line> struct alignas(Line) CacheLineAligned<T, Line, false> {
    T value{};
    std::array<uint8_t, Line - (sizeof(T) % Line)> pad_{};
};

/**
 * Wait-free single-producer / single-consumer triple buffer (`HR-02`, `REQ-CORE-020`).
 *
 * Three slots sit on independent 64-byte cache lines to eliminate false sharing.
 * Indices {producer_idx_, shared_state_ & 0x3, consumer_idx_} form a permutation of {0, 1, 2}.
 */
template <typename T> class LockFreeTripleBuffer {
  public:
    static constexpr size_t kCacheLine = 64;

    LockFreeTripleBuffer() noexcept = default;
    ~LockFreeTripleBuffer() noexcept = default;

    LockFreeTripleBuffer(const LockFreeTripleBuffer&) = delete;
    LockFreeTripleBuffer& operator=(const LockFreeTripleBuffer&) = delete;
    LockFreeTripleBuffer(LockFreeTripleBuffer&&) = delete;
    LockFreeTripleBuffer& operator=(LockFreeTripleBuffer&&) = delete;

    /** Returns mutable reference to the producer's exclusive staging slot (`noexcept`). */
    [[nodiscard]] T& staging() noexcept { return slots_[producer_idx_.value].value; }

    /** Publishes the current staging slot and receives a recycled slot for the next write. */
    void publish() noexcept {
        const uint8_t desired = static_cast<uint8_t>(producer_idx_.value | kDirtyBit);
        const uint8_t prev = shared_state_.value.exchange(desired, std::memory_order_acq_rel);
        producer_idx_.value = static_cast<uint8_t>(prev & kIndexMask);
    }

    /** Convenience helper: copies/moves `val` into staging and publishes atomically. */
    void publish(const T& val) noexcept {
        slots_[producer_idx_.value].value = val;
        publish();
    }

    /**
     * Checks whether the producer published a newer snapshot since the last acquire.
     * Wait-free: if no new frame exists, returns `false` without blocking and keeps
     * `latest()` intact.
     */
    [[nodiscard]] bool acquire() noexcept {
        const uint8_t cur = shared_state_.value.load(std::memory_order_acquire);
        if ((cur & kDirtyBit) == 0) {
            return false;
        }
        const uint8_t prev = shared_state_.value.exchange(consumer_idx_.value, std::memory_order_acq_rel);
        consumer_idx_.value = static_cast<uint8_t>(prev & kIndexMask);
        return true;
    }

    /** Returns immutable reference to the consumer's exclusively owned latest snapshot. */
    [[nodiscard]] const T& latest() const noexcept { return slots_[consumer_idx_.value].value; }

    /** Combined helper: acquires if fresh and returns `(latest(), updated)`. */
    [[nodiscard]] bool read_latest(T& out) noexcept {
        const bool updated = acquire();
        out = slots_[consumer_idx_.value].value;
        return updated;
    }

  private:
    static constexpr uint8_t kIndexMask = 0x03U;
    static constexpr uint8_t kDirtyBit = 0x04U;

    std::array<CacheLineAligned<T, kCacheLine>, 3> slots_{};
    CacheLineAligned<std::atomic<uint8_t>, kCacheLine> shared_state_{std::atomic<uint8_t>{1U}};
    CacheLineAligned<uint8_t, kCacheLine> producer_idx_{0U};
    CacheLineAligned<uint8_t, kCacheLine> consumer_idx_{2U};
};

} // namespace aether
