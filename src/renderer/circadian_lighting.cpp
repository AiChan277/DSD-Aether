#include "renderer/circadian_lighting.hpp"

#include "core/circadian_timezones.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>

namespace aether::renderer {

namespace {

constexpr std::array<CircadianTimezoneSpec, core::kCircadianTimezoneCount> build_timezone_specs() noexcept {
    std::array<CircadianTimezoneSpec, core::kCircadianTimezoneCount> out{};
    for (size_t i = 0; i < core::kCircadianTimezoneCount; ++i) {
        const auto& tz = core::kCircadianTimezones[i];
        out[i] = CircadianTimezoneSpec{tz.full_label, tz.utc_offset_minutes, tz.is_system_local};
    }
    return out;
}

constexpr auto kTimezones = build_timezone_specs();

constexpr const char* kTimezoneEnumPipeString =
    "System Local Time|"
    "UTC-12:00 (IDLW)|UTC-11:00 (SST)|UTC-10:00 HST (Honolulu)|UTC-09:00 AKST (Anchorage)|"
    "UTC-08:00 PST (Los Angeles)|UTC-07:00 MST (Denver)|UTC-06:00 CST (Chicago)|UTC-05:00 EST (New York)|"
    "UTC-04:00 AST|UTC-03:00 BRT (Sao Paulo)|UTC-02:00|UTC-01:00 CVT|"
    "UTC+00:00 UTC (London)|UTC+01:00 CET (Paris / Berlin)|UTC+02:00 EET (Athens)|UTC+03:00 MSK / AST|"
    "UTC+04:00 GST (Dubai)|UTC+05:00 PKT|UTC+05:30 IST (India)|UTC+06:00 BST|"
    "UTC+07:00 WIB (Jakarta / Surakarta)|UTC+08:00 WITA / SGT (Singapore)|UTC+09:00 WIT / JST (Tokyo)|"
    "UTC+09:30 ACST (Adelaide)|UTC+10:00 AEST (Sydney)|UTC+11:00 SBT|UTC+12:00 NZST (Auckland)|"
    "UTC+14:00 LINT (Kiritimati)";

struct CircadianKeyframe {
    float hour;
    float key_az;
    float key_el;
    float key_ev;
    float key_kelvin;
    std::array<float, 3> key_tint;
    float fill_az;
    float fill_el;
    float fill_ev;
    std::array<float, 3> fill_rgb;
    float rim_az;
    float rim_el;
    float rim_ev;
    float rim_kelvin;
    std::array<float, 3> rim_tint;
    float shadow_shift;
};

// Monotonic 24-hour palette keyframes (cyclical wrap at 0.0 == 24.0) guaranteeing zero pops at midnight or dusk.
constexpr std::array<CircadianKeyframe, 9> kKeyframes = {{
    // 00:00 Midnight Studio (moonlight rim + cyan-lavender monitor key + warm indoor fill)
    {0.0f,
     18.0f,
     14.0f,
     1.12f,
     4600.0f,
     {0.86f, 0.93f, 1.00f},
     -48.0f,
     10.0f,
     0.54f,
     {0.92f, 0.72f, 0.62f},
     -138.0f,
     28.0f,
     1.05f,
     6500.0f,
     {0.80f, 0.90f, 1.00f},
     0.78f},
    // 04:30 Pre-Dawn
    {4.5f,
     28.0f,
     12.0f,
     1.10f,
     4400.0f,
     {0.88f, 0.89f, 1.00f},
     -50.0f,
     11.0f,
     0.52f,
     {0.74f, 0.68f, 0.88f},
     -142.0f,
     24.0f,
     0.96f,
     6400.0f,
     {0.82f, 0.88f, 1.00f},
     0.78f},
    // 06:00 Dawn (low eastern rose-peach key + cool periwinkle fill)
    {6.0f,
     52.0f,
     12.0f,
     1.16f,
     3600.0f,
     {1.00f, 0.80f, 0.72f},
     -58.0f,
     14.0f,
     0.56f,
     {0.52f, 0.62f, 0.96f},
     -132.0f,
     18.0f,
     0.88f,
     5200.0f,
     {1.00f, 0.84f, 0.80f},
     0.82f},
    // 08:30 Morning Clean Daylight
    {8.5f,
     36.0f,
     30.0f,
     1.26f,
     5400.0f,
     {1.00f, 0.96f, 0.94f},
     -52.0f,
     12.0f,
     0.48f,
     {0.62f, 0.76f, 0.98f},
     148.0f,
     22.0f,
     0.74f,
     6000.0f,
     {0.86f, 0.94f, 1.00f},
     0.74f},
    // 12:30 Midday Studio Daylight
    {12.5f,
     10.0f,
     46.0f,
     1.32f,
     5800.0f,
     {1.00f, 0.99f, 0.97f},
     -48.0f,
     10.0f,
     0.46f,
     {0.64f, 0.80f, 1.00f},
     155.0f,
     26.0f,
     0.70f,
     6200.0f,
     {0.88f, 0.95f, 1.00f},
     0.72f},
    // 15:30 Late Afternoon Transition
    {15.5f,
     -34.0f,
     26.0f,
     1.26f,
     5000.0f,
     {1.00f, 0.92f, 0.82f},
     54.0f,
     12.0f,
     0.50f,
     {0.62f, 0.68f, 0.96f},
     136.0f,
     20.0f,
     0.84f,
     5200.0f,
     {1.00f, 0.88f, 0.74f},
     0.76f},
    // 17:00 Golden Hour / Sunset (golden-amber key + deep violet/magenta shadow fill + strong warm rim)
    {17.0f,
     -62.0f,
     10.0f,
     1.22f,
     3100.0f,
     {1.00f, 0.72f, 0.44f},
     60.0f,
     14.0f,
     0.58f,
     {0.60f, 0.42f, 0.82f},
     124.0f,
     16.0f,
     1.10f,
     3400.0f,
     {1.00f, 0.70f, 0.42f},
     0.86f},
    // 18:30 Blue Hour -> Night Studio Transition
    {18.5f,
     -38.0f,
     12.0f,
     1.14f,
     4100.0f,
     {0.90f, 0.88f, 1.00f},
     52.0f,
     12.0f,
     0.56f,
     {0.78f, 0.64f, 0.80f},
     -134.0f,
     24.0f,
     1.04f,
     6300.0f,
     {0.80f, 0.90f, 1.00f},
     0.80f},
    // 24:00 Wraps seamlessly back to 00:00
    {24.0f,
     18.0f,
     14.0f,
     1.12f,
     4600.0f,
     {0.86f, 0.93f, 1.00f},
     -48.0f,
     10.0f,
     0.54f,
     {0.92f, 0.72f, 0.62f},
     -138.0f,
     28.0f,
     1.05f,
     6500.0f,
     {0.80f, 0.90f, 1.00f},
     0.78f},
}};

[[nodiscard]] float smooth_step01(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float lerpf(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] std::array<float, 3> lerp3(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                         float t) noexcept {
    return {lerpf(a[0], b[0], t), lerpf(a[1], b[1], t), lerpf(a[2], b[2], t)};
}

[[nodiscard]] float lerp_angle_deg(float a, float b, float t) noexcept {
    float diff = b - a;
    while (diff > 180.0f) {
        diff -= 360.0f;
    }
    while (diff < -180.0f) {
        diff += 360.0f;
    }
    return a + diff * t;
}

[[nodiscard]] float srgb_to_lin_scalar(float u) noexcept {
    u = std::clamp(u, 0.0f, 1.0f);
    return (u <= 0.04045f) ? (u / 12.92f) : std::pow((u + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] std::array<float, 3> mul3(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}

} // namespace

int circadian_timezone_count() noexcept {
    return static_cast<int>(kTimezones.size());
}

const CircadianTimezoneSpec& circadian_timezone_spec(int index) noexcept {
    const int idx = std::clamp(index, 0, circadian_timezone_count() - 1);
    return kTimezones[static_cast<size_t>(idx)];
}

const char* circadian_timezone_enum_items() noexcept {
    return kTimezoneEnumPipeString;
}

const char* circadian_phase_name(CircadianPhase phase) noexcept {
    switch (phase) {
    case CircadianPhase::Dawn:
        return "Dawn";
    case CircadianPhase::Daytime:
        return "Daytime";
    case CircadianPhase::GoldenHour:
        return "Golden Hour";
    case CircadianPhase::NightStudio:
    default:
        return "Night Studio";
    }
}

std::array<float, 3> kelvin_to_linear_rgb(float kelvin) noexcept {
    // Tanner Helland blackbody approximation -> linear light RGB normalized cleanly around white
    const float temp = std::clamp(kelvin, 2000.0f, 12000.0f) / 100.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    if (temp <= 66.0f) {
        r = 1.0f;
        g = std::clamp((99.4708025f * std::log(temp) - 161.119568f) / 255.0f, 0.0f, 1.0f);
        if (temp <= 19.0f) {
            b = 0.0f;
        } else {
            b = std::clamp((138.517731f * std::log(temp - 10.0f) - 305.044792f) / 255.0f, 0.0f, 1.0f);
        }
    } else {
        r = std::clamp((329.698727f * std::pow(temp - 60.0f, -0.133204759f)) / 255.0f, 0.0f, 1.0f);
        g = std::clamp((288.122169f * std::pow(temp - 60.0f, -0.075514849f)) / 255.0f, 0.0f, 1.0f);
        b = 1.0f;
    }
    const float lr = srgb_to_lin_scalar(r);
    const float lg = srgb_to_lin_scalar(g);
    const float lb = srgb_to_lin_scalar(b);
    const float peak = std::max({lr, lg, lb, 1e-4f});
    return {lr / peak, lg / peak, lb / peak};
}

float resolve_circadian_hour(int tz_index, int override_minutes) noexcept {
    if (override_minutes >= 0) {
        const int m = std::clamp(override_minutes, 0, 1439);
        return static_cast<float>(m) / 60.0f;
    }

    const CircadianTimezoneSpec& tz = circadian_timezone_spec(tz_index);
#if defined(_WIN32)
    SYSTEMTIME st{};
    if (tz.is_system_local) {
        GetLocalTime(&st);
        const float h = static_cast<float>(st.wHour) + static_cast<float>(st.wMinute) / 60.0f +
                        static_cast<float>(st.wSecond) / 3600.0f;
        return std::fmod(h + 24.0f, 24.0f);
    }
    GetSystemTime(&st);
    const int utc_minutes = static_cast<int>(st.wHour) * 60 + static_cast<int>(st.wMinute) + tz.utc_offset_minutes;
    const float sec_frac = static_cast<float>(st.wSecond) / 3600.0f;
    float wrapped = std::fmod(static_cast<float>(utc_minutes) / 60.0f + sec_frac, 24.0f);
    if (wrapped < 0.0f) {
        wrapped += 24.0f;
    }
    return wrapped;
#else
    std::time_t t_now = std::time(nullptr);
    std::tm tm_now{};
    if (tz.is_system_local) {
        localtime_r(&t_now, &tm_now);
        return std::fmod(static_cast<float>(tm_now.tm_hour) + static_cast<float>(tm_now.tm_min) / 60.0f, 24.0f);
    }
    gmtime_r(&t_now, &tm_now);
    const int utc_min = tm_now.tm_hour * 60 + tm_now.tm_min + tz.utc_offset_minutes;
    float wrapped = std::fmod(static_cast<float>(utc_min) / 60.0f, 24.0f);
    if (wrapped < 0.0f) {
        wrapped += 24.0f;
    }
    return wrapped;
#endif
}

CircadianLightingResult evaluate_circadian_lighting(float hour_0_to_24) noexcept {
    float h = std::fmod(hour_0_to_24, 24.0f);
    if (h < 0.0f) {
        h += 24.0f;
    }

    size_t k0 = 0;
    size_t k1 = 1;
    for (size_t i = 0; i + 1 < kKeyframes.size(); ++i) {
        if (h >= kKeyframes[i].hour && h <= kKeyframes[i + 1].hour) {
            k0 = i;
            k1 = i + 1;
            break;
        }
    }

    const CircadianKeyframe& a = kKeyframes[k0];
    const CircadianKeyframe& b = kKeyframes[k1];
    const float span = std::max(0.01f, b.hour - a.hour);
    const float t = smooth_step01((h - a.hour) / span);

    CircadianLightingResult out{};
    out.local_hour_24 = h;
    if (h >= 5.0f && h < 7.0f) {
        out.phase = CircadianPhase::Dawn;
    } else if (h >= 7.0f && h < 15.5f) {
        out.phase = CircadianPhase::Daytime;
    } else if (h >= 15.5f && h < 18.0f) {
        out.phase = CircadianPhase::GoldenHour;
    } else {
        out.phase = CircadianPhase::NightStudio;
    }

    out.key_azimuth_deg = lerp_angle_deg(a.key_az, b.key_az, t);
    out.key_elevation_deg = lerpf(a.key_el, b.key_el, t);
    out.key_intensity = lerpf(a.key_ev, b.key_ev, t);
    out.key_kelvin = lerpf(a.key_kelvin, b.key_kelvin, t);
    const auto kt_key = kelvin_to_linear_rgb(out.key_kelvin);
    const auto artistic_key = lerp3(a.key_tint, b.key_tint, t);
    out.key_rgb = lerp3(kt_key, mul3(kt_key, artistic_key), 0.45f);

    out.fill_azimuth_deg = lerp_angle_deg(a.fill_az, b.fill_az, t);
    out.fill_elevation_deg = lerpf(a.fill_el, b.fill_el, t);
    out.fill_intensity = lerpf(a.fill_ev, b.fill_ev, t);
    out.fill_rgb = lerp3(a.fill_rgb, b.fill_rgb, t);

    out.rim_azimuth_deg = lerp_angle_deg(a.rim_az, b.rim_az, t);
    out.rim_elevation_deg = lerpf(a.rim_el, b.rim_el, t);
    out.rim_intensity = lerpf(a.rim_ev, b.rim_ev, t);
    out.rim_kelvin = lerpf(a.rim_kelvin, b.rim_kelvin, t);
    const auto kt_rim = kelvin_to_linear_rgb(out.rim_kelvin);
    out.rim_rgb = lerp3(kt_rim, lerp3(a.rim_tint, b.rim_tint, t), 0.55f);

    out.shadow_warm_cool_shift = lerpf(a.shadow_shift, b.shadow_shift, t);
    return out;
}

void apply_circadian_to_rig(const CircadianLightingResult& res, LightingRig& rig) noexcept {
    ensure_studio_lights_initialized(rig);

    int32_t sun_idx = rig.circadian_sun_slot_id;
    if (sun_idx < 0 || static_cast<uint32_t>(sun_idx) >= rig.studio_light_count) {
        sun_idx = -1;
        for (uint32_t i = 0; i < rig.studio_light_count; ++i) {
            if (rig.studio_lights[i].fixture == StudioFixtureProfile::NaturalSunCieSky) {
                sun_idx = static_cast<int32_t>(i);
                break;
            }
        }
        if (sun_idx < 0) {
            sun_idx = (rig.dominant_key_light_id < rig.studio_light_count)
                          ? static_cast<int32_t>(rig.dominant_key_light_id)
                          : 0;
        }
    }

    const uint32_t uidx = static_cast<uint32_t>(std::clamp(sun_idx, 0, static_cast<int32_t>(kMaxStudioLights - 1u)));
    const bool new_slot = (uidx >= rig.studio_light_count);
    if (new_slot) {
        rig.studio_light_count = uidx + 1u;
        rig.studio_lights[uidx] = StudioLightSpec{};
    }

    auto& sun = rig.studio_lights[uidx];
    if (new_slot || sun.fixture != StudioFixtureProfile::NaturalSunCieSky) {
        configure_fixture_defaults(sun, StudioFixtureProfile::NaturalSunCieSky);
        std::snprintf(sun.name.data(), sun.name.size(), "%s", "Circadian Solar Sun");
    }
    sun.passive_flag = PassiveFlagMode::None;
    sun.enabled = true;
    sun.cast_shadow = true;
    sun.use_orbit_angles = true;
    sun.orbit_azimuth_deg = res.key_azimuth_deg;
    sun.orbit_elevation_deg = res.key_elevation_deg;
    sun.tint_rgb = res.key_rgb;
    sun.kelvin_cct = res.key_kelvin;
    sun.intensity_ev100 = res.key_intensity;

    rig.circadian_sun_slot_id = static_cast<int32_t>(uidx);
    rig.shadow_warm_cool_shift = res.shadow_warm_cool_shift;
}

} // namespace aether::renderer
