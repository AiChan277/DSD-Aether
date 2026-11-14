#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "triple_buffer.hpp"

namespace aether {

enum class RingOverflowPolicy : uint8_t {
    /** Incoming item is dropped (returns `false`) when capacity is reached. */
    DropNewest = 0,
    /** Drops non-critical items early so reserved trailing slots remain open for critical events. */
    DropNewestReserveCritical = 1,
};

/**
 * Wait-free single-producer / single-consumer fixed-capacity ring buffer (`HR-01`, `HR-02`).
 * Usable capacity is `Capacity - 1` or full `Capacity` via monotonic indices (`mask = Capacity - 1`).
 */
template <typename T, size_t Capacity> class SpscRing {
  public:
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "SpscRing Capacity must be a power of 2 >= 2");
    static constexpr size_t kCacheLine = 64;

    SpscRing() noexcept = default;
    ~SpscRing() noexcept = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /** Attempts wait-free enqueue by producer. Returns `false` if full (`DropNewest`). */
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_t head = head_.value.load(std::memory_order_relaxed);
        const size_t tail = tail_.value.load(std::memory_order_acquire);
        if ((head - tail) >= Capacity) {
            dropped_.value.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_.value[head & kMask] = item;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    /** Variant that drops low-priority items earlier so the last `Reserve` slots stay open for critical items. */
    [[nodiscard]] bool try_push_with_reserve(const T& item, size_t reserve_slots) noexcept {
        const size_t head = head_.value.load(std::memory_order_relaxed);
        const size_t tail = tail_.value.load(std::memory_order_acquire);
        const size_t used = head - tail;
        const size_t limit = reserve_slots < Capacity ? (Capacity - reserve_slots) : 0;
        if (used >= limit) {
            dropped_.value.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_.value[head & kMask] = item;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    /** Attempts wait-free dequeue by consumer. Returns `false` if empty. */
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const size_t tail = tail_.value.load(std::memory_order_relaxed);
        const size_t head = head_.value.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = buffer_.value[tail & kMask];
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t head = head_.value.load(std::memory_order_acquire);
        const size_t tail = tail_.value.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] uint64_t dropped_count() const noexcept { return dropped_.value.load(std::memory_order_relaxed); }

    void clear() noexcept { tail_.value.store(head_.value.load(std::memory_order_acquire), std::memory_order_release); }

  private:
    static constexpr size_t kMask = Capacity - 1;

    CacheLineAligned<std::atomic<size_t>, kCacheLine> head_{};
    CacheLineAligned<std::atomic<size_t>, kCacheLine> tail_{};
    CacheLineAligned<std::atomic<uint64_t>, kCacheLine> dropped_{};
    CacheLineAligned<std::array<T, Capacity>, kCacheLine> buffer_{};
};

/**
 * Latest-value mailbox for UI/control -> T2/T3 parameter delivery with monotonic epochs
 * (`REQ-CORE-021`, `HR-01`, `HR-02`).
 */
template <typename T> class ParamMailbox {
  public:
    ParamMailbox() noexcept = default;
    ~ParamMailbox() noexcept = default;

    ParamMailbox(const ParamMailbox&) = delete;
    ParamMailbox& operator=(const ParamMailbox&) = delete;

    /** Publishes a new value with strictly incrementing monotonic `epoch` (`>= 1`). */
    uint64_t store(const T& value) noexcept {
        const uint64_t epoch = ++writer_epoch_;
        auto& slot = buffer_.staging();
        slot.epoch = epoch;
        slot.value = value;
        buffer_.publish();
        return epoch;
    }

    /**
     * Consumer poll: if a strictly newer epoch was stored since `last_seen_epoch`,
     * writes the newest snapshot to `out`, advances `last_seen_epoch`, and returns `true`.
     */
    [[nodiscard]] bool poll(uint64_t& last_seen_epoch, T& out) noexcept {
        const bool updated = buffer_.acquire();
        const auto& latest = buffer_.latest();
        if (!updated && latest.epoch <= last_seen_epoch) {
            return false;
        }
        if (latest.epoch > last_seen_epoch) {
            out = latest.value;
            last_seen_epoch = latest.epoch;
            return true;
        }
        return false;
    }

    [[nodiscard]] uint64_t latest_epoch() noexcept {
        (void)buffer_.acquire();
        return buffer_.latest().epoch;
    }

  private:
    struct StampedValue {
        uint64_t epoch{0};
        T value{};
    };

    LockFreeTripleBuffer<StampedValue> buffer_{};
    alignas(64) uint64_t writer_epoch_{0};
};

} // namespace aether
