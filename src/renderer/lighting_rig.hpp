#pragma once

#include "renderer/studio_lighting.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace aether::renderer {

/** VRM-to-MMD 2.5D Visual Parity parameters (Rev 3.1 §8.3, serialized in .alfx). */
struct VrmMmdParityParams {
    bool auto_toon_harmonizer_enabled{true};
    float vrm_cel_crispness_boost{0.85f};
    float vrm_shadow_chroma_lift{0.22f};
    bool vrm_auto_edge_scale_enabled{true};
    float vrm_hair_angel_ring_boost{0.75f};
    bool pmx_alpha_bleed_padding{true};
    bool pmx_strict_material_draw_order{true};
};

struct LightingRig {

    // Spec §10 & 2D Hand-Drawn Live2D Illustration look parameters
    float global_live2d_flatness{0.75f};
    float face_normal_flatten{0.85f};
    float neck_seam_blend{1.00f};
    float cel_band_sharpness{0.025f};
    float shadow_warm_cool_shift{0.75f};
    float iro_tsuke_strength{0.85f};
    float crease_strength{0.75f};
    float hair_highlight_intensity{0.85f};
    float rim_step_strength{0.65f};
    float sdf_feather{0.08f};
    float hair_shadow_offset_px{6.0f};
    float hair_shadow_strength{0.60f};
    float rim_t_max{0.45f};
    float bounce_t_max{0.25f};
    float bounce_strength{0.35f};
    float toon_ramp_shift{-0.05f};
    float toon_ramp_toony{0.85f};
    float outline_width_scale{1.0f};
    float crease_threshold_deg{35.0f};
    float bloom_threshold{1.05f};
    float bloom_intensity{0.06f};
    uint32_t tone_mode{1}; // 0 = Standard clamp, 1 = Soft highlights (AgX shoulder)
    float bg_rgba[4]{0.0f, 0.0f, 0.0f, 0.0f};

    // 3-tier lighting, pre-quantized GI, Filter Pro, circadian, catchlights & [TESTING] crease disguise
    uint8_t lighting_mode{1}; // 0 = Traditional (Raster+SDF), 1 = Hybrid RT, 2 = 2.5D Path Tracing
    float eye_catchlight_intensity{0.85f};   // Cornea catchlight reflection brightness (0 = muted)
    float eye_catchlight_size{1.00f};        // Cornea catchlight angular scale (0.25..2.0)
    bool crease_disguise_enabled{false};     // [TESTING] Disguise Low-Poly Creases (default OFF)
    float crease_disguise_strength{0.70f};   // 0..1 blending toward safely welded N_healed when enabled
    bool enable_2d_filter_pro{true};
    float painterly_flatten_strength{0.55f};
    float dog_ink_boost{0.65f};
    bool enable_paper_warmth{false};
    float paper_warmth_strength{0.20f};
    float indirect_bleed_strength{0.75f};
    float rim_depth_offset_px{3.0f};
    bool circadian_enabled{false};
    int32_t circadian_timezone{0};
    int32_t circadian_time_minutes{-1}; // -1 = Live Clock, 0..1439 = Manual preview scrub

    // Single Lighting Authority: 16-slot StudioLightingStore + Environment Sky/Ambient Floor + Parity
    std::array<StudioLightSpec, kMaxStudioLights> studio_lights{};
    uint32_t studio_light_count{0};
    uint32_t dominant_key_light_id{0};
    float ambient_floor{0.28f};
    float environment_sky_strength{0.28f};
    float ground_bounce_strength{0.15f};
    float multi_light_soft_knee_threshold{0.85f};
    SplitLayerGradingParams split_grading{};
    SatsueiCompositeParams satsuei{};
    VrmMmdParityParams vrm_mmd_parity{};

    [[nodiscard]] const StudioLightSpec* dominant_key_light() const noexcept {
        const uint32_t n = std::min(studio_light_count, kMaxStudioLights);
        if (n == 0u) {
            return nullptr;
        }
        bool any_solo = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (studio_lights[i].solo && studio_lights[i].enabled) {
                any_solo = true;
                break;
            }
        }
        auto is_active_emitter = [any_solo](const StudioLightSpec& s) noexcept {
            if (!s.enabled || s.passive_flag != PassiveFlagMode::None) {
                return false;
            }
            return !any_solo || s.solo;
        };
        if (dominant_key_light_id < n && is_active_emitter(studio_lights[dominant_key_light_id])) {
            return &studio_lights[dominant_key_light_id];
        }
        for (uint32_t i = 0; i < n; ++i) {
            if (is_active_emitter(studio_lights[i])) {
                return &studio_lights[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] StudioLightSpec* dominant_key_light_mut() noexcept {
        return const_cast<StudioLightSpec*>(dominant_key_light());
    }

    [[nodiscard]] uint32_t active_studio_light_count() const noexcept {
        const uint32_t n = std::min(studio_light_count, kMaxStudioLights);
        bool any_solo = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (studio_lights[i].solo && studio_lights[i].enabled) {
                any_solo = true;
                break;
            }
        }
        uint32_t active = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const auto& s = studio_lights[i];
            if (s.enabled && (!any_solo || s.solo) && s.passive_flag == PassiveFlagMode::None) {
                ++active;
            }
        }
        return active;
    }

