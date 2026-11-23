#include "lighting/lighting_store.hpp"

#include <algorithm>
#include <cstdio>

namespace aether::lighting {

void StudioLightingStore::set_dominant_key_light_id(uint32_t idx) noexcept {
    const uint32_t n = std::min(rig_.studio_light_count, renderer::kMaxStudioLights);
    rig_.dominant_key_light_id = (n == 0u) ? 0u : std::min(idx, n - 1u);
    ++revision_;
}

void StudioLightingStore::apply_circadian(float hour_0_to_24) noexcept {
    const auto res = renderer::evaluate_circadian_lighting(hour_0_to_24);
    renderer::apply_circadian_to_rig(res, rig_);
    ++revision_;
}

void StudioLightingStore::reset_to_zero_lights() noexcept {
    rig_.studio_light_count = 0u;
    rig_.dominant_key_light_id = 0u;
    rig_.circadian_sun_slot_id = -1;
    rig_.studio_lights = {};
    rig_.ambient_floor = 0.0f;
    rig_.environment_sky_strength = 0.0f;
    rig_.ground_bounce_strength = 0.0f;
    rig_.circadian_enabled = false;
    ++revision_;
}

void StudioLightingStore::set_active_alfx_name(std::string_view name) noexcept {
    active_alfx_name_.fill('\0');
    if (!name.empty()) {
        const size_t n = std::min(name.size(), active_alfx_name_.size() - 1u);
        for (size_t i = 0; i < n; ++i) {
            active_alfx_name_[i] = name[i];
        }
    }
    ++revision_;
}

} // namespace aether::lighting
