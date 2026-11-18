#pragma once

#include "renderer/lighting_rig.hpp"

#include <array>
#include <cstdint>

namespace aether::renderer {

enum class CircadianPhase : uint8_t {
    NightStudio = 0, // 18:00 - 05:00 (moonlight rim + soft cyan/lavender monitor key + warm indoor fill)
    Dawn = 1,        // 05:00 - 07:00 (warm rose-peach key + cool periwinkle fill)
    Daytime = 2,     // 07:00 - 15:30 (clean high daylight + sky-blue indirect fill)
    GoldenHour = 3   // 15:30 - 18:00 (golden-amber key + deep violet/magenta shadow + strong warm rim)
};

struct CircadianTimezoneSpec {
    const char* label;
    int16_t utc_offset_minutes; // ignored when is_system_local == true
    bool is_system_local;
};

struct CircadianLightingResult {
    CircadianPhase phase{CircadianPhase::Daytime};
    float local_hour_24{12.0f}; // [0.0, 24.0)
    float key_azimuth_deg{32.0f};
    float key_elevation_deg{26.0f};
    float key_intensity{1.25f};
    float key_kelvin{5800.0f};
    std::array<float, 3> key_rgb{1.0f, 0.98f, 0.96f};

    float fill_azimuth_deg{-52.0f};
    float fill_elevation_deg{12.0f};
    float fill_intensity{0.48f};
    std::array<float, 3> fill_rgb{0.64f, 0.78f, 1.0f};

    float rim_azimuth_deg{148.0f};
    float rim_elevation_deg{22.0f};
    float rim_intensity{0.75f};
    float rim_kelvin{6200.0f};
    std::array<float, 3> rim_rgb{0.85f, 0.92f, 1.0f};

    float shadow_warm_cool_shift{0.75f};
};

/** Total entries in the circadian timezone dropdown (System Local Time + UTC-12:00..UTC+14:00). */
[[nodiscard]] int circadian_timezone_count() noexcept;
[[nodiscard]] const CircadianTimezoneSpec& circadian_timezone_spec(int index) noexcept;

/** '|' delimited enum string for ImGui/ParamRegistry matching `circadian_timezone_spec(i)`. */
[[nodiscard]] const char* circadian_timezone_enum_items() noexcept;
[[nodiscard]] const char* circadian_phase_name(CircadianPhase phase) noexcept;

/** Converts correlated color temperature in Kelvin `[2000, 12000]` to normalized linear RGB. */
[[nodiscard]] std::array<float, 3> kelvin_to_linear_rgb(float kelvin) noexcept;

/**
 * Resolves current fractional hour `[0.0, 24.0)` using either manual `override_minutes`
 * (`>= 0`, clamped `[0, 1439]`) or live wall-clock time for `tz_index` (`GetLocalTime` / `GetSystemTime + offset`).
 */
[[nodiscard]] float resolve_circadian_hour(int tz_index, int override_minutes) noexcept;

/** Smoothly evaluates solar trajectory, EV intensities, Kelvin tinting, and anime shadow palette over 24 hours. */
[[nodiscard]] CircadianLightingResult evaluate_circadian_lighting(float hour_0_to_24) noexcept;

/** Applies evaluated solar/studio rig onto `LightingRig` lights[0..2] (Key, Fill, Rim). */
void apply_circadian_to_rig(const CircadianLightingResult& res, LightingRig& rig) noexcept;

} // namespace aether::renderer