    [[nodiscard]] static LightingRig default_three_point() noexcept {
        LightingRig rig{};
        rig.rim_step_strength = 0.42f;
        rig.hair_highlight_intensity = 0.65f;
        rig.studio_light_count = 4;
        rig.dominant_key_light_id = 0;

        auto set_name = [](std::array<char, 40>& dst, const char* src) noexcept {
            dst.fill('\0');
            for (size_t i = 0; i + 1 < dst.size() && src[i] != '\0'; ++i) {
                dst[i] = src[i];
            }
        };

        // 0: Key Light (RectSoftbox calibrated at ~1.0 EV)
        {
            auto& s = rig.studio_lights[0];
            s = StudioLightSpec{};
            set_name(s.name, "Key Light");
            s.fixture = StudioFixtureProfile::RectSoftbox;
            s.enabled = true;
            s.cast_shadow = true;
            s.use_orbit_angles = true;
            s.orbit_azimuth_deg = 32.0f;
            s.orbit_elevation_deg = 26.0f;
            s.orbit_distance_m = 2.15f;
            s.tint_rgb = {1.0f, 0.98f, 0.95f};
            s.intensity_ev100 = 1.00f;
            s.width_or_outer_m = 0.90f;
            s.height_or_inner_m = 0.60f;
            s.softbox_enabled = true;
            s.catchlight_weight = 1.0f;
        }
        // 1: Soft Fill Light
        {
            auto& s = rig.studio_lights[1];
            s = StudioLightSpec{};
            set_name(s.name, "Soft Fill");
            s.fixture = StudioFixtureProfile::LanternChinaBall;
            s.enabled = true;
            s.cast_shadow = false;
            s.use_orbit_angles = true;
            s.orbit_azimuth_deg = -52.0f;
            s.orbit_elevation_deg = 12.0f;
            s.orbit_distance_m = 2.15f;
            s.tint_rgb = {0.72f, 0.83f, 1.0f};
            s.intensity_ev100 = 0.24f;
            s.width_or_outer_m = 0.45f;
            s.height_or_inner_m = 0.45f;
            s.softbox_enabled = true;
            s.catchlight_weight = 0.25f;
        }
        // 2: Occluded Rim Light
        {
            auto& s = rig.studio_lights[2];
            s = StudioLightSpec{};
            set_name(s.name, "Rim Light");
            s.fixture = StudioFixtureProfile::Stripbox;
            s.enabled = true;
            s.cast_shadow = false;
            s.use_orbit_angles = true;
            s.orbit_azimuth_deg = 148.0f;
            s.orbit_elevation_deg = 22.0f;
            s.orbit_distance_m = 2.15f;
            s.tint_rgb = {0.86f, 0.92f, 1.0f};
            s.intensity_ev100 = 0.28f;
            s.width_or_outer_m = 0.30f;
            s.height_or_inner_m = 1.20f;
            s.softbox_enabled = true;
            s.catchlight_weight = 0.40f;
        }
        // 3: Hair Orbit Specular Driver (angel-ring highlight)
        {
            auto& s = rig.studio_lights[3];
            s = StudioLightSpec{};
            set_name(s.name, "Hair Orbit");
            s.fixture = StudioFixtureProfile::Stripbox;
            s.enabled = true;
            s.cast_shadow = false;
            s.use_orbit_angles = true;
            s.orbit_azimuth_deg = 10.0f;
            s.orbit_elevation_deg = 52.0f;
            s.orbit_distance_m = 2.15f;
            s.tint_rgb = {1.0f, 0.98f, 0.96f};
            s.intensity_ev100 = 0.90f;
            s.width_or_outer_m = 0.30f;
            s.height_or_inner_m = 1.20f;
            s.softbox_enabled = true;
            s.catchlight_weight = 0.35f;
        }
        return rig;
    }
};


/** Named studio lighting rig preset record for JSON serialization (`config/lighting_rigs.json`). */
struct StudioRigPresetRecord {
    std::array<char, 64> name{};
    LightingRig rig{};
};

/** Fills `out_presets` with the 5 built-in complete studio lighting rig presets. */
void builtin_studio_rig_presets(std::array<StudioRigPresetRecord, 5>& out_presets,
                                const std::array<float, 3>& pivot_ws = {0.0f, 1.32f, 0.0f}) noexcept;

/** Loads studio lighting presets from a JSON file (`config/lighting_rigs.json`). */
[[nodiscard]] bool load_lighting_rigs_json(const std::filesystem::path& path,
                                           std::vector<StudioRigPresetRecord>& out_presets) noexcept;

/** Writes a list of studio lighting presets to a JSON file (`config/lighting_rigs.json`). */
[[nodiscard]] bool save_lighting_rigs_json(const std::filesystem::path& path,
                                           std::span<const StudioRigPresetRecord> presets) noexcept;

/** Updates or inserts a named preset inside `path` cleanly. */
[[nodiscard]] bool save_or_update_lighting_rig_preset(const std::filesystem::path& path, const char* preset_name,
                                                      const LightingRig& rig) noexcept;

} // namespace aether::renderer
