#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace aether::ui::anim {

/**
 * Allocation-free exponential smoothing helper (`§12.3`, `§12.14`).
 * When `reduce_motion` is enabled or difference is tiny, snaps immediately.
 */
[[nodiscard]] inline float approach(float current, float target, float half_life_s, float dt_s,
                                    bool reduce_motion) noexcept {
    if (reduce_motion || dt_s <= 0.0f || half_life_s <= 1e-5f) {
        return target;
    }
    const float diff = target - current;
    if (std::fabs(diff) < 1e-4f) {
        return target;
    }
    const float factor = 1.0f - std::exp((-0.69314718f * dt_s) / half_life_s);
    return current + diff * factor;
}

template <size_t Slots> class AnimBank {
  public:
    [[nodiscard]] float step(size_t slot, float target, float half_life_s, float dt_s, bool reduce_motion) noexcept {
        if (slot >= Slots) {
            return target;
        }
        if (!initialized_[slot]) {
            values_[slot] = target;
            initialized_[slot] = true;
            return target;
        }
        values_[slot] = approach(values_[slot], target, half_life_s, dt_s, reduce_motion);
        return values_[slot];
    }

    void reset(size_t slot, float value) noexcept {
        if (slot < Slots) {
            values_[slot] = value;
            initialized_[slot] = true;
        }
    }

  private:
    std::array<float, Slots> values_{};
    std::array<bool, Slots> initialized_{};
};

} // namespace aether::ui::anim
