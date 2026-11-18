#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace aether::core {

struct CircadianTimezoneInfo {
    const char* full_label;
    const char* short_label;
    int16_t utc_offset_minutes;
    bool is_system_local;
};

inline constexpr std::array<CircadianTimezoneInfo, 29> kCircadianTimezones = {{
    {"System Local Time", "Local Time", 0, true},
    {"UTC-12:00 (IDLW)", "UTC-12 IDLW", -12 * 60, false},
    {"UTC-11:00 (SST)", "UTC-11 SST", -11 * 60, false},
    {"UTC-10:00 HST (Honolulu)", "UTC-10 HST", -10 * 60, false},
    {"UTC-09:00 AKST (Anchorage)", "UTC-09 AKST", -9 * 60, false},
    {"UTC-08:00 PST (Los Angeles)", "UTC-08 PST", -8 * 60, false},
    {"UTC-07:00 MST (Denver)", "UTC-07 MST", -7 * 60, false},
    {"UTC-06:00 CST (Chicago)", "UTC-06 CST", -6 * 60, false},
    {"UTC-05:00 EST (New York)", "UTC-05 EST", -5 * 60, false},
    {"UTC-04:00 AST", "UTC-04 AST", -4 * 60, false},
    {"UTC-03:00 BRT (Sao Paulo)", "UTC-03 BRT", -3 * 60, false},
    {"UTC-02:00", "UTC-02:00", -2 * 60, false},
    {"UTC-01:00 CVT", "UTC-01 CVT", -1 * 60, false},
    {"UTC+00:00 UTC (London)", "UTC+00 UTC", 0, false},
    {"UTC+01:00 CET (Paris / Berlin)", "UTC+01 CET", 1 * 60, false},
    {"UTC+02:00 EET (Athens)", "UTC+02 EET", 2 * 60, false},
    {"UTC+03:00 MSK / AST", "UTC+03 MSK", 3 * 60, false},
    {"UTC+04:00 GST (Dubai)", "UTC+04 GST", 4 * 60, false},
    {"UTC+05:00 PKT", "UTC+05 PKT", 5 * 60, false},
    {"UTC+05:30 IST (India)", "UTC+05:30 IST", 5 * 60 + 30, false},
    {"UTC+06:00 BST", "UTC+06 BST", 6 * 60, false},
    {"UTC+07:00 WIB (Jakarta / Surakarta)", "UTC+07 WIB", 7 * 60, false},
    {"UTC+08:00 WITA / SGT (Singapore)", "UTC+08 WITA/SGT", 8 * 60, false},
    {"UTC+09:00 WIT / JST (Tokyo)", "UTC+09 WIT/JST", 9 * 60, false},
    {"UTC+09:30 ACST (Adelaide)", "UTC+09:30 ACST", 9 * 60 + 30, false},
    {"UTC+10:00 AEST (Sydney)", "UTC+10 AEST", 10 * 60, false},
    {"UTC+11:00 SBT", "UTC+11 SBT", 11 * 60, false},
    {"UTC+12:00 NZST (Auckland)", "UTC+12 NZST", 12 * 60, false},
    {"UTC+14:00 LINT (Kiritimati)", "UTC+14 LINT", 14 * 60, false},
}};

inline constexpr size_t kCircadianTimezoneCount = kCircadianTimezones.size();

[[nodiscard]] constexpr const CircadianTimezoneInfo& circadian_timezone_at(int index) noexcept {
    const int idx = std::clamp(index, 0, static_cast<int>(kCircadianTimezoneCount) - 1);
    return kCircadianTimezones[static_cast<size_t>(idx)];
}

} // namespace aether::core
