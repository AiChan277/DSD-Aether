#pragma once

#include "renderer/studio_lighting.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace aether::renderer {

enum class LightingFixturePreset : uint8_t {
    ArriSkyPanelS60C = 0,      // 64.5cm x 30.0cm soft diffusion area panel, rectangular eye catchlight
    ArriFresnelT2Orbiter = 1,  // 12-55 deg focusable Fresnel cone + 4-leaf barn-doors, circular catchlight
    DedolightDled7 = 2,        // Tight 8-25 deg precision rim/hair/accent spot, pinpoint catchlight
    BroadcastRingLight = 3,    // Camera-coaxial annular emitter, soft front fill, ring eye catchlight
    NaturalSunlightCieSky = 4, // 0.53 deg parallel solar beam + analytical CIE sky & ground bounce
    NaturalWindowPortal = 5,   // Lateral 1.2m x 1.8m window area portal, tall soft window catchlight
};

enum class LightRole : uint8_t {
    Key = 0,
    Fill = 1,
    Rim = 2,
    HairOrbit = 3,
};

enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct SceneLight {
    LightRole role{LightRole::Key};
    LightType type{LightType::Directional};
    bool enabled{true};
    bool cast_rt_shadow{true};
    bool use_2d_orbit_angle{true};
    float direction_or_pos[3]{0.42f, 0.56f, 0.71f};
    float color[3]{1.0f, 0.97f, 0.94f};
    float intensity{1.85f};
    float angular_radius_rad{0.06f};
    float cone_angle_rad{0.75f};
    float orbit_azimuth_deg{32.0f};
    float orbit_elevation_deg{28.0f};
};

struct LightingRig {
    static constexpr uint32_t kMaxLights = 8;

    std::array<SceneLight, kMaxLights> lights{};
    uint32_t active_count{4};

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

    // 3-tier lighting, pre-quantized GI, Filter Pro, circadian, photometric fixtures & [TESTING] crease disguise
    uint8_t lighting_mode{1}; // 0 = Traditional (Raster+SDF), 1 = Hybrid RT, 2 = 2.5D Path Tracing
    LightingFixturePreset fixture_preset{LightingFixturePreset::ArriSkyPanelS60C};
    float photometric_beam_angle_deg{32.0f}; // Focusable beam for Fresnel (12-55 deg) & Dedolight (8-25 deg)
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

    // Modular Studio Optical Modifiers (Key Light & Quick Controls)
    bool key_softbox_enabled{true};
    bool key_grid_enabled{false};
    float key_grid_angle_deg{40.0f}; // 30 / 40 / 60 deg
    bool key_barn_doors_enabled{false};
    std::array<float, 4> key_barn_door_cuts_deg{38.0f, 38.0f, 42.0f, 42.0f};
    bool key_diffusion_enabled{true};
    float key_diffusion_density{0.65f};
    bool key_gel_enabled{false};
    StudioGelPreset key_gel{StudioGelPreset::None};
    StudioGoboPattern key_gobo{StudioGoboPattern::None};

    // Extended 16-light Studio Array + Split Holo-Grading + Satsuei Composite
    std::array<StudioLightSpec, kMaxStudioLights> studio_lights{};
    uint32_t studio_light_count{0};
    bool use_studio_lights{false};
    int8_t key_studio_profile_override{-1};
    SplitLayerGradingParams split_grading{};
    SatsueiCompositeParams satsuei{};

