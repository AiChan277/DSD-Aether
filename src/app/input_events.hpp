#pragma once

#include "core/spsc_ring.hpp"

#include <cstdint>

namespace aether::app {

enum class InputEventKind : uint8_t {
    KeyDown = 0,
    KeyUp = 1,
    CharInput = 2,
    MouseMove = 3,
    MouseButton = 4,
    MouseWheel = 5,
    Focus = 6,
    DpiChanged = 7,
    Resize = 8,
};

struct InputEvent {
    InputEventKind kind{InputEventKind::MouseMove};
    uint8_t button_or_state{0};
    uint16_t reserved{0};
    uint32_t code{0}; // Win32 VK or UTF-32 char or DPI
    float x{0.0f};
    float y{0.0f};
    uint64_t timestamp_us{0};
};

/**
 * Lock-free T0 -> T3 input ring (`REQ-CORE-011`, `HR-01`, `HR-02`).
 * High-frequency continuous events (`MouseMove`, `MouseWheel`) drop when utilization
 * exceeds `(Capacity - kCriticalReserve)` so discrete critical events (`KeyDown`,
 * `KeyUp`, `MouseButton`, `Focus`, `Resize`) are preserved even under heavy mouse floods.
 */
class InputEventRing {
  public:
    static constexpr size_t kCapacity = 512;
    static constexpr size_t kCriticalReserve = 64;

    bool push(const InputEvent& ev) noexcept;
    bool pop(InputEvent& out) noexcept;

    [[nodiscard]] size_t size_approx() const noexcept { return ring_.size_approx(); }
    [[nodiscard]] uint64_t dropped_moves() const noexcept { return dropped_moves_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_critical() const noexcept {
        return dropped_critical_.load(std::memory_order_relaxed);
    }

  private:
    SpscRing<InputEvent, kCapacity> ring_{};
    std::atomic<uint64_t> dropped_moves_{0};
    std::atomic<uint64_t> dropped_critical_{0};
};

} // namespace aether::app
