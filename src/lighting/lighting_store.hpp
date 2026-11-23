#pragma once

#include "renderer/circadian_lighting.hpp"
#include "renderer/lighting_rig.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace aether::lighting {

/**
 * Single Lighting Authority for AETHER-2.5D Studio (Rev 3.1 §2.1-2.3).
 *
 * Owns the canonical 16-slot `StudioLightSpec` array (including passive flags and any locked
 * Circadian `NaturalSunCieSky` slot), environment/ambient floor terms, Satsuei optical composite,
 * 2D Filter Pro, and VRM-to-MMD visual parity configuration. No parallel Key/Fill/Rim rig or
 * phantom fallback emitter exists outside this store.
 */
class StudioLightingStore {
  public:
    StudioLightingStore() noexcept = default;
    explicit StudioLightingStore(const renderer::LightingRig& initial_rig) noexcept : rig_(initial_rig) {}

    [[nodiscard]] renderer::LightingRig& rig() noexcept { return rig_; }
    [[nodiscard]] const renderer::LightingRig& rig() const noexcept { return rig_; }

    [[nodiscard]] uint32_t active_light_count() const noexcept { return rig_.active_studio_light_count(); }

    [[nodiscard]] uint32_t total_slot_count() const noexcept {
        return std::min(rig_.studio_light_count, renderer::kMaxStudioLights);
    }

    [[nodiscard]] const renderer::StudioLightSpec* dominant_key_light() const noexcept {
        return rig_.dominant_key_light();
    }

    [[nodiscard]] renderer::StudioLightSpec* dominant_key_light_mut() noexcept { return rig_.dominant_key_light_mut(); }

    [[nodiscard]] uint32_t dominant_key_light_id() const noexcept { return rig_.dominant_key_light_id; }

    void set_dominant_key_light_id(uint32_t idx) noexcept;

    [[nodiscard]] int32_t circadian_sun_slot_id() const noexcept { return rig_.circadian_sun_slot_id; }

    /**
     * Drives the single locked `NaturalSunCieSky` slot inside `lights[]` when Circadian is active
     * without overwriting unrelated studio lights (Rev 3.1 §2.3).
     */
    void apply_circadian(float hour_0_to_24) noexcept;

    /** Explicitly clears all light slots and zeroes environment/ambient floor for Zero-Light isolation. */
    void reset_to_zero_lights() noexcept;

    [[nodiscard]] renderer::VrmMmdParityParams& vrm_mmd_parity() noexcept { return rig_.vrm_mmd_parity; }
    [[nodiscard]] const renderer::VrmMmdParityParams& vrm_mmd_parity() const noexcept { return rig_.vrm_mmd_parity; }

    [[nodiscard]] std::string_view active_alfx_name() const noexcept { return active_alfx_name_.data(); }
    void set_active_alfx_name(std::string_view name) noexcept;

    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    void mark_dirty() noexcept { ++revision_; }

  private:
    renderer::LightingRig rig_{renderer::LightingRig::default_three_point()};
    std::array<char, 64> active_alfx_name_{"Three-Point Broadcast"};
    uint64_t revision_{1};
};

} // namespace aether::lighting