    [[nodiscard]] static LightingRig default_three_point() noexcept {
        LightingRig rig{};
        rig.active_count = 4;
        rig.rim_step_strength = 0.42f;
        rig.hair_highlight_intensity = 0.65f;

        // 0: Key Light (ARRI SkyPanel S60-C calibrated at ~1.0 EV avatar anchor exposure)
        rig.lights[0].role = LightRole::Key;
        rig.lights[0].type = LightType::Directional;
        rig.lights[0].enabled = true;
        rig.lights[0].cast_rt_shadow = true;
        rig.lights[0].use_2d_orbit_angle = true;
        rig.lights[0].orbit_azimuth_deg = 32.0f;
        rig.lights[0].orbit_elevation_deg = 26.0f;
        rig.lights[0].color[0] = 1.0f;
        rig.lights[0].color[1] = 0.98f;
        rig.lights[0].color[2] = 0.95f;
        rig.lights[0].intensity = 1.00f;
        rig.lights[0].angular_radius_rad = 0.11f;

        // 1: Soft Fill Light
        rig.lights[1].role = LightRole::Fill;
        rig.lights[1].type = LightType::Directional;
        rig.lights[1].enabled = true;
        rig.lights[1].cast_rt_shadow = false;
        rig.lights[1].orbit_azimuth_deg = -52.0f;
        rig.lights[1].orbit_elevation_deg = 12.0f;
        rig.lights[1].color[0] = 0.72f;
        rig.lights[1].color[1] = 0.83f;
        rig.lights[1].color[2] = 1.0f;
        rig.lights[1].intensity = 0.24f;
        rig.lights[1].angular_radius_rad = 0.14f;

        // 2: Occluded Rim Light
        rig.lights[2].role = LightRole::Rim;
        rig.lights[2].type = LightType::Directional;
        rig.lights[2].enabled = true;
        rig.lights[2].cast_rt_shadow = true;
        rig.lights[2].orbit_azimuth_deg = 148.0f;
        rig.lights[2].orbit_elevation_deg = 22.0f;
        rig.lights[2].color[0] = 0.86f;
        rig.lights[2].color[1] = 0.92f;
        rig.lights[2].color[2] = 1.0f;
        rig.lights[2].intensity = 0.28f;
        rig.lights[2].angular_radius_rad = 0.04f;

        // 3: Hair Orbit Specular Driver (angel-ring highlight)
        rig.lights[3].role = LightRole::HairOrbit;
        rig.lights[3].type = LightType::Directional;
        rig.lights[3].enabled = true;
        rig.lights[3].cast_rt_shadow = false;
        rig.lights[3].orbit_azimuth_deg = 10.0f;
        rig.lights[3].orbit_elevation_deg = 52.0f;
        rig.lights[3].color[0] = 1.0f;
        rig.lights[3].color[1] = 0.98f;
        rig.lights[3].color[2] = 0.96f;
        rig.lights[3].intensity = 0.90f;
        return rig;
    }
};

/**
 * Configures `rig` with one of the 6 photometric studio (ARRI / Dedolight / Ring) or natural lighting
 * presets, auto-normalizing luminaire luminous flux at the avatar anchor so all profiles maintain balanced ~1.0 EV.
 */
