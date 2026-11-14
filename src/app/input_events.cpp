#include "app/input_events.hpp"

namespace aether::app {

bool InputEventRing::push(const InputEvent& ev) noexcept {
    if (ev.kind == InputEventKind::MouseMove || ev.kind == InputEventKind::MouseWheel) {
        if (!ring_.try_push_with_reserve(ev, kCriticalReserve)) {
            dropped_moves_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    if (!ring_.try_push(ev)) {
        dropped_critical_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool InputEventRing::pop(InputEvent& out) noexcept {
    return ring_.try_pop(out);
}

} // namespace aether::app