inline void apply_lighting_fixture_preset(LightingRig& rig, LightingFixturePreset preset,
                                          float custom_beam_deg = -1.0f) noexcept {
    rig.fixture_preset = preset;
    rig.key_studio_profile_override = -1;
    if (custom_beam_deg > 1.0f) {
        rig.photometric_beam_angle_deg = std::clamp(custom_beam_deg, 8.0f, 55.0f);
    }
    rig.active_count = 4;
    for (auto& l : rig.lights) {
        l.enabled = true;
        l.use_2d_orbit_angle = true;
    }
    rig.lights[0].role = LightRole::Key;
    rig.lights[0].type = LightType::Directional;
    rig.lights[0].cast_rt_shadow = true;
    rig.lights[1].role = LightRole::Fill;
    rig.lights[1].type = LightType::Directional;
    rig.lights[1].cast_rt_shadow = false;
    rig.lights[2].role = LightRole::Rim;
    rig.lights[2].type = LightType::Directional;
    rig.lights[2].cast_rt_shadow = true;
    rig.lights[3].role = LightRole::HairOrbit;
    rig.lights[3].type = LightType::Directional;
    rig.lights[3].cast_rt_shadow = false;

    switch (preset) {
    case LightingFixturePreset::ArriSkyPanelS60C:
        rig.lights[0].orbit_azimuth_deg = 32.0f;
        rig.lights[0].orbit_elevation_deg = 26.0f;
        rig.lights[0].color[0] = 1.00f;
        rig.lights[0].color[1] = 0.98f;
        rig.lights[0].color[2] = 0.95f;
        rig.lights[0].intensity = 1.00f;
        rig.lights[0].angular_radius_rad = 0.11f; // 64.5 cm x 30.0 cm soft diffuser penumbra

        rig.lights[1].orbit_azimuth_deg = -52.0f;
        rig.lights[1].orbit_elevation_deg = 12.0f;
        rig.lights[1].color[0] = 0.72f;
        rig.lights[1].color[1] = 0.83f;
        rig.lights[1].color[2] = 1.00f;
        rig.lights[1].intensity = 0.24f;

        rig.lights[2].orbit_azimuth_deg = 148.0f;
        rig.lights[2].orbit_elevation_deg = 22.0f;
        rig.lights[2].color[0] = 0.86f;
        rig.lights[2].color[1] = 0.92f;
        rig.lights[2].color[2] = 1.00f;
        rig.lights[2].intensity = 0.28f;
        break;

    case LightingFixturePreset::ArriFresnelT2Orbiter:
        rig.photometric_beam_angle_deg = std::clamp(rig.photometric_beam_angle_deg, 12.0f, 55.0f);
        rig.lights[0].orbit_azimuth_deg = 38.0f;
        rig.lights[0].orbit_elevation_deg = 28.0f;
        rig.lights[0].color[0] = 1.00f;
        rig.lights[0].color[1] = 0.96f;
        rig.lights[0].color[2] = 0.90f; // Crisp tungsten-daylight Fresnel
        rig.lights[0].intensity = 1.04f;
        rig.lights[0].angular_radius_rad = 0.042f;
        rig.lights[0].cone_angle_rad = rig.photometric_beam_angle_deg * (3.14159265f / 180.0f);

        rig.lights[1].orbit_azimuth_deg = -55.0f;
        rig.lights[1].orbit_elevation_deg = 10.0f;
        rig.lights[1].color[0] = 0.68f;
        rig.lights[1].color[1] = 0.79f;
        rig.lights[1].color[2] = 1.00f;
        rig.lights[1].intensity = 0.20f;

        rig.lights[2].orbit_azimuth_deg = 144.0f;
        rig.lights[2].orbit_elevation_deg = 24.0f;
        rig.lights[2].color[0] = 0.95f;
        rig.lights[2].color[1] = 0.96f;
        rig.lights[2].color[2] = 1.00f;
        rig.lights[2].intensity = 0.30f;
        break;

    case LightingFixturePreset::DedolightDled7:
        rig.photometric_beam_angle_deg = std::clamp(rig.photometric_beam_angle_deg, 8.0f, 25.0f);
        rig.lights[0].orbit_azimuth_deg = 46.0f;
        rig.lights[0].orbit_elevation_deg = 32.0f;
        rig.lights[0].color[0] = 1.00f;
        rig.lights[0].color[1] = 0.97f;
        rig.lights[0].color[2] = 0.92f;
        rig.lights[0].intensity = 1.06f;
        rig.lights[0].angular_radius_rad = 0.015f; // Optical condenser sharp edge
        rig.lights[0].cone_angle_rad = rig.photometric_beam_angle_deg * (3.14159265f / 180.0f);

        rig.lights[1].orbit_azimuth_deg = -58.0f;
        rig.lights[1].orbit_elevation_deg = 8.0f;
        rig.lights[1].color[0] = 0.62f;
        rig.lights[1].color[1] = 0.74f;
        rig.lights[1].color[2] = 0.96f;
        rig.lights[1].intensity = 0.18f;

        rig.lights[2].orbit_azimuth_deg = 138.0f;
        rig.lights[2].orbit_elevation_deg = 30.0f;
        rig.lights[2].color[0] = 1.00f;
        rig.lights[2].color[1] = 0.96f;
        rig.lights[2].color[2] = 0.90f;
        rig.lights[2].intensity = 0.38f;
        break;

    case LightingFixturePreset::BroadcastRingLight:
        rig.lights[0].orbit_azimuth_deg = 0.0f;
        rig.lights[0].orbit_elevation_deg = 6.0f;
        rig.lights[0].color[0] = 1.00f;
        rig.lights[0].color[1] = 0.99f;
        rig.lights[0].color[2] = 0.97f;
        rig.lights[0].intensity = 0.95f;
        rig.lights[0].angular_radius_rad = 0.085f;

        rig.lights[1].orbit_azimuth_deg = -18.0f;
        rig.lights[1].orbit_elevation_deg = -8.0f;
        rig.lights[1].color[0] = 0.94f;
        rig.lights[1].color[1] = 0.96f;
        rig.lights[1].color[2] = 1.00f;
        rig.lights[1].intensity = 0.16f;

        rig.lights[2].orbit_azimuth_deg = 155.0f;
        rig.lights[2].orbit_elevation_deg = 18.0f;
        rig.lights[2].color[0] = 0.88f;
        rig.lights[2].color[1] = 0.93f;
        rig.lights[2].color[2] = 1.00f;
        rig.lights[2].intensity = 0.22f;
        break;

    case LightingFixturePreset::NaturalSunlightCieSky:
        rig.lights[0].orbit_azimuth_deg = 42.0f;
        rig.lights[0].orbit_elevation_deg = 36.0f;
        rig.lights[0].color[0] = 1.00f;
        rig.lights[0].color[1] = 0.95f;
        rig.lights[0].color[2] = 0.86f; // Clear daylight solar beam
        rig.lights[0].intensity = 0.98f;
        rig.lights[0].angular_radius_rad = 0.00462f; // True 0.53 deg solar disc

        rig.lights[1].orbit_azimuth_deg = -30.0f;
        rig.lights[1].orbit_elevation_deg = 58.0f;
        rig.lights[1].color[0] = 0.56f;
        rig.lights[1].color[1] = 0.78f;
        rig.lights[1].color[2] = 1.00f; // CIE clear sky dome
        rig.lights[1].intensity = 0.28f;

        rig.lights[2].orbit_azimuth_deg = 148.0f;
        rig.lights[2].orbit_elevation_deg = 24.0f;
        rig.lights[2].color[0] = 0.72f;
        rig.lights[2].color[1] = 0.86f;
        rig.lights[2].color[2] = 1.00f;
        rig.lights[2].intensity = 0.26f;
        break;

    case LightingFixturePreset::NaturalWindowPortal:
        rig.lights[0].orbit_azimuth_deg = 66.0f;
        rig.lights[0].orbit_elevation_deg = 18.0f;
        rig.lights[0].color[0] = 0.92f;
        rig.lights[0].color[1] = 0.96f;
        rig.lights[0].color[2] = 1.00f; // Overcast/daylight sky through 1.2m x 1.8m window
        rig.lights[0].intensity = 1.04f;
        rig.lights[0].angular_radius_rad = 0.16f; // Wide soft window area penumbra

        rig.lights[1].orbit_azimuth_deg = -68.0f;
        rig.lights[1].orbit_elevation_deg = 8.0f;
        rig.lights[1].color[0] = 0.82f;
        rig.lights[1].color[1] = 0.84f;
        rig.lights[1].color[2] = 0.88f; // Soft interior room bounce wrap
        rig.lights[1].intensity = 0.30f;

        rig.lights[2].orbit_azimuth_deg = 140.0f;
        rig.lights[2].orbit_elevation_deg = 22.0f;
        rig.lights[2].color[0] = 0.86f;
        rig.lights[2].color[1] = 0.92f;
        rig.lights[2].color[2] = 1.00f;
        rig.lights[2].intensity = 0.20f;
        break;
    }
}

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
